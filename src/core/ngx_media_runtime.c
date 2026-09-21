#include "ngx_media_runtime.h"

#include <ngx_event.h>

#include "ngx_media_hls_segmenter.h"
#include "ngx_media_record.h"
#include "ngx_media_registry.h"
#include "ngx_media_selector.h"

/*
 * Per-stream outputs.  The program feed is drained on every runtime tick,
 * muxed into one burst and handed to HLS and to the PROGRAM recording; the
 * ISO tap muxes a named source before the selector and the RAW tap records
 * transport bytes before normalization (goal doc 20).
 */
typedef struct {
    ngx_uint_t             used;
    ngx_media_stream_t    *stream;
    ngx_media_cursor_t     cursor;
    ngx_media_ts_mux_t     mux;
    ngx_media_ts_burst_t   burst;
    unsigned               burst_open:1;
    ngx_media_trackset_t  *tracks;
    ngx_media_hls_t        hls;
    unsigned               hls_ready:1;
    ngx_media_record_t     program;
    unsigned               program_ready:1;
    ngx_media_ts_mux_t     iso_mux;
    ngx_media_ts_burst_t   iso_burst;
    ngx_media_trackset_t  *iso_tracks;
    unsigned               iso_open:1;
    ngx_media_record_t     iso;
    unsigned               iso_ready:1;
    uint64_t               generation;
    uint64_t               frames;
    uint64_t               bursts;
} ngx_media_runtime_outputs_t;

/* Per-stream player preparation: one conversion shared by every RTMP player. */
typedef struct {
    ngx_uint_t                 used;
    ngx_media_stream_t        *stream;
    ngx_media_cursor_t         cursor;
    ngx_media_rtmp_prepare_t   prepare;
    ngx_media_trackset_t      *tracks;
} ngx_media_runtime_prepare_t;

static ngx_media_runtime_outputs_t
    ngx_media_runtime_outputs[NGX_MEDIA_RUNTIME_MAX_OUTPUTS];
static ngx_media_runtime_prepare_t
    ngx_media_runtime_prepares[NGX_MEDIA_RUNTIME_MAX_PREPARE];

static ngx_media_record_t   ngx_media_runtime_raw;
static ngx_uint_t           ngx_media_runtime_raw_started;

static ngx_event_t          ngx_media_runtime_timer;
static ngx_uint_t           ngx_media_runtime_armed;
static ngx_msec_t           ngx_media_runtime_last_idle_log;

static ngx_str_t            ngx_media_runtime_none = ngx_string("none");

extern ngx_module_t  ngx_media_core_module;

static ngx_media_policy_t *
ngx_media_runtime_policy(void)
{
    ngx_media_policy_t  *policy;

    policy = (ngx_media_policy_t *)
                 ((ngx_cycle_t *) ngx_cycle)
                     ->conf_ctx[ngx_media_core_module.index];

    if (policy == NULL) {
        policy = ngx_media_policy_get((ngx_cycle_t *) ngx_cycle);
    }

    return policy;
}

/* --- outputs ------------------------------------------------------------- */

static ngx_media_runtime_outputs_t *
ngx_media_runtime_outputs_get(ngx_media_stream_t *stream, ngx_log_t *log)
{
    ngx_media_policy_t              *policy = ngx_media_runtime_policy();
    ngx_media_runtime_outputs_t     *out = NULL;
    ngx_media_hls_conf_t             hls_conf;
    ngx_media_record_conf_t          rec_conf;
    ngx_uint_t                       i;

    if (stream == NULL) {
        return NULL;
    }

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_OUTPUTS; i++) {

        if (ngx_media_runtime_outputs[i].used
            && ngx_media_runtime_outputs[i].stream == stream)
        {
            return &ngx_media_runtime_outputs[i];
        }
    }

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_OUTPUTS; i++) {

        if (!ngx_media_runtime_outputs[i].used) {
            out = &ngx_media_runtime_outputs[i];
            break;
        }
    }

    if (out == NULL) {
        return NULL;
    }

    ngx_memzero(out, sizeof(ngx_media_runtime_outputs_t));

    out->used = 1;
    out->stream = stream;
    out->generation = stream->generation;

    ngx_media_feed_cursor_init(&stream->program_feed, &out->cursor);

    if (ngx_media_ts_mux_init(&out->mux, NULL, log) != NGX_OK) {
        out->used = 0;
        return NULL;
    }

    if (policy == NULL) {
        return out;
    }

    if (policy->hls_path.len > 0) {
        ngx_media_hls_conf_default(&hls_conf);

        hls_conf.path = policy->hls_path;
        hls_conf.target_duration = NGX_MEDIA_RUNTIME_HLS_TARGET;
        hls_conf.min_duration = NGX_MEDIA_RUNTIME_HLS_TARGET / 2;
        hls_conf.max_duration = NGX_MEDIA_RUNTIME_HLS_TARGET * 2;

        if (ngx_media_hls_init(&out->hls, &hls_conf, log) == NGX_OK) {
            out->hls_ready = 1;

            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "media: hls output started for %V/%V in %V",
                          &stream->application, &stream->name,
                          &policy->hls_path);
        }
    }

    if (policy->record_program_path.len > 0) {
        ngx_media_record_conf_default(&rec_conf);

        rec_conf.path = policy->record_program_path;
        rec_conf.tap = NGX_MEDIA_RECORD_PROGRAM;

        if (ngx_media_record_init(&out->program, &rec_conf, log) == NGX_OK) {
            out->program_ready = 1;

            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "media: program recording started for %V/%V",
                          &stream->application, &stream->name);
        }
    }

    if (policy->record_iso_path.len > 0) {
        ngx_media_record_conf_default(&rec_conf);

        rec_conf.path = policy->record_iso_path;
        rec_conf.tap = NGX_MEDIA_RECORD_ISO;

        if (ngx_media_ts_mux_init(&out->iso_mux, NULL, log) == NGX_OK
            && ngx_media_record_init(&out->iso, &rec_conf, log) == NGX_OK)
        {
            out->iso_ready = 1;

            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "media: iso recording started for %V/%V source %V",
                          &stream->application, &stream->name,
                          &policy->record_iso_source);
        }
    }

    return out;
}

static void
ngx_media_runtime_outputs_flush(ngx_media_runtime_outputs_t *out)
{
    size_t  len;

    if (!out->burst_open) {
        return;
    }

    if (ngx_media_ts_mux_burst_end(&out->mux, &out->burst) == NGX_OK) {
        len = ngx_media_ts_burst_size(&out->burst);

        if (len > 0) {

            if (out->hls_ready) {
                (void) ngx_media_hls_add_burst(&out->hls, &out->burst);
            }

            if (out->program_ready) {
                (void) ngx_media_record_append(&out->program,
                                               out->burst.backing, 0, len);
            }
        }

        out->bursts++;
    }

    ngx_media_ts_mux_burst_destroy(&out->burst);
    out->burst_open = 0;
}

static void
ngx_media_runtime_iso_flush(ngx_media_runtime_outputs_t *out)
{
    size_t  len;

    if (!out->iso_open) {
        return;
    }

    if (ngx_media_ts_mux_burst_end(&out->iso_mux, &out->iso_burst) == NGX_OK) {
        len = ngx_media_ts_burst_size(&out->iso_burst);

        if (len > 0 && out->iso_ready) {
            (void) ngx_media_record_append(&out->iso, out->iso_burst.backing,
                                           0, len);
        }
    }

    ngx_media_ts_mux_burst_destroy(&out->iso_burst);
    out->iso_open = 0;
}

static void
ngx_media_runtime_outputs_drain(ngx_media_runtime_outputs_t *out,
    ngx_log_t *log)
{
    ngx_media_frame_t   frames[64];
    ngx_media_stream_t  *stream = out->stream;
    ngx_uint_t           count, i, status;

    (void) log;

    for ( ;; ) {
        status = ngx_media_feed_read(&stream->program_feed, &out->cursor, 64,
                                     512 * 1024, frames, &count);

        if (status == NGX_MEDIA_FEED_GENERATION_MISMATCH) {
            /* a switch is a discontinuity for every consumer */
            if (out->hls_ready) {
                (void) ngx_media_hls_discontinuity(&out->hls);
            }

            (void) ngx_media_feed_resync(&stream->program_feed, &out->cursor,
                                         NGX_MEDIA_FEED_RESYNC_KEYFRAME);
            continue;
        }

        if (status == NGX_MEDIA_FEED_OVERRUN) {
            (void) ngx_media_feed_resync(&stream->program_feed, &out->cursor,
                                         NGX_MEDIA_FEED_RESYNC_KEYFRAME);
            continue;
        }

        if (status != NGX_MEDIA_FEED_BATCH) {
            break;
        }

        if (stream->active != NULL && stream->active->tracks != NULL
            && out->tracks != stream->active->tracks)
        {
            if (ngx_media_ts_mux_set_tracks(&out->mux,
                                            stream->active->tracks) == NGX_OK)
            {
                out->tracks = stream->active->tracks;
            }
        }

        if (!out->burst_open
            && ngx_media_ts_mux_burst_init(&out->mux, &out->burst,
                                           256 * 1024) != NGX_OK)
        {
            ngx_media_feed_release(frames, count);
            return;
        }

        out->burst_open = 1;

        for (i = 0; i < count; i++) {

            if (ngx_media_ts_mux_write_frame(&out->mux, &out->burst,
                                             &frames[i], 0) == NGX_AGAIN)
            {
                ngx_media_runtime_outputs_flush(out);

                if (ngx_media_ts_mux_burst_init(&out->mux, &out->burst,
                                                256 * 1024) != NGX_OK)
                {
                    continue;
                }

                out->burst_open = 1;

                (void) ngx_media_ts_mux_write_frame(&out->mux, &out->burst,
                                                    &frames[i], 0);
            }

            out->frames++;
        }

        ngx_media_feed_release(frames, count);

        ngx_media_runtime_outputs_flush(out);
    }
}

void
ngx_media_runtime_iso_source(ngx_media_stream_t *stream,
    ngx_media_source_t *source, const ngx_media_frame_t *frame)
{
    ngx_media_policy_t              *policy = ngx_media_runtime_policy();
    ngx_media_runtime_outputs_t     *out;

    if (policy == NULL || policy->record_iso_path.len == 0 || stream == NULL
        || source == NULL)
    {
        return;
    }

    if (policy->record_iso_source.len != source->id.len
        || ngx_memcmp(policy->record_iso_source.data, source->id.data,
                      source->id.len) != 0)
    {
        return;
    }

    out = ngx_media_runtime_outputs_get(stream, ngx_cycle->log);

    if (out == NULL || !out->iso_ready) {
        return;
    }

    if (source->tracks != NULL && out->iso_tracks != source->tracks) {

        if (ngx_media_ts_mux_set_tracks(&out->iso_mux, source->tracks)
            == NGX_OK)
        {
            out->iso_tracks = source->tracks;
        }
    }

    if (!out->iso_open) {

        if (ngx_media_ts_mux_burst_init(&out->iso_mux, &out->iso_burst,
                                        64 * 1024) != NGX_OK)
        {
            return;
        }

        out->iso_open = 1;
    }

    if (ngx_media_ts_mux_write_frame(&out->iso_mux, &out->iso_burst, frame,
                                     0) == NGX_AGAIN)
    {
        ngx_media_runtime_iso_flush(out);

        if (ngx_media_ts_mux_burst_init(&out->iso_mux, &out->iso_burst,
                                        64 * 1024) != NGX_OK)
        {
            return;
        }

        out->iso_open = 1;

        (void) ngx_media_ts_mux_write_frame(&out->iso_mux, &out->iso_burst,
                                            frame, 0);
    }

    if (ngx_media_ts_burst_size(&out->iso_burst) > 48 * 1024) {
        ngx_media_runtime_iso_flush(out);
    }
}

ngx_uint_t
ngx_media_runtime_raw_ready(void)
{
    return ngx_media_runtime_raw_started;
}

ngx_int_t
ngx_media_runtime_raw_bytes(const u_char *data, size_t len)
{
    if (!ngx_media_runtime_raw_started) {
        return NGX_DECLINED;
    }

    return ngx_media_record_append_bytes(&ngx_media_runtime_raw, data, len);
}

static void
ngx_media_runtime_outputs_stop(ngx_media_runtime_outputs_t *out)
{
    if (!out->used) {
        return;
    }

    ngx_media_runtime_outputs_flush(out);
    ngx_media_runtime_iso_flush(out);

    if (out->hls_ready) {
        (void) ngx_media_hls_finish(&out->hls);
    }

    if (out->program_ready) {
        ngx_media_record_stop(&out->program);
    }

    if (out->iso_ready) {
        ngx_media_record_stop(&out->iso);
    }

    ngx_media_hls_destroy(&out->hls);
    ngx_media_ts_mux_destroy(&out->mux);
    ngx_media_ts_mux_destroy(&out->iso_mux);

    ngx_memzero(out, sizeof(ngx_media_runtime_outputs_t));
}

/* --- player preparation -------------------------------------------------- */

ngx_media_rtmp_prepare_t *
ngx_media_runtime_prepare(ngx_media_stream_t *stream, ngx_log_t *log)
{
    ngx_media_runtime_prepare_t  *slot = NULL;
    ngx_uint_t                    i;

    if (stream == NULL) {
        return NULL;
    }

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_PREPARE; i++) {

        if (ngx_media_runtime_prepares[i].used
            && ngx_media_runtime_prepares[i].stream == stream)
        {
            return &ngx_media_runtime_prepares[i].prepare;
        }
    }

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_PREPARE; i++) {

        if (!ngx_media_runtime_prepares[i].used) {
            slot = &ngx_media_runtime_prepares[i];
            break;
        }
    }

    if (slot == NULL) {
        return NULL;
    }

    ngx_memzero(slot, sizeof(ngx_media_runtime_prepare_t));

    slot->used = 1;
    slot->stream = stream;

    ngx_media_feed_cursor_init(&stream->program_feed, &slot->cursor);
    ngx_media_rtmp_prepare_init(&slot->prepare, 2048, 32 * 1024 * 1024);

    (void) log;

    return &slot->prepare;
}

static void
ngx_media_runtime_prepare_drain(ngx_media_stream_t *stream, ngx_log_t *log)
{
    ngx_media_runtime_prepare_t  *slot;
    ngx_media_frame_t             frames[NGX_MEDIA_RUNTIME_MAX_FRAMES_TICK];
    ngx_uint_t                    count, i, status;

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_PREPARE; i++) {

        if (ngx_media_runtime_prepares[i].used
            && ngx_media_runtime_prepares[i].stream == stream)
        {
            slot = &ngx_media_runtime_prepares[i];
            goto found;
        }
    }

    return;

found:

    for ( ;; ) {
        status = ngx_media_feed_read(&stream->program_feed, &slot->cursor,
                                     NGX_MEDIA_RUNTIME_MAX_FRAMES_TICK,
                                     1024 * 1024, frames, &count);

        if (status == NGX_MEDIA_FEED_GENERATION_MISMATCH
            || status == NGX_MEDIA_FEED_OVERRUN)
        {
            (void) ngx_media_feed_resync(&stream->program_feed, &slot->cursor,
                                         NGX_MEDIA_FEED_RESYNC_KEYFRAME);
            slot->tracks = NULL;   /* new generation: new sequence headers */
            continue;
        }

        if (status != NGX_MEDIA_FEED_BATCH) {
            break;
        }

        if (stream->active != NULL && stream->active->tracks != NULL
            && slot->tracks != stream->active->tracks)
        {
            if (ngx_media_rtmp_prepare_announce(&slot->prepare,
                                                stream->active->tracks) != NGX_OK)
            {
                ngx_log_error(NGX_LOG_WARN, log, 0,
                              "media: rtmp: could not announce %V/%V",
                              &stream->application, &stream->name);
            }

            slot->tracks = stream->active->tracks;
        }

        /* bounded per visit (goal doc 34 item 13) */
        for (i = 0; i < count && i < NGX_MEDIA_RUNTIME_MAX_FRAMES_TICK; i++) {

            if (frames[i].config) {
                continue;
            }

            if (ngx_media_rtmp_prepare_frame(&slot->prepare, &frames[i])
                != NGX_OK)
            {
                break;
            }
        }

        ngx_media_feed_release(frames, count);
    }
}

/* --- timer --------------------------------------------------------------- */

void
ngx_media_runtime_tick(ngx_log_t *log)
{
    ngx_media_registry_t        *registry;
    ngx_media_registry_entry_t  *entry;
    ngx_media_selector_result_t  res;
    ngx_media_stream_t          *stream;
    ngx_media_policy_t          *policy;
    ngx_queue_t                 *q;
    uint64_t                     before;
    ngx_msec_t                   now;

    now = ngx_current_msec;
    policy = ngx_media_runtime_policy();

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        return;
    }

    for (q = ngx_queue_head(&registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = q->next)
    {
        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);
        stream = &entry->stream;

        before = stream->switches;

        (void) ngx_media_selector_run(stream, now, &res);

        if (now - ngx_media_runtime_last_idle_log >= 1000) {
            ngx_log_debug6(NGX_LOG_DEBUG_EVENT, log, 0,
                           "media: selector tick stream=%V/%V active=%V "
                           "active_healthy=%ui best=%ui emergency=%ui",
                           &stream->application, &stream->name,
                           stream->active != NULL ? &stream->active->id
                                                  : &ngx_media_runtime_none,
                           res.active_eligible, res.best != NULL,
                           res.emergency != NULL);
        }

        if (policy != NULL
            && (policy->hls_path.len > 0
                || policy->record_program_path.len > 0
                || policy->record_iso_path.len > 0))
        {
            ngx_media_runtime_outputs_t  *out;

            out = ngx_media_runtime_outputs_get(stream, log);

            if (out != NULL) {
                ngx_media_runtime_outputs_drain(out, log);
            }
        }

        if (stream->active != NULL && stream->active->tracks != NULL) {
            ngx_media_runtime_prepare_drain(stream, log);
        }

        if (stream->switches != before) {
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "media: selector switched stream=%V/%V active=%V "
                          "generation=%ui switches=%uL",
                          &stream->application, &stream->name,
                          stream->active != NULL ? &stream->active->id
                                                 : &ngx_media_runtime_none,
                          stream->generation, stream->switches);
        }
    }

    if (now - ngx_media_runtime_last_idle_log >= 1000) {
        ngx_media_runtime_last_idle_log = now;
    }
}

static void
ngx_media_runtime_handler(ngx_event_t *ev)
{
    if (!ngx_media_runtime_armed) {
        return;
    }

    ngx_media_runtime_tick(ev->log);

    /* stop re-arming during shutdown so the worker can exit */
    if (ngx_exiting || ngx_terminate || ngx_quit) {
        return;
    }

    ngx_add_timer(ev, NGX_MEDIA_RUNTIME_INTERVAL);
}

ngx_uint_t
ngx_media_runtime_arm(ngx_cycle_t *cycle, ngx_log_t *log)
{
    ngx_media_policy_t  *policy;

    if (ngx_media_runtime_armed) {
        return 0;
    }

    /* deterministic ownership: worker 0 owns program state (goal doc 22) */
    if (ngx_process_slot != 0) {
        return 0;
    }

    policy = (ngx_media_policy_t *)
                 ((ngx_cycle_t *) ngx_cycle)
                     ->conf_ctx[ngx_media_core_module.index];

    if (policy == NULL) {
        policy = ngx_media_policy_get((ngx_cycle_t *) ngx_cycle);
    }

    if (policy != NULL && policy->record_raw_path.len > 0) {
        ngx_media_record_conf_t  rec_conf;

        ngx_media_record_conf_default(&rec_conf);

        rec_conf.path = policy->record_raw_path;
        rec_conf.tap = NGX_MEDIA_RECORD_RAW;

        if (ngx_media_record_init(&ngx_media_runtime_raw, &rec_conf, cycle->log)
            == NGX_OK)
        {
            ngx_media_runtime_raw_started = 1;

            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                          "media: raw recording started in %V",
                          &policy->record_raw_path);
        }
    }

    ngx_memzero(&ngx_media_runtime_timer, sizeof(ngx_event_t));

    ngx_media_runtime_timer.handler = ngx_media_runtime_handler;
    ngx_media_runtime_timer.log = log;
    ngx_media_runtime_timer.data = cycle;
    ngx_media_runtime_armed = 1;

    ngx_add_timer(&ngx_media_runtime_timer, NGX_MEDIA_RUNTIME_INTERVAL);

    return 1;
}

void
ngx_media_runtime_stop(void)
{
    if (!ngx_media_runtime_armed) {
        return;
    }

    ngx_media_runtime_armed = 0;

    if (ngx_media_runtime_timer.timer_set) {
        ngx_del_timer(&ngx_media_runtime_timer);
    }
}

void
ngx_media_runtime_shutdown(ngx_log_t *log)
{
    ngx_uint_t  i;

    (void) log;

    ngx_media_runtime_stop();

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_OUTPUTS; i++) {
        ngx_media_runtime_outputs_stop(&ngx_media_runtime_outputs[i]);
    }

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_PREPARE; i++) {

        if (ngx_media_runtime_prepares[i].used) {
            ngx_media_rtmp_prepare_destroy(
                &ngx_media_runtime_prepares[i].prepare);
            ngx_memzero(&ngx_media_runtime_prepares[i],
                        sizeof(ngx_media_runtime_prepare_t));
        }
    }

    if (ngx_media_runtime_raw_started) {
        ngx_media_record_stop(&ngx_media_runtime_raw);
        ngx_media_runtime_raw_started = 0;
    }
}

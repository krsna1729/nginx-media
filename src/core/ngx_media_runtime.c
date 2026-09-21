#include "ngx_media_runtime.h"
#include "ngx_media_file.h"
#include "ngx_media_hls_ingest.h"
#include "ngx_media_hls_pull.h"
#include "ngx_media_hls_push.h"

#include <ngx_event.h>

#include "ngx_media_hls_segmenter.h"
#include "ngx_media_record.h"
#include "ngx_media_registry.h"
#include "ngx_media_route.h"
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

static ngx_media_owner_dir_t     *ngx_media_runtime_owners;

/* sources that live in another worker and are fed through the routing layer */
#define NGX_MEDIA_RUNTIME_MAX_ROUTED 32

typedef struct {
    ngx_uint_t          used;
    uint32_t            hash;
    ngx_media_stream_t *stream;
    ngx_media_source_t *source;
} ngx_media_runtime_routed_t;

static ngx_media_runtime_routed_t ngx_media_runtime_routed[
    NGX_MEDIA_RUNTIME_MAX_ROUTED];
static uint64_t                   ngx_media_runtime_routed_frames;
static uint64_t                   ngx_media_runtime_routed_msgs;
static uint64_t                   ngx_media_runtime_routed_nopayload;
static ngx_media_runtime_sink_pt  ngx_media_runtime_sink;
static void                      *ngx_media_runtime_sink_ctx;

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

void
ngx_media_runtime_set_sink(ngx_media_runtime_sink_pt cb, void *ctx)
{
    ngx_media_runtime_sink = cb;
    ngx_media_runtime_sink_ctx = ctx;
}

/* does this burst carry a video sync point a receiver may resume at? */
static ngx_uint_t
ngx_media_runtime_burst_keyframe(const ngx_media_ts_burst_t *burst)
{
    ngx_uint_t  i;

    for (i = 0; i < burst->nslices; i++) {

        if (burst->slices[i].media_type == NGX_MEDIA_TYPE_VIDEO
            && burst->slices[i].keyframe)
        {
            return 1;
        }
    }

    return 0;
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

            if (ngx_media_runtime_sink != NULL) {
                ngx_media_runtime_sink(ngx_media_runtime_sink_ctx,
                                       out->stream, out->burst.backing, len,
                                       ngx_media_runtime_burst_keyframe(
                                           &out->burst));
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
                                     512 * 1024, ngx_current_msec, frames,
                                     &count);

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

/* --- owner side of the routing escape hatch ------------------------------ */

/*
 * Frames that arrive from another worker are fed into the local program as if
 * the publisher were local: the owner creates the stream and source from the
 * OPEN message and publishes every frame through the normal path.
 */
static ngx_int_t
ngx_media_runtime_route_sink(void *ctx, uint32_t hash,
    const ngx_media_ipc_header_t *header, ngx_media_buf_t *payload)
{
    ngx_media_registry_t  *registry;
    ngx_media_stream_t    *stream;
    ngx_media_source_t    *source;
    ngx_media_frame_t      frame;
    ngx_str_t              application, name, source_id;
    u_char                *p, *slash;
    ngx_media_feed_conf_t  feed_conf;

    (void) ctx;
    (void) hash;

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        return NGX_ERROR;
    }

    if (header->type == NGX_MEDIA_IPC_MSG_OPEN) {

        if (payload == NULL || ngx_media_buf_size(payload) < 3) {
            return NGX_ERROR;
        }

        p = ngx_media_buf_data(payload);

        slash = ngx_strlchr(p, p + ngx_media_buf_size(payload), '/');

        if (slash == NULL) {
            return NGX_ERROR;
        }

        application.data = p;
        application.len = (size_t) (slash - p);

        name.data = slash + 1;

        slash = ngx_strlchr(name.data,
                            p + ngx_media_buf_size(payload), '/');

        if (slash == NULL) {
            return NGX_ERROR;
        }

        name.len = (size_t) (slash - name.data);

        source_id.data = slash + 1;
        source_id.len = ngx_media_buf_size(payload)
                        - application.len - name.len - 2;

        feed_conf.max_units = 2048;
        feed_conf.max_bytes = 32 * 1024 * 1024;
        feed_conf.max_age = 10000;

        stream = ngx_media_registry_stream_create(registry, &application,
                                                  &name, &feed_conf,
                                                  ngx_cycle->log);

        if (stream == NULL) {
            return NGX_ERROR;
        }

        source = ngx_media_stream_source_find(stream, &source_id);

        if (source != NULL) {
            ngx_media_stream_source_remove(stream, source);
        }

        source = ngx_media_stream_source_add(stream, &source_id,
                                             header->source_type,
                                             header->priority,
                                             ngx_cycle->log);

        if (source == NULL) {
            return NGX_ERROR;
        }

        ngx_media_health_init(&source->health, &stream->selector,
                              ngx_current_msec);
        ngx_media_health_transport(&source->health, 1, ngx_current_msec);

        if (stream->active == NULL) {
            (void) ngx_media_stream_promote(stream, source);
        }

        /* remember the routed source so its frames have a destination */
        {
            ngx_uint_t  i;

            for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_ROUTED; i++) {

                if (!ngx_media_runtime_routed[i].used) {
                    ngx_media_runtime_routed[i].used = 1;
                    ngx_media_runtime_routed[i].hash = hash;
                    ngx_media_runtime_routed[i].stream = stream;
                    ngx_media_runtime_routed[i].source = source;
                    break;
                }
            }
        }

        /* the owner claims the stream in the shared directory */
        if (ngx_media_runtime_owners != NULL) {
            (void) ngx_media_owner_dir_claim(ngx_media_runtime_owners, hash,
                                             (ngx_uint_t) ngx_process_slot);
        }

        ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                      "media: routed source opened stream=%V/%V source=%V",
                      &application, &name, &source_id);

        return NGX_OK;
    }

    if (header->type == NGX_MEDIA_IPC_MSG_TRACKS) {
        ngx_media_trackset_t  set;
        const u_char         *q;
        uint32_t              count, i2;
        size_t                left;

        if (payload == NULL) {
            return NGX_ERROR;
        }

        q = ngx_media_buf_data(payload);
        left = ngx_media_buf_size(payload);

        if (left < sizeof(uint32_t)) {
            return NGX_ERROR;
        }

        ngx_memcpy(&count, q, sizeof(uint32_t));
        q += sizeof(uint32_t);
        left -= sizeof(uint32_t);

        if (count == 0 || count > 8
            || ngx_media_trackset_init(&set, count, ngx_cycle->log) != NGX_OK)
        {
            return NGX_ERROR;
        }

        for (i2 = 0; i2 < count; i2++) {
            uint32_t           fields[10];
            ngx_media_track_t  track;

            if (left < sizeof(fields)) {
                ngx_media_trackset_destroy(&set);
                return NGX_ERROR;
            }

            ngx_memcpy(fields, q, sizeof(fields));
            q += sizeof(fields);
            left -= sizeof(fields);

            if (fields[9] > left) {
                ngx_media_trackset_destroy(&set);
                return NGX_ERROR;
            }

            ngx_memzero(&track, sizeof(track));

            track.media_type = fields[0];
            track.codec = fields[1];
            track.payload_format = fields[2];
            track.sample_rate = fields[3];
            track.channels = fields[4];
            track.profile = fields[5];
            track.level = fields[6];
            track.width = fields[7];
            track.height = fields[8];

            if (fields[9] > 0) {
                ngx_media_buf_t  *config = ngx_media_buf_alloc(fields[9]);

                if (config == NULL) {
                    ngx_media_trackset_destroy(&set);
                    return NGX_ERROR;
                }

                ngx_memcpy(ngx_media_buf_data(config), q, fields[9]);
                (void) ngx_media_buf_freeze(config, fields[9]);

                track.config = config;
                (void) ngx_media_trackset_add(&set, &track);
                ngx_media_buf_unref(config);

            } else {
                (void) ngx_media_trackset_add(&set, &track);
            }

            q += fields[9];
            left -= fields[9];
        }

        for (i2 = 0; i2 < NGX_MEDIA_RUNTIME_MAX_ROUTED; i2++) {

            if (ngx_media_runtime_routed[i2].used
                && ngx_media_runtime_routed[i2].hash == hash)
            {
                (void) ngx_media_source_tracks_set(
                    ngx_media_runtime_routed[i2].source, &set, ngx_cycle->log);

                for (count = 0; count < set.count; count++) {

                    if (set.tracks[count].media_type == NGX_MEDIA_TYPE_VIDEO) {
                        ngx_media_runtime_routed[i2].source->has_video = 1;
                    }
                }

                break;
            }
        }

        ngx_media_trackset_destroy(&set);

        return NGX_OK;
    }

    if (header->type == NGX_MEDIA_IPC_MSG_CLOSE) {
        ngx_uint_t  i;

        /* the owning source disappears with its publisher */
        for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_ROUTED; i++) {

            if (ngx_media_runtime_routed[i].used
                && ngx_media_runtime_routed[i].hash == hash)
            {
                ngx_media_stream_source_remove(
                    ngx_media_runtime_routed[i].stream,
                    ngx_media_runtime_routed[i].source);

                ngx_memzero(&ngx_media_runtime_routed[i],
                            sizeof(ngx_media_runtime_routed_t));
                break;
            }
        }

        ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                      "media: routed source closed hash=%uL frames=%uL msgs=%uL "
                      "nopayload=%uL", header->hash,
                      ngx_media_runtime_routed_frames,
                      ngx_media_runtime_routed_msgs,
                      ngx_media_runtime_routed_nopayload);

        return NGX_OK;
    }

    /*
     * A media frame.  It goes to the *source* the OPEN message registered, not
     * to whatever is currently active: the source is still standby until its
     * first keyframe passes the gate, and the gate can only see frames that
     * were published to it.
     */
    {
        ngx_uint_t  i;

        for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_ROUTED; i++) {

            if (ngx_media_runtime_routed[i].used
                && ngx_media_runtime_routed[i].hash == hash)
            {
                break;
            }
        }

        ngx_media_runtime_routed_msgs++;

        if (i == NGX_MEDIA_RUNTIME_MAX_ROUTED) {
            return NGX_OK;
        }

        if (payload == NULL) {
            ngx_media_runtime_routed_nopayload++;
            return NGX_OK;
        }

        ngx_media_frame_init(&frame);

        frame.media_type = header->media_type;
        frame.codec = header->codec;
        frame.payload_format = header->payload_format;
        frame.track_index = header->track_index;
        frame.pts = header->pts;
        frame.dts = header->dts;
        frame.keyframe = header->keyframe ? 1 : 0;
        frame.config = header->config ? 1 : 0;

        /*
         * adopt() takes over the reference it is given, and the IPC message
         * still owns its own: take a reference for the frame so releasing the
         * message cannot free a buffer the program is still reading.
         */
        ngx_media_frame_adopt(&frame, ngx_media_buf_ref(payload));

        source = ngx_media_runtime_routed[i].source;
        stream = ngx_media_runtime_routed[i].stream;

        ngx_media_health_media(&source->health, frame.dts, ngx_current_msec);

        (void) ngx_media_stream_publish(stream, source, &frame,
                                        ngx_current_msec);

        ngx_media_frame_release(&frame);

        if ((++ngx_media_runtime_routed_frames % 200) == 0) {
            ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                          "media: %uL routed frames, program=%uL active=%V",
                          ngx_media_runtime_routed_frames, stream->program_frames,
                          stream->active != NULL ? &stream->active->id
                                                 : &ngx_media_runtime_none);
        }
    }

    return NGX_OK;
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
                                     1024 * 1024, ngx_current_msec, frames,
                                     &count);

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

static ngx_media_runtime_stats_t  ngx_media_runtime_stats;
static ngx_msec_t                 ngx_media_runtime_last_tick;

void
ngx_media_runtime_stats_get(ngx_media_runtime_stats_t *out)
{
    if (out != NULL) {
        *out = ngx_media_runtime_stats;
    }
}

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

    /*
     * The timer asks for a fixed interval, so the gap between two ticks is
     * the event loop's own delay: nothing here sleeps, and anything above the
     * interval is time this worker could not get back to its timer.
     */
    if (ngx_media_runtime_last_tick != 0) {
        ngx_msec_t  gap = now - ngx_media_runtime_last_tick;

        ngx_media_runtime_stats.last_gap = gap;

        if (gap > ngx_media_runtime_stats.max_gap) {
            ngx_media_runtime_stats.max_gap = gap;
        }

        if (gap > NGX_MEDIA_RUNTIME_INTERVAL
                   + NGX_MEDIA_RUNTIME_INTERVAL / 2)
        {
            ngx_media_runtime_stats.late_ticks++;
        }
    }

    ngx_media_runtime_last_tick = now;
    ngx_media_runtime_stats.ticks++;

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

        /* only the owner drives a program's selection and outputs */
        if (!ngx_media_route_is_owner((ngx_cycle_t *) ngx_cycle,
                                      ngx_media_owner_hash(&stream->application,
                                                           &stream->name)))
        {
            continue;
        }

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

        /*
         * File sources (goal doc 21) are paced by this tick: one bounded
         * chunk each, so a large file cannot stall a worker and the frames
         * enter the program through the normal source gate.
         */
        ngx_media_file_advance_all(log);

        /*
         * Push destinations watch the HLS directory rather than tapping the
         * segmenter: offering new files here costs a stat per name, and the
         * upload itself happens on the push pool.
         */
        if (policy != NULL && policy->hls_path.len > 0) {
            ngx_media_hls_push_scan(&policy->hls_path, log);
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

ngx_int_t
ngx_media_runtime_init(ngx_cycle_t *cycle, ngx_log_t *log)
{
    if (ngx_media_runtime_owners != NULL) {
        return NGX_OK;
    }

    /*
     * The push backend owns a small upload pool; starting it here means a
     * destination can be added through the control API at any time.
     */
    if (ngx_media_hls_push_register(log) != NGX_OK) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: hls push pool could not be started");
    }

    ngx_media_runtime_owners = ngx_media_owner_dir_attach(cycle, log);

    if (ngx_media_runtime_owners == NULL) {
        return NGX_ERROR;
    }

    if (ngx_media_route_worker_init(cycle, log) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_media_route_set_sink(ngx_media_runtime_route_sink, NULL);

    return NGX_OK;
}

ngx_media_owner_dir_t *
ngx_media_runtime_owner_dir(void)
{
    return ngx_media_runtime_owners;
}

ngx_uint_t
ngx_media_runtime_arm(ngx_cycle_t *cycle, ngx_log_t *log)
{
    ngx_media_policy_t  *policy;

    if (ngx_media_runtime_armed) {
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

/*
 * Releases the runtime outputs of one stream: flush what is buffered, finalize
 * the playlist and any recording part, and free the slot.  Called from ordered
 * teardown, before the stream's feed goes away, because the flush reads from
 * it.  Without this a create/delete cycle leaks an output slot every time.
 */
/* how many runtime output slots are in use: a leak shows up here */
ngx_uint_t
ngx_media_runtime_outputs_active(void)
{
    ngx_uint_t  i, active = 0;

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_OUTPUTS; i++) {
        active += ngx_media_runtime_outputs[i].used ? 1 : 0;
    }

    return active;
}

void
ngx_media_runtime_outputs_release(ngx_media_stream_t *stream)
{
    ngx_uint_t  i;

    if (stream == NULL) {
        return;
    }

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_OUTPUTS; i++) {

        if (ngx_media_runtime_outputs[i].used
            && ngx_media_runtime_outputs[i].stream == stream)
        {
            ngx_media_runtime_outputs_stop(&ngx_media_runtime_outputs[i]);
            return;
        }
    }
}

void
ngx_media_runtime_shutdown(ngx_log_t *log)
{
    ngx_media_hls_push_stop();
    ngx_media_hls_pull_stop_all();
    ngx_media_hls_ingest_stop_all();

    ngx_uint_t  i;

    (void) log;

    ngx_media_runtime_stop();
    ngx_media_route_worker_shutdown(log);

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

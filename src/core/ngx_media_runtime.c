#include "ngx_media_runtime.h"
#include "ngx_media_executor.h"
#include "ngx_media_transform.h"
#include "ngx_media_file.h"
#include "ngx_media_graph.h"
#include "ngx_media_hls_ingest.h"
#include "ngx_media_hls_pull.h"
#include "ngx_media_hls_push.h"

#include <ngx_event.h>
#include <ngx_event_posted.h>

#include "ngx_media_hls_segmenter.h"
#include "ngx_media_record.h"
#include "ngx_media_route.h"
#include "ngx_media_selector.h"
#include "ngx_media_source.h"
#include "ngx_media_ts_demux.h"

#include <time.h>
/*
 * Per-stream outputs.  The program feed is drained on every runtime visit,
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

    /* One external transform feeds one shared prepared package feed. */
    ngx_media_executor_t   executor;
    ngx_media_ts_demux_t   transform_demux;
    ngx_media_prepared_feed_t prepared_feed;
    ngx_media_cursor_t     prepared_cursor;
    ngx_media_ts_mux_t     package_mux;
    ngx_media_ts_burst_t   package_burst;
    ngx_media_trackset_t  *package_tracks;
    unsigned               package_burst_open:1;
    unsigned               transform_ready:1;
    unsigned               executor_events_ready:1;

    ngx_connection_t      *executor_input_connection;
    ngx_connection_t      *executor_output_connection;
    ngx_connection_t      *executor_error_connection;
    ngx_connection_t      *executor_pid_connection;

    uint64_t               generation;
    uint64_t               frames;
    uint64_t               bursts;
} ngx_media_runtime_outputs_t;

typedef struct {
    ngx_uint_t  frames;
    size_t      bytes;
    ngx_msec_t  started;
    ngx_uint_t  exhausted;
} ngx_media_runtime_budget_t;

static ngx_msec_t
ngx_media_runtime_clock_msec(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec  ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (ngx_msec_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
#endif

    return ngx_current_msec;
}

static ngx_uint_t
ngx_media_runtime_budget_ready(ngx_media_runtime_budget_t *budget)
{
    if (budget == NULL) {
        return 0;
    }

    if (budget->frames >= NGX_MEDIA_RUNTIME_VISIT_MAX_FRAMES
        || budget->bytes >= NGX_MEDIA_RUNTIME_VISIT_MAX_BYTES
        || (budget->frames > 0
            && ngx_media_runtime_clock_msec() - budget->started
               >= NGX_MEDIA_RUNTIME_VISIT_MAX_MS))
    {
        budget->exhausted = 1;
        return 0;
    }

    return 1;
}

static void
ngx_media_runtime_budget_account(ngx_media_runtime_budget_t *budget,
    ngx_media_frame_t *frames, ngx_uint_t count)
{
    ngx_uint_t  i;

    if (budget == NULL) {
        return;
    }

    budget->frames += count;

    for (i = 0; i < count; i++) {
        if (frames[i].payload != NULL) {
            budget->bytes += frames[i].payload->len;
        }
    }
}

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

#define NGX_MEDIA_RUNTIME_MAX_TRACK_CONFIG  (64 * 1024)

typedef struct {
    ngx_uint_t              used;

    /*
     * The stream identity the sender addressed.  It was uint32_t here while
     * the IPC header, the owner directory and the routing table all key on
     * the full 64-bit hash, so a routed source was looked up under a
     * truncated identity that could resolve to another stream's slot.
     */
    uint64_t                hash;
    uint64_t                incarnation;
    ngx_media_stream_t     *stream;
    ngx_media_source_t     *source;
    /* reassembly of one publisher's chunked frames, one per routed endpoint */
    ngx_media_ipc_frame_t   frame;
} ngx_media_runtime_routed_t;

static ngx_media_runtime_routed_t ngx_media_runtime_routed[
    NGX_MEDIA_RUNTIME_MAX_ROUTED];
static uint64_t                   ngx_media_runtime_routed_frames;
static uint64_t                   ngx_media_runtime_routed_msgs;
static uint64_t                   ngx_media_runtime_routed_nopayload;
static ngx_media_runtime_stats_t  ngx_media_runtime_stats;
static ngx_media_runtime_sink_pt  ngx_media_runtime_sink;
static void                      *ngx_media_runtime_sink_ctx;
static uint64_t                   ngx_media_runtime_prepared_next_id = 1;

static ngx_event_t          ngx_media_runtime_timer;
static ngx_event_t          ngx_media_runtime_wakeup_event;
static ngx_uint_t           ngx_media_runtime_in_tick;
static ngx_uint_t           ngx_media_runtime_timer_visit;
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

/*
 * One character of a path element built from an application or stream name.
 * Those come from the control API, and a name that contains a separator or
 * ".." would choose a directory outside the configured root, so anything
 * outside this set becomes an underscore.
 */
static u_char
ngx_media_runtime_path_char(u_char c)
{
    if ((c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '-'
        || c == '_')
    {
        return c;
    }

    return '_';
}

/*
 * The directory one program's HLS output goes to: the configured root, then the
 * application, then the stream name.
 *
 * Every program writing into the root would share one playlist name and one
 * set of segment names, which is not a smaller version of the same thing: the
 * programs overwrite each other's media and a viewer cannot tell whose
 * playlist it fetched.  A program's output is its own directory, and that is
 * also the directory a push destination watches.
 *
 * The string lives in the stream's pool: it is read by the HLS segmenter for
 * as long as the stream exists, and the stream's runtime outputs are released
 * before that pool goes.
 */
static ngx_str_t *
ngx_media_runtime_hls_dir(ngx_media_stream_t *stream, const ngx_str_t *root,
    ngx_log_t *log)
{
    ngx_str_t  *path;
    u_char     *p;
    size_t      app_len, len, i;

    /* an application is required by the API; an empty one still gets a level */
    app_len = (stream->application.len > 0) ? stream->application.len : 1;

    len = root->len + 1 + app_len + 1 + stream->name.len;

    path = ngx_pcalloc(stream->pool, sizeof(ngx_str_t));

    if (path == NULL) {
        return NULL;
    }

    p = ngx_pnalloc(stream->pool, len);

    if (p == NULL) {
        return NULL;
    }

    path->data = p;
    path->len = len;

    ngx_memcpy(p, root->data, root->len);
    p += root->len;
    *p++ = '/';

    if (stream->application.len > 0) {

        for (i = 0; i < stream->application.len; i++) {
            *p++ = ngx_media_runtime_path_char(stream->application.data[i]);
        }

    } else {
        *p++ = '_';
    }

    *p++ = '/';

    for (i = 0; i < stream->name.len; i++) {
        *p++ = ngx_media_runtime_path_char(stream->name.data[i]);
    }

    (void) log;

    return path;
}

static void
ngx_media_runtime_outputs_stop(ngx_media_runtime_outputs_t *out);
static ngx_int_t ngx_media_runtime_transform_demux_init(
    ngx_media_runtime_outputs_t *out, ngx_log_t *log);
static ngx_int_t ngx_media_runtime_transform_init(
    ngx_media_runtime_outputs_t *out, const ngx_media_executor_conf_t *conf,
    ngx_log_t *log);
static void ngx_media_runtime_transform_output(void *ctx, const u_char *data,
    size_t len, uint64_t epoch);
static void ngx_media_runtime_transform_event(void *ctx,
    const ngx_media_executor_event_t *event);
static void ngx_media_runtime_executor_io(void *ctx, ngx_uint_t active);
static void ngx_media_runtime_executor_output_handler(ngx_event_t *ev);
static void ngx_media_runtime_executor_error_handler(ngx_event_t *ev);
static void ngx_media_runtime_executor_input_handler(ngx_event_t *ev);
static void ngx_media_runtime_executor_pid_handler(ngx_event_t *ev);
static void ngx_media_runtime_executor_update_input(
    ngx_media_runtime_outputs_t *out);
static void
ngx_media_runtime_executor_close_connection(ngx_connection_t **connection,
    ngx_uint_t write)
{
    ngx_connection_t  *c;

    if (connection == NULL || *connection == NULL) {
        return;
    }

    c = *connection;

    if (write) {
        if (c->write->active) {
            (void) ngx_del_event(c->write, NGX_WRITE_EVENT, 0);
        }

    } else {
        if (c->read->active) {
            (void) ngx_del_event(c->read, NGX_READ_EVENT, 0);
        }
    }

    c->fd = (ngx_socket_t) -1;
    ngx_free_connection(c);
    *connection = NULL;
}
static void
ngx_media_runtime_executor_unwire(ngx_media_runtime_outputs_t *out)
{
    if (out == NULL) {
        return;
    }

    out->executor_events_ready = 0;
    ngx_media_runtime_executor_close_connection(
        &out->executor_input_connection, 1);
    ngx_media_runtime_executor_close_connection(
        &out->executor_output_connection, 0);
    ngx_media_runtime_executor_close_connection(
        &out->executor_error_connection, 0);
    ngx_media_runtime_executor_close_connection(
        &out->executor_pid_connection, 0);
}


static void
ngx_media_runtime_executor_io(void *ctx, ngx_uint_t active)
{
    ngx_media_runtime_outputs_t  *out = ctx;
    ngx_connection_t             *c;
    ngx_log_t                     *log;

    if (out == NULL) {
        return;
    }

    if (!active) {
        ngx_media_runtime_executor_unwire(out);
        return;
    }

    log = ngx_cycle->log;
    ngx_media_runtime_executor_unwire(out);

    c = ngx_get_connection(out->executor.output_fd, log);
    if (c == NULL) {
        goto failed;
    }
    out->executor_output_connection = c;
    c->data = out;
    c->read->handler = ngx_media_runtime_executor_output_handler;
    c->read->log = log;

    if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
        goto failed;
    }

    c = ngx_get_connection(out->executor.error_fd, log);
    if (c == NULL) {
        goto failed;
    }
    out->executor_error_connection = c;
    c->data = out;
    c->read->handler = ngx_media_runtime_executor_error_handler;
    c->read->log = log;

    if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
        goto failed;
    }

    c = ngx_get_connection(out->executor.input_fd, log);
    if (c == NULL) {
        goto failed;
    }
    out->executor_input_connection = c;
    c->data = out;
    c->write->handler = ngx_media_runtime_executor_input_handler;
    c->write->log = log;

    if (out->executor.pid_fd >= 0) {
        c = ngx_get_connection(out->executor.pid_fd, log);
        if (c == NULL) {
            goto failed;
        }
        out->executor_pid_connection = c;
        c->data = out;
        c->read->handler = ngx_media_runtime_executor_pid_handler;
        c->read->log = log;

        if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
            goto failed;
        }
    }

    out->executor_events_ready = 1;
    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: transform executor event wiring ready for %V/%V "
                  "pidfd=%ui",
                  &out->stream->application, &out->stream->name,
                  (ngx_uint_t) (out->executor.pid_fd >= 0));
    ngx_media_runtime_executor_update_input(out);
    return;

failed:
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "media: transform executor event wiring failed for %V/%V; "
                  "using runtime fallback",
                  &out->stream->application, &out->stream->name);
    ngx_media_runtime_executor_unwire(out);
}

static void
ngx_media_runtime_executor_update_input(ngx_media_runtime_outputs_t *out)
{
    ngx_connection_t  *c;
    ngx_uint_t         want;

    if (out == NULL || !out->executor_events_ready
        || out->executor_input_connection == NULL)
    {
        return;
    }

    c = out->executor_input_connection;
    want = (ngx_media_executor_state(&out->executor)
            == NGX_MEDIA_EXECUTOR_RUNNING
            && ngx_media_executor_pending(&out->executor) > 0);

    if (want && !c->write->active) {
        if (ngx_add_event(c->write, NGX_WRITE_EVENT, 0) != NGX_OK) {
            ngx_media_runtime_executor_unwire(out);
        }

    } else if (!want && c->write->active) {
        (void) ngx_del_event(c->write, NGX_WRITE_EVENT, 0);
    }
}

static void
ngx_media_runtime_executor_output_handler(ngx_event_t *ev)
{
    ngx_connection_t            *c;
    ngx_media_runtime_outputs_t *out;

    c = ev != NULL ? ev->data : NULL;
    out = c != NULL ? c->data : NULL;
    if (out == NULL) {
        return;
    }

    ngx_media_executor_io(&out->executor, ngx_current_msec, ev->log);
    ngx_media_runtime_executor_update_input(out);
    ngx_media_runtime_wakeup();
}

static void
ngx_media_runtime_executor_error_handler(ngx_event_t *ev)
{
    ngx_connection_t            *c;
    ngx_media_runtime_outputs_t *out;

    c = ev != NULL ? ev->data : NULL;
    out = c != NULL ? c->data : NULL;
    if (out == NULL) {
        return;
    }

    ngx_media_executor_io(&out->executor, ngx_current_msec, ev->log);
    ngx_media_runtime_executor_update_input(out);
    ngx_media_runtime_wakeup();
}

static void
ngx_media_runtime_executor_input_handler(ngx_event_t *ev)
{
    ngx_connection_t            *c;
    ngx_media_runtime_outputs_t *out;

    c = ev != NULL ? ev->data : NULL;
    out = c != NULL ? c->data : NULL;
    if (out == NULL) {
        return;
    }

    ngx_media_executor_io(&out->executor, ngx_current_msec, ev->log);
    ngx_media_runtime_executor_update_input(out);
    ngx_media_runtime_wakeup();
}

static void
ngx_media_runtime_executor_pid_handler(ngx_event_t *ev)
{
    ngx_connection_t            *c;
    ngx_media_runtime_outputs_t *out;

    c = ev != NULL ? ev->data : NULL;
    out = c != NULL ? c->data : NULL;
    if (out == NULL) {
        return;
    }

    ngx_media_executor_io(&out->executor, ngx_current_msec, ev->log);
    ngx_media_runtime_executor_update_input(out);
    ngx_media_runtime_wakeup();
}

static void ngx_media_runtime_transform_tracks(void *ctx,
    const ngx_media_trackset_t *tracks);
static void ngx_media_runtime_transform_frame(void *ctx,
    const ngx_media_frame_t *frame);
static void ngx_media_runtime_package_flush(
    ngx_media_runtime_outputs_t *out);
static void ngx_media_runtime_package_drain(
    ngx_media_runtime_outputs_t *out, ngx_log_t *log,
    ngx_media_runtime_budget_t *budget);
static ngx_uint_t ngx_media_runtime_burst_keyframe(
    const ngx_media_ts_burst_t *burst);
static ngx_int_t
ngx_media_runtime_transform_demux_init(ngx_media_runtime_outputs_t *out,
    ngx_log_t *log)
{
    ngx_media_ts_demux_conf_t  conf;
    ngx_media_ts_sink_t        sink;

    ngx_memzero(&conf, sizeof(conf));
    conf.max_tracks = NGX_MEDIA_TS_DEFAULT_TRACKS;
    conf.max_au_bytes = NGX_MEDIA_TS_DEFAULT_AU_BYTES;

    ngx_memzero(&sink, sizeof(sink));
    sink.tracks = ngx_media_runtime_transform_tracks;
    sink.frame = ngx_media_runtime_transform_frame;

    return ngx_media_ts_demux_init(&out->transform_demux, &conf, &sink, out,
                                   log);
}

static ngx_int_t
ngx_media_runtime_transform_init(ngx_media_runtime_outputs_t *out,
    const ngx_media_executor_conf_t *conf, ngx_log_t *log)
{
    ngx_media_feed_conf_t  feed_conf;
    uint64_t               id;

    ngx_memzero(&feed_conf, sizeof(feed_conf));
    feed_conf.max_units = 2048;
    feed_conf.max_bytes = 32 * 1024 * 1024;
    feed_conf.max_age = 10000;

    id = ngx_media_runtime_prepared_next_id++;
    if (id == 0) {
        id = ngx_media_runtime_prepared_next_id++;
    }

    if (ngx_media_prepared_feed_init(&out->prepared_feed, id, 1,
                                     &feed_conf, log)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    ngx_media_feed_cursor_init(&out->prepared_feed.feed,
                               &out->prepared_cursor);

    if (ngx_media_ts_mux_init(&out->package_mux, NULL, log) != NGX_OK
        || ngx_media_runtime_transform_demux_init(out, log) != NGX_OK)
    {
        ngx_media_prepared_feed_destroy(&out->prepared_feed);
        ngx_media_ts_mux_destroy(&out->package_mux);
        return NGX_ERROR;
    }

    if (ngx_media_executor_init(&out->executor, conf,
                                ngx_media_runtime_transform_output,
                                ngx_media_runtime_transform_event,
                                out, log)
        != NGX_OK)
    {
        ngx_media_ts_demux_destroy(&out->transform_demux);
        ngx_media_prepared_feed_destroy(&out->prepared_feed);
        ngx_media_ts_mux_destroy(&out->package_mux);
        return NGX_ERROR;
    }
    ngx_media_executor_set_io_callback(&out->executor,
                                      ngx_media_runtime_executor_io);

    out->transform_ready = 1;

    if (ngx_media_executor_start(&out->executor, ngx_current_msec, log)
        != NGX_OK)
    {
        out->transform_ready = 0;
        ngx_media_executor_stop(&out->executor, log);
        ngx_media_ts_demux_destroy(&out->transform_demux);
        ngx_media_prepared_feed_destroy(&out->prepared_feed);
        ngx_media_ts_mux_destroy(&out->package_mux);
        return NGX_ERROR;
    }

    return NGX_OK;
}

static void
ngx_media_runtime_transform_output(void *ctx, const u_char *data, size_t len,
    uint64_t epoch)
{
    ngx_media_runtime_outputs_t *out = ctx;

    if (out == NULL || !out->transform_ready || data == NULL || len == 0
        || epoch != ngx_media_executor_epoch(&out->executor))
    {
        return;
    }

    (void) ngx_media_ts_demux_feed(&out->transform_demux, data, len);
}

static void
ngx_media_runtime_transform_event(void *ctx,
    const ngx_media_executor_event_t *event)
{
    ngx_media_runtime_outputs_t  *out = ctx;
    if (out == NULL || event == NULL) {
        return;
    }

    if (event->type == NGX_MEDIA_EXECUTOR_EVENT_STARTED
        || event->type == NGX_MEDIA_EXECUTOR_EVENT_RESTARTED)
    {
        ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                      "media: transform executor %s for %V/%V epoch %uL",
                      event->type == NGX_MEDIA_EXECUTOR_EVENT_STARTED
                      ? "started" : "restarted",
                      &out->stream->application, &out->stream->name,
                      event->epoch);
    }

    if (event->type == NGX_MEDIA_EXECUTOR_EVENT_FAILED) {
        ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                      "media: transform executor failed for %V/%V",
                      &out->stream->application, &out->stream->name);
    }

    if (event->type != NGX_MEDIA_EXECUTOR_EVENT_EPOCH) {
        return;
    }

    out->package_tracks = NULL;
    out->prepared_feed.epoch = event->epoch;
    ngx_media_feed_discontinuity(&out->prepared_feed.feed);

    if (out->hls_ready) {
        (void) ngx_media_hls_discontinuity(&out->hls);
    }

    ngx_media_ts_demux_destroy(&out->transform_demux);
    (void) ngx_media_runtime_transform_demux_init(out, ngx_cycle->log);
}

static void
ngx_media_runtime_transform_tracks(void *ctx,
    const ngx_media_trackset_t *tracks)
{
    ngx_media_runtime_outputs_t *out = ctx;

    if (out != NULL) {
        out->package_tracks = (ngx_media_trackset_t *) tracks;
    }
}

static void
ngx_media_runtime_transform_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_runtime_outputs_t *out = ctx;

    if (out == NULL || frame == NULL) {
        return;
    }

    (void) ngx_media_prepared_feed_publish(&out->prepared_feed, frame,
                                           ngx_current_msec);
}

static void
ngx_media_runtime_package_flush(ngx_media_runtime_outputs_t *out)
{
    size_t len;

    if (!out->package_burst_open) {
        return;
    }

    if (ngx_media_ts_mux_burst_end(&out->package_mux, &out->package_burst)
        == NGX_OK)
    {
        len = ngx_media_ts_burst_size(&out->package_burst);

        if (len > 0) {
            if (out->hls_ready) {
                (void) ngx_media_hls_add_burst(&out->hls,
                                               &out->package_burst);
            }

            if (out->program_ready) {
                (void) ngx_media_record_append(&out->program,
                                               out->package_burst.backing,
                                               0, len);
            }

            if (ngx_media_runtime_sink != NULL) {
                ngx_media_runtime_sink(ngx_media_runtime_sink_ctx,
                                       out->stream,
                                       out->package_burst.backing, len,
                                       ngx_media_runtime_burst_keyframe(
                                           &out->package_burst));
            }
        }
    }

    ngx_media_ts_mux_burst_destroy(&out->package_burst);
    out->package_burst_open = 0;
}

static void
ngx_media_runtime_package_drain(ngx_media_runtime_outputs_t *out,
    ngx_log_t *log, ngx_media_runtime_budget_t *budget)
{
    ngx_media_frame_t  frames[64];
    ngx_uint_t          count, i, status, max_units;
    size_t              max_bytes;

    for (;;) {
        if (!ngx_media_runtime_budget_ready(budget)) {
            break;
        }

        max_units = 64;
        max_bytes = 512 * 1024;

        if (budget != NULL) {
            if (max_units > NGX_MEDIA_RUNTIME_VISIT_MAX_FRAMES
                              - budget->frames)
            {
                max_units = NGX_MEDIA_RUNTIME_VISIT_MAX_FRAMES
                            - budget->frames;
            }

            if (max_bytes > NGX_MEDIA_RUNTIME_VISIT_MAX_BYTES
                              - budget->bytes)
            {
                max_bytes = NGX_MEDIA_RUNTIME_VISIT_MAX_BYTES
                            - budget->bytes;
            }
        }

        if (max_units == 0 || max_bytes == 0) {
            if (budget != NULL) {
                budget->exhausted = 1;
            }
            break;
        }

        status = ngx_media_feed_read(&out->prepared_feed.feed,
                                     &out->prepared_cursor, max_units,
                                     max_bytes, ngx_current_msec, frames,
                                     &count);

        if (status == NGX_MEDIA_FEED_GENERATION_MISMATCH
            || status == NGX_MEDIA_FEED_OVERRUN)
        {
            if (out->hls_ready) {
                (void) ngx_media_hls_discontinuity(&out->hls);
            }

            (void) ngx_media_feed_resync(&out->prepared_feed.feed,
                                         &out->prepared_cursor,
                                         NGX_MEDIA_FEED_RESYNC_KEYFRAME);
            continue;
        }

        if (status != NGX_MEDIA_FEED_BATCH) {
            break;
        }

        ngx_media_runtime_budget_account(budget, frames, count);

        if (out->package_tracks != NULL
            && ngx_media_ts_mux_set_tracks(&out->package_mux,
                                           out->package_tracks) == NGX_OK)
        {
            /* Track contracts are immutable for this demux epoch. */
        }

        if (!out->package_burst_open
            && ngx_media_ts_mux_burst_init(&out->package_mux,
                                           &out->package_burst,
                                           256 * 1024) != NGX_OK)
        {
            ngx_media_feed_release(frames, count);
            return;
        }

        out->package_burst_open = 1;

        for (i = 0; i < count; i++) {
            if (ngx_media_ts_mux_write_frame(&out->package_mux,
                                             &out->package_burst,
                                             &frames[i], 0) == NGX_AGAIN)
            {
                ngx_media_runtime_package_flush(out);

                if (ngx_media_ts_mux_burst_init(&out->package_mux,
                                                &out->package_burst,
                                                256 * 1024) != NGX_OK)
                {
                    continue;
                }

                out->package_burst_open = 1;
                (void) ngx_media_ts_mux_write_frame(&out->package_mux,
                                                    &out->package_burst,
                                                    &frames[i], 0);
            }
        }

        ngx_media_feed_release(frames, count);
        ngx_media_runtime_package_flush(out);
    }

    (void) log;
}

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
        ngx_str_t  *dir = ngx_media_runtime_hls_dir(stream, &policy->hls_path,
                                                    log);

        if (dir == NULL) {
            ngx_media_runtime_outputs_stop(out);
            return NULL;
        }

        ngx_media_hls_conf_default(&hls_conf);

        hls_conf.path = *dir;
        hls_conf.target_duration = NGX_MEDIA_RUNTIME_HLS_TARGET;
        hls_conf.min_duration = NGX_MEDIA_RUNTIME_HLS_TARGET / 2;
        hls_conf.max_duration = NGX_MEDIA_RUNTIME_HLS_TARGET * 2;

        if (ngx_media_hls_init(&out->hls, &hls_conf, log) != NGX_OK) {
            ngx_media_runtime_outputs_stop(out);
            return NULL;
        }

        out->hls_ready = 1;

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: hls output started for %V/%V in %V",
                      &stream->application, &stream->name, dir);
    }

    if (policy->record_program_path.len > 0) {
        ngx_media_record_conf_default(&rec_conf);

        rec_conf.path = policy->record_program_path;
        rec_conf.tap = NGX_MEDIA_RECORD_PROGRAM;

        if (ngx_media_record_init(&out->program, &rec_conf, log) != NGX_OK) {
            ngx_media_runtime_outputs_stop(out);
            return NULL;
        }

        out->program_ready = 1;

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: program recording started for %V/%V",
                      &stream->application, &stream->name);
    }

    if (policy->record_iso_path.len > 0) {
        ngx_media_record_conf_default(&rec_conf);

        rec_conf.path = policy->record_iso_path;
        rec_conf.tap = NGX_MEDIA_RECORD_ISO;

        if (ngx_media_ts_mux_init(&out->iso_mux, NULL, log) != NGX_OK
            || ngx_media_record_init(&out->iso, &rec_conf, log) != NGX_OK)
        {
            ngx_media_runtime_outputs_stop(out);
            return NULL;
        }

        out->iso_ready = 1;

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: iso recording started for %V/%V source %V",
                      &stream->application, &stream->name,
                      &policy->record_iso_source);
    }

    if (stream->media_mode == NGX_MEDIA_STREAM_MEDIA_PROFILE
        && policy->transform_executor.executable.len > 0
        && ngx_media_runtime_transform_init(out,
                                            &policy->transform_executor,
                                            log)
           != NGX_OK)
    {
        ngx_media_runtime_outputs_stop(out);
        return NULL;
    }

    return out;
}

ngx_int_t
ngx_media_runtime_admit(ngx_media_stream_t *stream, ngx_log_t *log)
{
    ngx_media_policy_t          *policy;
    ngx_media_runtime_outputs_t *out;

    if (stream == NULL) {
        return NGX_ERROR;
    }

    policy = ngx_media_runtime_policy();
    if (stream->media_mode == NGX_MEDIA_STREAM_MEDIA_PROFILE
        && (policy == NULL || policy->transform_executor.executable.len == 0))
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: profile transform is not configured for %V/%V",
                      &stream->application, &stream->name);
        return NGX_ERROR;
    }


    if (policy == NULL
        || (policy->hls_path.len == 0
            && policy->record_program_path.len == 0
            && policy->record_iso_path.len == 0
            && policy->transform_executor.executable.len == 0))
    {
        return NGX_OK;
    }

    out = ngx_media_runtime_outputs_get(stream, log);

    if (out == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: no runtime output capacity for %V/%V",
                      &stream->application, &stream->name);
        return NGX_ERROR;
    }

    return NGX_OK;
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
    if (out->transform_ready) {
        if (ngx_media_ts_mux_burst_end(&out->mux, &out->burst) == NGX_OK) {
            len = ngx_media_ts_burst_size(&out->burst);

            if (len > 0
                && ngx_media_executor_feed(&out->executor,
                                           out->burst.backing, 0, len)
                   != NGX_OK)
            {
                ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                              "media: transform input journal is full for "
                              "%V/%V",
                              &out->stream->application, &out->stream->name);
            }
        }
        ngx_media_runtime_executor_update_input(out);

        out->bursts++;
        ngx_media_ts_mux_burst_destroy(&out->burst);
        out->burst_open = 0;
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
    ngx_log_t *log, ngx_media_runtime_budget_t *budget)
{
    ngx_media_frame_t   frames[64];
    ngx_media_stream_t  *stream = out->stream;
    ngx_uint_t           count, i, status, max_units;
    size_t               max_bytes;

    if (out->generation != stream->generation) {
        out->generation = stream->generation;

        if (out->transform_ready) {
            ngx_media_executor_stop(&out->executor, log);

            if (ngx_media_executor_start(&out->executor, ngx_current_msec,
                                         log)
                != NGX_OK)
            {
                ngx_log_error(NGX_LOG_WARN, log, 0,
                              "media: transform executor epoch restart "
                              "deferred for %V/%V",
                              &stream->application, &stream->name);
            }

        } else if (out->hls_ready) {
            (void) ngx_media_hls_discontinuity(&out->hls);
        }
    }


    for ( ;; ) {
        if (!ngx_media_runtime_budget_ready(budget)) {
            break;
        }

        max_units = 64;
        max_bytes = 512 * 1024;

        if (budget != NULL) {
            if (max_units > NGX_MEDIA_RUNTIME_VISIT_MAX_FRAMES
                              - budget->frames)
            {
                max_units = NGX_MEDIA_RUNTIME_VISIT_MAX_FRAMES
                            - budget->frames;
            }

            if (max_bytes > NGX_MEDIA_RUNTIME_VISIT_MAX_BYTES
                              - budget->bytes)
            {
                max_bytes = NGX_MEDIA_RUNTIME_VISIT_MAX_BYTES
                            - budget->bytes;
            }
        }

        if (max_units == 0 || max_bytes == 0) {
            if (budget != NULL) {
                budget->exhausted = 1;
            }
            break;
        }

        status = ngx_media_feed_read(&stream->program_feed, &out->cursor,
                                     max_units, max_bytes, ngx_current_msec,
                                     frames, &count);

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
        ngx_media_runtime_budget_account(budget, frames, count);

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
    if (out->transform_ready) {
        ngx_media_executor_maintenance(&out->executor, ngx_current_msec, log);

        if (!out->executor_events_ready) {
            ngx_media_executor_io(&out->executor, ngx_current_msec, log);
        }
        ngx_media_runtime_package_drain(out, log, budget);
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
    if (out->transform_ready) {
        ngx_media_runtime_package_flush(out);
        ngx_media_executor_stop(&out->executor, ngx_cycle->log);
        ngx_media_ts_demux_destroy(&out->transform_demux);
        ngx_media_prepared_feed_destroy(&out->prepared_feed);
        ngx_media_ts_mux_destroy(&out->package_mux);
        out->transform_ready = 0;
    }


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
ngx_media_runtime_route_sink(void *ctx, uint64_t hash,
    const ngx_media_ipc_header_t *header, ngx_media_buf_t *payload)
{
    ngx_media_registry_t  *registry;
    ngx_media_stream_t    *stream;
    ngx_media_source_t    *source;
    ngx_str_t              application, name, source_id;
    u_char                *p, *slash;
    ngx_media_feed_conf_t  feed_conf;

    (void) ctx;

    if (header == NULL) {
        return NGX_ERROR;
    }


    /*
     * A graph operation is desired state, not media: it belongs to this
     * worker's registry and needs no program of its own.  Applying it here
     * keeps the control plane on the same transport as the routing escape
     * hatch, with the same encoder, framing and bounds.
     */
    if (header->type == NGX_MEDIA_IPC_MSG_GRAPH) {
        return ngx_media_graph_apply(header, payload);
    }

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        return NGX_ERROR;
    }


    if (header->type == NGX_MEDIA_IPC_MSG_OPEN) {

        if (header->incarnation == 0
            || payload == NULL || ngx_media_buf_size(payload) < 3)
        {
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

        /*
         * The datagram itself is bounded, but its fields are copied into
         * long-lived pool objects below.  Keep the identity fields at the same
         * bound as graph operations; otherwise a peer could turn one OPEN
         * into a nearly-64 KiB stream/source allocation on every reconnect.
         */
        if (application.len == 0 || name.len == 0 || source_id.len == 0
            || application.len > NGX_MEDIA_GRAPH_MAX_NAME
            || name.len > NGX_MEDIA_GRAPH_MAX_NAME
            || source_id.len > NGX_MEDIA_GRAPH_MAX_NAME)
        {
            return NGX_ERROR;
        }

        feed_conf.max_units = 2048;
        feed_conf.max_bytes = 32 * 1024 * 1024;
        feed_conf.max_age = 10000;

        stream = ngx_media_registry_stream(registry, &application, &name);

        if (stream != NULL
            && stream->incarnation != header->incarnation)
        {
            ngx_media_runtime_stats.routed_identity_mismatches++;
            return NGX_OK;
        }

        if (stream == NULL) {
            stream = ngx_media_registry_stream_create(registry, &application,
                                                      &name, &feed_conf,
                                                      ngx_cycle->log);

            if (stream != NULL) {
                stream->incarnation = header->incarnation;
            }
        }

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

        /*
         * One slot per hash.  Frame lookups resolve to the oldest match, so a
         * repeated OPEN that took a second slot would leave every frame of
         * the new publisher pointed at the old, dead source for the life of
         * its session.  The source this OPEN replaces was already removed
         * above, so the slot is dropped without touching it.
         */
        {
            ngx_uint_t  i;

            for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_ROUTED; i++) {

                if (ngx_media_runtime_routed[i].used
                    && ngx_media_runtime_routed[i].hash == hash)
                {
                    ngx_media_ipc_frame_reset(&ngx_media_runtime_routed[i].frame);
                    ngx_memzero(&ngx_media_runtime_routed[i],
                                sizeof(ngx_media_runtime_routed_t));
                    break;
                }
            }
        }

        /* remember the routed source so its frames have a destination */
        {
            ngx_uint_t  i;

            for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_ROUTED; i++) {

                if (!ngx_media_runtime_routed[i].used) {
                    ngx_media_runtime_routed[i].used = 1;
                    ngx_media_runtime_routed[i].hash = hash;
                    ngx_media_runtime_routed[i].incarnation =
                        header->incarnation;
                    ngx_media_runtime_routed[i].stream = stream;
                    ngx_media_runtime_routed[i].source = source;
                    break;
                }
            }

            if (i == NGX_MEDIA_RUNTIME_MAX_ROUTED) {
                ngx_media_runtime_stats.routed_slot_overflows++;
                ngx_media_stream_source_remove(stream, source);
                return NGX_OK;
            }
        }

        /* the owner claims the stream in the shared directory */
        if (ngx_media_runtime_owners != NULL) {
            (void) ngx_media_owner_dir_claim(ngx_media_runtime_owners, hash,
                                             (ngx_uint_t) ngx_worker);
        }

        /*
         * A publisher creates its stream here, on the owner, not through the
         * API - so the other workers hear about it the way they hear about any
         * other mutation: a replica of the graph, carrying the source as
         * desired state.  Without this the stream a publisher is feeding would
         * be invisible to a control request that landed elsewhere.
         */
        (void) ngx_media_graph_stream_set(stream);
        (void) ngx_media_graph_source_set(stream, source, NULL, NULL);

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
            size_t             config_len;

            if (left < sizeof(fields)) {
                ngx_media_trackset_destroy(&set);
                return NGX_ERROR;
            }

            ngx_memcpy(fields, q, sizeof(fields));
            q += sizeof(fields);
            left -= sizeof(fields);
            config_len = (size_t) fields[9];

            if (config_len > left
                || config_len > NGX_MEDIA_RUNTIME_MAX_TRACK_CONFIG)
            {
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

            if (config_len > 0) {
                ngx_media_buf_t  *config = ngx_media_buf_alloc(config_len);
                if (config == NULL) {
                    ngx_media_trackset_destroy(&set);
                    return NGX_ERROR;
                }

                ngx_memcpy(ngx_media_buf_data(config), q, config_len);
                (void) ngx_media_buf_freeze(config, config_len);

                track.config = config;
                (void) ngx_media_trackset_add(&set, &track);
                ngx_media_buf_unref(config);

            } else {
                (void) ngx_media_trackset_add(&set, &track);
            }

            q += config_len;
            left -= config_len;
        }

        for (i2 = 0; i2 < NGX_MEDIA_RUNTIME_MAX_ROUTED; i2++) {

            if (ngx_media_runtime_routed[i2].used
                && ngx_media_runtime_routed[i2].hash == hash
                && ngx_media_runtime_routed[i2].incarnation
                   == header->incarnation)
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

        if (i2 == NGX_MEDIA_RUNTIME_MAX_ROUTED) {
            for (i2 = 0; i2 < NGX_MEDIA_RUNTIME_MAX_ROUTED; i2++) {
                if (ngx_media_runtime_routed[i2].used
                    && ngx_media_runtime_routed[i2].hash == hash)
                {
                    ngx_media_runtime_stats.routed_identity_mismatches++;
                    break;
                }
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
                && ngx_media_runtime_routed[i].hash == hash
                && ngx_media_runtime_routed[i].incarnation
                   == header->incarnation)
            {
                ngx_media_stream_t  *gone_stream =
                    ngx_media_runtime_routed[i].stream;
                ngx_media_source_t  *gone_source =
                    ngx_media_runtime_routed[i].source;

                ngx_media_stream_source_remove(gone_stream, gone_source);

                /*
                 * The replicas drop the same source: the publisher that
                 * created it is gone, and a replica that kept it would answer
                 * with a source nothing feeds.
                 */
                (void) ngx_media_graph_source_delete(gone_stream,
                                                     &gone_source->id,
                                                     gone_stream->revision);

                ngx_media_ipc_frame_reset(&ngx_media_runtime_routed[i].frame);

                ngx_memzero(&ngx_media_runtime_routed[i],
                            sizeof(ngx_media_runtime_routed_t));
                break;
            }
        }

        if (i == NGX_MEDIA_RUNTIME_MAX_ROUTED) {
            for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_ROUTED; i++) {
                if (ngx_media_runtime_routed[i].used
                    && ngx_media_runtime_routed[i].hash == hash)
                {
                    ngx_media_runtime_stats.routed_identity_mismatches++;
                    break;
                }
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
        ngx_media_ipc_message_t  message;
        ngx_media_buf_t         *assembled;
        ngx_media_frame_t        frame;
        ngx_uint_t               i;
        ngx_int_t                status;
        for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_ROUTED; i++) {

            if (ngx_media_runtime_routed[i].used
                && ngx_media_runtime_routed[i].hash == hash
                && ngx_media_runtime_routed[i].incarnation
                   == header->incarnation)
            {
                break;
            }
        }

        ngx_media_runtime_routed_msgs++;

        if (i == NGX_MEDIA_RUNTIME_MAX_ROUTED) {
            ngx_uint_t  j;

            for (j = 0; j < NGX_MEDIA_RUNTIME_MAX_ROUTED; j++) {
                if (ngx_media_runtime_routed[j].used
                    && ngx_media_runtime_routed[j].hash == hash)
                {
                    ngx_media_runtime_stats.routed_identity_mismatches++;
                    return NGX_OK;
                }
            }

            ngx_media_runtime_stats.routed_no_slot++;
            return NGX_OK;
        }

        if (payload == NULL) {
            ngx_media_runtime_routed_nopayload++;
            ngx_media_runtime_stats.routed_no_payload++;
            return NGX_OK;
        }


        /*
         * A frame larger than one datagram arrives as a run of chunks, each
         * carrying its own offset and the MORE flag.  Publishing a chunk would
         * put a truncated access unit into the program, so the run is
         * reassembled per endpoint first and only a complete frame is handed
         * on (the 4 MB ceiling is enforced by the reassembler).
         */
        message.header = *header;
        message.payload = payload;
        message.offset = 0;
        message.length = ngx_media_buf_size(payload);

        status = ngx_media_ipc_frame_feed(&ngx_media_runtime_routed[i].frame,
                                          &message);

        if (status == NGX_AGAIN) {
            return NGX_OK;   /* more chunks follow for this frame */
        }

        if (status != NGX_OK) {
            ngx_media_runtime_stats.routed_reassembly_errors++;
            return NGX_ERROR;   /* inconsistent run: the frame was dropped */
        }

        assembled = ngx_media_runtime_routed[i].frame.payload;

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
         * adopt() takes over the reference it is given, and the reassembly
         * still owns its own: take a reference for the frame so resetting the
         * reassembly cannot free a buffer the program is still reading.
         */
        ngx_media_frame_adopt(&frame, ngx_media_buf_ref(assembled));

        ngx_media_ipc_frame_reset(&ngx_media_runtime_routed[i].frame);

        source = ngx_media_runtime_routed[i].source;
        stream = ngx_media_runtime_routed[i].stream;

        ngx_media_health_media(&source->health, frame.dts, ngx_current_msec);

        if (ngx_media_stream_publish(stream, source, &frame,
                                     ngx_current_msec) != NGX_OK)
        {
            ngx_media_runtime_stats.routed_publish_errors++;
        }

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
ngx_media_runtime_prepare_drain(ngx_media_stream_t *stream, ngx_log_t *log,
    ngx_media_runtime_budget_t *budget)
{
    ngx_media_runtime_prepare_t  *slot;
    ngx_media_frame_t             frames[NGX_MEDIA_RUNTIME_MAX_FRAMES_TICK];
    ngx_uint_t                    count, i, status, max_units;
    size_t                        max_bytes;

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
        if (!ngx_media_runtime_budget_ready(budget)) {
            break;
        }

        max_units = NGX_MEDIA_RUNTIME_MAX_FRAMES_TICK;
        max_bytes = 1024 * 1024;

        if (budget != NULL) {
            if (max_units > NGX_MEDIA_RUNTIME_VISIT_MAX_FRAMES
                              - budget->frames)
            {
                max_units = NGX_MEDIA_RUNTIME_VISIT_MAX_FRAMES
                            - budget->frames;
            }

            if (max_bytes > NGX_MEDIA_RUNTIME_VISIT_MAX_BYTES
                              - budget->bytes)
            {
                max_bytes = NGX_MEDIA_RUNTIME_VISIT_MAX_BYTES
                            - budget->bytes;
            }
        }

        if (max_units == 0 || max_bytes == 0) {
            if (budget != NULL) {
                budget->exhausted = 1;
            }
            break;
        }

        status = ngx_media_feed_read(&stream->program_feed, &slot->cursor,
                                     max_units, max_bytes, ngx_current_msec,
                                     frames, &count);

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
        ngx_media_runtime_budget_account(budget, frames, count);

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
ngx_media_runtime_claim(const ngx_str_t *application, const ngx_str_t *name)
{
    uint64_t  hash;

    if (ngx_media_runtime_owners == NULL || application == NULL
        || name == NULL)
    {
        return;
    }

    hash = ngx_media_owner_hash(application, name);

    (void) ngx_media_owner_dir_claim(ngx_media_runtime_owners, hash,
                                     (ngx_uint_t) ngx_worker);
}

void
ngx_media_runtime_release(const ngx_str_t *application, const ngx_str_t *name)
{
    uint64_t  hash;

    if (ngx_media_runtime_owners == NULL || application == NULL
        || name == NULL)
    {
        return;
    }

    hash = ngx_media_owner_hash(application, name);

    ngx_media_owner_dir_release(ngx_media_runtime_owners, hash,
                                (ngx_uint_t) ngx_worker);
}

void
ngx_media_runtime_progress(const ngx_str_t *application, const ngx_str_t *name,
    uint64_t local_generation, uint64_t local_frames,
    ngx_media_runtime_progress_t *out)
{
    ngx_media_owner_record_t  record;
    uint64_t                  hash;
    ngx_uint_t                owner;

    if (out == NULL) {
        return;
    }

    out->owner = (ngx_uint_t) ngx_worker;
    out->local = 1;
    out->generation = local_generation;
    out->frames = local_frames;

    if (ngx_media_runtime_owners == NULL || application == NULL
        || name == NULL)
    {
        return;
    }

    /*
     * The owner is derived from the full 64-bit identity.  A uint32_t copy of
     * it named a different worker as soon as hash % workers and
     * (hash & 0xffffffff) % workers differ - the API's `owner` field then
     * disagreed with the worker that drove the stream, and the owner directory
     * lookup missed a record that was published under the full hash.
     */
    hash = ngx_media_owner_hash(application, name);
    owner = ngx_media_route_owner((ngx_cycle_t *) ngx_cycle, hash);

    out->owner = owner;

    if (owner == (ngx_uint_t) ngx_worker) {
        /* this worker drives the program: its own numbers are the answer */
        return;
    }

    /*
     * A replica does not invent progress.  It reports what the owner
     * published, and when the owner is not reporting - never claimed, dead, or
     * past its heartbeat - it says so by reporting nothing rather than the
     * zeros of a program that is running somewhere else.
     */
    out->local = 0;
    out->generation = 0;
    out->frames = 0;

    if (ngx_media_owner_dir_observe(ngx_media_runtime_owners, hash, &record)
        == NGX_OK)
    {
        out->generation = record.generation;
        out->frames = record.frames;
    }
}

static ngx_msec_t                 ngx_media_runtime_last_tick;

void
ngx_media_runtime_stats_get(ngx_media_runtime_stats_t *out)
{
    if (out != NULL) {
        *out = ngx_media_runtime_stats;
    }
}

void
ngx_media_runtime_wakeup(void)
{
    if (!ngx_media_runtime_armed
        || ngx_media_runtime_in_tick
        || ngx_exiting || ngx_terminate || ngx_quit)
    {
        return;
    }

    if (ngx_media_runtime_wakeup_event.posted) {
        ngx_media_runtime_stats.wakeup_coalesced++;
        return;
    }

    ngx_post_event(&ngx_media_runtime_wakeup_event, &ngx_posted_events);
    ngx_media_runtime_stats.wakeups++;
}

static ngx_uint_t
ngx_media_runtime_visit(ngx_log_t *log)
{
    ngx_media_registry_t        *registry;
    ngx_media_registry_entry_t  *entry;
    ngx_media_selector_result_t  res;
    ngx_media_stream_t          *stream;
    ngx_media_policy_t          *policy;
    ngx_msec_t                   now;
    ngx_media_runtime_budget_t   budget;
    ngx_queue_t                 *q;
    uint64_t                     before = 0;

    if (ngx_media_runtime_in_tick) {
        return 0;
    }

    ngx_media_runtime_in_tick = 1;

    now = ngx_current_msec;
    ngx_memzero(&budget, sizeof(budget));
    budget.started = ngx_media_runtime_clock_msec();
    policy = ngx_media_runtime_policy();

    /*
     * Only timer visits measure event-loop cadence.  Posted media wakeups are
     * intentionally much more frequent and must not make a healthy timer look
     * late or turn the cadence metric into a media-rate metric.
     */
    if (ngx_media_runtime_timer_visit) {
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
    }

    ngx_media_runtime_stats.ticks++;
    if (ngx_media_runtime_timer_visit) {
        ngx_media_runtime_stats.periodic_visits++;
    } else {
        ngx_media_runtime_stats.media_only_visits++;
    }

    if (ngx_media_runtime_timer_visit) {
        ngx_media_graph_repair_tick(log);
        ngx_media_runtime_stats.reconnecting = 0;
    }

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        ngx_media_runtime_in_tick = 0;
        return 0;
    }

    if (ngx_media_runtime_timer_visit) {
        /*
         * File sources (goal doc 21) are paced by the periodic visit: one
         * bounded chunk each, so a large file cannot stall a worker and the
         * frames enter the program through the normal source gate.  This walks
         * every file source, so it must not run in a posted media visit.
         */
        ngx_media_file_advance_all(log);

        /*
         * HLS readers perform transport and demux work in their own threads
         * and signal their owning worker through a source eventfd.  The
         * periodic drain-all pass remains a bounded safety net for a source
         * event that races teardown.
         */
        ngx_media_hls_pull_drain_all();
        ngx_media_hls_ingest_drain_all();
        ngx_media_hls_pull_reap(log);
        ngx_media_hls_ingest_reap(log);

        /*
         * A deleted stream's pool is held until the last reader that
         * references it has been closed.  Reaping above makes the memory
         * freeable without making the delete wait on an origin.
         */
        ngx_media_registry_drain(registry, log);
    }

    for (q = ngx_queue_head(&registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = q->next)
    {
        if (!ngx_media_runtime_budget_ready(&budget)) {
            break;
        }

        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);
        stream = &entry->stream;

        /* only the owner drives a program's selection and outputs */
        if (!ngx_media_route_is_owner((ngx_cycle_t *) ngx_cycle,
                                      ngx_media_owner_hash(&stream->application,
                                                           &stream->name)))
        {
            continue;
        }

        if (ngx_media_runtime_timer_visit) {
            /*
             * Refresh our claim on the stream.  Ownership is recorded in a
             * shared directory with a heartbeat, and a record whose heartbeat
             * has gone stale is reclaimable by another worker.  Heartbeats
             * therefore belong to the periodic visit, not to media rate.
             */
            if (ngx_media_runtime_owners != NULL) {
                uint64_t  hash = ngx_media_owner_hash(&stream->application,
                                                      &stream->name);

                (void) ngx_media_owner_dir_heartbeat(
                    ngx_media_runtime_owners, hash, (ngx_uint_t) ngx_worker,
                    stream->generation, stream->program_frames,
                    ngx_media_stream_source_count(stream));
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
        }
        if (policy != NULL
            && (policy->hls_path.len > 0
                || policy->record_program_path.len > 0
                || policy->record_iso_path.len > 0
                || policy->transform_executor.executable.len > 0))
        {
            ngx_media_runtime_outputs_t  *out;

            out = ngx_media_runtime_outputs_get(stream, log);

            if (out != NULL) {

                /*
                 * A push destination watches a directory, and a program's
                 * HLS output is a directory of its own.  Directory discovery
                 * is periodic maintenance; output feed progress below is
                 * allowed on a posted media visit.
                 */
                if (ngx_media_runtime_timer_visit && out->hls_ready) {
                    ngx_media_hls_push_scan(&out->hls.conf.path, log);
                }

                ngx_media_runtime_outputs_drain(out, log, &budget);
            }
        }

        if (stream->active != NULL && stream->active->tracks != NULL) {
            ngx_media_runtime_prepare_drain(stream, log, &budget);
        }

        if (budget.exhausted) {
            break;
        }

        if (ngx_media_runtime_timer_visit) {
            /*
             * A source whose transport is up but which is not carrying media
             * yet is reconnecting (goal doc 28).  Selector and liveness
             * accounting stay at the periodic cadence.
             */
            {
                ngx_queue_t       *sq;
                ngx_media_source_t *source;

                for (sq = ngx_queue_head(&stream->sources);
                     sq != (ngx_queue_t *) &stream->sources;
                     sq = sq->next)
                {
                    source = ngx_queue_data(sq, ngx_media_source_t, queue);

                    if (source->state == NGX_MEDIA_SOURCE_AWAITING_SYNC) {
                        ngx_media_runtime_stats.reconnecting++;
                    }
                }
            }

            if (stream->switches != before) {
                ngx_log_error(NGX_LOG_NOTICE, log, 0,
                              "media: selector switched stream=%V/%V "
                              "active=%V generation=%ui switches=%uL",
                              &stream->application, &stream->name,
                              stream->active != NULL ? &stream->active->id
                                                     : &ngx_media_runtime_none,
                              stream->generation, stream->switches);
            }
        }
    }

    {
        ngx_msec_t  service = ngx_media_runtime_clock_msec() - budget.started;

        ngx_media_runtime_stats.last_service = service;

        if (service > ngx_media_runtime_stats.max_service) {
            ngx_media_runtime_stats.max_service = service;
        }
    }

    if (now - ngx_media_runtime_last_idle_log >= 1000) {
        ngx_media_runtime_last_idle_log = now;
    }
    ngx_media_runtime_in_tick = 0;
    return budget.exhausted;
}
void
ngx_media_runtime_periodic_visit(ngx_log_t *log)
{
    ngx_uint_t  more;

    ngx_media_runtime_timer_visit = 1;
    more = ngx_media_runtime_visit(log);
    ngx_media_runtime_timer_visit = 0;

    if (more) {
        ngx_media_runtime_stats.budget_reposts++;
        ngx_media_runtime_wakeup();
    }
}


void
ngx_media_runtime_media_visit(ngx_log_t *log)
{
    ngx_uint_t  more;

    ngx_media_runtime_timer_visit = 0;
    more = ngx_media_runtime_visit(log);

    if (more) {
        ngx_media_runtime_stats.budget_reposts++;
        ngx_media_runtime_wakeup();
    }
}



static void
ngx_media_runtime_wakeup_handler(ngx_event_t *ev)
{
    if (!ngx_media_runtime_armed
        || ngx_exiting || ngx_terminate || ngx_quit)
    {
        return;
    }

    ngx_media_runtime_media_visit(ev->log);
}

static void
ngx_media_runtime_handler(ngx_event_t *ev)
{
    if (!ngx_media_runtime_armed) {
        return;
    }

    ngx_media_runtime_periodic_visit(ev->log);

    /* stop re-arming during shutdown so the worker can exit */
    if (ngx_exiting || ngx_terminate || ngx_quit) {
        return;
    }

    ngx_add_timer(ev, NGX_MEDIA_RUNTIME_INTERVAL);
}

/*
 * The shared revision sequence, read from the owner directory.  Zero when
 * there is no directory: the caller then keeps a local number, which is what a
 * single writer wants anyway.
 */
static uint64_t
ngx_media_runtime_revision(void)
{
    return ngx_media_owner_dir_revision_next(ngx_media_runtime_owners);
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

    /*
     * Every desired-state mutation takes its revision from the shared
     * sequence, so a mutation on one worker and a mutation on another are
     * ordered: replicas resolve the conflict by the higher number instead of
     * each keeping whichever operation reached it last.
     */
    ngx_media_revision_provider(ngx_media_runtime_revision);

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
    ngx_memzero(&ngx_media_runtime_wakeup_event, sizeof(ngx_event_t));

    ngx_media_runtime_timer.handler = ngx_media_runtime_handler;
    ngx_media_runtime_timer.log = log;
    ngx_media_runtime_timer.data = cycle;

    ngx_media_runtime_wakeup_event.handler = ngx_media_runtime_wakeup_handler;
    ngx_media_runtime_wakeup_event.log = log;
    ngx_media_runtime_wakeup_event.data = cycle;

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
    ngx_media_runtime_in_tick = 0;
    ngx_media_runtime_timer_visit = 0;

    if (ngx_media_runtime_timer.timer_set) {
        ngx_del_timer(&ngx_media_runtime_timer);
    }

    if (ngx_media_runtime_wakeup_event.posted) {
        ngx_delete_posted_event(&ngx_media_runtime_wakeup_event);
    }
}

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

/*
 * Releases the runtime state of one stream: flush what is buffered, finalize
 * the playlist and any recording part, and free the output slot; drop the
 * player preparation and the routed slots.  Called from ordered teardown,
 * before the stream's feed goes away, because the flush reads from it.
 * Without this a create/delete cycle leaks an output slot every time.
 *
 * The stream's player preparation goes with it.  It reads the same feed and
 * holds the FLV conversion of the program, so it has to be dropped at this
 * same point in ordered teardown; releasing it only at worker exit would leak
 * a prepare slot, and the program payload pinned behind its ring, on every
 * create/delete cycle -- after NGX_MEDIA_RUNTIME_MAX_PREPARE cycles the next
 * stream's prepare() returns NULL and its RTMP players and destinations
 * silently receive nothing.
 */
void
ngx_media_runtime_stream_release(ngx_media_stream_t *stream)
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
            break;
        }
    }

    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_PREPARE; i++) {

        if (ngx_media_runtime_prepares[i].used
            && ngx_media_runtime_prepares[i].stream == stream)
        {
            ngx_media_rtmp_prepare_destroy(
                &ngx_media_runtime_prepares[i].prepare);
            ngx_memzero(&ngx_media_runtime_prepares[i],
                        sizeof(ngx_media_runtime_prepare_t));
            break;
        }
    }

    /*
     * A routed slot publishes into the stream it remembers, so it cannot
     * outlive it: the next frame from that publisher would be written into a
     * feed that is going away.  The slot is dropped, and the reassembly it
     * holds is reset first because the partial frame it is carrying is its
     * own reference - dropping the slot without that leaks the buffers.
     *
     * The publisher keeps sending into a route that no longer has a slot; its
     * frames are ignored until it opens again, which is the same thing that
     * happens to any transport whose stream was deleted.
     */
    for (i = 0; i < NGX_MEDIA_RUNTIME_MAX_ROUTED; i++) {

        if (ngx_media_runtime_routed[i].used
            && ngx_media_runtime_routed[i].stream == stream)
        {
            ngx_media_ipc_frame_reset(&ngx_media_runtime_routed[i].frame);
            ngx_memzero(&ngx_media_runtime_routed[i],
                        sizeof(ngx_media_runtime_routed_t));
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

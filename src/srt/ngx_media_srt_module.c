/*
 * nginx-media SRT ingest module (goal doc 11.2, 11.3).
 *
 * Static configuration for now is a single listener capability:
 *
 *     media_srt_listen 127.0.0.1:9000;
 *
 * Worker 0 owns the listener (goal doc 22).  The transport helper thread runs
 * one shared poll over the listener and every session, and hands compact
 * events plus raw MPEG-TS chunks (tagged with their session) to the worker
 * through an eventfd.  The worker parses and validates the Stream ID,
 * registers the publisher as a source of a logical stream, demuxes its
 * MPEG-TS into encoded frames and publishes them through the activation gate
 * into the program feed.
 */

#include "ngx_media_platform.h"
#include "ngx_media_registry.h"
#include "ngx_media_route.h"
#include "ngx_media_runtime.h"
#include "ngx_media_srt_output.h"

/*
 * The backends media_srt_backend can select.  Both are weak: a build may link
 * either, both, or neither, and the directive reports which ones it carries.
 */
extern ngx_media_srt_ops_t  ngx_media_srt_haivision_ops
    __attribute__((weak));
extern ngx_media_srt_ops_t  ngx_media_srt_udp_ops
    __attribute__((weak));
#include "ngx_media_selector.h"
#include "ngx_media_srt_ingest.h"
#include "ngx_media_ts_demux.h"

#include <ngx_event.h>

#define NGX_MEDIA_SRT_MAX_PRIORITIES 16

typedef struct {
    ngx_str_t   id;        /* publisher identity from the stream id */
    ngx_uint_t  priority;  /* trusted operator configuration */
} ngx_media_srt_priority_t;

typedef struct {
    ngx_str_t                listen;
    unsigned                 listen_set:1;
    ngx_media_srt_priority_t priorities[NGX_MEDIA_SRT_MAX_PRIORITIES];
    ngx_uint_t               npriorities;

    /* SRT destinations fed from the shared program preparation */
    ngx_media_srt_output_conf_t  outputs[NGX_MEDIA_SRT_MAX_OUTPUTS];
    ngx_uint_t                   noutputs;
} ngx_media_srt_main_conf_t;

typedef struct {
    uint64_t               id;
    ngx_uint_t             used;
    ngx_media_stream_t    *stream;
    ngx_media_source_t    *source;

    /* set when this worker does not own the stream and routes to the owner */
    unsigned               routed:1;
    uint32_t               hash;
    uint64_t               routed_sequence;
    ngx_media_ts_demux_t   demux;
    ngx_uint_t             demux_ready;
    uint64_t               frames_video;
    uint64_t               frames_audio;
    uint64_t               keyframes;
    uint64_t               config_frames;
} ngx_media_srt_slot_t;

static void *ngx_media_srt_create_conf(ngx_cycle_t *cycle);
static char *ngx_media_srt_listen_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_media_srt_init_process(ngx_cycle_t *cycle);
static void ngx_media_srt_exit_process(ngx_cycle_t *cycle);
static void ngx_media_srt_handler(ngx_event_t *ev);
static ngx_int_t ngx_media_srt_parse_endpoint(ngx_pool_t *pool,
    const ngx_str_t *endpoint, ngx_str_t *host, ngx_uint_t *port);

static char *ngx_media_srt_priority_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_srt_output_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_srt_backend_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void ngx_media_srt_output_sink(void *ctx, ngx_media_stream_t *stream,
    ngx_media_buf_t *burst, size_t len, ngx_uint_t keyframe);
static void ngx_media_srt_output_handler(ngx_event_t *ev);
static ngx_uint_t ngx_media_srt_priority(const ngx_str_t *id);
static void ngx_media_srt_stream_policy(ngx_media_stream_t *stream);
static uint64_t ngx_media_srt_demux_errors(
    const ngx_media_ts_demux_stats_t *stats);
static void ngx_media_srt_sink_frame(void *ctx,
    const ngx_media_frame_t *frame);
static void ngx_media_srt_sink_tracks(void *ctx,
    const ngx_media_trackset_t *tracks);
static void ngx_media_srt_slot_open(ngx_log_t *log, uint64_t session_id,
    ngx_media_srt_streamid_t *id);
static void ngx_media_srt_slot_close(ngx_log_t *log, uint64_t session_id);
static ngx_media_srt_slot_t *ngx_media_srt_slot_find(uint64_t id);
static ngx_media_srt_slot_t *ngx_media_srt_slot_alloc(void);

static ngx_media_srt_ingest_t   ngx_media_srt_ingest;
static ngx_connection_t        *ngx_media_srt_connection;
static ngx_uint_t               ngx_media_srt_started;
static uint64_t                 ngx_media_srt_drained_bytes;
static uint64_t                 ngx_media_srt_drained_chunks;
static ngx_msec_t               ngx_media_srt_last_summary;

static ngx_media_srt_slot_t  ngx_media_srt_slots[
    NGX_MEDIA_SRT_MAX_SESSIONS];

/* SRT destinations fed from the shared program preparation */
static ngx_media_srt_outputs_t  *ngx_media_srt_outputs;
static ngx_connection_t         *ngx_media_srt_output_connection;

static ngx_media_feed_conf_t    ngx_media_srt_feed_conf = {
    2048, 32 * 1024 * 1024, 10000
};

static ngx_command_t ngx_media_srt_commands[] = {

    { ngx_string("media_srt_listen"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_srt_listen_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_source_priority"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE2,
      ngx_media_srt_priority_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_backend"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_srt_backend_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_output"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE2|NGX_CONF_TAKE3,
      ngx_media_srt_output_cmd,
      0,
      0,
      NULL },

      ngx_null_command
};

static ngx_core_module_t ngx_media_srt_module_ctx = {
    ngx_string("media_srt"),
    ngx_media_srt_create_conf,
    NULL
};

ngx_module_t ngx_media_srt_module = {
    NGX_MODULE_V1,
    &ngx_media_srt_module_ctx,     /* module context */
    ngx_media_srt_commands,        /* module directives */
    NGX_CORE_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    ngx_media_srt_init_process,    /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    ngx_media_srt_exit_process,    /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_media_srt_slot_t *
ngx_media_srt_slot_find(uint64_t id)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
        if (ngx_media_srt_slots[i].used
            && ngx_media_srt_slots[i].id == id)
        {
            return &ngx_media_srt_slots[i];
        }
    }

    return NULL;
}

static ngx_media_srt_slot_t *
ngx_media_srt_slot_alloc(void)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
        if (!ngx_media_srt_slots[i].used) {
            ngx_memzero(&ngx_media_srt_slots[i],
                        sizeof(ngx_media_srt_slot_t));
            ngx_media_srt_slots[i].used = 1;

            return &ngx_media_srt_slots[i];
        }
    }

    return NULL;
}

static void
ngx_media_srt_sink_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_srt_slot_t  *session = ctx;

    if (session == NULL) {
        return;
    }

    if (session->routed) {
        /* the program lives on another worker: hand the frame over once */
        (void) ngx_media_route_frame((ngx_cycle_t *) ngx_cycle, session->hash,
                                     frame, session->routed_sequence);
    }

    if (session->stream != NULL && session->source != NULL) {
        ngx_media_health_media(&session->source->health, frame->dts,
                               ngx_current_msec);

        /* ISO tap: this source before the selector */
        ngx_media_runtime_iso_source(session->stream, session->source, frame);

        (void) ngx_media_stream_publish(session->stream, session->source,
                                        frame, ngx_current_msec);
    }

    if (session->routed) {
        session->routed_sequence++;
    }

    if (frame->config) {
        session->config_frames++;
        return;
    }

    if (frame->media_type == NGX_MEDIA_TYPE_VIDEO) {
        session->frames_video++;

        if (frame->keyframe) {
            session->keyframes++;
        }

    } else if (frame->media_type == NGX_MEDIA_TYPE_AUDIO) {
        session->frames_audio++;
    }
}

static void
ngx_media_srt_sink_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_media_srt_slot_t  *session = ctx;
    ngx_uint_t                i;

    if (session != NULL && session->routed) {
        (void) ngx_media_route_tracks((ngx_cycle_t *) ngx_cycle, session->hash,
                                      tracks);
    }

    if (session != NULL && session->source != NULL) {
        (void) ngx_media_source_tracks_set(session->source, tracks,
                                           ngx_cycle->log);
    }

    for (i = 0; i < tracks->count; i++) {

        if (tracks->tracks[i].media_type == NGX_MEDIA_TYPE_VIDEO
            && session != NULL && session->source != NULL)
        {
            /* the standby cache opens at a video keyframe only */
            session->source->has_video = 1;
        }

        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                      "media: srt track %ui media=%ui codec=%ui format=%ui "
                      "rate=%ui channels=%ui config=%s",
                      i,
                      tracks->tracks[i].media_type,
                      tracks->tracks[i].codec,
                      tracks->tracks[i].payload_format,
                      tracks->tracks[i].sample_rate,
                      tracks->tracks[i].channels,
                      tracks->tracks[i].config != NULL ? "yes" : "no");
    }
}

static void
ngx_media_srt_demux_start(ngx_media_srt_slot_t *session, ngx_log_t *log)
{
    ngx_media_ts_demux_conf_t  conf;
    ngx_media_ts_sink_t        sink;

    if (session->demux_ready) {
        ngx_media_ts_demux_destroy(&session->demux);
        session->demux_ready = 0;
    }

    conf.max_tracks = 8;
    conf.max_au_bytes = 1024 * 1024;

    sink.tracks = ngx_media_srt_sink_tracks;
    sink.frame = ngx_media_srt_sink_frame;

    if (ngx_media_ts_demux_init(&session->demux, &conf, &sink, session, log)
        != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: could not start the TS demuxer");
        return;
    }

    session->demux_ready = 1;
}

static void
ngx_media_srt_slot_open(ngx_log_t *log, uint64_t session_id,
    ngx_media_srt_streamid_t *id)
{
    ngx_media_registry_t     *registry;
    ngx_media_source_t       *source;
    ngx_media_stream_t       *stream;
    ngx_media_srt_slot_t  *session;

    session = ngx_media_srt_slot_alloc();

    if (session == NULL) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: no free ingest session slot for session=%uL",
                      session_id);
        return;
    }

    session->id = session_id;

    /*
     * The program lives on exactly one worker.  When the publisher lands
     * elsewhere, the frames are forwarded over the bounded inter-worker
     * transport instead of being registered here (goal doc 22, 23).
     */
    session->hash = ngx_media_owner_hash(&id->application, &id->stream);

    if (!ngx_media_route_is_owner((ngx_cycle_t *) ngx_cycle, session->hash)) {
        session->routed = 1;

        if (ngx_media_route_open((ngx_cycle_t *) ngx_cycle, session->hash,
                                 &id->application, &id->stream, &id->source,
                                 NGX_MEDIA_SOURCE_SRT,
                                 ngx_media_srt_priority(&id->source))
            != NGX_OK)
        {
            ngx_log_error(NGX_LOG_WARN, log, 0,
                          "media: could not route %V/%V to its owner worker",
                          &id->application, &id->stream);
            session->routed = 0;
            ngx_memzero(session, sizeof(ngx_media_srt_slot_t));
            return;
        }

        ngx_media_srt_demux_start(session, log);

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: srt publisher routed to the owner stream=%V/%V "
                      "source=%V", &id->application, &id->stream, &id->source);

        return;
    }

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: no stream registry in this worker");
        return;
    }

    stream = ngx_media_registry_stream_create(registry, &id->application,
                                              &id->stream,
                                              &ngx_media_srt_feed_conf, log);

    if (stream == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: could not create stream %V/%V",
                      &id->application, &id->stream);
        return;
    }

    /* a reconnect of the same identity replaces the previous incarnation */
    source = ngx_media_stream_source_find(stream, &id->source);

    if (source != NULL) {
        ngx_media_stream_source_remove(stream, source);
        source = NULL;
    }

    /* priority comes from trusted configuration, never from the encoder */
    source = ngx_media_stream_source_add(stream, &id->source,
                                         NGX_MEDIA_SOURCE_SRT,
                                         ngx_media_srt_priority(&id->source),
                                         log);

    if (source == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: could not register source %V for %V/%V",
                      &id->source, &id->application, &id->stream);
        return;
    }

    ngx_media_srt_stream_policy(stream);

    /* publish ownership so other workers can route to us */
    {
        ngx_media_owner_dir_t  *dir = ngx_media_runtime_owner_dir();

        if (dir != NULL) {
            (void) ngx_media_owner_dir_claim(dir, session->hash,
                                             (ngx_uint_t) ngx_process_slot);
        }
    }

    session->stream = stream;
    session->source = source;

    ngx_media_health_init(&source->health, &stream->selector,
                          ngx_current_msec);
    ngx_media_health_transport(&source->health, 1, ngx_current_msec);

    /*
     * Bootstrap: the first source of a stream is promoted through the normal
     * safe path and becomes active at its first keyframe.
     */
    if (stream->active == NULL) {
        (void) ngx_media_stream_promote(stream, source);
    }

    ngx_media_srt_demux_start(session, log);

    if (stream->active != NULL) {
        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: srt program stream=%V/%V sources=%ui active=%V",
                      &stream->application, &stream->name,
                      ngx_media_stream_source_count(stream),
                      &stream->active->id);

    } else {
        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: srt program stream=%V/%V sources=%ui "
                      "active=none",
                      &stream->application, &stream->name,
                      ngx_media_stream_source_count(stream));
    }
}

static void
ngx_media_srt_slot_close(ngx_log_t *log, uint64_t session_id)
{
    ngx_media_ts_demux_stats_t  stats;
    ngx_media_srt_slot_t    *session;
    ngx_media_stream_t         *stream;

    session = ngx_media_srt_slot_find(session_id);

    if (session == NULL) {
        return;
    }

    if (session->demux_ready) {
        ngx_media_ts_demux_flush(&session->demux);
        ngx_media_ts_demux_stats(&session->demux, &stats);

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: srt demux video=%uL audio=%uL keyframes=%uL "
                      "config=%uL pcr=%uL last_pcr=%L sync_errors=%uL "
                      "continuity_errors=%uL psi_errors=%uL crc_errors=%uL "
                      "pes_errors=%uL au_overflows=%uL unsupported=%uL",
                      session->frames_video,
                      session->frames_audio,
                      session->keyframes,
                      session->config_frames,
                      (uint64_t) stats.has_pcr,
                      (int64_t) stats.last_pcr,
                      stats.sync_errors,
                      stats.continuity_errors,
                      stats.psi_errors,
                      stats.crc_errors,
                      stats.pes_errors,
                      stats.au_overflows,
                      stats.unsupported_streams);

        ngx_media_ts_demux_destroy(&session->demux);
        session->demux_ready = 0;
    }

    if (session->routed) {
        (void) ngx_media_route_close((ngx_cycle_t *) ngx_cycle, session->hash);

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: routed srt publisher closed hash=%uL frames=%uL",
                      session->hash, session->routed_sequence);

        ngx_memzero(session, sizeof(ngx_media_srt_slot_t));
        return;
    }

    stream = session->stream;

    if (session->source != NULL) {
        ngx_media_health_transport(&session->source->health, 0,
                                   ngx_current_msec);
    }

    if (stream != NULL && session->source != NULL) {
        ngx_media_stream_source_remove(stream, session->source);
        session->source = NULL;
    }

    if (stream != NULL) {

        if (stream->active != NULL) {
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "media: srt program stream=%V/%V frames=%uL "
                          "generation=%ui switches=%uL sources=%ui active=%V",
                          &stream->application, &stream->name,
                          stream->program_frames, stream->generation,
                          stream->switches,
                          ngx_media_stream_source_count(stream),
                          &stream->active->id);

        } else {
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "media: srt program stream=%V/%V frames=%uL "
                          "generation=%ui switches=%uL sources=%ui "
                          "active=none",
                          &stream->application, &stream->name,
                          stream->program_frames, stream->generation,
                          stream->switches,
                          ngx_media_stream_source_count(stream));
        }
    }

    ngx_memzero(session, sizeof(ngx_media_srt_slot_t));
}

extern ngx_module_t  ngx_media_core_module;

/*
 * media_srt_backend haivision|udp
 *
 * The transport backend is a build-time-pluggable implementation of the same
 * contract (goal doc 11.1).  "srt" is the SRT library backend: Haivision/srt
 * is the reference library, and robotweax/srt is a clean-slate implementation
 * of the same C API that the build can link instead without any code change.
 * "udp" is the test double used to qualify that boundary; it is not an SRT
 * implementation and is not built by default.
 */
static char *
ngx_media_srt_backend_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t  *value = cf->args->elts;

    (void) cmd;
    (void) conf;

    if (value[1].len == sizeof("srt") - 1
        && ngx_strncmp(value[1].data, "srt", sizeof("srt") - 1) == 0)
    {
        if (&ngx_media_srt_haivision_ops == NULL) {
            return "this build has no SRT library backend"
                   " (build with MEDIA_SRT_BACKEND=srt or both)";
        }

        ngx_media_srt_set_backend(&ngx_media_srt_haivision_ops);
        return NGX_CONF_OK;
    }

    if (value[1].len == sizeof("udp") - 1
        && ngx_strncmp(value[1].data, "udp", sizeof("udp") - 1) == 0)
    {
        if (&ngx_media_srt_udp_ops == NULL) {
            return "this build has no UDP test double"
                   " (build with MEDIA_SRT_BACKEND=udp or both)";
        }

        ngx_media_srt_set_backend(&ngx_media_srt_udp_ops);
        return NGX_CONF_OK;
    }

    return "backend must be srt (the SRT library) or udp (test double)";
}

/*
 * media_srt_output <application/stream> <host:port> [streamid]
 *
 * The destination consumes the transport bursts the program runtime already
 * prepares for HLS and recording, so SRT output adds no second preparation
 * and no per-receiver copy (goal doc 16, 34 item 11).
 */
static char *
ngx_media_srt_output_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_srt_main_conf_t    *mcf = conf;
    ngx_media_srt_output_conf_t  *out;
    ngx_str_t                    *value = cf->args->elts;
    u_char                       *colon;
    ngx_str_t                     resource;
    ngx_int_t                     port;

    (void) cmd;

    if (mcf->noutputs >= NGX_MEDIA_SRT_MAX_OUTPUTS) {
        return "too many media_srt_output destinations";
    }

    resource = value[1];

    if (resource.len == 0) {
        return "destination stream must not be empty";
    }

    out = &mcf->outputs[mcf->noutputs];

    ngx_memzero(out, sizeof(ngx_media_srt_output_conf_t));

    colon = ngx_strlchr(resource.data, resource.data + resource.len, '/');

    if (colon == NULL) {
        return "destination stream must be application/stream";
    }

    out->application.data = resource.data;
    out->application.len = (size_t) (colon - resource.data);
    out->stream.data = colon + 1;
    out->stream.len = resource.len - out->application.len - 1;

    if (out->application.len == 0 || out->stream.len == 0) {
        return "destination stream must be application/stream";
    }

    colon = ngx_strlchr(value[2].data, value[2].data + value[2].len, ':');

    if (colon == NULL) {
        return "destination address must be host:port";
    }

    out->host.len = (size_t) (colon - value[2].data);

    port = ngx_atoi(colon + 1, value[2].len - out->host.len - 1);

    if (out->host.len == 0 || port < 1 || port > 65535) {
        return "destination address must be host:port";
    }

    /*
     * The transport layer takes a C string, and the configuration value is a
     * slice of "host:port": keep a terminated copy of the host.
     */
    out->host.data = ngx_pnalloc(cf->pool, out->host.len + 1);

    if (out->host.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(out->host.data, value[2].data, out->host.len);
    out->host.data[out->host.len] = '\0';

    out->port = (ngx_uint_t) port;

    if (cf->args->nelts == 4) {

        if (value[3].len == 0
            || value[3].len > NGX_MEDIA_SRT_STREAMID_MAX)
        {
            return "stream id is too long";
        }

        out->streamid = value[3];
    }

    out->max_units = 256;
    out->max_bytes = 8 * 1024 * 1024;
    /* short timeouts keep shutdown and reconnect latency bounded */
    out->connect_timeout = 2000;
    out->send_timeout = 1000;

    mcf->noutputs++;

    return NGX_CONF_OK;
}

/* the program runtime hands every prepared burst here */
static void
ngx_media_srt_output_sink(void *ctx, ngx_media_stream_t *stream,
    ngx_media_buf_t *burst, size_t len, ngx_uint_t keyframe)
{
    ngx_media_srt_outputs_t  *outs = ctx;

    if (outs == NULL || stream == NULL) {
        return;
    }

    (void) ngx_media_srt_outputs_push(outs, &stream->application,
                                      &stream->name, burst, len, keyframe);
}

/* destination status changes arrive on their own eventfd */
static void
ngx_media_srt_output_handler(ngx_event_t *ev)
{
    ngx_connection_t           *c = ev->data;
    ngx_media_srt_outputs_t    *outs = c->data;
    ngx_media_srt_out_event_t   events[8];
    ngx_uint_t                  n, i;
    u_char                      evbuf[8];
    ssize_t                     r;

    r = read(outs != NULL ? ngx_media_srt_outputs_notify_fd(outs) : -1, evbuf,
             sizeof(evbuf));
    (void) r;

    if (outs == NULL) {
        return;
    }

    for ( ;; ) {
        n = ngx_media_srt_outputs_event_read(outs, events, 8);

        if (n == 0) {
            break;
        }

        for (i = 0; i < n; i++) {

            if (events[i].type == NGX_MEDIA_SRT_OUT_EVENT_CONNECTED) {
                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt output %ui connected (bursts=%uL "
                              "bytes=%uL dropped=%uL)",
                              events[i].index, events[i].sent_bursts,
                              events[i].sent_bytes, events[i].dropped);

            } else {
                ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                              "media: srt output %ui not connected "
                              "(attempts=%uL dropped=%uL)",
                              events[i].index, events[i].reconnects,
                              events[i].dropped);
            }
        }
    }
}

static void
ngx_media_srt_stream_policy(ngx_media_stream_t *stream)
{
    ngx_media_policy_t  *policy;

    policy = (ngx_media_policy_t *)
                 ((ngx_cycle_t *) ngx_cycle)
                     ->conf_ctx[ngx_media_core_module.index];

    if (policy == NULL) {
        policy = ngx_media_policy_get((ngx_cycle_t *) ngx_cycle);
    }

    if (policy != NULL) {
        ngx_media_stream_set_policy(stream, policy);
    }
}

static uint64_t
ngx_media_srt_demux_errors(const ngx_media_ts_demux_stats_t *stats)
{
    return stats->sync_errors + stats->transport_errors
           + stats->continuity_errors + stats->psi_errors
           + stats->crc_errors + stats->pes_errors;
}

static ngx_uint_t
ngx_media_srt_priority(const ngx_str_t *id)
{
    ngx_media_srt_main_conf_t  *mcf;
    ngx_uint_t                  i;

    mcf = (ngx_media_srt_main_conf_t *)
              ((ngx_cycle_t *) ngx_cycle)
                  ->conf_ctx[ngx_media_srt_module.index];

    if (mcf == NULL) {
        return 0;
    }

    for (i = 0; i < mcf->npriorities; i++) {
        if (mcf->priorities[i].id.len == id->len
            && ngx_memcmp(mcf->priorities[i].id.data, id->data, id->len) == 0)
        {
            return mcf->priorities[i].priority;
        }
    }

    return 0;
}

static char *
ngx_media_srt_priority_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_srt_main_conf_t  *mcf = conf;
    ngx_str_t                  *value;
    ngx_int_t                   priority;

    (void) cmd;

    value = cf->args->elts;

    if (mcf->npriorities >= NGX_MEDIA_SRT_MAX_PRIORITIES) {
        return "too many media_srt_source_priority directives";
    }

    priority = ngx_atoi(value[2].data, value[2].len);
    if (priority == NGX_ERROR || priority < 0 || priority > 65535) {
        return "invalid priority";
    }

    mcf->priorities[mcf->npriorities].id = value[1];
    mcf->priorities[mcf->npriorities].priority = (ngx_uint_t) priority;
    mcf->npriorities++;

    return NGX_CONF_OK;
}

static void *
ngx_media_srt_create_conf(ngx_cycle_t *cycle)
{
    ngx_media_srt_main_conf_t  *mcf;

    mcf = ngx_pcalloc(cycle->pool, sizeof(ngx_media_srt_main_conf_t));
    if (mcf == NULL) {
        return NULL;
    }

    return mcf;
}

static char *
ngx_media_srt_listen_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_srt_main_conf_t  *mcf = conf;
    ngx_str_t                  *value;

    (void) cmd;

    if (mcf->listen_set) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (value[1].len == 0) {
        return NGX_CONF_ERROR;
    }

    mcf->listen = value[1];
    mcf->listen_set = 1;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_media_srt_parse_endpoint(ngx_pool_t *pool, const ngx_str_t *endpoint,
    ngx_str_t *host, ngx_uint_t *port)
{
    u_char     *colon, *last;
    ngx_int_t   value;

    last = endpoint->data + endpoint->len;

    colon = ngx_strlchr(endpoint->data, last, ':');

    if (colon == NULL || colon == endpoint->data || colon + 1 == last) {
        return NGX_ERROR;
    }

    value = ngx_atoi(colon + 1, last - colon - 1);
    if (value < 1 || value > 65535) {
        return NGX_ERROR;
    }

    host->len = colon - endpoint->data;
    host->data = ngx_pnalloc(pool, host->len);
    if (host->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(host->data, endpoint->data, host->len);

    *port = (ngx_uint_t) value;

    return NGX_OK;
}

static ngx_int_t
ngx_media_srt_init_process(ngx_cycle_t *cycle)
{
    ngx_media_srt_main_conf_t   *mcf;
    ngx_media_srt_ingest_conf_t  conf;
    ngx_connection_t            *c;
    ngx_str_t                    host;
    ngx_uint_t                   port;

    mcf = (ngx_media_srt_main_conf_t *)
              cycle->conf_ctx[ngx_media_srt_module.index];

    /*
     * Which transport implementation is in use, and which library provides
     * it.  Haivision/srt and robotweax/srt are indistinguishable in code, so
     * this line is how a deployment knows what it linked.
     */
    if (ngx_media_srt_backend() != NULL) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "media: srt transport backend=%s library=%s",
                      ngx_media_srt_backend()->name,
                      ngx_media_srt_backend()->library_version != NULL
                          ? ngx_media_srt_backend()->library_version()
                          : "unknown");
    }

    /*
     * Every worker adopts the shared owner directory and its routing
     * endpoints, and arms the program runtime: a worker that does not accept
     * publishers still owns the programs its hash selects.
     */
    if (ngx_media_runtime_init(cycle, cycle->log) != NGX_OK) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: could not initialise the program runtime");
        return NGX_ERROR;
    }

    (void) ngx_media_runtime_arm(cycle, cycle->log);

    if (mcf == NULL || !mcf->listen_set) {
        return NGX_OK;
    }

    /* transport sockets stay with worker 0 (goal doc 22) */
    if (ngx_process_slot != 0) {
        return NGX_OK;
    }

    if (ngx_media_srt_parse_endpoint(cycle->pool, &mcf->listen, &host, &port)
        != NGX_OK)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: invalid media_srt_listen \"%V\"", &mcf->listen);
        return NGX_ERROR;
    }

    ngx_memzero(&conf, sizeof(conf));

    conf.host = host;
    conf.port = port;
    conf.max_chunks = 256;
    conf.max_bytes = 8 * 1024 * 1024;
    conf.max_events = 64;
    conf.max_sessions = NGX_MEDIA_SRT_MAX_SESSIONS;

    if (ngx_media_srt_ingest_start(&ngx_media_srt_ingest, &conf, cycle->log)
        != NGX_OK)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: could not start the SRT ingest runtime");
        return NGX_ERROR;
    }

    c = ngx_get_connection(ngx_media_srt_ingest.notify_fd, cycle->log);
    if (c == NULL) {
        ngx_media_srt_ingest_stop(&ngx_media_srt_ingest);
        return NGX_ERROR;
    }

    c->data = &ngx_media_srt_ingest;
    c->read->handler = ngx_media_srt_handler;
    c->read->log = cycle->log;

    if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
        ngx_free_connection(c);
        ngx_media_srt_ingest_stop(&ngx_media_srt_ingest);
        return NGX_ERROR;
    }

    ngx_media_srt_connection = c;
    ngx_media_srt_started = 1;

    /* SRT destinations consume the shared preparation of the program runtime */
    if (mcf->noutputs > 0) {
        ngx_media_srt_outputs_t  *outs;

        if (ngx_media_srt_outputs_start(&outs, mcf->outputs, mcf->noutputs,
                                        NGX_MEDIA_SRT_OUT_MAX_EVENTS,
                                        cycle->log) != NGX_OK)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "media: could not start the SRT outputs");
            return NGX_ERROR;
        }

        ngx_media_srt_outputs = outs;

        c = ngx_get_connection(ngx_media_srt_outputs_notify_fd(outs),
                               cycle->log);

        if (c != NULL) {
            c->data = outs;
            c->read->handler = ngx_media_srt_output_handler;
            c->read->log = cycle->log;

            if (ngx_add_event(c->read, NGX_READ_EVENT, 0) == NGX_OK) {
                ngx_media_srt_output_connection = c;

            } else {
                ngx_free_connection(c);
            }
        }

        ngx_media_runtime_set_sink(ngx_media_srt_output_sink, outs);

        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "media: %ui SRT destination(s) started", mcf->noutputs);
    }


    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  "media: SRT ingest runtime started for %V", &mcf->listen);

    return NGX_OK;
}

static void
ngx_media_srt_handler(ngx_event_t *ev)
{
    ngx_connection_t            *c = ev->data;
    ngx_media_srt_ingest_t      *ingest = c->data;
    ngx_media_srt_event_t        events[16];
    ngx_media_ts_ingest_chunk_t  chunks[16];
    ngx_media_ts_ingest_stats_t  stats;
    ngx_media_ts_demux_stats_t   demux_stats;
    ngx_media_srt_streamid_t     id;
    ngx_media_srt_slot_t     *session;
    ngx_uint_t                   n, i, count;
    ngx_uint_t                   force_summary;
    uint64_t                     bytes;
    u_char                       evbuf[8];
    ssize_t                      r;

    r = read(ingest->notify_fd, evbuf, sizeof(evbuf));
    (void) r;

    force_summary = 0;

    for ( ;; ) {
        n = ngx_media_srt_ingest_event_read(ingest, events, 16);

        if (n == 0) {
            break;
        }

        for (i = 0; i < n; i++) {

            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                           "media: srt event type=%ui session=%uL bytes=%uL",
                           events[i].type, events[i].session_id,
                           events[i].bytes);

            switch (events[i].type) {

            case NGX_MEDIA_SRT_EVENT_READY:
                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt listener ready on %V:%ui",
                              &ingest->conf.host, ingest->conf.port);
                break;

            case NGX_MEDIA_SRT_EVENT_FAILED:
                ngx_log_error(NGX_LOG_EMERG, ev->log, 0,
                              "media: srt listener failed on %V:%ui",
                              &ingest->conf.host, ingest->conf.port);
                break;

            case NGX_MEDIA_SRT_EVENT_OPEN:

                if (ngx_media_srt_streamid_parse(events[i].streamid,
                                                 events[i].streamid_len, &id)
                    != NGX_OK)
                {
                    ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                                  "media: srt publisher rejected session=%uL: "
                                  "malformed stream id", events[i].session_id);
                    break;
                }

                if (id.mode_kind != NGX_MEDIA_SRT_MODE_PUBLISH) {
                    ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                                  "media: srt publisher rejected session=%uL: "
                                  "mode is not publish", events[i].session_id);
                    break;
                }

                if (id.source.len == 0) {
                    ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                                  "media: srt publisher rejected session=%uL: "
                                  "no source identity in the stream id",
                                  events[i].session_id);
                    break;
                }

                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt source open app=%V stream=%V "
                              "source=%V session=%uL",
                              &id.application, &id.stream, &id.source,
                              events[i].session_id);

                ngx_media_srt_slot_open(ev->log, events[i].session_id, &id);
                break;

            case NGX_MEDIA_SRT_EVENT_CLOSE:
                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt source close session=%uL bytes=%uL "
                              "chunks=%uL",
                              events[i].session_id, events[i].bytes,
                              events[i].chunks);

                ngx_media_srt_slot_close(ev->log, events[i].session_id);

                force_summary = 1;
                break;

            default:
                break;
            }
        }
    }

    bytes = 0;

    for ( ;; ) {
        count = ngx_media_ts_ingest_read(&ingest->payload, chunks, 16);

        if (count == 0) {
            break;
        }

        for (i = 0; i < count; i++) {
            bytes += chunks[i].len;

            if (ngx_media_runtime_raw_ready()) {
                /* RAW tap: transport bytes before normalization */
                (void) ngx_media_runtime_raw_bytes(chunks[i].data,
                                                   chunks[i].len);
            }

            session = ngx_media_srt_slot_find(chunks[i].session_id);

            if (session != NULL && session->demux_ready) {
                (void) ngx_media_ts_demux_feed(&session->demux, chunks[i].data,
                                               chunks[i].len);
            }
        }

        ngx_media_ts_ingest_release(chunks, count);

        ngx_media_srt_drained_chunks += count;
    }

    if (bytes > 0) {
        ngx_media_srt_drained_bytes += bytes;

        for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
            session = &ngx_media_srt_slots[i];

            if (!session->used || !session->demux_ready
                || session->source == NULL)
            {
                continue;
            }

            ngx_media_ts_demux_stats(&session->demux, &demux_stats);

            ngx_media_health_container(&session->source->health,
                                       ngx_media_srt_demux_errors(&demux_stats),
                                       ngx_current_msec);
        }
    }

    if (bytes > 0 || force_summary) {

        if (force_summary
            || ngx_current_msec - ngx_media_srt_last_summary >= 1000)
        {
            ngx_media_ts_ingest_stats(&ingest->payload, &stats);

            ngx_log_error(NGX_LOG_INFO, ev->log, 0,
                          "media: srt ingest drained bytes=%uL chunks=%uL "
                          "dropped_chunks=%uL sessions=%uL",
                          ngx_media_srt_drained_bytes,
                          ngx_media_srt_drained_chunks,
                          stats.chunks_dropped,
                          (uint64_t) ingest->sessions_accepted);

            ngx_media_srt_last_summary = ngx_current_msec;
        }
    }
}

static void
ngx_media_srt_exit_process(ngx_cycle_t *cycle)
{
    ngx_uint_t  i;

    (void) cycle;

    if (!ngx_media_srt_started) {
        return;
    }

    ngx_media_srt_started = 0;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
        if (ngx_media_srt_slots[i].demux_ready) {
            ngx_media_ts_demux_destroy(&ngx_media_srt_slots[i].demux);
            ngx_media_srt_slots[i].demux_ready = 0;
        }
    }

    if (ngx_media_srt_output_connection != NULL) {
        (void) ngx_del_event(ngx_media_srt_output_connection->read,
                             NGX_READ_EVENT, 0);
        ngx_media_srt_output_connection->fd = (ngx_socket_t) -1;
        ngx_free_connection(ngx_media_srt_output_connection);
        ngx_media_srt_output_connection = NULL;
    }

    if (ngx_media_srt_outputs != NULL) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "media: stopping SRT destinations");
        ngx_media_runtime_set_sink(NULL, NULL);
        ngx_media_srt_outputs_stop(ngx_media_srt_outputs);
        ngx_media_srt_outputs = NULL;
    }

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "media: stopping program runtime");
    ngx_media_runtime_shutdown(cycle->log);
    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "media: program runtime stopped");

    if (ngx_media_srt_connection != NULL) {
        (void) ngx_del_event(ngx_media_srt_connection->read, NGX_READ_EVENT, 0);
        ngx_media_srt_connection->fd = (ngx_socket_t) -1;
        ngx_free_connection(ngx_media_srt_connection);
        ngx_media_srt_connection = NULL;
    }

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "media: stopping SRT ingest");
    ngx_media_srt_ingest_stop(&ngx_media_srt_ingest);
    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "media: SRT ingest stopped");
    ngx_media_srt_shutdown();
}

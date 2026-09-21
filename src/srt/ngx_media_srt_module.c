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
} ngx_media_srt_main_conf_t;

typedef struct {
    uint64_t               id;
    ngx_uint_t             used;
    ngx_media_stream_t    *stream;
    ngx_media_source_t    *source;
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
static void ngx_media_srt_timer_handler(ngx_event_t *ev);
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

/* selection ticks: failover and switchback decisions */
#define NGX_MEDIA_SRT_SELECTOR_INTERVAL 100

static ngx_event_t              ngx_media_srt_selector_timer;
static ngx_uint_t               ngx_media_srt_selector_armed;
static ngx_msec_t               ngx_media_srt_last_idle_log;

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

    if (session->stream != NULL && session->source != NULL) {
        ngx_media_health_media(&session->source->health, frame->dts,
                               ngx_current_msec);

        (void) ngx_media_stream_publish(session->stream, session->source,
                                        frame, ngx_current_msec);
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

static ngx_str_t  ngx_media_srt_none = ngx_string("none");

extern ngx_module_t  ngx_media_core_module;

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

static void
ngx_media_srt_timer_handler(ngx_event_t *ev)
{
    ngx_media_registry_t        *registry;
    ngx_media_registry_entry_t  *entry;
    ngx_media_selector_result_t  res;
    ngx_media_stream_t          *stream;
    ngx_queue_t                 *q;
    uint64_t                     before;
    ngx_msec_t                   now;

    now = ngx_current_msec;

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry != NULL) {

        for (q = ngx_queue_head(&registry->entries);
             q != (ngx_queue_t *) &registry->entries;
             q = q->next)
        {
            entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);
            stream = &entry->stream;

            before = stream->switches;

            (void) ngx_media_selector_run(stream, now, &res);

            if (now - ngx_media_srt_last_idle_log >= 1000) {
                ngx_log_debug6(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                               "media: selector tick stream=%V/%V active=%V "
                               "active_healthy=%ui best=%ui emergency=%ui",
                               &stream->application, &stream->name,
                               stream->active != NULL ? &stream->active->id
                                                      : &ngx_media_srt_none,
                               res.active_eligible, res.best != NULL,
                               res.emergency != NULL);
            }

            if (stream->switches != before) {
                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: selector switched stream=%V/%V "
                              "active=%V generation=%ui switches=%uL "
                              "emergency=%uL",
                              &stream->application, &stream->name,
                              stream->active != NULL ? &stream->active->id
                                                     : &ngx_media_srt_none,
                              stream->generation, stream->switches,
                              stream->emergency_switches);
            }

            if (stream->active != NULL && !res.active_eligible
                && res.best == NULL && res.emergency == NULL
                && now - ngx_media_srt_last_idle_log >= 1000)
            {
                ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                              "media: selector stream=%V/%V has no eligible "
                              "source; the program is idle on %V",
                              &stream->application, &stream->name,
                              &stream->active->id);

                ngx_media_srt_last_idle_log = now;
            }
        }
    }

    if (!ngx_exiting && ngx_media_srt_selector_armed) {
        ngx_add_timer(ev, NGX_MEDIA_SRT_SELECTOR_INTERVAL);
    }
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

    if (mcf == NULL || !mcf->listen_set) {
        return NGX_OK;
    }

    /* deterministic ownership: worker 0 owns the listener (goal doc 22) */
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

    ngx_memzero(&ngx_media_srt_selector_timer, sizeof(ngx_event_t));

    ngx_media_srt_selector_timer.handler = ngx_media_srt_timer_handler;
    ngx_media_srt_selector_timer.log = cycle->log;
    ngx_media_srt_selector_timer.data = cycle;
    ngx_media_srt_selector_armed = 1;

    ngx_add_timer(&ngx_media_srt_selector_timer,
                  NGX_MEDIA_SRT_SELECTOR_INTERVAL);

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

    ngx_media_srt_selector_armed = 0;

    if (ngx_media_srt_selector_timer.timer_set) {
        ngx_del_timer(&ngx_media_srt_selector_timer);
    }

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
        if (ngx_media_srt_slots[i].demux_ready) {
            ngx_media_ts_demux_destroy(&ngx_media_srt_slots[i].demux);
            ngx_media_srt_slots[i].demux_ready = 0;
        }
    }

    if (ngx_media_srt_connection != NULL) {
        (void) ngx_del_event(ngx_media_srt_connection->read, NGX_READ_EVENT, 0);
        ngx_free_connection(ngx_media_srt_connection);
        ngx_media_srt_connection->fd = (ngx_socket_t) -1;
        ngx_media_srt_connection = NULL;
    }

    ngx_media_srt_ingest_stop(&ngx_media_srt_ingest);
    ngx_media_srt_shutdown();
}

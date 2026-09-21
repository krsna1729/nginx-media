/*
 * nginx-media SRT ingest module (goal doc 11.2).
 *
 * Static configuration for now is a single listener capability:
 *
 *     media_srt_listen 127.0.0.1:9000;
 *
 * Worker 0 owns the listener (goal doc 22).  The transport helper thread
 * accepts publishers and hands compact events plus raw MPEG-TS chunks to the
 * worker through an eventfd; the worker parses and validates the Stream ID,
 * registers the publisher and drains the bounded payload queue.  Phase 2
 * attaches the TS demuxer to the drained bytes.
 */

#include "ngx_media_platform.h"
#include "ngx_media_srt_ingest.h"

#include <ngx_event.h>

typedef struct {
    ngx_str_t   listen;
    unsigned    listen_set:1;
} ngx_media_srt_main_conf_t;

static void *ngx_media_srt_create_conf(ngx_cycle_t *cycle);
static char *ngx_media_srt_listen_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_media_srt_init_process(ngx_cycle_t *cycle);
static void ngx_media_srt_exit_process(ngx_cycle_t *cycle);
static void ngx_media_srt_handler(ngx_event_t *ev);
static ngx_int_t ngx_media_srt_parse_endpoint(ngx_pool_t *pool,
    const ngx_str_t *endpoint, ngx_str_t *host, ngx_uint_t *port);

static ngx_media_srt_ingest_t   ngx_media_srt_ingest;
static ngx_connection_t        *ngx_media_srt_connection;
static ngx_uint_t               ngx_media_srt_started;
static uint64_t                 ngx_media_srt_drained_bytes;
static uint64_t                 ngx_media_srt_drained_chunks;
static ngx_msec_t               ngx_media_srt_last_summary;

static ngx_command_t ngx_media_srt_commands[] = {

    { ngx_string("media_srt_listen"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_srt_listen_cmd,
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
    u_char  *colon, *last;
    ngx_int_t value;

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
    ngx_media_srt_streamid_t     id;
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

                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt source open app=%V stream=%V "
                              "source=%V session=%uL",
                              &id.application, &id.stream, &id.source,
                              events[i].session_id);
                break;

            case NGX_MEDIA_SRT_EVENT_CLOSE:
                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt source close session=%uL bytes=%uL "
                              "chunks=%uL",
                              events[i].session_id, events[i].bytes,
                              events[i].chunks);
                force_summary = 1;
                break;

            default:
                break;
            }
        }
    }

    bytes = 0;

    for ( ;; ) {
        /* phase 2 attaches the TS demuxer to these chunks */
        count = ngx_media_ts_ingest_read(&ingest->payload, chunks, 16);

        if (count == 0) {
            break;
        }

        for (i = 0; i < count; i++) {
            bytes += chunks[i].len;
        }

        ngx_media_ts_ingest_release(chunks, count);

        ngx_media_srt_drained_chunks += count;
    }

    if (bytes > 0) {
        ngx_media_srt_drained_bytes += bytes;
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
    (void) cycle;

    if (!ngx_media_srt_started) {
        return;
    }

    ngx_media_srt_started = 0;

    if (ngx_media_srt_connection != NULL) {
        (void) ngx_del_event(ngx_media_srt_connection->read, NGX_READ_EVENT, 0);
        ngx_free_connection(ngx_media_srt_connection);
        ngx_media_srt_connection->fd = (ngx_socket_t) -1;
        ngx_media_srt_connection = NULL;
    }

    ngx_media_srt_ingest_stop(&ngx_media_srt_ingest);
    ngx_media_srt_shutdown();
}

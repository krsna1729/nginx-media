/*
 * Control API (goal doc 25).
 *
 *   GET  /media/api/v1/streams
 *   GET  /media/api/v1/streams/{app}/{stream}
 *   GET  /media/api/v1/streams/{app}/{stream}/sources
 *   POST /media/api/v1/streams/{app}/{stream}/switch?source=<id>
 *
 * The API reads and mutates the runtime stream registry of the worker that
 * serves the request.  The SRT listener and the registry live in worker 0;
 * deployments that serve the API from several workers need the inter-worker
 * routing phase (goal doc 23), and until then non-owner workers report
 * "not_owner" rather than inventing state.
 */

#include "ngx_media_platform.h"
#include "ngx_media_registry.h"

#include <ngx_http.h>

#define NGX_MEDIA_API_BUF_SIZE  (64 * 1024)

static char *ngx_media_api_set(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_media_api_init(ngx_conf_t *cf);
static ngx_int_t ngx_media_api_handler(ngx_http_request_t *r);
static ngx_int_t ngx_media_api_dispatch(ngx_http_request_t *r,
    ngx_media_registry_t *registry, u_char **last, u_char *end);
static ngx_int_t ngx_media_api_stream_json(u_char **last, u_char *end,
    ngx_media_stream_t *stream);
static ngx_int_t ngx_media_api_sources_json(u_char **last, u_char *end,
    ngx_media_stream_t *stream);
static const char *ngx_media_api_state_name(ngx_uint_t state);
static ngx_int_t ngx_media_api_send(ngx_http_request_t *r, ngx_int_t status,
    ngx_str_t *body);
static ngx_int_t ngx_media_api_arg(ngx_http_request_t *r, const char *name,
    ngx_str_t *value);

static ngx_command_t ngx_media_api_commands[] = {

    { ngx_string("media_api"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_media_api_set,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};

static ngx_http_module_t ngx_media_api_module_ctx = {
    NULL,                          /* preconfiguration */
    ngx_media_api_init,            /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    NULL,                          /* create location configuration */
    NULL                           /* merge location configuration */
};

ngx_module_t ngx_media_api_module = {
    NGX_MODULE_V1,
    &ngx_media_api_module_ctx,     /* module context */
    ngx_media_api_commands,        /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static char *
ngx_media_api_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    (void) cmd;
    (void) conf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_media_api_handler;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_media_api_init(ngx_conf_t *cf)
{
    (void) cf;

    return NGX_OK;
}

static const char *
ngx_media_api_state_name(ngx_uint_t state)
{
    switch (state) {

    case NGX_MEDIA_SOURCE_STANDBY:
        return "standby";

    case NGX_MEDIA_SOURCE_AWAITING_SYNC:
        return "awaiting_sync";

    case NGX_MEDIA_SOURCE_ACTIVE:
        return "active";

    case NGX_MEDIA_SOURCE_DRAINING:
        return "draining";

    default:
        return "unknown";
    }
}

static ngx_int_t
ngx_media_api_sources_json(u_char **last, u_char *end,
    ngx_media_stream_t *stream)
{
    ngx_queue_t         *q;
    ngx_media_source_t  *source;
    ngx_uint_t           first = 1;

    *last = ngx_snprintf(*last, end - *last, "[");

    for (q = ngx_queue_head(&stream->sources);
         q != (ngx_queue_t *) &stream->sources;
         q = q->next)
    {
        source = ngx_queue_data(q, ngx_media_source_t, queue);

        *last = ngx_snprintf(*last, end - *last,
                             "%s{\"id\":\"%V\",\"type\":%ui,\"state\":\"%s\","
                             "\"priority\":%ui,\"healthy\":%s,\"eligible\":%s,"
                             "\"active\":%s,\"frames_in\":%uL,"
                             "\"frames_out\":%uL,\"writers\":%ui,"
                             "\"preroll_units\":%ui,\"preroll_bytes\":%uz,"
                             "\"preroll_overflows\":%uL}",
                             first ? "" : ",",
                             &source->id,
                             source->type,
                             ngx_media_api_state_name(source->state),
                             source->priority,
                             source->healthy ? "true" : "false",
                             source->eligible ? "true" : "false",
                             source->active ? "true" : "false",
                             source->frames_in,
                             source->frames_out,
                             source->writers,
                             ngx_media_source_preroll_units(source),
                             ngx_media_source_preroll_bytes(source),
                             source->preroll.overflows);

        if (*last >= end - 1) {
            return NGX_ERROR;
        }

        first = 0;
    }

    *last = ngx_snprintf(*last, end - *last, "]");

    return (*last < end - 1) ? NGX_OK : NGX_ERROR;
}

static ngx_int_t
ngx_media_api_stream_json(u_char **last, u_char *end, ngx_media_stream_t *stream)
{
    *last = ngx_snprintf(*last, end - *last,
                         "{\"application\":\"%V\",\"name\":\"%V\","
                         "\"generation\":%ui,\"switches\":%uL,"
                         "\"program_frames\":%uL,\"active\":",
                         &stream->application, &stream->name,
                         stream->generation, stream->switches,
                         stream->program_frames);

    if (stream->active != NULL) {
        *last = ngx_snprintf(*last, end - *last, "\"%V\"", &stream->active->id);

    } else {
        *last = ngx_snprintf(*last, end - *last, "null");
    }

    *last = ngx_snprintf(*last, end - *last, ",\"sources\":");

    if (ngx_media_api_sources_json(last, end, stream) != NGX_OK) {
        return NGX_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, "}");

    return (*last < end - 1) ? NGX_OK : NGX_ERROR;
}

/* splits "/media/api/v1/streams/{app}/{stream}[/{action}]" into parts */
static ngx_int_t
ngx_media_api_parse(u_char *uri, size_t len, ngx_str_t *application,
    ngx_str_t *name, ngx_str_t *action)
{
    u_char  *p, *end, *slash;
    size_t   prefix = sizeof("/media/api/v1/streams") - 1;

    application->len = 0;
    name->len = 0;
    action->len = 0;

    if (len < prefix || ngx_memcmp(uri, "/media/api/v1/streams", prefix) != 0) {
        return NGX_DECLINED;
    }

    p = uri + prefix;
    end = uri + len;

    if (p == end) {
        return NGX_OK;                       /* collection */
    }

    if (*p != '/') {
        return NGX_DECLINED;
    }

    p++;
    slash = ngx_strlchr(p, end, '/');
    if (slash == NULL) {
        return NGX_DECLINED;
    }

    application->data = p;
    application->len = slash - p;
    p = slash + 1;

    slash = ngx_strlchr(p, end, '/');

    if (slash == NULL) {
        name->data = p;
        name->len = end - p;

    } else {
        name->data = p;
        name->len = slash - p;

        action->data = slash + 1;
        action->len = end - slash - 1;
    }

    if (application->len == 0 || name->len == 0) {
        return NGX_DECLINED;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_media_api_arg(ngx_http_request_t *r, const char *name, ngx_str_t *value)
{
    ngx_str_t  key;

    key.data = (u_char *) name;
    key.len = strlen(name);

    return ngx_http_arg(r, key.data, key.len, value);
}

static ngx_int_t
ngx_media_api_dispatch(ngx_http_request_t *r, ngx_media_registry_t *registry,
    u_char **last, u_char *end)
{
    ngx_str_t           application, name, action, source_id;
    ngx_media_stream_t *stream;
    ngx_media_source_t *source;
    ngx_queue_t        *q;
    ngx_media_registry_entry_t  *entry;
    ngx_uint_t          first = 1;

    switch (ngx_media_api_parse(r->uri.data, r->uri.len, &application, &name,
                                &action))
    {
    case NGX_DECLINED:
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"unknown_endpoint\"}");
        return NGX_HTTP_NOT_FOUND;

    case NGX_ERROR:
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"bad_request\"}");
        return NGX_HTTP_BAD_REQUEST;

    default:
        break;
    }

    /* collection: GET /streams */
    if (name.len == 0) {

        if (r->method != NGX_HTTP_GET) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"method_not_allowed\"}");
            return NGX_HTTP_NOT_ALLOWED;
        }

        *last = ngx_snprintf(*last, end - *last, "{\"streams\":[");

        for (q = ngx_queue_head(&registry->entries);
             q != (ngx_queue_t *) &registry->entries;
             q = q->next)
        {
            entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);
            stream = &entry->stream;

            *last = ngx_snprintf(*last, end - *last, "%s", first ? "" : ",");

            if (ngx_media_api_stream_json(last, end, stream) != NGX_OK) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            first = 0;
        }

        *last = ngx_snprintf(*last, end - *last, "],\"count\":%ui}",
                             ngx_media_registry_count(registry));

        return NGX_HTTP_OK;
    }

    stream = ngx_media_registry_stream(registry, &application, &name);

    if (stream == NULL) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"stream_not_found\"}");
        return NGX_HTTP_NOT_FOUND;
    }

    if (action.len == 0) {

        if (r->method != NGX_HTTP_GET) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"method_not_allowed\"}");
            return NGX_HTTP_NOT_ALLOWED;
        }

        if (ngx_media_api_stream_json(last, end, stream) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        return NGX_HTTP_OK;
    }

    if (action.len == sizeof("sources") - 1
        && ngx_memcmp(action.data, "sources", sizeof("sources") - 1) == 0)
    {
        *last = ngx_snprintf(*last, end - *last, "{\"sources\":");

        if (ngx_media_api_sources_json(last, end, stream) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last, "}");

        return NGX_HTTP_OK;
    }

    if (action.len == sizeof("switch") - 1
        && ngx_memcmp(action.data, "switch", sizeof("switch") - 1) == 0)
    {
        if (r->method != NGX_HTTP_POST) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"method_not_allowed\"}");
            return NGX_HTTP_NOT_ALLOWED;
        }

        if (ngx_media_api_arg(r, "source", &source_id) != NGX_OK) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"missing_source_argument\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        source = ngx_media_stream_source_find(stream, &source_id);

        if (source == NULL) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"source_not_found\","
                                 "\"source\":\"%V\"}", &source_id);
            return NGX_HTTP_NOT_FOUND;
        }

        if (ngx_media_stream_promote(stream, source) != NGX_OK) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"switch_failed\"}");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "media: api switch stream=%V/%V source=%V",
                      &application, &name, &source_id);

        *last = ngx_snprintf(*last, end - *last,
                             "{\"stream\":\"%V/%V\",\"requested\":\"%V\","
                             "\"active\":", &application, &name, &source_id);

        if (stream->active != NULL) {
            *last = ngx_snprintf(*last, end - *last, "\"%V\"",
                                 &stream->active->id);

        } else {
            *last = ngx_snprintf(*last, end - *last, "null");
        }

        *last = ngx_snprintf(*last, end - *last,
                             ",\"generation\":%ui,\"switches\":%uL}",
                             stream->generation, stream->switches);

        return NGX_HTTP_OK;
    }

    if (action.len == sizeof("switchback") - 1
        && ngx_memcmp(action.data, "switchback", sizeof("switchback") - 1) == 0)
    {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"not_implemented\","
                             "\"detail\":\"switchback policy arrives with "
                             "automatic failover\"}");
        return NGX_HTTP_NOT_IMPLEMENTED;
    }

    *last = ngx_snprintf(*last, end - *last, "{\"error\":\"unknown_action\"}");

    return NGX_HTTP_NOT_FOUND;
}

static ngx_int_t
ngx_media_api_send(ngx_http_request_t *r, ngx_int_t status, ngx_str_t *body)
{
    ngx_int_t    rc;
    ngx_buf_t   *b;
    ngx_chain_t  out;

    r->headers_out.status = status;
    r->headers_out.content_length_n = body->len;

    ngx_str_set(&r->headers_out.content_type, "application/json");

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->pos = body->data;
    b->last = body->data + body->len;
    b->memory = 1;
    b->last_buf = 1;

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}

static ngx_int_t
ngx_media_api_handler(ngx_http_request_t *r)
{
    ngx_media_registry_t  *registry;
    ngx_str_t              body;
    u_char                *buf, *last, *end;
    ngx_int_t              status, rc;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_POST))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    buf = ngx_pnalloc(r->pool, NGX_MEDIA_API_BUF_SIZE);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    last = buf;
    end = buf + NGX_MEDIA_API_BUF_SIZE;

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        last = ngx_snprintf(last, end - last, "{\"error\":\"no_registry\"}");
        status = NGX_HTTP_INTERNAL_SERVER_ERROR;

    } else {
        status = ngx_media_api_dispatch(r, registry, &last, end);
    }

    body.data = buf;
    body.len = last - buf;

    return ngx_media_api_send(r, status, &body);
}

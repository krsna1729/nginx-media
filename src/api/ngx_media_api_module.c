/*
 * Control API (goal doc 25).
 *
 *   GET  /media/api/v1/streams
 *   GET  /media/api/v1/streams/{app}/{stream}
 *   GET  /media/api/v1/streams/{app}/{stream}/sources
 *   POST /media/api/v1/streams/{app}/{stream}/switch?source=<id>
 *
 * The API reads and mutates the runtime stream registry of the worker that
 * serves the request, and every stream and source mutation is broadcast to the
 * other workers, so each holds a replica of the graph and any worker can
 * answer a read (see ngx_media_graph.h).  What is not replicated is the
 * program: one worker drives a stream, only that worker holds its program feed
 * and outputs, and only that worker materialises the transport of a file, an
 * HLS origin or a watched directory.  A read that lands elsewhere reports the
 * graph and the progress the owner published; the operations that only make
 * sense where the program lives - a manual switch, a switchback, and a
 * destination, which is an output rather than graph state - are answered by
 * the owner or refused with `not_owner` naming it, never applied to a replica
 * that nothing watches.
 */

#include "ngx_media_platform.h"
#include "ngx_media_registry.h"
#include "ngx_media_destination.h"
#include "ngx_media_file.h"
#include "ngx_media_graph.h"
#include "ngx_media_compat.h"
#include "ngx_media_hls_profile.h"
#include "ngx_media_policy.h"
#include "ngx_media_hls_ingest.h"

#include <fcntl.h>
#include <unistd.h>
#include "ngx_media_hls_pull.h"
#include "ngx_media_runtime.h"
#include "ngx_media_selector.h"

#include <ngx_http.h>

#define NGX_MEDIA_API_FEED_UNITS   2048
#define NGX_MEDIA_API_FEED_BYTES   (32 * 1024 * 1024)
#define NGX_MEDIA_API_FEED_AGE     10000

#define NGX_MEDIA_API_BUF_SIZE  (64 * 1024)

static ngx_str_t  ngx_media_api_none = ngx_string("none");

typedef struct {
    ngx_str_t   ingest_dir;    /* where media_hls_ingest stores segments */
} ngx_media_api_loc_conf_t;

static void *ngx_media_api_create_loc_conf(ngx_conf_t *cf);
static char *ngx_media_api_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_media_api_set(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_hls_ingest_set(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_media_hls_ingest_handler(ngx_http_request_t *r);
static void ngx_media_hls_ingest_ready(ngx_http_request_t *r);
static ngx_int_t ngx_media_api_init(ngx_conf_t *cf);
static ngx_int_t ngx_media_api_handler(ngx_http_request_t *r);
static void ngx_media_api_body_ready(ngx_http_request_t *r);
static ngx_int_t ngx_media_api_json_array(const ngx_str_t *body,
    const char *key, ngx_str_t *out);
static ngx_int_t ngx_media_api_desired_get(ngx_media_registry_t *registry,
    u_char **last, u_char *end);
static ngx_int_t ngx_media_api_desired_put(ngx_http_request_t *r,
    ngx_media_registry_t *registry, u_char **last, u_char *end);
static ngx_int_t ngx_media_api_desired_children(ngx_http_request_t *r,
    ngx_media_stream_t *stream, ngx_str_t *item, ngx_uint_t *created);
static ngx_int_t ngx_media_api_sources(ngx_http_request_t *r,
    ngx_media_stream_t *stream, ngx_str_t *action, u_char **last, u_char *end);
static ngx_int_t ngx_media_api_destinations(ngx_http_request_t *r,
    ngx_media_stream_t *stream, ngx_str_t *action, u_char **last, u_char *end);
static ngx_int_t ngx_media_api_destination_json(
    ngx_media_destination_t *destination, u_char **last, u_char *end,
    ngx_int_t created);
static ngx_int_t ngx_media_api_stream_create(ngx_http_request_t *r,
    ngx_media_registry_t *registry, u_char **last, u_char *end);
static ngx_int_t ngx_media_api_stream_delete(ngx_http_request_t *r,
    ngx_media_registry_t *registry, ngx_str_t *application, ngx_str_t *name,
    u_char **last, u_char *end);
static ngx_int_t ngx_media_api_stream_patch(ngx_http_request_t *r,
    ngx_media_registry_t *registry, ngx_str_t *application, ngx_str_t *name,
    u_char **last, u_char *end);
static ngx_int_t ngx_media_api_dispatch(ngx_http_request_t *r,
    ngx_media_registry_t *registry, u_char **last, u_char *end);
static ngx_int_t ngx_media_api_stream_json(u_char **last, u_char *end,
    ngx_media_stream_t *stream);
static ngx_int_t ngx_media_api_sources_json(u_char **last, u_char *end,
    ngx_media_stream_t *stream);
static ngx_int_t ngx_media_api_not_owner(ngx_media_stream_t *stream,
    u_char **last, u_char *end);
static const char *ngx_media_api_state_name(ngx_uint_t state);
static ngx_int_t ngx_media_api_send(ngx_http_request_t *r, ngx_int_t status,
    ngx_str_t *body);
static ngx_int_t ngx_media_api_arg(ngx_http_request_t *r, const char *name,
    ngx_str_t *value);
static ngx_int_t ngx_media_api_json_string(u_char **last, u_char *end,
    const ngx_str_t *value);
static ngx_int_t ngx_media_api_prom_prefix(u_char **last, u_char *end,
    const char *metric, const ngx_str_t *application, const ngx_str_t *name,
    const ngx_str_t *source, const char *percentile);
static ngx_int_t ngx_media_api_json_stream_name(u_char **last, u_char *end,
    const ngx_str_t *application, const ngx_str_t *name);
static ngx_int_t ngx_media_api_media_mode(const ngx_str_t *text,
    ngx_uint_t *mode);
static ngx_command_t ngx_media_api_commands[] = {

    { ngx_string("media_api"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_media_api_set,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    /*
     * media_hls_ingest <directory>;
     *
     * Accepts HLS segments pushed to us - someone else's encoder PUTs them -
     * and stores them where a source of type hls_push is watching.  The two
     * halves stay separate on purpose: this endpoint does not know what reads
     * the directory, so an upload arriving by any other means works too.
     */
    { ngx_string("media_hls_ingest"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_media_hls_ingest_set,
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

    ngx_media_api_create_loc_conf, /* create location configuration */
    ngx_media_api_merge_loc_conf   /* merge location configuration */
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

static void *
ngx_media_api_create_loc_conf(ngx_conf_t *cf)
{
    ngx_media_api_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_media_api_loc_conf_t));

    if (conf == NULL) {
        return NULL;
    }

    ngx_str_null(&conf->ingest_dir);

    return conf;
}

static char *
ngx_media_api_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_media_api_loc_conf_t  *prev = parent;
    ngx_media_api_loc_conf_t  *conf = child;

    (void) cf;

    if (conf->ingest_dir.len == 0) {
        conf->ingest_dir = prev->ingest_dir;
    }

    return NGX_CONF_OK;
}

/* --- HLS ingest endpoint ------------------------------------------------- */

static char *
ngx_media_hls_ingest_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_api_loc_conf_t  *mlcf = conf;
    ngx_http_core_loc_conf_t  *clcf;
    ngx_str_t                 *value = cf->args->elts;

    (void) cmd;

    if (mlcf->ingest_dir.len != 0) {
        return "duplicate media_hls_ingest";
    }

    mlcf->ingest_dir = value[1];

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_media_hls_ingest_handler;

    /*
     * The body is written to a file by nginx's own machinery rather than read
     * into memory: a segment is megabytes, and taking it as a temp file means
     * this handler never holds one.  It also lets the upload be linked into
     * place whole, so a reader never sees a partial segment.
     */
    clcf->client_body_in_file_only = 1;

    return NGX_CONF_OK;
}

/*
 * PUT /segment.ts -> the body lands in the watched directory.
 *
 * The name comes from the request URI, so an uploader names its segments the
 * way the reader expects, and the file appears atomically: the temp file is
 * linked into place, so a reader either sees a whole segment or none of it.
 */
static void
ngx_media_hls_ingest_ready(ngx_http_request_t *r)
{
    ngx_media_api_loc_conf_t  *mlcf;
    ngx_str_t                  name, target;
    u_char                    *slash;
    ngx_int_t                  stored;   /* not `rc`: something in the include
                                          * chain defines that name */

    if (r->request_body == NULL || r->request_body->temp_file == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    mlcf = ngx_http_get_module_loc_conf(r, ngx_media_api_module);

    if (mlcf == NULL || mlcf->ingest_dir.len == 0) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    /* bounded reverse search: r->uri is a slice and is not NUL-terminated */
    slash = ngx_media_strrlchr(r->uri.data, r->uri.data + r->uri.len, '/');
    name.data = (slash != NULL) ? slash + 1 : r->uri.data;
    name.len = r->uri.len - (size_t) (name.data - r->uri.data);

    if (name.len < 4
        || ngx_memcmp(name.data + name.len - 3, ".ts", 3) != 0
        || name.data[0] == '.')
    {
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    {
        size_t  i;
        for (i = 0; i < name.len; i++) {
            u_char c = name.data[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                  || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            {
                ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
                return;
            }
        }
    }
    target.len = mlcf->ingest_dir.len + 1 + name.len;
    target.data = ngx_pnalloc(r->pool, target.len + 1);

    if (target.data == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    ngx_snprintf(target.data, target.len, "%V/%V", &mlcf->ingest_dir, &name);
    target.data[target.len] = '\0';

    /*
     * Renamed into place, so a reader either sees a whole segment or none of
     * it.  nginx writes the body to client_body_temp_path, which therefore
     * has to be on the same filesystem as the ingest directory - the same
     * requirement any nginx upload-to-final-location setup has, and the
     * reason that directive exists.
     */
    stored = ngx_rename_file(r->request_body->temp_file->file.name.data,
                             target.data);

    if (stored == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                      "media: could not store uploaded segment as %V", &target);
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "media: hls ingest stored name=%V dir=%V len=%uz (%O bytes)",
                  &name, &mlcf->ingest_dir, target.len,
                  r->request_body->temp_file->file.offset);

    ngx_http_finalize_request(r, NGX_HTTP_CREATED);
}

static ngx_int_t
ngx_media_hls_ingest_handler(ngx_http_request_t *r)
{
    ngx_int_t  rc;

    if (!(r->method & (NGX_HTTP_PUT|NGX_HTTP_POST))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_read_client_request_body(r, ngx_media_hls_ingest_ready);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
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
ngx_media_api_put_bytes(u_char **last, u_char *end, const u_char *data,
    size_t len)
{
    if ((size_t) (end - *last) < len) {
        return NGX_ERROR;
    }

    *last = ngx_cpymem(*last, data, len);

    return NGX_OK;
}

static ngx_int_t
ngx_media_api_json_string(u_char **last, u_char *end, const ngx_str_t *value)
{
    static const u_char  hex[] = "0123456789abcdef";
    u_char                escaped[6];
    u_char                c;
    size_t                i;

    if (ngx_media_api_put_bytes(last, end, (u_char *) "\"", 1) != NGX_OK) {
        return NGX_ERROR;
    }

    for (i = 0; i < value->len; i++) {
        c = value->data[i];

        switch (c) {
        case '"':
            if (ngx_media_api_put_bytes(last, end, (u_char *) "\\\"", 2)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            break;

        case '\\':
            if (ngx_media_api_put_bytes(last, end, (u_char *) "\\\\", 2)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            break;

        case '\b':
            if (ngx_media_api_put_bytes(last, end, (u_char *) "\\b", 2)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            break;

        case '\f':
            if (ngx_media_api_put_bytes(last, end, (u_char *) "\\f", 2)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            break;

        case '\n':
            if (ngx_media_api_put_bytes(last, end, (u_char *) "\\n", 2)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            break;

        case '\r':
            if (ngx_media_api_put_bytes(last, end, (u_char *) "\\r", 2)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            break;

        case '\t':
            if (ngx_media_api_put_bytes(last, end, (u_char *) "\\t", 2)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            break;

        default:
            if (c < 0x20) {
                escaped[0] = '\\';
                escaped[1] = 'u';
                escaped[2] = '0';
                escaped[3] = '0';
                escaped[4] = hex[c >> 4];
                escaped[5] = hex[c & 0x0f];

                if (ngx_media_api_put_bytes(last, end, escaped,
                                            sizeof(escaped))
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }
                break;
            }

            if (ngx_media_api_put_bytes(last, end, &c, 1) != NGX_OK) {
                return NGX_ERROR;
            }
            break;
        }
    }

    return ngx_media_api_put_bytes(last, end, (u_char *) "\"", 1);
}

static ngx_int_t
ngx_media_api_json_stream_name(u_char **last, u_char *end,
    const ngx_str_t *application, const ngx_str_t *name)
{
    u_char  *name_start;
    size_t   name_len;

    if (ngx_media_api_json_string(last, end, application) != NGX_OK) {
        return NGX_ERROR;
    }

    (*last)--;

    if (ngx_media_api_put_bytes(last, end, (u_char *) "/", 1) != NGX_OK) {
        return NGX_ERROR;
    }

    name_start = *last;

    if (ngx_media_api_json_string(last, end, name) != NGX_OK) {
        return NGX_ERROR;
    }

    name_len = *last - name_start;
    ngx_memmove(name_start, name_start + 1, name_len - 1);
    (*last)--;

    return NGX_OK;
}

static ngx_int_t
ngx_media_api_prom_string(u_char **last, u_char *end, const ngx_str_t *value)
{
    u_char  c, slash = '\\';
    size_t  i;

    for (i = 0; i < value->len; i++) {
        c = value->data[i];

        if (c == '\\' || c == '"') {
            if (ngx_media_api_put_bytes(last, end, &slash, 1) != NGX_OK
                || ngx_media_api_put_bytes(last, end, &c, 1) != NGX_OK)
            {
                return NGX_ERROR;
            }

        } else if (c == '\n') {
            if (ngx_media_api_put_bytes(last, end, (u_char *) "\\n", 2)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

        } else if (c < 0x20) {
            return NGX_ERROR;

        } else if (ngx_media_api_put_bytes(last, end, &c, 1) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_media_api_prom_prefix(u_char **last, u_char *end, const char *metric,
    const ngx_str_t *application, const ngx_str_t *name,
    const ngx_str_t *source, const char *percentile)
{
    ngx_str_t  text;

    text.data = (u_char *) metric;
    text.len = strlen(metric);

    if (ngx_media_api_put_bytes(last, end, text.data, text.len) != NGX_OK
        || ngx_media_api_put_bytes(last, end,
                                   (u_char *) "{application=\"", 14)
           != NGX_OK
        || ngx_media_api_prom_string(last, end, application) != NGX_OK
        || ngx_media_api_put_bytes(last, end, (u_char *) "\",name=\"", 8)
           != NGX_OK
        || ngx_media_api_prom_string(last, end, name) != NGX_OK
        || ngx_media_api_put_bytes(last, end, (u_char *) "\"", 1) != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (source != NULL) {
        if (ngx_media_api_put_bytes(last, end, (u_char *) ",source=\"", 9)
                != NGX_OK
            || ngx_media_api_prom_string(last, end, source) != NGX_OK
            || ngx_media_api_put_bytes(last, end, (u_char *) "\"", 1)
                   != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    if (percentile != NULL) {
        text.data = (u_char *) percentile;
        text.len = strlen(percentile);

        if (ngx_media_api_put_bytes(last, end, (u_char *) ",percentile=\"",
                                    13)
                != NGX_OK
            || ngx_media_api_put_bytes(last, end, text.data, text.len)
                   != NGX_OK
            || ngx_media_api_put_bytes(last, end, (u_char *) "\"", 1)
                   != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
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

        *last = ngx_snprintf(*last, end - *last, "%s{\"id\":",
                             first ? "" : ",");

        if (ngx_media_api_json_string(last, end, &source->id) != NGX_OK) {
            return NGX_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last,
                             ",\"type\":%ui,\"state\":\"%s\","
                             "\"priority\":%ui,\"healthy\":%s,"
                             "\"eligible\":%s,\"active\":%s,\"compat\":\"%s\","
                             "\"evidence\":%ui,\"health_transitions\":%uL,"
                             "\"container_errors\":%uL,\"frames_in\":%uL,"
                             "\"frames_out\":%uL,\"writers\":%ui,"
                             "\"preroll_units\":%ui,\"preroll_bytes\":%uz,"
                             "\"preroll_overflows\":%uL,"
                             "\"preroll_unit_overflows\":%uL,"
                             "\"preroll_byte_overflows\":%uL,"
                             "\"preroll_high_water_units\":%ui,"
                             "\"preroll_high_water_bytes\":%uz}",
                             source->type,
                             ngx_media_api_state_name(source->state),
                             source->priority,
                             source->health.healthy ? "true" : "false",
                             source->health.eligible ? "true" : "false",
                             source->active ? "true" : "false",
                             ngx_media_compat_name(source->compat),
                             source->health.evidence,
                             source->health.transitions,
                             source->health.container_errors,
                             source->frames_in,
                             source->frames_out,
                             source->writers,
                             ngx_media_source_preroll_units(source),
                             ngx_media_source_preroll_bytes(source),
                             ngx_media_source_preroll_overflows(source),
                             ngx_media_source_preroll_unit_overflows(source),
                             ngx_media_source_preroll_byte_overflows(source),
                             ngx_media_source_preroll_high_water_units(source),
                             ngx_media_source_preroll_high_water_bytes(source));

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
    ngx_media_runtime_progress_t  progress;

    ngx_media_runtime_progress(&stream->application, &stream->name,
                               stream->generation, stream->program_frames,
                               &progress);

    *last = ngx_snprintf(*last, end - *last, "{\"application\":");

    if (ngx_media_api_json_string(last, end, &stream->application) != NGX_OK) {
        return NGX_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, ",\"name\":");

    if (ngx_media_api_json_string(last, end, &stream->name) != NGX_OK) {
        return NGX_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last,
                         ",\"owner\":%ui,\"observed_here\":%s,"
                         "\"revision\":%uL,\"incarnation\":%uL,"
                         "\"media\":\"%s\",\"generation\":%ui,"
                         "\"switches\":%uL,\"emergency_switches\":%uL,"
                         "\"failure_timeout_ms\":%ui,"
                         "\"recovery_timeout_ms\":%ui,\"switchback\":%ui,"
                         "\"program_frames\":%uL,\"active\":",
                         progress.owner, progress.local ? "true" : "false",
                         stream->revision, stream->incarnation,
                         stream->media_mode == NGX_MEDIA_STREAM_MEDIA_PROFILE
                         ? "profile" : "source",
                         progress.generation, stream->switches,
                         stream->emergency_switches,
                         stream->selector.failure_timeout,
                         stream->selector.recovery_timeout,
                         stream->selector.switchback, progress.frames);

    if (stream->active != NULL) {
        if (ngx_media_api_json_string(last, end, &stream->active->id)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

    } else {
        *last = ngx_snprintf(*last, end - *last, "null");
    }

    *last = ngx_snprintf(*last, end - *last, ",\"sources\":");

    if (ngx_media_api_sources_json(last, end, stream) != NGX_OK) {
        return NGX_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, ",\"destinations\":[");

    {
        ngx_queue_t              *q;
        ngx_media_destination_t  *destination;
        ngx_uint_t                first = 1;

        for (q = ngx_queue_head(&stream->destinations);
             q != (ngx_queue_t *) &stream->destinations;
             q = q->next)
        {
            destination = ngx_queue_data(q, ngx_media_destination_t, queue);

            if (!first) {
                *last = ngx_snprintf(*last, end - *last, ",");
            }

            first = 0;

            if (ngx_media_api_destination_json(destination, last, end, -1)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
        }
    }

    *last = ngx_snprintf(*last, end - *last, "]");
    *last = ngx_snprintf(*last, end - *last,
                         ",\"fanout_ms\":{\"p50\":%M,\"p95\":%M,"
                         "\"p99\":%M,\"max\":%uL},\"dispatched\":%uL}",
                         ngx_media_feed_fanout_percentile(&stream->program_feed,
                                                          500),
                         ngx_media_feed_fanout_percentile(&stream->program_feed,
                                                          950),
                         ngx_media_feed_fanout_percentile(&stream->program_feed,
                                                          990),
                         ngx_media_feed_fanout_max(&stream->program_feed),
                         ngx_media_feed_fanout_count(&stream->program_feed));

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

/*
 * GET /media/api/v1/metrics
 *
 * Prometheus text format.  Every number here is already maintained by the
 * program runtime; nothing is computed or sampled on the request path, so
 * scraping costs one walk of the registry.
 */
static ngx_int_t
ngx_media_api_metrics(ngx_media_registry_t *registry, u_char **last,
    u_char *end)
{
    ngx_queue_t                    *q, *sq;
    ngx_media_registry_entry_t     *entry;
    ngx_media_stream_t             *stream;
    ngx_media_source_t             *source;
    ngx_media_runtime_stats_t       stats;
    ngx_media_runtime_progress_t    progress;

    ngx_media_runtime_stats_get(&stats);

    *last = ngx_snprintf(*last, end - *last,
                         "# HELP nginx_media_stream_generation "
                         "selection generation of the program\n"
                         "# TYPE nginx_media_stream_generation counter\n"
                         "# HELP nginx_media_stream_switches "
                         "switches taken by the selector\n"
                         "# TYPE nginx_media_stream_switches counter\n"
                         "# HELP nginx_media_stream_program_frames "
                         "frames carried by the program\n"
                         "# TYPE nginx_media_stream_program_frames counter\n"
                         "# HELP nginx_media_source_frames_in "
                         "frames accepted from the source\n"
                         "# TYPE nginx_media_source_frames_in counter\n"
                         "# HELP nginx_media_source_frames_out "
                         "frames the source delivered to the program\n"
                         "# TYPE nginx_media_source_frames_out counter\n"
                         "# HELP nginx_media_source_healthy "
                         "1 when the source passes its health policy\n"
                         "# TYPE nginx_media_source_healthy gauge\n"
                         "# HELP nginx_media_source_active "
                         "1 when the source is on air\n"
                         "# TYPE nginx_media_source_active gauge\n"
                         "# HELP nginx_media_source_preroll_units "
                         "units retained in the source preroll cache\n"
                         "# TYPE nginx_media_source_preroll_units gauge\n"
                         "# HELP nginx_media_source_preroll_bytes "
                         "payload bytes retained in the source preroll cache\n"
                         "# TYPE nginx_media_source_preroll_bytes gauge\n"
                         "# HELP nginx_media_source_preroll_overflows_total "
                         "source preroll resets caused by a retention ceiling\n"
                         "# TYPE nginx_media_source_preroll_overflows_total "
                         "counter\n"
                         "# HELP nginx_media_source_preroll_unit_overflows_total "
                         "source preroll resets caused by the unit ceiling\n"
                         "# TYPE nginx_media_source_preroll_unit_overflows_total "
                         "counter\n"
                         "# HELP nginx_media_source_preroll_byte_overflows_total "
                         "source preroll resets caused by the byte ceiling\n"
                         "# TYPE nginx_media_source_preroll_byte_overflows_total "
                         "counter\n"
                         "# HELP nginx_media_source_preroll_high_water_units "
                         "highest source preroll units retained\n"
                         "# TYPE nginx_media_source_preroll_high_water_units "
                         "gauge\n"
                         "# HELP nginx_media_source_preroll_high_water_bytes "
                         "highest source preroll payload bytes retained\n"
                         "# TYPE nginx_media_source_preroll_high_water_bytes "
                         "gauge\n"
                         "# HELP nginx_media_stream_fanout_delay_ms "
                         "dispatch_time minus program_publish_time, upper "
                         "bound of the bucket the percentile falls in\n"
                         "# TYPE nginx_media_stream_fanout_delay_ms gauge\n"
                         "# HELP nginx_media_stream_dispatched_total "
                         "units taken by consumers from the program feed\n"
                         "# TYPE nginx_media_stream_dispatched_total counter\n"
                         "# HELP nginx_media_stream_feed_units "
                         "units retained by the program feed, the lag a slow "
                         "consumer is running at\n"
                         "# TYPE nginx_media_stream_feed_units gauge\n"
                         "# HELP nginx_media_stream_feed_bytes "
                         "payload bytes retained by the program feed\n"
                         "# TYPE nginx_media_stream_feed_bytes gauge\n"
                         "# HELP nginx_media_stream_feed_evictions_total "
                         "program feed units removed by a capacity ceiling\n"
                         "# TYPE nginx_media_stream_feed_evictions_total "
                         "counter\n"
                         "# HELP nginx_media_stream_feed_overruns_total "
                         "consumer reads that fell behind the retained window\n"
                         "# TYPE nginx_media_stream_feed_overruns_total counter\n"
                         "# HELP nginx_media_stream_feed_generation_mismatches_total "
                         "consumer reads rejected for a stale generation or cursor\n"
                         "# TYPE nginx_media_stream_feed_generation_mismatches_total "
                         "counter\n"
                         "# HELP nginx_media_stream_feed_publish_errors_total "
                         "program feed publish failures\n"
                         "# TYPE nginx_media_stream_feed_publish_errors_total "
                         "counter\n"
                         "# HELP nginx_media_stream_feed_high_water_units "
                         "highest retained program feed units\n"
                         "# TYPE nginx_media_stream_feed_high_water_units gauge\n"
                         "# HELP nginx_media_stream_feed_high_water_bytes "
                         "highest retained program feed payload bytes\n"
                         "# TYPE nginx_media_stream_feed_high_water_bytes gauge\n"
                         "# HELP nginx_media_runtime_outputs "
                         "runtime output slots in use\n"
                         "# TYPE nginx_media_runtime_outputs gauge\n"
                         "# HELP nginx_media_streams_draining "
                         "deleted streams whose memory is still held by a "
                         "reader that is stopping\n"
                         "# TYPE nginx_media_streams_draining gauge\n"
                         "# HELP nginx_media_worker_event_loop_delay_ms "
                         "interval between the last two runtime ticks, which "
                         "the timer asks to be 100ms\n"
                         "# TYPE nginx_media_worker_event_loop_delay_ms gauge\n"
                         "# HELP nginx_media_worker_event_loop_max_delay_ms "
                         "worst tick interval this worker has seen\n"
                         "# TYPE nginx_media_worker_event_loop_max_delay_ms gauge\n"
                         "# HELP nginx_media_worker_late_ticks_total "
                         "ticks that missed their interval by more than half\n"
                         "# TYPE nginx_media_worker_late_ticks_total counter\n"
                         "nginx_media_runtime_outputs %ui\n"
                         "nginx_media_streams_draining %ui\n"
                         "nginx_media_worker_event_loop_delay_ms %M\n"
                         "nginx_media_worker_event_loop_max_delay_ms %M\n"
                         "nginx_media_worker_late_ticks_total %uL\n"
                         "# HELP nginx_media_worker_service_ms "
                         "how long the last runtime tick took, the time this "
                         "worker spent serving every program it owns\n"
                         "# TYPE nginx_media_worker_service_ms gauge\n"
                         "# HELP nginx_media_worker_max_service_ms "
                         "worst tick duration this worker has seen\n"
                         "# TYPE nginx_media_worker_max_service_ms gauge\n"
                         "# HELP nginx_media_reconnecting_sources "
                         "sources whose transport is up but which are not "
                         "carrying media yet\n"
                         "# TYPE nginx_media_reconnecting_sources gauge\n"
                         "# HELP nginx_media_graph_undelivered_total "
                         "graph operations this worker could not hand to a "
                         "peer worker, cumulative\n"
                         "# TYPE nginx_media_graph_undelivered_total counter\n"
                         "# HELP nginx_media_runtime_routed_identity_mismatches_total "
                         "routed messages rejected for a different stream incarnation\n"
                         "# TYPE nginx_media_runtime_routed_identity_mismatches_total "
                         "counter\n"
                         "# HELP nginx_media_runtime_routed_slot_overflows_total "
                         "routed opens rejected because the fixed slot table was full\n"
                         "# TYPE nginx_media_runtime_routed_slot_overflows_total "
                         "counter\n"
                         "# HELP nginx_media_runtime_routed_no_slot_total "
                         "routed media messages received after their slot was removed\n"
                         "# TYPE nginx_media_runtime_routed_no_slot_total counter\n"
                         "# HELP nginx_media_runtime_routed_no_payload_total "
                         "routed media messages without a payload\n"
                         "# TYPE nginx_media_runtime_routed_no_payload_total counter\n"
                         "# HELP nginx_media_runtime_routed_reassembly_errors_total "
                         "routed frames rejected by the bounded reassembler\n"
                         "# TYPE nginx_media_runtime_routed_reassembly_errors_total "
                         "counter\n"
                         "# HELP nginx_media_runtime_routed_publish_errors_total "
                         "routed frames rejected by source or program publication\n"
                         "# TYPE nginx_media_runtime_routed_publish_errors_total "
                         "counter\n"
                         "nginx_media_worker_service_ms %M\n"
                         "nginx_media_worker_max_service_ms %M\n"
                         "nginx_media_reconnecting_sources %ui\n"
                         "nginx_media_graph_undelivered_total %uL\n"
                         "nginx_media_runtime_routed_identity_mismatches_total %uL\n"
                         "nginx_media_runtime_routed_slot_overflows_total %uL\n"
                         "nginx_media_runtime_routed_no_slot_total %uL\n"
                         "nginx_media_runtime_routed_no_payload_total %uL\n"
                         "nginx_media_runtime_routed_reassembly_errors_total %uL\n"
                         "nginx_media_runtime_routed_publish_errors_total %uL\n",
                         ngx_media_runtime_outputs_active(),
                         ngx_media_registry_draining_count(registry),
                         stats.last_gap, stats.max_gap, stats.late_ticks,
                         stats.last_service, stats.max_service,
                         stats.reconnecting,
                         ngx_media_route_broadcast_undelivered(),
                         stats.routed_identity_mismatches,
                         stats.routed_slot_overflows,
                         stats.routed_no_slot,
                         stats.routed_no_payload,
                         stats.routed_reassembly_errors,
                         stats.routed_publish_errors);

    for (q = ngx_queue_head(&registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = q->next)
    {
        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);
        stream = &entry->stream;

        ngx_media_runtime_progress(&stream->application, &stream->name,
                                   stream->generation, stream->program_frames,
                                   &progress);

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_generation",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %ui\n",
                             progress.generation);

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_switches",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %uL\n", stream->switches);

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_program_frames",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                             progress.frames);

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_fanout_delay_ms",
                &stream->application, &stream->name, NULL, "50") != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %M\n",
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 500));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_fanout_delay_ms",
                &stream->application, &stream->name, NULL, "95") != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %M\n",
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 950));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_fanout_delay_ms",
                &stream->application, &stream->name, NULL, "99") != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %M\n",
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 990));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_fanout_delay_ms",
                &stream->application, &stream->name, NULL, "99.9")
            != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %M\n",
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 999));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_dispatched_total",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                             ngx_media_feed_fanout_count(
                                 &stream->program_feed));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_feed_units",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %ui\n",
                             ngx_media_feed_units(&stream->program_feed));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_feed_bytes",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %uz\n",
                             ngx_media_feed_bytes(&stream->program_feed));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_feed_evictions_total",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                             ngx_media_feed_evictions(&stream->program_feed));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_feed_overruns_total",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                             ngx_media_feed_overruns(&stream->program_feed));

        if (ngx_media_api_prom_prefix(
                last, end,
                "nginx_media_stream_feed_generation_mismatches_total",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                             ngx_media_feed_generation_mismatches(
                                 &stream->program_feed));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_feed_publish_errors_total",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                             ngx_media_feed_publish_errors(
                                 &stream->program_feed));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_feed_high_water_units",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %ui\n",
                             ngx_media_feed_high_water_units(
                                 &stream->program_feed));

        if (ngx_media_api_prom_prefix(
                last, end, "nginx_media_stream_feed_high_water_bytes",
                &stream->application, &stream->name, NULL, NULL) != NGX_OK)
        {
            return NGX_ERROR;
        }
        *last = ngx_snprintf(*last, end - *last, "} %uz\n",
                             ngx_media_feed_high_water_bytes(
                                 &stream->program_feed));

        for (sq = ngx_queue_head(&stream->sources);
             sq != (ngx_queue_t *) &stream->sources;
             sq = sq->next)
        {
            source = ngx_queue_data(sq, ngx_media_source_t, queue);

            if (ngx_media_api_prom_prefix(
                    last, end, "nginx_media_source_frames_in",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                                 source->frames_in);

            if (ngx_media_api_prom_prefix(
                    last, end, "nginx_media_source_frames_out",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                                 source->frames_out);

            if (ngx_media_api_prom_prefix(
                    last, end, "nginx_media_source_healthy",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %d\n",
                                 source->health.healthy ? 1 : 0);

            if (ngx_media_api_prom_prefix(
                    last, end, "nginx_media_source_active",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %d\n",
                                 source->active ? 1 : 0);

            if (ngx_media_api_prom_prefix(
                    last, end, "nginx_media_source_preroll_units",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %ui\n",
                                 ngx_media_source_preroll_units(source));

            if (ngx_media_api_prom_prefix(
                    last, end, "nginx_media_source_preroll_bytes",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %uz\n",
                                 ngx_media_source_preroll_bytes(source));

            if (ngx_media_api_prom_prefix(
                    last, end, "nginx_media_source_preroll_overflows_total",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                                 ngx_media_source_preroll_overflows(source));

            if (ngx_media_api_prom_prefix(
                    last, end,
                    "nginx_media_source_preroll_unit_overflows_total",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                                 ngx_media_source_preroll_unit_overflows(
                                     source));

            if (ngx_media_api_prom_prefix(
                    last, end,
                    "nginx_media_source_preroll_byte_overflows_total",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %uL\n",
                                 ngx_media_source_preroll_byte_overflows(
                                     source));

            if (ngx_media_api_prom_prefix(
                    last, end, "nginx_media_source_preroll_high_water_units",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %ui\n",
                                 ngx_media_source_preroll_high_water_units(
                                     source));

            if (ngx_media_api_prom_prefix(
                    last, end, "nginx_media_source_preroll_high_water_bytes",
                    &stream->application, &stream->name, &source->id, NULL)
                != NGX_OK)
            {
                return NGX_ERROR;
            }
            *last = ngx_snprintf(*last, end - *last, "} %uz\n",
                                 ngx_media_source_preroll_high_water_bytes(
                                     source));
        }
    }

    return (*last < end - 1) ? NGX_OK : NGX_ERROR;
}

/* --- request body ------------------------------------------------------- */

/*
 * The control API takes small JSON objects.  A full parser would be a
 * dependency and an attack surface for no gain here: the bodies are flat
 * objects of short strings and integers, so this reads exactly that and
 * rejects anything else.
 */
typedef struct {
    ngx_str_t  body;
    u_char    *pos;
} ngx_media_api_json_t;

static ngx_int_t
ngx_media_api_read_body(ngx_http_request_t *r, ngx_pool_t *pool,
    ngx_str_t *body)
{
    ngx_chain_t  *cl;
    size_t        len = 0, pos = 0;
    u_char       *dst;

    body->len = 0;
    body->data = NULL;

    if (r->headers_in.content_length_n <= 0) {
        return NGX_OK;
    }

    if (r->headers_in.content_length_n > 8192) {
        return NGX_ERROR;
    }

    len = (size_t) r->headers_in.content_length_n;
    dst = ngx_pnalloc(pool, len + 1);

    if (dst == NULL) {
        return NGX_ERROR;
    }

    for (cl = r->request_body->bufs; cl != NULL; cl = cl->next) {
        size_t  take = cl->buf->last - cl->buf->pos;

        if (pos + take > len) {
            take = len - pos;
        }

        ngx_memcpy(dst + pos, cl->buf->pos, take);
        pos += take;

        if (pos == len) {
            break;
        }
    }

    if (pos != len) {
        return NGX_ERROR;
    }

    dst[len] = '\0';

    body->data = dst;
    body->len = len;

    return NGX_OK;
}

/* Decode one JSON string.  `out` may alias the input because escapes shrink. */
static ngx_int_t
ngx_media_api_json_parse_string(u_char *p, u_char *end, u_char **next,
    u_char *out, size_t cap, ngx_str_t *value)
{
    u_char       *q;
    ngx_uint_t    code, low;
    size_t        i;

    q = out;

    while (p < end) {
        if (*p == '"') {
            value->data = out;
            value->len = q - out;
            *next = p + 1;
            return NGX_OK;
        }

        if (*p != '\\') {
            if (*p < 0x20 || (size_t) (out + cap - q) < 1) {
                return NGX_ERROR;
            }

            *q++ = *p++;
            continue;
        }

        p++;

        if (p == end) {
            return NGX_ERROR;
        }

        switch (*p++) {
        case '"':
            code = '"';
            break;

        case '\\':
            code = '\\';
            break;

        case '/':
            code = '/';
            break;

        case 'b':
            code = '\b';
            break;

        case 'f':
            code = '\f';
            break;

        case 'n':
            code = '\n';
            break;

        case 'r':
            code = '\r';
            break;

        case 't':
            code = '\t';
            break;

        case 'u':
            if ((size_t) (end - p) < 4) {
                return NGX_ERROR;
            }

            code = 0;

            for (i = 0; i < 4; i++) {
                if (*p >= '0' && *p <= '9') {
                    code = (code << 4) | (*p - '0');

                } else if (*p >= 'a' && *p <= 'f') {
                    code = (code << 4) | (*p - 'a' + 10);

                } else if (*p >= 'A' && *p <= 'F') {
                    code = (code << 4) | (*p - 'A' + 10);

                } else {
                    return NGX_ERROR;
                }

                p++;
            }

            if (code >= 0xD800 && code <= 0xDBFF) {
                if ((size_t) (end - p) < 6 || p[0] != '\\' || p[1] != 'u') {
                    return NGX_ERROR;
                }

                p += 2;
                low = 0;

                for (i = 0; i < 4; i++) {
                    if (*p >= '0' && *p <= '9') {
                        low = (low << 4) | (*p - '0');

                    } else if (*p >= 'a' && *p <= 'f') {
                        low = (low << 4) | (*p - 'a' + 10);

                    } else if (*p >= 'A' && *p <= 'F') {
                        low = (low << 4) | (*p - 'A' + 10);

                    } else {
                        return NGX_ERROR;
                    }

                    p++;
                }

                if (low < 0xDC00 || low > 0xDFFF) {
                    return NGX_ERROR;
                }

                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);

            } else if (code >= 0xDC00 && code <= 0xDFFF) {
                return NGX_ERROR;
            }
            break;

        default:
            return NGX_ERROR;
        }

        if (code <= 0x7F) {
            if ((size_t) (out + cap - q) < 1) {
                return NGX_ERROR;
            }

            *q++ = (u_char) code;

        } else if (code <= 0x7FF) {
            if ((size_t) (out + cap - q) < 2) {
                return NGX_ERROR;
            }

            *q++ = (u_char) (0xC0 | (code >> 6));
            *q++ = (u_char) (0x80 | (code & 0x3F));

        } else if (code <= 0xFFFF) {
            if ((size_t) (out + cap - q) < 3) {
                return NGX_ERROR;
            }

            *q++ = (u_char) (0xE0 | (code >> 12));
            *q++ = (u_char) (0x80 | ((code >> 6) & 0x3F));
            *q++ = (u_char) (0x80 | (code & 0x3F));

        } else {
            if ((size_t) (out + cap - q) < 4) {
                return NGX_ERROR;
            }

            *q++ = (u_char) (0xF0 | (code >> 18));
            *q++ = (u_char) (0x80 | ((code >> 12) & 0x3F));
            *q++ = (u_char) (0x80 | ((code >> 6) & 0x3F));
            *q++ = (u_char) (0x80 | (code & 0x3F));
        }
    }

    return NGX_ERROR;
}

/* "key": "value"  |  "key": 123   -- the first match, or NGX_DECLINED */
static ngx_int_t
ngx_media_api_json_field(const ngx_str_t *body, const char *key,
    ngx_str_t *value)
{
    u_char       *p, *end, *next, *start;
    u_char        key_data[64];
    ngx_str_t     parsed_key, parsed_value;
    size_t        key_len = strlen(key);

    if (body->data == NULL || key_len >= sizeof(key_data)) {
        return NGX_DECLINED;
    }

    p = body->data;
    end = body->data + body->len;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'
                           || *p == '\n' || *p == '{' || *p == ','))
        {
            p++;
        }

        if (p == end) {
            break;
        }

        if (*p != '"'
            || ngx_media_api_json_parse_string(p + 1, end, &next,
                                               key_data, sizeof(key_data),
                                               &parsed_key)
               != NGX_OK)
        {
            return NGX_DECLINED;
        }

        p = next;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'
                           || *p == '\n'))
        {
            p++;
        }

        if (p == end || *p++ != ':') {
            return NGX_DECLINED;
        }

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'
                           || *p == '\n'))
        {
            p++;
        }

        if (p == end) {
            return NGX_DECLINED;
        }

        if (*p == '"') {
            start = p + 1;

            if (ngx_media_api_json_parse_string(start, end, &next, start,
                                                end - start, &parsed_value)
                != NGX_OK)
            {
                return NGX_DECLINED;
            }

            p = next;

            if (parsed_key.len == key_len
                && ngx_memcmp(parsed_key.data, key, key_len) == 0)
            {
                *value = parsed_value;
                return NGX_OK;
            }

            continue;
        }

        start = p;

        while (p < end && *p != ',' && *p != '}') {
            p++;
        }

        while (p > start && (p[-1] == ' ' || p[-1] == '\t'
                             || p[-1] == '\r' || p[-1] == '\n'))
        {
            p--;
        }

        if (parsed_key.len == key_len
            && ngx_memcmp(parsed_key.data, key, key_len) == 0)
        {
            value->data = start;
            value->len = p - start;
            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}
static ngx_int_t
ngx_media_api_media_mode(const ngx_str_t *text, ngx_uint_t *mode)
{
    if (text == NULL || mode == NULL) {
        return NGX_ERROR;
    }

    if (text->len == sizeof("source") - 1
        && ngx_strncasecmp(text->data, (u_char *) "source",
                           sizeof("source") - 1) == 0)
    {
        *mode = NGX_MEDIA_STREAM_MEDIA_SOURCE;
        return NGX_OK;
    }

    if (text->len == sizeof("profile") - 1
        && ngx_strncasecmp(text->data, (u_char *) "profile",
                           sizeof("profile") - 1) == 0)
    {
        *mode = NGX_MEDIA_STREAM_MEDIA_PROFILE;
        return NGX_OK;
    }

    return NGX_ERROR;
}

static ngx_int_t
ngx_media_api_arg(ngx_http_request_t *r, const char *name, ngx_str_t *value)
{
    ngx_str_t  key;

    key.data = (u_char *) name;
    key.len = strlen(name);

    return ngx_http_arg(r, key.data, key.len, value);
}

/*
 * POST /media/api/v1/streams
 *
 * Body: {"application":"live","name":"news"}.  Creating a stream does not
 * require a connected source, and creating one that already exists is not an
 * error: it returns the existing object, so a controller can replay desired
 * state after a restart without duplicating anything.
 */
static ngx_int_t
ngx_media_api_stream_create(ngx_http_request_t *r,
    ngx_media_registry_t *registry, u_char **last, u_char *end)
{
    ngx_str_t              body, application, name, media_text;
    ngx_media_stream_t    *stream;
    ngx_media_feed_conf_t  feed_conf;
    ngx_media_policy_t    *policy;
    ngx_uint_t             existed, media_set = 0;
    ngx_uint_t             media_mode = NGX_MEDIA_STREAM_MEDIA_SOURCE;
    ngx_int_t               rc;

    if (ngx_media_api_read_body(r, r->pool, &body) != NGX_OK) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"body_too_large\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_media_api_json_field(&body, "application", &application)
        != NGX_OK
        || ngx_media_api_json_field(&body, "name", &name) != NGX_OK
        || application.len == 0 || name.len == 0)
    {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"application_and_name_required\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_media_api_json_field(&body, "media", &media_text) == NGX_OK) {
        media_set = 1;

        if (ngx_media_api_media_mode(&media_text, &media_mode) != NGX_OK) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"unknown_media_mode\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        if (media_mode == NGX_MEDIA_STREAM_MEDIA_PROFILE) {
            policy = ngx_media_policy_get((ngx_cycle_t *) ngx_cycle);

            if (policy == NULL
                || policy->transform_executor.executable.len == 0)
            {
                *last = ngx_snprintf(*last, end - *last,
                                     "{\"error\":\"transform_unavailable\"}");
                return NGX_HTTP_BAD_REQUEST;
            }
        }
    }

    existed = (ngx_media_registry_stream(registry, &application, &name)
               != NULL);

    ngx_memzero(&feed_conf, sizeof(feed_conf));

    feed_conf.max_units = NGX_MEDIA_API_FEED_UNITS;
    feed_conf.max_bytes = NGX_MEDIA_API_FEED_BYTES;
    feed_conf.max_age = NGX_MEDIA_API_FEED_AGE;

    /*
     * The stream outlives this request, and so does everything the registry
     * builds from it: its pool, its sources and the readers those open.  The
     * log has to outlive the request too - the connection's dies with the
     * connection, and a reader logging through it afterwards reads freed
     * memory - so what is kept is the worker's.
     */
    stream = ngx_media_registry_stream_create(registry, &application, &name,
                                              &feed_conf,
                                              ((ngx_cycle_t *) ngx_cycle)->log);

    /*
     * The stream's owner is the deterministic slot for its hash, not this
     * worker: this request may have landed anywhere, and a program has one
     * driver on every worker's reckoning (ngx_media_route_owner).
     *
     * Publishing a record here is how the owner announces that it has the
     * stream; anywhere else it is a no-op, because the directory refuses a
     * record for a slot the hash does not name.  It used to name the worker
     * that answered the request instead, and that worker was then the owner
     * on every worker's reading - including the driver's, which is how an
     * accepting worker came to open a publisher's source locally while the
     * graph reported another worker as the owner.
     */
    if (stream != NULL) {
        ngx_media_runtime_claim(&application, &name);
    }

    if (stream == NULL) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"stream_create_failed\"}");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (media_set && stream->media_mode != media_mode) {
        stream->media_mode = media_mode;
        ngx_media_stream_touch(stream);
    } else if (!existed) {
        ngx_media_stream_touch(stream);
    }

    /*
     * Every worker gets the stream, so the next request for it - a read, or a
     * source added by an operator - is answered by whichever worker takes it
     * instead of coming back 404.  Re-sending an unchanged stream is not a
     * special case: the operation is idempotent, and it is how a controller
     * replays desired state to a worker whose replica is behind.
     */
    rc = ngx_media_graph_stream_set(stream);

    if (rc == NGX_ERROR) {
        if (!existed) {
            ngx_media_runtime_release(&application, &name);
            (void) ngx_media_registry_stream_destroy(registry, stream);
        }

        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"runtime_output_capacity\"}");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, "{\"application\":");

    if (ngx_media_api_json_string(last, end, &stream->application) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, ",\"name\":");

    if (ngx_media_api_json_string(last, end, &stream->name) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last,
                         ",\"media\":\"%s\",\"revision\":%uL,"
                         "\"incarnation\":%uL,\"created\":%s}",
                         stream->media_mode == NGX_MEDIA_STREAM_MEDIA_PROFILE
                         ? "profile" : "source",
                         stream->revision, stream->incarnation,
                         existed ? "false" : "true");

    return existed ? NGX_HTTP_OK : NGX_HTTP_CREATED;
}

/*
 * DELETE /media/api/v1/streams/{application}/{name}[?revision=N]
 *
 * Teardown is ordered and idempotent: deleting a stream that is not there
 * succeeds, because the caller asked for an end state and that end state
 * holds.
 */
static ngx_int_t
ngx_media_api_stream_delete(ngx_http_request_t *r,
    ngx_media_registry_t *registry, ngx_str_t *application, ngx_str_t *name,
    u_char **last, u_char *end)
{
    ngx_media_stream_t  *stream;
    ngx_str_t            revision_text;
    ngx_int_t            revision;
    ngx_uint_t           stated = 0;
    uint64_t             op_revision;

    stream = ngx_media_registry_stream(registry, application, name);

    if (ngx_media_api_arg(r, "revision", &revision_text) == NGX_OK) {
        revision = ngx_atoi(revision_text.data, revision_text.len);

        if (revision >= 0) {
            stated = 1;
        }

        if (stream != NULL && stated
            && (uint64_t) revision != stream->revision)
        {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"stale_revision\","
                                 "\"revision\":%uL}",
                                 stream->revision);
            return NGX_HTTP_CONFLICT;
        }
    }

    if (stream == NULL) {
        /*
         * Deleting what is not here still has to reach the workers that do
         * have it: a delete that lands on a worker which has not yet received
         * the stream's creation is the ordinary race between a controller's
         * two requests, and answering "absent" without telling anyone would
         * leave the stream running everywhere else.
         *
         * The operation carries the next revision of the shared sequence when
         * the caller stated none, which makes the delete the newest operation
         * there is: an operation that was in flight when it was issued is
         * older than it, so no replica resurrects the stream afterwards.  A
         * caller that stated a revision gets that number instead, and the
         * replicas that have moved past it keep their newer state.
         */
        op_revision = stated ? (uint64_t) revision
                             : ngx_media_revision_next(0);

        ngx_media_registry_tombstone(
            registry, ngx_media_owner_hash(application, name), op_revision);

        (void) ngx_media_graph_stream_delete(application, name, 0, op_revision);

        *last = ngx_snprintf(*last, end - *last, "{\"application\":");

        if (ngx_media_api_json_string(last, end, application) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last, ",\"name\":");

        if (ngx_media_api_json_string(last, end, name) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last,
                             ",\"deleted\":false,\"reason\":\"absent\","
                             "\"revision\":%uL}", op_revision);
        return NGX_HTTP_OK;
    }

    if (!stated) {
        /*
         * An unconditional delete is the newest operation: taking the next
         * revision makes it newer than the number this worker last knew, so a
         * replica that has moved past that number removes the stream too, and
         * the tombstone the delete leaves behind is above every operation that
         * raced it.
         */
        ngx_media_stream_touch(stream);
    }

    op_revision = stream->revision;

    ngx_media_runtime_release(&stream->application, &stream->name);

    /*
     * The replicas drop it too, before this worker's copy goes away: the
     * operation carries the revision the deletion moved past, so a replica
     * that has already moved past it keeps the newer object instead of
     * deleting it, and one that has not removes the stream.  Without this the
     * stream only disappears here.
     */
    (void) ngx_media_graph_stream_delete(&stream->application, &stream->name,
                                         stream->incarnation, op_revision);

    if (ngx_media_registry_stream_destroy(registry, stream) != NGX_OK) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"stream_delete_failed\"}");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, "{\"application\":");

    if (ngx_media_api_json_string(last, end, application) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, ",\"name\":");

    if (ngx_media_api_json_string(last, end, name) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last,
                         ",\"deleted\":true,\"revision\":%uL}", op_revision);

    return NGX_HTTP_OK;
}

/*
 * PATCH /media/api/v1/streams/{application}/{name}
 *
 * Body carries the fields to change plus, optionally, the revision the caller
 * last saw.  A revision that no longer matches is refused rather than
 * silently overwriting a newer desired state.
 */
static ngx_int_t
ngx_media_api_stream_patch(ngx_http_request_t *r,
    ngx_media_registry_t *registry, ngx_str_t *application, ngx_str_t *name,
    u_char **last, u_char *end)
{
    ngx_media_stream_t  *stream;
    ngx_str_t            body, value, media_text;
    ngx_media_policy_t  *policy;
    ngx_uint_t           media_mode;
    ngx_int_t             n;

    stream = ngx_media_registry_stream(registry, application, name);

    if (stream == NULL) {
        *last = ngx_snprintf(*last, end - *last, "{\"error\":\"no_stream\"}");
        return NGX_HTTP_NOT_FOUND;
    }

    if (ngx_media_api_read_body(r, r->pool, &body) != NGX_OK) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"body_too_large\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_media_api_json_field(&body, "revision", &value) == NGX_OK) {
        n = ngx_atoi(value.data, value.len);

        if (n >= 0 && (uint64_t) n != stream->revision) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"stale_revision\","
                                 "\"revision\":%uL}",
                                 stream->revision);
            return NGX_HTTP_CONFLICT;
        }
    }

    if (ngx_media_api_json_field(&body, "media", &media_text) == NGX_OK) {
        if (ngx_media_api_media_mode(&media_text, &media_mode) != NGX_OK) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"unknown_media_mode\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        if (media_mode == NGX_MEDIA_STREAM_MEDIA_PROFILE) {
            policy = ngx_media_policy_get((ngx_cycle_t *) ngx_cycle);

            if (policy == NULL
                || policy->transform_executor.executable.len == 0)
            {
                *last = ngx_snprintf(*last, end - *last,
                                     "{\"error\":\"transform_unavailable\"}");
                return NGX_HTTP_BAD_REQUEST;
            }
        }

        if (stream->media_mode != media_mode) {
            stream->media_mode = media_mode;
            ngx_media_stream_touch(stream);
        }
    }

    if (ngx_media_api_json_field(&body, "failure_timeout_ms", &value)
        == NGX_OK)
    {
        n = ngx_atoi(value.data, value.len);

        if (n < 1) {

            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"bad_failure_timeout\"}");
            return NGX_HTTP_BAD_REQUEST;
        }
        stream->selector.failure_timeout = (ngx_msec_t) n;
        ngx_media_stream_touch(stream);
    }

    if (ngx_media_api_json_field(&body, "recovery_timeout_ms", &value)
        == NGX_OK)
    {
        n = ngx_atoi(value.data, value.len);

        if (n < 1) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"bad_recovery_timeout\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        stream->selector.recovery_timeout = (ngx_msec_t) n;
        ngx_media_stream_touch(stream);
    }

    /* the desired-state change reaches the replicas, policy fields included */
    (void) ngx_media_graph_stream_set(stream);

    *last = ngx_snprintf(*last, end - *last, "{\"application\":");

    if (ngx_media_api_json_string(last, end, application) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, ",\"name\":");

    if (ngx_media_api_json_string(last, end, name) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last,
                         ",\"media\":\"%s\",\"revision\":%uL}",
                         stream->media_mode == NGX_MEDIA_STREAM_MEDIA_PROFILE
                         ? "profile" : "source",
                         stream->revision);

    return NGX_HTTP_OK;
}

/* --- source CRUD -------------------------------------------------------- */

static ngx_uint_t
ngx_media_api_source_type(const ngx_str_t *text)
{
    if (text->len == sizeof("srt") - 1
        && ngx_strncasecmp(text->data, (u_char *) "srt", 3) == 0)
    {
        return NGX_MEDIA_SOURCE_SRT;
    }

    if (text->len == sizeof("rtmp") - 1
        && ngx_strncasecmp(text->data, (u_char *) "rtmp", 4) == 0)
    {
        return NGX_MEDIA_SOURCE_RTMP;
    }

    if (text->len == sizeof("file") - 1
        && ngx_strncasecmp(text->data, (u_char *) "file", 4) == 0)
    {
        return NGX_MEDIA_SOURCE_FILE;
    }

    if (text->len == sizeof("hls_pull") - 1
        && ngx_strncasecmp(text->data, (u_char *) "hls_pull", 8) == 0)
    {
        return NGX_MEDIA_SOURCE_HLS_PULL;
    }

    if (text->len == sizeof("hls_push") - 1
        && ngx_strncasecmp(text->data, (u_char *) "hls_push", 8) == 0)
    {
        return NGX_MEDIA_SOURCE_HLS_PUSH;
    }

    return 0;
}

/*
 * POST /media/api/v1/streams/{application}/{name}/sources
 *
 * Body: {"id":"encoder-b","type":"srt","priority":50}.  A source is created
 * without a transport attached: the identity is a label the operator chooses,
 * and a publisher that presents it attaches to this object.  Creating one
 * that exists returns the existing object, so replay is safe.
 */
static ngx_int_t
ngx_media_api_source_create(ngx_http_request_t *r, ngx_media_stream_t *stream,
    u_char **last, u_char *end)
{
    ngx_str_t           body, id, type_text, priority_text;
    ngx_str_t           source_path = ngx_null_string;
    ngx_str_t           source_ca = ngx_null_string;
    ngx_media_source_t *source;
    ngx_uint_t          type = NGX_MEDIA_SOURCE_SRT, priority = 0;
    ngx_int_t           n;

    if (ngx_media_api_read_body(r, r->pool, &body) != NGX_OK) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"body_too_large\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_media_api_json_field(&body, "id", &id) != NGX_OK || id.len == 0) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"id_required\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_media_api_json_field(&body, "type", &type_text) == NGX_OK) {
        type = ngx_media_api_source_type(&type_text);

        if (type == 0) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"unknown_source_type\"}");
            return NGX_HTTP_BAD_REQUEST;
        }
    }

    if (ngx_media_api_json_field(&body, "priority", &priority_text)
        == NGX_OK)
    {
        n = ngx_atoi(priority_text.data, priority_text.len);

        if (n < 0) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"bad_priority\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        priority = (ngx_uint_t) n;
    }

    source = ngx_media_stream_source_find(stream, &id);
    if (source != NULL) {

        *last = ngx_snprintf(*last, end - *last, "{\"id\":");

        if (ngx_media_api_json_string(last, end, &source->id) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last,
                             ",\"revision\":%uL,\"created\":false}",
                             source->revision);
        return NGX_HTTP_OK;
    }

    /*
     * A file, a directory to watch and an origin to pull are each defined by a
     * path, and the path is what the owner opens - so it is required here
     * whatever worker this request landed on, even when this worker is not the
     * one that will open it.
     */
    if (type == NGX_MEDIA_SOURCE_FILE) {

        if (ngx_media_api_json_field(&body, "path", &source_path) != NGX_OK
            || source_path.len == 0)
        {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"path_required_for_file_source\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

    } else if (type == NGX_MEDIA_SOURCE_HLS_PUSH) {

        if (ngx_media_api_json_field(&body, "path", &source_path) != NGX_OK
            || source_path.len == 0)
        {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"path_required_for_hls_push\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

    } else if (type == NGX_MEDIA_SOURCE_HLS_PULL) {

        if (ngx_media_api_json_field(&body, "path", &source_path) != NGX_OK
            || source_path.len == 0)
        {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"path_required_for_hls_pull\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        if (ngx_media_api_json_field(&body, "ca_file", &source_ca) != NGX_OK) {
            ngx_str_null(&source_ca);
        }
    }

    /*
     * A file source has to be opened, not merely registered: it owns a reader
     * that the runtime tick advances.  Everything else is a label a transport
     * attaches to later - and a reader is opened on the worker that drives the
     * stream, with the other workers registering the desired state and letting
     * the owner open it, so one program reads a file once rather than once per
     * worker.
     */
    /* the reader this may open keeps the log, so it must outlive the request */
    source = ngx_media_graph_source_open(stream, &id, type, priority,
                                         &source_path, &source_ca,
                                         ((ngx_cycle_t *) ngx_cycle)->log);

    if (source == NULL) {

        /*
         * A reader that could not be opened is the caller's problem only where
         * this worker is the one that had to open it - a path that is not
         * there, an origin that will not answer.  Anywhere else the request is
         * refused for a reason only the owner can see, so the source is
         * registered and the owner reports what it could not open.
         */
        if (ngx_media_graph_owns(&stream->application, &stream->name)) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"source_open_failed\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"source_create_failed\"}");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_media_source_touch(source);
    ngx_media_stream_touch(stream);

    /* the source, its path and its desired state reach every other worker */
    (void) ngx_media_graph_source_set(stream, source, &source_path,
                                      &source_ca);

    *last = ngx_snprintf(*last, end - *last, "{\"id\":");

    if (ngx_media_api_json_string(last, end, &source->id) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last,
                         ",\"revision\":%uL,\"created\":true}",
                         source->revision);

    return NGX_HTTP_CREATED;
}

/*
 * DELETE /media/api/v1/streams/{application}/{name}/sources/{source}
 *
 * Removing the active source is a normal operation: the selector fails over
 * through the same path a failure would take, which is what the acceptance
 * contract asks for.  Idempotent, like stream deletion.
 */
static ngx_int_t
ngx_media_api_source_delete(ngx_media_stream_t *stream, ngx_str_t *source_id,
    u_char **last, u_char *end)
{
    ngx_media_source_t  *source;

    source = ngx_media_stream_source_find(stream, source_id);

    if (source == NULL) {
        *last = ngx_snprintf(*last, end - *last, "{\"id\":");

        if (ngx_media_api_json_string(last, end, source_id) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last,
                             ",\"deleted\":false,\"reason\":\"absent\"}");
        return NGX_HTTP_OK;
    }


    (void) ngx_media_graph_source_delete(stream, source_id, stream->revision);

    *last = ngx_snprintf(*last, end - *last, "{\"id\":");

    if (ngx_media_api_json_string(last, end, source_id) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, ",\"deleted\":true}");

    return NGX_HTTP_OK;
}

/*
 * POST .../sources/{source}/enable and /disable
 *
 * Desired state, so it is accepted whatever the transport is doing: disabling
 * a source takes it out of selection without tearing its session down, which
 * is what an operator wants when they intend to bring it back.
 */
static ngx_int_t
ngx_media_api_source_set_enabled(ngx_media_stream_t *stream,
    ngx_str_t *source_id, ngx_uint_t enabled, u_char **last, u_char *end)
{
    ngx_media_source_t  *source;

    source = ngx_media_stream_source_find(stream, source_id);

    if (source == NULL) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"source_not_found\"}");
        return NGX_HTTP_NOT_FOUND;
    }

    source->enabled = enabled ? 1 : 0;

    ngx_media_source_touch(source);
    ngx_media_stream_touch(stream);

    /*
     * Disabling is desired state, so it is replicated like any other: the
     * replica answers with the same desired state, and the owner is the one
     * that takes it out of selection.  No path travels here - the reader, if
     * this source has one, already exists on the owner.
     */
    (void) ngx_media_graph_source_set(stream, source, NULL, NULL);

    *last = ngx_snprintf(*last, end - *last, "{\"id\":");

    if (ngx_media_api_json_string(last, end, &source->id) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last,
                         ",\"enabled\":%s,\"revision\":%uL}",
                         enabled ? "true" : "false", source->revision);

    return NGX_HTTP_OK;
}

/* the action after the stream name: sources, sources/{id}[/enable|disable] */
static ngx_int_t
ngx_media_api_sources(ngx_http_request_t *r, ngx_media_stream_t *stream,
    ngx_str_t *action, u_char **last, u_char *end)
{
    ngx_str_t  rest, source_id, verb;
    u_char    *slash;

    if (action->len < sizeof("sources") - 1
        || ngx_strncmp(action->data, "sources", sizeof("sources") - 1) != 0)
    {
        return NGX_DECLINED;
    }

    rest.data = action->data + sizeof("sources") - 1;
    rest.len = action->len - (sizeof("sources") - 1);

    if (rest.len == 0) {
        if (r->method == NGX_HTTP_POST) {
            return ngx_media_api_source_create(r, stream, last, end);
        }

        return NGX_DECLINED;
    }

    if (rest.data[0] != '/') {
        return NGX_DECLINED;
    }

    rest.data++;
    rest.len--;

    slash = ngx_strlchr(rest.data, rest.data + rest.len, '/');

    if (slash == NULL) {
        source_id = rest;
        verb.len = 0;

    } else {
        source_id.data = rest.data;
        source_id.len = slash - rest.data;
        verb.data = slash + 1;
        verb.len = rest.len - source_id.len - 1;
    }

    if (source_id.len == 0) {
        return NGX_DECLINED;
    }

    if (verb.len == 0) {
        if (r->method == NGX_HTTP_DELETE) {
            return ngx_media_api_source_delete(stream, &source_id, last, end);
        }

        if (r->method == NGX_HTTP_GET) {
            ngx_media_source_t  *source;

            source = ngx_media_stream_source_find(stream, &source_id);

            if (source == NULL) {
                *last = ngx_snprintf(*last, end - *last,
                                     "{\"error\":\"source_not_found\"}");
                return NGX_HTTP_NOT_FOUND;
            }

            *last = ngx_snprintf(*last, end - *last, "{\"id\":");

            if (ngx_media_api_json_string(last, end, &source->id) != NGX_OK) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            *last = ngx_snprintf(*last, end - *last,
                                 ",\"type\":%ui,\"priority\":%ui,"
                                 "\"enabled\":%s,\"state\":\"%s\","
                                 "\"revision\":%uL}",
                                 source->type, source->priority,
                                 source->enabled ? "true" : "false",
                                 ngx_media_api_state_name(source->state),
                                 source->revision);

            return NGX_HTTP_OK;
        }

        return NGX_DECLINED;
    }

    if (r->method != NGX_HTTP_POST) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    if (verb.len == sizeof("enable") - 1
        && ngx_strncmp(verb.data, "enable", sizeof("enable") - 1) == 0)
    {
        return ngx_media_api_source_set_enabled(stream, &source_id, 1, last,
                                                end);
    }

    if (verb.len == sizeof("disable") - 1
        && ngx_strncmp(verb.data, "disable", sizeof("disable") - 1) == 0)
    {
        return ngx_media_api_source_set_enabled(stream, &source_id, 0, last,
                                                end);
    }

    return NGX_DECLINED;
}

/* --- destination CRUD --------------------------------------------------- */

static ngx_uint_t
ngx_media_api_dest_type(const ngx_str_t *text)
{
    if (text->len == sizeof("srt") - 1
        && ngx_strncasecmp(text->data, (u_char *) "srt", 3) == 0)
    {
        return NGX_MEDIA_DEST_SRT;
    }

    if (text->len == sizeof("rtmp") - 1
        && ngx_strncasecmp(text->data, (u_char *) "rtmp", 4) == 0)
    {
        return NGX_MEDIA_DEST_RTMP;
    }

    if (text->len == sizeof("hls_push") - 1
        && ngx_strncasecmp(text->data, (u_char *) "hls_push", 8) == 0)
    {
        return NGX_MEDIA_DEST_HLS_PUSH;
    }

    if (text->len == sizeof("record") - 1
        && ngx_strncasecmp(text->data, (u_char *) "record", 6) == 0)
    {
        return NGX_MEDIA_DEST_RECORD;
    }

    return 0;
}

/*
 * An endpoint may carry a credential in its URL - YouTube's HLS ingest does -
 * so a read of the destination reports the endpoint with the credential
 * removed.  The origin stays, which is what an operator actually needs to see;
 * the key does not.  A value that is not URL-shaped passes through unchanged.
 */
static ngx_str_t
ngx_media_api_safe_host(ngx_media_destination_t *destination, u_char *buf,
    size_t cap)
{
    ngx_str_t  out;

    ngx_media_redact_url(&destination->host, buf, cap, &out);

    return out;
}

static ngx_int_t
ngx_media_api_destination_json(ngx_media_destination_t *destination,
    u_char **last, u_char *end, ngx_int_t created)
{
    u_char     safe[512];
    ngx_str_t  host;

    host = ngx_media_api_safe_host(destination, safe, sizeof(safe));

    *last = ngx_snprintf(*last, end - *last, "{\"id\":");

    if (ngx_media_api_json_string(last, end, &destination->id) != NGX_OK) {
        return NGX_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, ",\"type\":%ui,\"host\":",
                         destination->type);

    if (ngx_media_api_json_string(last, end, &host) != NGX_OK) {
        return NGX_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last,
                         ",\"port\":%ui,\"enabled\":%s,\"revision\":%uL",
                         destination->port,
                         destination->enabled ? "true" : "false",
                         destination->revision);

    if (created >= 0) {
        *last = ngx_snprintf(*last, end - *last, ",\"created\":%s",
                             created ? "true" : "false");
    }

    *last = ngx_snprintf(*last, end - *last, "}");

    return (*last < end - 1) ? NGX_OK : NGX_ERROR;
}

/*
 * POST /media/api/v1/streams/{application}/{name}/destinations
 *
 * Body: {"id":"out1","type":"srt","host":"127.0.0.1","port":9100}.  A
 * destination starts as soon as it is created, so adding one while the
 * program is live is the normal case.  Replaying the request returns the
 * existing object.
 */
static ngx_int_t
ngx_media_api_destination_create(ngx_http_request_t *r,
    ngx_media_stream_t *stream, u_char **last, u_char *end)
{
    ngx_str_t                body, id, type_text, host, port_text, streamid;
    ngx_str_t                profile_name;
    ngx_media_destination_t *destination;
    ngx_uint_t               type = 0, port = 0;
    ngx_int_t                n;
    ngx_str_t               *copy;

    if (ngx_media_api_read_body(r, r->pool, &body) != NGX_OK) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"body_too_large\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_media_api_json_field(&body, "id", &id) != NGX_OK || id.len == 0) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"id_required\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_media_api_json_field(&body, "type", &type_text) != NGX_OK) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"type_required\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    type = ngx_media_api_dest_type(&type_text);

    if (type == 0) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"unknown_destination_type\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_media_api_json_field(&body, "host", &host) != NGX_OK
        || host.len == 0)
    {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"host_required\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    /*
     * A port belongs to a socket destination.  An HLS push destination is
     * given an endpoint URL in host and a watched directory in path, so a
     * port is neither required nor meaningful there.
     */
    if (ngx_media_api_json_field(&body, "port", &port_text) == NGX_OK) {
        n = ngx_atoi(port_text.data, port_text.len);

        if (n < 1 || n > 65535) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"bad_port\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        port = (ngx_uint_t) n;

    } else if (type != NGX_MEDIA_DEST_HLS_PUSH) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"port_required\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    destination = ngx_media_destination_find(stream, &id);

    if (destination != NULL) {
        if (ngx_media_api_destination_json(destination, last, end, 0)
            != NGX_OK)
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        return NGX_HTTP_OK;
    }

    destination = ngx_media_destination_add(stream, &id, type,
                                            r->connection->log);

    if (destination == NULL) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"destination_create_failed\"}");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    copy = ngx_media_destination_strdup(stream->pool, &host);
    if (copy == NULL) {
        ngx_media_destination_remove(stream, destination);
        *last = ngx_snprintf(*last, end - *last, "{\"error\":\"out_of_memory\"}");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    destination->host = *copy;
    destination->port = port;

    if (ngx_media_api_json_field(&body, "streamid", &streamid) == NGX_OK) {
        copy = ngx_media_destination_strdup(stream->pool, &streamid);

        if (copy != NULL) {
            destination->streamid = *copy;
        }
    }

    if (ngx_media_api_json_field(&body, "path", &streamid) == NGX_OK) {
        copy = ngx_media_destination_strdup(stream->pool, &streamid);

        if (copy != NULL) {
            destination->path = *copy;
        }
    }

    if (ngx_media_api_json_field(&body, "ca_file", &streamid) == NGX_OK) {
        copy = ngx_media_destination_strdup(stream->pool, &streamid);

        if (copy != NULL) {
            destination->ca_file = *copy;
        }
    }

    /*
     * A platform profile validates the destination and fills its defaults
     * before anything is started: a configuration the platform would reject
     * should fail here, not on the wire at three in the morning.
     */
    ngx_str_null(&profile_name);

    if (ngx_media_api_json_field(&body, "profile", &profile_name) == NGX_OK
        && profile_name.len > 0)
    {
        const ngx_media_hls_profile_t  *profile;
        const char                     *why;
        ngx_uint_t                      duration = 0, window = 0, post = 0;

        profile = ngx_media_hls_profile_find(&profile_name);

        if (profile == NULL) {
            /*
             * The object is already linked into the stream by now, so an
             * error return has to take it back out: otherwise a 400 leaves a
             * destination that is listed, that the replay contract treats as
             * created, and that can never carry media because its impl is
             * NULL.
             */
            ngx_media_destination_remove(stream, destination);

            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"unknown_profile\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        if (ngx_media_api_json_field(&body, "segment_duration_ms", &streamid)
            == NGX_OK)
        {
            n = ngx_atoi(streamid.data, streamid.len);

            if (n > 0) {
                duration = (ngx_uint_t) n;
            }
        }

        if (ngx_media_api_json_field(&body, "playlist_window", &streamid)
            == NGX_OK)
        {
            n = ngx_atoi(streamid.data, streamid.len);

            if (n > 0) {
                window = (ngx_uint_t) n;
            }
        }

        if (ngx_media_hls_profile_apply(profile, &host, &duration, &window,
                                        &post, &why) != NGX_OK)
        {
            ngx_media_destination_remove(stream, destination);

            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"profile_violation\","
                                 "\"detail\":\"%s\"}", why);
            return NGX_HTTP_BAD_REQUEST;
        }

        destination->segment_duration_ms = duration;
        destination->playlist_window = window;
        destination->http_post = post;

        copy = ngx_media_destination_strdup(stream->pool, &profile_name);

        if (copy != NULL) {
            destination->profile = *copy;
        }
    }

    if (ngx_media_destination_start(stream, destination,
                                    r->connection->log) != NGX_OK)
    {
        ngx_media_destination_remove(stream, destination);

        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"destination_start_failed\"}");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_media_destination_touch(destination);

    if (ngx_media_api_destination_json(destination, last, end, 1)
        != NGX_OK)
    {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    return NGX_HTTP_CREATED;
}

static ngx_int_t
ngx_media_api_destination_delete(ngx_media_stream_t *stream,
    ngx_str_t *id, u_char **last, u_char *end)
{
    ngx_media_destination_t  *destination;

    destination = ngx_media_destination_find(stream, id);

    if (destination == NULL) {
        *last = ngx_snprintf(*last, end - *last, "{\"id\":");

        if (ngx_media_api_json_string(last, end, id) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last,
                             ",\"deleted\":false,\"reason\":\"absent\"}");
        return NGX_HTTP_OK;
    }

    ngx_media_destination_remove(stream, destination);

    *last = ngx_snprintf(*last, end - *last, "{\"id\":");

    if (ngx_media_api_json_string(last, end, id) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last, ",\"deleted\":true}");

    return NGX_HTTP_OK;
}

static ngx_int_t
ngx_media_api_destinations(ngx_http_request_t *r, ngx_media_stream_t *stream,
    ngx_str_t *action, u_char **last, u_char *end)
{
    ngx_str_t                rest, id;
    ngx_queue_t             *q;
    ngx_media_destination_t *destination;
    ngx_uint_t               first = 1;

    if (action->len < sizeof("destinations") - 1
        || ngx_strncmp(action->data, "destinations",
                       sizeof("destinations") - 1) != 0)
    {
        return NGX_DECLINED;
    }

    /*
     * A destination is an output, not graph state a replica can act on: it is
     * started where the program runs, and a sender or an uploader started on a
     * worker that does not drive the stream would never be handed anything to
     * carry.  So the mutations are answered by the owner, with the owner
     * named, rather than accepted here and silently pointless; the reads are
     * not - a read reports the graph.
     */
    if (r->method != NGX_HTTP_GET
        && ngx_media_api_not_owner(stream, last, end))
    {
        return NGX_HTTP_CONFLICT;
    }

    rest.data = action->data + sizeof("destinations") - 1;
    rest.len = action->len - (sizeof("destinations") - 1);

    if (rest.len == 0) {

        if (r->method == NGX_HTTP_POST) {
            return ngx_media_api_destination_create(r, stream, last, end);
        }

        if (r->method != NGX_HTTP_GET) {
            return NGX_DECLINED;
        }

        *last = ngx_snprintf(*last, end - *last, "{\"destinations\":[");

        for (q = ngx_queue_head(&stream->destinations);
             q != (ngx_queue_t *) &stream->destinations;
             q = q->next)
        {
            destination = ngx_queue_data(q, ngx_media_destination_t, queue);

            *last = ngx_snprintf(*last, end - *last, "%s",
                                 first ? "" : ",");
            first = 0;

            if (ngx_media_api_destination_json(destination, last, end, -1)
                != NGX_OK)
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
        }

        *last = ngx_snprintf(*last, end - *last, "],\"count\":%ui}",
                             ngx_media_destination_count(stream));

        if (*last >= end - 1) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        return NGX_HTTP_OK;
    }

    if (rest.data[0] != '/') {
        return NGX_DECLINED;
    }

    id.data = rest.data + 1;
    id.len = rest.len - 1;

    if (id.len == 0) {
        return NGX_DECLINED;
    }

    destination = ngx_media_destination_find(stream, &id);

    /* DELETE is answered even when the object is gone, as for streams */
    if (destination == NULL && r->method != NGX_HTTP_DELETE) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"destination_not_found\"}");
        return NGX_HTTP_NOT_FOUND;
    }

    if (r->method == NGX_HTTP_DELETE) {
        return ngx_media_api_destination_delete(stream, &id, last, end);
    }

    if (r->method == NGX_HTTP_GET) {
        if (ngx_media_api_destination_json(destination, last, end, -1)
            != NGX_OK)
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        return NGX_HTTP_OK;
    }

    return NGX_DECLINED;
}

/* --- desired state ------------------------------------------------------ */

/*
 * GET /media/api/v1/desired
 *
 * The whole graph as one document, in the same shape PUT accepts.  This is
 * the replay contract the normative revision allows instead of a database: a
 * controller keeps this document and re-applies it after a restart, and
 * because every create is idempotent, replay cannot duplicate anything.
 */
static ngx_int_t
ngx_media_api_desired_get(ngx_media_registry_t *registry, u_char **last,
    u_char *end)
{
    ngx_queue_t                 *q, *sq;
    ngx_media_registry_entry_t  *entry;
    ngx_media_stream_t          *stream;
    ngx_media_source_t          *source;
    ngx_media_destination_t     *destination;
    ngx_uint_t                   first = 1;

    *last = ngx_snprintf(*last, end - *last, "{\"streams\":[");

    for (q = ngx_queue_head(&registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = q->next)
    {
        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);
        stream = &entry->stream;

        *last = ngx_snprintf(*last, end - *last, "%s{\"application\":",
                             first ? "" : ",");

        if (ngx_media_api_json_string(last, end, &stream->application)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last, ",\"name\":");

        if (ngx_media_api_json_string(last, end, &stream->name) != NGX_OK) {
            return NGX_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last,
                             ",\"revision\":%uL,\"media\":\"%s\","
                             "\"sources\":[",
                             stream->revision,
                             stream->media_mode == NGX_MEDIA_STREAM_MEDIA_PROFILE
                             ? "profile" : "source");
        first = 0;

        {
            ngx_uint_t  sfirst = 1;

            for (sq = ngx_queue_head(&stream->sources);
                 sq != (ngx_queue_t *) &stream->sources;
                 sq = sq->next)
            {
                source = ngx_queue_data(sq, ngx_media_source_t, queue);

                *last = ngx_snprintf(*last, end - *last,
                                     "%s{\"id\":", sfirst ? "" : ",");

                if (ngx_media_api_json_string(last, end, &source->id)
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }

                *last = ngx_snprintf(*last, end - *last,
                                     ",\"type\":%ui,\"priority\":%ui,"
                                     "\"path\":",
                                     source->type, source->priority);

                if (ngx_media_api_json_string(last, end, &source->path)
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }

                if (source->ca_file.len != 0) {
                    *last = ngx_snprintf(*last, end - *last,
                                         ",\"ca_file\":");

                    if (ngx_media_api_json_string(last, end,
                                                  &source->ca_file) != NGX_OK)
                    {
                        return NGX_ERROR;
                    }
                }

                *last = ngx_snprintf(*last, end - *last,
                                     ",\"enabled\":%s,\"revision\":%uL}",
                                     source->enabled ? "true" : "false",
                                     source->revision);
                sfirst = 0;
            }
        }

        *last = ngx_snprintf(*last, end - *last, "],\"destinations\":[");

        {
            ngx_uint_t  dfirst = 1;
            u_char      safe[512];
            ngx_str_t   host;

            for (sq = ngx_queue_head(&stream->destinations);
                 sq != (ngx_queue_t *) &stream->destinations;
                 sq = sq->next)
            {
                destination = ngx_queue_data(sq, ngx_media_destination_t,
                                             queue);
                host = ngx_media_api_safe_host(destination, safe,
                                               sizeof(safe));

                *last = ngx_snprintf(*last, end - *last,
                                     "%s{\"id\":", dfirst ? "" : ",");

                if (ngx_media_api_json_string(last, end, &destination->id)
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }

                *last = ngx_snprintf(*last, end - *last,
                                     ",\"type\":%ui,\"host\":",
                                     destination->type);

                if (ngx_media_api_json_string(last, end, &host) != NGX_OK) {
                    return NGX_ERROR;
                }

                *last = ngx_snprintf(*last, end - *last,
                                     ",\"port\":%ui,\"enabled\":%s,"
                                     "\"revision\":%uL}",
                                     destination->port,
                                     destination->enabled ? "true" : "false",
                                     destination->revision);
                dfirst = 0;
            }
        }

        *last = ngx_snprintf(*last, end - *last,
                             "],\"fanout_ms\":{\"p50\":%M,\"p95\":%M,"
                             "\"p99\":%M,\"max\":%uL},\"dispatched\":%uL}",
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 500),
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 950),
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 990),
                             ngx_media_feed_fanout_max(&stream->program_feed),
                             ngx_media_feed_fanout_count(
                                 &stream->program_feed));

        if (*last >= end - 1) {
            return NGX_ERROR;
        }
    }

    *last = ngx_snprintf(*last, end - *last, "],\"count\":%ui}",
                         ngx_media_registry_count(registry));

    return (*last < end - 1) ? NGX_OK : NGX_ERROR;
}

/*
 * Applies one stream's sources and destinations from a desired document.
 * Existing children are left exactly as they are, which is what makes a
 * replay idempotent: the controller's document is the same, so the result
 * must be too.
 */
/*
 * A type in a desired document may be written as a name or as the number the
 * read side emits, so a document round-trips through GET and PUT unchanged.
 */
static ngx_uint_t
ngx_media_api_source_type_value(const ngx_str_t *text)
{
    ngx_uint_t  named = ngx_media_api_source_type(text);
    ngx_int_t   n;

    if (named != 0) {
        return named;
    }

    n = ngx_atoi(text->data, text->len);

    if (n >= NGX_MEDIA_SOURCE_SRT && n <= NGX_MEDIA_SOURCE_HLS_PUSH) {
        return (ngx_uint_t) n;
    }

    return 0;
}

static ngx_uint_t
ngx_media_api_dest_type_value(const ngx_str_t *text)
{
    ngx_uint_t  named = ngx_media_api_dest_type(text);
    ngx_int_t   n;

    if (named != 0) {
        return named;
    }

    n = ngx_atoi(text->data, text->len);

    if (n >= NGX_MEDIA_DEST_SRT && n <= NGX_MEDIA_DEST_RECORD) {
        return (ngx_uint_t) n;
    }

    return 0;
}

static ngx_int_t
ngx_media_api_desired_children(ngx_http_request_t *r,
    ngx_media_stream_t *stream, ngx_str_t *item, ngx_uint_t *created)
{
    ngx_str_t              array, child, value, id;
    ngx_str_t              source_path, source_ca;
    ngx_media_source_t    *source;
    ngx_media_destination_t  *destination;
    ngx_uint_t              type, priority, port;
    u_char                 *p, *stop;
    ngx_str_t              *copy;
    ngx_int_t                n;

    if (ngx_media_api_json_array(item, "sources", &array) == NGX_OK) {

        p = array.data + 1;
        stop = array.data + array.len - 1;

        while (p < stop) {

            if (*p == '{') {
                u_char     *close = p;
                ngx_int_t   depth = 0;

                for (; close < stop; close++) {

                    if (*close == '{') {
                        depth++;

                    } else if (*close == '}') {
                        depth--;

                        if (depth == 0) {
                            break;
                        }
                    }
                }

                if (close == stop) {
                    return NGX_ERROR;
                }

                child.data = p;
                child.len = close - p + 1;
                p = close + 1;

            } else {
                p++;
                continue;
            }

            if (ngx_media_api_json_field(&child, "id", &value) != NGX_OK
                || value.len == 0)
            {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "media: desired: source without an id");
                return NGX_ERROR;
            }

            id = value;

            type = NGX_MEDIA_SOURCE_SRT;

            if (ngx_media_api_json_field(&child, "type", &value) == NGX_OK) {
                type = ngx_media_api_source_type_value(&value);

                if (type == 0) {
                    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                                  "media: desired: unknown source type \"%V\"",
                                  &value);
                    return NGX_ERROR;
                }
            }

            ngx_str_null(&source_path);
            ngx_str_null(&source_ca);

            if (ngx_media_api_json_field(&child, "path", &value) == NGX_OK) {
                source_path = value;
            }

            if (ngx_media_api_json_field(&child, "ca_file", &value)
                == NGX_OK)
            {
                source_ca = value;
            }

            if (source_path.len > NGX_MEDIA_GRAPH_MAX_PATH
                || source_ca.len > NGX_MEDIA_GRAPH_MAX_PATH)
            {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "media: desired: source path is too long");
                return NGX_ERROR;
            }

            if ((type == NGX_MEDIA_SOURCE_FILE
                 || type == NGX_MEDIA_SOURCE_HLS_PUSH
                 || type == NGX_MEDIA_SOURCE_HLS_PULL)
                && source_path.len == 0)
            {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "media: desired: source %V needs a path",
                              &id);
                return NGX_ERROR;
            }

            source = ngx_media_stream_source_find(stream, &id);

            if (source != NULL) {
                if (source_path.len != 0 || source_ca.len != 0) {
                    (void) ngx_media_graph_source_set(stream, source,
                                                      &source_path,
                                                      &source_ca);
                }

                continue;
            }

            priority = 0;

            if (ngx_media_api_json_field(&child, "priority", &value)
                == NGX_OK)
            {
                n = ngx_atoi(value.data, value.len);

                if (n > 0) {
                    priority = (ngx_uint_t) n;
                }
            }

            source = ngx_media_graph_source_open(stream, &id, type, priority,
                                                 &source_path, &source_ca,
                                                 r->connection->log);

            if (source == NULL) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "media: desired: source %V could not be opened",
                              &id);
                return NGX_ERROR;
            }

            ngx_media_source_touch(source);
            (*created)++;

            /*
             * The owner materialises path-bearing sources; replicas retain
             * the same desired path and wait for their owner.  SRT sources
             * still remain labels until a publisher attaches.
             */
            (void) ngx_media_graph_source_set(stream, source, &source_path,
                                              &source_ca);
        }
    }

    if (ngx_media_api_json_array(item, "destinations", &array) == NGX_OK) {

        p = array.data + 1;
        stop = array.data + array.len - 1;

        while (p < stop) {

            if (*p == '{') {
                u_char     *close = p;
                ngx_int_t   depth = 0;

                for (; close < stop; close++) {

                    if (*close == '{') {
                        depth++;

                    } else if (*close == '}') {
                        depth--;

                        if (depth == 0) {
                            break;
                        }
                    }
                }

                if (close == stop) {
                    return NGX_ERROR;
                }

                child.data = p;
                child.len = close - p + 1;
                p = close + 1;

            } else {
                p++;
                continue;
            }

            if (ngx_media_api_json_field(&child, "id", &value) != NGX_OK
                || value.len == 0)
            {
                return NGX_ERROR;
            }

            if (ngx_media_destination_find(stream, &value) != NULL) {
                continue;
            }

            /*
             * A destination is started where the program runs, and the graph
             * replica does not carry outputs: applying one here when this
             * worker does not drive the stream would start a sender or an
             * uploader that is never handed media.  The document is refused
             * instead, naming the owner, so the controller can apply it there
             * - every create in it is idempotent, so the streams and sources
             * already applied are not applied twice.
             */
            if (!ngx_media_graph_owns(&stream->application, &stream->name)) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "media: desired: destination %V belongs to "
                              "%V/%V, which worker %ui does not drive",
                              &value, &stream->application, &stream->name,
                              (ngx_uint_t) ngx_worker);
                return NGX_DECLINED;
            }

            type = 0;

            if (ngx_media_api_json_field(&child, "type", &value) == NGX_OK) {
                type = ngx_media_api_dest_type_value(&value);
            }

            if (type == 0) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "media: desired: unknown destination type \"%V\"",
                              &value);
                return NGX_ERROR;
            }

            if (ngx_media_api_json_field(&child, "id", &value) != NGX_OK) {
                return NGX_ERROR;
            }

            destination = ngx_media_destination_add(stream, &value, type,
                                                    r->connection->log);

            if (destination == NULL) {
                return NGX_ERROR;
            }

            if (ngx_media_api_json_field(&child, "host", &value) == NGX_OK) {
                copy = ngx_media_destination_strdup(stream->pool, &value);

                if (copy == NULL) {
                    return NGX_ERROR;
                }

                destination->host = *copy;
            }

            if (ngx_media_api_json_field(&child, "port", &value) == NGX_OK) {
                n = ngx_atoi(value.data, value.len);

                if (n > 0 && n <= 65535) {
                    port = (ngx_uint_t) n;
                    destination->port = port;
                }
            }

            if (ngx_media_destination_start(stream, destination,
                                            r->connection->log) != NGX_OK)
            {
                return NGX_ERROR;
            }

            ngx_media_destination_touch(destination);
            (*created)++;
        }
    }

    return NGX_OK;
}

/*
 * The array value of "key".  ngx_media_api_json_field() reads a scalar and
 * stops at the first comma, which silently truncates an array of objects, so
 * arrays get their own reader that respects nesting.
 */
static ngx_int_t
ngx_media_api_json_array(const ngx_str_t *body, const char *key,
    ngx_str_t *out)
{
    u_char  *p, *end, *start;
    size_t   key_len = strlen(key);
    ngx_int_t depth = 0;

    if (body->data == NULL) {
        return NGX_DECLINED;
    }

    p = body->data;
    end = body->data + body->len;

    while (p < end) {

        if (*p != '"' || (size_t) (end - p) < key_len + 3
            || ngx_memcmp(p + 1, key, key_len) != 0
            || p[1 + key_len] != '"')
        {
            p++;
            continue;
        }

        p += key_len + 2;

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        if (p == end || *p != ':') {
            continue;
        }

        p++;

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        if (p == end || *p != '[') {
            return NGX_DECLINED;
        }

        start = p;

        for (; p < end; p++) {

            if (*p == '[') {
                depth++;

            } else if (*p == ']') {
                depth--;

                if (depth == 0) {
                    out->data = start;
                    out->len = p - start + 1;
                    return NGX_OK;
                }
            }
        }

        return NGX_DECLINED;
    }

    return NGX_DECLINED;
}

/* the "streams" array of a desired document */
static ngx_int_t
ngx_media_api_desired_streams(const ngx_str_t *body, ngx_str_t *out)
{
    return ngx_media_api_json_array(body, "streams", out);
}

/*
 * PUT /media/api/v1/desired
 *
 * Applies a desired document.  Everything here is create-or-update, so a
 * controller may replay the same document as often as it likes: streams that
 * exist are reused, sources and destinations that exist are left alone.  A
 * stream that is absent from the document is *not* deleted - pruning is the
 * controller's decision, made with the delete calls, not a side effect of a
 * replay.
 */
static ngx_int_t
ngx_media_api_desired_put(ngx_http_request_t *r,
    ngx_media_registry_t *registry, u_char **last, u_char *end)
{
    ngx_str_t              body, streams, item, application, name, media_text;
    ngx_media_stream_t    *stream;
    ngx_media_feed_conf_t  feed_conf;
    ngx_media_policy_t    *policy;
    u_char                *p, *stop;
    ngx_uint_t             applied = 0, created = 0, stream_existed;
    ngx_uint_t             media_mode;
    ngx_int_t               rc;

    if (ngx_media_api_read_body(r, r->pool, &body) != NGX_OK) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"body_too_large\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_media_api_desired_streams(&body, &streams) != NGX_OK) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"streams_array_required\"}");
        return NGX_HTTP_BAD_REQUEST;
    }

    ngx_memzero(&feed_conf, sizeof(feed_conf));

    feed_conf.max_units = NGX_MEDIA_API_FEED_UNITS;
    feed_conf.max_bytes = NGX_MEDIA_API_FEED_BYTES;
    feed_conf.max_age = NGX_MEDIA_API_FEED_AGE;

    /*
     * The array is walked one object at a time.  This is deliberately not a
     * general JSON parser: the document shape is fixed and flat per object,
     * and anything else is rejected rather than half-understood.
     */
    p = streams.data + 1;
    stop = streams.data + streams.len - 1;

    while (p < stop) {

        if (*p != '{') {
            p++;
            continue;
        }

        {
            u_char  *close = p;
            ngx_int_t  depth = 0;

            for (; close < stop; close++) {

                if (*close == '{') {
                    depth++;

                } else if (*close == '}') {
                    depth--;

                    if (depth == 0) {
                        break;
                    }
                }
            }

            if (close == stop) {
                break;
            }

            item.data = p;
            item.len = close - p + 1;
            p = close + 1;
        }

        if (ngx_media_api_json_field(&item, "application", &application)
            != NGX_OK
            || ngx_media_api_json_field(&item, "name", &name) != NGX_OK
            || application.len == 0 || name.len == 0)
        {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"stream_needs_application_and_name\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        if (ngx_media_api_json_field(&item, "media", &media_text)
            == NGX_OK)
        {
            if (ngx_media_api_media_mode(&media_text, &media_mode)
                != NGX_OK)
            {
                *last = ngx_snprintf(*last, end - *last,
                                     "{\"error\":\"unknown_media_mode\"}");
                return NGX_HTTP_BAD_REQUEST;
            }

            if (media_mode == NGX_MEDIA_STREAM_MEDIA_PROFILE) {
                policy = ngx_media_policy_get((ngx_cycle_t *) ngx_cycle);

                if (policy == NULL
                    || policy->transform_executor.executable.len == 0)
                {
                    *last = ngx_snprintf(*last, end - *last,
                                         "{\"error\":\"transform_unavailable\"}");
                    return NGX_HTTP_BAD_REQUEST;
                }
            }

        }
        stream_existed =
            (ngx_media_registry_stream(registry, &application, &name) != NULL);

        stream = ngx_media_registry_stream_create(registry, &application,
                                                  &name, &feed_conf,
                                                  ((ngx_cycle_t *) ngx_cycle)
                                                      ->log);

        if (stream == NULL) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"stream_create_failed\"}");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        applied++;
        if (ngx_media_api_json_field(&item, "media", &media_text)
            == NGX_OK && stream->media_mode != media_mode)
        {
            stream->media_mode = media_mode;
            ngx_media_stream_touch(stream);
        }


        rc = ngx_media_api_desired_children(r, stream, &item, &created);

        if (rc == NGX_DECLINED) {
            /*
             * The document asks for an output that only the stream's owner can
             * start; the rest of it was applied, and applying it again there
             * is idempotent.
             */
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"destination_needs_owner\","
                                 "\"owner\":%ui,\"stream\":",
                                 ngx_media_route_owner(
                                     (ngx_cycle_t *) ngx_cycle,
                                     ngx_media_owner_hash(
                                         &stream->application, &stream->name)));

            if (ngx_media_api_json_stream_name(
                    last, end, &stream->application, &stream->name)
                != NGX_OK)
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            *last = ngx_snprintf(*last, end - *last, "}");
            return NGX_HTTP_CONFLICT;
        }

        if (rc != NGX_OK) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"child_apply_failed\"}");
            return NGX_HTTP_BAD_REQUEST;
        }

        /*
         * A replay is how a controller heals a deployment, so it replicates
         * like any other mutation: every worker ends up holding the same
         * document, whether this worker created the stream or re-used one it
         * already had.
         */
        rc = ngx_media_graph_stream_set(stream);

        if (rc == NGX_ERROR) {
            if (!stream_existed) {
                ngx_media_runtime_release(&application, &name);
                (void) ngx_media_registry_stream_destroy(registry, stream);
            }

            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"runtime_output_capacity\"}");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    *last = ngx_snprintf(*last, end - *last,
                         "{\"applied\":%ui,\"children_created\":%ui}",
                         applied, created);

    return NGX_HTTP_OK;
}

/*
 * A graph mutation can be applied on any worker, but the program it describes
 * is driven by exactly one.  A manual switch acts on the running program, so a
 * worker that is not its owner refuses it and names the owner, instead of
 * accepting it and moving a copy of the graph that nothing watches.
 */
static ngx_int_t
ngx_media_api_not_owner(ngx_media_stream_t *stream, u_char **last, u_char *end)
{
    ngx_uint_t  owner;

    if (ngx_media_graph_owns(&stream->application, &stream->name)) {
        return 0;
    }

    owner = ngx_media_route_owner((ngx_cycle_t *) ngx_cycle,
                                  ngx_media_owner_hash(&stream->application,
                                                       &stream->name));

    *last = ngx_snprintf(*last, end - *last,
                         "{\"error\":\"not_owner\",\"owner\":%ui}", owner);

    return 1;
}

static ngx_int_t
ngx_media_api_dispatch(ngx_http_request_t *r, ngx_media_registry_t *registry,
    u_char **last, u_char *end)
{
    ngx_str_t           application = ngx_null_string;
    ngx_str_t           name = ngx_null_string;
    ngx_str_t           action = ngx_null_string;
    ngx_str_t           source_id;
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

    /* collection: GET /streams, POST /streams */
    if (name.len == 0) {

        if (r->method == NGX_HTTP_POST) {
            return ngx_media_api_stream_create(r, registry, last, end);
        }

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

    /*
     * DELETE is answered even when the object is gone: the caller asked for
     * an end state, and that end state holds.  Everything else needs the
     * object to exist.
     */
    if (stream == NULL && !(action.len == 0 && r->method == NGX_HTTP_DELETE))
    {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"stream_not_found\"}");
        return NGX_HTTP_NOT_FOUND;
    }

    if (action.len == 0) {

        if (r->method == NGX_HTTP_DELETE) {
            return ngx_media_api_stream_delete(r, registry, &application,
                                               &name, last, end);
        }

        if (r->method == NGX_HTTP_PATCH) {
            return ngx_media_api_stream_patch(r, registry, &application,
                                              &name, last, end);
        }

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

    if (action.len >= sizeof("destinations") - 1
        && ngx_memcmp(action.data, "destinations",
                      sizeof("destinations") - 1) == 0)
    {
        ngx_int_t  drc = ngx_media_api_destinations(r, stream, &action, last,
                                                    end);

        if (drc != NGX_DECLINED) {
            return drc;
        }
    }

    if (action.len >= sizeof("sources") - 1
        && ngx_memcmp(action.data, "sources", sizeof("sources") - 1) == 0)
    {
        ngx_int_t  rc = ngx_media_api_sources(r, stream, &action, last, end);

        if (rc != NGX_DECLINED) {
            return rc;
        }
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

        if (ngx_media_api_not_owner(stream, last, end)) {
            return NGX_HTTP_CONFLICT;
        }

        source = ngx_media_stream_source_find(stream, &source_id);

        if (source == NULL) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"source_not_found\","
                                 "\"source\":");

            if (ngx_media_api_json_string(last, end, &source_id) != NGX_OK) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            *last = ngx_snprintf(*last, end - *last, "}");
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

        *last = ngx_snprintf(*last, end - *last, "{\"stream\":");

        if (ngx_media_api_json_stream_name(last, end, &application, &name)
            != NGX_OK)
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last, ",\"requested\":");

        if (ngx_media_api_json_string(last, end, &source_id) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last, ",\"active\":");

        if (stream->active != NULL) {
            if (ngx_media_api_json_string(last, end, &stream->active->id)
                != NGX_OK)
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

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
        ngx_int_t  rc;

        if (r->method != NGX_HTTP_POST) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"method_not_allowed\"}");
            return NGX_HTTP_NOT_ALLOWED;
        }

        if (ngx_media_api_not_owner(stream, last, end)) {
            return NGX_HTTP_CONFLICT;
        }

        rc = ngx_media_selector_switchback(stream, ngx_current_msec);

        if (rc == NGX_DECLINED) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"no_better_source\","
                                 "\"switchback\":%ui}",
                                 stream->selector.switchback);
            return NGX_HTTP_CONFLICT;
        }

        if (rc != NGX_OK) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"switchback_failed\"}");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "media: api switchback stream=%V/%V active=%V",
                      &application, &name,
                      stream->active != NULL ? &stream->active->id
                                             : &ngx_media_api_none);

        *last = ngx_snprintf(*last, end - *last, "{\"stream\":");

        if (ngx_media_api_json_stream_name(last, end, &application, &name)
            != NGX_OK)
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        *last = ngx_snprintf(*last, end - *last, ",\"active\":");

        if (stream->active != NULL) {
            if (ngx_media_api_json_string(last, end, &stream->active->id)
                != NGX_OK)
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

        } else {
            *last = ngx_snprintf(*last, end - *last, "null");
        }

        *last = ngx_snprintf(*last, end - *last,
                             ",\"generation\":%ui,\"switches\":%uL}",
                             stream->generation, stream->switches);

        return NGX_HTTP_OK;
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

static void
ngx_media_api_body_ready(ngx_http_request_t *r)
{
    ngx_media_registry_t  *registry;
    ngx_str_t              body;
    u_char                *buf, *last, *end;
    ngx_int_t              status;

    buf = ngx_pnalloc(r->pool, NGX_MEDIA_API_BUF_SIZE);
    if (buf == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    last = buf;
    end = buf + NGX_MEDIA_API_BUF_SIZE;

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        last = ngx_snprintf(last, end - last, "{\"error\":\"no_registry\"}");
        status = NGX_HTTP_INTERNAL_SERVER_ERROR;

    } else if (r->uri.len == sizeof("/media/api/v1/desired") - 1
               && ngx_memcmp(r->uri.data, "/media/api/v1/desired",
                             sizeof("/media/api/v1/desired") - 1) == 0)
    {
        if (r->method == NGX_HTTP_PUT || r->method == NGX_HTTP_POST) {
            status = ngx_media_api_desired_put(r, registry, &last, end);

        } else if (r->method == NGX_HTTP_GET) {
            status = (ngx_media_api_desired_get(registry, &last, end)
                      == NGX_OK)
                         ? NGX_HTTP_OK
                         : NGX_HTTP_INTERNAL_SERVER_ERROR;

        } else {
            last = ngx_snprintf(last, end - last,
                                "{\"error\":\"method_not_allowed\"}");
            status = NGX_HTTP_NOT_ALLOWED;
        }
    } else if (r->uri.len == sizeof("/media/api/v1/metrics") - 1
               && ngx_memcmp(r->uri.data, "/media/api/v1/metrics",
                             sizeof("/media/api/v1/metrics") - 1) == 0)
    {
        status = (ngx_media_api_metrics(registry, &last, end) == NGX_OK)
                     ? NGX_HTTP_OK
                     : NGX_HTTP_INTERNAL_SERVER_ERROR;
    } else {
        status = ngx_media_api_dispatch(r, registry, &last, end);
    }

    body.data = buf;
    body.len = last - buf;

    status = ngx_media_api_send(r, status, &body);

    ngx_http_finalize_request(r, status);
}

static ngx_int_t
ngx_media_api_handler(ngx_http_request_t *r)
{
    ngx_int_t  rc;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_POST|NGX_HTTP_DELETE
                       |NGX_HTTP_PATCH|NGX_HTTP_PUT)))
    {
        return NGX_HTTP_NOT_ALLOWED;
    }

    /*
     * Mutations carry a JSON body, so the request is only dispatched once it
     * has arrived; nginx calls ngx_media_api_body_ready() when it has.
     */
    rc = ngx_http_read_client_request_body(r, ngx_media_api_body_ready);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}

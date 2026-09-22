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
                             "\"active\":%s,\"compat\":\"%s\","
                             "\"evidence\":%ui,\"health_transitions\":%uL,"
                             "\"container_errors\":%uL,"
                             "\"frames_in\":%uL,"
                             "\"frames_out\":%uL,\"writers\":%ui,"
                             "\"preroll_units\":%ui,\"preroll_bytes\":%uz,"
                             "\"preroll_overflows\":%uL}",
                             first ? "" : ",",
                             &source->id,
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
    ngx_media_runtime_progress_t  progress;

    /*
     * Generation and program frames describe the media a program carried, and
     * a program has one driver.  On the worker that drives it they are the
     * local numbers; on a replica they are the numbers the owner published,
     * with the owner named, so a read never mixes "this program is not here"
     * into a field that means "the program has carried nothing".
     */
    ngx_media_runtime_progress(&stream->application, &stream->name,
                               stream->generation, stream->program_frames,
                               &progress);

    *last = ngx_snprintf(*last, end - *last,
                         "{\"application\":\"%V\",\"name\":\"%V\","
                         "\"owner\":%ui,\"observed_here\":%s,"
                         "\"generation\":%ui,\"switches\":%uL,"
                         "\"emergency_switches\":%uL,"
                         "\"failure_timeout_ms\":%ui,"
                         "\"recovery_timeout_ms\":%ui,"
                         "\"switchback\":%ui,"
                         "\"program_frames\":%uL,\"active\":",
                         &stream->application, &stream->name,
                         progress.owner,
                         progress.local ? "true" : "false",
                         progress.generation, stream->switches,
                         stream->emergency_switches,
                         stream->selector.failure_timeout,
                         stream->selector.recovery_timeout,
                         stream->selector.switchback,
                         progress.frames);

    if (stream->active != NULL) {
        *last = ngx_snprintf(*last, end - *last, "\"%V\"", &stream->active->id);

    } else {
        *last = ngx_snprintf(*last, end - *last, "null");
    }

    *last = ngx_snprintf(*last, end - *last, ",\"sources\":");

    if (ngx_media_api_sources_json(last, end, stream) != NGX_OK) {
        return NGX_ERROR;
    }

    /*
     * A program's destinations belong to the same read as its sources: they
     * are the other half of "what is this stream attached to", and the
     * collection route has always reported them.  The endpoint is redacted
     * exactly as everywhere else -- a destination host may carry a credential.
     */
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
            (void) ngx_media_api_destination_json(destination, last, end, -1);
        }
    }

    *last = ngx_snprintf(*last, end - *last, "]");

    /*
     * fanout_delay percentiles (goal doc 32): how long a unit of media waits
     * after the program publishes it before a consumer takes it.  This is the
     * number an operator can feel, and the one that says whether the
     * deployment still has headroom.
     */
    *last = ngx_snprintf(*last, end - *last,
                         ",\"fanout_ms\":{\"p50\":%M,\"p95\":%M,\"p99\":%M,"
                         "\"max\":%uL},\"dispatched\":%uL}",
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
                         "# HELP nginx_media_runtime_outputs "
                         "runtime output slots in use\n"
                         "# TYPE nginx_media_runtime_outputs gauge\n"
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
                         "nginx_media_worker_service_ms %M\n"
                         "nginx_media_worker_max_service_ms %M\n"
                         "nginx_media_reconnecting_sources %ui\n"
                         "nginx_media_graph_undelivered_total %uL\n",
                         ngx_media_runtime_outputs_active(),
                         stats.last_gap, stats.max_gap, stats.late_ticks,
                         stats.last_service, stats.max_service,
                         stats.reconnecting,
                         ngx_media_route_broadcast_undelivered());

    for (q = ngx_queue_head(&registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = q->next)
    {
        ngx_media_runtime_progress_t  progress;

        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);
        stream = &entry->stream;

        /*
         * Generation and program frames are the program's, not a replica's:
         * they are read the same way the stream document reads them, so a
         * scrape of any worker reports the same figures.
         */
        ngx_media_runtime_progress(&stream->application, &stream->name,
                                   stream->generation, stream->program_frames,
                                   &progress);

        *last = ngx_snprintf(*last, end - *last,
                             "nginx_media_stream_generation"
                             "{application=\"%V\",name=\"%V\"} %ui\n"
                             "nginx_media_stream_switches"
                             "{application=\"%V\",name=\"%V\"} %uL\n"
                             "nginx_media_stream_program_frames"
                             "{application=\"%V\",name=\"%V\"} %uL\n"
                             "nginx_media_stream_fanout_delay_ms"
                             "{application=\"%V\",name=\"%V\",percentile=\"50\"} %M\n"
                             "nginx_media_stream_fanout_delay_ms"
                             "{application=\"%V\",name=\"%V\",percentile=\"95\"} %M\n"
                             "nginx_media_stream_fanout_delay_ms"
                             "{application=\"%V\",name=\"%V\",percentile=\"99\"} %M\n"
                             "nginx_media_stream_fanout_delay_ms"
                             "{application=\"%V\",name=\"%V\",percentile=\"99.9\"} %M\n"
                             "nginx_media_stream_dispatched_total"
                             "{application=\"%V\",name=\"%V\"} %uL\n"
                             "nginx_media_stream_feed_units"
                             "{application=\"%V\",name=\"%V\"} %ui\n"
                             "nginx_media_stream_feed_bytes"
                             "{application=\"%V\",name=\"%V\"} %uz\n",
                             &stream->application, &stream->name,
                             progress.generation,
                             &stream->application, &stream->name,
                             stream->switches,
                             &stream->application, &stream->name,
                             progress.frames,
                             &stream->application, &stream->name,
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 500),
                             &stream->application, &stream->name,
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 950),
                             &stream->application, &stream->name,
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 990),
                             &stream->application, &stream->name,
                             ngx_media_feed_fanout_percentile(
                                 &stream->program_feed, 999),
                             &stream->application, &stream->name,
                             ngx_media_feed_fanout_count(&stream->program_feed),
                             &stream->application, &stream->name,
                             ngx_media_feed_units(&stream->program_feed),
                             &stream->application, &stream->name,
                             ngx_media_feed_bytes(&stream->program_feed));

        for (sq = ngx_queue_head(&stream->sources);
             sq != (ngx_queue_t *) &stream->sources;
             sq = sq->next)
        {
            source = ngx_queue_data(sq, ngx_media_source_t, queue);

            *last = ngx_snprintf(*last, end - *last,
                                 "nginx_media_source_frames_in"
                                 "{application=\"%V\",name=\"%V\","
                                 "source=\"%V\"} %uL\n"
                                 "nginx_media_source_frames_out"
                                 "{application=\"%V\",name=\"%V\","
                                 "source=\"%V\"} %uL\n"
                                 "nginx_media_source_healthy"
                                 "{application=\"%V\",name=\"%V\","
                                 "source=\"%V\"} %d\n"
                                 "nginx_media_source_active"
                                 "{application=\"%V\",name=\"%V\","
                                 "source=\"%V\"} %d\n",
                                 &stream->application, &stream->name,
                                 &source->id, source->frames_in,
                                 &stream->application, &stream->name,
                                 &source->id, source->frames_out,
                                 &stream->application, &stream->name,
                                 &source->id, source->health.healthy ? 1 : 0,
                                 &stream->application, &stream->name,
                                 &source->id, source->active ? 1 : 0);

            if (*last >= end - 1) {
                return NGX_ERROR;
            }
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

/* "key": "value"  |  "key": 123   -- the first match, or NGX_DECLINED */
static ngx_int_t
ngx_media_api_json_field(const ngx_str_t *body, const char *key,
    ngx_str_t *value)
{
    u_char     *p, *end, *start;
    size_t      key_len = strlen(key);

    if (body->data == NULL) {
        return NGX_DECLINED;
    }

    p = body->data;
    end = body->data + body->len;

    while (p < end) {
        if (*p != '"') {
            p++;
            continue;
        }

        if ((size_t) (end - p) < key_len + 3
            || ngx_strncmp(p + 1, key, key_len) != 0
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
            p++;
            continue;
        }

        p++;

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        if (p == end) {
            return NGX_DECLINED;
        }

        if (*p == '"') {
            start = ++p;

            while (p < end && *p != '"') {
                p++;
            }

            if (p == end) {
                return NGX_DECLINED;
            }

            value->data = start;
            value->len = p - start;
            return NGX_OK;
        }

        start = p;

        while (p < end && *p != ',' && *p != '}' && *p != ' ') {
            p++;
        }

        value->data = start;
        value->len = p - start;
        return NGX_OK;
    }

    return NGX_DECLINED;
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
    ngx_str_t              body, application, name;
    ngx_media_stream_t    *stream;
    ngx_media_feed_conf_t  feed_conf;
    ngx_uint_t             existed;

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

    existed = (ngx_media_registry_stream(registry, &application, &name)
               != NULL);

    ngx_memzero(&feed_conf, sizeof(feed_conf));

    feed_conf.max_units = NGX_MEDIA_API_FEED_UNITS;
    feed_conf.max_bytes = NGX_MEDIA_API_FEED_BYTES;
    feed_conf.max_age = NGX_MEDIA_API_FEED_AGE;

    stream = ngx_media_registry_stream_create(registry, &application, &name,
                                              &feed_conf, r->connection->log);

    /*
     * The worker that creates a stream owns it.
     *
     * The alternative is the deterministic hash, which spreads programs
     * evenly, and it does not work yet: a source that carries its own reader
     * - a file, an origin, a directory - is opened by whoever applies the
     * mutation, and only the owner materialises it.  With the hash, the
     * originating worker is not the owner, so the reader it opened feeds a
     * replica nobody drives.  Measured: eight programs over four workers kept
     * 4000 frames with the claim and 1000 without, while the owners went from
     * two to four.
     *
     * So ownership follows the creating worker for now, and the cost is that
     * the spread follows nginx's accept distribution rather than a hash.
     * Making the hash work means routing the originating apply through the
     * owner-only materialisation path, which is the follow-up.
     */
    if (stream != NULL) {
        ngx_media_runtime_claim(&application, &name);
    }

    if (stream == NULL) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"stream_create_failed\"}");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (!existed) {
        ngx_media_stream_touch(stream);
    }

    /*
     * Every worker gets the stream, so the next request for it - a read, or a
     * source added by an operator - is answered by whichever worker takes it
     * instead of coming back 404.  Re-sending an unchanged stream is not a
     * special case: the operation is idempotent, and it is how a controller
     * replays desired state to a worker whose replica is behind.
     */
    (void) ngx_media_graph_stream_set(stream);

    *last = ngx_snprintf(*last, end - *last,
                         "{\"application\":\"%V\",\"name\":\"%V\","
                         "\"revision\":%uL,\"created\":%s}",
                         &stream->application, &stream->name, stream->revision,
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

    stream = ngx_media_registry_stream(registry, application, name);

    if (stream == NULL) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"application\":\"%V\",\"name\":\"%V\","
                             "\"deleted\":false,\"reason\":\"absent\"}",
                             application, name);
        return NGX_HTTP_OK;
    }

    if (ngx_media_api_arg(r, "revision", &revision_text) == NGX_OK) {
        revision = ngx_atoi(revision_text.data, revision_text.len);

        if (revision >= 0 && (uint64_t) revision != stream->revision) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"stale_revision\","
                                 "\"revision\":%uL}",
                                 stream->revision);
            return NGX_HTTP_CONFLICT;
        }
    }

    ngx_media_runtime_release(&stream->application, &stream->name);

    /*
     * The replicas drop it too, before this worker's copy goes away: the
     * operation carries the revision the stream had, so a replica that has
     * already moved past it keeps the newer object instead of deleting it.
     */
    (void) ngx_media_graph_stream_delete(&stream->application, &stream->name,
                                         stream->revision);

    if (ngx_media_registry_stream_destroy(registry, stream) != NGX_OK) {
        *last = ngx_snprintf(*last, end - *last,
                             "{\"error\":\"stream_delete_failed\"}");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *last = ngx_snprintf(*last, end - *last,
                         "{\"application\":\"%V\",\"name\":\"%V\","
                         "\"deleted\":true}", application, name);

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
    ngx_str_t            body, value;
    ngx_int_t            n;

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

    *last = ngx_snprintf(*last, end - *last,
                         "{\"application\":\"%V\",\"name\":\"%V\","
                         "\"revision\":%uL}", &stream->application,
                         &stream->name, stream->revision);

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
        *last = ngx_snprintf(*last, end - *last,
                             "{\"id\":\"%V\",\"revision\":%uL,"
                             "\"created\":false}", &source->id,
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
    source = ngx_media_graph_source_open(stream, &id, type, priority,
                                         &source_path, &source_ca,
                                         r->connection->log);

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

    *last = ngx_snprintf(*last, end - *last,
                         "{\"id\":\"%V\",\"revision\":%uL,"
                         "\"created\":true}", &source->id, source->revision);

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
        *last = ngx_snprintf(*last, end - *last,
                             "{\"id\":\"%V\",\"deleted\":false,"
                             "\"reason\":\"absent\"}", source_id);
        return NGX_HTTP_OK;
    }

    ngx_media_stream_source_remove(stream, source);
    ngx_media_stream_touch(stream);

    (void) ngx_media_graph_source_delete(stream, source_id, stream->revision);

    *last = ngx_snprintf(*last, end - *last,
                         "{\"id\":\"%V\",\"deleted\":true}", source_id);

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

    *last = ngx_snprintf(*last, end - *last,
                         "{\"id\":\"%V\",\"enabled\":%s,"
                         "\"revision\":%uL}",
                         &source->id, enabled ? "true" : "false",
                         source->revision);

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

            *last = ngx_snprintf(*last, end - *last,
                                 "{\"id\":\"%V\",\"type\":%ui,"
                                 "\"priority\":%ui,\"enabled\":%s,"
                                 "\"state\":\"%s\",\"revision\":%uL}",
                                 &source->id, source->type, source->priority,
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
    u_char     *tail = (u_char *) "}";
    u_char      safe[512];
    ngx_str_t   host;

    if (created >= 0) {
        tail = (u_char *) (created ? ",\"created\":true}"
                                   : ",\"created\":false}");
    }

    host = ngx_media_api_safe_host(destination, safe, sizeof(safe));

    *last = ngx_snprintf(*last, end - *last,
                         "{\"id\":\"%V\",\"type\":%ui,\"host\":\"%V\","
                         "\"port\":%ui,\"enabled\":%s,"
                         "\"revision\":%uL%s",
                         &destination->id, destination->type,
                         &host, destination->port,
                         destination->enabled ? "true" : "false",
                         destination->revision, tail);

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
        *last = ngx_snprintf(*last, end - *last,
                             "{\"id\":\"%V\",\"deleted\":false,"
                             "\"reason\":\"absent\"}", id);
        return NGX_HTTP_OK;
    }

    ngx_media_destination_remove(stream, destination);

    *last = ngx_snprintf(*last, end - *last,
                         "{\"id\":\"%V\",\"deleted\":true}", id);

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
    ngx_queue_t              *q, *sq;
    ngx_media_registry_entry_t  *entry;
    ngx_media_stream_t       *stream;
    ngx_media_source_t       *source;
    ngx_media_destination_t  *destination;
    ngx_uint_t                first = 1;

    *last = ngx_snprintf(*last, end - *last, "{\"streams\":[");

    for (q = ngx_queue_head(&registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = q->next)
    {
        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);
        stream = &entry->stream;

        *last = ngx_snprintf(*last, end - *last,
                             "%s{\"application\":\"%V\",\"name\":\"%V\","
                             "\"revision\":%uL,\"sources\":[",
                             first ? "" : ",", &stream->application,
                             &stream->name, stream->revision);
        first = 0;

        if (*last >= end - 1) {
            return NGX_ERROR;
        }

        {
            ngx_uint_t  sfirst = 1;

            for (sq = ngx_queue_head(&stream->sources);
                 sq != (ngx_queue_t *) &stream->sources;
                 sq = sq->next)
            {
                source = ngx_queue_data(sq, ngx_media_source_t, queue);

                *last = ngx_snprintf(*last, end - *last,
                                     "%s{\"id\":\"%V\",\"type\":%ui,"
                                     "\"priority\":%ui,\"enabled\":%s,"
                                     "\"revision\":%uL}",
                                     sfirst ? "" : ",", &source->id,
                                     source->type, source->priority,
                                     source->enabled ? "true" : "false",
                                     source->revision);
                sfirst = 0;

                if (*last >= end - 1) {
                    return NGX_ERROR;
                }
            }
        }

        *last = ngx_snprintf(*last, end - *last, "],\"destinations\":[");

        if (*last >= end - 1) {
            return NGX_ERROR;
        }

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
                                     "%s{\"id\":\"%V\",\"type\":%ui,"
                                     "\"host\":\"%V\",\"port\":%ui,"
                                     "\"enabled\":%s,\"revision\":%uL}",
                                     dfirst ? "" : ",", &destination->id,
                                     destination->type, &host,
                                     destination->port,
                                     destination->enabled ? "true" : "false",
                                     destination->revision);
                dfirst = 0;

                if (*last >= end - 1) {
                    return NGX_ERROR;
                }
            }
        }

        /*
         * fanout_delay percentiles (goal doc 32): how long a unit of media
         * waits after the program publishes it before a consumer takes it.
         * This is the number an operator can feel, and the one that says
         * whether the deployment still has headroom.
         */
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
    ngx_str_t           array, child, value;
    ngx_media_source_t *source;
    ngx_media_destination_t  *destination;
    ngx_uint_t          type, priority, port;
    u_char             *p, *stop;
    ngx_str_t          *copy;
    ngx_int_t           n;

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

            if (ngx_media_stream_source_find(stream, &value) != NULL) {
                continue;
            }

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

            priority = 0;

            if (ngx_media_api_json_field(&child, "priority", &value)
                == NGX_OK)
            {
                n = ngx_atoi(value.data, value.len);

                if (n > 0) {
                    priority = (ngx_uint_t) n;
                }
            }

            if (ngx_media_api_json_field(&child, "id", &value) != NGX_OK) {
                return NGX_ERROR;
            }

            source = ngx_media_stream_source_add(stream, &value, type,
                                                 priority,
                                                 r->connection->log);

            if (source == NULL) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "media: desired: source %V could not be added",
                              &value);
                return NGX_ERROR;
            }

            ngx_media_source_touch(source);
            (*created)++;

            /*
             * A desired document names source identities, not paths, so the
             * source is replicated as what it is here: a label a transport
             * attaches to later.  No reader is opened for it on any worker.
             */
            (void) ngx_media_graph_source_set(stream, source, NULL, NULL);
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
    ngx_str_t              body, streams, item, application, name;
    ngx_media_stream_t    *stream;
    ngx_media_feed_conf_t  feed_conf;
    u_char                *p, *stop;
    ngx_uint_t             applied = 0, created = 0;
    ngx_int_t              rc;

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

        stream = ngx_media_registry_stream_create(registry, &application,
                                                  &name, &feed_conf,
                                                  r->connection->log);

        if (stream == NULL) {
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"stream_create_failed\"}");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        applied++;

        rc = ngx_media_api_desired_children(r, stream, &item, &created);

        if (rc == NGX_DECLINED) {
            /*
             * The document asks for an output that only the stream's owner can
             * start; the rest of it was applied, and applying it again there
             * is idempotent.
             */
            *last = ngx_snprintf(*last, end - *last,
                                 "{\"error\":\"destination_needs_owner\","
                                 "\"owner\":%ui,\"stream\":\"%V/%V\"}",
                                 ngx_media_route_owner(
                                     (ngx_cycle_t *) ngx_cycle,
                                     ngx_media_owner_hash(
                                         &stream->application, &stream->name)),
                                 &stream->application, &stream->name);
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
        (void) ngx_media_graph_stream_set(stream);
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

        *last = ngx_snprintf(*last, end - *last,
                             "{\"stream\":\"%V/%V\",\"active\":", &application,
                             &name);

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

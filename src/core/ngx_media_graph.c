#include "ngx_media_graph.h"

#include "ngx_media_route.h"
#include "ngx_media_stream.h"
#include "ngx_media_file.h"
#include "ngx_media_hls_pull.h"
#include "ngx_media_hls_ingest.h"

/*
 * The graph a replica stream is built with.  These are the API's own feed
 * defaults (ngx_media_api_module.c): a stream has to look the same whichever
 * worker created it, and a source operation that overtook its stream
 * operation must not build a stream with different ceilings.
 */
#define NGX_MEDIA_GRAPH_FEED_UNITS   2048
#define NGX_MEDIA_GRAPH_FEED_BYTES   (32 * 1024 * 1024)
#define NGX_MEDIA_GRAPH_FEED_AGE     10000

static ngx_int_t
ngx_media_graph_encode(const ngx_media_graph_op_t *op,
    ngx_media_ipc_header_t *header, ngx_media_buf_t **payload, size_t *length);

static ngx_int_t
ngx_media_graph_decode(const ngx_media_ipc_header_t *header,
    ngx_media_buf_t *payload, ngx_media_graph_op_t *op);

static u_char *
ngx_media_graph_copy(u_char *p, const ngx_str_t *value);

static ngx_int_t
ngx_media_graph_send(const ngx_media_graph_op_t *op)
{
    ngx_media_ipc_header_t  header;
    ngx_media_buf_t        *payload = NULL;
    ngx_uint_t              peers = 0, delivered;
    size_t                  length = 0;

    if (ngx_media_graph_encode(op, &header, &payload, &length) != NGX_OK) {
        return NGX_ERROR;
    }

    delivered = ngx_media_route_broadcast((ngx_cycle_t *) ngx_cycle, &header,
                                          payload, length, &peers);

    ngx_media_buf_unref(payload);

    if (peers == 0 || delivered == peers) {
        /* one worker is its own replica, and every peer took it */
        return NGX_OK;
    }

    /*
     * Best effort, and it says so: this worker has the mutation, a peer does
     * not.  The request that caused it still succeeds - stalling a control
     * request on a worker that is gone would be worse than a replica that
     * heals on the next operation - and ngx_media_route_broadcast() has
     * already logged which peer was missed.
     */
    return NGX_AGAIN;
}

/* the wire layout: the fixed part, then the strings in field order */
static ngx_int_t
ngx_media_graph_encode(const ngx_media_graph_op_t *op,
    ngx_media_ipc_header_t *header, ngx_media_buf_t **payload, size_t *length)
{
    ngx_media_graph_wire_t  wire;
    ngx_media_buf_t        *buf;
    size_t                  len;
    u_char                 *p;

    if (op == NULL || header == NULL || payload == NULL || length == NULL) {
        return NGX_ERROR;
    }

    len = sizeof(ngx_media_graph_wire_t) + op->application.len + op->name.len
          + op->id.len + op->path.len + op->ca_file.len;

    if (len > NGX_MEDIA_IPC_MAX_PAYLOAD) {
        /* refuse rather than truncate: a partial graph is worse than none */
        ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                      "media: graph operation for %V/%V is too large (%uz "
                      "bytes); not replicated", &op->application, &op->name,
                      len);
        return NGX_ERROR;
    }

    buf = ngx_media_buf_alloc(len);

    if (buf == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&wire, sizeof(wire));

    wire.kind = (uint32_t) op->kind;
    wire.revision = op->revision;
    wire.source_revision = op->source_revision;
    wire.type = (uint32_t) op->type;
    wire.priority = (uint32_t) op->priority;
    wire.enabled = op->enabled ? 1 : 0;
    wire.failure_timeout = (uint32_t) op->failure_timeout;
    wire.recovery_timeout = (uint32_t) op->recovery_timeout;
    wire.application_len = (uint32_t) op->application.len;
    wire.name_len = (uint32_t) op->name.len;
    wire.id_len = (uint32_t) op->id.len;
    wire.path_len = (uint32_t) op->path.len;
    wire.ca_file_len = (uint32_t) op->ca_file.len;

    p = ngx_media_buf_data(buf);

    ngx_memcpy(p, &wire, sizeof(wire));
    p += sizeof(wire);

    p = ngx_media_graph_copy(p, &op->application);
    p = ngx_media_graph_copy(p, &op->name);
    p = ngx_media_graph_copy(p, &op->id);
    p = ngx_media_graph_copy(p, &op->path);
    p = ngx_media_graph_copy(p, &op->ca_file);

    (void) ngx_media_buf_freeze(buf, (size_t) (p - ngx_media_buf_data(buf)));

    ngx_memzero(header, sizeof(*header));

    header->version = NGX_MEDIA_IPC_VERSION;
    header->type = NGX_MEDIA_IPC_MSG_GRAPH;
    header->hash = ngx_media_owner_hash(&op->application, &op->name);

    *payload = buf;
    *length = ngx_media_buf_size(buf);

    return NGX_OK;
}

ngx_int_t
ngx_media_graph_stream_set(const ngx_media_stream_t *stream)
{
    ngx_media_graph_op_t  op;

    if (stream == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&op, sizeof(op));

    op.kind = NGX_MEDIA_GRAPH_STREAM_SET;
    op.revision = stream->revision;
    op.failure_timeout = (ngx_uint_t) stream->selector.failure_timeout;
    op.recovery_timeout = (ngx_uint_t) stream->selector.recovery_timeout;
    op.application = stream->application;
    op.name = stream->name;

    return ngx_media_graph_send(&op);
}

ngx_int_t
ngx_media_graph_stream_delete(const ngx_str_t *application,
    const ngx_str_t *name, uint64_t revision)
{
    ngx_media_graph_op_t  op;

    if (application == NULL || name == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&op, sizeof(op));

    op.kind = NGX_MEDIA_GRAPH_STREAM_DELETE;
    op.revision = revision;
    op.application = *application;
    op.name = *name;

    return ngx_media_graph_send(&op);
}

ngx_int_t
ngx_media_graph_source_set(const ngx_media_stream_t *stream,
    const ngx_media_source_t *source, const ngx_str_t *path,
    const ngx_str_t *ca_file)
{
    ngx_media_graph_op_t  op;

    if (stream == NULL || source == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&op, sizeof(op));

    op.kind = NGX_MEDIA_GRAPH_SOURCE_SET;
    op.revision = stream->revision;
    op.source_revision = source->revision;
    op.type = source->type;
    op.priority = source->priority;
    op.enabled = source->enabled ? 1 : 0;
    op.application = stream->application;
    op.name = stream->name;
    op.id = source->id;

    if (path != NULL) {
        op.path = *path;
    }

    if (ca_file != NULL) {
        op.ca_file = *ca_file;
    }

    return ngx_media_graph_send(&op);
}

ngx_int_t
ngx_media_graph_source_delete(const ngx_media_stream_t *stream,
    const ngx_str_t *id, uint64_t revision)
{
    ngx_media_graph_op_t  op;

    if (stream == NULL || id == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&op, sizeof(op));

    op.kind = NGX_MEDIA_GRAPH_SOURCE_DELETE;
    op.revision = revision;
    op.application = stream->application;
    op.name = stream->name;
    op.id = *id;

    return ngx_media_graph_send(&op);
}

/* copies one field of an operation into the payload the sender is building */
static u_char *
ngx_media_graph_copy(u_char *p, const ngx_str_t *value)
{
    if (value == NULL || value->len == 0) {
        return p;
    }

    ngx_memcpy(p, value->data, value->len);

    return p + value->len;
}

static ngx_int_t
ngx_media_graph_decode(const ngx_media_ipc_header_t *header,
    ngx_media_buf_t *payload, ngx_media_graph_op_t *op)
{
    ngx_media_graph_wire_t  wire;
    size_t                  need, offset;
    u_char                 *p;

    if (header == NULL || op == NULL || payload == NULL
        || ngx_media_buf_size(payload) < sizeof(wire))
    {
        return NGX_ERROR;
    }

    ngx_memcpy(&wire, ngx_media_buf_data(payload), sizeof(wire));

    need = sizeof(wire) + (size_t) wire.application_len + wire.name_len
           + wire.id_len + wire.path_len + wire.ca_file_len;

    if (ngx_media_buf_size(payload) != need
        || wire.application_len == 0 || wire.name_len == 0)
    {
        return NGX_ERROR;
    }

    if (wire.application_len > NGX_MEDIA_GRAPH_MAX_NAME
        || wire.name_len > NGX_MEDIA_GRAPH_MAX_NAME
        || wire.id_len > NGX_MEDIA_GRAPH_MAX_NAME
        || wire.path_len > NGX_MEDIA_GRAPH_MAX_PATH
        || wire.ca_file_len > NGX_MEDIA_GRAPH_MAX_PATH)
    {
        return NGX_ERROR;
    }

    if (wire.kind < NGX_MEDIA_GRAPH_STREAM_SET
        || wire.kind > NGX_MEDIA_GRAPH_SOURCE_DELETE)
    {
        return NGX_ERROR;
    }

    ngx_memzero(op, sizeof(*op));

    op->kind = wire.kind;
    op->revision = wire.revision;
    op->source_revision = wire.source_revision;
    op->type = wire.type;
    op->priority = wire.priority;
    op->enabled = wire.enabled ? 1 : 0;
    op->failure_timeout = wire.failure_timeout;
    op->recovery_timeout = wire.recovery_timeout;

    p = ngx_media_buf_data(payload) + sizeof(wire);
    offset = 0;

    op->application.data = p + offset;
    op->application.len = wire.application_len;
    offset += wire.application_len;

    op->name.data = p + offset;
    op->name.len = wire.name_len;
    offset += wire.name_len;

    op->id.data = p + offset;
    op->id.len = wire.id_len;
    offset += wire.id_len;

    op->path.data = p + offset;
    op->path.len = wire.path_len;
    offset += wire.path_len;

    op->ca_file.data = p + offset;
    op->ca_file.len = wire.ca_file_len;

    return NGX_OK;
}

ngx_uint_t
ngx_media_graph_owns(const ngx_str_t *application, const ngx_str_t *name)
{
    if (application == NULL || name == NULL) {
        return 0;
    }

    return ngx_media_route_is_owner((ngx_cycle_t *) ngx_cycle,
                                    ngx_media_owner_hash(application, name));
}

ngx_media_source_t *
ngx_media_graph_source_open(ngx_media_stream_t *stream, const ngx_str_t *id,
    ngx_uint_t type, ngx_uint_t priority, const ngx_str_t *path,
    const ngx_str_t *ca_file, ngx_log_t *log)
{
    ngx_str_t  empty = ngx_null_string;

    if (stream == NULL || id == NULL || id->len == 0) {
        return NULL;
    }

    if (path == NULL) {
        path = &empty;
    }

    if (ca_file == NULL) {
        ca_file = &empty;
    }

    /*
     * The graph is replicated; the program is not.  A source that carries its
     * own reader - a file, an origin, a directory - is materialised only on
     * the worker that owns the stream, so four workers do not read the same
     * file four times.  Everywhere else the source is registered as desired
     * state, which is what answers reads and what the owner's replica of the
     * graph is compared against.
     */
    if (!ngx_media_graph_owns(&stream->application, &stream->name)) {
        return ngx_media_stream_source_add(stream, id, type, priority, log);
    }

    switch (type) {

    case NGX_MEDIA_SOURCE_FILE:

        if (path->len == 0) {
            ngx_log_error(NGX_LOG_WARN, log, 0,
                          "media: file source %V for %V/%V has no path",
                          id, &stream->application, &stream->name);
            return ngx_media_stream_source_add(stream, id, type, priority,
                                               log);
        }

        /*
         * The API's file sources are one-shot, and the mode is not part of an
         * operation: a replica opens the same file the same way the request
         * asked for, which is the only way the API asks for one.
         */
        if (ngx_media_file_open(stream, id, path, NGX_MEDIA_FILE_ONCE, log)
            == NULL)
        {
            return NULL;
        }

        return ngx_media_stream_source_find(stream, id);

    case NGX_MEDIA_SOURCE_HLS_PUSH:

        if (path->len == 0) {
            ngx_log_error(NGX_LOG_WARN, log, 0,
                          "media: hls push source %V for %V/%V has no "
                          "directory", id, &stream->application, &stream->name);
            return ngx_media_stream_source_add(stream, id, type, priority,
                                               log);
        }

        if (ngx_media_hls_ingest_open(stream, id, path, log) == NULL) {
            return NULL;
        }

        return ngx_media_stream_source_find(stream, id);

    case NGX_MEDIA_SOURCE_HLS_PULL:

        if (path->len == 0) {
            ngx_log_error(NGX_LOG_WARN, log, 0,
                          "media: hls pull source %V for %V/%V has no url",
                          id, &stream->application, &stream->name);
            return ngx_media_stream_source_add(stream, id, type, priority,
                                               log);
        }

        if (ngx_media_hls_pull_open(stream, id, path, ca_file, log) == NULL) {
            return NULL;
        }

        return ngx_media_stream_source_find(stream, id);

    default:
        return ngx_media_stream_source_add(stream, id, type, priority, log);
    }
}

/* the stream an operation belongs to, created when the operation overtook it */
static ngx_media_stream_t *
ngx_media_graph_stream(ngx_media_registry_t *registry,
    const ngx_media_graph_op_t *op, ngx_log_t *log)
{
    ngx_media_stream_t    *stream;
    ngx_media_feed_conf_t  feed_conf;

    stream = ngx_media_registry_stream(registry, &op->application, &op->name);

    if (stream != NULL) {
        return stream;
    }

    /*
     * Two workers mutating one stream send their operations independently, so
     * a source operation can arrive before the stream operation that created
     * its stream.  The stream is implicit in the source operation, so build it
     * here rather than dropping the source: a later stream operation carries
     * the same fields and is idempotent.
     */
    feed_conf.max_units = NGX_MEDIA_GRAPH_FEED_UNITS;
    feed_conf.max_bytes = NGX_MEDIA_GRAPH_FEED_BYTES;
    feed_conf.max_age = NGX_MEDIA_GRAPH_FEED_AGE;

    return ngx_media_registry_stream_create(registry, &op->application,
                                            &op->name, &feed_conf, log);
}

ngx_int_t
ngx_media_graph_apply(const ngx_media_ipc_header_t *header,
    ngx_media_buf_t *payload)
{
    ngx_media_graph_op_t  op;
    ngx_media_registry_t *registry;
    ngx_media_stream_t   *stream;
    ngx_media_source_t   *source;
    ngx_log_t            *log = ngx_cycle->log;

    if (ngx_media_graph_decode(header, payload, &op) != NGX_OK) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: malformed graph operation from another worker");
        return NGX_ERROR;
    }

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        return NGX_ERROR;
    }

    stream = ngx_media_registry_stream(registry, &op.application, &op.name);

    /*
     * Ordering.  Delivery from one worker is ordered by the transport, so an
     * operation is only out of order when it raced a different worker's
     * mutation of the same stream.  The revision the API assigned to that
     * mutation is the tie-break: an operation the stream has already moved
     * past is dropped, which is what keeps a stale delete from removing a
     * stream a newer operation just updated, and a stale create from
     * resurrecting one a newer operation removed.
     */
    if (stream != NULL && stream->revision > op.revision) {
        ngx_log_debug4(NGX_LOG_DEBUG_CORE, log, 0,
                       "media: graph operation type=%ui for %V/%V is stale "
                       "(replica revision=%uL); dropped",
                       op.kind, &op.application, &op.name, stream->revision);
        return NGX_OK;
    }

    switch (op.kind) {

    case NGX_MEDIA_GRAPH_STREAM_SET:

        stream = ngx_media_graph_stream(registry, &op, log);

        if (stream == NULL) {
            return NGX_ERROR;
        }

        stream->selector.failure_timeout = (ngx_msec_t) op.failure_timeout;
        stream->selector.recovery_timeout = (ngx_msec_t) op.recovery_timeout;

        /*
         * A replica mirrors the revision of the operation instead of bumping
         * a counter of its own: the guard above compares operations that come
         * from every worker, so the number has to mean the same thing on each
         * of them.
         */
        stream->revision = op.revision;

        return NGX_OK;

    case NGX_MEDIA_GRAPH_STREAM_DELETE:

        if (stream == NULL) {
            return NGX_OK;   /* idempotent: the end state already holds */
        }

        /*
         * Ownership is a shared record, so whoever holds it releases it.  The
         * worker that took the request may not have been the owner.
         */
        ngx_media_runtime_release(&op.application, &op.name);

        if (ngx_media_registry_stream_destroy(registry, stream) != NGX_OK) {
            return NGX_ERROR;
        }

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: graph replica dropped stream=%V/%V",
                      &op.application, &op.name);

        return NGX_OK;

    case NGX_MEDIA_GRAPH_SOURCE_SET:

        stream = ngx_media_graph_stream(registry, &op, log);

        if (stream == NULL || op.id.len == 0) {
            return NGX_ERROR;
        }

        source = ngx_media_stream_source_find(stream, &op.id);

        if (source == NULL) {
            source = ngx_media_graph_source_open(stream, &op.id, op.type,
                                                 op.priority, &op.path,
                                                 &op.ca_file, log);

            if (source == NULL) {
                ngx_log_error(NGX_LOG_WARN, log, 0,
                              "media: could not register source %V on %V/%V",
                              &op.id, &op.application, &op.name);
                return NGX_ERROR;
            }

        } else if (source->type == op.type) {
            source->priority = op.priority;
        }

        /*
         * The desired fields are set from the operation, not just when the
         * source is new: an enable or a priority change is the same operation
         * with the same fields, so applying it twice is a no-op and applying
         * it after another worker's change converges.
         */
        source->enabled = op.enabled ? 1 : 0;
        source->revision = op.source_revision;
        stream->revision = op.revision;

        return NGX_OK;

    case NGX_MEDIA_GRAPH_SOURCE_DELETE:

        if (stream == NULL || op.id.len == 0) {
            return NGX_OK;   /* idempotent */
        }

        source = ngx_media_stream_source_find(stream, &op.id);

        if (source != NULL) {
            ngx_media_stream_source_remove(stream, source);
        }

        stream->revision = op.revision;

        return NGX_OK;

    default:
        break;
    }

    return NGX_ERROR;
}

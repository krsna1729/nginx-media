#include "ngx_media_destination.h"
#include "ngx_media_stream.h"
#include "ngx_media_egress_manager.h"

#define NGX_MEDIA_DESTINATION_MAX_BACKENDS  4

static const ngx_media_destination_ops_t
    *ngx_media_destination_backends[NGX_MEDIA_DESTINATION_MAX_BACKENDS];

static const ngx_media_destination_ops_t *
ngx_media_destination_backend(ngx_uint_t type)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_DESTINATION_MAX_BACKENDS; i++) {

        if (ngx_media_destination_backends[i] != NULL
            && ngx_media_destination_backends[i]->type == type)
        {
            return ngx_media_destination_backends[i];
        }
    }

    return NULL;
}

ngx_int_t
ngx_media_destination_register(const ngx_media_destination_ops_t *ops)
{
    ngx_uint_t  i;

    if (ops == NULL || ops->add == NULL || ops->remove == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < NGX_MEDIA_DESTINATION_MAX_BACKENDS; i++) {

        if (ngx_media_destination_backends[i] == NULL
            || ngx_media_destination_backends[i]->type == ops->type)
        {
            ngx_media_destination_backends[i] = ops;
            return NGX_OK;
        }
    }

    return NGX_ERROR;
}

void
ngx_media_destination_touch(ngx_media_destination_t *destination)
{
    if (destination != NULL) {
        destination->revision++;
    }
}

ngx_str_t *
ngx_media_destination_strdup(ngx_pool_t *pool, const ngx_str_t *src)
{
    ngx_str_t  *dst;

    if (src == NULL || src->len == 0) {
        return NULL;
    }

    dst = ngx_pcalloc(pool, sizeof(ngx_str_t));
    if (dst == NULL) {
        return NULL;
    }

    dst->data = ngx_pnalloc(pool, src->len + 1);
    if (dst->data == NULL) {
        return NULL;
    }

    ngx_memcpy(dst->data, src->data, src->len);
    dst->data[src->len] = '\0';
    dst->len = src->len;

    return dst;
}

ngx_media_destination_t *
ngx_media_destination_add(ngx_media_stream_t *stream, const ngx_str_t *id,
    ngx_uint_t type, ngx_log_t *log)
{
    ngx_media_destination_t  *destination;
    ngx_str_t                *copy;

    /* the model allocates from the stream's pool, so no logger is needed */
    (void) log;

    if (stream == NULL || id == NULL || id->len == 0) {
        return NULL;
    }

    if (ngx_media_destination_find(stream, id) != NULL) {
        return NULL;
    }

    destination = ngx_pcalloc(stream->pool, sizeof(ngx_media_destination_t));
    if (destination == NULL) {
        return NULL;
    }

    copy = ngx_media_destination_strdup(stream->pool, id);
    if (copy == NULL) {
        return NULL;
    }

    destination->id = *copy;
    destination->stream = stream;
    destination->type = type;
    destination->enabled = 1;

    ngx_queue_insert_tail(&stream->destinations, &destination->queue);

    ngx_media_stream_touch(stream);

    return destination;
}

ngx_media_destination_t *
ngx_media_destination_find(ngx_media_stream_t *stream, const ngx_str_t *id)
{
    ngx_queue_t              *q;
    ngx_media_destination_t  *destination;

    if (stream == NULL || id == NULL) {
        return NULL;
    }

    for (q = ngx_queue_head(&stream->destinations);
         q != (ngx_queue_t *) &stream->destinations;
         q = q->next)
    {
        destination = ngx_queue_data(q, ngx_media_destination_t, queue);

        if (destination->id.len == id->len
            && ngx_memcmp(destination->id.data, id->data, id->len) == 0)
        {
            return destination;
        }
    }

    return NULL;
}

ngx_uint_t
ngx_media_destination_count(const ngx_media_stream_t *stream)
{
    ngx_queue_t  *q;
    ngx_uint_t    count = 0;

    if (stream == NULL) {
        return 0;
    }

    for (q = ngx_queue_head(&stream->destinations);
         q != (ngx_queue_t *) &stream->destinations;
         q = q->next)
    {
        count++;
    }

    return count;
}

static ngx_uint_t
ngx_media_destination_egress_engine(ngx_uint_t type)
{
    switch (type) {
    case NGX_MEDIA_DEST_SRT:
        return NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD;
    case NGX_MEDIA_DEST_RTMP:
        return NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP;
    case NGX_MEDIA_DEST_HLS_PUSH:
        return NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL;
    default:
        return NGX_MEDIA_EGRESS_ENGINE_SHARED;
    }
}
ngx_int_t
ngx_media_destination_start(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination, ngx_log_t *log)
{
    const ngx_media_destination_ops_t  *ops;
    ngx_media_egress_descriptor_t       descriptor;
    uint64_t                            token;
    ngx_uint_t                          engine;

    if (stream == NULL || destination == NULL || !destination->enabled) {
        return NGX_ERROR;
    }

    if (destination->impl != NULL || destination->egress_token != 0) {
        return NGX_OK;
    }

    ops = ngx_media_destination_backend(destination->type);

    /*
     * No backend means the type was accepted by the control API but nothing
     * in this build can carry it.  The model does not log, so it stays usable
     * from hosts without nginx's logging.
     */
    if (ops == NULL) {
        return NGX_ERROR;
    }

    engine = ngx_media_destination_egress_engine(destination->type);
    if (engine == NGX_MEDIA_EGRESS_ENGINE_SHARED) {
        return NGX_ERROR;
    }

    ngx_memzero(&descriptor, sizeof(descriptor));
    descriptor.application = stream->application;
    descriptor.stream = stream->name;
    descriptor.destination = destination->id;
    descriptor.stream_incarnation = stream->incarnation;
    descriptor.representation_id = stream->incarnation;
    descriptor.representation_epoch = stream->generation;
    descriptor.feed_id = stream->incarnation;
    descriptor.feed_epoch = stream->program_feed.generation;
    descriptor.protocol = destination->type;
    descriptor.engine = engine;
    descriptor.deadline_msec = destination->segment_duration_ms;

    if (ngx_media_egress_manager_admit(&descriptor, &token, log) != NGX_OK) {
        return NGX_ERROR;
    }

    destination->egress_token = token;

    if (ops->add(stream, destination, log) != NGX_OK) {
        ngx_media_egress_manager_release(token);
        destination->egress_token = 0;
        return NGX_ERROR;
    }

    return NGX_OK;
}

/*
 * Ordered teardown: stop the transport before dropping the object, so a
 * sender thread is never left writing into a destination that no longer
 * exists.  The object itself is pool-allocated and released with the stream.
 */
void
ngx_media_destination_remove(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination)
{
    const ngx_media_destination_ops_t  *ops;

    if (stream == NULL || destination == NULL) {
        return;
    }

    ops = ngx_media_destination_backend(destination->type);

    if (ops != NULL && destination->impl != NULL) {
        ops->remove(stream, destination);
    }
    ngx_media_egress_manager_release(destination->egress_token);
    destination->egress_token = 0;

    destination->impl = NULL;
    destination->stream = NULL;

    ngx_queue_remove(&destination->queue);

    ngx_media_stream_touch(stream);
}

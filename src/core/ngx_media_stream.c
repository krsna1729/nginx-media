#include "ngx_media_stream.h"
#include "ngx_media_timeline.h"

typedef struct {
    ngx_media_stream_t  *stream;
    ngx_media_source_t  *source;
    ngx_msec_t           now;
} ngx_media_stream_replay_t;

static ngx_int_t ngx_media_stream_write_program(ngx_media_stream_t *stream,
    ngx_media_source_t *source, const ngx_media_frame_t *frame, ngx_msec_t now);
static ngx_int_t ngx_media_stream_switch_now(ngx_media_stream_t *stream,
    ngx_media_source_t *source, ngx_msec_t now);
static ngx_int_t ngx_media_stream_replay_frame(void *ctx,
    const ngx_media_frame_t *frame);

void
ngx_media_stream_set_policy(ngx_media_stream_t *stream,
    const ngx_media_policy_t *policy)
{
    if (stream == NULL || policy == NULL) {
        return;
    }

    stream->selector.failure_timeout = policy->failure_timeout;
    stream->selector.recovery_timeout = policy->recovery_timeout;
    stream->selector.switch_keyframe = policy->switch_keyframe;
    stream->selector.switchback = policy->switchback;
}

/* every mutation of the desired state bumps the object revision */
void
ngx_media_stream_touch(ngx_media_stream_t *stream)
{
    if (stream != NULL) {
        stream->revision++;
    }
}

ngx_int_t
ngx_media_stream_init(ngx_media_stream_t *stream, ngx_pool_t *pool,
    ngx_log_t *log, const ngx_str_t *application, const ngx_str_t *name,
    const ngx_media_feed_conf_t *feed_conf)
{
    if (stream == NULL || pool == NULL || name == NULL || name->len == 0) {
        return NGX_ERROR;
    }

    ngx_media_policy_t  defaults;

    ngx_memzero(stream, sizeof(ngx_media_stream_t));

    ngx_media_policy_init(&defaults);
    ngx_media_stream_set_policy(stream, &defaults);

    stream->pool = pool;

    stream->name.data = ngx_pnalloc(pool, name->len);
    if (stream->name.data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(stream->name.data, name->data, name->len);
    stream->name.len = name->len;

    if (application != NULL && application->len > 0) {
        stream->application.data = ngx_pnalloc(pool, application->len);

        if (stream->application.data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(stream->application.data, application->data,
                   application->len);
        stream->application.len = application->len;
    }

    ngx_queue_init(&stream->sources);
    ngx_queue_init(&stream->consumers);

    ngx_media_timeline_init(&stream->timeline);

    if (ngx_media_feed_init(&stream->program_feed, feed_conf, log) != NGX_OK) {
        return NGX_ERROR;
    }

    stream->generation = 1;
    stream->running = 1;

    return NGX_OK;
}

void
ngx_media_stream_destroy(ngx_media_stream_t *stream)
{
    ngx_queue_t  *q, *next;

    if (stream == NULL || stream->pool == NULL) {
        return;
    }

    for (q = ngx_queue_head(&stream->sources);
         q != (ngx_queue_t *) &stream->sources;
         q = next)
    {
        next = q->next;

        {
            ngx_media_source_t  *source
                = ngx_queue_data(q, ngx_media_source_t, queue);

            ngx_media_source_preroll_destroy(source);
            ngx_media_source_tracks_destroy(source);
        }
    }

    ngx_queue_init(&stream->sources);

    ngx_media_feed_destroy(&stream->program_feed);

    stream->active = NULL;
    stream->running = 0;
}

ngx_media_source_t *
ngx_media_stream_source_add(ngx_media_stream_t *stream, const ngx_str_t *id,
    ngx_uint_t type, ngx_uint_t priority, ngx_log_t *log)
{
    ngx_media_source_t  *source;

    if (stream == NULL || id == NULL || id->len == 0) {
        return NULL;
    }

    if (ngx_media_stream_source_find(stream, id) != NULL) {
        return NULL;
    }

    source = ngx_pcalloc(stream->pool, sizeof(ngx_media_source_t));
    if (source == NULL) {
        return NULL;
    }

    source->id.data = ngx_pnalloc(stream->pool, id->len);
    if (source->id.data == NULL) {
        return NULL;
    }

    ngx_memcpy(source->id.data, id->data, id->len);
    source->id.len = id->len;

    source->stream = stream;
    source->type = type;
    source->priority = priority;
    source->enabled = 1;
    source->state = NGX_MEDIA_SOURCE_STANDBY;

    if (ngx_media_source_preroll_init(source, NGX_MEDIA_PREROLL_DEFAULT_UNITS,
                                      NGX_MEDIA_PREROLL_DEFAULT_BYTES, log)
        != NGX_OK)
    {
        return NULL;
    }

    ngx_queue_insert_tail(&stream->sources, &source->queue);

    return source;
}

ngx_media_source_t *
ngx_media_stream_source_find(ngx_media_stream_t *stream, const ngx_str_t *id)
{
    ngx_queue_t         *q;
    ngx_media_source_t  *source;

    if (stream == NULL || id == NULL) {
        return NULL;
    }

    for (q = ngx_queue_head(&stream->sources);
         q != (ngx_queue_t *) &stream->sources;
         q = q->next)
    {
        source = ngx_queue_data(q, ngx_media_source_t, queue);

        if (source->id.len == id->len
            && ngx_memcmp(source->id.data, id->data, id->len) == 0)
        {
            return source;
        }
    }

    return NULL;
}

ngx_uint_t
ngx_media_stream_source_count(const ngx_media_stream_t *stream)
{
    ngx_queue_t  *q;
    ngx_uint_t    n;

    if (stream == NULL) {
        return 0;
    }

    n = 0;

    for (q = ngx_queue_head((ngx_queue_t *) &stream->sources);
         q != (ngx_queue_t *) &stream->sources;
         q = q->next)
    {
        n++;
    }

    return n;
}

void
ngx_media_stream_source_remove(ngx_media_stream_t *stream,
    ngx_media_source_t *source)
{
    if (stream == NULL || source == NULL || source->stream != stream) {
        return;
    }

    if (source->writers > 0) {
        /* in-flight writers must drain before destruction */
        source->pending_remove = 1;
        return;
    }

    if (stream->active == source) {
        stream->active = NULL;
        source->active = 0;
        source->state = NGX_MEDIA_SOURCE_STANDBY;

        /*
         * The program idles until a replacement is promoted; consumers see an
         * explicit discontinuity rather than a silent gap.
         */
        ngx_media_timeline_discontinuity(&stream->timeline);
        ngx_media_feed_discontinuity(&stream->program_feed);
        stream->generation++;
    }

    ngx_media_source_preroll_destroy(source);
    ngx_media_source_tracks_destroy(source);

    ngx_queue_remove(&source->queue);

    source->stream = NULL;
}

ngx_int_t
ngx_media_stream_lease_begin(ngx_media_stream_t *stream,
    ngx_media_source_t *source)
{
    if (stream == NULL || source == NULL || source->stream != stream) {
        return NGX_ERROR;
    }

    ngx_media_source_lease_begin(source);

    return NGX_OK;
}

void
ngx_media_stream_lease_end(ngx_media_stream_t *stream,
    ngx_media_source_t *source)
{
    if (stream == NULL || source == NULL) {
        return;
    }

    ngx_media_source_lease_end(source);
}

ngx_int_t
ngx_media_stream_publish(ngx_media_stream_t *stream,
    ngx_media_source_t *source, const ngx_media_frame_t *frame, ngx_msec_t now)
{
    ngx_int_t  rc;

    if (stream == NULL || source == NULL || frame == NULL
        || source->stream != stream)
    {
        return NGX_ERROR;
    }

    source->frames_in++;
    source->last_media = now;

    if (source->state == NGX_MEDIA_SOURCE_ACTIVE) {

        (void) ngx_media_stream_lease_begin(stream, source);

        rc = ngx_media_stream_write_program(stream, source, frame, now);

        ngx_media_stream_lease_end(stream, source);

        ngx_media_stream_switch_resolve(stream);

        return rc;
    }

    /* standby: keep the complete-GOP cache hot for an immediate switch */
    (void) ngx_media_source_preroll_push(source, frame);

    if (source->state == NGX_MEDIA_SOURCE_AWAITING_SYNC && frame->keyframe) {
        /* a fresh decodable boundary arrived while the switch was pending */
        return ngx_media_stream_switch_now(stream, source, now);
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_stream_promote(ngx_media_stream_t *stream,
    ngx_media_source_t *source)
{
    if (stream == NULL || source == NULL || source->stream != stream) {
        return NGX_ERROR;
    }

    if (stream->active == source) {
        source->pending_switch = 0;
        return NGX_OK;
    }

    source->pending_switch = 1;
    source->state = NGX_MEDIA_SOURCE_AWAITING_SYNC;

    /*
     * When the standby already holds a decodable boundary the switch can
     * complete immediately from the cached GOP instead of waiting for the
     * next keyframe.
     */
    if (ngx_media_source_preroll_ready(source)) {
        return ngx_media_stream_switch_now(stream, source, source->last_media);
    }

    return NGX_OK;
}

void
ngx_media_stream_switch_resolve(ngx_media_stream_t *stream)
{
    ngx_queue_t         *q, *next;
    ngx_media_source_t  *source;

    if (stream == NULL) {
        return;
    }

    for (q = ngx_queue_head(&stream->sources);
         q != (ngx_queue_t *) &stream->sources;
         q = next)
    {
        next = q->next;

        source = ngx_queue_data(q, ngx_media_source_t, queue);

        if (source->pending_remove && source->writers == 0) {
            source->pending_remove = 0;
            ngx_media_stream_source_remove(stream, source);
            continue;
        }

        if (source->pending_switch
            && source->state == NGX_MEDIA_SOURCE_AWAITING_SYNC
            && ngx_media_source_preroll_ready(source))
        {
            (void) ngx_media_stream_switch_now(stream, source,
                                               source->last_media);
        }
    }
}

static ngx_int_t
ngx_media_stream_replay_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_stream_replay_t  *replay = ctx;

    return ngx_media_stream_write_program(replay->stream, replay->source,
                                          frame, replay->now);
}

static ngx_int_t
ngx_media_stream_switch_now(ngx_media_stream_t *stream,
    ngx_media_source_t *source, ngx_msec_t now)
{
    ngx_media_source_t        *old;
    ngx_media_stream_replay_t  replay;

    old = stream->active;

    if (old == source) {
        source->pending_switch = 0;
        return NGX_OK;
    }

    if (old != NULL && old->writers > 0) {
        /* the demoted source still has frames in flight: retry after drain */
        return NGX_AGAIN;
    }

    if (old != NULL) {
        old->state = NGX_MEDIA_SOURCE_STANDBY;
        old->active = 0;

        /*
         * A real source change: consumers must see an explicit generation
         * change rather than a silent splice.  The first activation of a
         * stream is not a switch.
         */
        ngx_media_timeline_discontinuity(&stream->timeline);
        ngx_media_feed_discontinuity(&stream->program_feed);
        stream->generation++;
        stream->switches++;
    }

    source->state = NGX_MEDIA_SOURCE_ACTIVE;
    source->active = 1;
    source->pending_switch = 0;
    stream->active = source;

    /* replay the cached GOP so the program continues from a decodable point */
    replay.stream = stream;
    replay.source = source;
    replay.now = now;

    ngx_media_source_preroll_replay(source, ngx_media_stream_replay_frame,
                                    &replay);
    ngx_media_source_preroll_reset(source);

    return NGX_OK;
}

static ngx_int_t
ngx_media_stream_write_program(ngx_media_stream_t *stream,
    ngx_media_source_t *source, const ngx_media_frame_t *frame, ngx_msec_t now)
{
    ngx_media_frame_t  mapped;
    int64_t            pts, dts;

    if (ngx_media_timeline_map(&stream->timeline, frame->pts, frame->dts,
                               &pts, &dts) != NGX_OK)
    {
        return NGX_ERROR;
    }

    mapped = *frame;
    mapped.pts = pts;
    mapped.dts = dts;

    if (ngx_media_feed_publish(&stream->program_feed, &mapped, now) != NGX_OK) {
        return NGX_ERROR;
    }

    source->frames_out++;
    stream->program_frames++;

    return NGX_OK;
}

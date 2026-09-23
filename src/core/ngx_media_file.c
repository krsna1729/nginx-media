#include "ngx_media_file.h"
#include "ngx_media_health.h"
#include "ngx_media_runtime.h"
#include "ngx_media_stream.h"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#define NGX_MEDIA_FILE_CHUNK  65536

static ngx_media_file_source_t  *ngx_media_file_all;
static pthread_mutex_t           ngx_media_file_mutex = PTHREAD_MUTEX_INITIALIZER;

static void
ngx_media_file_sink_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_media_file_source_t  *source = ctx;

    if (source == NULL || source->source == NULL || tracks == NULL) {
        return;
    }

    (void) ngx_media_source_tracks_set(source->source, tracks,
                                       source->file.log);
    (void) ngx_media_health_tracks(&source->source->health, 1);
}

static void
ngx_media_file_sink_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_file_source_t  *source = ctx;

    if (source == NULL || source->stream == NULL || source->source == NULL) {
        return;
    }

    /*
     * A file is a source like any other: it goes through the same health
     * model and the same gate, so selection and compatibility need to know
     * nothing about where the bytes came from.
     */
    ngx_media_health_media(&source->source->health, frame->dts,
                           ngx_current_msec);

    ngx_media_runtime_iso_source(source->stream, source->source, frame);

    (void) ngx_media_stream_publish(source->stream, source->source, frame,
                                    ngx_current_msec);

    source->frames++;
}

ngx_media_file_source_t *
ngx_media_file_open(ngx_media_stream_t *stream, const ngx_str_t *id,
    const ngx_str_t *path, ngx_uint_t mode, ngx_log_t *log)
{
    ngx_media_file_source_t  *source;
    ngx_media_ts_sink_t       sink;
    ngx_str_t                *copy;

    if (stream == NULL || id == NULL || id->len == 0 || path == NULL
        || path->len == 0)
    {
        return NULL;
    }

    source = ngx_pcalloc(stream->pool, sizeof(ngx_media_file_source_t));
    if (source == NULL) {
        return NULL;
    }

    copy = ngx_pcalloc(stream->pool, sizeof(ngx_str_t));
    if (copy == NULL) {
        return NULL;
    }

    copy->data = ngx_pnalloc(stream->pool, path->len + 1);
    if (copy->data == NULL) {
        return NULL;
    }

    ngx_memcpy(copy->data, path->data, path->len);
    copy->data[path->len] = '\0';
    copy->len = path->len;

    source->path = *copy;
    source->stream = stream;
    source->mode = mode;

    /* the source exists before any media does, like a waiting publisher */
    source->source = ngx_media_stream_source_add(stream, id,
                                                 NGX_MEDIA_SOURCE_FILE, 0,
                                                 log);
    if (source->source == NULL) {
        return NULL;
    }

    source->file.fd = ngx_open_file(copy->data, NGX_FILE_RDONLY,
                                    NGX_FILE_OPEN, 0);
    if (source->file.fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "media: could not open file source \"%V\"", &source->path);
        ngx_media_file_close(source);
        return NULL;
    }

    source->file.log = log;
    source->file.name = source->path;

    sink.tracks = ngx_media_file_sink_tracks;
    sink.frame = ngx_media_file_sink_frame;

    if (ngx_media_ts_demux_init(&source->demux, NULL, &sink, source, log)
        != NGX_OK)
    {
        ngx_media_file_close(source);
        return NULL;
    }

    /*
     * Health has to be initialised before the first transport report.  A
     * zeroed health struct has required == 0 and recovery_timeout == 0, so
     * evaluate() marks the source healthy and eligible on its first pass and
     * a hard failure survives less than one tick - the selector would never
     * fail over away from a source that is plainly dead.
     */
    ngx_media_health_init(&source->source->health, &stream->selector, ngx_current_msec);
    source->source->health.failure_timeout = NGX_MEDIA_BURSTY_FAILURE_TIMEOUT;
    ngx_media_health_transport(&source->source->health, 1, ngx_current_msec);
    ngx_media_source_touch(source->source);

    source->chunk = ngx_pnalloc(stream->pool, NGX_MEDIA_FILE_CHUNK);

    if (source->chunk == NULL) {
        ngx_media_file_close(source);
        return NULL;
    }

    (void) pthread_mutex_lock(&ngx_media_file_mutex);
    source->next = ngx_media_file_all;
    ngx_media_file_all = source;
    (void) pthread_mutex_unlock(&ngx_media_file_mutex);

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: file source %V opened for %V/%V from %V", id,
                  &stream->application, &stream->name, &source->path);

    return source;
}

ngx_int_t
ngx_media_file_advance(ngx_media_file_source_t *source, ngx_log_t *log)
{
    u_char    *buf;
    ssize_t    n;

    if (source == NULL) {
        return NGX_ERROR;
    }

    if (source->finished) {
        return NGX_DONE;
    }

    buf = source->chunk;

    if (buf == NULL) {
        return NGX_ERROR;
    }

    n = ngx_read_fd(source->file.fd, buf, NGX_MEDIA_FILE_CHUNK);

    if (n == 0) {

        if (source->mode == NGX_MEDIA_FILE_LOOP) {

            if (lseek(source->file.fd, 0, SEEK_SET) == (off_t) -1) {
                source->finished = 1;
                return NGX_ERROR;
            }

            ngx_log_error(NGX_LOG_INFO, log, 0,
                          "media: file source %V looping", &source->path);

            return NGX_OK;
        }

        /* the tail may hold a partial access unit */
        ngx_media_ts_demux_flush(&source->demux);
        source->finished = 1;

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: file source %V finished (%uL frames)",
                      &source->path, source->frames);

        return NGX_DONE;
    }

    if (n < 0) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "media: file source %V read failed", &source->path);
        source->finished = 1;
        return NGX_ERROR;
    }

    source->bytes_read += (uint64_t) n;

    if (ngx_media_ts_demux_feed(&source->demux, buf, (size_t) n) != NGX_OK) {
        source->finished = 1;
        return NGX_ERROR;
    }

    return NGX_OK;
}

/*
 * Source removal publishes pending_remove before detaching the source.  The
 * reader owns its source pointer until close joins it, so the atomic flag is
 * the only lifetime check needed here.
 */
static ngx_uint_t
ngx_media_file_removed(ngx_media_file_source_t *source)
{
    return (source->source == NULL
            || ngx_atomic_fetch_add(&source->source->pending_remove, 0));
}

/*
 * Ordered teardown, and safe to repeat: the unlink finds nothing the second
 * time, the descriptor is NGX_INVALID_FILE after the first close, the demux
 * is zeroed by destroy() and the source pointer is NULLed here, so closing a
 * reader that has already finished - or already been closed - is a no-op.
 */
void
ngx_media_file_close(ngx_media_file_source_t *source)
{
    ngx_media_file_source_t **link;

    if (source == NULL) {
        return;
    }

    (void) pthread_mutex_lock(&ngx_media_file_mutex);

    for (link = &ngx_media_file_all; *link != NULL; link = &(*link)->next) {

        if (*link == source) {
            *link = source->next;
            break;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_file_mutex);

    if (source->file.fd != NGX_INVALID_FILE) {
        (void) ngx_close_file(source->file.fd);
        source->file.fd = NGX_INVALID_FILE;
    }

    ngx_media_ts_demux_destroy(&source->demux);

    if (source->stream != NULL && source->source != NULL) {
        ngx_media_stream_source_remove(source->stream, source->source);
        source->source = NULL;
    }

    source->finished = 1;
}

void
ngx_media_file_close_stream(ngx_media_stream_t *stream)
{
    ngx_media_file_source_t  *source, *found;

    if (stream == NULL) {
        return;
    }

    for ( ;; ) {
        found = NULL;

        (void) pthread_mutex_lock(&ngx_media_file_mutex);

        for (source = ngx_media_file_all; source != NULL;
             source = source->next)
        {
            if (source->stream == stream) {
                found = source;
                break;
            }
        }

        (void) pthread_mutex_unlock(&ngx_media_file_mutex);

        if (found == NULL) {
            return;
        }

        /*
         * close() unlinks the reader, so the next pass finds the rest rather
         * than the one just released.
         */
        ngx_log_error(NGX_LOG_NOTICE, found->file.log, 0,
                      "media: file source %V removed with its stream, "
                      "closing its reader", &found->path);

        ngx_media_file_close(found);
    }
}

void
ngx_media_file_advance_all(ngx_log_t *log)
{
    ngx_media_file_source_t **link;
    ngx_media_file_source_t  *source, *removed, *next;

    removed = NULL;

    (void) pthread_mutex_lock(&ngx_media_file_mutex);

    for (link = &ngx_media_file_all; *link != NULL; ) {
        source = *link;

        if (ngx_media_file_removed(source)) {
            /*
             * Splice the reader off the registry for teardown below:
             * close() takes this mutex and would deadlock here, and a source
             * being destroyed must not be walked by the next tick.  The link
             * is reused for the local list, which nothing else sees.
             */
            *link = source->next;
            source->next = removed;
            removed = source;
            continue;
        }

        if (!source->finished) {
            (void) ngx_media_file_advance(source, log);
        }

        link = &source->next;
    }

    (void) pthread_mutex_unlock(&ngx_media_file_mutex);

    /*
     * The reader is already off the registry, so this is the ordered teardown
     * of a stopped reader: close() releases the descriptor, the demuxer and
     * the source itself.  Without it a deleted file source stayed linked and
     * was still advanced by every tick, reading a file no one owns.
     */
    while (removed != NULL) {
        source = removed;
        next = removed->next;

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: file source %V removed, closing its reader",
                      &source->path);

        ngx_media_file_close(source);

        removed = next;
    }
}

ngx_uint_t
ngx_media_file_count(void)
{
    ngx_media_file_source_t  *source;
    ngx_uint_t                count = 0;

    (void) pthread_mutex_lock(&ngx_media_file_mutex);

    for (source = ngx_media_file_all; source != NULL; source = source->next) {
        count++;
    }

    (void) pthread_mutex_unlock(&ngx_media_file_mutex);

    return count;
}

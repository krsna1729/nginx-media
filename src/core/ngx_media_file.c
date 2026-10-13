#include "ngx_media_file.h"
#include "ngx_media_health.h"
#include "ngx_media_runtime.h"
#include "ngx_media_stream.h"

#include <fcntl.h>
#include <unistd.h>

#define NGX_MEDIA_FILE_CHUNK  65536

static ngx_media_file_source_t  *ngx_media_file_all;
/* File readers, like the model, are owned exclusively by the worker loop. */

static void
ngx_media_file_sink_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_media_file_source_t  *source = ctx;

    if (source == NULL || source->source == NULL || tracks == NULL
        || ngx_atomic_fetch_add(&source->source->pending_remove, 0))
    {
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

    if (source == NULL || source->stream == NULL || source->source == NULL
        || ngx_atomic_fetch_add(&source->source->pending_remove, 0))
    {
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

    if (stream == NULL || !stream->running || id == NULL || id->len == 0
        || path == NULL || path->len == 0
        || !ngx_media_model_available(NGX_MEDIA_MODEL_SOURCE))
    {
        return NULL;
    }

    source = ngx_alloc(sizeof(ngx_media_file_source_t), log);
    if (source == NULL) {
        return NULL;
    }
    ngx_memzero(source, sizeof(*source));
    source->file.fd = NGX_INVALID_FILE;
    if (ngx_media_string_set(&source->path, path, log) != NGX_OK) {
        ngx_free(source);
        return NULL;
    }
    source->stream = stream;
    source->mode = mode;

    /* the source exists before any media does, like a waiting publisher */
    source->source = ngx_media_stream_source_add(stream, id,
                                                 NGX_MEDIA_SOURCE_FILE, 0,
                                                 log);
    if (source->source == NULL) {
        ngx_media_file_close(source);
        return NULL;
    }
    ngx_media_source_retain(source->source);

    /*
     * Opened non-blocking, then checked: open(2) on a FIFO with no writer, or
     * a device that waits for its hardware, blocks the calling nginx worker -
     * every connection on it - until something happens on the other end.  The
     * file is read synchronously on the event loop, so only a regular file
     * is a source.
     */
    source->file.fd = ngx_open_file(source->path.data,
                                    NGX_FILE_RDONLY|NGX_FILE_NONBLOCK,
                                    NGX_FILE_OPEN, 0);
    if (source->file.fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "media: could not open file source \"%V\"", &source->path);
        ngx_media_file_close(source);
        return NULL;
    }

    {
        ngx_file_info_t  fi;

        if (ngx_fd_info(source->file.fd, &fi) == NGX_FILE_ERROR
            || !ngx_is_file(&fi))
        {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "media: file source \"%V\" is not a regular file",
                          &source->path);
            ngx_media_file_close(source);
            return NULL;
        }
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

    source->chunk = ngx_alloc(NGX_MEDIA_FILE_CHUNK, log);

    if (source->chunk == NULL) {
        ngx_media_file_close(source);
        return NULL;
    }

    source->next = ngx_media_file_all;
    ngx_media_file_all = source;

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

/* The reader holds a source reference until close releases the reader. */
static ngx_uint_t
ngx_media_file_removed(ngx_media_file_source_t *source)
{
    return (source->source == NULL
            || ngx_atomic_fetch_add(&source->source->pending_remove, 0));
}

/* Close consumes the reader pointer; callers must not reuse it. */
void
ngx_media_file_close(ngx_media_file_source_t *source)
{
    ngx_media_file_source_t **link;

    if (source == NULL) {
        return;
    }


    for (link = &ngx_media_file_all; *link != NULL; link = &(*link)->next) {

        if (*link == source) {
            *link = source->next;
            break;
        }
    }


    if (source->file.fd != NGX_INVALID_FILE) {
        (void) ngx_close_file(source->file.fd);
        source->file.fd = NGX_INVALID_FILE;
    }

    ngx_media_ts_demux_destroy(&source->demux);

    if (source->stream != NULL && source->source != NULL) {
        ngx_media_stream_source_remove(source->stream, source->source);
        ngx_media_source_release(source->source);
        source->source = NULL;
    }

    ngx_free(source->chunk);
    ngx_free(source->path.data);
    ngx_free(source);
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


        for (source = ngx_media_file_all; source != NULL;
             source = source->next)
        {
            if (source->stream == stream) {
                found = source;
                break;
            }
        }


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
    ngx_media_file_source_t *source, *next;

    for (source = ngx_media_file_all; source != NULL; source = next) {
        next = source->next;
        if (ngx_media_file_removed(source)) {
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "media: file source %V removed, closing its reader",
                          &source->path);
            ngx_media_file_close(source);
        } else if (!source->finished) {
            (void) ngx_media_file_advance(source, log);
        }
    }
}

ngx_uint_t
ngx_media_file_count(void)
{
    ngx_media_file_source_t  *source;
    ngx_uint_t                count = 0;


    for (source = ngx_media_file_all; source != NULL; source = source->next) {
        count++;
    }


    return count;
}

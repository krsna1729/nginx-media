#include "ngx_media_hls_ingest.h"
#include "ngx_media_health.h"
#include "ngx_media_runtime.h"
#include "ngx_media_stream.h"

#include <dirent.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NGX_MEDIA_HLS_INGEST_SEGMENT_MAX  (8 * 1024 * 1024)
#define NGX_MEDIA_HLS_INGEST_SEEN         256
#define NGX_MEDIA_HLS_INGEST_NAME_MAX     256

struct ngx_media_hls_ingest_source_s {
    ngx_media_stream_t      *stream;
    ngx_media_source_t      *source;
    ngx_media_ts_demux_t     demux;

    ngx_str_t                directory;

    pthread_t                thread;
    ngx_uint_t               thread_started;
    ngx_uint_t               stopping;

    /*
     * Set by the reader thread as its last act, after the loop and before it
     * returns, so the reaper knows the thread has stopped touching this
     * reader and joining it cannot block.  Without it a reap could park on a
     * thread that is still asleep between directory scans.  An atomic flag
     * read by the reaper is the same idiom the SRT session uses for
     * close_requested: the join that follows is what actually publishes the
     * reader's writes.
     */
    ngx_atomic_t             exited;

    /*
     * Names already read.  A directory is not ordered by arrival, so what
     * matters is that each name is read exactly once, not that the reader
     * keeps up with the uploader.
     */
    u_char                   seen[NGX_MEDIA_HLS_INGEST_SEEN][NGX_MEDIA_HLS_INGEST_NAME_MAX];
    ngx_uint_t               nseen;

    uint64_t                 segments;
    uint64_t                 frames;
    uint64_t                 failures;

    struct ngx_media_hls_ingest_source_s  *next;
};

static ngx_media_hls_ingest_source_t  *ngx_media_hls_ingest_all;
static pthread_mutex_t                 ngx_media_hls_ingest_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static void
ngx_media_hls_ingest_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_media_hls_ingest_source_t  *ingest = ctx;

    if (ingest == NULL || ingest->source == NULL || tracks == NULL) {
        return;
    }

    (void) ngx_media_source_tracks_set(ingest->source, tracks, NULL);
    (void) ngx_media_health_tracks(&ingest->source->health, 1);
}

static void
ngx_media_hls_ingest_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_hls_ingest_source_t  *ingest = ctx;

    if (ingest == NULL || ingest->stream == NULL || ingest->source == NULL) {
        return;
    }

    /*
     * An uploaded segment is a source like any other, which is what makes
     * "make it eligible through the normal health model" true rather than
     * aspirational: nothing here is special-cased for uploads.
     */
    ngx_media_health_media(&ingest->source->health, frame->dts,
                           ngx_current_msec);

    ngx_media_runtime_iso_source(ingest->stream, ingest->source, frame);

    (void) ngx_media_stream_publish(ingest->stream, ingest->source, frame,
                                    ngx_current_msec);

    ingest->frames++;
}

/*
 * Whether this name has already been read.  Read-only on purpose.
 *
 * It used to record the name it was asked about, which made it a filter that
 * consumed what it filtered: a scan of the directory marked every name it
 * examined as read and returned only the lowest, so everything else was
 * skipped for good.  Recording happens where a name is actually chosen.
 */
static ngx_uint_t
ngx_media_hls_ingest_seen(ngx_media_hls_ingest_source_t *ingest,
    const u_char *name, size_t len)
{
    ngx_uint_t  i;

    for (i = 0; i < ingest->nseen; i++) {

        if (strlen((char *) ingest->seen[i]) == len
            && ngx_memcmp(ingest->seen[i], name, len) == 0)
        {
            return 1;
        }
    }

    return 0;
}

/* records a name as read; called only for the segment that is returned */
static void
ngx_media_hls_ingest_record(ngx_media_hls_ingest_source_t *ingest,
    const u_char *name, size_t len)
{
    if (ingest->nseen < NGX_MEDIA_HLS_INGEST_SEEN
        && len < NGX_MEDIA_HLS_INGEST_NAME_MAX)
    {
        ngx_memcpy(ingest->seen[ingest->nseen], name, len);
        ingest->seen[ingest->nseen][len] = '\0';
        ingest->nseen++;
    }
}

/*
 * Reads the directory and returns the first name not yet read.  Names are
 * compared lexically so segments arrive in the order the uploader wrote them,
 * which matters because the demuxer assembles access units across them.
 */
static ngx_int_t
ngx_media_hls_ingest_next(ngx_media_hls_ingest_source_t *ingest, u_char *out,
    size_t cap)
{
    DIR            *d;
    struct dirent  *de;
    u_char          best[NGX_MEDIA_HLS_INGEST_NAME_MAX];
    ngx_uint_t      found = 0;

    d = opendir((char *) ingest->directory.data);

    if (d == NULL) {
        return NGX_ERROR;
    }

    best[0] = '\0';

    while ((de = readdir(d)) != NULL) {

        size_t  len = strlen(de->d_name);

        if (len < 4 || len >= sizeof(best)
            || strcmp(de->d_name + len - 3, ".ts") != 0)
        {
            continue;
        }

        if (ngx_media_hls_ingest_seen(ingest, (u_char *) de->d_name, len)) {
            continue;
        }

        /* take the lowest remaining name; recording happens below, once */
        if (!found || strcmp(de->d_name, (char *) best) < 0) {
            ngx_memcpy(best, de->d_name, len + 1);
            found = 1;
        }
    }

    closedir(d);

    if (!found || strlen((char *) best) >= cap) {
        return NGX_DECLINED;
    }

    strcpy((char *) out, (char *) best);

    ngx_media_hls_ingest_record(ingest, out, strlen((char *) out));

    return NGX_OK;
}

/*
 * Whether the source behind this reader was removed through the control API.
 * source_remove() detaches it, or only flags it when a writer is in flight,
 * so both are checked.  This is the reader's own pointer, which only close()
 * clears, and close() cannot run while this thread is using the reader - it
 * joins the thread first.
 */
static ngx_uint_t
ngx_media_hls_ingest_removed(ngx_media_hls_ingest_source_t *ingest)
{
    return (ingest->source == NULL
            || ingest->source->stream == NULL
            || ingest->source->pending_remove);
}

static void *
ngx_media_hls_ingest_thread(void *data)
{
    ngx_media_hls_ingest_source_t  *ingest = data;
    u_char                          name[NGX_MEDIA_HLS_INGEST_NAME_MAX];
    u_char                          path[NGX_MEDIA_HLS_INGEST_NAME_MAX + 512];
    u_char                         *segment;
    FILE                           *file;
    size_t                          n, total;

    segment = ngx_pnalloc(ingest->stream->pool,
                          NGX_MEDIA_HLS_INGEST_SEGMENT_MAX);

    if (segment == NULL) {
        /* nothing is left running to touch the reader; the reaper may join */
        (void) ngx_atomic_cmp_set(&ingest->exited, 0, 1);
        return NULL;
    }

    while (!ingest->stopping) {

        if (ngx_media_hls_ingest_removed(ingest)) {
            /*
             * The source was removed through the control API, so there is
             * nothing left to publish into.  Leave the loop: the tick reaps
             * exited readers, and it cannot destroy this one while it is
             * still reading.
             */
            break;
        }

        if (ngx_media_hls_ingest_next(ingest, name, sizeof(name))
            != NGX_OK)
        {
            usleep(200000);
            continue;
        }

        if (snprintf((char *) path, sizeof(path), "%s/%s",
                     ingest->directory.data, name) >= (int) sizeof(path))
        {
            continue;
        }

        file = fopen((char *) path, "rb");

        if (file == NULL) {
            ingest->failures++;
            continue;
        }

        total = 0;

        while (total < NGX_MEDIA_HLS_INGEST_SEGMENT_MAX
               && (n = fread(segment + total, 1,
                             NGX_MEDIA_HLS_INGEST_SEGMENT_MAX - total,
                             file)) > 0)
        {
            total += n;
        }

        fclose(file);

        if (total == 0) {
            continue;
        }

        if (ngx_media_ts_demux_feed(&ingest->demux, segment, total) != NGX_OK) {
            ingest->failures++;
            continue;
        }

        ingest->segments++;
        (void) ngx_media_health_transport(&ingest->source->health, 1,
                                          ngx_current_msec);
    }

    /*
     * The loop has left the reader alone: nothing below touches the demux,
     * the source or the segment buffer.  Marking that here is what lets the
     * reaper join this thread from the tick without waiting on a directory
     * scan - the source was removed through the control API, so its reader
     * has to go with it instead of holding a thread and a directory
     * registration for the life of the worker.
     */
    (void) ngx_atomic_cmp_set(&ingest->exited, 0, 1);

    return NULL;
}

ngx_media_hls_ingest_source_t *
ngx_media_hls_ingest_open(ngx_media_stream_t *stream, const ngx_str_t *id,
    const ngx_str_t *directory, ngx_log_t *log)
{
    ngx_media_hls_ingest_source_t  *ingest;
    ngx_media_ts_sink_t             sink;
    ngx_str_t                      *copy;
    DIR                            *d;

    if (stream == NULL || id == NULL || id->len == 0 || directory == NULL
        || directory->len == 0)
    {
        return NULL;
    }

    /*
     * The directory is copied and NUL-terminated before anything opens it:
     * the caller's ngx_str_t is a slice of a request body, and opendir() needs
     * a C string.  Reading past it gives ENOENT for a directory that plainly
     * exists, which is how this was found.
     */
    ingest = ngx_pcalloc(stream->pool,
                         sizeof(ngx_media_hls_ingest_source_t));

    if (ingest == NULL) {
        return NULL;
    }

    copy = ngx_pcalloc(stream->pool, sizeof(ngx_str_t));

    if (copy == NULL) {
        return NULL;
    }

    copy->data = ngx_pnalloc(stream->pool, directory->len + 1);

    if (copy->data == NULL) {
        return NULL;
    }

    ngx_memcpy(copy->data, directory->data, directory->len);
    copy->data[directory->len] = '\0';
    copy->len = directory->len;

    ingest->directory = *copy;
    ingest->stream = stream;

    d = opendir((char *) copy->data);

    if (d == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "media: hls ingest directory \"%V\" is not readable",
                      directory);
        return NULL;
    }

    closedir(d);

    ingest->source = ngx_media_stream_source_add(stream, id,
                                                 NGX_MEDIA_SOURCE_HLS_PUSH, 0,
                                                 log);

    if (ingest->source == NULL) {
        return NULL;
    }

    sink.tracks = ngx_media_hls_ingest_tracks;
    sink.frame = ngx_media_hls_ingest_frame;

    if (ngx_media_ts_demux_init(&ingest->demux, NULL, &sink, ingest, log)
        != NGX_OK)
    {
        return NULL;
    }

    /*
     * Health has to be initialised before the first transport report.  A
     * zeroed health struct has required == 0 and recovery_timeout == 0, so
     * evaluate() marks the source healthy and eligible on its first pass and
     * a hard failure survives less than one tick - the selector would never
     * fail over away from a source that is plainly dead.
     */
    ngx_media_health_init(&ingest->source->health, &stream->selector, ngx_current_msec);
    ingest->source->health.failure_timeout = NGX_MEDIA_BURSTY_FAILURE_TIMEOUT;
    ngx_media_health_transport(&ingest->source->health, 1, ngx_current_msec);
    ngx_media_source_touch(ingest->source);

    (void) pthread_mutex_lock(&ngx_media_hls_ingest_mutex);
    ingest->next = ngx_media_hls_ingest_all;
    ngx_media_hls_ingest_all = ingest;
    (void) pthread_mutex_unlock(&ngx_media_hls_ingest_mutex);

    if (pthread_create(&ingest->thread, NULL, ngx_media_hls_ingest_thread,
                       ingest) != 0)
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: hls ingest %V could not start its reader", id);
        return NULL;
    }

    ingest->thread_started = 1;

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: hls ingest source %V opened for %V/%V from %V", id,
                  &stream->application, &stream->name, &ingest->directory);

    return ingest;
}

/*
 * Ordered teardown: stop the reader, join it, unlink it, release it.  Safe to
 * call more than once and safe on a reader that has already finished - the
 * join is guarded by thread_started, the unlink finds nothing the second
 * time, the demux is zeroed by destroy() and the source pointer is NULLed
 * here, so a repeated close is a no-op rather than a double free.
 */
void
ngx_media_hls_ingest_close(ngx_media_hls_ingest_source_t *ingest)
{
    ngx_media_hls_ingest_source_t **link;

    if (ingest == NULL) {
        return;
    }

    ingest->stopping = 1;

    if (ingest->thread_started) {
        (void) pthread_join(ingest->thread, NULL);
        ingest->thread_started = 0;
    }

    (void) pthread_mutex_lock(&ngx_media_hls_ingest_mutex);

    for (link = &ngx_media_hls_ingest_all; *link != NULL;
         link = &(*link)->next)
    {
        if (*link == ingest) {
            *link = ingest->next;
            break;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_ingest_mutex);

    ngx_media_ts_demux_destroy(&ingest->demux);

    if (ingest->stream != NULL && ingest->source != NULL) {
        ngx_media_stream_source_remove(ingest->stream, ingest->source);
        ingest->source = NULL;
    }
}

/*
 * Closes every ingest reader whose source was removed through the control API
 * and whose thread has already left its loop.  Called from the runtime tick.
 *
 * A removed source has to take its reader with it: the reader holds a thread,
 * a demuxer and an 8 MiB segment buffer, and frames it publishes are dropped
 * once the source is detached, so leaving it running leaks all of that once
 * per create/delete cycle.  The reader notices the removal in its own loop and
 * exits; this collects it.  Only exited readers are closed, so the join in
 * close() cannot block the tick on a slow directory scan.
 */
void
ngx_media_hls_ingest_reap(ngx_log_t *log)
{
    ngx_media_hls_ingest_source_t  *ingest;

    for ( ;; ) {
        ingest = NULL;

        (void) pthread_mutex_lock(&ngx_media_hls_ingest_mutex);

        for (ingest = ngx_media_hls_ingest_all; ingest != NULL;
             ingest = ingest->next)
        {
            if (ingest->exited && ngx_media_hls_ingest_removed(ingest)) {
                break;
            }
        }

        (void) pthread_mutex_unlock(&ngx_media_hls_ingest_mutex);

        if (ingest == NULL) {
            return;
        }

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: hls ingest source %V removed, closing its reader",
                      ingest->source != NULL ? &ingest->source->id
                                             : &ingest->directory);

        /* unlinks the reader, so the next pass cannot see it again */
        ngx_media_hls_ingest_close(ingest);
    }
}

void
ngx_media_hls_ingest_stop_all(void)
{
    ngx_media_hls_ingest_source_t  *ingest;

    (void) pthread_mutex_lock(&ngx_media_hls_ingest_mutex);

    for (ingest = ngx_media_hls_ingest_all; ingest != NULL;
         ingest = ingest->next)
    {
        ingest->stopping = 1;
    }

    for (ingest = ngx_media_hls_ingest_all; ingest != NULL;
         ingest = ingest->next)
    {
        if (ingest->thread_started) {
            (void) pthread_join(ingest->thread, NULL);
            ingest->thread_started = 0;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_ingest_mutex);
}

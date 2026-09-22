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
#define NGX_MEDIA_HLS_EVENT_CAPACITY      128
#define NGX_MEDIA_HLS_EVENT_MAX_TRACKS    NGX_MEDIA_TS_DEFAULT_TRACKS

enum {
    NGX_MEDIA_HLS_EVENT_TRACKS = 1,
    NGX_MEDIA_HLS_EVENT_FRAME,
    NGX_MEDIA_HLS_EVENT_TRANSPORT
};

typedef struct {
    ngx_uint_t  type;

    union {
        struct {
            ngx_media_track_t  tracks[NGX_MEDIA_HLS_EVENT_MAX_TRACKS];
            ngx_uint_t          count;
        } trackset;

        ngx_media_frame_t  frame;
        ngx_uint_t          healthy;
    } data;
} ngx_media_hls_event_t;


struct ngx_media_hls_ingest_source_s {
    ngx_media_stream_t      *stream;
    ngx_media_source_t      *source;
    ngx_media_ts_demux_t     demux;
    ngx_log_t               *log;

    /*
     * Set when a segment was read that carries nothing this build can carry,
     * so the reason is said once rather than on every segment.  The reader
     * thread owns it: it is written where the demux is fed and read nowhere
     * else.
     */
    ngx_uint_t               input_reported;

    ngx_str_t                directory;
    u_char                  *segment;

    ngx_media_hls_event_t   *events;
    ngx_uint_t               events_capacity;
    uint64_t                 events_head;
    uint64_t                 events_tail;
    pthread_mutex_t          events_mutex;
    ngx_uint_t               events_mutex_initialized;
    ngx_atomic_t             events_dropped;

    pthread_t                thread;
    ngx_uint_t               thread_started;
    ngx_atomic_t             stopping;

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
    ngx_uint_t               seen_next;
    uint64_t                 segments;
    uint64_t                 frames;
    uint64_t                 failures;

    struct ngx_media_hls_ingest_source_s  *next;
};

static ngx_media_hls_ingest_source_t  *ngx_media_hls_ingest_all;
static pthread_mutex_t                 ngx_media_hls_ingest_mutex =
    PTHREAD_MUTEX_INITIALIZER;
static void ngx_media_hls_ingest_event_release(
    ngx_media_hls_event_t *event);
static ngx_int_t ngx_media_hls_ingest_event_push(
    ngx_media_hls_ingest_source_t *ingest, ngx_media_hls_event_t *event);
static ngx_int_t ngx_media_hls_ingest_event_pop(
    ngx_media_hls_ingest_source_t *ingest, ngx_media_hls_event_t *event);
static void ngx_media_hls_ingest_event_clear(
    ngx_media_hls_ingest_source_t *ingest);
static void ngx_media_hls_ingest_event_drain(
    ngx_media_hls_ingest_source_t *ingest);
static void ngx_media_hls_ingest_transport(
    ngx_media_hls_ingest_source_t *ingest, ngx_uint_t healthy);
static ngx_uint_t ngx_media_hls_ingest_source_removed(
    ngx_media_hls_ingest_source_t *ingest);


static void
ngx_media_hls_ingest_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_media_hls_ingest_source_t  *ingest = ctx;
    ngx_media_hls_event_t            event;
    ngx_uint_t                       i;

    if (ingest == NULL || tracks == NULL
        || ngx_atomic_fetch_add(&ingest->stopping, 0)
        || tracks->count > NGX_MEDIA_HLS_EVENT_MAX_TRACKS)
    {
        return;
    }

    ngx_memzero(&event, sizeof(event));
    event.type = NGX_MEDIA_HLS_EVENT_TRACKS;
    event.data.trackset.count = tracks->count;

    for (i = 0; i < tracks->count; i++) {
        event.data.trackset.tracks[i] = tracks->tracks[i];
        (void) ngx_media_buf_ref(event.data.trackset.tracks[i].config);
    }

    (void) ngx_media_hls_ingest_event_push(ingest, &event);
}


static void
ngx_media_hls_ingest_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_hls_ingest_source_t  *ingest = ctx;
    ngx_media_hls_event_t            event;

    if (ingest == NULL || frame == NULL
        || ngx_atomic_fetch_add(&ingest->stopping, 0))
    {
        return;
    }

    ngx_memzero(&event, sizeof(event));
    event.type = NGX_MEDIA_HLS_EVENT_FRAME;

    if (ngx_media_frame_copy(&event.data.frame, frame) != NGX_OK) {
        return;
    }

    (void) ngx_media_hls_ingest_event_push(ingest, &event);
}
static void
ngx_media_hls_ingest_event_release(ngx_media_hls_event_t *event)
{
    ngx_uint_t  i;

    if (event == NULL) {
        return;
    }

    if (event->type == NGX_MEDIA_HLS_EVENT_TRACKS) {
        for (i = 0; i < event->data.trackset.count; i++) {
            ngx_media_buf_unref(event->data.trackset.tracks[i].config);
        }

    } else if (event->type == NGX_MEDIA_HLS_EVENT_FRAME) {
        ngx_media_frame_release(&event->data.frame);
    }

    ngx_memzero(event, sizeof(*event));
}

static ngx_int_t
ngx_media_hls_ingest_event_push(ngx_media_hls_ingest_source_t *ingest,
    ngx_media_hls_event_t *event)
{
    ngx_media_hls_event_t  *slot;

    if (ingest == NULL || event == NULL || ingest->events == NULL
        || !ingest->events_mutex_initialized)
    {
        ngx_media_hls_ingest_event_release(event);
        return NGX_ERROR;
    }

    (void) pthread_mutex_lock(&ingest->events_mutex);

    if (ingest->events_head - ingest->events_tail
        >= ingest->events_capacity)
    {
        (void) pthread_mutex_unlock(&ingest->events_mutex);
        (void) ngx_atomic_fetch_add(&ingest->events_dropped, 1);
        ngx_media_hls_ingest_event_release(event);
        return NGX_AGAIN;
    }

    slot = &ingest->events[ingest->events_head
                           % ingest->events_capacity];
    *slot = *event;
    ingest->events_head++;

    (void) pthread_mutex_unlock(&ingest->events_mutex);

    return NGX_OK;
}

static ngx_int_t
ngx_media_hls_ingest_event_pop(ngx_media_hls_ingest_source_t *ingest,
    ngx_media_hls_event_t *event)
{
    ngx_media_hls_event_t  *slot;

    if (ingest == NULL || event == NULL || ingest->events == NULL
        || !ingest->events_mutex_initialized)
    {
        return NGX_ERROR;
    }

    (void) pthread_mutex_lock(&ingest->events_mutex);

    if (ingest->events_tail == ingest->events_head) {
        (void) pthread_mutex_unlock(&ingest->events_mutex);
        return NGX_DECLINED;
    }

    slot = &ingest->events[ingest->events_tail
                           % ingest->events_capacity];
    *event = *slot;
    ngx_memzero(slot, sizeof(*slot));
    ingest->events_tail++;

    (void) pthread_mutex_unlock(&ingest->events_mutex);

    return NGX_OK;
}

static void
ngx_media_hls_ingest_event_clear(ngx_media_hls_ingest_source_t *ingest)
{
    ngx_media_hls_event_t  event;

    if (ingest == NULL) {
        return;
    }

    while (ngx_media_hls_ingest_event_pop(ingest, &event) == NGX_OK) {
        ngx_media_hls_ingest_event_release(&event);
    }
}

static void
ngx_media_hls_ingest_event_drain(ngx_media_hls_ingest_source_t *ingest)
{
    ngx_media_hls_event_t  event;
    ngx_media_trackset_t   tracks;
    ngx_media_source_t    *source;

    if (ingest == NULL) {
        return;
    }

    for ( ;; ) {
        if (ngx_media_hls_ingest_event_pop(ingest, &event) != NGX_OK) {
            return;
        }

        source = ingest->source;

        if (source != NULL && source->stream == ingest->stream) {
            switch (event.type) {

            case NGX_MEDIA_HLS_EVENT_TRACKS:
                tracks.tracks = event.data.trackset.tracks;
                tracks.count = event.data.trackset.count;
                tracks.capacity = event.data.trackset.count;
                (void) ngx_media_source_tracks_set(source, &tracks, NULL);
                (void) ngx_media_health_tracks(&source->health, 1);
                break;

            case NGX_MEDIA_HLS_EVENT_FRAME:
                ngx_media_health_media(&source->health,
                                       event.data.frame.dts,
                                       ngx_current_msec);
                ngx_media_runtime_iso_source(ingest->stream, source,
                                             &event.data.frame);
                (void) ngx_media_stream_publish(ingest->stream, source,
                                                &event.data.frame,
                                                ngx_current_msec);
                ingest->frames++;
                break;

            case NGX_MEDIA_HLS_EVENT_TRANSPORT:
                (void) ngx_media_health_transport(&source->health,
                                                  event.data.healthy,
                                                  ngx_current_msec);
                break;

            default:
                break;
            }
        }

        ngx_media_hls_ingest_event_release(&event);
    }
}

static void
ngx_media_hls_ingest_transport(ngx_media_hls_ingest_source_t *ingest,
    ngx_uint_t healthy)
{
    ngx_media_hls_event_t  event;

    if (ingest == NULL || ngx_atomic_fetch_add(&ingest->stopping, 0)) {
        return;
    }

    ngx_memzero(&event, sizeof(event));
    event.type = NGX_MEDIA_HLS_EVENT_TRANSPORT;
    event.data.healthy = healthy;
    (void) ngx_media_hls_ingest_event_push(ingest, &event);
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
    ngx_uint_t  slot;

    if (len >= NGX_MEDIA_HLS_INGEST_NAME_MAX) {
        return;
    }

    if (ingest->nseen < NGX_MEDIA_HLS_INGEST_SEEN) {
        slot = ingest->nseen++;
    } else {
        slot = ingest->seen_next;
        ingest->seen_next = (ingest->seen_next + 1) % NGX_MEDIA_HLS_INGEST_SEEN;
    }

    ngx_memcpy(ingest->seen[slot], name, len);
    ingest->seen[slot][len] = '\0';
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
 * joins the thread first - so the reader may read it as well as the worker.
 */
static ngx_uint_t
ngx_media_hls_ingest_source_removed(ngx_media_hls_ingest_source_t *ingest)
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

    segment = ingest->segment;

    if (segment == NULL) {
        (void) ngx_atomic_cmp_set(&ingest->exited, 0, 1);
        return NULL;
    }

    while (!ngx_atomic_fetch_add(&ingest->stopping, 0)) {

        if (ngx_media_hls_ingest_source_removed(ingest)) {
            /*
             * The source was removed through the control API, so there is
             * nothing left to read for.  Leave the loop: the tick reaps
             * exited readers, and the stream's memory cannot be released
             * until it has.
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

        /*
         * What an HLS input may carry is MPEG-TS with H.264 or H.265 video
         * and/or AAC audio.  The demux tracks exactly those stream types and
         * counts the ones it does not know, so a segment that carries only
         * others builds no tracks and publishes no frames: the source never
         * becomes healthy and nothing says why.  This is the why, once.
         */
        if (!ingest->input_reported && !ingest->demux.tracks_ready
            && ingest->demux.stats.unsupported_streams > 0)
        {
            ingest->input_reported = 1;

            ngx_log_error(NGX_LOG_WARN, ingest->log, 0,
                          "media: hls ingest source %V carries no stream type "
                          "this build can carry (MPEG-TS with H.264, H.265 or "
                          "AAC); it will not become healthy",
                          ingest->source != NULL ? &ingest->source->id
                                                 : &ingest->directory);
        }

        ingest->segments++;
        ngx_media_hls_ingest_transport(ingest, 1);
    }

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
    ingest->log = log;

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
        ngx_media_stream_source_remove(stream, ingest->source);
        ingest->source = NULL;
        return NULL;
    }
    ingest->segment = ngx_alloc(NGX_MEDIA_HLS_INGEST_SEGMENT_MAX, log);

    if (ingest->segment == NULL) {
        ngx_media_ts_demux_destroy(&ingest->demux);
        ngx_media_stream_source_remove(stream, ingest->source);
        ingest->source = NULL;
        return NULL;
    }

    ingest->events_capacity = NGX_MEDIA_HLS_EVENT_CAPACITY;
    ingest->events = ngx_alloc(ingest->events_capacity
                               * sizeof(ngx_media_hls_event_t), log);

    if (ingest->events == NULL) {
        ngx_free(ingest->segment);
        ingest->segment = NULL;
        ngx_media_ts_demux_destroy(&ingest->demux);
        ngx_media_stream_source_remove(stream, ingest->source);
        ingest->source = NULL;
        return NULL;
    }

    ngx_memzero(ingest->events,
                ingest->events_capacity * sizeof(ngx_media_hls_event_t));

    if (pthread_mutex_init(&ingest->events_mutex, NULL) != 0) {
        ngx_free(ingest->events);
        ingest->events = NULL;
        ngx_free(ingest->segment);
        ingest->segment = NULL;
        ngx_media_ts_demux_destroy(&ingest->demux);
        ngx_media_stream_source_remove(stream, ingest->source);
        ingest->source = NULL;
        return NULL;
    }

    ingest->events_mutex_initialized = 1;

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
        ngx_media_hls_ingest_close(ingest);
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

    (void) ngx_atomic_fetch_add(&ingest->stopping, 1);

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
    ngx_media_hls_ingest_event_clear(ingest);

    if (ingest->events_mutex_initialized) {
        (void) pthread_mutex_destroy(&ingest->events_mutex);
        ingest->events_mutex_initialized = 0;
    }

    ngx_free(ingest->events);
    ingest->events = NULL;
    ngx_free(ingest->segment);
    ingest->segment = NULL;


    ngx_media_ts_demux_destroy(&ingest->demux);

    if (ingest->stream != NULL && ingest->source != NULL) {
        ngx_media_stream_source_remove(ingest->stream, ingest->source);
        ingest->source = NULL;
    }
}

ngx_uint_t
ngx_media_hls_ingest_stream_readers(const ngx_media_stream_t *stream)
{
    ngx_media_hls_ingest_source_t  *ingest;
    ngx_uint_t                      count = 0;

    if (stream == NULL) {
        return 0;
    }

    (void) pthread_mutex_lock(&ngx_media_hls_ingest_mutex);

    for (ingest = ngx_media_hls_ingest_all; ingest != NULL;
         ingest = ingest->next)
    {
        if (ingest->stream == stream) {
            count++;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_ingest_mutex);

    return count;
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
            if (ngx_atomic_fetch_add(&ingest->exited, 0)
                && ngx_media_hls_ingest_source_removed(ingest))
            {
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
ngx_media_hls_ingest_drain_all(void)
{
    ngx_media_hls_ingest_source_t  *ingest;

    (void) pthread_mutex_lock(&ngx_media_hls_ingest_mutex);

    for (ingest = ngx_media_hls_ingest_all; ingest != NULL;
         ingest = ingest->next)
    {
        ngx_media_hls_ingest_event_drain(ingest);
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_ingest_mutex);
}

void
ngx_media_hls_ingest_stop_all(void)
{
    ngx_media_hls_ingest_source_t  *ingest;

    (void) pthread_mutex_lock(&ngx_media_hls_ingest_mutex);

    for (ingest = ngx_media_hls_ingest_all; ingest != NULL;
         ingest = ingest->next)
    {
        (void) ngx_atomic_fetch_add(&ingest->stopping, 1);
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_ingest_mutex);

    for (ingest = ngx_media_hls_ingest_all; ingest != NULL;
         ingest = ingest->next)
    {
        if (ingest->thread_started) {
            (void) pthread_join(ingest->thread, NULL);
            ingest->thread_started = 0;
        }

        ngx_media_hls_ingest_event_clear(ingest);

        if (ingest->events_mutex_initialized) {
            (void) pthread_mutex_destroy(&ingest->events_mutex);
            ingest->events_mutex_initialized = 0;
        }

        ngx_free(ingest->events);
        ingest->events = NULL;
        ngx_free(ingest->segment);
        ingest->segment = NULL;
        ngx_media_ts_demux_destroy(&ingest->demux);
    }
}

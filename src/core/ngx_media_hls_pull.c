#include "ngx_media_hls_pull.h"
#include "ngx_media_health.h"
#include "ngx_media_http.h"
#include "ngx_media_runtime.h"
#include "ngx_media_stream.h"

#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define NGX_MEDIA_HLS_PULL_PLAYLIST_MAX  (64 * 1024)
#define NGX_MEDIA_HLS_PULL_SEGMENT_MAX   (8 * 1024 * 1024)
#define NGX_MEDIA_HLS_PULL_NAMES         64
#define NGX_MEDIA_HLS_PULL_NAME_MAX      256

/*
 * One segment name already fetched.  The name is stored as it came from the
 * playlist; one longer than the slot is keyed on its first NAME_MAX bytes and
 * its full length, so it is still remembered instead of being re-fetched on
 * every poll.  Only names that share that prefix *and* their length can be
 * confused, which a segmenter does not produce.
 */
typedef struct {
    u_char      name[NGX_MEDIA_HLS_PULL_NAME_MAX];
    size_t      len;      /* the full length, not the stored prefix */
} ngx_media_hls_pull_seen_t;

typedef struct ngx_media_hls_pull_s {
    ngx_media_stream_t      *stream;
    ngx_media_source_t      *source;
    ngx_media_ts_demux_t     demux;

    ngx_str_t                url;         /* the playlist */
    ngx_str_t                base;        /* everything up to the last / */
    ngx_str_t                ca_file;     /* TLS trust anchor, empty for the
                                           * system store */
    ngx_log_t               *log;

    pthread_t                thread;
    ngx_uint_t               thread_started;
    ngx_uint_t               stopping;

    /*
     * Set by the reader thread as its last act, after the loop and before it
     * returns, so the reaper knows the thread has stopped touching this
     * reader and joining it cannot block.  Without it a reap could park on a
     * thread that is still asleep between refreshes.  An atomic flag read by
     * the reaper is the same idiom the SRT session uses for close_requested:
     * the join that follows is what actually publishes the reader's writes.
     */
    ngx_atomic_t             exited;

    /*
     * Segment names already fetched, so a refresh does not repeat them.  A
     * ring, not a list: when it fills, the oldest entry gives way, because a
     * name the table refuses to store is fetched and demuxed again on every
     * poll, which publishes the same media twice with timestamps that no
     * longer move forward.
     */
    ngx_media_hls_pull_seen_t  seen[NGX_MEDIA_HLS_PULL_NAMES];
    ngx_uint_t                 nseen;      /* slots in use, up to NAMES */
    ngx_uint_t                 seen_next;  /* the ring: oldest slot first */

    uint64_t                 segments;
    uint64_t                 frames;
    uint64_t                 failures;

    struct ngx_media_hls_pull_s  *next;
} ngx_media_hls_pull_t;

static ngx_media_hls_pull_t  *ngx_media_hls_pull_all;
static pthread_mutex_t        ngx_media_hls_pull_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- helpers ------------------------------------------------------------- */

static void
ngx_media_hls_pull_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_media_hls_pull_t  *pull = ctx;

    if (pull == NULL || pull->source == NULL || tracks == NULL) {
        return;
    }

    (void) ngx_media_source_tracks_set(pull->source, tracks, NULL);
    (void) ngx_media_health_tracks(&pull->source->health, 1);
}

static void
ngx_media_hls_pull_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_hls_pull_t  *pull = ctx;

    if (pull == NULL || pull->stream == NULL || pull->source == NULL) {
        return;
    }

    /*
     * A pulled HLS stream is a source like any other: the same health model
     * and the same gate, so the selector and compatibility need to know
     * nothing about where the bytes came from.  This is what lets a pull
     * source be promoted like a publisher.
     */
    ngx_media_health_media(&pull->source->health, frame->dts,
                           ngx_current_msec);

    ngx_media_runtime_iso_source(pull->stream, pull->source, frame);

    (void) ngx_media_stream_publish(pull->stream, pull->source, frame,
                                    ngx_current_msec);

    pull->frames++;
}

static ngx_uint_t
ngx_media_hls_pull_seen(ngx_media_hls_pull_t *pull, const u_char *name,
    size_t len)
{
    ngx_media_hls_pull_seen_t  *entry;
    size_t                      key;
    ngx_uint_t                  i;

    /* what a slot can hold; the length below separates the rest */
    key = (len < NGX_MEDIA_HLS_PULL_NAME_MAX) ? len
                                              : NGX_MEDIA_HLS_PULL_NAME_MAX;

    for (i = 0; i < pull->nseen; i++) {

        entry = &pull->seen[i];

        if (entry->len == len && ngx_memcmp(entry->name, name, key) == 0) {
            return 1;
        }
    }

    return 0;
}

/* records a segment as fetched; called only after the fetch succeeds */
static void
ngx_media_hls_pull_record(ngx_media_hls_pull_t *pull, const u_char *name,
    size_t len)
{
    ngx_media_hls_pull_seen_t  *entry;
    size_t                      key;

    key = (len < NGX_MEDIA_HLS_PULL_NAME_MAX) ? len
                                              : NGX_MEDIA_HLS_PULL_NAME_MAX;

    if (pull->nseen < NGX_MEDIA_HLS_PULL_NAMES) {
        entry = &pull->seen[pull->nseen++];

    } else {
        entry = &pull->seen[pull->seen_next];
        pull->seen_next = (pull->seen_next + 1) % NGX_MEDIA_HLS_PULL_NAMES;
    }

    ngx_memcpy(entry->name, name, key);
    entry->len = len;
}

/* absolute URL for a playlist entry, which may be relative or absolute */
static ngx_int_t
ngx_media_hls_pull_segment_url(ngx_media_hls_pull_t *pull, const u_char *name,
    size_t len, ngx_str_t *out)
{
    if (len > 7 && ngx_memcmp(name, (u_char *) "http://", 7) == 0) {
        out->data = ngx_pnalloc(pull->stream->pool, len + 1);

        if (out->data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(out->data, name, len);
        out->data[len] = '\0';
        out->len = len;

        return NGX_OK;
    }

    if (pull->base.len + len + 1 > 4096) {
        return NGX_ERROR;
    }

    out->data = ngx_pnalloc(pull->stream->pool, pull->base.len + len + 1);

    if (out->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(out->data, pull->base.data, pull->base.len);
    ngx_memcpy(out->data + pull->base.len, name, len);
    out->len = pull->base.len + len;
    out->data[out->len] = '\0';

    return NGX_OK;
}

/* --- the pull loop ------------------------------------------------------- */

/*
 * Whether the source behind this reader was removed through the control API.
 * source_remove() detaches it, or only flags it when a writer is in flight,
 * so both are checked.  This is the reader's own pointer, which only close()
 * clears, and close() cannot run while this thread is using the reader - it
 * joins the thread first.
 */
static ngx_uint_t
ngx_media_hls_pull_removed(ngx_media_hls_pull_t *pull)
{
    return (pull->source == NULL
            || pull->source->stream == NULL
            || pull->source->pending_remove);
}

/*
 * Waits out one refresh interval in slices.  The cadence is the same - the
 * slices add up to the interval the pull used to sleep for - but a source
 * removed through the control API is noticed within a slice instead of up to
 * the whole interval later, and the reader still stops without the tick
 * having to wake or join it.
 */
#define NGX_MEDIA_HLS_PULL_IDLE_SLICE_US  100000
#define NGX_MEDIA_HLS_PULL_IDLE_SLICES    20

static void
ngx_media_hls_pull_idle(ngx_media_hls_pull_t *pull)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_HLS_PULL_IDLE_SLICES; i++) {

        if (pull->stopping || ngx_media_hls_pull_removed(pull)) {
            return;
        }

        usleep(NGX_MEDIA_HLS_PULL_IDLE_SLICE_US);
    }
}

static void *
ngx_media_hls_pull_thread(void *data)
{
    ngx_media_hls_pull_t  *pull = data;
    u_char                *playlist;
    u_char                *segment;
    ngx_str_t              segment_url;
    size_t                 playlist_len, segment_len;
    u_char                *p, *line_end;
    ngx_uint_t             fetched;

    playlist = ngx_pnalloc(pull->stream->pool, NGX_MEDIA_HLS_PULL_PLAYLIST_MAX);
    segment = ngx_pnalloc(pull->stream->pool, NGX_MEDIA_HLS_PULL_SEGMENT_MAX);

    if (playlist == NULL || segment == NULL) {
        /* nothing is left running to touch the reader; the reaper may join */
        (void) ngx_atomic_cmp_set(&pull->exited, 0, 1);
        return NULL;
    }

    while (!pull->stopping) {

        if (ngx_media_hls_pull_removed(pull)) {
            /*
             * The source was removed through the control API, so there is
             * nothing left to publish into.  Leave the loop: the tick reaps
             * exited readers, and it cannot destroy this one while it is
             * still using it.
             */
            break;
        }

        if (ngx_media_http_get(&pull->url, &pull->ca_file, playlist,
                               NGX_MEDIA_HLS_PULL_PLAYLIST_MAX,
                               &playlist_len, pull->log) != NGX_OK)
        {
            pull->failures++;
            (void) ngx_media_health_transport(&pull->source->health, 0,
                                              ngx_current_msec);
            ngx_media_hls_pull_idle(pull);
            continue;
        }

        (void) ngx_media_health_transport(&pull->source->health, 1,
                                          ngx_current_msec);

        fetched = 0;

        /*
         * Walk the playlist a line at a time.  A media playlist is a list of
         * URIs with #EXT* tags between them; anything not starting with '#'
         * and not empty is a segment.
         */
        p = playlist;

        while (p < playlist + playlist_len && !pull->stopping
               && !ngx_media_hls_pull_removed(pull))
        {

            line_end = (u_char *) strchr((char *) p, '\n');

            if (line_end == NULL) {
                line_end = playlist + playlist_len;
            }

            {
                u_char  *line = p;
                size_t   len = line_end - p;

                p = line_end + 1;

                while (len > 0 && (line[len - 1] == '\r'
                                   || line[len - 1] == ' '))
                {
                    len--;
                }

                if (len == 0 || line[0] == '#') {
                    continue;
                }

                /*
                 * A read-only check.  Recording here would mark every name in
                 * the playlist as fetched the first time the playlist is
                 * walked, so only the lowest one would ever be read and the
                 * rest would be skipped for good - the same mistake the
                 * ingest reader made.  Recording happens after the fetch.
                 */
                if (ngx_media_hls_pull_seen(pull, line, len)) {
                    continue;
                }

                if (ngx_media_hls_pull_segment_url(pull, line, len,
                                                   &segment_url) != NGX_OK)
                {
                    continue;
                }

                if (ngx_media_http_get(&segment_url, &pull->ca_file, segment,
                                       NGX_MEDIA_HLS_PULL_SEGMENT_MAX,
                                       &segment_len, pull->log) != NGX_OK)
                {
                    pull->failures++;
                    continue;
                }

                if (ngx_media_ts_demux_feed(&pull->demux, segment,
                                            segment_len) != NGX_OK)
                {
                    pull->failures++;
                    continue;
                }

                ngx_media_hls_pull_record(pull, line, len);

                pull->segments++;
                fetched++;
            }
        }

        if (fetched == 0) {
            /* nothing new: wait for the origin to publish more */
            ngx_media_hls_pull_idle(pull);
        }
    }

    /*
     * The loop has left the reader alone: nothing below touches the demux,
     * the source or the buffers.  Marking that here is what lets the reaper
     * join this thread from the tick without waiting on the origin - the
     * source was removed through the control API, so its reader has to go
     * with it instead of holding a thread and its buffers for the life of
     * the worker.
     */
    (void) ngx_atomic_cmp_set(&pull->exited, 0, 1);

    return NULL;
}

/* --- lifecycle ----------------------------------------------------------- */

ngx_media_hls_pull_t *
ngx_media_hls_pull_open(ngx_media_stream_t *stream, const ngx_str_t *id,
    const ngx_str_t *url, const ngx_str_t *ca_file, ngx_log_t *log)
{
    ngx_media_hls_pull_t  *pull;
    ngx_media_ts_sink_t    sink;
    ngx_str_t             *copy;
    u_char                *slash;

    if (stream == NULL || id == NULL || id->len == 0 || url == NULL
        || url->len == 0)
    {
        return NULL;
    }

    pull = ngx_pcalloc(stream->pool, sizeof(ngx_media_hls_pull_t));

    if (pull == NULL) {
        return NULL;
    }

    copy = ngx_pcalloc(stream->pool, sizeof(ngx_str_t));

    if (copy == NULL) {
        return NULL;
    }

    copy->data = ngx_pnalloc(stream->pool, url->len + 1);

    if (copy->data == NULL) {
        return NULL;
    }

    ngx_memcpy(copy->data, url->data, url->len);
    copy->data[url->len] = '\0';
    copy->len = url->len;

    pull->url = *copy;
    pull->stream = stream;
    pull->log = log;

    /*
     * The caller's strings are slices of something it owns - a request body,
     * usually - and a reader outlives the request that created it.  Both have
     * to be copied into the stream's pool.  The URL was; the CA file was not,
     * so the pull worked for exactly as long as the creating request was
     * alive and then read whatever the body's memory had become.
     */
    if (ca_file != NULL && ca_file->len > 0) {
        copy = ngx_pcalloc(stream->pool, sizeof(ngx_str_t));

        if (copy == NULL) {
            return NULL;
        }

        copy->data = ngx_pnalloc(stream->pool, ca_file->len + 1);

        if (copy->data == NULL) {
            return NULL;
        }

        ngx_memcpy(copy->data, ca_file->data, ca_file->len);
        copy->data[ca_file->len] = '\0';
        copy->len = ca_file->len;

        pull->ca_file = *copy;
    }

    /* relative playlist entries resolve against the playlist's directory */
    slash = (u_char *) strrchr((char *) copy->data, '/');

    if (slash != NULL && slash > copy->data + 7) {
        pull->base.data = copy->data;
        pull->base.len = (size_t) (slash - copy->data) + 1;
    }

    pull->source = ngx_media_stream_source_add(stream, id,
                                               NGX_MEDIA_SOURCE_HLS_PULL, 0,
                                               log);

    if (pull->source == NULL) {
        return NULL;
    }

    sink.tracks = ngx_media_hls_pull_tracks;
    sink.frame = ngx_media_hls_pull_frame;

    if (ngx_media_ts_demux_init(&pull->demux, NULL, &sink, pull, log)
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
    ngx_media_health_init(&pull->source->health, &stream->selector, ngx_current_msec);
    pull->source->health.failure_timeout = NGX_MEDIA_BURSTY_FAILURE_TIMEOUT;
    ngx_media_health_transport(&pull->source->health, 1, ngx_current_msec);
    ngx_media_source_touch(pull->source);

    (void) pthread_mutex_lock(&ngx_media_hls_pull_mutex);
    pull->next = ngx_media_hls_pull_all;
    ngx_media_hls_pull_all = pull;
    (void) pthread_mutex_unlock(&ngx_media_hls_pull_mutex);

    if (pthread_create(&pull->thread, NULL, ngx_media_hls_pull_thread, pull)
        != 0)
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: hls pull %V could not start its reader", id);
        return NULL;
    }

    pull->thread_started = 1;

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: hls pull source %V opened for %V/%V from %V", id,
                  &stream->application, &stream->name, &pull->url);

    return pull;
}

/*
 * Ordered teardown: stop the reader, join it, unlink it, release it.  Safe to
 * call more than once and safe on a reader that has already finished - the
 * join is guarded by thread_started, the unlink finds nothing the second
 * time, the demux is zeroed by destroy() and the source pointer is NULLed
 * here, so a repeated close is a no-op rather than a double free.
 */
void
ngx_media_hls_pull_close(ngx_media_hls_pull_t *pull)
{
    ngx_media_hls_pull_t **link;

    if (pull == NULL) {
        return;
    }

    /*
     * Stop first and join, so no callback can be in flight when the source
     * is torn down: the reader publishes into the stream, and the stream is
     * about to lose this source.
     */
    pull->stopping = 1;

    if (pull->thread_started) {
        (void) pthread_join(pull->thread, NULL);
        pull->thread_started = 0;
    }

    (void) pthread_mutex_lock(&ngx_media_hls_pull_mutex);

    for (link = &ngx_media_hls_pull_all; *link != NULL; link = &(*link)->next) {

        if (*link == pull) {
            *link = pull->next;
            break;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_pull_mutex);

    ngx_media_ts_demux_destroy(&pull->demux);

    if (pull->stream != NULL && pull->source != NULL) {
        ngx_media_stream_source_remove(pull->stream, pull->source);
        pull->source = NULL;
    }
}

/*
 * Closes every pull reader whose source was removed through the control API
 * and whose thread has already left its loop.  Called from the runtime tick.
 *
 * A removed source has to take its reader with it: the reader holds a thread,
 * a demuxer and an 8 MiB segment buffer, and frames it publishes are dropped
 * once the source is detached, so leaving it running leaks all of that once
 * per create/delete cycle.  The reader notices the removal in its own loop and
 * exits; this collects it.  Only exited readers are closed, so the join in
 * close() cannot block the tick on an origin that is slow to answer.
 */
void
ngx_media_hls_pull_reap(ngx_log_t *log)
{
    ngx_media_hls_pull_t  *pull;

    for ( ;; ) {
        pull = NULL;

        (void) pthread_mutex_lock(&ngx_media_hls_pull_mutex);

        for (pull = ngx_media_hls_pull_all; pull != NULL; pull = pull->next) {

            if (pull->exited && ngx_media_hls_pull_removed(pull)) {
                break;
            }
        }

        (void) pthread_mutex_unlock(&ngx_media_hls_pull_mutex);

        if (pull == NULL) {
            return;
        }

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: hls pull source %V removed, closing its reader",
                      pull->source != NULL ? &pull->source->id : &pull->url);

        /* unlinks the reader, so the next pass cannot see it again */
        ngx_media_hls_pull_close(pull);
    }
}

void
ngx_media_hls_pull_stop_all(void)
{
    ngx_media_hls_pull_t  *pull;

    (void) pthread_mutex_lock(&ngx_media_hls_pull_mutex);

    for (pull = ngx_media_hls_pull_all; pull != NULL; pull = pull->next) {
        pull->stopping = 1;
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_pull_mutex);

    (void) pthread_mutex_lock(&ngx_media_hls_pull_mutex);

    for (pull = ngx_media_hls_pull_all; pull != NULL; pull = pull->next) {

        if (pull->thread_started) {
            (void) pthread_join(pull->thread, NULL);
            pull->thread_started = 0;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_pull_mutex);
}

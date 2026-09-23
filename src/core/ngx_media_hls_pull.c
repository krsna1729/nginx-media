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
 * The longest URL this reader builds or asks for.  A segment URL is the
 * playlist's directory plus a playlist entry (or the entry alone when it is
 * absolute), built into one fixed scratch buffer in the reader, and it bounds
 * the request target the HTTP client is handed.  One named bound covers the
 * scratch, the build and the request, so they cannot drift apart: the scratch
 * used to be sized from the playlist bound - sixteen times what it could ever
 * hold - while the check that mattered was a bare 4096 in one of the two
 * branches, so an absolute entry far longer than a relative one was accepted.
 */
#define NGX_MEDIA_HLS_PULL_URL_MAX       4096
#define NGX_MEDIA_HLS_EVENT_CAPACITY     128
#define NGX_MEDIA_HLS_EVENT_MAX_TRACKS   NGX_MEDIA_TS_DEFAULT_TRACKS

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

    /* set once, by the reader, when a segment carried nothing carryable */
    ngx_uint_t               input_reported;

    ngx_str_t                id;
    ngx_str_t                url;         /* the playlist */
    ngx_str_t                base;        /* everything up to the last / */
    ngx_str_t                ca_file;     /* TLS trust anchor, empty for the
                                           * system store */
    ngx_log_t               *log;

    /*
     * Reader-thread scratch: the playlist and segment buffers, and the one
     * buffer a segment URL is built in.  Allocated when the reader is opened
     * and freed by close() only after the thread has been joined, so the
     * scratch never outlives the thread and no request that created the
     * reader can free it under it.
     */
    u_char                  *playlist;
    u_char                  *segment;
    u_char                  *segment_url;
    size_t                   segment_url_capacity;

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
     * thread that is still asleep between refreshes.  An atomic flag read by
     * the reaper is the same idiom the SRT session uses for close_requested:
     * the join that follows is what actually publishes the reader's writes.
     */
    ngx_atomic_t             exited;

    /*
     * Segment names already fetched, so a refresh does not repeat them.  The
     * bounded table covers the recent window; the lexical high-water mark
     * keeps immutable names older than it out even after that table wraps.
     * This relies on the segmenter naming its immutable files monotonically.
     */
    ngx_media_hls_pull_seen_t  seen[NGX_MEDIA_HLS_PULL_NAMES];
    ngx_uint_t                 nseen;      /* slots in use, up to NAMES */
    ngx_uint_t                 seen_next;  /* the ring: oldest slot first */
    u_char                     last_name[NGX_MEDIA_HLS_PULL_URL_MAX];
    size_t                     last_name_len;
    ngx_uint_t                 last_name_set;

    uint64_t                 segments;
    uint64_t                 frames;
    uint64_t                 failures;

    struct ngx_media_hls_pull_s  *next;
} ngx_media_hls_pull_t;

static ngx_media_hls_pull_t  *ngx_media_hls_pull_all;
static pthread_mutex_t        ngx_media_hls_pull_mutex = PTHREAD_MUTEX_INITIALIZER;
static void ngx_media_hls_pull_event_release(
    ngx_media_hls_event_t *event);
static ngx_int_t ngx_media_hls_pull_event_push(
    ngx_media_hls_pull_t *pull, ngx_media_hls_event_t *event);
static ngx_int_t ngx_media_hls_pull_event_pop(
    ngx_media_hls_pull_t *pull, ngx_media_hls_event_t *event);
static void ngx_media_hls_pull_event_clear(
    ngx_media_hls_pull_t *pull);
static void ngx_media_hls_pull_event_drain(
    ngx_media_hls_pull_t *pull);
static void ngx_media_hls_pull_transport(
    ngx_media_hls_pull_t *pull, ngx_uint_t healthy);
static ngx_uint_t ngx_media_hls_pull_source_removed(
    ngx_media_hls_pull_t *pull);
static ngx_int_t ngx_media_hls_pull_name_cmp(const u_char *a, size_t alen,
    const u_char *b, size_t blen);

/* --- helpers ------------------------------------------------------------- */

static void
ngx_media_hls_pull_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_media_hls_pull_t  *pull = ctx;
    ngx_media_hls_event_t  event;
    ngx_uint_t             i;

    if (pull == NULL || tracks == NULL
        || ngx_atomic_fetch_add(&pull->stopping, 0)
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

    (void) ngx_media_hls_pull_event_push(pull, &event);
}

static void
ngx_media_hls_pull_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_hls_pull_t  *pull = ctx;
    ngx_media_hls_event_t  event;

    if (pull == NULL || frame == NULL
        || ngx_atomic_fetch_add(&pull->stopping, 0))
    {
        return;
    }

    ngx_memzero(&event, sizeof(event));
    event.type = NGX_MEDIA_HLS_EVENT_FRAME;

    if (ngx_media_frame_copy(&event.data.frame, frame) != NGX_OK) {
        return;
    }

    (void) ngx_media_hls_pull_event_push(pull, &event);
}
static void
ngx_media_hls_pull_event_release(ngx_media_hls_event_t *event)
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
ngx_media_hls_pull_event_push(ngx_media_hls_pull_t *pull,
    ngx_media_hls_event_t *event)
{
    ngx_media_hls_event_t  *slot;

    if (pull == NULL || event == NULL || pull->events == NULL
        || !pull->events_mutex_initialized)
    {
        ngx_media_hls_pull_event_release(event);
        return NGX_ERROR;
    }

    (void) pthread_mutex_lock(&pull->events_mutex);

    if (pull->events_head - pull->events_tail >= pull->events_capacity) {
        (void) pthread_mutex_unlock(&pull->events_mutex);
        (void) ngx_atomic_fetch_add(&pull->events_dropped, 1);
        ngx_media_hls_pull_event_release(event);
        return NGX_AGAIN;
    }

    slot = &pull->events[pull->events_head % pull->events_capacity];
    *slot = *event;
    pull->events_head++;

    (void) pthread_mutex_unlock(&pull->events_mutex);

    return NGX_OK;
}

static ngx_int_t
ngx_media_hls_pull_event_pop(ngx_media_hls_pull_t *pull,
    ngx_media_hls_event_t *event)
{
    ngx_media_hls_event_t  *slot;

    if (pull == NULL || event == NULL || pull->events == NULL
        || !pull->events_mutex_initialized)
    {
        return NGX_ERROR;
    }

    (void) pthread_mutex_lock(&pull->events_mutex);

    if (pull->events_tail == pull->events_head) {
        (void) pthread_mutex_unlock(&pull->events_mutex);
        return NGX_DECLINED;
    }

    slot = &pull->events[pull->events_tail % pull->events_capacity];
    *event = *slot;
    ngx_memzero(slot, sizeof(*slot));
    pull->events_tail++;

    (void) pthread_mutex_unlock(&pull->events_mutex);

    return NGX_OK;
}

static void
ngx_media_hls_pull_event_clear(ngx_media_hls_pull_t *pull)
{
    ngx_media_hls_event_t  event;

    if (pull == NULL) {
        return;
    }

    while (ngx_media_hls_pull_event_pop(pull, &event) == NGX_OK) {
        ngx_media_hls_pull_event_release(&event);
    }
}

static void
ngx_media_hls_pull_event_drain(ngx_media_hls_pull_t *pull)
{
    ngx_media_hls_event_t  event;
    ngx_media_trackset_t   tracks;
    ngx_media_source_t    *source;

    if (pull == NULL) {
        return;
    }

    for ( ;; ) {
        if (ngx_media_hls_pull_event_pop(pull, &event) != NGX_OK) {
            return;
        }

        source = pull->source;

        if (source != NULL && source->stream == pull->stream) {
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
                ngx_media_runtime_iso_source(pull->stream, source,
                                             &event.data.frame);
                (void) ngx_media_stream_publish(pull->stream, source,
                                                &event.data.frame,
                                                ngx_current_msec);
                pull->frames++;
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

        ngx_media_hls_pull_event_release(&event);
    }
}

static void
ngx_media_hls_pull_transport(ngx_media_hls_pull_t *pull,
    ngx_uint_t healthy)
{
    ngx_media_hls_event_t  event;

    if (pull == NULL || ngx_atomic_fetch_add(&pull->stopping, 0)) {
        return;
    }

    ngx_memzero(&event, sizeof(event));
    event.type = NGX_MEDIA_HLS_EVENT_TRANSPORT;
    event.data.healthy = healthy;
    (void) ngx_media_hls_pull_event_push(pull, &event);
}

static ngx_int_t
ngx_media_hls_pull_name_cmp(const u_char *a, size_t alen,
    const u_char *b, size_t blen)
{
    size_t    len;
    ngx_int_t rc;

    len = (alen < blen) ? alen : blen;
    rc = (len == 0) ? 0 : ngx_memcmp(a, b, len);

    if (rc != 0) {
        return rc;
    }

    return (alen > blen) - (alen < blen);
}

static ngx_uint_t
ngx_media_hls_pull_seen(ngx_media_hls_pull_t *pull, const u_char *name,
    size_t len)
{
    ngx_media_hls_pull_seen_t  *entry;
    size_t                      key;
    ngx_uint_t                  i;

    if (pull->last_name_set
        && ngx_media_hls_pull_name_cmp(name, len, pull->last_name,
                                       pull->last_name_len) <= 0)
    {
        return 1;
    }

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

    if (len < NGX_MEDIA_HLS_PULL_URL_MAX
        && (!pull->last_name_set
            || ngx_media_hls_pull_name_cmp(name, len, pull->last_name,
                                           pull->last_name_len) > 0))
    {
        ngx_memcpy(pull->last_name, name, len);
        pull->last_name_len = len;
        pull->last_name_set = 1;
    }
}

/* absolute URL for a playlist entry, which may be relative or absolute */
static ngx_int_t
ngx_media_hls_pull_segment_url(ngx_media_hls_pull_t *pull, const u_char *name,
    size_t len, ngx_str_t *out)
{
    size_t  total;

    if (pull == NULL || name == NULL || out == NULL
        || pull->segment_url == NULL
        || pull->segment_url_capacity == 0)
    {
        return NGX_ERROR;
    }

    /*
     * One bound for both branches, and it is the one the scratch was sized
     * from: a longer entry is refused here rather than truncated into the
     * buffer, and the room the terminator needs is part of the check.
     */
    if (len >= NGX_MEDIA_HLS_PULL_URL_MAX) {
        return NGX_ERROR;
    }

    if (len > 7 && ngx_memcmp(name, (u_char *) "http://", 7) == 0) {
        ngx_memcpy(pull->segment_url, name, len);
        pull->segment_url[len] = '\0';
        out->data = pull->segment_url;
        out->len = len;

        return NGX_OK;
    }

    if (pull->base.len > NGX_MEDIA_HLS_PULL_URL_MAX - len - 1) {
        return NGX_ERROR;
    }

    total = pull->base.len + len;

    ngx_memcpy(pull->segment_url, pull->base.data, pull->base.len);
    ngx_memcpy(pull->segment_url + pull->base.len, name, len);
    pull->segment_url[total] = '\0';
    out->data = pull->segment_url;
    out->len = total;

    return NGX_OK;
}

/* --- the pull loop ------------------------------------------------------- */

/*
 * Source removal publishes pending_remove before detaching the source.  The
 * reader owns its source pointer until close joins it, so the atomic flag is
 * the only cross-thread lifetime check needed here.
 */
static ngx_uint_t
ngx_media_hls_pull_source_removed(ngx_media_hls_pull_t *pull)
{
    return (pull->source == NULL
            || ngx_atomic_fetch_add(&pull->source->pending_remove, 0));
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

        if (ngx_atomic_fetch_add(&pull->stopping, 0)
            || ngx_media_hls_pull_source_removed(pull))
        {
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

    playlist = pull->playlist;
    segment = pull->segment;

    if (playlist == NULL || segment == NULL || pull->segment_url == NULL) {
        (void) ngx_atomic_cmp_set(&pull->exited, 0, 1);
        return NULL;
    }

    while (!ngx_atomic_fetch_add(&pull->stopping, 0)) {

        if (ngx_media_hls_pull_source_removed(pull)) {
            /*
             * The source was removed through the control API, so there is
             * nothing left to fetch for.  Leave the loop: the tick reaps
             * exited readers, and the stream's memory cannot be released
             * until it has.
             */
            break;
        }

        if (ngx_media_http_get(&pull->url, &pull->ca_file, playlist,
                               NGX_MEDIA_HLS_PULL_PLAYLIST_MAX,
                               &playlist_len, pull->log) != NGX_OK)
        {
            pull->failures++;

            ngx_log_error(NGX_LOG_INFO, pull->log, 0,
                          "media: hls pull %V could not fetch the playlist "
                          "(failures=%ui)", &pull->id, pull->failures);
            ngx_media_hls_pull_transport(pull, 0);
            ngx_media_hls_pull_idle(pull);
            continue;
        }

        ngx_media_hls_pull_transport(pull, 1);
        fetched = 0;

        /*
         * Walk the playlist a line at a time.  A media playlist is a list of
         * URIs with #EXT* tags between them; anything not starting with '#'
         * and not empty is a segment.
         */
        p = playlist;

        while (p < playlist + playlist_len
               && !ngx_atomic_fetch_add(&pull->stopping, 0))
        {
            line_end = (u_char *) strchr((char *) p, '\n');

            if (line_end == NULL) {
                line_end = playlist + playlist_len;
            }

            {
                u_char  *line = p;
                size_t   len = line_end - p;

                p = (line_end < playlist + playlist_len)
                    ? line_end + 1 : line_end;

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
                 * the playlist as fetched the first time the playlist was
                 * walked, so only the lowest one would ever be read and the
                 * rest would be skipped for good.  Recording happens after
                 * the fetch.
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

                    ngx_log_error(NGX_LOG_INFO, pull->log,
                                  0, "media: hls pull %V could not fetch %V "
                                  "(failures=%ui)", &pull->id, &segment_url,
                                  pull->failures);
                    continue;
                }

                if (ngx_media_ts_demux_feed(&pull->demux, segment,
                                            segment_len) != NGX_OK)
                {
                    pull->failures++;
                    continue;
                }

                /*
                 * What an HLS input may carry is MPEG-TS with H.264 or H.265
                 * video and/or AAC audio.  A segment that carries only stream
                 * types the demux does not know builds no tracks and publishes
                 * no frames, so the source never becomes healthy: this is the
                 * reason, said once.
                 */
                if (!pull->input_reported && !pull->demux.tracks_ready
                    && pull->demux.stats.unsupported_streams > 0)
                {
                    pull->input_reported = 1;

                    ngx_log_error(NGX_LOG_WARN, pull->log, 0,
                                  "media: hls pull source %V carries no stream "
                                  "type this build can carry (MPEG-TS with "
                                  "H.264, H.265 or AAC); it will not become "
                                  "healthy",
                                  pull->source != NULL ? &pull->source->id
                                                       : &pull->url);
                }

                ngx_media_hls_pull_record(pull, line, len);
                pull->segments++;
                fetched++;

                ngx_log_error(NGX_LOG_INFO, pull->log,
                              0, "media: hls pull %V fetched segment %ui "
                              "(%uz bytes)", &pull->id, pull->segments,
                              segment_len);
            }
        }

        if (fetched == 0) {
            /* nothing new: wait for the origin to publish more */
            ngx_media_hls_pull_idle(pull);
        }
    }

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
    ngx_str_t             *copy, *ca_copy;
    u_char                *slash;

    if (stream == NULL || id == NULL || id->len == 0 || url == NULL
        || url->len == 0)
    {
        return NULL;
    }

    if (url->len > NGX_MEDIA_HLS_PULL_URL_MAX) {
        /*
         * Every segment URL this reader builds starts with this one (for a
         * relative entry, with the directory part of it), so a playlist URL
         * past the bound could only ever produce segment URLs the builder
         * refuses: say so here, where the operator can see the reason, rather
         * than leave a reader that polls the playlist and fetches no segment.
         */
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: hls pull %V url is longer than the %d byte "
                      "segment url bound", id, NGX_MEDIA_HLS_PULL_URL_MAX);
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
        /*
         * A separate variable on purpose: `copy` is still the URL's copy and
         * the base below is computed from it.  Reusing the name here made the
         * base come out of the certificate path, so every relative segment
         * resolved to a directory on this machine instead of the origin's.
         */
        ca_copy = ngx_pcalloc(stream->pool, sizeof(ngx_str_t));

        if (ca_copy == NULL) {
            return NULL;
        }

        ca_copy->data = ngx_pnalloc(stream->pool, ca_file->len + 1);

        if (ca_copy->data == NULL) {
            return NULL;
        }

        ngx_memcpy(ca_copy->data, ca_file->data, ca_file->len);
        ca_copy->data[ca_file->len] = '\0';
        ca_copy->len = ca_file->len;

        pull->ca_file = *ca_copy;
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
    pull->id = pull->source->id;

    sink.tracks = ngx_media_hls_pull_tracks;
    sink.frame = ngx_media_hls_pull_frame;

    if (ngx_media_ts_demux_init(&pull->demux, NULL, &sink, pull, log)
        != NGX_OK)
    {
        ngx_media_stream_source_remove(stream, pull->source);
        pull->source = NULL;
        return NULL;
    }
    pull->playlist = ngx_alloc(NGX_MEDIA_HLS_PULL_PLAYLIST_MAX, log);
    pull->segment = ngx_alloc(NGX_MEDIA_HLS_PULL_SEGMENT_MAX, log);

    /*
     * The segment URL scratch, sized from the one bound a URL may reach plus
     * its terminator: reader-thread scratch, owned by this reader and freed by
     * ngx_media_hls_pull_close() after it has joined that thread, so no
     * request that created the reader can free it out from under the thread.
     */
    pull->segment_url_capacity = NGX_MEDIA_HLS_PULL_URL_MAX + 1;
    pull->segment_url = ngx_alloc(pull->segment_url_capacity, log);

    if (pull->playlist == NULL || pull->segment == NULL
        || pull->segment_url == NULL)
    {
        ngx_free(pull->segment_url);
        pull->segment_url = NULL;
        ngx_free(pull->segment);
        pull->segment = NULL;
        ngx_free(pull->playlist);
        pull->playlist = NULL;
        ngx_media_ts_demux_destroy(&pull->demux);
        ngx_media_stream_source_remove(stream, pull->source);
        pull->source = NULL;
        return NULL;
    }

    pull->events_capacity = NGX_MEDIA_HLS_EVENT_CAPACITY;
    pull->events = ngx_alloc(pull->events_capacity
                             * sizeof(ngx_media_hls_event_t), log);

    if (pull->events == NULL) {
        ngx_free(pull->segment_url);
        pull->segment_url = NULL;
        ngx_free(pull->segment);
        pull->segment = NULL;
        ngx_free(pull->playlist);
        pull->playlist = NULL;
        ngx_media_ts_demux_destroy(&pull->demux);
        ngx_media_stream_source_remove(stream, pull->source);
        pull->source = NULL;
        return NULL;
    }

    ngx_memzero(pull->events,
                pull->events_capacity * sizeof(ngx_media_hls_event_t));

    if (pthread_mutex_init(&pull->events_mutex, NULL) != 0) {
        ngx_free(pull->events);
        pull->events = NULL;
        ngx_free(pull->segment_url);
        pull->segment_url = NULL;
        ngx_free(pull->segment);
        pull->segment = NULL;
        ngx_free(pull->playlist);
        pull->playlist = NULL;
        ngx_media_ts_demux_destroy(&pull->demux);
        ngx_media_stream_source_remove(stream, pull->source);
        pull->source = NULL;
        return NULL;
    }

    pull->events_mutex_initialized = 1;

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
        ngx_media_hls_pull_close(pull);
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
    (void) ngx_atomic_fetch_add(&pull->stopping, 1);

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
    ngx_media_hls_pull_event_clear(pull);

    if (pull->events_mutex_initialized) {
        (void) pthread_mutex_destroy(&pull->events_mutex);
        pull->events_mutex_initialized = 0;
    }

    ngx_free(pull->events);
    pull->events = NULL;
    ngx_free(pull->segment_url);
    pull->segment_url = NULL;
    ngx_free(pull->segment);
    pull->segment = NULL;
    ngx_free(pull->playlist);
    pull->playlist = NULL;


    ngx_media_ts_demux_destroy(&pull->demux);

    if (pull->stream != NULL && pull->source != NULL) {
        ngx_media_stream_source_remove(pull->stream, pull->source);
        pull->source = NULL;
    }
}

ngx_uint_t
ngx_media_hls_pull_stream_readers(const ngx_media_stream_t *stream)
{
    ngx_media_hls_pull_t  *pull;
    ngx_uint_t             count = 0;

    if (stream == NULL) {
        return 0;
    }

    (void) pthread_mutex_lock(&ngx_media_hls_pull_mutex);

    for (pull = ngx_media_hls_pull_all; pull != NULL; pull = pull->next) {

        if (pull->stream == stream) {
            count++;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_pull_mutex);

    return count;
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
            if (ngx_atomic_fetch_add(&pull->exited, 0)
                && ngx_media_hls_pull_source_removed(pull))
            {
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
ngx_media_hls_pull_drain_all(void)
{
    ngx_media_hls_pull_t  *pull;

    (void) pthread_mutex_lock(&ngx_media_hls_pull_mutex);

    for (pull = ngx_media_hls_pull_all; pull != NULL; pull = pull->next) {
        ngx_media_hls_pull_event_drain(pull);
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_pull_mutex);
}
void
ngx_media_hls_pull_stop_all(void)
{
    ngx_media_hls_pull_t  *pull;

    (void) pthread_mutex_lock(&ngx_media_hls_pull_mutex);

    for (pull = ngx_media_hls_pull_all; pull != NULL; pull = pull->next) {
        (void) ngx_atomic_fetch_add(&pull->stopping, 1);
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_pull_mutex);

    for (pull = ngx_media_hls_pull_all; pull != NULL; pull = pull->next) {
        if (pull->thread_started) {
            (void) pthread_join(pull->thread, NULL);
            pull->thread_started = 0;
        }

        ngx_media_hls_pull_event_clear(pull);

        if (pull->events_mutex_initialized) {
            (void) pthread_mutex_destroy(&pull->events_mutex);
            pull->events_mutex_initialized = 0;
        }

        ngx_free(pull->events);
        pull->events = NULL;
        ngx_free(pull->segment_url);
        pull->segment_url = NULL;
        ngx_free(pull->segment);
        pull->segment = NULL;
        ngx_free(pull->playlist);
        pull->playlist = NULL;
        ngx_media_ts_demux_destroy(&pull->demux);
    }
}

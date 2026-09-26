#include "ngx_media_hls_push.h"
#include "ngx_media_hls_playlist.h"
#include "ngx_media_hls_profile.h"
#include "ngx_media_http.h"
#include "ngx_media_destination.h"
#include "ngx_media_stream.h"
#include "ngx_media_egress_manager.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sys/stat.h>

#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define NGX_MEDIA_HLS_PUSH_QUEUE     64
#define NGX_MEDIA_HLS_PUSH_POOL      4

/*
 * A failed upload is retried for up to one segment duration (DASH-IF Live
 * Media Ingest: an ingest source retries for as long as the object is still
 * useful, then moves on), with a backoff that starts short and doubles.  The
 * item waits at the head of its destination's queue, so the uploader thread
 * serves other destinations meanwhile, and the order - segment before the
 * playlist that names it - is kept.
 */
#define NGX_MEDIA_HLS_PUSH_BUDGET_MS      2000    /* no duration stated */
#define NGX_MEDIA_HLS_PUSH_BACKOFF_MS     100
#define NGX_MEDIA_HLS_PUSH_BACKOFF_MAX_MS 1000

/* segments that never reached the destination, listed with EXT-X-GAP */
#define NGX_MEDIA_HLS_PUSH_MISSING   8
#define NGX_MEDIA_HLS_PUSH_NAME_MAX  64

/* segments uploaded and not yet expired, for DELETE */
#define NGX_MEDIA_HLS_PUSH_SENT      64
#define NGX_MEDIA_HLS_PUSH_DELETES   8       /* per playlist, at most */

#define NGX_MEDIA_HLS_PUSH_PLAYLIST_MAX  (256 * 1024)

enum {
    NGX_MEDIA_HLS_PUSH_DONE = 0,
    NGX_MEDIA_HLS_PUSH_FAILED,
    NGX_MEDIA_HLS_PUSH_RETRY,
    NGX_MEDIA_HLS_PUSH_SKIPPED
};

typedef struct {
    ngx_atomic_t  refs;
    int           fd;
    off_t         size;
    u_char        path[];
} ngx_media_hls_push_file_t;

typedef struct {
    ngx_media_hls_push_file_t  *file;
    struct timespec             enqueued;
    uint64_t                    first_ns;     /* first attempt, 0 before */
    uint64_t                    not_before_ns;
    ngx_uint_t                  attempts;
} ngx_media_hls_push_item_t;

typedef struct {
    u_char   data[NGX_MEDIA_HLS_PUSH_NAME_MAX];
    size_t   len;
} ngx_media_hls_push_name_t;


struct ngx_media_hls_push_t {
    /*
     * The object outlives its stream.  It and all strings used by uploader
     * threads are raw heap-owned, never tied to an NGINX pool.
     */
    ngx_atomic_t             refs;


    ngx_str_t                directory;    /* output directory */
    ngx_str_t                url;          /* remote endpoint, with trailing / */
    ngx_str_t                ca_file;      /* TLS trust anchor, empty for the
                                            * system store */

    uint64_t                 egress_token;
    ngx_uint_t               placement;

    /* the destination's settings, validated by its profile */
    const char              *method;       /* "PUT" or "POST" */
    ngx_uint_t               window;       /* 0: the stream's */
    ngx_uint_t               delete_expired;
    ngx_msec_t               budget_ms;    /* retry budget: a segment */

    /*
     * Used by the one upload in flight only (push->inflight serialises it),
     * so without the mutex: the kept connection and the segments uploaded
     * and not yet expired.
     */
    ngx_media_http_conn_t      conn;
    ngx_media_hls_push_name_t  sent[NGX_MEDIA_HLS_PUSH_SENT];
    ngx_uint_t                 nsent;
    ngx_media_hls_push_name_t  first;      /* its first segment */

    /* under the mutex: segments the destination does not have */
    ngx_media_hls_push_name_t  missing[NGX_MEDIA_HLS_PUSH_MISSING];
    ngx_uint_t                 nmissing;
    ngx_media_hls_push_item_t  queue[NGX_MEDIA_HLS_PUSH_QUEUE];
    ngx_uint_t               head, tail, count;
    ngx_media_hls_push_item_t  inflight_item;
    ngx_uint_t                 inflight;

    pthread_mutex_t          mutex;
    pthread_mutex_t          report_mutex;
    ngx_uint_t               stopping;


    uint64_t                 uploaded;
    uint64_t                 uploaded_bytes;
    uint64_t                 dropped;
    uint64_t                 failed;
    uint64_t                 retried;
    uint64_t                 gaps;
    uint64_t                 deleted;
    uint64_t                 reported_uploaded_bytes;
    uint64_t                 reported_dropped;
    uint64_t                 reported_failed;
    struct timespec          last_report;

    ngx_media_hls_push_t    *next;
};

static ngx_media_hls_push_t   *ngx_media_hls_push_all;
static pthread_mutex_t         ngx_media_hls_push_all_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t               ngx_media_hls_push_pool[NGX_MEDIA_HLS_PUSH_POOL];
static ngx_uint_t              ngx_media_hls_push_pool_size;
static ngx_atomic_t           ngx_media_hls_push_stopping;
static ngx_atomic_t          ngx_media_hls_push_active_workers;
static ngx_uint_t             ngx_media_hls_push_pool_ids[
    NGX_MEDIA_HLS_PUSH_POOL];
static uint64_t               ngx_media_hls_push_last_adapt_ns;
static uint64_t               ngx_media_hls_push_last_cpu_ns[
    NGX_MEDIA_HLS_PUSH_POOL];
static ngx_uint_t             ngx_media_hls_push_cpu_sampled;
static ngx_log_t              *ngx_media_hls_push_log;

#define NGX_MEDIA_HLS_PUSH_LOG(level, ...)                                    \
    do {                                                                      \
        if (ngx_media_hls_push_log != NULL) {                                 \
            ngx_log_error((level), ngx_media_hls_push_log, 0, __VA_ARGS__);   \
        }                                                                     \
    } while (0)

static ngx_uint_t
ngx_media_hls_push_active(void)
{
    return (ngx_uint_t)
        ngx_atomic_fetch_add(&ngx_media_hls_push_active_workers, 0);
}

static ngx_uint_t
ngx_media_hls_push_is_stopping(void)
{
    return ngx_atomic_fetch_add(&ngx_media_hls_push_stopping, 0) != 0;
}

static ngx_int_t
ngx_media_hls_push_set_active(ngx_uint_t active)
{
    ngx_uint_t  current;

    if (active == 0 || active > NGX_MEDIA_HLS_PUSH_POOL
        || ngx_media_hls_push_is_stopping())
    {
        return NGX_ERROR;
    }

    for ( ;; ) {
        current = ngx_media_hls_push_active();
        if (current == active
            || ngx_atomic_cmp_set(&ngx_media_hls_push_active_workers, current,
                                  active))
        {
            return NGX_OK;
        }
    }
}

static uint64_t
ngx_media_hls_push_now_ns(void)
{
    struct timespec  ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t) ts.tv_sec * UINT64_C(1000000000)
           + (uint64_t) ts.tv_nsec;
}

/* --- queue --------------------------------------------------------------- */

static const u_char *ngx_media_hls_push_basename(const u_char *path);
static ngx_uint_t ngx_media_hls_push_is_playlist(const u_char *name);
static void ngx_media_hls_push_missing_add(ngx_media_hls_push_t *push,
    const u_char *name);

/*
 * One reference on the object.  The list holds one, every upload in flight
 * holds one, and the last release frees only raw heap storage.
 */
static void
ngx_media_hls_push_release(ngx_media_hls_push_t *push)
{
    if (ngx_atomic_fetch_add(&push->refs, -1) != 1) {
        return;
    }

    (void) pthread_mutex_destroy(&push->report_mutex);
    (void) pthread_mutex_destroy(&push->mutex);

    ngx_media_http_conn_close(&push->conn);

    free(push->directory.data);
    free(push->url.data);
    free(push->ca_file.data);
    free(push);
}

static void
ngx_media_hls_push_file_release(ngx_media_hls_push_file_t *file)
{
    if (file != NULL && ngx_atomic_fetch_add(&file->refs, -1) == 1) {
        (void) close(file->fd);
        free(file);
    }
}

/* a heap copy, independent of the stream's pool */
static ngx_int_t
ngx_media_hls_push_strdup(const ngx_str_t *src, ngx_str_t *dst)
{
    dst->len = src->len;

    if (src->len == 0) {
        dst->data = NULL;
        return NGX_OK;
    }

    dst->data = malloc(src->len + 1);

    if (dst->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(dst->data, src->data, src->len);
    dst->data[src->len] = '\0';

    return NGX_OK;
}

#define NGX_MEDIA_HLS_PUSH_REPORT_INTERVAL_MS  1000

static ngx_msec_t
ngx_media_hls_push_elapsed_msec(const struct timespec *then,
    const struct timespec *now)
{
    int64_t  sec, nsec;

    sec = (int64_t) now->tv_sec - (int64_t) then->tv_sec;
    nsec = (int64_t) now->tv_nsec - (int64_t) then->tv_nsec;

    return (ngx_msec_t) (sec * 1000 + nsec / 1000000);
}

/*
 * Snapshot under the push mutex, then report without holding it.  The token
 * is copied at add time; no NGINX state is consulted by uploader threads.
 */
static void
ngx_media_hls_push_report(ngx_media_hls_push_t *push, ngx_uint_t force)
{
    ngx_media_egress_report_t  report;
    ngx_media_hls_push_item_t *item;
    struct timespec            now;
    ngx_uint_t                 i, index;
    size_t                     queue_bytes;
    ngx_msec_t                 lag;

    if (push->egress_token == 0) {
        return;
    }

    (void) pthread_mutex_lock(&push->report_mutex);

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        (void) pthread_mutex_unlock(&push->report_mutex);
        return;
    }

    (void) pthread_mutex_lock(&push->mutex);

    if (!force && push->last_report.tv_sec != 0
        && ngx_media_hls_push_elapsed_msec(&push->last_report, &now)
           < NGX_MEDIA_HLS_PUSH_REPORT_INTERVAL_MS)
    {
        (void) pthread_mutex_unlock(&push->mutex);
        (void) pthread_mutex_unlock(&push->report_mutex);
        return;
    }

    ngx_memzero(&report, sizeof(report));
    report.delivered_bytes = push->uploaded_bytes
                             - push->reported_uploaded_bytes;
    report.dropped_units = push->dropped - push->reported_dropped;
    report.transport_errors = push->failed - push->reported_failed;
    report.backpressure_events = report.dropped_units;
    report.placement = push->placement;

    push->reported_uploaded_bytes = push->uploaded_bytes;
    push->reported_dropped = push->dropped;
    push->reported_failed = push->failed;
    push->last_report = now;

    queue_bytes = (push->inflight && push->inflight_item.file != NULL)
                      ? (size_t) push->inflight_item.file->size : 0;
    lag = push->inflight
              ? ngx_media_hls_push_elapsed_msec(
                    &push->inflight_item.enqueued, &now)
              : 0;

    for (i = 0, index = push->head; i < push->count;
         i++, index = (index + 1) % NGX_MEDIA_HLS_PUSH_QUEUE)
    {
        item = &push->queue[index];
        if (item->file->size > 0) {
            queue_bytes += (size_t) item->file->size;
        }
        if (!push->inflight && i == 0) {
            lag = ngx_media_hls_push_elapsed_msec(&item->enqueued, &now);
        }
    }
    report.queue_bytes = queue_bytes;
    report.queue_lag_msec = lag;

    (void) pthread_mutex_unlock(&push->mutex);

    ngx_media_egress_manager_report(push->egress_token, &report);
    (void) pthread_mutex_unlock(&push->report_mutex);
}

static ngx_int_t
ngx_media_hls_push_enqueue(ngx_media_hls_push_t *push,
    ngx_media_hls_push_file_t *file)
{
    struct timespec  now;

    if (push == NULL || file == NULL) {
        return NGX_ERROR;
    }

    (void) pthread_mutex_lock(&push->mutex);

    if (push->stopping) {
        (void) pthread_mutex_unlock(&push->mutex);
        return NGX_ERROR;
    }

    if (push->count == NGX_MEDIA_HLS_PUSH_QUEUE) {
        const u_char  *lost;

        /* a segment dropped here is one the destination will not have */
        lost = ngx_media_hls_push_basename(push->queue[push->head].file->path);
        if (!ngx_media_hls_push_is_playlist(lost)) {
            ngx_media_hls_push_missing_add(push, lost);
        }

        ngx_media_hls_push_file_release(push->queue[push->head].file);
        ngx_memzero(&push->queue[push->head],
                    sizeof(push->queue[push->head]));
        push->head = (push->head + 1) % NGX_MEDIA_HLS_PUSH_QUEUE;
        push->count--;
        push->dropped++;
    }

    push->queue[push->tail].file = file;
    (void) clock_gettime(CLOCK_MONOTONIC, &now);
    push->queue[push->tail].enqueued = now;

    push->tail = (push->tail + 1) % NGX_MEDIA_HLS_PUSH_QUEUE;
    push->count++;

    (void) pthread_mutex_unlock(&push->mutex);

    ngx_media_hls_push_report(push, 0);
    return NGX_OK;
}

/* --- one object ----------------------------------------------------------- */

static const u_char *
ngx_media_hls_push_basename(const u_char *path)
{
    const u_char  *base = (const u_char *) strrchr((const char *) path, '/');

    return (base != NULL) ? base + 1 : path;
}

static ngx_uint_t
ngx_media_hls_push_is_playlist(const u_char *name)
{
    size_t  len = strlen((const char *) name);

    return len > 5
           && ngx_strncasecmp((u_char *) name + len - 5, (u_char *) ".m3u8", 5)
              == 0;
}

static void
ngx_media_hls_push_name_set(ngx_media_hls_push_name_t *n, const u_char *data,
    size_t len)
{
    n->len = (len < sizeof(n->data)) ? len : sizeof(n->data) - 1;
    ngx_memcpy(n->data, data, n->len);
    n->data[n->len] = '\0';
}

/* under the mutex: the destination lacks this segment */
static void
ngx_media_hls_push_missing_add(ngx_media_hls_push_t *push, const u_char *name)
{
    if (push->nmissing == NGX_MEDIA_HLS_PUSH_MISSING) {
        memmove(&push->missing[0], &push->missing[1],
                    (NGX_MEDIA_HLS_PUSH_MISSING - 1)
                    * sizeof(push->missing[0]));
        push->nmissing--;
    }

    ngx_media_hls_push_name_set(&push->missing[push->nmissing++], name,
                                strlen((const char *) name));
}

/*
 * A status worth trying again: no answer at all, a timeout, a rate limit, or
 * a server error.  Any other 4xx is the request itself, and sending it again
 * would get the same answer.
 */
static ngx_uint_t
ngx_media_hls_push_retryable(ngx_int_t status)
{
    return status == NGX_ERROR || status == 408 || status == 429
           || status >= 500;
}

/*
 * The destination's playlist: the stream's, trimmed to its window and with
 * its missing segments marked, and the DELETEs it makes due.
 */
static ngx_int_t
ngx_media_hls_push_playlist(ngx_media_hls_push_t *push,
    ngx_media_hls_push_file_t *file, const u_char *name)
{
    ngx_media_hls_playlist_opts_t   opts;
    ngx_media_hls_playlist_info_t  *info = NULL;
    ngx_str_t                       missing[NGX_MEDIA_HLS_PUSH_MISSING];
    ngx_str_t                       first;
    ngx_media_hls_push_name_t       names[NGX_MEDIA_HLS_PUSH_MISSING];
    ngx_media_hls_push_name_t       expired[NGX_MEDIA_HLS_PUSH_DELETES];
    ngx_uint_t                      i, j, k, nexpired = 0, found;
    u_char                         *in = NULL, *out = NULL;
    size_t                          out_len = 0, cap;
    ssize_t                         n;
    ngx_int_t                       status, rc;

    if (file->size <= 0 || file->size > NGX_MEDIA_HLS_PUSH_PLAYLIST_MAX) {
        return ngx_media_http_send(&push->conn, &push->url, &push->ca_file,
                                   push->method, name, file->fd, NULL,
                                   file->size, "application/vnd.apple.mpegurl",
                                   ngx_media_hls_push_log);
    }

    cap = (size_t) file->size + 64
          + NGX_MEDIA_HLS_PUSH_MISSING * sizeof("#EXT-X-GAP\n");
    in = malloc((size_t) file->size);
    out = malloc(cap);
    info = malloc(sizeof(*info));

    if (in == NULL || out == NULL || info == NULL) {
        status = NGX_ERROR;
        goto done;
    }

    n = pread(file->fd, in, (size_t) file->size, 0);

    (void) pthread_mutex_lock(&push->mutex);
    for (i = 0; i < push->nmissing; i++) {
        names[i] = push->missing[i];
        missing[i].data = names[i].data;
        missing[i].len = names[i].len;
    }
    opts.nmissing = push->nmissing;
    (void) pthread_mutex_unlock(&push->mutex);

    first.data = push->first.data;
    first.len = push->first.len;

    opts.window = push->window;
    opts.first = &first;
    opts.missing = missing;

    rc = (n == (ssize_t) file->size)
         ? ngx_media_hls_playlist_rewrite(in, (size_t) n, &opts, out, cap,
                                          &out_len, info)
         : NGX_ERROR;

    if (rc != NGX_OK) {
        /* not a playlist this can rewrite: it goes as it is */
        status = ngx_media_http_send(&push->conn, &push->url, &push->ca_file,
                                     push->method, name, file->fd, NULL,
                                     file->size,
                                     "application/vnd.apple.mpegurl",
                                     ngx_media_hls_push_log);
        goto done;
    }

    status = ngx_media_http_send(&push->conn, &push->url, &push->ca_file,
                                 push->method, name, -1, out, (off_t) out_len,
                                 "application/vnd.apple.mpegurl",
                                 ngx_media_hls_push_log);

    if (status < 200 || status >= 300) {
        goto done;
    }

    (void) pthread_mutex_lock(&push->mutex);

    push->gaps += info->gaps;

    /* a missing segment that left the window is no longer anyone's concern */
    for (i = 0, j = 0; i < push->nmissing; i++) {
        for (k = 0; k < info->segments; k++) {
            if (info->uri[k].len == push->missing[i].len
                && ngx_memcmp(info->uri[k].data, push->missing[i].data,
                              info->uri[k].len) == 0)
            {
                push->missing[j++] = push->missing[i];
                break;
            }
        }
    }
    push->nmissing = j;

    (void) pthread_mutex_unlock(&push->mutex);

    /*
     * Expiry, after the playlist that no longer lists them (DASH-IF: the
     * playlist goes first, so a reader never follows a playlist to a deleted
     * segment).  Everything uploaded before the first segment the window
     * still lists has expired.
     */
    found = push->nsent;

    for (k = 0; k < info->segments && found == push->nsent; k++) {
        for (i = 0; i < push->nsent; i++) {
            if (info->uri[k].len == push->sent[i].len
                && ngx_memcmp(info->uri[k].data, push->sent[i].data,
                              info->uri[k].len) == 0)
            {
                found = i;
                break;
            }
        }
    }

    if (found != push->nsent && found > 0) {
        for (i = 0; i < found; i++) {
            if (push->delete_expired && nexpired < NGX_MEDIA_HLS_PUSH_DELETES) {
                expired[nexpired++] = push->sent[i];
            }
        }

        memmove(&push->sent[0], &push->sent[found],
                    (push->nsent - found) * sizeof(push->sent[0]));
        push->nsent -= found;
    }

    for (i = 0; i < nexpired && push->delete_expired; i++) {
        rc = ngx_media_http_send(&push->conn, &push->url, &push->ca_file,
                                 "DELETE", expired[i].data, -1, NULL, 0, NULL,
                                 ngx_media_hls_push_log);

        if ((rc >= 200 && rc < 300) || rc == 404) {
            (void) pthread_mutex_lock(&push->mutex);
            push->deleted++;
            (void) pthread_mutex_unlock(&push->mutex);

        } else if (rc == 403 || rc == 405 || rc == 501) {
            /* an endpoint that does not take DELETE is not asked again */
            push->delete_expired = 0;

            NGX_MEDIA_HLS_PUSH_LOG(NGX_LOG_NOTICE,
                          "media: hls push endpoint answered DELETE with %i; "
                          "expired segments are left to it", rc);
        }
    }

done:

    free(info);
    free(out);
    free(in);

    return status;
}

/*
 * Uploads one queued object.  DONE, FAILED (given up on), RETRY with
 * item->not_before_ns set, or SKIPPED.
 */
static ngx_uint_t
ngx_media_hls_push_upload(ngx_media_hls_push_t *push,
    ngx_media_hls_push_item_t *item)
{
    ngx_media_hls_push_file_t  *file = item->file;
    const u_char               *name = ngx_media_hls_push_basename(file->path);
    ngx_uint_t                  playlist, i;
    ngx_int_t                   status;
    uint64_t                    now, backoff;

    playlist = ngx_media_hls_push_is_playlist(name);

    if (playlist && push->first.len == 0) {
        /*
         * A destination added between a segment and its playlist: the
         * playlist lists only segments this destination never had, and a
         * playlist never goes before its segments.  The next one will.
         */
        return NGX_MEDIA_HLS_PUSH_SKIPPED;
    }

    if (!playlist && push->first.len == 0) {
        ngx_media_hls_push_name_set(&push->first, name,
                                    strlen((const char *) name));
    }

    if (item->first_ns == 0) {
        item->first_ns = ngx_media_hls_push_now_ns();
    }

    item->attempts++;

    if (playlist) {
        status = ngx_media_hls_push_playlist(push, file, name);

    } else {
        status = ngx_media_http_send(&push->conn, &push->url, &push->ca_file,
                                     push->method, name, file->fd, NULL,
                                     file->size, "video/mp2t",
                                     ngx_media_hls_push_log);
    }

    if (status >= 200 && status < 300) {

        if (!playlist) {
            if (push->nsent == NGX_MEDIA_HLS_PUSH_SENT) {
                memmove(&push->sent[0], &push->sent[1],
                            (NGX_MEDIA_HLS_PUSH_SENT - 1)
                            * sizeof(push->sent[0]));
                push->nsent--;
            }
            ngx_media_hls_push_name_set(&push->sent[push->nsent++], name,
                                        strlen((const char *) name));

            /* a segment that arrives late is no longer missing */
            (void) pthread_mutex_lock(&push->mutex);
            for (i = 0; i < push->nmissing; i++) {
                if (strcmp((char *) push->missing[i].data, (const char *) name) == 0) {
                    memmove(&push->missing[i], &push->missing[i + 1],
                                (push->nmissing - i - 1)
                                * sizeof(push->missing[0]));
                    push->nmissing--;
                    break;
                }
            }
            (void) pthread_mutex_unlock(&push->mutex);
        }

        return NGX_MEDIA_HLS_PUSH_DONE;
    }

    now = ngx_media_hls_push_now_ns();
    backoff = (uint64_t) NGX_MEDIA_HLS_PUSH_BACKOFF_MS
              << (item->attempts - 1 < 4 ? item->attempts - 1 : 4);
    if (backoff > NGX_MEDIA_HLS_PUSH_BACKOFF_MAX_MS) {
        backoff = NGX_MEDIA_HLS_PUSH_BACKOFF_MAX_MS;
    }
    backoff *= UINT64_C(1000000);

    if (ngx_media_hls_push_retryable(status) && status != NGX_DECLINED
        && now + backoff - item->first_ns
           <= (uint64_t) push->budget_ms * UINT64_C(1000000))
    {
        item->not_before_ns = now + backoff;
        return NGX_MEDIA_HLS_PUSH_RETRY;
    }

    NGX_MEDIA_HLS_PUSH_LOG(NGX_LOG_WARN,
                  "media: hls push gave up on %s after %ui attempts "
                  "(last answer %i)%s", name, item->attempts, status,
                  playlist ? "" : "; playlists list it as a gap");

    if (!playlist) {
        (void) pthread_mutex_lock(&push->mutex);
        ngx_media_hls_push_missing_add(push, name);
        (void) pthread_mutex_unlock(&push->mutex);
    }

    return NGX_MEDIA_HLS_PUSH_FAILED;
}

/*
 * One uploader dequeues under the locks and uploads without them.  Holding
 * the destination list while a remote blocks would stall the event-loop
 * sealed-file notifier and unrelated destinations.
 */
static void *
ngx_media_hls_push_thread(void *data)
{
    ngx_media_hls_push_t       *push;
    ngx_media_hls_push_t       *work_push[NGX_MEDIA_HLS_PUSH_POOL * 2];
    ngx_media_hls_push_item_t   work_item[NGX_MEDIA_HLS_PUSH_POOL * 2];
    ngx_media_hls_push_file_t  *file;
    ngx_uint_t                  worker_id = *(ngx_uint_t *) data;
    ngx_uint_t                  nwork, i, got, active, outcome;
    uint64_t                    now_ns;

    for ( ;; ) {
        active = ngx_media_hls_push_active();
        if (worker_id >= active) {
            if (ngx_media_hls_push_is_stopping()) {
                return NULL;
            }
            usleep(20000);
            continue;
        }

        nwork = 0;
        now_ns = ngx_media_hls_push_now_ns();
        (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);

        for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {
            if (nwork >= NGX_MEDIA_HLS_PUSH_POOL * 2) {
                break;
            }

            (void) pthread_mutex_lock(&push->mutex);
            if (push->count == 0 || push->stopping || push->inflight
                || (push->queue[push->head].not_before_ns != 0
                    && push->queue[push->head].not_before_ns > now_ns))
            {
                (void) pthread_mutex_unlock(&push->mutex);
                continue;
            }

            work_item[nwork] = push->queue[push->head];
            ngx_memzero(&push->queue[push->head],
                        sizeof(push->queue[push->head]));
            work_push[nwork] = push;
            push->inflight = 1;
            push->inflight_item = work_item[nwork];
            nwork++;

            /*
             * The list lock protects this destination reference against
             * removal; the queued file reference transfers to the upload.
             */
            (void) ngx_atomic_fetch_add(&push->refs, 1);
            push->head = (push->head + 1) % NGX_MEDIA_HLS_PUSH_QUEUE;
            push->count--;

            (void) pthread_mutex_unlock(&push->mutex);
            ngx_media_hls_push_report(push, 0);
        }

        (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);
        got = nwork;

        for (i = 0; i < nwork; i++) {
            file = work_item[i].file;
            outcome = ngx_media_hls_push_upload(work_push[i], &work_item[i]);

            (void) pthread_mutex_lock(&work_push[i]->mutex);
            work_push[i]->inflight = 0;
            ngx_memzero(&work_push[i]->inflight_item,
                        sizeof(work_push[i]->inflight_item));

            if (outcome == NGX_MEDIA_HLS_PUSH_RETRY
                && !work_push[i]->stopping
                && work_push[i]->count < NGX_MEDIA_HLS_PUSH_QUEUE)
            {
                /* back to the head: it still goes before what follows it */
                work_push[i]->head = (work_push[i]->head
                                      + NGX_MEDIA_HLS_PUSH_QUEUE - 1)
                                     % NGX_MEDIA_HLS_PUSH_QUEUE;
                work_push[i]->queue[work_push[i]->head] = work_item[i];
                work_push[i]->count++;
                work_push[i]->retried++;
                file = NULL;            /* the queue holds the reference */

            } else if (outcome == NGX_MEDIA_HLS_PUSH_SKIPPED) {
                /* nothing sent, nothing lost */

            } else if (outcome == NGX_MEDIA_HLS_PUSH_DONE) {
                work_push[i]->uploaded++;
                work_push[i]->uploaded_bytes += (uint64_t) file->size;

            } else {
                work_push[i]->failed++;
            }

            (void) pthread_mutex_unlock(&work_push[i]->mutex);
            ngx_media_hls_push_file_release(file);
            ngx_media_hls_push_report(work_push[i], 0);

            /* last holder out frees the destination, uploads included */
            ngx_media_hls_push_release(work_push[i]);
        }

        if (got == 0) {
            if (ngx_media_hls_push_is_stopping()) {
                return NULL;
            }
            usleep(20000);
        }
    }
}

/* --- backend ------------------------------------------------------------- */

static ngx_int_t
ngx_media_hls_push_add(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination, ngx_log_t *log)
{
    ngx_media_hls_push_t  *push;

    if (destination->path.len == 0 || destination->host.len == 0) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: hls push destination %V needs a path (the "
                      "output directory) and a host (the endpoint)",
                      &destination->id);
        return NGX_ERROR;
    }

    push = calloc(1, sizeof(ngx_media_hls_push_t));

    if (push == NULL
        || ngx_media_hls_push_strdup(&destination->path,
                                     &push->directory) != NGX_OK
        || ngx_media_hls_push_strdup(&destination->host,
                                     &push->url) != NGX_OK
        || ngx_media_hls_push_strdup(&destination->ca_file,
                                     &push->ca_file) != NGX_OK)
    {
        if (push != NULL) {
            free(push->directory.data);
            free(push->url.data);
            free(push->ca_file.data);
            free(push);
        }
        return NGX_ERROR;
    }

    push->egress_token = destination->egress_token;
    push->placement = 0;
    push->method = (destination->http_method == NGX_MEDIA_HLS_PUSH_POST)
                   ? "POST" : "PUT";
    push->window = destination->playlist_window;
    push->delete_expired = destination->delete_expired;
    push->budget_ms = (destination->segment_duration_ms > 0)
                      ? destination->segment_duration_ms
                      : NGX_MEDIA_HLS_PUSH_BUDGET_MS;
    ngx_media_http_conn_init(&push->conn);
    push->refs = 1;                    /* the destination list's reference */

    if (pthread_mutex_init(&push->mutex, NULL) != 0) {
        free(push->directory.data);
        free(push->url.data);
        free(push->ca_file.data);
        free(push);
        return NGX_ERROR;
    }

    if (pthread_mutex_init(&push->report_mutex, NULL) != 0) {
        (void) pthread_mutex_destroy(&push->mutex);
        free(push->directory.data);
        free(push->url.data);
        free(push->ca_file.data);
        free(push);
        return NGX_ERROR;
    }


    (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);
    push->next = ngx_media_hls_push_all;
    ngx_media_hls_push_all = push;
    (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

    destination->impl = push;

    {
        /*
         * The endpoint carries the credential, so it is redacted before it
         * reaches a log.  Acceptance case 15 asks for exactly this, and the
         * safe way to get it is for the reporting path to be unable to print
         * the raw value at all.
         */
        u_char     safe[512];
        ngx_str_t  redacted;

        ngx_media_redact_url(&push->url, safe, sizeof(safe), &redacted);

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: hls push destination %V started for %V/%V: "
                      "%V -> %V (%s, window %ui, retry budget %M ms, "
                      "delete expired %s)", &destination->id,
                      &stream->application, &stream->name, &push->directory,
                      &redacted, push->method, push->window, push->budget_ms,
                      push->delete_expired ? "on" : "off");
    }

    return NGX_OK;
}

static void
ngx_media_hls_push_remove(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination)
{
    ngx_media_hls_push_t  *push = destination->impl;
    ngx_media_hls_push_t **link;

    (void) stream;

    if (push == NULL) {
        return;
    }

    /*
     * Unlink first, then stop: the list mutex protects dequeues, so after
     * this returns no uploader can take a new reference.  An in-flight upload
     * holds its own reference until it finishes.
     */
    (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);

    for (link = &ngx_media_hls_push_all; *link != NULL;
         link = &(*link)->next)
    {
        if (*link == push) {
            *link = push->next;
            break;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

    (void) pthread_mutex_lock(&push->mutex);
    push->stopping = 1;
    while (push->count > 0) {
        ngx_media_hls_push_file_release(push->queue[push->head].file);
        ngx_memzero(&push->queue[push->head],
                    sizeof(push->queue[push->head]));
        push->head = (push->head + 1) % NGX_MEDIA_HLS_PUSH_QUEUE;
        push->count--;
        push->dropped++;
    }
    push->tail = push->head;
    (void) pthread_mutex_unlock(&push->mutex);
    ngx_media_hls_push_report(push, 1);

    destination->impl = NULL;

    ngx_media_hls_push_release(push);
}

static ngx_media_destination_ops_t  ngx_media_hls_push_ops = {
    NGX_MEDIA_DEST_HLS_PUSH,
    ngx_media_hls_push_add,
    ngx_media_hls_push_remove
};

ngx_int_t
ngx_media_hls_push_register(ngx_log_t *log)
{
    ngx_uint_t  i, active;

    ngx_media_hls_push_log = log;
    ngx_media_hls_push_stopping = 0;
    ngx_media_hls_push_last_adapt_ns = 0;
    ngx_media_hls_push_cpu_sampled = 0;
    ngx_memzero(ngx_media_hls_push_last_cpu_ns,
                sizeof(ngx_media_hls_push_last_cpu_ns));
    active = ngx_media_egress_manager_fixed_workers(
                 NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL);
    if (active == 0) {
        active = 1;
    }
    if (ngx_media_hls_push_set_active(active) != NGX_OK
        || ngx_media_destination_register(&ngx_media_hls_push_ops) != NGX_OK)
    {
        return NGX_ERROR;
    }

    for (i = 0; i < NGX_MEDIA_HLS_PUSH_POOL; i++) {
        ngx_media_hls_push_pool_ids[i] = i;
        if (pthread_create(&ngx_media_hls_push_pool[i], NULL,
                           ngx_media_hls_push_thread,
                           &ngx_media_hls_push_pool_ids[i]) != 0)
        {
            (void) ngx_atomic_cmp_set(&ngx_media_hls_push_stopping, 0, 1);
            return NGX_ERROR;
        }
        ngx_media_hls_push_pool_size++;
    }

    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 0,
        ngx_media_hls_push_active());
    return NGX_OK;
}

void
ngx_media_hls_push_stop(void)
{
    ngx_uint_t  i;

    (void) ngx_atomic_cmp_set(&ngx_media_hls_push_stopping, 0, 1);
    for (i = 0; i < ngx_media_hls_push_pool_size; i++) {
        (void) pthread_join(ngx_media_hls_push_pool[i], NULL);
    }

    ngx_media_hls_push_pool_size = 0;
    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 0, 0);
}


void
ngx_media_hls_push_adapt(void)
{
    struct timespec  cpu_time;
    clockid_t        cpu_clock;
    pthread_t        *thread;
    uint64_t          wall_ns, wall_delta, cpu_ns, cpu_delta, busy;
    ngx_uint_t        active, peak_busy, i, desired, fixed;

    if (ngx_media_hls_push_pool_size == 0
        || ngx_media_hls_push_is_stopping())
    {
        return;
    }

    wall_ns = ngx_media_hls_push_now_ns();
    if (wall_ns == 0
        || (ngx_media_hls_push_last_adapt_ns != 0
            && wall_ns - ngx_media_hls_push_last_adapt_ns
               < UINT64_C(1000000000)))
    {
        return;
    }

    wall_delta = (ngx_media_hls_push_last_adapt_ns != 0)
                     ? wall_ns - ngx_media_hls_push_last_adapt_ns : 0;
    active = ngx_media_hls_push_active();
    peak_busy = 0;

    for (i = 0; i < ngx_media_hls_push_pool_size; i++) {
        thread = &ngx_media_hls_push_pool[i];
        if (pthread_getcpuclockid(*thread, &cpu_clock) != 0
            || clock_gettime(cpu_clock, &cpu_time) != 0)
        {
            ngx_media_hls_push_last_cpu_ns[i] = 0;
            continue;
        }

        cpu_ns = (uint64_t) cpu_time.tv_sec * UINT64_C(1000000000)
                 + (uint64_t) cpu_time.tv_nsec;
        if (i < active && ngx_media_hls_push_cpu_sampled
            && ngx_media_hls_push_last_cpu_ns[i] != 0
            && cpu_ns >= ngx_media_hls_push_last_cpu_ns[i]
            && wall_delta != 0)
        {
            cpu_delta = cpu_ns - ngx_media_hls_push_last_cpu_ns[i];
            busy = cpu_delta * 1000 / wall_delta;
            if (busy > peak_busy) {
                peak_busy = (busy > 1000) ? 1000 : (ngx_uint_t) busy;
            }
        }
        ngx_media_hls_push_last_cpu_ns[i] = cpu_ns;
    }

    ngx_media_hls_push_last_adapt_ns = wall_ns;
    if (!ngx_media_hls_push_cpu_sampled) {
        ngx_media_hls_push_cpu_sampled = 1;
        return;
    }

    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, peak_busy, active);
    fixed = ngx_media_egress_manager_fixed_workers(
                NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL);
    desired = (fixed != 0)
                  ? fixed
                  : ngx_media_egress_manager_recommend_workers(
                        NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, active, 1,
                        NGX_MEDIA_HLS_PUSH_POOL);
    if (desired != active
        && ngx_media_hls_push_set_active(desired) == NGX_OK)
    {
        ngx_media_egress_manager_engine_load(
            NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, peak_busy, desired);
    }
}

void
ngx_media_hls_push_sealed(const ngx_str_t *directory, const ngx_str_t *path,
    ngx_log_t *log)
{
    ngx_media_hls_push_t       *push;
    ngx_media_hls_push_file_t  *file;
    ngx_uint_t                  matched;
    struct stat                 st;
    int                         fd;
    ngx_int_t                   rc;
    int                         err;
    size_t                      bytes;

    if (directory == NULL || directory->data == NULL || directory->len == 0
        || path == NULL || path->data == NULL || path->len == 0
        || ngx_media_hls_push_all == NULL)
    {
        return;
    }

    (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);
    matched = 0;
    for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {
        if (push->directory.len == directory->len
            && ngx_memcmp(push->directory.data, directory->data,
                          directory->len) == 0)
        {
            matched = 1;
            break;
        }
    }

    if (!matched) {
        (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);
        return;
    }

    file = NULL;
    err = (path->len > SIZE_MAX - sizeof(*file) - 1)
              ? ENAMETOOLONG : 0;
    if (err == 0) {
        bytes = sizeof(*file) + path->len + 1;
        file = calloc(1, bytes);
        if (file == NULL) {
            err = ENOMEM;
        }
    }

    if (err == 0) {
        ngx_memcpy(file->path, path->data, path->len);
        file->path[path->len] = '\0';
        fd = open((char *) file->path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            err = ngx_errno;
            free(file);
            file = NULL;

        } else if (fstat(fd, &st) != 0) {
            err = ngx_errno;
            (void) close(fd);
            free(file);
            file = NULL;

        } else if (!S_ISREG(st.st_mode) || st.st_size < 0) {
            err = EINVAL;
            (void) close(fd);
            free(file);
            file = NULL;
        } else {
            file->fd = fd;
            file->size = st.st_size;
            file->refs = 1;             /* notifier's reference */
        }
    }

    for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {
        if (push->directory.len != directory->len
            || ngx_memcmp(push->directory.data, directory->data,
                          directory->len) != 0)
        {
            continue;
        }

        if (file == NULL) {
            (void) pthread_mutex_lock(&push->mutex);
            if (err == ENOENT) {
                push->dropped++;
            } else {
                push->failed++;
            }
            (void) pthread_mutex_unlock(&push->mutex);
            ngx_media_hls_push_report(push, 1);
            continue;
        }

        (void) ngx_atomic_fetch_add(&file->refs, 1);
        rc = ngx_media_hls_push_enqueue(push, file);
        if (rc != NGX_OK) {
            ngx_media_hls_push_file_release(file);
            if (log != NULL) {
                ngx_log_error(NGX_LOG_WARN, log, 0,
                              "media: hls push could not queue sealed file %V",
                              path);
            }
        }
    }
    (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

    ngx_media_hls_push_file_release(file);
}

ngx_uint_t
ngx_media_hls_push_count(void)
{
    ngx_media_hls_push_t  *push;
    ngx_uint_t             count = 0;

    (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);

    for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {
        count++;
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

    return count;
}

uint64_t
ngx_media_hls_push_uploaded_total(void)
{
    ngx_media_hls_push_t  *push;
    uint64_t               total = 0;
    (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);

    for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {
        (void) pthread_mutex_lock(&push->mutex);
        total += push->uploaded;
        (void) pthread_mutex_unlock(&push->mutex);
    }
    (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

    return total;
}

uint64_t
ngx_media_hls_push_dropped_total(void)
{
    ngx_media_hls_push_t  *push;
    uint64_t               total = 0;
    (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);

    for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {
        (void) pthread_mutex_lock(&push->mutex);
        total += push->dropped;
        (void) pthread_mutex_unlock(&push->mutex);
    }
    (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

    return total;
}

uint64_t
ngx_media_hls_push_failed_total(void)
{
    ngx_media_hls_push_t  *push;
    uint64_t               total = 0;
    (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);

    for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {
        (void) pthread_mutex_lock(&push->mutex);
        total += push->failed;
        (void) pthread_mutex_unlock(&push->mutex);
    }
    (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

    return total;
}

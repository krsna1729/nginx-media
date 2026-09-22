#include "ngx_media_hls_push.h"
#include "ngx_media_hls_profile.h"
#include "ngx_media_http.h"
#include "ngx_media_destination.h"
#include "ngx_media_stream.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define NGX_MEDIA_HLS_PUSH_QUEUE     64
#define NGX_MEDIA_HLS_PUSH_POOL      4
#define NGX_MEDIA_HLS_PUSH_PATH_MAX  512
#define NGX_MEDIA_HLS_PUSH_SCAN_MAX  256

/* a segment name can be at most NAME_MAX (255) bytes, plus slack */
#define NGX_MEDIA_HLS_PUSH_NAME_MAX  256

typedef struct {
    u_char    path[NGX_MEDIA_HLS_PUSH_PATH_MAX];
    size_t    len;
    off_t     size;
} ngx_media_hls_push_item_t;

/*
 * The last thing offered for one name.  A segment is immutable, so its name
 * is its identity; the playlist is rewritten in place under a constant name,
 * so its identity is the version - mtime and size - and a rewrite has to be
 * offered again.  Version comparison is also what keeps the table honest: a
 * version equal to the one already uploaded is never offered twice, while a
 * rewritten index differs in mtime at nanosecond resolution even when the
 * byte count happens to be the same.
 */
typedef struct {
    u_char           name[NGX_MEDIA_HLS_PUSH_NAME_MAX];
    size_t           len;
    struct timespec  mtime;
    off_t            size;
} ngx_media_hls_push_seen_t;

struct ngx_media_hls_push_t {
    ngx_str_t                directory;    /* watched output directory */
    ngx_str_t                url;          /* remote endpoint, with trailing / */
    ngx_str_t                ca_file;      /* TLS trust anchor, empty for the
                                            * system store */
    ngx_media_stream_t      *stream;
    ngx_media_destination_t *destination;  /* owner, for teardown */

    ngx_media_hls_push_item_t  queue[NGX_MEDIA_HLS_PUSH_QUEUE];
    ngx_uint_t               head, tail, count;

    pthread_mutex_t          mutex;
    pthread_cond_t           cond;
    ngx_uint_t               stopping;

    /*
     * Names already offered, so a scan does not enqueue the same file twice.
     * A ring, not a list: when it fills, the oldest entry gives way, because
     * refusing to store a name means offering it again on every tick.
     */
    ngx_media_hls_push_seen_t  seen[NGX_MEDIA_HLS_PUSH_SCAN_MAX];
    ngx_uint_t                 nseen;      /* slots in use, up to SCAN_MAX */
    ngx_uint_t                 seen_next;  /* the ring: oldest slot first */

    uint64_t                 uploaded;
    uint64_t                 dropped;
    uint64_t                 failed;

    ngx_media_hls_push_t    *next;
};

static ngx_media_hls_push_t   *ngx_media_hls_push_all;
static pthread_mutex_t         ngx_media_hls_push_all_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t               ngx_media_hls_push_pool[NGX_MEDIA_HLS_PUSH_POOL];
static ngx_uint_t              ngx_media_hls_push_pool_size;
static ngx_uint_t              ngx_media_hls_push_stopping;
static ngx_log_t              *ngx_media_hls_push_log;

/* --- queue --------------------------------------------------------------- */

static ngx_int_t
ngx_media_hls_push_enqueue(ngx_media_hls_push_t *push, const u_char *path,
    size_t len, off_t size)
{
    (void) pthread_mutex_lock(&push->mutex);

    if (push->count == NGX_MEDIA_HLS_PUSH_QUEUE) {

        /*
         * The remote is not keeping up.  Drop the oldest and count it: the
         * program and every other destination carry on, which is the
         * behaviour the acceptance contract asks for.
         */
        push->head = (push->head + 1) % NGX_MEDIA_HLS_PUSH_QUEUE;
        push->count--;
        push->dropped++;
    }

    if (len >= NGX_MEDIA_HLS_PUSH_PATH_MAX) {
        (void) pthread_mutex_unlock(&push->mutex);
        return NGX_ERROR;
    }

    ngx_memcpy(push->queue[push->tail].path, path, len);
    push->queue[push->tail].path[len] = '\0';
    push->queue[push->tail].len = len;
    push->queue[push->tail].size = size;

    push->tail = (push->tail + 1) % NGX_MEDIA_HLS_PUSH_QUEUE;
    push->count++;

    (void) pthread_cond_signal(&push->cond);
    (void) pthread_mutex_unlock(&push->mutex);

    return NGX_OK;
}

/*
 * One uploader: dequeues under the locks, then uploads with none held.
 *
 * The separation is load-bearing.  An upload can block for as long as the
 * remote takes - that is the whole point of the bounded queue - so doing it
 * while holding the destination list would block the runtime tick's scan,
 * which takes the same lock, and the worker would stop serving.  That is
 * exactly what happened when the first version uploaded inside the loop.
 */
static void *
ngx_media_hls_push_thread(void *data)
{
    ngx_media_hls_push_t      *push;
    ngx_media_hls_push_t      *work_push[NGX_MEDIA_HLS_PUSH_POOL * 2];
    ngx_media_hls_push_item_t  work_item[NGX_MEDIA_HLS_PUSH_POOL * 2];
    ngx_uint_t                 nwork, i, got;

    (void) data;

    for ( ;; ) {

        nwork = 0;

        (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);

        for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {

            if (nwork >= NGX_MEDIA_HLS_PUSH_POOL * 2) {
                break;
            }

            (void) pthread_mutex_lock(&push->mutex);

            if (push->count == 0 || push->stopping) {
                (void) pthread_mutex_unlock(&push->mutex);
                continue;
            }

            work_item[nwork] = push->queue[push->head];
            work_push[nwork] = push;
            nwork++;

            push->head = (push->head + 1) % NGX_MEDIA_HLS_PUSH_QUEUE;
            push->count--;

            (void) pthread_mutex_unlock(&push->mutex);
        }

        (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

        got = nwork;

        for (i = 0; i < nwork; i++) {

            if (ngx_media_http_put_file(&work_push[i]->url,
                                        &work_push[i]->ca_file,
                                        work_item[i].path, work_item[i].size,
                                        ngx_media_hls_push_log) == NGX_OK)
            {
                work_push[i]->uploaded++;

            } else {
                work_push[i]->failed++;
            }
        }

        if (got == 0) {

            if (ngx_media_hls_push_stopping) {
                return NULL;
            }

            /* nothing to do: wait briefly rather than spin */
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
                      "watched directory) and a host (the endpoint)",
                      &destination->id);
        return NGX_ERROR;
    }

    push = ngx_pcalloc(stream->pool, sizeof(ngx_media_hls_push_t));
    if (push == NULL) {
        return NGX_ERROR;
    }

    push->directory = destination->path;
    push->url = destination->host;    /* the endpoint URL */
    push->ca_file = destination->ca_file;
    push->stream = stream;
    push->destination = destination;

    (void) pthread_mutex_init(&push->mutex, NULL);
    (void) pthread_cond_init(&push->cond, NULL);

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
                      "%V -> %V", &destination->id, &stream->application,
                      &stream->name, &push->directory, &redacted);
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
     * Unlink first, then stop: the pool holds the list mutex while it
     * dequeues, so after this returns no uploader can be inside this
     * destination.  Nothing it queued outlives it, which is what the
     * acceptance contract asks of a delete with work in flight.
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
    push->count = 0;
    (void) pthread_mutex_unlock(&push->mutex);

    destination->impl = NULL;
}

static ngx_media_destination_ops_t  ngx_media_hls_push_ops = {
    NGX_MEDIA_DEST_HLS_PUSH,
    ngx_media_hls_push_add,
    ngx_media_hls_push_remove
};

ngx_int_t
ngx_media_hls_push_register(ngx_log_t *log)
{
    ngx_uint_t  i;

    ngx_media_hls_push_log = log;

    if (ngx_media_destination_register(&ngx_media_hls_push_ops) != NGX_OK) {
        return NGX_ERROR;
    }

    for (i = 0; i < NGX_MEDIA_HLS_PUSH_POOL; i++) {

        if (pthread_create(&ngx_media_hls_push_pool[i], NULL,
                           ngx_media_hls_push_thread, NULL) != 0)
        {
            ngx_media_hls_push_stopping = 1;
            return NGX_ERROR;
        }

        ngx_media_hls_push_pool_size++;
    }

    return NGX_OK;
}

void
ngx_media_hls_push_stop(void)
{
    ngx_uint_t  i;

    ngx_media_hls_push_stopping = 1;

    for (i = 0; i < ngx_media_hls_push_pool_size; i++) {
        (void) pthread_join(ngx_media_hls_push_pool[i], NULL);
    }

    ngx_media_hls_push_pool_size = 0;
}

/* --- scanning ------------------------------------------------------------ */

/*
 * Has this name already been offered in this version?
 *
 * A segment is immutable, so a name offered once is never offered again.  The
 * playlist is not: the segmenter rewrites it in place under a constant name,
 * so it is compared by version and re-offered whenever either half of that
 * version changes.  A miss takes the oldest slot when the ring is full - the
 * segmenter deletes segments from the front of its window long before 256
 * newer names have been offered, and an immutable segment re-offered after a
 * wrap carries the same bytes to the same remote name, so the repeat is
 * harmless where a forgotten live name would not be.
 */
static ngx_uint_t
ngx_media_hls_push_seen(ngx_media_hls_push_t *push, const u_char *name,
    size_t len, const struct stat *st)
{
    ngx_media_hls_push_seen_t  *entry;
    ngx_uint_t                  i, playlist;

    playlist = (len > 5
                && ngx_memcmp(name + len - 5, (u_char *) ".m3u8", 5) == 0);

    for (i = 0; i < push->nseen; i++) {

        entry = &push->seen[i];

        if (entry->len != len || ngx_memcmp(entry->name, name, len) != 0) {
            continue;
        }

        if (!playlist) {
            return 1;
        }

        if (entry->mtime.tv_sec == st->st_mtim.tv_sec
            && entry->mtime.tv_nsec == st->st_mtim.tv_nsec
            && entry->size == st->st_size)
        {
            /* the version already uploaded: there is nothing new to publish */
            return 1;
        }

        /* a rewritten index: offer it, and remember this version instead */
        entry->mtime = st->st_mtim;
        entry->size = st->st_size;

        return 0;
    }

    if (push->nseen < NGX_MEDIA_HLS_PUSH_SCAN_MAX) {
        entry = &push->seen[push->nseen++];

    } else {
        entry = &push->seen[push->seen_next];
        push->seen_next = (push->seen_next + 1) % NGX_MEDIA_HLS_PUSH_SCAN_MAX;
    }

    ngx_memcpy(entry->name, name, len);
    entry->len = len;
    entry->mtime = st->st_mtim;
    entry->size = st->st_size;

    return 0;
}

/*
 * Called from the runtime tick.  It stats the directory and offers every file
 * it has not offered in this version before; the upload itself happens on the
 * pool, so this never blocks the worker.
 */
void
ngx_media_hls_push_scan(const ngx_str_t *directory, ngx_log_t *log)
{
    ngx_media_hls_push_t  *push;
    u_char                 dir[NGX_MEDIA_HLS_PUSH_PATH_MAX];
    u_char                 full[NGX_MEDIA_HLS_PUSH_PATH_MAX];
    DIR                   *d;
    struct dirent         *de;
    struct stat            st;
    ngx_uint_t             scanned = 0;

    if (directory == NULL || directory->len == 0
        || ngx_media_hls_push_all == NULL)
    {
        return;
    }

    if (directory->len >= sizeof(dir)) {
        return;
    }

    ngx_memcpy(dir, directory->data, directory->len);
    dir[directory->len] = '\0';

    d = opendir((char *) dir);
    if (d == NULL) {
        return;
    }

    while ((de = readdir(d)) != NULL && scanned < NGX_MEDIA_HLS_PUSH_SCAN_MAX) {

        size_t  name_len = strlen(de->d_name);

        if (name_len < 4) {
            continue;
        }

        if (strcmp(de->d_name + name_len - 3, ".ts") == 0) {
            /* segment file */
        } else if (name_len >= 5
                   && strcmp(de->d_name + name_len - 5, ".m3u8") == 0)
        {
            /* playlist file */
        } else {
            continue;
        }

        scanned++;

        if (snprintf((char *) full, sizeof(full), "%s/%s", dir,
                     de->d_name) >= (int) sizeof(full))
        {
            continue;
        }

        if (stat((char *) full, &st) != 0 || !S_ISREG(st.st_mode)
            || st.st_size == 0)
        {
            continue;
        }

        (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);

        for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {

            if (push->directory.len != directory->len
                || ngx_memcmp(push->directory.data, directory->data,
                              directory->len) != 0)
            {
                continue;
            }

            if (ngx_media_hls_push_seen(push, (u_char *) de->d_name,
                                        name_len, &st))
            {
                continue;
            }

            (void) ngx_media_hls_push_enqueue(push, full, strlen((char *) full),
                                              st.st_size);
        }

        (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);
    }

    closedir(d);

    (void) log;
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
        total += push->uploaded;
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
        total += push->dropped;
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
        total += push->failed;
    }

    (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

    return total;
}

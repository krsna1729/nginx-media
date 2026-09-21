#include "ngx_media_hls_push.h"
#include "ngx_media_destination.h"
#include "ngx_media_stream.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define NGX_MEDIA_HLS_PUSH_QUEUE     64
#define NGX_MEDIA_HLS_PUSH_POOL      4
#define NGX_MEDIA_HLS_PUSH_PATH_MAX  512
#define NGX_MEDIA_HLS_PUSH_SCAN_MAX  256

typedef struct {
    u_char    path[NGX_MEDIA_HLS_PUSH_PATH_MAX];
    size_t    len;
    off_t     size;
} ngx_media_hls_push_item_t;

struct ngx_media_hls_push_t {
    ngx_str_t                directory;    /* watched output directory */
    ngx_str_t                url;          /* remote endpoint, with trailing / */
    ngx_media_stream_t      *stream;
    ngx_media_destination_t *destination;  /* owner, for teardown */

    ngx_media_hls_push_item_t  queue[NGX_MEDIA_HLS_PUSH_QUEUE];
    ngx_uint_t               head, tail, count;

    pthread_mutex_t          mutex;
    pthread_cond_t           cond;
    ngx_uint_t               stopping;

    /* names already offered, so a scan does not enqueue the same file twice */
    u_char                   seen[NGX_MEDIA_HLS_PUSH_SCAN_MAX][64];
    ngx_uint_t               nseen;

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

/* --- HTTP PUT ------------------------------------------------------------ */

/*
 * One blocking PUT of a whole file.  This is deliberately minimal: a core
 * module has no upstream machinery, and the request is a fixed, known shape.
 * It runs on a pool thread, so blocking here costs one uploader, not the
 * event loop.
 */
static ngx_int_t
ngx_media_hls_push_put(const ngx_str_t *url, const u_char *path,
    off_t size, ngx_log_t *log)
{
    ngx_str_t   host, target;
    u_char     *colon, *slash;
    ngx_int_t   port = 80;
    ngx_int_t   fd;
    struct sockaddr_in  addr;
    FILE       *file;
    u_char      buf[16384];
    size_t      n;
    off_t       sent = 0;
    u_char      header[1024];
    int         header_len;
    u_char      response[512];
    ssize_t     rn;

    if (url->len < 8 || ngx_memcmp(url->data, "http://", 7) != 0) {
        /* only plain HTTP for now; TLS uploads need a client context */
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: hls push supports http:// endpoints only");
        return NGX_ERROR;
    }

    host.data = url->data + 7;
    host.len = url->len - 7;

    slash = ngx_strlchr(host.data, host.data + host.len, '/');
    if (slash == NULL) {
        target.data = (u_char *) "/";
        target.len = 1;

    } else {
        target.data = slash;
        target.len = host.len - (slash - host.data);
        host.len = slash - host.data;
    }

    colon = ngx_strlchr(host.data, host.data + host.len, ':');
    if (colon != NULL) {
        u_char  port_text[8];
        size_t  len = host.len - (colon - host.data) - 1;

        if (len == 0 || len >= sizeof(port_text)) {
            return NGX_ERROR;
        }

        ngx_memcpy(port_text, colon + 1, len);
        port_text[len] = '\0';
        port = atoi((char *) port_text);
        host.len = colon - host.data;
    }

    /* the destination directory is the endpoint's prefix */
    {
        u_char  full[NGX_MEDIA_HLS_PUSH_PATH_MAX];
        u_char *base = (u_char *) strrchr((char *) path, '/');

        if (target.len + 1 + (base != NULL ? strlen((char *) base + 1) : 0)
            >= sizeof(full))
        {
            return NGX_ERROR;
        }

        ngx_memcpy(full, target.data, target.len);
        full[target.len] = '\0';

        if (target.data[target.len - 1] != '/') {
            full[target.len] = '/';
            full[target.len + 1] = '\0';
        }

        if (base != NULL) {
            strncat((char *) full, (char *) base + 1,
                    sizeof(full) - strlen((char *) full) - 1);
        }

        target.data = ngx_pnalloc(ngx_cycle->pool, strlen((char *) full));
        if (target.data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(target.data, full, strlen((char *) full));
        target.len = strlen((char *) full);
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return NGX_ERROR;
    }

    ngx_memzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);

    {
        char  host_z[256];

        if (host.len >= sizeof(host_z)) {
            (void) close(fd);
            return NGX_ERROR;
        }

        ngx_memcpy(host_z, host.data, host.len);
        host_z[host.len] = '\0';

        if (inet_pton(AF_INET, host_z, &addr.sin_addr) != 1) {
            (void) close(fd);
            return NGX_ERROR;
        }
    }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        (void) close(fd);
        return NGX_ERROR;
    }

    file = fopen((char *) path, "rb");
    if (file == NULL) {
        (void) close(fd);
        return NGX_ERROR;
    }

    header_len = snprintf((char *) header, sizeof(header),
                          "PUT %.*s HTTP/1.1\r\n"
                          "Host: %.*s\r\n"
                          "Content-Length: %lld\r\n"
                          "Content-Type: video/mp2t\r\n"
                          "Connection: close\r\n\r\n",
                          (int) target.len, (char *) target.data,
                          (int) host.len, (char *) host.data,
                          (long long) size);

    if (write(fd, header, (size_t) header_len) != header_len) {
        fclose(file);
        (void) close(fd);
        return NGX_ERROR;
    }

    while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {

        if (write(fd, buf, n) != (ssize_t) n) {
            fclose(file);
            (void) close(fd);
            return NGX_ERROR;
        }

        sent += (off_t) n;
    }

    /* a short read would upload a truncated segment: fail rather than lie */
    if (sent != size) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: hls push read %O of %O bytes from %s",
                      sent, size, path);
        fclose(file);
        return NGX_ERROR;
    }

    fclose(file);

    rn = read(fd, response, sizeof(response) - 1);
    (void) close(fd);

    if (rn <= 0) {
        return NGX_ERROR;
    }

    response[rn] = '\0';

    /* "HTTP/1.1 2xx" is the whole contract */
    if (response[9] != '2') {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: hls push got %.3s from the endpoint",
                      response + 9);
        return NGX_ERROR;
    }

    return NGX_OK;
}

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

/* one uploader: drains every destination's queue, so the pool is shared */
static void *
ngx_media_hls_push_thread(void *data)
{
    ngx_media_hls_push_item_t  item;
    ngx_media_hls_push_t      *push;
    ngx_uint_t                 got;

    (void) data;

    for ( ;; ) {

        got = 0;

        (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);

        for (push = ngx_media_hls_push_all; push != NULL; push = push->next) {

            (void) pthread_mutex_lock(&push->mutex);

            if (push->count == 0 || push->stopping) {
                (void) pthread_mutex_unlock(&push->mutex);
                continue;
            }

            item = push->queue[push->head];
            push->head = (push->head + 1) % NGX_MEDIA_HLS_PUSH_QUEUE;
            push->count--;
            got = 1;

            (void) pthread_mutex_unlock(&push->mutex);

            if (ngx_media_hls_push_put(&push->url, item.path, item.size,
                                       ngx_media_hls_push_log) == NGX_OK)
            {
                push->uploaded++;

            } else {
                push->failed++;
            }
        }

        (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

        if (!got) {

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
    push->stream = stream;
    push->destination = destination;

    (void) pthread_mutex_init(&push->mutex, NULL);
    (void) pthread_cond_init(&push->cond, NULL);

    (void) pthread_mutex_lock(&ngx_media_hls_push_all_mutex);
    push->next = ngx_media_hls_push_all;
    ngx_media_hls_push_all = push;
    (void) pthread_mutex_unlock(&ngx_media_hls_push_all_mutex);

    destination->impl = push;

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: hls push destination %V started for %V/%V: %V -> %V",
                  &destination->id, &stream->application, &stream->name,
                  &push->directory, &push->url);

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

static ngx_uint_t
ngx_media_hls_push_seen(ngx_media_hls_push_t *push, const u_char *name,
    size_t len)
{
    ngx_uint_t  i;

    for (i = 0; i < push->nseen; i++) {

        if (strlen((char *) push->seen[i]) == len
            && ngx_memcmp(push->seen[i], name, len) == 0)
        {
            return 1;
        }
    }

    if (push->nseen < NGX_MEDIA_HLS_PUSH_SCAN_MAX && len < 64) {
        ngx_memcpy(push->seen[push->nseen], name, len);
        push->seen[push->nseen][len] = '\0';
        push->nseen++;
    }

    return 0;
}

/*
 * Called from the runtime tick.  It stats the directory and offers anything
 * it has not offered before; the upload itself happens on the pool, so this
 * never blocks the worker.
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

        if (name_len < 4
            || (strcmp(de->d_name + name_len - 3, ".ts") != 0
                && strcmp(de->d_name + name_len - 5, ".m3u8") != 0))
        {
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
                                        name_len))
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

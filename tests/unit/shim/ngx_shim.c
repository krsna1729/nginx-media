#include "ngx_media_http.h"

#include "ngx_shim.h"

#include <stdlib.h>
#include <time.h>

size_t ngx_media_test_allocs;
size_t ngx_media_test_frees;

int ngx_media_test_fail_alloc;

/*
 * When NGX_MEDIA_TEST_POISON is set, allocations are 0xAA filled and carry a
 * poisoned tail: a buffer handed to a syscall without a terminator then reads
 * 0xAA instead of zeros, producing a visibly wrong filename.
 */
#define NGX_MEDIA_TEST_REDZONE 32
static int ngx_media_test_poison = -1;

void *
ngx_media_test_alloc(size_t size, ngx_log_t *log)
{
    void  *p;

    (void) log;

    if (ngx_media_test_fail_alloc) {
        return NULL;
    }

    if (ngx_media_test_poison < 0) {
        const char  *env = getenv("NGX_MEDIA_TEST_POISON");

        ngx_media_test_poison = (env != NULL && env[0] == '1') ? 1 : 0;
    }

    if (ngx_media_test_poison) {
        p = malloc(size + NGX_MEDIA_TEST_REDZONE);

        if (p != NULL) {
            memset(p, 0xAA, size + NGX_MEDIA_TEST_REDZONE);
        }

    } else {
        p = malloc(size);
    }

    if (p != NULL) {
        /* counters are read and written from test threads too */
        (void) __atomic_fetch_add(&ngx_media_test_allocs, 1, __ATOMIC_SEQ_CST);
    }

    return p;
}

void
ngx_media_test_free(void *ptr)
{
    if (ptr != NULL) {
        (void) __atomic_fetch_add(&ngx_media_test_frees, 1, __ATOMIC_SEQ_CST);
    }

    free(ptr);
}

typedef struct ngx_media_test_pool_block_s {
    struct ngx_media_test_pool_block_s  *next;
    void                                *ptr;
} ngx_media_test_pool_block_t;

struct ngx_pool_s {
    ngx_media_test_pool_block_t  *blocks;
};

ngx_pool_t *
ngx_create_pool(size_t size, ngx_log_t *log)
{
    ngx_pool_t  *pool;

    (void) size;

    pool = ngx_media_test_alloc(sizeof(ngx_pool_t), log);
    if (pool == NULL) {
        return NULL;
    }

    pool->blocks = NULL;

    return pool;
}

void *
ngx_palloc(ngx_pool_t *pool, size_t size)
{
    ngx_media_test_pool_block_t  *block;
    void                         *p;

    if (pool == NULL) {
        return NULL;
    }

    block = ngx_media_test_alloc(sizeof(ngx_media_test_pool_block_t), NULL);
    p = ngx_media_test_alloc(size, NULL);

    if (block == NULL || p == NULL) {
        ngx_media_test_free(block);
        ngx_media_test_free(p);
        return NULL;
    }

    ngx_memzero(p, size);

    block->ptr = p;
    block->next = pool->blocks;
    pool->blocks = block;

    return p;
}

void *
ngx_pnalloc(ngx_pool_t *pool, size_t size)
{
    return ngx_palloc(pool, size);
}

void *
ngx_pcalloc(ngx_pool_t *pool, size_t size)
{
    return ngx_palloc(pool, size);
}

void
ngx_destroy_pool(ngx_pool_t *pool)
{
    ngx_media_test_pool_block_t  *block, *next;

    if (pool == NULL) {
        return;
    }

    for (block = pool->blocks; block != NULL; block = next) {
        next = block->next;
        ngx_media_test_free(block->ptr);
        ngx_media_test_free(block);
    }

    ngx_media_test_free(pool);
}

/* see ngx_shim.h: the unit build has no event loop refreshing this clock */
ngx_msec_t
ngx_media_test_msec(void)
{
    struct timespec  ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);

    return (ngx_msec_t) ((uint64_t) ts.tv_sec * 1000
                         + (uint64_t) ts.tv_nsec / 1000000);
}

/* see ngx_shim.h: the unit build has no log target */
void
ngx_log_error(ngx_uint_t level, ngx_log_t *log, int err, const char *fmt, ...)
{
    (void) level;
    (void) log;
    (void) err;
    (void) fmt;
}

/*
 * Unit suites do not perform network I/O.  HLS push integration exercises the
 * real HTTP client; the linked worker-pool code still uses its real notifier.
 */
ngx_int_t
ngx_media_http_put_file(const ngx_str_t *url, const ngx_str_t *ca_file,
    const u_char *path, int file_fd, off_t size, ngx_log_t *log)
{
    (void) url;
    (void) ca_file;
    (void) path;
    (void) file_fd;
    (void) size;
    (void) log;

    return NGX_ERROR;
}

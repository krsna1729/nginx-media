#include "ngx_shim.h"

#include <stdlib.h>

size_t ngx_media_test_allocs;
size_t ngx_media_test_frees;

void *
ngx_media_test_alloc(size_t size, ngx_log_t *log)
{
    void  *p;

    (void) log;

    p = malloc(size);

    if (p != NULL) {
        ngx_media_test_allocs++;
    }

    return p;
}

void
ngx_media_test_free(void *ptr)
{
    if (ptr != NULL) {
        ngx_media_test_frees++;
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

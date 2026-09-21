#ifndef NGX_MEDIA_TEST_SHIM_H
#define NGX_MEDIA_TEST_SHIM_H

/*
 * Test-only stand-in for the NGINX runtime surface used by the portable media
 * core.  Unit tests compile the core sources with -DNGX_MEDIA_UNIT_TEST so that
 * ngx_media_platform.h pulls this header instead of the NGINX headers.
 * Keep this surface tiny: only types and primitives the core actually uses.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef intptr_t                    ngx_int_t;
typedef uintptr_t                   ngx_uint_t;
typedef ngx_uint_t                  ngx_msec_t;
typedef ngx_int_t                   ngx_flag_t;
typedef uintptr_t                   ngx_atomic_uint_t;
typedef volatile ngx_atomic_uint_t  ngx_atomic_t;
typedef unsigned char               u_char;

typedef struct {
    size_t   len;
    u_char  *data;
} ngx_str_t;

typedef struct ngx_pool_s  ngx_pool_t;
typedef struct ngx_log_s   ngx_log_t;
typedef struct ngx_queue_s ngx_queue_t;

struct ngx_queue_s {
    ngx_queue_t  *prev;
    ngx_queue_t  *next;
};

#define NGX_OK          0
#define NGX_ERROR      -1
#define NGX_AGAIN      -2
#define NGX_BUSY       -3
#define NGX_DECLINED   -5

/* allocation accounting: lets tests prove buffer and feed lifetimes */
extern size_t ngx_media_test_allocs;
extern size_t ngx_media_test_frees;

void *ngx_media_test_alloc(size_t size, ngx_log_t *log);
void ngx_media_test_free(void *ptr);

#define ngx_alloc(size, log)     ngx_media_test_alloc((size), (log))
#define ngx_free(p)              ngx_media_test_free(p)

#define ngx_memzero(buf, n)      (void) memset((buf), 0, (n))
#define ngx_memcpy(dst, src, n)  (void) memcpy((dst), (src), (n))
#define ngx_memcmp(s1, s2, n)    memcmp((s1), (s2), (n))

#define ngx_atomic_fetch_add(p, n) \
    __atomic_fetch_add((p), (n), __ATOMIC_SEQ_CST)

#define ngx_atomic_cmp_set(lock, old, set) \
    __sync_bool_compare_and_swap((lock), (old), (set))

#endif /* NGX_MEDIA_TEST_SHIM_H */

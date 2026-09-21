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

/* the small part of the nginx cycle the portable core can rely on */
typedef struct ngx_cycle_s {
    ngx_pool_t  *pool;
    ngx_log_t   *log;
} ngx_cycle_t;
typedef struct ngx_queue_s ngx_queue_t;

struct ngx_queue_s {
    ngx_queue_t  *prev;
    ngx_queue_t  *next;
};

/* the queue macros mirror nginx's ngx_queue.h */
#define ngx_queue_init(q)                                                     \
    (q)->prev = q;                                                            \
    (q)->next = q

#define ngx_queue_empty(h) (h == (h)->prev)

#define ngx_queue_insert_head(h, x)                                           \
    (x)->next = (h)->next;                                                    \
    (x)->next->prev = x;                                                      \
    (x)->prev = h;                                                            \
    (h)->next = x

#define ngx_queue_insert_tail(h, x)                                           \
    (x)->prev = (h)->prev;                                                    \
    (x)->prev->next = x;                                                      \
    (x)->next = h;                                                            \
    (h)->prev = x

#define ngx_queue_head(h) (h)->next
#define ngx_queue_last(h) (h)->prev

#define ngx_queue_remove(x)                                                   \
    (x)->next->prev = (x)->prev;                                              \
    (x)->prev->next = (x)->next

#define ngx_queue_data(q, type, link)                                         \
    (type *) ((u_char *) q - offsetof(type, link))

/*
 * The policy keeps its switch flag in nginx's flag-slot sentinel so that
 * ngx_conf_set_flag_slot can own it.  Test builds have no nginx headers, so
 * the same value has to exist here; nginx defines it as -1.
 */
#ifndef NGX_CONF_UNSET
#define NGX_CONF_UNSET  -1
#endif

#define NGX_OK          0
#define NGX_ERROR      -1
#define NGX_AGAIN      -2
#define NGX_BUSY       -3
#define NGX_DECLINED   -5

/* allocation accounting: lets tests prove buffer and feed lifetimes */
extern size_t ngx_media_test_allocs;
extern size_t ngx_media_test_frees;

/* set to 1 to make every allocation fail, for NULL handling tests */
extern int ngx_media_test_fail_alloc;

void *ngx_media_test_alloc(size_t size, ngx_log_t *log);
void ngx_media_test_free(void *ptr);

/*
 * Minimal pool: the test double tracks every allocation so leaks are visible
 * and frees them all on destroy.  Unlike ngx_palloc(), memory is zeroed, which
 * can only make tests stricter than production.
 */
ngx_pool_t *ngx_create_pool(size_t size, ngx_log_t *log);
void *ngx_palloc(ngx_pool_t *pool, size_t size);
void *ngx_pnalloc(ngx_pool_t *pool, size_t size);
void *ngx_pcalloc(ngx_pool_t *pool, size_t size);
void ngx_destroy_pool(ngx_pool_t *pool);

#define ngx_alloc(size, log)     ngx_media_test_alloc((size), (log))
#define ngx_free(p)              ngx_media_test_free(p)

/*
 * nginx defines this in ngx_config.h, which the unit build does not include.
 * The module headers use it, so the shim has to supply it.
 */
#define ngx_inline               inline

#define ngx_memzero(buf, n)      (void) memset((buf), 0, (n))
#define ngx_memcpy(dst, src, n)  (void) memcpy((dst), (src), (n))
#define ngx_memcmp(s1, s2, n)    memcmp((s1), (s2), (n))

#define ngx_atomic_fetch_add(p, n) \
    __atomic_fetch_add((p), (n), __ATOMIC_SEQ_CST)

#define ngx_atomic_cmp_set(lock, old, set) \
    __sync_bool_compare_and_swap((lock), (old), (set))

#endif /* NGX_MEDIA_TEST_SHIM_H */

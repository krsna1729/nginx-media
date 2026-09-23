#ifndef NGX_MEDIA_REGISTRY_H
#define NGX_MEDIA_REGISTRY_H

#include "ngx_media.h"
#include "ngx_media_stream.h"
#include "ngx_media_tombstone.h"

/*
 * Runtime stream registry.
 *
 * Streams are runtime objects (goal doc normative revision): configuration
 * establishes capabilities, not the stream database.  The registry is
 * per-worker state that both the SRT ingest module and the control API read
 * and write.
 *
 * Ownership is worker-local: the SRT listener runs in worker 0, so the
 * registry contents live there.  Cross-worker access is the inter-worker
 * routing phase (goal doc 23).
 */

/*
 * One stream, with the pool that carries it.
 *
 * The pool is the entry's own rather than the registry's, so deleting a
 * stream releases its memory instead of leaving it in the cycle pool until
 * the worker exits.  A reader that holds a thread - an origin being pulled, a
 * directory being watched - can be inside that memory while it stops, which
 * is what the draining list is for: the entry leaves the registry at once,
 * and its pool waits on the draining list until the last reader that
 * references it has been closed.
 */
typedef struct {
    ngx_queue_t         link;
    ngx_pool_t         *pool;
    ngx_media_stream_t  stream;

    ngx_msec_t          draining_since;   /* when the delete found it busy */
    ngx_uint_t          draining_warned;  /* the slow-reader warning is once */
} ngx_media_registry_entry_t;

typedef struct {
    ngx_queue_t   entries;
    ngx_queue_t   draining;
    ngx_pool_t   *pool;
    ngx_log_t    *log;
    ngx_uint_t    count;
    ngx_uint_t    draining_count;

    /* streams that were deleted: see ngx_media_tombstone.h */
    ngx_media_tombstone_ring_t  tombstones;
} ngx_media_registry_t;

/* per-worker registry, created on first use from the cycle pool */
ngx_media_registry_t *ngx_media_registry_get(ngx_cycle_t *cycle);

ngx_int_t ngx_media_registry_init(ngx_media_registry_t *registry,
    ngx_pool_t *pool, ngx_log_t *log);
void ngx_media_registry_destroy(ngx_media_registry_t *registry);

ngx_media_stream_t *ngx_media_registry_stream(ngx_media_registry_t *registry,
    const ngx_str_t *application, const ngx_str_t *name);

/* Pointer-liveness check for transport sessions that outlive a delete. */
ngx_uint_t ngx_media_registry_stream_is_live(
    const ngx_media_registry_t *registry, const ngx_media_stream_t *stream);
/*
 * log is kept: the stream's pool and everything built from it log through it,
 * and a stream outlives the connection that created it.  Pass the worker's
 * log, never a connection's.
 */
ngx_media_stream_t *ngx_media_registry_stream_create(
    ngx_media_registry_t *registry, const ngx_str_t *application,
    const ngx_str_t *name, const ngx_media_feed_conf_t *feed_conf,
    ngx_log_t *log);

/* ordered teardown; NGX_ERROR when the stream is not in this registry */
ngx_int_t ngx_media_registry_stream_destroy(ngx_media_registry_t *registry,
    ngx_media_stream_t *stream);

/*
 * Remembers a deletion, and answers whether an operation is old enough that
 * the deletion has already moved past it: a replica drops such an operation
 * instead of letting it create the stream again.  1 means "drop it".
 */
void ngx_media_registry_tombstone(ngx_media_registry_t *registry,
    uint64_t hash, uint64_t revision);
ngx_uint_t ngx_media_registry_is_tombstoned(
    const ngx_media_registry_t *registry, uint64_t hash, uint64_t revision);

/*
 * Releases the pools of the streams whose readers have all stopped, and
 * returns at once when there are none.  Called from the runtime tick, after
 * the readers have been reaped: a reader that holds a thread is closed there,
 * and closing it is what lets the stream it points at be freed.
 */
void ngx_media_registry_drain(ngx_media_registry_t *registry, ngx_log_t *log);

/* streams deleted whose pool is still held by a reader that is stopping */
ngx_uint_t ngx_media_registry_draining_count(
    const ngx_media_registry_t *registry);

ngx_uint_t ngx_media_registry_count(const ngx_media_registry_t *registry);

#endif /* NGX_MEDIA_REGISTRY_H */

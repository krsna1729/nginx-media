#ifndef NGX_MEDIA_REGISTRY_H
#define NGX_MEDIA_REGISTRY_H

#include "ngx_media.h"
#include "ngx_media_stream.h"

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

typedef struct {
    ngx_queue_t         link;
    ngx_media_stream_t  stream;
} ngx_media_registry_entry_t;

typedef struct {
    ngx_queue_t   entries;
    ngx_pool_t   *pool;
    ngx_uint_t    count;
} ngx_media_registry_t;

/* per-worker registry, created on first use from the cycle pool */
ngx_media_registry_t *ngx_media_registry_get(ngx_cycle_t *cycle);

ngx_int_t ngx_media_registry_init(ngx_media_registry_t *registry,
    ngx_pool_t *pool, ngx_log_t *log);
void ngx_media_registry_destroy(ngx_media_registry_t *registry);

ngx_media_stream_t *ngx_media_registry_stream(ngx_media_registry_t *registry,
    const ngx_str_t *application, const ngx_str_t *name);
ngx_media_stream_t *ngx_media_registry_stream_create(
    ngx_media_registry_t *registry, const ngx_str_t *application,
    const ngx_str_t *name, const ngx_media_feed_conf_t *feed_conf,
    ngx_log_t *log);

/* ordered teardown; NGX_ERROR when the stream is not in this registry */
ngx_int_t ngx_media_registry_stream_destroy(ngx_media_registry_t *registry,
    ngx_media_stream_t *stream);

ngx_uint_t ngx_media_registry_count(const ngx_media_registry_t *registry);

#endif /* NGX_MEDIA_REGISTRY_H */

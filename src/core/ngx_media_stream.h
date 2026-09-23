#ifndef NGX_MEDIA_STREAM_H
#define NGX_MEDIA_STREAM_H

#include "ngx_media.h"
#include "ngx_media_policy.h"
#include "ngx_media_source.h"

/*
 * Logical stream: source registry, activation gate and program feed
 * (goal doc 4.3, 5, 7, 13).
 *
 *   sources -> safe selection -> continuous program -> program feed
 *
 * Publishing a frame is the only path into the program feed:
 *
 *   - an ACTIVE source publishes immediately, holding a write lease
 *   - a standby source only fills its complete-GOP cache
 *   - a pending promotion completes only at a decodable boundary, only after
 *     the demoted source's leases have drained, and it bumps the generation
 *     so consumers see an explicit discontinuity
 *
 * The timeline maps source timestamps to monotonic program timestamps.
 */

/* applies a selection policy (defaults are applied at init) */
void ngx_media_stream_set_policy(ngx_media_stream_t *stream,
    const ngx_media_policy_t *policy);

/* bumps the object revision: call it from every desired-state mutation */
void ngx_media_stream_touch(ngx_media_stream_t *stream);

/* unique identity for one application/name object lifetime */
uint64_t ngx_media_stream_incarnation_next(void);

ngx_int_t ngx_media_stream_init(ngx_media_stream_t *stream, ngx_pool_t *pool,
    ngx_log_t *log, const ngx_str_t *application, const ngx_str_t *name,
    const ngx_media_feed_conf_t *feed_conf);
void ngx_media_stream_destroy(ngx_media_stream_t *stream);

ngx_media_source_t *ngx_media_stream_source_add(ngx_media_stream_t *stream,
    const ngx_str_t *id, ngx_uint_t type, ngx_uint_t priority, ngx_log_t *log);
ngx_media_source_t *ngx_media_stream_source_find(ngx_media_stream_t *stream,
    const ngx_str_t *id);
void ngx_media_stream_source_remove(ngx_media_stream_t *stream,
    ngx_media_source_t *source);
ngx_uint_t ngx_media_stream_source_count(const ngx_media_stream_t *stream);

ngx_int_t ngx_media_stream_publish(ngx_media_stream_t *stream,
    ngx_media_source_t *source, const ngx_media_frame_t *frame,
    ngx_msec_t now);

/* manual promotion: completes at the cached boundary or at the next keyframe */
ngx_int_t ngx_media_stream_promote(ngx_media_stream_t *stream,
    ngx_media_source_t *source);

/* retries deferred switches and removals after leases drain */
void ngx_media_stream_switch_resolve(ngx_media_stream_t *stream);

ngx_int_t ngx_media_stream_lease_begin(ngx_media_stream_t *stream,
    ngx_media_source_t *source);
void ngx_media_stream_lease_end(ngx_media_stream_t *stream,
    ngx_media_source_t *source);

#endif /* NGX_MEDIA_STREAM_H */

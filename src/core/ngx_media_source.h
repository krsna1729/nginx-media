#ifndef NGX_MEDIA_SOURCE_H
#define NGX_MEDIA_SOURCE_H

#include "ngx_media.h"

/*
 * Source standby cache and write leases (goal doc 5, 6).
 *
 * Every standby source keeps a *complete-GOP* cache: it starts at a video
 * keyframe, a newer keyframe replaces it wholesale, and a hard unit or byte
 * ceiling clears it rather than keeping an incomplete GOP.  Sources without a
 * video track use the explicit alternative rule that their first frame opens
 * the cache.
 *
 * A producer acquires a short-lived write lease before publishing a frame.
 * Promotions wait for outstanding leases of the demoted source to drain, so
 * frames from the old and the new source can never enter the program feed as
 * concurrent active writers.
 */

#define NGX_MEDIA_PREROLL_DEFAULT_UNITS  512
#define NGX_MEDIA_PREROLL_DEFAULT_BYTES  (4 * 1024 * 1024)

typedef ngx_int_t (*ngx_media_preroll_replay_pt)(void *ctx,
    const ngx_media_frame_t *frame);

/* bumps the object revision: call it from every desired-state mutation */
void ngx_media_source_touch(ngx_media_source_t *source);

ngx_int_t ngx_media_source_preroll_init(ngx_media_source_t *source,
    ngx_uint_t max_units, size_t max_bytes, ngx_log_t *log);
void ngx_media_source_preroll_destroy(ngx_media_source_t *source);

/* NGX_OK when cached, NGX_AGAIN when the ceilings cleared the cache */
ngx_int_t ngx_media_source_preroll_push(ngx_media_source_t *source,
    const ngx_media_frame_t *frame);

void ngx_media_source_preroll_reset(ngx_media_source_t *source);

/* replays the cached frames oldest first */
void ngx_media_source_preroll_replay(ngx_media_source_t *source,
    ngx_media_preroll_replay_pt cb, void *ctx);

ngx_uint_t ngx_media_source_preroll_units(const ngx_media_source_t *source);
size_t ngx_media_source_preroll_bytes(const ngx_media_source_t *source);
ngx_uint_t ngx_media_source_preroll_ready(const ngx_media_source_t *source);

void ngx_media_source_lease_begin(ngx_media_source_t *source);
void ngx_media_source_lease_end(ngx_media_source_t *source);

/*
 * Stores the source's track contract (a copy) so the selector can classify
 * compatibility against the program.  Replaces any previous contract.
 */
ngx_int_t ngx_media_source_tracks_set(ngx_media_source_t *source,
    const ngx_media_trackset_t *tracks, ngx_log_t *log);
void ngx_media_source_tracks_destroy(ngx_media_source_t *source);

#endif /* NGX_MEDIA_SOURCE_H */

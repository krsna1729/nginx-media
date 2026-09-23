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

/*
 * The revision sequence, which is one sequence for every worker.
 *
 * A desired-state mutation takes the next number from the provider the worker
 * registered - the shared owner directory's counter - so two workers'
 * operations on one stream are comparable and every replica resolves a
 * conflict the same way: the higher revision is the newer state, and the
 * lower one is dropped.  A counter per worker cannot do that.  Two workers
 * both call their own next number the same thing, so each replica keeps
 * whichever operation it happened to see last and the replicas disagree.
 *
 * Without a provider - unit builds, or a process with no shared directory -
 * the number stays local, which is the same thing when there is one writer.
 */
typedef uint64_t (*ngx_media_revision_pt)(void);

void ngx_media_revision_provider(ngx_media_revision_pt provider);

/* the next revision after local: the shared one when it is ahead, else local */
uint64_t ngx_media_revision_next(uint64_t local);

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

uint64_t ngx_media_source_preroll_overflows(
    const ngx_media_source_t *source);
uint64_t ngx_media_source_preroll_unit_overflows(
    const ngx_media_source_t *source);
uint64_t ngx_media_source_preroll_byte_overflows(
    const ngx_media_source_t *source);
ngx_uint_t ngx_media_source_preroll_high_water_units(
    const ngx_media_source_t *source);
size_t ngx_media_source_preroll_high_water_bytes(
    const ngx_media_source_t *source);

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

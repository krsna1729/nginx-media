#ifndef NGX_MEDIA_TIMELINE_H
#define NGX_MEDIA_TIMELINE_H

#include "ngx_media.h"

/*
 * Timeline normalization (goal doc 7).
 *
 * Program time must remain monotonic across source switches:
 *
 *   offset = last_program_dts + 1 - first_new_source_dts
 *   program_dts = source_dts + offset
 *
 * and the composition offset must be preserved:
 *
 *   program_pts - program_dts == source_pts - source_dts
 *
 * The first frame of a program maps to 0.  A source clock discontinuity inside
 * one generation (a mapped DTS that would not advance) causes a re-anchor
 * instead of a regression: program time never goes backwards, and the counter
 * records the re-anchor.
 *
 * Timestamps are the raw 33-bit PES values; unwrapping and PCR-based pacing
 * belong to the caller, not here.
 */

void ngx_media_timeline_init(ngx_media_timeline_t *timeline);

/* a source switch or another explicit discontinuity: re-anchor on the next frame */
void ngx_media_timeline_discontinuity(ngx_media_timeline_t *timeline);

ngx_int_t ngx_media_timeline_map(ngx_media_timeline_t *timeline,
    int64_t source_pts, int64_t source_dts, int64_t *program_pts,
    int64_t *program_dts);

#endif /* NGX_MEDIA_TIMELINE_H */

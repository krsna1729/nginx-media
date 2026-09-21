#ifndef NGX_MEDIA_COMPAT_H
#define NGX_MEDIA_COMPAT_H

#include "ngx_media.h"

/*
 * Track contract compatibility (goal doc 9).
 *
 * Before a graceful switch the incoming source's track contract is compared
 * with the current program:
 *
 *   READY         interchangeable: same codecs, same essential parameters
 *   DEGRADED      switchable, but the program changes shape (resolution,
 *                 frame rate policy, audio parameters, an extra track)
 *   INCOMPATIBLE  a program track has no counterpart, or the codec differs
 *
 * Codec configuration bytes are deliberately not part of the comparison:
 * every encoder has its own SPS/PPS, and the program carries explicit config
 * frames across a switch.
 *
 * A deployment may still switch to an incompatible source when it is the only
 * one left; that emergency switch is surfaced to downstream consumers through
 * the generation change and the stream's emergency counter.
 */

#define NGX_MEDIA_COMPAT_READY         1
#define NGX_MEDIA_COMPAT_DEGRADED      2
#define NGX_MEDIA_COMPAT_INCOMPATIBLE  3

ngx_uint_t ngx_media_compat_classify(const ngx_media_trackset_t *program,
    const ngx_media_trackset_t *candidate);
const char *ngx_media_compat_name(ngx_uint_t compat);

#endif /* NGX_MEDIA_COMPAT_H */

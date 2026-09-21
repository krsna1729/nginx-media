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

/*
 * Bounded reverse search, the mirror of ngx_strlchr().
 *
 * nginx has no reverse counterpart, and reaching for strrchr() on an ngx_str_t
 * is a trap: the data is a slice of something larger and is not
 * NUL-terminated, so strrchr() reads past it.  That mistake has been made
 * three times in this codebase - the SRT listener host, an opendir() call, and
 * a request URI - each time failing in a way that looked like something else
 * entirely.  Use this instead.
 */
static ngx_inline u_char *
ngx_media_strrlchr(u_char *line, u_char *last, u_char ch)
{
    while (last > line) {

        if (*(last - 1) == ch) {
            return last - 1;
        }

        last--;
    }

    return NULL;
}

#endif /* NGX_MEDIA_COMPAT_H */

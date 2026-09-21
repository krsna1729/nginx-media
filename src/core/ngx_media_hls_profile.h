#ifndef NGX_MEDIA_HLS_PROFILE_H
#define NGX_MEDIA_HLS_PROFILE_H

#include "ngx_media.h"

/*
 * HLS destination profiles (normative revision).
 *
 * A profile is a named set of platform rules layered on the generic HLS HTTP
 * publisher: validation and defaults, not a special path through the media
 * core.  Platform contracts change, and when they do only the profile moves.
 *
 * The rules are versioned.  A deployment states which revision it is targeting
 * so that a change upstream is a deliberate act rather than a silent one.
 */

typedef struct {
    const char  *name;             /* "youtube_live" */
    ngx_uint_t   revision;         /* the contract revision implemented */

    ngx_uint_t   https_required;   /* transport security, not content crypto */
    ngx_uint_t   min_segment_ms;
    ngx_uint_t   max_segment_ms;
    ngx_uint_t   max_window;       /* outstanding segments in the playlist */
    ngx_uint_t   mpegts_only;
} ngx_media_hls_profile_t;

/* the profile named, or NULL */
const ngx_media_hls_profile_t *ngx_media_hls_profile_find(const ngx_str_t *name);

/*
 * Checks a destination against its profile and applies the profile's
 * defaults.  Returns NGX_OK, or NGX_DECLINED with a reason in *why when the
 * configuration violates the contract.
 *
 * The parameters are in/out: segment_duration_ms and playlist_window are
 * filled from the profile when unset, and validated when set.
 */
ngx_int_t ngx_media_hls_profile_apply(const ngx_media_hls_profile_t *profile,
    const ngx_str_t *url, ngx_uint_t *segment_duration_ms,
    ngx_uint_t *playlist_window, ngx_uint_t *http_post, const char **why);

/*
 * A URL with its credential removed: userinfo and query string are dropped.
 *
 * An HLS endpoint carries its key in the URL - YouTube's ingest does - so the
 * URL is secret material and must not reach logs, metrics, the control API,
 * tracing or a crash dump.  Everything that reports an endpoint goes through
 * this, and the redaction is by construction rather than by remembering.
 */
void ngx_media_redact_url(const ngx_str_t *url, u_char *buf, size_t cap,
    ngx_str_t *out);

#endif /* NGX_MEDIA_HLS_PROFILE_H */

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
    ngx_uint_t   default_segment_ms;
    ngx_uint_t   default_method;   /* NGX_MEDIA_HLS_PUSH_PUT or _POST */
    ngx_uint_t   delete_expired;   /* DELETE segments that left the window */
} ngx_media_hls_profile_t;

#define NGX_MEDIA_HLS_PUSH_PUT   1
#define NGX_MEDIA_HLS_PUSH_POST  2

/* the longest playlist window a destination may ask for */
#define NGX_MEDIA_HLS_PUSH_WINDOW_MAX   32
#define NGX_MEDIA_HLS_PUSH_SEGMENT_MIN  1000
#define NGX_MEDIA_HLS_PUSH_SEGMENT_MAX  30000

/*
 * What an HLS push destination asks of the stream and of its uploader.  The
 * API fills the fields the request names and leaves the rest unset; apply()
 * validates them and fills the defaults, from the profile if there is one.
 */
typedef struct {
    ngx_uint_t   segment_duration_ms;  /* 0: unset; out: the target */
    ngx_uint_t   segment_max_ms;       /* out: the longest it accepts,
                                        * 0 when it states no limit */
    ngx_uint_t   playlist_window;      /* 0: unset; out: 0 is the stream's */
    ngx_uint_t   method;               /* 0: unset */
    ngx_int_t    delete_expired;       /* -1: unset */
} ngx_media_hls_push_settings_t;

/* the profile named, or NULL */
const ngx_media_hls_profile_t *ngx_media_hls_profile_find(const ngx_str_t *name);

/*
 * Checks a destination against its profile - or, with no profile, against
 * the generic publisher's limits - and fills the defaults.  Returns NGX_OK,
 * or NGX_DECLINED with a reason in *why when the configuration violates the
 * contract.
 */
ngx_int_t ngx_media_hls_profile_apply(const ngx_media_hls_profile_t *profile,
    const ngx_str_t *url, ngx_media_hls_push_settings_t *settings,
    const char **why);

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

#ifndef NGX_MEDIA_HLS_PLAYLIST_H
#define NGX_MEDIA_HLS_PLAYLIST_H

#include "ngx_media.h"

/*
 * A destination's view of the stream's media playlist.
 *
 * The segmenter writes one playlist for the stream; each HLS push destination
 * may need a different one - a shorter window than the local origin keeps
 * (YouTube takes at most five outstanding segments), and a gap where one of
 * its own segment uploads failed.  The rewrite is RFC 8216 conformant:
 *
 *   - segments leave from the front only - those past the window, and
 *     those from before the destination's first segment - and
 *     EXT-X-MEDIA-SEQUENCE advances by the number removed (6.2.2), so every
 *     segment keeps its number;
 *   - a removed EXT-X-DISCONTINUITY advances EXT-X-DISCONTINUITY-SEQUENCE,
 *     which is added if the playlist had none;
 *   - a segment the destination does not have is kept in place and marked
 *     EXT-X-GAP (RFC 8216bis 4.4.4.7) rather than removed: removing a
 *     segment from the middle would renumber everything after it.
 *
 * Everything else - header tags, segment tags, the end list - is copied
 * byte for byte.
 */

#define NGX_MEDIA_HLS_PLAYLIST_SEGMENTS_MAX  256

typedef struct {
    ngx_uint_t        window;      /* segments to keep, 0 = all of them */
    const ngx_str_t  *first;       /* segments before this URI are removed
                                    * too, when it is listed: what a
                                    * destination that joined later never
                                    * had */
    const ngx_str_t  *missing;     /* segment URIs to mark EXT-X-GAP */
    ngx_uint_t        nmissing;
} ngx_media_hls_playlist_opts_t;

typedef struct {
    ngx_uint_t  segments;          /* segments in the result */
    ngx_uint_t  removed;           /* segments trimmed from the front */
    ngx_uint_t  gaps;              /* segments marked EXT-X-GAP */
    uint64_t    media_sequence;    /* the result's first media sequence */
    ngx_str_t   uri[NGX_MEDIA_HLS_PLAYLIST_SEGMENTS_MAX];
                                   /* the result's segment URIs, in order,
                                    * pointing into the input */
} ngx_media_hls_playlist_info_t;

/*
 * Rewrites the media playlist in[0..len) into out[0..cap).  NGX_OK with
 * *out_len set; NGX_DECLINED for input that is not a media playlist (a
 * master playlist, or no #EXTM3U); NGX_ERROR when out is too small or the
 * playlist has more than NGX_MEDIA_HLS_PLAYLIST_SEGMENTS_MAX segments.
 * info may be NULL.
 */
ngx_int_t ngx_media_hls_playlist_rewrite(const u_char *in, size_t len,
    const ngx_media_hls_playlist_opts_t *opts, u_char *out, size_t cap,
    size_t *out_len, ngx_media_hls_playlist_info_t *info);

#endif /* NGX_MEDIA_HLS_PLAYLIST_H */

#include "ngx_media_hls_playlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t      start;             /* first line of the segment's tags */
    size_t      uri_start;         /* its URI line */
    size_t      end;               /* past the URI line's newline */
    ngx_uint_t  discontinuity;
    ngx_uint_t  gap;
} ngx_media_hls_playlist_seg_t;

static ngx_uint_t
ngx_media_hls_playlist_tag(const u_char *line, size_t len, const char *tag)
{
    size_t  n = strlen(tag);

    return len >= n && ngx_strncmp(line, tag, n) == 0
           && (len == n || line[n] == ':' || line[n] == '\r');
}

static ngx_uint_t
ngx_media_hls_playlist_header_tag(const u_char *line, size_t len)
{
    static const char  *header[] = {
        "#EXTM3U", "#EXT-X-VERSION", "#EXT-X-TARGETDURATION",
        "#EXT-X-MEDIA-SEQUENCE", "#EXT-X-DISCONTINUITY-SEQUENCE",
        "#EXT-X-PLAYLIST-TYPE", "#EXT-X-INDEPENDENT-SEGMENTS",
        "#EXT-X-START", "#EXT-X-ALLOW-CACHE", "#EXT-X-I-FRAMES-ONLY",
        "#EXT-X-SERVER-CONTROL", "#EXT-X-PART-INF", NULL
    };

    ngx_uint_t  i;

    for (i = 0; header[i] != NULL; i++) {
        if (ngx_media_hls_playlist_tag(line, len, header[i])) {
            return 1;
        }
    }

    return 0;
}

static ngx_uint_t
ngx_media_hls_playlist_is_missing(const ngx_media_hls_playlist_opts_t *opts,
    const u_char *uri, size_t len)
{
    ngx_uint_t  i;

    for (i = 0; opts != NULL && i < opts->nmissing; i++) {
        if (opts->missing[i].len == len
            && ngx_memcmp(opts->missing[i].data, uri, len) == 0)
        {
            return 1;
        }
    }

    return 0;
}

/* appends, failing once the output is full */
static ngx_int_t
ngx_media_hls_playlist_put(u_char *out, size_t cap, size_t *pos,
    const void *data, size_t len)
{
    if (len > cap - *pos) {
        return NGX_ERROR;
    }

    ngx_memcpy(out + *pos, data, len);
    *pos += len;

    return NGX_OK;
}

ngx_int_t
ngx_media_hls_playlist_rewrite(const u_char *in, size_t len,
    const ngx_media_hls_playlist_opts_t *opts, u_char *out, size_t cap,
    size_t *out_len, ngx_media_hls_playlist_info_t *info)
{
    ngx_media_hls_playlist_seg_t  *segs;
    size_t                         pos, next, line_len, header_end;
    size_t                         trailer, o;
    const u_char                  *line, *nl;
    ngx_uint_t                     nsegs = 0, first, i, removed_disc = 0;
    ngx_uint_t                     in_segment = 0;
    ngx_uint_t                     disc = 0, gap = 0;
    uint64_t                       media_seq = 0, disc_seq = 0;
    ngx_int_t                      rc = NGX_ERROR;
    char                           num[64];
    int                            n;

    *out_len = 0;

    if (len < 7 || ngx_strncmp(in, "#EXTM3U", 7) != 0) {
        return NGX_DECLINED;
    }

    segs = calloc(NGX_MEDIA_HLS_PLAYLIST_SEGMENTS_MAX, sizeof(*segs));
    if (segs == NULL) {
        return NGX_ERROR;
    }

    /* pass 1: the header's extent, and every segment's lines */
    header_end = len;
    trailer = len;
    next = 0;

    for (pos = 0; pos < len; pos = next) {
        nl = memchr(in + pos, '\n', len - pos);
        next = (nl != NULL) ? (size_t) (nl - in) + 1 : len;
        line = in + pos;
        line_len = next - pos;
        while (line_len > 0
               && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
        {
            line_len--;
        }

        if (ngx_media_hls_playlist_tag(line, line_len, "#EXT-X-STREAM-INF")) {
            rc = NGX_DECLINED;              /* a master playlist */
            goto done;
        }

        if (!in_segment && header_end == len
            && (line_len == 0 || ngx_media_hls_playlist_header_tag(line,
                                                                  line_len)))
        {
            if (ngx_media_hls_playlist_tag(line, line_len,
                                           "#EXT-X-MEDIA-SEQUENCE"))
            {
                media_seq = strtoull((char *) line + 22, NULL, 10);

            } else if (ngx_media_hls_playlist_tag(line, line_len,
                                              "#EXT-X-DISCONTINUITY-SEQUENCE"))
            {
                disc_seq = strtoull((char *) line + 30, NULL, 10);
            }
            continue;
        }

        if (header_end == len) {
            header_end = pos;
        }

        if (ngx_media_hls_playlist_tag(line, line_len, "#EXT-X-ENDLIST")) {
            trailer = pos;
            break;
        }

        if (line_len == 0) {
            continue;
        }

        if (!in_segment) {
            if (nsegs == NGX_MEDIA_HLS_PLAYLIST_SEGMENTS_MAX) {
                goto done;
            }
            in_segment = 1;
            disc = 0;
            gap = 0;
            segs[nsegs].start = pos;
        }

        if (line[0] == '#') {
            if (ngx_media_hls_playlist_tag(line, line_len,
                                           "#EXT-X-DISCONTINUITY"))
            {
                disc = 1;
            } else if (ngx_media_hls_playlist_tag(line, line_len,
                                                  "#EXT-X-GAP"))
            {
                gap = 1;
            }
            continue;
        }

        /* the URI line ends the segment */
        segs[nsegs].uri_start = pos;
        segs[nsegs].end = next;
        segs[nsegs].discontinuity = disc;
        segs[nsegs].gap = gap;
        nsegs++;
        in_segment = 0;
    }

    if (header_end == len) {
        header_end = (trailer < len) ? trailer : len;
    }

    first = (opts != NULL && opts->window != 0 && nsegs > opts->window)
                ? nsegs - opts->window : 0;

    if (opts != NULL && opts->first != NULL && opts->first->len > 0) {
        for (i = 0; i < nsegs; i++) {
            const u_char  *uri = in + segs[i].uri_start;
            size_t         bare = segs[i].end - segs[i].uri_start;

            while (bare > 0
                   && (uri[bare - 1] == '\n' || uri[bare - 1] == '\r'))
            {
                bare--;
            }

            if (bare == opts->first->len
                && ngx_memcmp(uri, opts->first->data, bare) == 0)
            {
                if (i > first) {
                    first = i;
                }
                break;
            }
        }
    }

    for (i = 0; i < first; i++) {
        removed_disc += segs[i].discontinuity;
    }

    /* pass 2: the header, with the sequences moved past what was removed */
    o = 0;
    next = 0;

    for (pos = 0; pos < header_end; pos = next) {
        nl = memchr(in + pos, '\n', header_end - pos);
        next = (nl != NULL) ? (size_t) (nl - in) + 1 : header_end;
        line = in + pos;
        line_len = next - pos;

        if (first > 0
            && ngx_media_hls_playlist_tag(line, line_len,
                                          "#EXT-X-MEDIA-SEQUENCE"))
        {
            continue;               /* written below with the new number */
        }

        if (removed_disc > 0
            && ngx_media_hls_playlist_tag(line, line_len,
                                          "#EXT-X-DISCONTINUITY-SEQUENCE"))
        {
            continue;
        }

        if (ngx_media_hls_playlist_put(out, cap, &o, line, line_len)
            != NGX_OK)
        {
            goto done;
        }

        if (line_len == 0 || line[line_len - 1] != '\n') {
            if (ngx_media_hls_playlist_put(out, cap, &o, "\n", 1) != NGX_OK) {
                goto done;
            }
        }
    }

    if (first > 0) {
        n = snprintf(num, sizeof(num), "#EXT-X-MEDIA-SEQUENCE:%llu\n",
                     (unsigned long long) (media_seq + first));
        if (ngx_media_hls_playlist_put(out, cap, &o, num, (size_t) n)
            != NGX_OK)
        {
            goto done;
        }
    }

    if (removed_disc > 0) {
        n = snprintf(num, sizeof(num), "#EXT-X-DISCONTINUITY-SEQUENCE:%llu\n",
                     (unsigned long long) (disc_seq + removed_disc));
        if (ngx_media_hls_playlist_put(out, cap, &o, num, (size_t) n)
            != NGX_OK)
        {
            goto done;
        }
    }

    if (info != NULL) {
        info->segments = nsegs - first;
        info->removed = first;
        info->gaps = 0;
        info->media_sequence = media_seq + first;
    }

    /* the segments kept, a missing one marked where its URI is */
    for (i = first; i < nsegs; i++) {
        const u_char  *uri = in + segs[i].uri_start;
        size_t         uri_len = segs[i].end - segs[i].uri_start;
        size_t         bare = uri_len;

        while (bare > 0 && (uri[bare - 1] == '\n' || uri[bare - 1] == '\r')) {
            bare--;
        }

        if (ngx_media_hls_playlist_put(out, cap, &o, in + segs[i].start,
                                       segs[i].uri_start - segs[i].start)
            != NGX_OK)
        {
            goto done;
        }

        if (!segs[i].gap
            && ngx_media_hls_playlist_is_missing(opts, uri, bare))
        {
            if (ngx_media_hls_playlist_put(out, cap, &o, "#EXT-X-GAP\n", 11)
                != NGX_OK)
            {
                goto done;
            }
            if (info != NULL) {
                info->gaps++;
            }
        }

        if (ngx_media_hls_playlist_put(out, cap, &o, uri, bare) != NGX_OK
            || ngx_media_hls_playlist_put(out, cap, &o, "\n", 1) != NGX_OK)
        {
            goto done;
        }

        if (info != NULL) {
            info->uri[i - first].data = (u_char *) uri;
            info->uri[i - first].len = bare;
        }
    }

    if (trailer < len
        && ngx_media_hls_playlist_put(out, cap, &o, in + trailer,
                                      len - trailer) != NGX_OK)
    {
        goto done;
    }

    *out_len = o;
    rc = NGX_OK;

done:

    free(segs);
    return rc;
}

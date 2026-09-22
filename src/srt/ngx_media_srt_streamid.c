#include "ngx_media_srt_streamid.h"

static ngx_int_t ngx_media_srt_name_ok(const u_char *p, size_t len,
    ngx_uint_t allow_slash);

/*
 * Resource, stream, application and source identities are restricted to a
 * conservative character set.  These names end up in configuration lookups,
 * metrics and file names, so control characters, whitespace, separators and
 * percent-escapes are rejected instead of being sanitized later.
 */
static ngx_int_t
ngx_media_srt_name_ok(const u_char *p, size_t len, ngx_uint_t allow_slash)
{
    size_t   i;
    u_char   c;

    if (len == 0 || len > NGX_MEDIA_SRT_STREAMID_NAME_MAX) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        c = p[i];

        if ((c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.')
        {
            continue;
        }

        if (allow_slash && c == '/') {
            continue;
        }

        return 0;
    }

    return 1;
}

void
ngx_media_srt_streamid_init(ngx_media_srt_streamid_t *id)
{
    if (id == NULL) {
        return;
    }

    ngx_memzero(id, sizeof(ngx_media_srt_streamid_t));
}

static int
ngx_media_srt_streamid_hex(u_char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/*
 * Callers do not agree on whether a stream id arrives percent-encoded.
 * ffmpeg 6.1.1 passes whatever follows streamid= verbatim, so the
 * conventional "%23!::r=live/news,m=publish" arrives encoded; later ffmpeg
 * decodes it first and the same string arrives decoded.  Both are accepted
 * here rather than making the caller's version the operator's problem - a
 * stream id that cannot be parsed is a publisher that cannot connect, and
 * which ffmpeg you have is not something a deployment should have to know.
 *
 * Decoding only ever shortens, so a buffer the size of the input is enough.
 */
static size_t
ngx_media_srt_streamid_decode(const u_char *src, size_t len, u_char *dst)
{
    size_t  i, n = 0;
    int     hi, lo;

    for (i = 0; i < len; i++) {

        if (src[i] == '%' && i + 2 < len) {
            hi = ngx_media_srt_streamid_hex(src[i + 1]);
            lo = ngx_media_srt_streamid_hex(src[i + 2]);

            if (hi >= 0 && lo >= 0) {
                dst[n++] = (u_char) ((hi << 4) | lo);
                i += 2;
                continue;
            }
        }

        /* a stray % is left alone rather than corrupting the id */
        dst[n++] = src[i];
    }

    return n;
}

ngx_int_t
ngx_media_srt_streamid_parse(const u_char *data, size_t len,
    ngx_media_srt_streamid_t *id)
{
    const u_char  *p, *end, *pair_end, *eq, *val, *slash;
    size_t         key_len, val_len;

    if (data == NULL || id == NULL || len == 0
        || len > NGX_MEDIA_SRT_STREAMID_MAX)
    {
        return NGX_ERROR;
    }

    /* first, because init zeroes the storage the decode is about to fill */
    ngx_media_srt_streamid_init(id);

    /* decoded into the struct, which owns the bytes from here on */
    len = ngx_media_srt_streamid_decode(data, len, id->storage);
    data = id->storage;

    id->raw.data = (u_char *) data;
    id->raw.len = len;

    p = data;
    end = data + len;

    if (end - p >= 4 && p[0] == '#' && p[1] == '!' && p[2] == ':'
        && p[3] == ':')
    {
        p += 4;
    }

    while (p < end) {

        pair_end = memchr(p, ',', end - p);
        if (pair_end == NULL) {
            pair_end = end;
        }

        if (pair_end == p) {
            /* empty pair */
            return NGX_ERROR;
        }

        eq = memchr(p, '=', pair_end - p);
        if (eq == NULL) {
            /* pair without a value */
            return NGX_ERROR;
        }

        key_len = eq - p;
        val = eq + 1;
        val_len = pair_end - val;

        id->pairs++;

        if (key_len == 1) {

            switch (p[0]) {

            case 'r':
                if (id->resource.len != 0) {
                    return NGX_ERROR;      /* duplicate resource */
                }

                id->resource.data = (u_char *) val;
                id->resource.len = val_len;
                break;

            case 'm':
                if (id->mode.len != 0) {
                    return NGX_ERROR;      /* duplicate mode */
                }

                id->mode.data = (u_char *) val;
                id->mode.len = val_len;
                break;

            case 's':
                if (id->source.len != 0) {
                    return NGX_ERROR;      /* duplicate source */
                }

                id->source.data = (u_char *) val;
                id->source.len = val_len;
                break;

            default:
                /* other SRT keys ('u', 't', 'h', ...) are ignored here */
                break;
            }
        }

        if (pair_end == end) {
            break;
        }

        p = pair_end + 1;

        if (p == end) {
            /* trailing comma: empty final pair */
            return NGX_ERROR;
        }
    }

    if (id->resource.len == 0) {
        return NGX_ERROR;                  /* resource is mandatory */
    }

    if (!ngx_media_srt_name_ok(id->resource.data, id->resource.len, 1)) {
        return NGX_ERROR;
    }

    slash = memchr(id->resource.data, '/', id->resource.len);

    if (slash != NULL) {
        id->application.data = id->resource.data;
        id->application.len = slash - id->resource.data;

        id->stream.data = (u_char *) slash + 1;
        id->stream.len = id->resource.len - id->application.len - 1;

        if (!ngx_media_srt_name_ok(id->application.data, id->application.len,
                                   0)
            || !ngx_media_srt_name_ok(id->stream.data, id->stream.len, 0))
        {
            return NGX_ERROR;
        }

    } else {
        id->stream = id->resource;

        if (!ngx_media_srt_name_ok(id->stream.data, id->stream.len, 0)) {
            return NGX_ERROR;
        }
    }

    if (id->source.len != 0
        && !ngx_media_srt_name_ok(id->source.data, id->source.len, 0))
    {
        return NGX_ERROR;
    }

    if (id->mode.len == 7 && ngx_memcmp(id->mode.data, "publish", 7) == 0) {
        id->mode_kind = NGX_MEDIA_SRT_MODE_PUBLISH;

    } else if (id->mode.len == 7
               && ngx_memcmp(id->mode.data, "request", 7) == 0)
    {
        id->mode_kind = NGX_MEDIA_SRT_MODE_REQUEST;

    } else {
        id->mode_kind = NGX_MEDIA_SRT_MODE_UNKNOWN;
    }

    return NGX_OK;
}

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

    ngx_media_srt_streamid_init(id);

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

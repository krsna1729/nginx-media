#include "ngx_media_nal.h"

/* H.264 and H.265 both delimit NAL units with 3- or 4-byte start codes. */
static const u_char *
ngx_media_nal_start_code(const u_char *p, const u_char *end, size_t *sc_len)
{
    while (p + 3 <= end) {

        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p + 4 <= end && p[3] == 1) {
            *sc_len = 4;
            return p;
        }

        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            *sc_len = 3;
            return p;
        }

        p++;
    }

    return NULL;
}

void
ngx_media_nal_iter_init(ngx_media_nal_iter_t *it, const u_char *data,
    size_t len)
{
    it->pos = data;
    it->end = data + len;
}

ngx_uint_t
ngx_media_nal_iter_next(ngx_media_nal_iter_t *it, ngx_media_nal_t *nal)
{
    const u_char  *sc, *nal_start, *next, *nal_end;
    size_t         sc_len = 0;

    if (it->pos == NULL || it->pos >= it->end) {
        return 0;
    }

    sc = ngx_media_nal_start_code(it->pos, it->end, &sc_len);
    if (sc == NULL) {
        it->pos = it->end;
        return 0;
    }

    nal_start = sc + sc_len;

    next = ngx_media_nal_start_code(nal_start, it->end, &sc_len);

    if (next == NULL) {
        nal_end = it->end;
        it->pos = it->end;

    } else {
        nal_end = next;
        it->pos = next;
    }

    /* trailing zero bytes belong to the next start code */
    while (nal_end > nal_start && nal_end[-1] == 0) {
        nal_end--;
    }

    if (nal_end <= nal_start) {
        /* empty NAL unit: look for the next one */
        return ngx_media_nal_iter_next(it, nal);
    }

    nal->data = (u_char *) nal_start;
    nal->len = nal_end - nal_start;

    return 1;
}

ngx_uint_t
ngx_media_nal_type(ngx_uint_t codec, const u_char *nal, size_t len)
{
    if (nal == NULL || len == 0) {
        return 0;
    }

    if (codec == NGX_MEDIA_CODEC_H265) {
        return (ngx_uint_t) ((nal[0] >> 1) & 0x3F);
    }

    return (ngx_uint_t) (nal[0] & 0x1F);
}

ngx_uint_t
ngx_media_nal_is_vcl(ngx_uint_t codec, ngx_uint_t type)
{
    if (codec == NGX_MEDIA_CODEC_H265) {
        return (type <= 31) ? 1 : 0;
    }

    return (type >= NGX_MEDIA_H264_NAL_SLICE
            && type <= NGX_MEDIA_H264_NAL_IDR) ? 1 : 0;
}

ngx_uint_t
ngx_media_nal_is_config(ngx_uint_t codec, ngx_uint_t type)
{
    if (codec == NGX_MEDIA_CODEC_H265) {
        return (type == NGX_MEDIA_H265_NAL_VPS
                || type == NGX_MEDIA_H265_NAL_SPS
                || type == NGX_MEDIA_H265_NAL_PPS) ? 1 : 0;
    }

    return (type == NGX_MEDIA_H264_NAL_SPS
            || type == NGX_MEDIA_H264_NAL_PPS) ? 1 : 0;
}

ngx_uint_t
ngx_media_nal_is_keyframe(ngx_uint_t codec, ngx_uint_t type)
{
    if (codec == NGX_MEDIA_CODEC_H265) {
        /* BLA_W_LP .. CRA are IRAP pictures: valid resync points */
        return (type >= NGX_MEDIA_H265_NAL_BLA_W_LP
                && type <= NGX_MEDIA_H265_NAL_CRA) ? 1 : 0;
    }

    return (type == NGX_MEDIA_H264_NAL_IDR) ? 1 : 0;
}

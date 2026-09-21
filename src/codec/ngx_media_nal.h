#ifndef NGX_MEDIA_NAL_H
#define NGX_MEDIA_NAL_H

#include "ngx_media.h"

/*
 * Annex B NAL unit iteration and classification for H.264 and H.265.
 *
 * The demuxer works on access-unit granularity (a payload unit start begins
 * one access unit), so this module answers the questions the media core asks
 * about an assembled access unit: which NAL units it contains, whether it
 * carries codec configuration, and whether it is a sync boundary.
 */

/* H.264 NAL unit types */
#define NGX_MEDIA_H264_NAL_SLICE       1
#define NGX_MEDIA_H264_NAL_IDR         5
#define NGX_MEDIA_H264_NAL_SEI         6
#define NGX_MEDIA_H264_NAL_SPS         7
#define NGX_MEDIA_H264_NAL_PPS         8
#define NGX_MEDIA_H264_NAL_AUD         9

/* H.265 NAL unit types */
#define NGX_MEDIA_H265_NAL_BLA_W_LP    16
#define NGX_MEDIA_H265_NAL_IDR_W_RADL  19
#define NGX_MEDIA_H265_NAL_IDR_N_LP    20
#define NGX_MEDIA_H265_NAL_CRA         21
#define NGX_MEDIA_H265_NAL_VPS         32
#define NGX_MEDIA_H265_NAL_SPS         33
#define NGX_MEDIA_H265_NAL_PPS         34
#define NGX_MEDIA_H265_NAL_AUD         35

typedef struct {
    u_char  *data;   /* first byte of the NAL unit, header included */
    size_t   len;
} ngx_media_nal_t;

typedef struct {
    const u_char  *pos;
    const u_char  *end;
} ngx_media_nal_iter_t;

void ngx_media_nal_iter_init(ngx_media_nal_iter_t *it, const u_char *data,
    size_t len);

/* returns 1 while NAL units remain, 0 at the end of the buffer */
ngx_uint_t ngx_media_nal_iter_next(ngx_media_nal_iter_t *it,
    ngx_media_nal_t *nal);

ngx_uint_t ngx_media_nal_type(ngx_uint_t codec, const u_char *nal, size_t len);
ngx_uint_t ngx_media_nal_is_vcl(ngx_uint_t codec, ngx_uint_t type);
ngx_uint_t ngx_media_nal_is_config(ngx_uint_t codec, ngx_uint_t type);
ngx_uint_t ngx_media_nal_is_keyframe(ngx_uint_t codec, ngx_uint_t type);

#endif /* NGX_MEDIA_NAL_H */

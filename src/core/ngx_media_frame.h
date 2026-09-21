#ifndef NGX_MEDIA_FRAME_H
#define NGX_MEDIA_FRAME_H

#include "ngx_media_platform.h"
#include "ngx_media_buffer.h"

/*
 * Compact encoded-media frame (goal doc 4.2).
 *
 * A frame owns one reference to its payload buffer.  Descriptors are cheap to
 * copy; payload bytes are never copied on a steady-state path.  Be explicit
 * about the representation in payload_format: "normalized" means common
 * ownership, timestamps and metadata, not that every codec is converted to a
 * single byte framing.
 *
 * Frames must be initialised with ngx_media_frame_init() (or zeroed) before
 * any other operation.
 */
typedef struct {
    ngx_uint_t       media_type;      /* video / audio / data */
    ngx_uint_t       codec;
    ngx_uint_t       payload_format;  /* explicit representation */
    ngx_uint_t       track_index;
    int64_t          pts;
    int64_t          dts;
    unsigned         keyframe:1;
    unsigned         config:1;
    ngx_media_buf_t *payload;
} ngx_media_frame_t;

void ngx_media_frame_init(ngx_media_frame_t *frame);
void ngx_media_frame_adopt(ngx_media_frame_t *frame, ngx_media_buf_t *payload);
ngx_int_t ngx_media_frame_copy(ngx_media_frame_t *dst,
    const ngx_media_frame_t *src);
void ngx_media_frame_release(ngx_media_frame_t *frame);

#endif /* NGX_MEDIA_FRAME_H */

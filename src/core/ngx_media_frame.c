#include "ngx_media_frame.h"

void
ngx_media_frame_init(ngx_media_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    ngx_memzero(frame, sizeof(ngx_media_frame_t));
}

void
ngx_media_frame_adopt(ngx_media_frame_t *frame, ngx_media_buf_t *payload)
{
    if (frame == NULL) {
        return;
    }

    ngx_media_buf_unref(frame->payload);

    frame->payload = payload;
}

ngx_int_t
ngx_media_frame_copy(ngx_media_frame_t *dst, const ngx_media_frame_t *src)
{
    if (dst == NULL || src == NULL) {
        return NGX_ERROR;
    }

    *dst = *src;
    ngx_media_buf_ref(dst->payload);

    return NGX_OK;
}

void
ngx_media_frame_release(ngx_media_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    ngx_media_buf_unref(frame->payload);

    ngx_memzero(frame, sizeof(ngx_media_frame_t));
}

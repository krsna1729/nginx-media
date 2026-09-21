#include "ngx_media_buffer.h"

ngx_media_buf_t *
ngx_media_buf_alloc(size_t capacity)
{
    ngx_media_buf_t  *buf;

    if (capacity > (size_t) -1 - sizeof(ngx_media_buf_t)) {
        return NULL;
    }

    buf = ngx_alloc(sizeof(ngx_media_buf_t) + capacity, NULL);
    if (buf == NULL) {
        return NULL;
    }

    buf->refs = 1;
    buf->len = 0;
    buf->capacity = capacity;
    buf->data = (u_char *) (buf + 1);

    return buf;
}

ngx_int_t
ngx_media_buf_freeze(ngx_media_buf_t *buf, size_t len)
{
    if (buf == NULL || len > buf->capacity) {
        return NGX_ERROR;
    }

    buf->len = len;

    return NGX_OK;
}

ngx_media_buf_t *
ngx_media_buf_ref(ngx_media_buf_t *buf)
{
    if (buf == NULL) {
        return NULL;
    }

    (void) ngx_atomic_fetch_add(&buf->refs, 1);

    return buf;
}

void
ngx_media_buf_unref(ngx_media_buf_t *buf)
{
    if (buf == NULL) {
        return;
    }

    if (ngx_atomic_fetch_add(&buf->refs, -1) == 1) {
        ngx_free(buf);
    }
}

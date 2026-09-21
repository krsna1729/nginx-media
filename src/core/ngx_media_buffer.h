#ifndef NGX_MEDIA_BUFFER_H
#define NGX_MEDIA_BUFFER_H

#include "ngx_media_platform.h"

typedef struct ngx_media_buf_s ngx_media_buf_t;

/*
 * Immutable, reference-counted payload buffer (goal doc 4.1).
 *
 * A buffer is allocated with ngx_media_buf_alloc(), written by its owner and
 * then frozen with ngx_media_buf_freeze().  After freezing the payload is
 * shared by reference across demux, program feed, HLS, RTMP and SRT paths and
 * must not be mutated.  The data area lives in the same allocation as the
 * header so that no steady-state path copies payload bytes.
 */
struct ngx_media_buf_s {
    ngx_atomic_t   refs;
    size_t         len;
    size_t         capacity;
    u_char        *data;
};

ngx_media_buf_t *ngx_media_buf_alloc(size_t capacity);
ngx_int_t ngx_media_buf_freeze(ngx_media_buf_t *buf, size_t len);
ngx_media_buf_t *ngx_media_buf_ref(ngx_media_buf_t *buf);
void ngx_media_buf_unref(ngx_media_buf_t *buf);

#define ngx_media_buf_data(buf)      ((buf)->data)
#define ngx_media_buf_size(buf)      ((buf)->len)
#define ngx_media_buf_capacity(buf)  ((buf)->capacity)
#define ngx_media_buf_refs(buf)      ((ngx_uint_t) (buf)->refs)

#endif /* NGX_MEDIA_BUFFER_H */

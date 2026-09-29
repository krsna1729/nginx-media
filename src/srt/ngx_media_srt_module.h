#ifndef NGX_MEDIA_SRT_MODULE_H
#define NGX_MEDIA_SRT_MODULE_H

#include "ngx_media_stream.h"

/* Sessions that still hold a stream pool reference until their CLOSE event. */
ngx_uint_t ngx_media_srt_stream_readers(const ngx_media_stream_t *stream);

#endif /* NGX_MEDIA_SRT_MODULE_H */

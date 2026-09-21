#ifndef NGX_MEDIA_SRT_STREAMID_H
#define NGX_MEDIA_SRT_STREAMID_H

#include "ngx_media_platform.h"

/*
 * SRT Stream ID parsing (goal doc 11.5).
 *
 * Canonical initial form:
 *
 *   #!::r=live/news,m=publish,s=encoder-a
 *
 * The parser extracts the resource, the access-control mode and the source
 * identity.  The source identity is informational: source priority must come
 * from trusted configuration, never from this field.
 *
 * All ngx_str_t members point into the caller's buffer; no copy is made and
 * the input must outlive the parsed structure.
 */

#define NGX_MEDIA_SRT_STREAMID_MAX      512
#define NGX_MEDIA_SRT_STREAMID_NAME_MAX 128

#define NGX_MEDIA_SRT_MODE_UNKNOWN      0
#define NGX_MEDIA_SRT_MODE_PUBLISH      1
#define NGX_MEDIA_SRT_MODE_REQUEST      2

typedef struct {
    ngx_str_t   raw;          /* whole stream id, without a copy */
    ngx_str_t   resource;     /* r= */
    ngx_str_t   mode;         /* m= */
    ngx_str_t   source;       /* s= */
    ngx_str_t   application;  /* resource before the first '/' */
    ngx_str_t   stream;       /* resource after the first '/' (or all of it) */
    ngx_uint_t  mode_kind;    /* NGX_MEDIA_SRT_MODE_* */
    ngx_uint_t  pairs;        /* number of key=value pairs seen */
} ngx_media_srt_streamid_t;

void ngx_media_srt_streamid_init(ngx_media_srt_streamid_t *id);
ngx_int_t ngx_media_srt_streamid_parse(const u_char *data, size_t len,
    ngx_media_srt_streamid_t *id);

#endif /* NGX_MEDIA_SRT_STREAMID_H */

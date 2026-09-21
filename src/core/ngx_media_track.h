#ifndef NGX_MEDIA_TRACK_H
#define NGX_MEDIA_TRACK_H

#include "ngx_media_platform.h"
#include "ngx_media_buffer.h"

/*
 * Track contract of a source or of the program (goal doc 4.4, 9).
 *
 * The codec private config (avcC/hvcC/AudioSpecificConfig) is a reference
 * counted payload buffer like any other.  Compatibility classification for
 * graceful switches compares track contracts; this type carries the fields
 * that comparison needs.
 */
typedef struct {
    ngx_uint_t       media_type;
    ngx_uint_t       codec;
    ngx_uint_t       payload_format;
    ngx_uint_t       profile;
    ngx_uint_t       level;
    ngx_uint_t       width;
    ngx_uint_t       height;
    ngx_uint_t       frame_rate_num;
    ngx_uint_t       frame_rate_den;
    ngx_uint_t       sample_rate;
    ngx_uint_t       channels;
    ngx_media_buf_t *config;
} ngx_media_track_t;

typedef struct {
    ngx_media_track_t  *tracks;
    ngx_uint_t          count;
    ngx_uint_t          capacity;
} ngx_media_trackset_t;

ngx_int_t ngx_media_trackset_init(ngx_media_trackset_t *set,
    ngx_uint_t capacity, ngx_log_t *log);
void ngx_media_trackset_destroy(ngx_media_trackset_t *set);

/* returns the index of the added track, or a negative value on error */
ngx_int_t ngx_media_trackset_add(ngx_media_trackset_t *set,
    const ngx_media_track_t *track);
const ngx_media_track_t *ngx_media_trackset_get(
    const ngx_media_trackset_t *set, ngx_uint_t index);

#endif /* NGX_MEDIA_TRACK_H */

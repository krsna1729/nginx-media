#ifndef NGX_MEDIA_H
#define NGX_MEDIA_H

#include "ngx_media_platform.h"
#include "ngx_media_buffer.h"
#include "ngx_media_frame.h"
#include "ngx_media_feed.h"
#include "ngx_media_track.h"

/* media types */
#define NGX_MEDIA_TYPE_VIDEO   1
#define NGX_MEDIA_TYPE_AUDIO   2
#define NGX_MEDIA_TYPE_DATA    3

/* codecs */
#define NGX_MEDIA_CODEC_NONE   0
#define NGX_MEDIA_CODEC_H264   1
#define NGX_MEDIA_CODEC_H265   2
#define NGX_MEDIA_CODEC_AAC    3

/* explicit encoded payload representations */
#define NGX_MEDIA_PAYLOAD_ANNEXB  1
#define NGX_MEDIA_PAYLOAD_AVCC    2
#define NGX_MEDIA_PAYLOAD_ADTS    3
#define NGX_MEDIA_PAYLOAD_RAW     4

/* source types (goal doc 4.4 and normative revision) */
#define NGX_MEDIA_SOURCE_SRT       1
#define NGX_MEDIA_SOURCE_RTMP      2
#define NGX_MEDIA_SOURCE_FILE      3
#define NGX_MEDIA_SOURCE_HLS_PULL  4
#define NGX_MEDIA_SOURCE_HLS_PUSH  5

/* source activation gate states (goal doc 5) */
#define NGX_MEDIA_SOURCE_STANDBY        1
#define NGX_MEDIA_SOURCE_AWAITING_SYNC  2
#define NGX_MEDIA_SOURCE_ACTIVE         3
#define NGX_MEDIA_SOURCE_DRAINING       4

/* switchback policy (goal doc 8) */
#define NGX_MEDIA_SWITCHBACK_AUTO    1
#define NGX_MEDIA_SWITCHBACK_MANUAL  2
#define NGX_MEDIA_SWITCHBACK_NEVER   3

typedef struct ngx_media_application_s ngx_media_application_t;
typedef struct ngx_media_stream_s ngx_media_stream_t;
typedef struct ngx_media_source_s ngx_media_source_t;
typedef struct ngx_media_source_ops_s ngx_media_source_ops_t;

/*
 * Timeline object (goal doc 7).  Program time must remain monotonic across
 * source switches:
 *
 *   offset = last_program_dts + 1 - first_new_source_dts
 *   program_dts = source_dts + offset
 *
 * and program_pts - program_dts == source_pts - source_dts must be preserved.
 * The timeline belongs to the logical stream, never to an input protocol.
 */
typedef struct {
    int64_t   offset;
    int64_t   last_program_dts;
    int64_t   last_program_pts;
    uint64_t  switches;
} ngx_media_timeline_t;

/* selection policy (goal doc 8) */
typedef struct {
    ngx_msec_t  failure_timeout;
    ngx_msec_t  recovery_timeout;
    ngx_uint_t  switch_keyframe;
    ngx_uint_t  switchback;        /* NGX_MEDIA_SWITCHBACK_* */
} ngx_media_selector_t;

/* logical stream (goal doc 4.3) */
struct ngx_media_stream_s {
    ngx_pool_t             *pool;
    ngx_str_t               application;
    ngx_str_t               name;
    ngx_queue_t             sources;
    ngx_media_source_t     *active;
    ngx_queue_t             consumers;
    ngx_media_timeline_t    timeline;
    ngx_media_selector_t    selector;
    ngx_media_feed_t        program_feed;
    ngx_uint_t              generation;
    unsigned                running:1;
};

/* source (goal doc 4.4).  Priority comes from trusted configuration, never
 * from an encoder-supplied Stream ID field. */
struct ngx_media_source_s {
    ngx_media_stream_t     *stream;
    ngx_str_t               id;
    ngx_uint_t              type;      /* NGX_MEDIA_SOURCE_* */
    ngx_uint_t              priority;
    ngx_uint_t              state;     /* activation gate state */
    ngx_msec_t              last_media;
    ngx_msec_t              healthy_since;
    ngx_media_trackset_t   *tracks;
    ngx_media_source_ops_t *ops;
    void                   *input_ctx;
    unsigned                healthy:1;
    unsigned                eligible:1;
    unsigned                active:1;
};

/* application: runtime graph root (normative revision) */
struct ngx_media_application_s {
    ngx_pool_t         *pool;
    ngx_str_t           name;
    ngx_queue_t         streams;
    unsigned            dynamic_streams:1;
};

#endif /* NGX_MEDIA_H */

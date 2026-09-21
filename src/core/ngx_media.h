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

/* selection policy (goal doc 8) */
typedef struct {
    ngx_msec_t  failure_timeout;
    ngx_msec_t  recovery_timeout;
    ngx_uint_t  switch_keyframe;
    ngx_uint_t  switchback;        /* NGX_MEDIA_SWITCHBACK_* */
} ngx_media_selector_t;

/* timeline (goal doc 7): program time must stay monotonic across switches */
typedef struct {
    int64_t   offset;            /* program_dts = source_dts + offset */
    int64_t   last_program_dts;
    int64_t   last_program_pts;
    uint64_t  switches;
    uint64_t  resyncs;           /* re-anchors caused by source discontinuities */
    unsigned  anchored:1;
    unsigned  has_program:1;
} ngx_media_timeline_t;

/*
 * Complete-GOP standby cache (goal doc 6).  It starts at a video keyframe, is
 * replaced wholesale by a newer keyframe, and is cleared (never kept partial)
 * when a hard ceiling is reached.
 */
typedef struct {
    ngx_media_frame_t  *units;
    ngx_uint_t          capacity;    /* power of two */
    ngx_uint_t          max_units;
    size_t              max_bytes;
    uint64_t            head;
    uint64_t            tail;
    size_t              bytes;
    uint64_t            overflows;   /* resets caused by the ceilings */
    unsigned            have_boundary:1;
} ngx_media_preroll_t;

/*
 * Source health (goal doc 8).  Layered evidence, never a blended score; the
 * behaviour lives in ngx_media_health.h.
 */
typedef struct {
    ngx_uint_t   evidence;             /* bits currently satisfied */
    ngx_uint_t   required;             /* bits the policy requires */
    ngx_msec_t   failure_timeout;
    ngx_msec_t   recovery_timeout;
    ngx_msec_t   last_media;
    ngx_msec_t   last_ts_progress;
    ngx_msec_t   last_container_error;
    ngx_msec_t   unhealthy_since;
    ngx_msec_t   healthy_since;
    int64_t      last_dts;
    uint64_t     container_errors;     /* cumulative, from the demuxer */
    uint64_t     transitions;
    unsigned     healthy:1;
    unsigned     eligible:1;
} ngx_media_health_t;

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

    /*
     * Object revision (normative revision): bumped by every mutation of the
     * desired state, so a controller can send the revision it last saw and
     * have a stale write rejected instead of silently overwriting a newer
     * one.  Distinct from generation, which counts media switches.
     */
    uint64_t                revision;

    uint64_t                switches;
    uint64_t                emergency_switches;
    uint64_t                program_frames;
    unsigned                running:1;
};

/* source (goal doc 4.4).  Priority comes from trusted configuration, never
 * from an encoder-supplied Stream ID field. */
struct ngx_media_source_s {
    ngx_media_stream_t     *stream;
    ngx_str_t               id;
    uint64_t                revision;

    /*
     * Desired state, as opposed to the observed state below: an operator can
     * disable a source without deleting it, and the selector then ignores it
     * however healthy it looks.  Sources are created enabled.
     */
    unsigned                enabled:1;

    ngx_uint_t              type;      /* NGX_MEDIA_SOURCE_* */
    ngx_uint_t              priority;
    ngx_uint_t              state;     /* activation gate state */
    ngx_msec_t              last_media;
    ngx_msec_t              healthy_since;
    ngx_media_trackset_t   *tracks;
    ngx_media_source_ops_t *ops;
    void                   *input_ctx;
    ngx_queue_t             queue;     /* link in the stream's source queue */
    ngx_media_preroll_t     preroll;
    ngx_media_health_t      health;
    ngx_uint_t              compat;    /* vs the program track contract */
    ngx_uint_t              writers;   /* outstanding write leases */
    uint64_t                frames_in;
    uint64_t                frames_out;
    unsigned                healthy:1;
    unsigned                eligible:1;
    unsigned                active:1;
    unsigned                has_video:1;
    unsigned                pending_switch:1;
    unsigned                pending_remove:1;
};

/* application: runtime graph root (normative revision) */
struct ngx_media_application_s {
    ngx_pool_t         *pool;
    ngx_str_t           name;
    ngx_queue_t         streams;
    unsigned            dynamic_streams:1;
};

#endif /* NGX_MEDIA_H */

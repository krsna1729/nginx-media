#ifndef NGX_MEDIA_HLS_SEGMENTER_H
#define NGX_MEDIA_HLS_SEGMENTER_H

#include "ngx_media.h"
#include "ngx_media_ts_mux.h"

/*
 * MPEG-TS HLS segmenter (goal doc 19).
 *
 * Rules:
 *   - a segment begins only at a video sync boundary
 *   - a normal cut happens on a keyframe once the segment is at least
 *     min_duration long
 *   - a segment is never longer than max_duration or larger than
 *     max_segment_bytes
 *   - retained segments (count and bytes) are bounded and evicted files are
 *     removed from disk
 *   - a program generation change closes the current segment and puts
 *     #EXT-X-DISCONTINUITY before the next one
 *
 * The segmenter never copies media: a segment is a list of byte ranges that
 * reference the shared burst buffers handed to it by the driver.
 */

#define NGX_MEDIA_HLS_MAX_PIECES 4096

typedef struct {
    ngx_str_t   path;                /* directory that receives the files */
    ngx_str_t   playlist_name;       /* e.g. index.m3u8 */
    ngx_str_t   segment_prefix;      /* e.g. seg- */
    ngx_msec_t  target_duration;
    ngx_msec_t  min_duration;
    ngx_msec_t  max_duration;
    size_t      max_segment_bytes;
    ngx_uint_t  max_segments;
    size_t      max_retained_bytes;
} ngx_media_hls_conf_t;

typedef struct {
    ngx_media_buf_t  *buf;
    size_t            offset;
    size_t            len;
} ngx_media_hls_piece_t;

typedef struct {
    uint64_t    sequence;
    ngx_str_t   name;
    ngx_msec_t  duration;
    size_t      bytes;
    unsigned    discontinuity:1;
} ngx_media_hls_segment_t;

typedef struct {
    ngx_media_hls_conf_t  conf;

    ngx_media_hls_piece_t  pieces[NGX_MEDIA_HLS_MAX_PIECES];
    ngx_uint_t             npieces;
    size_t                 bytes;
    int64_t                first_dts;
    int64_t                last_dts;
    unsigned               started:1;      /* a sync boundary was seen */
    unsigned               discontinuity_next:1;

    ngx_media_hls_segment_t *segments;     /* bounded playlist window */
    ngx_uint_t               nsegments;
    ngx_uint_t               segments_capacity;
    uint64_t                 next_sequence;
    uint64_t                 discontinuity_sequence;
    size_t                   retained_bytes;

    uint64_t   segments_written;
    uint64_t   segments_evicted;
    uint64_t   forced_cuts;
    uint64_t   dropped_frames;
    uint64_t   bytes_written;
    uint64_t   errors;

    /*
     * Where a failure is reported.  The segmenter used to take a log and
     * discard it, so a write that failed - a full disk, a directory the
     * worker cannot write into - was counted in `errors` and dropped in
     * silence.  From outside, the output simply stopped appearing.
     */
    ngx_log_t *log;
} ngx_media_hls_t;

void ngx_media_hls_conf_default(ngx_media_hls_conf_t *conf);
ngx_int_t ngx_media_hls_init(ngx_media_hls_t *hls,
    const ngx_media_hls_conf_t *conf, ngx_log_t *log);
void ngx_media_hls_destroy(ngx_media_hls_t *hls);

/* one muxed burst with its frame slices */
ngx_int_t ngx_media_hls_add_burst(ngx_media_hls_t *hls,
    const ngx_media_ts_burst_t *burst);

/* the program timeline changed: close the segment, mark the next one */
ngx_int_t ngx_media_hls_discontinuity(ngx_media_hls_t *hls);

/* close the current segment (stream end or reload) */
ngx_int_t ngx_media_hls_finish(ngx_media_hls_t *hls);

ngx_uint_t ngx_media_hls_segments(const ngx_media_hls_t *hls);
ngx_msec_t ngx_media_hls_pending_duration(const ngx_media_hls_t *hls);

#endif /* NGX_MEDIA_HLS_SEGMENTER_H */

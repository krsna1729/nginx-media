#ifndef NGX_MEDIA_TS_MUX_H
#define NGX_MEDIA_TS_MUX_H

#include "ngx_media.h"
#include "ngx_media_ts_crc.h"
#include "ngx_media_ts_demux.h"

/*
 * MPEG-TS muxer with batched burst preparation (goal doc 10).
 *
 *   frames -> mux burst into one backing allocation -> freeze/refcount once
 *          -> publish slices -> many consumers
 *
 * A burst owns exactly one payload buffer: every transport packet of the
 * batch is written into it, and consumers (HLS segmenter, recording, SRT
 * output, ...) share that buffer by reference and address parts of it with
 * slices.  Nothing in the fanout path copies media.
 *
 * PSI (PAT/PMT) is written at the start of every burst so any consumer that
 * begins reading at a burst boundary can decode it.
 */

#define NGX_MEDIA_TS_MUX_MAX_SLICES 256

#define NGX_MEDIA_TS_MUX_PID_PAT    0x0000
#define NGX_MEDIA_TS_MUX_PID_PMT    0x1000
#define NGX_MEDIA_TS_MUX_PID_PCR    0x1001
#define NGX_MEDIA_TS_MUX_PID_VIDEO  0x1011
#define NGX_MEDIA_TS_MUX_PID_AUDIO  0x1100

typedef struct {
    size_t      offset;    /* byte range inside the burst backing buffer */
    size_t      len;
    int64_t     pts;
    int64_t     dts;
    ngx_uint_t  media_type;
    ngx_uint_t  codec;
    unsigned    keyframe:1;
} ngx_media_ts_slice_t;

typedef struct {
    ngx_media_buf_t       *backing;   /* one allocation for the whole burst */
    u_char                *cursor;
    size_t                 capacity;
    ngx_media_ts_slice_t   slices[NGX_MEDIA_TS_MUX_MAX_SLICES];
    ngx_uint_t             nslices;
} ngx_media_ts_burst_t;

typedef struct {
    ngx_uint_t  pmt_pid;
    ngx_uint_t  pcr_pid;
    ngx_uint_t  video_pid;
    ngx_uint_t  audio_pid;
    size_t      default_burst_bytes;
    size_t      max_burst_bytes;     /* hard ceiling for one burst */
} ngx_media_ts_mux_conf_t;

typedef struct {
    ngx_media_ts_mux_conf_t  conf;

    ngx_uint_t   cc_pat;
    ngx_uint_t   cc_pmt;
    ngx_uint_t   cc_video;
    ngx_uint_t   cc_audio;
    ngx_uint_t   cc_pcr;

    ngx_uint_t   video_stream_type;
    ngx_uint_t   audio_stream_type;
    unsigned     tracks_ready:1;
    unsigned     psi_ready:1;

    uint64_t     bursts;
    uint64_t     packets;
    uint64_t     bytes;
    uint64_t     psi_packets;
    uint64_t     pcr_packets;
    uint64_t     frame_bytes;
    uint64_t     frame_drops;        /* frames that did not fit a burst */
} ngx_media_ts_mux_t;

void ngx_media_ts_mux_conf_default(ngx_media_ts_mux_conf_t *conf);
ngx_int_t ngx_media_ts_mux_init(ngx_media_ts_mux_t *mux,
    const ngx_media_ts_mux_conf_t *conf, ngx_log_t *log);
void ngx_media_ts_mux_destroy(ngx_media_ts_mux_t *mux);

/* track contract: decides the PMT stream types and PID layout */
ngx_int_t ngx_media_ts_mux_set_tracks(ngx_media_ts_mux_t *mux,
    const ngx_media_trackset_t *tracks);

ngx_int_t ngx_media_ts_mux_burst_init(ngx_media_ts_mux_t *mux,
    ngx_media_ts_burst_t *burst, size_t capacity);
void ngx_media_ts_mux_burst_destroy(ngx_media_ts_burst_t *burst);

/* copies the frame's encoded payload into the burst as one PES */
ngx_int_t ngx_media_ts_mux_write_frame(ngx_media_ts_mux_t *mux,
    ngx_media_ts_burst_t *burst, const ngx_media_frame_t *frame,
    unsigned pcr);

/* freezes the backing buffer; the caller shares it by reference */
ngx_int_t ngx_media_ts_mux_burst_end(ngx_media_ts_mux_t *mux,
    ngx_media_ts_burst_t *burst);

size_t ngx_media_ts_burst_size(const ngx_media_ts_burst_t *burst);

#endif /* NGX_MEDIA_TS_MUX_H */

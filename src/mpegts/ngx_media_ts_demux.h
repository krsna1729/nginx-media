#ifndef NGX_MEDIA_TS_DEMUX_H
#define NGX_MEDIA_TS_DEMUX_H

#include "ngx_media.h"
#include "ngx_media_nal.h"
#include "ngx_media_aac.h"

/*
 * In-process MPEG-TS demultiplexer (goal doc 10).
 *
 *   TS bytes -> packets -> PSI (PAT/PMT) -> PES -> access units / ADTS frames
 *           -> ngx_media_frame_t (encoded, never decoded)
 *
 * No decoding happens here: video stays Annex B (H.264/H.265) and audio stays
 * ADTS (AAC).  Timestamps are passed through as the raw 33-bit PES values;
 * unwrapping and program-time mapping belong to the stream timeline.
 *
 * Output is callback based so the same demuxer feeds tests, the SRT worker
 * ingest path and, later, file/HLS sources.
 */

#define NGX_MEDIA_TS_PACKET_SIZE    188
#define NGX_MEDIA_TS_SYNC_BYTE      0x47
#define NGX_MEDIA_TS_PID_PAT        0x0000
#define NGX_MEDIA_TS_PID_NULL       0x1FFF
#define NGX_MEDIA_TS_SECTION_MAX    1024

/* ITU-T H.222.0 stream types */
#define NGX_MEDIA_TS_STREAM_MPEG2_VIDEO 0x02
#define NGX_MEDIA_TS_STREAM_MPEG1_AUDIO 0x03
#define NGX_MEDIA_TS_STREAM_MPEG2_AUDIO 0x04
#define NGX_MEDIA_TS_STREAM_AAC_ADTS    0x0F
#define NGX_MEDIA_TS_STREAM_H264        0x1B
#define NGX_MEDIA_TS_STREAM_H265        0x24

#define NGX_MEDIA_TS_DEFAULT_TRACKS     8
#define NGX_MEDIA_TS_DEFAULT_AU_BYTES   (4 * 1024 * 1024)

typedef struct {
    ngx_uint_t  max_tracks;
    size_t      max_au_bytes;   /* hard ceiling for one access-unit assembly */
} ngx_media_ts_demux_conf_t;

typedef struct {
    uint64_t    bytes;
    uint64_t    packets;
    uint64_t    frames_out;
    uint64_t    config_frames_out;
    uint64_t    sync_errors;
    uint64_t    transport_errors;
    uint64_t    continuity_errors;
    uint64_t    psi_errors;
    uint64_t    crc_errors;
    uint64_t    pes_errors;
    uint64_t    au_overflows;
    uint64_t    unsupported_streams;
    uint64_t    skipped_bytes;
    int64_t     last_pcr;       /* 90 kHz PCR base of the last PCR seen */
    ngx_uint_t  has_pcr;
} ngx_media_ts_demux_stats_t;

typedef struct {
    void (*tracks)(void *ctx, const ngx_media_trackset_t *tracks);
    void (*frame)(void *ctx, const ngx_media_frame_t *frame);
} ngx_media_ts_sink_t;

typedef struct {
    ngx_uint_t    pid;
    ngx_uint_t    stream_type;
    ngx_uint_t    codec;
    ngx_uint_t    track_index;

    ngx_uint_t    cc;
    ngx_uint_t    cc_valid;

    u_char       *au;          /* access-unit assembly buffer */
    size_t        au_len;
    size_t        au_cap;
    ngx_uint_t    au_overflow;

    int64_t       pts;
    int64_t       dts;
    ngx_uint_t    has_pts;
    ngx_uint_t    has_dts;
    int64_t       last_pts;    /* audio pacing when a PES lacks a timestamp */

    ngx_uint_t pes_remaining;  /* payload bytes left in a bounded PES */

    ngx_media_buf_t *config;   /* last emitted codec configuration payload */
    size_t           config_len;

    ngx_media_track_t track;   /* descriptor published to the sink */
} ngx_media_ts_track_t;

typedef struct {
    ngx_uint_t    pid;
    u_char        buf[NGX_MEDIA_TS_SECTION_MAX];
    size_t        len;
    size_t        expected;
    ngx_uint_t    cc;
    ngx_uint_t    cc_valid;
    ngx_uint_t    assembling;
} ngx_media_ts_psi_t;

typedef struct {
    ngx_uint_t                 max_tracks;
    size_t                     max_au_bytes;

    ngx_media_ts_track_t      *tracks;
    ngx_uint_t                 ntracks;

    ngx_media_trackset_t       trackset;
    ngx_uint_t                 tracks_ready;

    ngx_media_ts_sink_t        sink;
    void                      *sink_ctx;

    ngx_media_ts_psi_t         pat;
    ngx_media_ts_psi_t         pmt;

    ngx_uint_t                 pmt_pid;
    ngx_uint_t                 pat_valid;
    ngx_uint_t                 pmt_valid;
    ngx_uint_t                 pcr_pid;

    u_char                     tail[NGX_MEDIA_TS_PACKET_SIZE];
    size_t                     tail_len;

    ngx_media_ts_demux_stats_t stats;
} ngx_media_ts_demux_t;

ngx_int_t ngx_media_ts_demux_init(ngx_media_ts_demux_t *demux,
    const ngx_media_ts_demux_conf_t *conf, const ngx_media_ts_sink_t *sink,
    void *sink_ctx, ngx_log_t *log);
void ngx_media_ts_demux_destroy(ngx_media_ts_demux_t *demux);

/* feed arbitrary byte runs; partial packets are carried across calls */
ngx_int_t ngx_media_ts_demux_feed(ngx_media_ts_demux_t *demux,
    const u_char *data, size_t len);

/* end of stream: flush a partially assembled access unit */
void ngx_media_ts_demux_flush(ngx_media_ts_demux_t *demux);

void ngx_media_ts_demux_stats(const ngx_media_ts_demux_t *demux,
    ngx_media_ts_demux_stats_t *out);

#endif /* NGX_MEDIA_TS_DEMUX_H */

#include "ngx_media_test.h"
#include "ngx_media_ts_demux.h"

#define PMT_PID   0x1000
#define VIDEO_PID 0x1011
#define AUDIO_PID 0x1100

typedef struct {
    u_char      buf[188 * 256];
    size_t      len;
} ts_writer_t;

typedef struct {
    ngx_media_frame_t  frames[64];
    ngx_uint_t         nframes;
    ngx_media_track_t  tracks[8];
    ngx_uint_t         ntracks;
    ngx_uint_t         track_notifications;
} sink_t;

static void
sink_frame(void *ctx, const ngx_media_frame_t *frame)
{
    sink_t  *s = ctx;

    if (s->nframes < 64) {
        (void) ngx_media_frame_copy(&s->frames[s->nframes], frame);
        s->nframes++;
    }
}

static void
sink_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    sink_t     *s = ctx;
    ngx_uint_t  i;

    s->track_notifications++;
    s->ntracks = (tracks->count < 8) ? tracks->count : 8;

    for (i = 0; i < s->ntracks; i++) {
        s->tracks[i] = tracks->tracks[i];
    }
}

static void
sink_reset(sink_t *s)
{
    ngx_uint_t  i;

    for (i = 0; i < s->nframes; i++) {
        ngx_media_frame_release(&s->frames[i]);
    }

    ngx_memzero(s, sizeof(sink_t));
}

static uint32_t
crc32_mpeg(const u_char *p, size_t len)
{
    uint32_t  crc = 0xFFFFFFFFu;
    size_t    i;
    int       b;

    for (i = 0; i < len; i++) {
        crc ^= (uint32_t) p[i] << 24;

        for (b = 0; b < 8; b++) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u
                                      : (crc << 1);
        }
    }

    return crc;
}

static void
put_crc(u_char *p, size_t len)
{
    uint32_t  crc = crc32_mpeg(p, len);

    p[len] = (u_char) (crc >> 24);
    p[len + 1] = (u_char) (crc >> 16);
    p[len + 2] = (u_char) (crc >> 8);
    p[len + 3] = (u_char) crc;
}

static void
write_packet(ts_writer_t *w, ngx_uint_t pid, ngx_uint_t pusi, ngx_uint_t cc,
    const u_char *payload, size_t len, ngx_uint_t with_pcr, uint64_t pcr)
{
    u_char  *p = w->buf + w->len;
    size_t   af = 0;

    TEST_ASSERT(len <= 184);

    memset(p, 0xFF, NGX_MEDIA_TS_PACKET_SIZE);

    p[0] = 0x47;
    p[1] = (u_char) ((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    p[2] = (u_char) (pid & 0xFF);

    if (with_pcr) {
        af = 1 + 7;
        p[3] = (u_char) ((0x03 << 4) | (cc & 0x0F));
        p[4] = 7;
        p[5] = 0x10;
        p[6] = (u_char) (pcr >> 25);
        p[7] = (u_char) (pcr >> 17);
        p[8] = (u_char) (pcr >> 9);
        p[9] = (u_char) (pcr >> 1);
        p[10] = (u_char) (((pcr & 1) << 7) | 0x7E);

    } else if (len < 184) {
        size_t  stuff = 184 - len - 1;

        af = 1 + stuff;
        p[3] = (u_char) ((0x03 << 4) | (cc & 0x0F));
        p[4] = (u_char) stuff;

        if (stuff > 0) {
            p[5] = 0x00;            /* no adaptation flags */
        }

    } else {
        p[3] = (u_char) ((0x01 << 4) | (cc & 0x0F));
    }

    if (len > 184 - af) {
        len = 184 - af;
    }

    if (len > 0) {
        memcpy(p + 4 + af, payload, len);
    }

    w->len += NGX_MEDIA_TS_PACKET_SIZE;
}

static void
put_pes_ts(u_char *p, ngx_uint_t prefix, uint64_t v)
{
    p[0] = (u_char) ((prefix << 4) | (((v >> 30) & 0x07) << 1) | 1);
    p[1] = (u_char) ((v >> 22) & 0xFF);
    p[2] = (u_char) ((((v >> 15) & 0x7F) << 1) | 1);
    p[3] = (u_char) ((v >> 7) & 0xFF);
    p[4] = (u_char) (((v & 0x7F) << 1) | 1);
}

static size_t
write_pes(u_char *out, ngx_uint_t stream_id, ngx_uint_t has_pts,
    ngx_uint_t has_dts, uint64_t pts, uint64_t dts, const u_char *payload,
    size_t len)
{
    size_t  off = 9;
    size_t  total;

    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0x01;
    out[3] = (u_char) stream_id;
    out[6] = 0x80;
    out[7] = (u_char) ((has_pts ? 0x80 : 0x00) | (has_dts ? 0x40 : 0x00));

    if (has_pts) {
        put_pes_ts(out + 9, has_dts ? 0x3 : 0x2, pts);
        off += 5;
    }

    if (has_dts) {
        put_pes_ts(out + 14, 0x1, dts);
        off += 5;
    }

    out[8] = (u_char) (off - 9);

    memcpy(out + off, payload, len);

    total = off - 6 + len;
    out[4] = (u_char) ((total >> 8) & 0xFF);
    out[5] = (u_char) (total & 0xFF);

    return off + len;
}

static void
write_pes_packets(ts_writer_t *w, ngx_uint_t pid, ngx_uint_t *cc,
    const u_char *pes, size_t len, ngx_uint_t with_pcr, uint64_t pcr)
{
    size_t      off = 0;
    ngx_uint_t  first = 1;

    while (off < len) {
        size_t  take = len - off;

        if (take > 184) {
            take = 184;
        }

        write_packet(w, pid, first, *cc, pes + off, take,
                     (first && with_pcr) ? 1 : 0, pcr);

        *cc = (*cc + 1) & 0x0F;
        off += take;
        first = 0;
    }
}

static void
write_psi(ts_writer_t *w, ngx_uint_t pid, ngx_uint_t *cc,
    const u_char *section, size_t len)
{
    u_char  payload[184];

    memset(payload, 0xFF, sizeof(payload));

    payload[0] = 0x00;              /* pointer_field */
    memcpy(payload + 1, section, len);

    write_packet(w, pid, 1, *cc, payload, sizeof(payload), 0, 0);

    *cc = (*cc + 1) & 0x0F;
}

static size_t
build_pat(u_char *sec, ngx_uint_t pmt_pid)
{
    sec[0] = 0x00;
    sec[1] = 0xB0;
    sec[2] = 0x0D;
    sec[3] = 0x00;
    sec[4] = 0x01;                  /* transport_stream_id */
    sec[5] = 0xC1;                  /* version 0, current */
    sec[6] = 0x00;
    sec[7] = 0x00;
    sec[8] = 0x00;
    sec[9] = 0x01;                  /* program_number 1 */
    sec[10] = (u_char) (0xE0 | ((pmt_pid >> 8) & 0x1F));
    sec[11] = (u_char) (pmt_pid & 0xFF);

    put_crc(sec, 12);

    return 16;
}

static size_t
build_pmt(u_char *sec, ngx_uint_t pcr_pid, const ngx_uint_t *pids,
    const u_char *types, ngx_uint_t n)
{
    size_t      len = 9 + 5 * n + 4;
    size_t      p;
    ngx_uint_t  i;

    sec[0] = 0x02;
    sec[1] = (u_char) (0xB0 | ((len >> 8) & 0x0F));
    sec[2] = (u_char) (len & 0xFF);
    sec[3] = 0x00;
    sec[4] = 0x01;                  /* program_number */
    sec[5] = 0xC1;                  /* version 0, current */
    sec[6] = 0x00;
    sec[7] = 0x00;
    sec[8] = (u_char) (0xE0 | ((pcr_pid >> 8) & 0x1F));
    sec[9] = (u_char) (pcr_pid & 0xFF);
    sec[10] = 0xF0;                 /* program_info_length 0 */
    sec[11] = 0x00;

    p = 12;

    for (i = 0; i < n; i++) {
        sec[p] = types[i];
        sec[p + 1] = (u_char) (0xE0 | ((pids[i] >> 8) & 0x1F));
        sec[p + 2] = (u_char) (pids[i] & 0xFF);
        sec[p + 3] = 0xF0;          /* ES_info_length 0 */
        sec[p + 4] = 0x00;
        p += 5;
    }

    put_crc(sec, p);

    return p + 4;
}

static size_t
build_adts(u_char *out, size_t payload_len, ngx_uint_t profile,
    ngx_uint_t sf_index, ngx_uint_t channels)
{
    size_t  frame_len = 7 + payload_len;

    out[0] = 0xFF;
    out[1] = 0xF1;
    out[2] = (u_char) ((profile << 6) | (sf_index << 2)
                       | ((channels >> 2) & 0x01));
    out[3] = (u_char) (((channels & 0x03) << 6)
                       | ((frame_len >> 11) & 0x03));
    out[4] = (u_char) ((frame_len >> 3) & 0xFF);
    out[5] = (u_char) (((frame_len & 0x07) << 5) | 0x1F);
    out[6] = 0xFC;

    memset(out + 7, 0xAA, payload_len);

    return frame_len;
}

/* AUD + SPS + PPS + IDR: configuration plus one sync access unit */
static size_t
build_video_keyframe_au(u_char *out)
{
    static const u_char  au[] = {
        0x00, 0x00, 0x00, 0x01, 0x09, 0xF0,
        0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E,
        0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21
    };

    memcpy(out, au, sizeof(au));

    return sizeof(au);
}

static size_t
build_video_p_frame_au(u_char *out)
{
    static const u_char  au[] = {
        0x00, 0x00, 0x00, 0x01, 0x09, 0xF0,
        0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x22
    };

    memcpy(out, au, sizeof(au));

    return sizeof(au);
}

static int
setup_demux(ngx_media_ts_demux_t *demux, sink_t *sink, size_t max_au_bytes)
{
    ngx_media_ts_demux_conf_t  conf;
    ngx_media_ts_sink_t        s;

    conf.max_tracks = 8;
    conf.max_au_bytes = max_au_bytes;

    s.tracks = sink_tracks;
    s.frame = sink_frame;

    return (ngx_media_ts_demux_init(demux, &conf, &s, sink, NULL) == NGX_OK)
           ? 0 : -1;
}

static void
write_psi_pair(ts_writer_t *w, ngx_uint_t *pat_cc, ngx_uint_t *pmt_cc,
    ngx_uint_t pcr_pid, const ngx_uint_t *pids, const u_char *types,
    ngx_uint_t n)
{
    u_char  sec[128];

    write_psi(w, NGX_MEDIA_TS_PID_PAT, pat_cc, sec, build_pat(sec, PMT_PID));

    {
        u_char  pmt[128];

        write_psi(w, PMT_PID, pmt_cc, pmt,
                  build_pmt(pmt, pcr_pid, pids, types, n));
    }
}

int
main(void)
{
    ts_writer_t                w;
    sink_t                     sink;
    ngx_media_ts_demux_t       demux;
    ngx_media_ts_demux_stats_t stats;
    ngx_uint_t                 pat_cc, pmt_cc, video_cc, audio_cc;
    u_char                     pes[4096];
    u_char                     payload[4096];
    size_t                     video_au_len, audio_len, pes_len, i;
    ngx_uint_t                 pids[2];
    u_char                     types[2];

    ngx_memzero(&sink, sizeof(sink));
    ngx_memzero(&w, sizeof(w));

    pat_cc = pmt_cc = video_cc = audio_cc = 0;
    pids[0] = VIDEO_PID;
    pids[1] = AUDIO_PID;
    types[0] = NGX_MEDIA_TS_STREAM_H264;
    types[1] = NGX_MEDIA_TS_STREAM_AAC_ADTS;

    TEST_ASSERT_EQ_INT(setup_demux(&demux, &sink, 0), 0);

    TEST_CASE("PAT/PMT discovery publishes the track set");
    w.len = 0;
    write_psi_pair(&w, &pat_cc, &pmt_cc, VIDEO_PID, pids, types, 2);

    TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux, w.buf, w.len), NGX_OK);
    ngx_media_ts_demux_stats(&demux, &stats);
    TEST_ASSERT_EQ_U64(stats.psi_errors, 0);
    TEST_ASSERT_EQ_U64(stats.crc_errors, 0);
    TEST_ASSERT(sink.track_notifications >= 1);
    TEST_ASSERT_EQ_U64(sink.ntracks, 2);
    TEST_ASSERT_EQ_U64(sink.tracks[0].codec, NGX_MEDIA_CODEC_H264);
    TEST_ASSERT_EQ_U64(sink.tracks[0].media_type, NGX_MEDIA_TYPE_VIDEO);
    TEST_ASSERT_EQ_U64(sink.tracks[1].codec, NGX_MEDIA_CODEC_AAC);
    TEST_ASSERT_EQ_U64(sink.tracks[1].media_type, NGX_MEDIA_TYPE_AUDIO);

    TEST_CASE("keyframe access unit emits config plus a sync frame");
    w.len = 0;
    video_au_len = build_video_keyframe_au(payload);
    pes_len = write_pes(pes, 0xE0, 1, 0, 900000, 0, payload, video_au_len);
    write_pes_packets(&w, VIDEO_PID, &video_cc, pes, pes_len, 1, 450000);

    TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux, w.buf, w.len), NGX_OK);
    ngx_media_ts_demux_flush(&demux);

    ngx_media_ts_demux_stats(&demux, &stats);
    TEST_ASSERT_EQ_U64(stats.frames_out, 1);
    TEST_ASSERT_EQ_U64(stats.config_frames_out, 1);
    TEST_ASSERT_EQ_U64(stats.has_pcr, 1);
    TEST_ASSERT_EQ_I64(stats.last_pcr, 450000);
    TEST_ASSERT_EQ_U64(sink.nframes, 2);

    TEST_ASSERT_EQ_U64(sink.frames[0].config, 1);
    TEST_ASSERT_EQ_U64(sink.frames[0].track_index, 0);
    TEST_ASSERT_EQ_U64(sink.frames[0].payload_format, NGX_MEDIA_PAYLOAD_ANNEXB);
    TEST_ASSERT_NOT_NULL(sink.frames[0].payload);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(sink.frames[0].payload), 16);

    TEST_ASSERT_EQ_U64(sink.frames[1].config, 0);
    TEST_ASSERT_EQ_U64(sink.frames[1].codec, NGX_MEDIA_CODEC_H264);
    TEST_ASSERT_EQ_U64(sink.frames[1].keyframe, 1);
    TEST_ASSERT_EQ_I64(sink.frames[1].pts, 900000);
    TEST_ASSERT_EQ_I64(sink.frames[1].dts, 900000);
    TEST_ASSERT_NOT_NULL(sink.frames[1].payload);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(sink.frames[1].payload), video_au_len);
    TEST_ASSERT_EQ_U64(ngx_memcmp(ngx_media_buf_data(sink.frames[1].payload),
                                  payload, video_au_len), 0);

    TEST_CASE("non-key access unit does not repeat the config frame");
    sink_reset(&sink);
    w.len = 0;
    video_au_len = build_video_p_frame_au(payload);
    pes_len = write_pes(pes, 0xE0, 1, 1, 903000, 902000, payload,
                        video_au_len);
    write_pes_packets(&w, VIDEO_PID, &video_cc, pes, pes_len, 0, 0);

    TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux, w.buf, w.len), NGX_OK);
    ngx_media_ts_demux_flush(&demux);

    TEST_ASSERT_EQ_U64(sink.nframes, 1);
    TEST_ASSERT_EQ_U64(sink.frames[0].keyframe, 0);
    TEST_ASSERT_EQ_I64(sink.frames[0].pts, 903000);
    TEST_ASSERT_EQ_I64(sink.frames[0].dts, 902000);

    TEST_CASE("audio PES is sliced into ADTS frames with paced timestamps");
    sink_reset(&sink);
    w.len = 0;
    audio_len = 0;

    for (i = 0; i < 3; i++) {
        audio_len += build_adts(payload + audio_len, 10, 1, 3, 2);
    }

    pes_len = write_pes(pes, 0xC0, 1, 0, 1000, 0, payload, audio_len);
    write_pes_packets(&w, AUDIO_PID, &audio_cc, pes, pes_len, 0, 0);

    TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux, w.buf, w.len), NGX_OK);
    ngx_media_ts_demux_flush(&demux);

    TEST_ASSERT_EQ_U64(sink.nframes, 4);
    TEST_ASSERT_EQ_U64(sink.frames[0].config, 1);
    TEST_ASSERT_EQ_U64(sink.frames[0].media_type, NGX_MEDIA_TYPE_AUDIO);
    TEST_ASSERT_EQ_U64(sink.frames[0].payload_format, NGX_MEDIA_PAYLOAD_RAW);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(sink.frames[0].payload), 2);
    TEST_ASSERT_EQ_U64(*(ngx_media_buf_data(sink.frames[0].payload)), 0x11);
    TEST_ASSERT_EQ_U64(*(ngx_media_buf_data(sink.frames[0].payload) + 1), 0x90);

    TEST_ASSERT_EQ_I64(sink.frames[1].pts, 1000);
    TEST_ASSERT_EQ_I64(sink.frames[2].pts, 2920);
    TEST_ASSERT_EQ_I64(sink.frames[3].pts, 4840);
    TEST_ASSERT_EQ_U64(sink.frames[1].payload_format, NGX_MEDIA_PAYLOAD_ADTS);
    TEST_ASSERT_EQ_U64(sink.frames[1].keyframe, 1);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(sink.frames[1].payload), 17);
    TEST_ASSERT_EQ_U64(sink.tracks[1].sample_rate, 48000);
    TEST_ASSERT_EQ_U64(sink.tracks[1].channels, 2);

    TEST_CASE("continuity and transport errors are counted");
    {
        ngx_media_ts_demux_t  d;
        ts_writer_t           v;
        u_char               *p;
        ngx_uint_t            pcc = 0, vcc = 0, mcc = 0;

        ngx_memzero(&v, sizeof(v));
        sink_reset(&sink);

        TEST_ASSERT_EQ_INT(setup_demux(&d, &sink, 0), 0);

        write_psi_pair(&v, &pcc, &mcc, VIDEO_PID, pids, types, 2);

        video_au_len = build_video_p_frame_au(payload);
        pes_len = write_pes(pes, 0xE0, 1, 0, 906000, 0, payload,
                            video_au_len);
        write_pes_packets(&v, VIDEO_PID, &vcc, pes, pes_len, 0, 0);

        /* expected continuity counter is 1 here, 12 arrives instead */
        write_packet(&v, VIDEO_PID, 0, 12, payload, 16, 0, 0);

        p = v.buf + v.len;
        write_packet(&v, VIDEO_PID, 0, 13, payload, 16, 0, 0);
        p[1] |= 0x80;               /* transport error indicator */

        TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&d, v.buf, v.len), NGX_OK);
        ngx_media_ts_demux_stats(&d, &stats);
        TEST_ASSERT_EQ_U64(stats.continuity_errors, 1);
        TEST_ASSERT_EQ_U64(stats.transport_errors, 1);

        ngx_media_ts_demux_destroy(&d);
    }

    TEST_CASE("sync loss is reported and the stream resynchronizes");
    {
        ngx_media_ts_demux_t  d;
        ts_writer_t           v;
        ngx_uint_t            pcc = 0, vcc = 0, mcc = 0;

        ngx_memzero(&v, sizeof(v));
        sink_reset(&sink);

        TEST_ASSERT_EQ_INT(setup_demux(&d, &sink, 0), 0);

        write_psi_pair(&v, &pcc, &mcc, VIDEO_PID, pids, types, 2);

        video_au_len = build_video_keyframe_au(payload);
        pes_len = write_pes(pes, 0xE0, 1, 0, 45000, 0, payload, video_au_len);
        write_pes_packets(&v, VIDEO_PID, &vcc, pes, pes_len, 0, 0);

        /* one junk byte shifts the whole stream by one byte */
        {
            ts_writer_t  shifted;

            ngx_memzero(&shifted, sizeof(shifted));
            shifted.buf[0] = 0x00;
            memcpy(shifted.buf + 1, v.buf, v.len);
            shifted.len = v.len + 1;

            TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&d, shifted.buf,
                                                       shifted.len), NGX_OK);
        }
        ngx_media_ts_demux_flush(&d);

        ngx_media_ts_demux_stats(&d, &stats);
        TEST_ASSERT(stats.sync_errors >= 1);
        TEST_ASSERT(stats.skipped_bytes >= 1);
        TEST_ASSERT(stats.frames_out >= 1);
        TEST_ASSERT_EQ_I64(sink.frames[sink.nframes - 1].pts, 45000);

        ngx_media_ts_demux_destroy(&d);
    }

    TEST_CASE("a corrupted PAT is rejected by CRC and changes nothing");
    sink_reset(&sink);
    w.len = 0;

    {
        u_char  sec[128];

        write_psi(&w, NGX_MEDIA_TS_PID_PAT, &pat_cc, sec,
                  build_pat(sec, 0x1F00));

        /* break the CRC inside the section (packet offset 4 + pointer + 12) */
        w.buf[w.len - NGX_MEDIA_TS_PACKET_SIZE + 4 + 1 + 12] ^= 0xFF;
    }

    TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux, w.buf, w.len), NGX_OK);
    ngx_media_ts_demux_stats(&demux, &stats);
    TEST_ASSERT_EQ_U64(stats.crc_errors, 1);
    TEST_ASSERT_EQ_U64(demux.pmt_pid, PMT_PID);
    TEST_ASSERT_EQ_U64(sink.track_notifications, 0);

    TEST_CASE("unsupported stream types are counted, not tracked");
    {
        ngx_uint_t  more_pids[3];
        u_char      more_types[3];

        more_pids[0] = VIDEO_PID;
        more_pids[1] = AUDIO_PID;
        more_pids[2] = 0x1200;
        more_types[0] = NGX_MEDIA_TS_STREAM_H264;
        more_types[1] = NGX_MEDIA_TS_STREAM_AAC_ADTS;
        more_types[2] = NGX_MEDIA_TS_STREAM_MPEG2_VIDEO;

        w.len = 0;
        write_psi_pair(&w, &pat_cc, &pmt_cc, VIDEO_PID, more_pids,
                       more_types, 3);

        TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux, w.buf, w.len),
                           NGX_OK);
        ngx_media_ts_demux_stats(&demux, &stats);
        TEST_ASSERT_EQ_U64(stats.unsupported_streams, 1);
        TEST_ASSERT_EQ_U64(demux.ntracks, 2);
    }

    TEST_CASE("partial feeds are carried across calls");
    {
        ngx_media_ts_demux_t  demux2;
        ts_writer_t           w2;
        size_t                half;
        ngx_uint_t            pcc = 0, vcc = 0, mcc = 0;

        ngx_memzero(&w2, sizeof(w2));
        sink_reset(&sink);

        w2.len = 0;
        write_psi_pair(&w2, &pcc, &mcc, VIDEO_PID, pids, types, 2);

        video_au_len = build_video_keyframe_au(payload);
        pes_len = write_pes(pes, 0xE0, 1, 0, 45000, 0, payload, video_au_len);
        write_pes_packets(&w2, VIDEO_PID, &vcc, pes, pes_len, 0, 0);

        TEST_ASSERT_EQ_INT(setup_demux(&demux2, &sink, 0), 0);

        half = 300;

        TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux2, w2.buf, half),
                           NGX_OK);
        TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux2, w2.buf + half,
                                                   w2.len - half), NGX_OK);
        ngx_media_ts_demux_flush(&demux2);

        ngx_media_ts_demux_stats(&demux2, &stats);
        TEST_ASSERT_EQ_U64(stats.frames_out, 1);
        TEST_ASSERT_EQ_U64(stats.sync_errors, 0);
        TEST_ASSERT_EQ_U64(sink.nframes, 2);
        TEST_ASSERT_EQ_I64(sink.frames[1].pts, 45000);

        ngx_media_ts_demux_destroy(&demux2);
    }

    TEST_CASE("oversized access units are dropped, never emitted");
    {
        ngx_media_ts_demux_t  demux3;
        ts_writer_t           w3;
        u_char                big[512];
        ngx_uint_t            vcc = 0, pcc = 0, mcc = 0;

        ngx_memzero(&w3, sizeof(w3));
        ngx_memzero(big, sizeof(big));
        sink_reset(&sink);

        big[0] = 0x00; big[1] = 0x00; big[2] = 0x00; big[3] = 0x01;
        big[4] = 0x41;

        TEST_ASSERT_EQ_INT(setup_demux(&demux3, &sink, 64), 0);

        write_psi_pair(&w3, &pcc, &mcc, VIDEO_PID, pids, types, 2);

        pes_len = write_pes(pes, 0xE0, 1, 0, 1000, 0, big, sizeof(big));
        write_pes_packets(&w3, VIDEO_PID, &vcc, pes, pes_len, 0, 0);

        TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux3, w3.buf, w3.len),
                           NGX_OK);
        ngx_media_ts_demux_flush(&demux3);

        ngx_media_ts_demux_stats(&demux3, &stats);
        TEST_ASSERT(stats.au_overflows >= 1);
        TEST_ASSERT_EQ_U64(stats.frames_out, 0);

        ngx_media_ts_demux_destroy(&demux3);
    }

    sink_reset(&sink);
    ngx_media_ts_demux_destroy(&demux);

    TEST_LEAKS();

    TEST_MAIN_END();
}

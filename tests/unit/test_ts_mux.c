#include "ngx_media_test.h"
#include "ngx_media_ts_demux.h"
#include "ngx_media_ts_mux.h"

typedef struct {
    ngx_media_frame_t  frames[32];
    ngx_uint_t         nframes;
    ngx_media_track_t  tracks[8];
    ngx_uint_t         ntracks;
    ngx_uint_t         track_calls;
} sink_t;

static void
sink_frame(void *ctx, const ngx_media_frame_t *frame)
{
    sink_t  *s = ctx;

    if (s->nframes < 32) {
        (void) ngx_media_frame_copy(&s->frames[s->nframes], frame);
        s->nframes++;
    }
}

static void
sink_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    sink_t     *s = ctx;
    ngx_uint_t  i;

    s->track_calls++;
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

static ngx_media_frame_t
video_frame(size_t len, int64_t pts, int64_t dts, unsigned keyframe)
{
    ngx_media_frame_t  frame;
    ngx_media_buf_t   *payload;
    u_char            *p;

    ngx_media_frame_init(&frame);

    frame.media_type = NGX_MEDIA_TYPE_VIDEO;
    frame.codec = NGX_MEDIA_CODEC_H264;
    frame.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    frame.pts = pts;
    frame.dts = dts;
    frame.keyframe = keyframe ? 1 : 0;

    payload = ngx_media_buf_alloc(len);
    if (payload == NULL) {
        return frame;
    }

    p = ngx_media_buf_data(payload);

    /* a synthetic access unit: start code + filler */
    p[0] = 0x00;
    p[1] = 0x00;
    p[2] = 0x00;
    p[3] = 0x01;
    p[4] = keyframe ? 0x65 : 0x41;

    if (len > 5) {
        memset(p + 5, 0x55, len - 5);
    }

    (void) ngx_media_buf_freeze(payload, len);
    frame.payload = payload;

    return frame;
}

static ngx_media_frame_t
audio_frame(size_t len, int64_t pts)
{
    ngx_media_frame_t  frame;
    ngx_media_buf_t   *payload;
    u_char            *p;

    ngx_media_frame_init(&frame);

    frame.media_type = NGX_MEDIA_TYPE_AUDIO;
    frame.codec = NGX_MEDIA_CODEC_AAC;
    frame.payload_format = NGX_MEDIA_PAYLOAD_ADTS;
    frame.pts = pts;
    frame.dts = pts;
    frame.keyframe = 1;

    payload = ngx_media_buf_alloc(len);
    if (payload == NULL) {
        return frame;
    }

    p = ngx_media_buf_data(payload);

    /* minimal ADTS frame so the demuxer can split it again */
    p[0] = 0xFF;
    p[1] = 0xF1;
    p[2] = (1 << 6) | (3 << 2);
    p[3] = (2 << 6) | (u_char) (((len) >> 11) & 0x03);
    p[4] = (u_char) (((len) >> 3) & 0xFF);
    p[5] = (u_char) ((((len) & 0x07) << 5) | 0x1F);
    p[6] = 0xFC;

    if (len > 7) {
        memset(p + 7, 0x77, len - 7);
    }

    (void) ngx_media_buf_freeze(payload, len);
    frame.payload = payload;

    return frame;
}

int
main(void)
{
    ngx_media_ts_mux_t          mux;
    ngx_media_ts_mux_conf_t     conf;
    ngx_media_ts_demux_t        demux;
    ngx_media_ts_demux_conf_t   dconf;
    ngx_media_ts_sink_t         sink;
    ngx_media_ts_burst_t        burst;
    ngx_media_ts_demux_stats_t  stats;
    ngx_media_trackset_t        tracks;
    ngx_media_track_t           track;
    sink_t                      out;
    ngx_media_frame_t           frame;
    ngx_uint_t                  i;

    ngx_memzero(&out, sizeof(out));

    TEST_CASE("muxer setup");
    ngx_media_ts_mux_conf_default(&conf);
    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_init(&mux, &conf, NULL), NGX_OK);

    TEST_ASSERT_EQ_INT(ngx_media_trackset_init(&tracks, 4, NULL), NGX_OK);

    ngx_memzero(&track, sizeof(track));
    track.media_type = NGX_MEDIA_TYPE_VIDEO;
    track.codec = NGX_MEDIA_CODEC_H264;
    track.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    TEST_ASSERT(ngx_media_trackset_add(&tracks, &track) >= 0);

    ngx_memzero(&track, sizeof(track));
    track.media_type = NGX_MEDIA_TYPE_AUDIO;
    track.codec = NGX_MEDIA_CODEC_AAC;
    track.payload_format = NGX_MEDIA_PAYLOAD_ADTS;
    track.sample_rate = 48000;
    track.channels = 2;
    TEST_ASSERT(ngx_media_trackset_add(&tracks, &track) >= 0);

    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_set_tracks(&mux, &tracks), NGX_OK);

    TEST_CASE("one burst, one backing allocation, sliced per frame");
    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_burst_init(&mux, &burst, 64 * 1024),
                       NGX_OK);
    TEST_ASSERT_NOT_NULL(burst.backing);

    /* keyframe with configuration, a P frame and two AAC frames */
    frame = video_frame(2000, 900000, 900000, 1);
    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_write_frame(&mux, &burst, &frame, 1),
                       NGX_OK);
    ngx_media_frame_release(&frame);

    for (i = 0; i < 3; i++) {
        frame = video_frame(500 + i, 903600 + (int64_t) i * 3600,
                            903600 + (int64_t) i * 3600, 0);
        TEST_ASSERT_EQ_INT(ngx_media_ts_mux_write_frame(&mux, &burst, &frame,
                                                        0), NGX_OK);
        ngx_media_frame_release(&frame);
    }

    for (i = 0; i < 2; i++) {
        frame = audio_frame(20, 900000 + (int64_t) i * 1920);
        TEST_ASSERT_EQ_INT(ngx_media_ts_mux_write_frame(&mux, &burst, &frame,
                                                        0), NGX_OK);
        ngx_media_frame_release(&frame);
    }

    TEST_ASSERT_EQ_U64(burst.nslices, 6);
    TEST_ASSERT_EQ_U64(mux.pcr_packets, 1);

    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_burst_end(&mux, &burst), NGX_OK);

    TEST_ASSERT(ngx_media_ts_burst_size(&burst) > 0);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(burst.backing),
                       ngx_media_ts_burst_size(&burst));

    /* every slice addresses the single backing allocation */
    for (i = 0; i < burst.nslices; i++) {
        TEST_ASSERT(burst.slices[i].len > 0);
        TEST_ASSERT(burst.slices[i].offset + burst.slices[i].len
                    <= ngx_media_ts_burst_size(&burst));
    }

    TEST_ASSERT_EQ_U64(burst.slices[0].keyframe, 1);
    TEST_ASSERT_EQ_I64(burst.slices[1].pts, 903600);
    TEST_ASSERT_EQ_U64(burst.slices[4].media_type, NGX_MEDIA_TYPE_AUDIO);

    TEST_CASE("the demuxer recovers exactly the frames that were muxed");
    dconf.max_tracks = 8;
    dconf.max_au_bytes = 1024 * 1024;

    sink.tracks = sink_tracks;
    sink.frame = sink_frame;

    TEST_ASSERT_EQ_INT(ngx_media_ts_demux_init(&demux, &dconf, &sink, &out,
                                               NULL), NGX_OK);

    TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux,
                                               ngx_media_buf_data(burst.backing),
                                               ngx_media_ts_burst_size(&burst)),
                       NGX_OK);
    ngx_media_ts_demux_flush(&demux);

    ngx_media_ts_demux_stats(&demux, &stats);

    TEST_ASSERT_EQ_U64(stats.sync_errors, 0);
    TEST_ASSERT_EQ_U64(stats.transport_errors, 0);
    TEST_ASSERT_EQ_U64(stats.continuity_errors, 0);
    TEST_ASSERT_EQ_U64(stats.psi_errors, 0);
    TEST_ASSERT_EQ_U64(stats.crc_errors, 0);
    TEST_ASSERT_EQ_U64(stats.pes_errors, 0);
    TEST_ASSERT_EQ_U64(stats.unsupported_streams, 0);
    TEST_ASSERT_EQ_U64(stats.has_pcr, 1);
    TEST_ASSERT_EQ_I64(stats.last_pcr, 900000);

    TEST_ASSERT(out.track_calls >= 1);
    TEST_ASSERT_EQ_U64(out.ntracks, 2);
    TEST_ASSERT_EQ_U64(out.tracks[0].media_type, NGX_MEDIA_TYPE_VIDEO);
    TEST_ASSERT_EQ_U64(out.tracks[0].codec, NGX_MEDIA_CODEC_H264);
    TEST_ASSERT_EQ_U64(out.tracks[1].media_type, NGX_MEDIA_TYPE_AUDIO);
    TEST_ASSERT_EQ_U64(out.tracks[1].codec, NGX_MEDIA_CODEC_AAC);
    TEST_ASSERT_EQ_U64(out.tracks[1].sample_rate, 48000);
    TEST_ASSERT_EQ_U64(out.tracks[1].channels, 2);

    /* four video access units, one audio config frame and two audio frames */
    TEST_ASSERT_EQ_U64(out.nframes, 7);

    TEST_ASSERT_EQ_I64(out.frames[0].pts, 900000);
    TEST_ASSERT_EQ_I64(out.frames[0].dts, 900000);
    TEST_ASSERT_EQ_U64(out.frames[0].keyframe, 1);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(out.frames[0].payload), 2000);
    TEST_ASSERT_EQ_U64(ngx_media_buf_data(out.frames[0].payload)[4], 0x65);

    TEST_ASSERT_EQ_I64(out.frames[1].pts, 903600);
    TEST_ASSERT_EQ_U64(out.frames[1].keyframe, 0);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(out.frames[1].payload), 500);
    TEST_ASSERT_EQ_U64(ngx_media_buf_data(out.frames[1].payload)[4], 0x41);

    TEST_ASSERT_EQ_I64(out.frames[2].pts, 907200);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(out.frames[2].payload), 501);

    /* the audio configuration frame precedes the audio media frames */
    TEST_ASSERT_EQ_U64(out.frames[3].media_type, NGX_MEDIA_TYPE_AUDIO);
    TEST_ASSERT_EQ_U64(out.frames[3].config, 1);
    TEST_ASSERT_EQ_U64(out.frames[3].payload_format, NGX_MEDIA_PAYLOAD_RAW);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(out.frames[3].payload), 2);

    TEST_ASSERT_EQ_U64(out.frames[4].media_type, NGX_MEDIA_TYPE_AUDIO);
    TEST_ASSERT_EQ_U64(out.frames[4].payload_format, NGX_MEDIA_PAYLOAD_ADTS);
    TEST_ASSERT_EQ_I64(out.frames[4].pts, 900000);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(out.frames[4].payload), 20);

    TEST_ASSERT_EQ_I64(out.frames[5].pts, 901920);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(out.frames[5].payload), 20);

    /* the last video access unit is flushed when the audio PES arrives */
    TEST_ASSERT_EQ_U64(out.frames[6].media_type, NGX_MEDIA_TYPE_VIDEO);
    TEST_ASSERT_EQ_I64(out.frames[6].pts, 910800);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(out.frames[6].payload), 502);

    sink_reset(&out);

    TEST_CASE("continuity continues across bursts");
    ngx_media_ts_mux_burst_destroy(&burst);

    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_burst_init(&mux, &burst, 16 * 1024),
                       NGX_OK);

    frame = video_frame(800, 914400, 914400, 0);
    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_write_frame(&mux, &burst, &frame, 0),
                       NGX_OK);
    ngx_media_frame_release(&frame);
    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_burst_end(&mux, &burst), NGX_OK);

    TEST_ASSERT_EQ_INT(ngx_media_ts_demux_feed(&demux,
                                               ngx_media_buf_data(burst.backing),
                                               ngx_media_ts_burst_size(&burst)),
                       NGX_OK);

    ngx_media_ts_demux_stats(&demux, &stats);
    TEST_ASSERT_EQ_U64(stats.continuity_errors, 0);

    TEST_CASE("a frame that does not fit is rejected cleanly");
    {
        size_t  psi_only;

        ngx_media_ts_mux_burst_destroy(&burst);

        TEST_ASSERT_EQ_INT(ngx_media_ts_mux_burst_init(&mux, &burst, 1024),
                           NGX_OK);

        /* the burst currently holds only PAT and PMT */
        psi_only = ngx_media_ts_burst_size(&burst);
        TEST_ASSERT_EQ_U64(psi_only, 2 * 188);

        frame = video_frame(4000, 1000000, 1000000, 0);
        TEST_ASSERT_EQ_INT(ngx_media_ts_mux_write_frame(&mux, &burst, &frame,
                                                        0), NGX_AGAIN);
        ngx_media_frame_release(&frame);

        /* the rejected frame left no packets or slices behind */
        TEST_ASSERT_EQ_U64(burst.nslices, 0);
        TEST_ASSERT_EQ_U64(ngx_media_ts_burst_size(&burst), psi_only);

        /* the same frame fits a larger burst */
        ngx_media_ts_mux_burst_destroy(&burst);
        TEST_ASSERT_EQ_INT(ngx_media_ts_mux_burst_init(&mux, &burst, 8192),
                           NGX_OK);

        frame = video_frame(4000, 1000000, 1000000, 0);
        TEST_ASSERT_EQ_INT(ngx_media_ts_mux_write_frame(&mux, &burst, &frame,
                                                        0), NGX_OK);
        ngx_media_frame_release(&frame);
        TEST_ASSERT_EQ_INT(ngx_media_ts_mux_burst_end(&mux, &burst), NGX_OK);
        TEST_ASSERT_EQ_U64(burst.nslices, 1);
    }

    TEST_CASE("NULL tolerance");
    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_init(NULL, NULL, NULL), NGX_ERROR);
    ngx_media_ts_mux_destroy(NULL);
    ngx_media_ts_mux_burst_destroy(NULL);
    TEST_ASSERT_EQ_U64(ngx_media_ts_burst_size(NULL), 0);
    ngx_media_ts_mux_conf_default(NULL);

    ngx_media_ts_mux_burst_destroy(&burst);
    ngx_media_ts_demux_destroy(&demux);
    ngx_media_trackset_destroy(&tracks);
    sink_reset(&out);

    TEST_LEAKS();

    TEST_MAIN_END();
}

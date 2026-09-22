/*
 * RTMP adapter: avcC / AudioSpecificConfig handling, AVCC <-> Annex B
 * reframing, FLV message bodies, the publisher state machine and the shared
 * fanout ring.
 *
 * The publisher test drives the same message sequence a real encoder sends:
 * an AVC sequence header, an AAC sequence header and then media messages; the
 * assertions check the resulting frames against the conventions the MPEG-TS
 * ingest path uses, so RTMP sources are interchangeable with SRT sources.
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_aac.h"
#include "ngx_media_nal.h"
#include "ngx_media_rtmp_adapter.h"

#include <stdio.h>

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if (!(cond)) {                                                        \
            ngx_media_test_failures++;                                        \
            printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,               \
                   ##__VA_ARGS__);                                            \
        }                                                                     \
    } while (0)

/* the SPS/PPS a real encoder sends, shortened to their headers */
static const u_char sps[] = {
    0x67, 0x42, 0xC0, 0x1E, 0xD9, 0x00, 0xF0, 0x11, 0x7E, 0x40
};
static const u_char pps[] = { 0x68, 0xCE, 0x3C, 0x80 };

static void
test_avcc(void)
{
    ngx_media_rtmp_avcc_t  avcc, parsed;
    u_char                 record[128];
    u_char                 annexb[128];
    size_t                 record_len = 0, annexb_len = 0;

    TEST_CASE("avcC build, parse and Annex B conversion");

    ngx_memzero(&avcc, sizeof(avcc));

    avcc.version = 1;
    avcc.profile = 0x42;
    avcc.compatibility = 0xC0;
    avcc.level = 0x1E;
    avcc.nal_length_size = 4;
    avcc.nsps = 1;
    avcc.sps[0].data = (u_char *) sps;
    avcc.sps[0].len = sizeof(sps);
    avcc.npps = 1;
    avcc.pps[0].data = (u_char *) pps;
    avcc.pps[0].len = sizeof(pps);

    CHECK(ngx_media_rtmp_avcc_build(&avcc, record, sizeof(record),
                                    &record_len) == NGX_OK,
          "avcC built");
    /* 6 header bytes, SPS with its length, the PPS count, PPS with its length */
    CHECK(record_len == 6 + 2 + sizeof(sps) + 1 + 2 + sizeof(pps),
          "avcC length: %lu", record_len);
    CHECK(record[0] == 1 && record[1] == 0x42 && record[3] == 0x1E,
          "avcC header fields");
    CHECK(record[4] == 0xFF, "nal length size 4 encoded");

    CHECK(ngx_media_rtmp_avcc_parse(record, record_len, &parsed) == NGX_OK,
          "avcC parsed");
    CHECK(parsed.nsps == 1 && parsed.npps == 1, "one SPS and one PPS");
    CHECK(parsed.nal_length_size == 4, "length size parsed");
    CHECK(parsed.profile == 0x42 && parsed.level == 0x1E, "profile and level");
    CHECK(parsed.sps[0].len == sizeof(sps)
          && ngx_memcmp(parsed.sps[0].data, sps, sizeof(sps)) == 0,
          "SPS round trip");
    CHECK(parsed.pps[0].len == sizeof(pps)
          && ngx_memcmp(parsed.pps[0].data, pps, sizeof(pps)) == 0,
          "PPS round trip");

    CHECK(ngx_media_rtmp_avcc_annexb_size(&parsed)
          == (4 + sizeof(sps)) + (4 + sizeof(pps)),
          "annexb size helper: %lu",
          ngx_media_rtmp_avcc_annexb_size(&parsed));

    CHECK(ngx_media_rtmp_avcc_to_annexb(&parsed, annexb, sizeof(annexb),
                                        &annexb_len) == NGX_OK,
          "parameter sets to Annex B");

    CHECK(annexb_len == 4 + sizeof(sps) + 4 + sizeof(pps),
          "Annex B length: %lu", annexb_len);
    CHECK(annexb[0] == 0 && annexb[1] == 0 && annexb[2] == 0
          && annexb[3] == 1 && annexb[4] == 0x67, "SPS start code");
    CHECK(annexb[4 + sizeof(sps)] == 0
          && annexb[7 + sizeof(sps)] == 1
          && annexb[8 + sizeof(sps)] == 0x68, "PPS start code");

    /* truncated and corrupt records are rejected */
    CHECK(ngx_media_rtmp_avcc_parse(record, 4, &parsed) == NGX_ERROR,
          "short avcC rejected");
    CHECK(ngx_media_rtmp_avcc_parse(record, record_len - 1, &parsed)
          == NGX_ERROR, "truncated avcC rejected");
    {
        u_char  bad[128];

        ngx_memcpy(bad, record, record_len);
        bad[0] = 2;   /* unsupported configuration version */

        CHECK(ngx_media_rtmp_avcc_parse(bad, record_len, &parsed) == NGX_ERROR,
              "bad avcC version rejected");
    }
}

static void
test_reframing(void)
{
    u_char   avcc[64], annexb[64], back[64];
    size_t   avcc_len = 0, annexb_len = 0, back_len = 0, pos = 0;

    TEST_CASE("AVCC payload to Annex B and back");

    /* two NAL units: an IDR slice and a non-IDR slice */
    {
        u_char  nal1[] = { 0x65, 0x88, 0x84, 0x00, 0x33 };
        u_char  nal2[] = { 0x41, 0x9A, 0x02 };

        avcc[pos++] = 0; avcc[pos++] = 0; avcc[pos++] = 0;
        avcc[pos++] = sizeof(nal1);
        ngx_memcpy(avcc + pos, nal1, sizeof(nal1));
        pos += sizeof(nal1);

        avcc[pos++] = 0; avcc[pos++] = 0; avcc[pos++] = 0;
        avcc[pos++] = sizeof(nal2);
        ngx_memcpy(avcc + pos, nal2, sizeof(nal2));
        pos += sizeof(nal2);
    }

    avcc_len = pos;

    CHECK(ngx_media_rtmp_avcc_payload_to_annexb(avcc, avcc_len, 4, annexb,
                                                sizeof(annexb),
                                                &annexb_len) == NGX_OK,
          "AVCC payload converted");
    /* start codes have the same width as the length prefixes they replace */
    CHECK(annexb_len == avcc_len, "payload length preserved: %lu", annexb_len);
    CHECK(annexb[0] == 0 && annexb[3] == 1 && annexb[4] == 0x65,
          "first NAL");
    CHECK(annexb[9] == 0 && annexb[12] == 1 && annexb[13] == 0x41,
          "second NAL");

    CHECK(ngx_media_rtmp_annexb_payload_to_avcc(annexb, annexb_len, 4, back,
                                                sizeof(back),
                                                &back_len) == NGX_OK,
          "Annex B converted back");
    CHECK(back_len == avcc_len, "round trip length: %lu", back_len);
    CHECK(ngx_memcmp(back, avcc, avcc_len) == 0, "round trip bytes");

    /* three byte start codes are understood too */
    {
        u_char  small[32];
        size_t  small_len = 0;

        small[0] = 0; small[1] = 0; small[2] = 1; small[3] = 0x65;
        small[4] = 0xAA; small[5] = 0xBB;

        CHECK(ngx_media_rtmp_annexb_payload_to_avcc(small, 6, 4, back,
                                                    sizeof(back),
                                                    &small_len) == NGX_OK,
              "three byte start code accepted");
        CHECK(small_len == 4 + 3, "length prefix replaces the start code: %lu",
              small_len);
        CHECK(back[0] == 0 && back[1] == 0 && back[2] == 0 && back[3] == 3,
              "length prefix written big endian: %02x %02x %02x %02x",
              back[0], back[1], back[2], back[3]);
        CHECK(back[4] == 0x65 && back[5] == 0xAA && back[6] == 0xBB,
              "NAL bytes preserved");
    }

    /* a length prefix that runs past the payload is rejected */
    CHECK(ngx_media_rtmp_avcc_payload_to_annexb(avcc, avcc_len - 2, 4, annexb,
                                                sizeof(annexb),
                                                &annexb_len) == NGX_ERROR,
          "truncated AVCC payload rejected");
}

static void
test_asc(void)
{
    ngx_media_rtmp_asc_t  asc, parsed;
    u_char                buf[8];
    size_t                len = 0;

    TEST_CASE("AudioSpecificConfig");

    ngx_memzero(&asc, sizeof(asc));

    asc.object_type = 2;          /* AAC LC */
    asc.sample_rate_index = 3;    /* 48000 */
    asc.sample_rate = 48000;
    asc.channels = 2;

    CHECK(ngx_media_rtmp_asc_build(&asc, buf, sizeof(buf), &len) == NGX_OK,
          "ASC built");
    CHECK(len == 2, "two bytes");
    CHECK(buf[0] == 0x11 && buf[1] == 0x90, "ASC bytes: %02x %02x", buf[0],
          buf[1]);

    CHECK(ngx_media_rtmp_asc_parse(buf, len, &parsed) == NGX_OK,
          "ASC parsed");
    CHECK(parsed.object_type == 2, "object type");
    CHECK(parsed.sample_rate == 48000, "sample rate");
    CHECK(parsed.channels == 2, "channels");

    /* the ASC the TS path synthesizes for the same stream must match */
    {
        ngx_media_adts_t  adts;
        u_char            from_adts[8];
        size_t            adts_len = 0;

        ngx_memzero(&adts, sizeof(adts));

        adts.object_type = 2;
        adts.sample_rate = 48000;
        adts.channels = 2;

        CHECK(ngx_media_adts_audio_specific_config(&adts, from_adts,
                                                   sizeof(from_adts),
                                                   &adts_len) == NGX_OK,
              "TS path ASC built");
        CHECK(adts_len == len && ngx_memcmp(from_adts, buf, len) == 0,
              "both paths agree on the AudioSpecificConfig");
    }

    CHECK(ngx_media_rtmp_asc_parse(buf, 1, &parsed) == NGX_ERROR,
          "short ASC rejected");
}

static void
test_flv_framing(void)
{
    u_char  body[64];
    size_t  len = 0;

    TEST_CASE("FLV message bodies");

    CHECK(ngx_media_rtmp_flv_video(body, sizeof(body), &len, 1, 40,
                                   (const u_char *) "\x65\x01", 2) == NGX_OK,
          "keyframe body built");
    CHECK(len == 7, "video body length: %lu", len);
    CHECK(body[0] == 0x17, "keyframe, AVC: %02x", body[0]);
    CHECK(body[1] == NGX_MEDIA_RTMP_AVC_NALU, "packet type NALU");
    CHECK(body[2] == 0 && body[3] == 0 && body[4] == 40,
          "composition time 40: %02x %02x %02x", body[2], body[3], body[4]);

    CHECK(ngx_media_rtmp_flv_video(body, sizeof(body), &len, 0, 0,
                                   (const u_char *) "\x41", 1) == NGX_OK,
          "inter frame body built");
    CHECK(body[0] == 0x27, "inter frame, AVC: %02x", body[0]);

    CHECK(ngx_media_rtmp_flv_audio(body, sizeof(body), &len, 0,
                                   (const u_char *) "\x01\x02", 2) == NGX_OK,
          "audio body built");
    CHECK(len == 4, "audio body length: %lu", len);
    CHECK(body[0] == 0xAF, "AAC, 44 kHz, 16 bit, stereo: %02x", body[0]);
    CHECK(body[1] == NGX_MEDIA_RTMP_AAC_RAW, "packet type raw");

    CHECK(ngx_media_rtmp_flv_audio(body, sizeof(body), &len, 1,
                                   (const u_char *) "\x11\x90", 2) == NGX_OK,
          "audio sequence header built");
    CHECK(body[1] == NGX_MEDIA_RTMP_AAC_SEQUENCE, "packet type sequence");

    CHECK(ngx_media_rtmp_flv_video(body, 3, &len, 1, 0,
                                   (const u_char *) "\x65", 1) == NGX_ERROR,
          "short destination rejected");
}

/* collects the frames a publisher produces */
typedef struct {
    ngx_uint_t               frames;
    ngx_uint_t               configs;
    ngx_media_frame_t        last;
    ngx_media_buf_t         *last_payload;
    ngx_uint_t               video_frames;
    ngx_uint_t               audio_frames;
    ngx_media_trackset_t     tracks;
    ngx_uint_t               track_notifications;
    int64_t                  pts[8];
    int64_t                  dts[8];
    ngx_uint_t               keyframes[8];
} frame_sink_t;

static ngx_int_t
sink_frame(void *ctx, const ngx_media_frame_t *frame)
{
    frame_sink_t  *sink = ctx;
    ngx_uint_t     index = sink->frames;

    if (index < 8) {
        sink->pts[index] = frame->pts;
        sink->dts[index] = frame->dts;
        sink->keyframes[index] = frame->keyframe;
    }

    if (frame->config) {
        sink->configs++;

    } else if (frame->media_type == NGX_MEDIA_TYPE_VIDEO) {
        sink->video_frames++;

    } else if (frame->media_type == NGX_MEDIA_TYPE_AUDIO) {
        sink->audio_frames++;
    }

    if (sink->last_payload != NULL) {
        ngx_media_buf_unref(sink->last_payload);
    }

    sink->last_payload = ngx_media_buf_ref(frame->payload);
    sink->last = *frame;
    sink->last.payload = sink->last_payload;
    sink->frames++;

    return NGX_OK;
}

static ngx_int_t
sink_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    frame_sink_t  *sink = ctx;
    ngx_uint_t     i;

    sink->track_notifications++;

    if (sink->tracks.tracks == NULL) {
        if (ngx_media_trackset_init(&sink->tracks, 4, NULL) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    for (i = 0; i < tracks->count; i++) {

        if (i < sink->tracks.count) {
            ngx_media_buf_unref(sink->tracks.tracks[i].config);
            sink->tracks.tracks[i] = tracks->tracks[i];
            sink->tracks.tracks[i].config =
                ngx_media_buf_ref(tracks->tracks[i].config);

        } else {
            (void) ngx_media_trackset_add(&sink->tracks, &tracks->tracks[i]);
        }
    }

    return NGX_OK;
}

/*
 * Enhanced RTMP.  The fixture bytes are the shape ffmpeg n9 actually writes
 * for HEVC: 0x90 = extended header, keyframe, SequenceStart, then "hvc1",
 * then an HEVCDecoderConfigurationRecord.
 */
static void
test_enhanced_rtmp(void)
{
    ngx_media_rtmp_hvcc_t   hvcc;
    u_char                  record[512];
    size_t                  record_len = 0, annexb_len = 0;
    u_char                  annexb[512];
    u_char                  tag[64];
    ngx_media_rtmp_publisher_t  pub;
    frame_sink_t            sink;
    ngx_uint_t              configs = 0;
    u_char                  vps[] = { 0x40, 0x01, 0x0c, 0x01 };
    u_char                  sps[] = { 0x42, 0x01, 0x01, 0x60 };
    u_char                  pps[] = { 0x44, 0x01, 0xc0 };

    TEST_CASE("enhanced RTMP: hvcC, extended headers and fourcc dispatch");

    /* build a record from parameter sets, then read it back */
    ngx_memzero(&hvcc, sizeof(hvcc));

    hvcc.version = 1;
    hvcc.nal_length_size = 4;
    hvcc.profile_idc = 1;
    hvcc.level = 60;
    hvcc.nvps = 1;
    hvcc.vps[0].data = vps;
    hvcc.vps[0].len = sizeof(vps);
    hvcc.nsps = 1;
    hvcc.sps[0].data = sps;
    hvcc.sps[0].len = sizeof(sps);
    hvcc.npps = 1;
    hvcc.pps[0].data = pps;
    hvcc.pps[0].len = sizeof(pps);

    CHECK(ngx_media_rtmp_hvcc_build(&hvcc, record, sizeof(record),
                                    &record_len) == NGX_OK,
          "hvcC built");
    CHECK(record_len > 23, "record carries the arrays: %lu", record_len);
    CHECK(record[0] == 1, "configurationVersion is 1");
    CHECK(record[22] == 3, "three arrays: VPS, SPS, PPS");

    {
        ngx_media_rtmp_hvcc_t  parsed;

        CHECK(ngx_media_rtmp_hvcc_parse(record, record_len, &parsed)
              == NGX_OK, "hvcC parsed");
        CHECK(parsed.nvps == 1 && parsed.nsps == 1 && parsed.npps == 1,
              "parameter sets recovered: %lu/%lu/%lu",
              parsed.nvps, parsed.nsps, parsed.npps);
        CHECK(parsed.level == 60, "level preserved: %lu", parsed.level);

        CHECK(ngx_media_rtmp_hvcc_to_annexb(&parsed, annexb, sizeof(annexb),
                                            &annexb_len) == NGX_OK,
              "converted to Annex B");
        CHECK(annexb_len == 12 + sizeof(vps) + sizeof(sps) + sizeof(pps),
              "blob carries three start codes: %lu", annexb_len);
    }

    /* a truncated record must be refused, not read past */
    CHECK(ngx_media_rtmp_hvcc_parse(record, 10, &hvcc) == NGX_ERROR,
          "short record rejected");
    CHECK(ngx_media_rtmp_hvcc_parse(record, record_len - 4, &hvcc)
          == NGX_ERROR, "truncated record rejected");

    /* the publisher: an extended sequence start then a coded frame */
    ngx_memzero(&sink, sizeof(sink));

    CHECK(ngx_media_rtmp_publisher_init(&pub, NULL) == NGX_OK,
          "publisher initialised");

    tag[0] = (u_char) (NGX_MEDIA_RTMP_EX_HEADER_FLAG
                       | (NGX_MEDIA_RTMP_FRAME_KEYFRAME << 4)
                       | NGX_MEDIA_RTMP_EX_SEQUENCE_START);
    ngx_memcpy(tag + 1, "hvc1", 4);
    ngx_memcpy(tag + 5, record, record_len);

    CHECK(ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_VIDEO, 0,
                                        tag, 5 + record_len, sink_frame,
                                        sink_tracks, &sink) == NGX_OK,
          "sequence start accepted");
    CHECK(pub.have_hvcc == 1, "the publisher kept the hvcC");
    CHECK(pub.tracks.count == 1, "one video track registered");
    CHECK(pub.tracks.tracks[0].codec == NGX_MEDIA_CODEC_H265,
          "track codec is H.265");
    CHECK(pub.tracks.tracks[0].config != NULL, "track carries a config blob");

    configs = sink.frames;

    /* CodedFrames: fourcc, three composition-time bytes, then NAL units */
    tag[0] = (u_char) (NGX_MEDIA_RTMP_EX_HEADER_FLAG
                       | (NGX_MEDIA_RTMP_FRAME_KEYFRAME << 4)
                       | NGX_MEDIA_RTMP_EX_CODED_FRAMES);
    ngx_memcpy(tag + 1, "hvc1", 4);
    tag[5] = tag[6] = tag[7] = 0;
    tag[8] = 0; tag[9] = 0; tag[10] = 0; tag[11] = 3;   /* one 3-byte NAL */
    tag[12] = 0x26; tag[13] = 0x01; tag[14] = 0xaf;

    CHECK(ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_VIDEO, 40,
                                        tag, 15, sink_frame, sink_tracks,
                                        &sink) == NGX_OK,
          "coded frame accepted");
    CHECK(sink.frames == configs + 1, "the coded frame reached the sink");
    /* the sequence header went to configs, so exactly one coded frame */
    CHECK(sink.video_frames == 1, "counted as video: %lu", sink.video_frames);
    CHECK(sink.configs == 1, "one configuration frame: %lu", sink.configs);

    /* an unknown fourcc is counted and skipped, never mis-parsed */
    tag[0] = (u_char) (NGX_MEDIA_RTMP_EX_HEADER_FLAG
                       | (NGX_MEDIA_RTMP_FRAME_INTER << 4)
                       | NGX_MEDIA_RTMP_EX_CODED_FRAMES);
    ngx_memcpy(tag + 1, "av01", 4);

    CHECK(ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_VIDEO, 80,
                                        tag, 15, sink_frame, sink_tracks,
                                        &sink) == NGX_OK,
          "unknown fourcc handled");
    CHECK(sink.frames == configs + 1, "it produced no frame");
    CHECK(pub.skipped >= 1, "it was counted as skipped");

    if (sink.last_payload != NULL) {
        ngx_media_buf_unref(sink.last_payload);
    }

    ngx_media_trackset_destroy(&sink.tracks);
    ngx_media_rtmp_publisher_destroy(&pub);
}

static void
test_publisher(void)
{
    ngx_media_rtmp_publisher_t  pub;
    frame_sink_t                sink;
    u_char                      message[256];
    size_t                      pos;
    ngx_uint_t                  i;

    TEST_CASE("publisher: sequence headers, media and configuration");

    ngx_memzero(&sink, sizeof(sink));

    CHECK(ngx_media_rtmp_publisher_init(&pub, NULL) == NGX_OK,
          "publisher initialised");

    /* AVC sequence header: FLV body with an avcC record */
    pos = 0;
    message[pos++] = 0x17;             /* keyframe, AVC */
    message[pos++] = NGX_MEDIA_RTMP_AVC_SEQUENCE;
    message[pos++] = 0; message[pos++] = 0; message[pos++] = 0;

    {
        ngx_media_rtmp_avcc_t  avcc;
        size_t                 record_len = 0;

        ngx_memzero(&avcc, sizeof(avcc));

        avcc.version = 1;
        avcc.profile = 0x42;
        avcc.compatibility = 0xC0;
        avcc.level = 0x1E;
        avcc.nal_length_size = 4;
        avcc.nsps = 1;
        avcc.sps[0].data = (u_char *) sps;
        avcc.sps[0].len = sizeof(sps);
        avcc.npps = 1;
        avcc.pps[0].data = (u_char *) pps;
        avcc.pps[0].len = sizeof(pps);

        CHECK(ngx_media_rtmp_avcc_build(&avcc, message + pos,
                                        sizeof(message) - pos,
                                        &record_len) == NGX_OK,
              "avcC for the sequence header");
        pos += record_len;
    }

    CHECK(ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_VIDEO, 0,
                                        message, pos, sink_frame,
                                        sink_tracks, &sink) == NGX_OK,
          "sequence header accepted");

    CHECK(sink.configs == 1, "one config frame: %lu", sink.configs);
    CHECK(sink.track_notifications == 1, "tracks announced");
    CHECK(sink.tracks.count == 1, "one track so far");
    CHECK(sink.tracks.tracks[0].media_type == NGX_MEDIA_TYPE_VIDEO,
          "video track");
    CHECK(sink.tracks.tracks[0].codec == NGX_MEDIA_CODEC_H264, "H.264");
    CHECK(sink.tracks.tracks[0].payload_format == NGX_MEDIA_PAYLOAD_ANNEXB,
          "Annex B like the TS path");
    CHECK(sink.tracks.tracks[0].profile == 0x42
          && sink.tracks.tracks[0].level == 0x1E, "profile and level");
    CHECK(sink.tracks.tracks[0].config != NULL
          && ngx_media_buf_size(sink.tracks.tracks[0].config)
             == 4 + sizeof(sps) + 4 + sizeof(pps),
          "config blob is SPS/PPS in Annex B");

    /* AAC sequence header: FLV body with an AudioSpecificConfig */
    message[0] = 0xAF;
    message[1] = NGX_MEDIA_RTMP_AAC_SEQUENCE;
    message[2] = 0x11;
    message[3] = 0x90;

    {
        ngx_int_t  rc = ngx_media_rtmp_publisher_feed(&pub,
                          NGX_MEDIA_RTMP_MSG_AUDIO, 0, message, 4, sink_frame,
                          sink_tracks, &sink);


        CHECK(rc == NGX_OK, "audio sequence header accepted");
    }

    CHECK(sink.tracks.count == 2, "two tracks: %lu", sink.tracks.count);
    CHECK(sink.tracks.tracks[1].media_type == NGX_MEDIA_TYPE_AUDIO,
          "audio track");
    CHECK(sink.tracks.tracks[1].codec == NGX_MEDIA_CODEC_AAC, "AAC");
    CHECK(sink.tracks.tracks[1].payload_format == NGX_MEDIA_PAYLOAD_ADTS,
          "ADTS like the TS path");
    CHECK(sink.tracks.tracks[1].sample_rate == 48000
          && sink.tracks.tracks[1].channels == 2, "rate and channels");

    /* a video message: two NAL units with a composition offset */
    pos = 0;
    message[pos++] = 0x27;             /* inter frame, AVC */
    message[pos++] = NGX_MEDIA_RTMP_AVC_NALU;
    /* composition time 40 ms, big endian 24 bit */
    message[pos++] = 0; message[pos++] = 0; message[pos++] = 40;

    {
        size_t  before = sink.frames;

        message[pos++] = 0; message[pos++] = 0; message[pos++] = 0;
        message[pos++] = 3;
        message[pos++] = 0x41; message[pos++] = 0x9A; message[pos++] = 0x01;

        CHECK(ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_VIDEO, 120,
                                            message, pos, sink_frame,
                                            sink_tracks, &sink) == NGX_OK,
              "video message accepted");
        CHECK(sink.frames == before + 1, "one frame produced");

        CHECK(sink.last.payload_format == NGX_MEDIA_PAYLOAD_ANNEXB,
              "payload converted to Annex B");
        CHECK(ngx_media_buf_size(sink.last.payload) == 4 + 3,
              "annexb payload length: %lu",
              ngx_media_buf_size(sink.last.payload));
        CHECK(ngx_media_buf_data(sink.last.payload)[0] == 0
              && ngx_media_buf_data(sink.last.payload)[3] == 1
              && ngx_media_buf_data(sink.last.payload)[4] == 0x41,
              "start code before the NAL unit");

        CHECK(sink.last.dts == 120 * NGX_MEDIA_RTMP_TIMESCALE,
              "dts from the message timestamp: %ld", (long) sink.last.dts);
        CHECK(sink.last.pts == (120 + 40) * NGX_MEDIA_RTMP_TIMESCALE,
              "pts includes the composition offset: %ld", (long) sink.last.pts);
        CHECK(sink.last.keyframe == 0, "inter frame is not a keyframe");
    }

    /* an audio message: raw AAC becomes ADTS */
    {
        size_t  before = sink.frames;

        message[0] = 0xAF;
        message[1] = NGX_MEDIA_RTMP_AAC_RAW;
        message[2] = 0x21; message[3] = 0x22; message[4] = 0x23;
        message[5] = 0x24;

        CHECK(ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_AUDIO, 160,
                                            message, 6, sink_frame,
                                            sink_tracks, &sink) == NGX_OK,
              "audio message accepted");
        CHECK(sink.frames == before + 1, "one audio frame produced");
        CHECK(sink.last.payload_format == NGX_MEDIA_PAYLOAD_ADTS,
              "ADTS framing added");
        CHECK(ngx_media_buf_size(sink.last.payload) == 7 + 4,
              "ADTS frame length: %lu", ngx_media_buf_size(sink.last.payload));

        {
            ngx_media_adts_t  parsed;

            CHECK(ngx_media_adts_parse(ngx_media_buf_data(sink.last.payload),
                                       ngx_media_buf_size(sink.last.payload),
                                       &parsed) == NGX_OK,
                  "generated ADTS header parses");
            CHECK(parsed.sample_rate == 48000, "sample rate: %lu",
                  parsed.sample_rate);
            CHECK(parsed.channels == 2, "channels: %lu", parsed.channels);
            CHECK(parsed.frame_len == 11, "frame length: %lu",
                  parsed.frame_len);
            CHECK(parsed.object_type == 2, "object type: %lu",
                  parsed.object_type);
        }

        CHECK(sink.last.pts == sink.last.dts, "audio pts equals dts");
        CHECK(sink.last.dts == 160 * NGX_MEDIA_RTMP_TIMESCALE, "audio dts");
    }

    /* media before configuration is skipped, not fatal */
    {
        ngx_media_rtmp_publisher_t  fresh;
        frame_sink_t                other;

        ngx_memzero(&other, sizeof(other));

        CHECK(ngx_media_rtmp_publisher_init(&fresh, NULL) == NGX_OK,
              "fresh publisher");

        message[0] = 0x27;
        message[1] = NGX_MEDIA_RTMP_AVC_NALU;
        message[2] = 0; message[3] = 0; message[4] = 0;
        message[5] = 0; message[6] = 0; message[7] = 0;
        message[8] = 2;
        message[9] = 0x41; message[10] = 0x01;

        CHECK(ngx_media_rtmp_publisher_feed(&fresh, NGX_MEDIA_RTMP_MSG_VIDEO, 0,
                                            message, 11, sink_frame,
                                            sink_tracks, &other) == NGX_OK,
              "video without a sequence header is tolerated");
        CHECK(other.frames == 0, "no frame produced: %lu", other.frames);
        CHECK(fresh.skipped == 1, "counted as skipped");

        ngx_media_rtmp_publisher_destroy(&fresh);
        ngx_media_trackset_destroy(&other.tracks);

        if (other.last_payload != NULL) {
            ngx_media_buf_unref(other.last_payload);
        }
    }

    /* a corrupt sequence header is rejected without breaking the session */
    {
        u_char  bad[16];

        bad[0] = 0x17;
        bad[1] = NGX_MEDIA_RTMP_AVC_SEQUENCE;
        bad[2] = bad[3] = bad[4] = 0;
        bad[5] = 9;   /* nonsense avcC */
        for (i = 6; i < sizeof(bad); i++) {
            bad[i] = 0xFF;
        }

        CHECK(ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_VIDEO, 0,
                                            bad, sizeof(bad), sink_frame,
                                            sink_tracks, &sink) == NGX_OK,
              "corrupt sequence header tolerated");
        CHECK(pub.errors == 1, "error counted: %lu", pub.errors);
    }

    if (sink.last_payload != NULL) {
        ngx_media_buf_unref(sink.last_payload);
    }

    ngx_media_trackset_destroy(&sink.tracks);
    ngx_media_rtmp_publisher_destroy(&pub);
}

static void
test_fanout(void)
{
    ngx_media_rtmp_fanout_t   fan;
    ngx_media_buf_t          *payload;
    const ngx_media_rtmp_media_t  *unit;
    ngx_uint_t                i;

    TEST_CASE("shared fanout ring");

    ngx_media_rtmp_fanout_init(&fan, 4, 0);

    CHECK(fan.capacity == 4, "capacity honoured");

    for (i = 0; i < 3; i++) {
        payload = ngx_media_buf_alloc(100);

        CHECK(payload != NULL, "payload allocated");
        (void) ngx_media_buf_freeze(payload, 100);

        CHECK(ngx_media_rtmp_fanout_push(&fan, NGX_MEDIA_RTMP_MSG_VIDEO, 0,
                                         i * 40, payload, i == 0, 0) == NGX_OK,
              "unit %lu pushed", i);
        ngx_media_buf_unref(payload);
    }

    CHECK(ngx_media_rtmp_fanout_head(&fan) == 3, "head advanced");

    unit = ngx_media_rtmp_fanout_next(&fan, 0);
    CHECK(unit != NULL && unit->sequence == 0, "read from the start");
    CHECK(unit != NULL && unit->type == NGX_MEDIA_RTMP_MSG_VIDEO, "type kept");
    CHECK(unit != NULL && unit->keyframe == 1, "keyframe flag kept");

    unit = ngx_media_rtmp_fanout_next(&fan, 2);
    CHECK(unit != NULL && unit->sequence == 2, "read from a cursor");
    CHECK(unit != NULL && unit->timestamp == 80, "timestamp kept");

    CHECK(ngx_media_rtmp_fanout_next(&fan, 3) == NULL, "past the head");

    /* overrun: the oldest unit is evicted and readers are told */
    for (i = 0; i < 3; i++) {
        payload = ngx_media_buf_alloc(100);
        (void) ngx_media_buf_freeze(payload, 100);

        CHECK(ngx_media_rtmp_fanout_push(&fan, NGX_MEDIA_RTMP_MSG_AUDIO, 1, 0,
                                         payload, 1, 0) == NGX_OK,
              "unit pushed");
        ngx_media_buf_unref(payload);
    }

    unit = ngx_media_rtmp_fanout_next(&fan, 0);
    CHECK(unit != NULL && unit->sequence >= fan.tail,
          "a stale cursor resumes at the oldest retained unit");

    ngx_media_rtmp_fanout_destroy(&fan);
}

/*
 * Item 15 of the definition of done: RTMP fanout shares payloads rather than
 * copying media per connection.
 *
 * A program frame becomes exactly one FLV unit in the shared ring.  Each
 * player then builds its own chunk headers around *that* buffer: the writer
 * slices the payload as a scatter/gather list and takes one reference, so a
 * connection costs headers and a reference, never a copy of the media.
 *
 * These are the assertions a per-connection copy cannot satisfy: every
 * player's packet points at the same payload allocation, every media slice
 * points inside it, the reference count grows by exactly one per player, and
 * the ring accounts the payload once however many players read it.
 */
static void
test_player_sharing(void)
{
    static ngx_media_rtmp_packet_t  packets[8];

    const ngx_uint_t                players = 8;
    ngx_media_rtmp_prepare_t        prep;
    ngx_media_rtmp_writer_t         writer;
    ngx_media_trackset_t            tracks;
    ngx_media_track_t               track;
    ngx_media_buf_t                *config, *payload;
    ngx_media_frame_t               frame;
    const ngx_media_rtmp_media_t   *unit;
    const u_char                   *base;
    size_t                          shared_len, retained;
    ngx_uint_t                      i, j, media_parts;
    size_t                          media_bytes;
    u_char                         *p;

    TEST_CASE("every rtmp player shares one payload; none copies it");

    ngx_media_rtmp_prepare_init(&prep, 8, 0);

    CHECK(ngx_media_trackset_init(&tracks, 4, NULL) == NGX_OK,
          "trackset initialised");

    config = ngx_media_buf_alloc(4 + sizeof(sps) + 4 + sizeof(pps));
    p = ngx_media_buf_data(config);
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1;
    ngx_memcpy(p + 4, sps, sizeof(sps));
    p[4 + sizeof(sps)] = 0;
    p[5 + sizeof(sps)] = 0;
    p[6 + sizeof(sps)] = 0;
    p[7 + sizeof(sps)] = 1;
    ngx_memcpy(p + 8 + sizeof(sps), pps, sizeof(pps));
    (void) ngx_media_buf_freeze(config, 4 + sizeof(sps) + 4 + sizeof(pps));

    ngx_memzero(&track, sizeof(track));
    track.media_type = NGX_MEDIA_TYPE_VIDEO;
    track.codec = NGX_MEDIA_CODEC_H264;
    track.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    track.config = config;

    CHECK(ngx_media_trackset_add(&tracks, &track) >= 0, "video track added");
    ngx_media_buf_unref(config);

    CHECK(ngx_media_rtmp_prepare_announce(&prep, &tracks) == NGX_OK,
          "sequence headers announced");

    /*
     * A media frame several chunks long, so the packet really is a
     * scatter/gather list of slices into the shared buffer rather than one
     * contiguous reference.
     */
    payload = ngx_media_buf_alloc(6 * 520);
    p = ngx_media_buf_data(payload);

    for (i = 0; i < 5; i++) {
        p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1;
        memset(p + 4, 0x41 + i, 512);
        p += 4 + 512;
    }

    (void) ngx_media_buf_freeze(payload, 6 * 520);

    ngx_media_frame_init(&frame);
    frame.media_type = NGX_MEDIA_TYPE_VIDEO;
    frame.codec = NGX_MEDIA_CODEC_H264;
    frame.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    frame.pts = frame.dts = 90 * 40;
    frame.keyframe = 1;
    ngx_media_frame_adopt(&frame, payload);

    CHECK(ngx_media_rtmp_prepare_frame(&prep, &frame) == NGX_OK,
          "program frame prepared");
    ngx_media_frame_release(&frame);

    /* unit 0 is the sequence header, unit 1 is the media frame */
    unit = ngx_media_rtmp_fanout_next(&prep.fan, 1);
    CHECK(unit != NULL && unit->config == 0, "the media unit");
    CHECK(unit != NULL && ngx_media_buf_size(unit->payload) > 128 * 4,
          "the media unit is several chunks long");

    if (unit == NULL) {
        ngx_media_rtmp_prepare_destroy(&prep);
        ngx_media_trackset_destroy(&tracks);
        return;
    }

    base = ngx_media_buf_data(unit->payload);
    shared_len = ngx_media_buf_size(unit->payload);
    retained = prep.fan.bytes;

    CHECK(ngx_media_buf_refs(unit->payload) == 1,
          "the ring holds the only reference before anyone reads it");

    ngx_media_rtmp_writer_init(&writer, NGX_MEDIA_RTMP_DEFAULT_CHUNK,
                               NGX_MEDIA_RTMP_MAX_MESSAGE);

    for (i = 0; i < players; i++) {
        ngx_media_rtmp_packet_init(&packets[i]);

        CHECK(ngx_media_rtmp_writer_message(&writer, &packets[i], 4,
                                            unit->type, 1, unit->timestamp,
                                            unit->payload, 0, shared_len)
              == NGX_OK, "player %lu built its packet", i);

        CHECK(packets[i].payload == unit->payload,
              "player %lu references the shared payload, not a copy", i);

        media_parts = 0;
        media_bytes = 0;

        for (j = 0; j < packets[i].nparts; j++) {
            const u_char  *d = packets[i].parts[j].data;
            size_t         l = packets[i].parts[j].len;

            if (d >= packets[i].head
                && d < packets[i].head + packets[i].head_len)
            {
                /* a chunk header, owned by this packet and this player */
                continue;
            }

            media_parts++;
            media_bytes += l;

            CHECK(d >= base && d + l <= base + shared_len,
                  "player %lu slice %lu lies inside the shared payload", i, j);
        }

        CHECK(media_parts > 4,
              "player %lu slices the shared buffer: %lu parts", i, media_parts);
        CHECK(media_bytes == shared_len,
              "player %lu carries every shared byte once", i);
        CHECK(ngx_media_buf_refs(unit->payload) == 1 + i + 1,
              "one reference per player, no copy: %lu players", i + 1);
    }

    /* reading is free: the ring still accounts the payload exactly once */
    CHECK(prep.fan.bytes == retained,
          "players added %lu bytes to the ring",
          (unsigned long) (prep.fan.bytes - retained));

    for (i = 0; i < players; i++) {
        ngx_media_rtmp_packet_destroy(&packets[i]);
    }

    CHECK(ngx_media_buf_refs(unit->payload) == 1,
          "the ring is the only holder once the players are gone");

    ngx_media_rtmp_prepare_destroy(&prep);
    ngx_media_trackset_destroy(&tracks);
}

static void
test_prepare(void)
{
    ngx_media_rtmp_prepare_t   prep;
    ngx_media_trackset_t       tracks;
    ngx_media_track_t          track;
    ngx_media_buf_t           *config;
    ngx_media_frame_t          frame;
    ngx_media_buf_t           *payload;
    const ngx_media_rtmp_media_t  *unit;
    u_char                    *p;

    TEST_CASE("program preparation for players");

    ngx_media_rtmp_prepare_init(&prep, 8, 0);

    /* the program contract: video (Annex B parameter sets) and audio (ASC) */
    CHECK(ngx_media_trackset_init(&tracks, 4, NULL) == NGX_OK,
          "trackset initialised");

    config = ngx_media_buf_alloc(4 + sizeof(sps) + 4 + sizeof(pps));
    p = ngx_media_buf_data(config);
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1;
    ngx_memcpy(p + 4, sps, sizeof(sps));
    p[4 + sizeof(sps)] = 0;
    p[5 + sizeof(sps)] = 0;
    p[6 + sizeof(sps)] = 0;
    p[7 + sizeof(sps)] = 1;
    ngx_memcpy(p + 8 + sizeof(sps), pps, sizeof(pps));
    (void) ngx_media_buf_freeze(config, 4 + sizeof(sps) + 4 + sizeof(pps));

    ngx_memzero(&track, sizeof(track));
    track.media_type = NGX_MEDIA_TYPE_VIDEO;
    track.codec = NGX_MEDIA_CODEC_H264;
    track.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    track.profile = sps[1];
    track.level = sps[3];
    track.config = config;

    CHECK(ngx_media_trackset_add(&tracks, &track) >= 0, "video track added");

    ngx_media_buf_unref(config);

    CHECK(ngx_media_rtmp_prepare_announce(&prep, &tracks) == NGX_OK,
          "sequence headers announced");

    unit = ngx_media_rtmp_fanout_next(&prep.fan, 0);
    CHECK(unit != NULL && unit->config == 1, "a sequence header unit");
    CHECK(unit != NULL && unit->type == NGX_MEDIA_RTMP_MSG_VIDEO,
          "video sequence header");

    if (unit != NULL) {
        const u_char  *b = ngx_media_buf_data(unit->payload);

        CHECK(ngx_media_buf_size(unit->payload) == 5 + 6 + 2 + sizeof(sps)
              + 1 + 2 + sizeof(pps), "avcC in the body: %lu",
              ngx_media_buf_size(unit->payload));
        CHECK(b[0] == 0x17, "keyframe, AVC: %02x", b[0]);
        CHECK(b[1] == NGX_MEDIA_RTMP_AVC_SEQUENCE, "sequence packet");
        CHECK(b[5] == 1 && b[6] == sps[1] && b[8] == sps[3],
              "avcC built from the SPS: %02x %02x %02x", b[5], b[6], b[8]);
    }

    /* a program video frame: Annex B becomes AVCC inside an FLV body */
    payload = ngx_media_buf_alloc(4 + 3);
    p = ngx_media_buf_data(payload);
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1;
    p[4] = 0x41; p[5] = 0x9A; p[6] = 0x11;
    (void) ngx_media_buf_freeze(payload, 7);

    ngx_media_frame_init(&frame);
    frame.media_type = NGX_MEDIA_TYPE_VIDEO;
    frame.codec = NGX_MEDIA_CODEC_H264;
    frame.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    frame.pts = 90 * 100;
    frame.dts = 90 * 80;
    frame.keyframe = 0;
    ngx_media_frame_adopt(&frame, payload);   /* takes over our reference */

    CHECK(ngx_media_rtmp_prepare_frame(&prep, &frame) == NGX_OK,
          "video frame prepared");

    unit = ngx_media_rtmp_fanout_next(&prep.fan, 1);
    CHECK(unit != NULL && unit->timestamp == 80, "timestamp in milliseconds");
    CHECK(unit != NULL && ngx_media_buf_size(unit->payload) == 5 + 4 + 3,
          "AVCC body length: %lu",
          unit != NULL ? ngx_media_buf_size(unit->payload) : 0);

    if (unit != NULL) {
        const u_char  *b = ngx_media_buf_data(unit->payload);

        CHECK(b[0] == 0x27, "inter frame, AVC: %02x", b[0]);
        CHECK(b[2] == 0 && b[3] == 0 && b[4] == 20,
              "composition time 20: %02x %02x %02x", b[2], b[3], b[4]);
        CHECK(b[5] == 0 && b[6] == 0 && b[7] == 0 && b[8] == 3,
              "length prefix instead of the start code");
        CHECK(b[9] == 0x41, "NAL bytes preserved");
    }

    ngx_media_frame_release(&frame);

    /* a program audio frame: the ADTS header is stripped */
    {
        ngx_media_adts_t  adts;
        u_char            adts_frame[16];
        size_t            adts_len = 0;

        ngx_memzero(&adts, sizeof(adts));
        adts.object_type = 2;
        adts.sample_rate = 48000;
        adts.channels = 2;
        adts.frame_len = 7 + 4;

        CHECK(ngx_media_adts_write(&adts, adts_frame, sizeof(adts_frame),
                                   &adts_len) == NGX_OK, "ADTS header written");
        CHECK(adts_len == 7, "seven byte header: %lu", adts_len);
        adts_frame[7] = 0xA1; adts_frame[8] = 0xA2;
        adts_frame[9] = 0xA3; adts_frame[10] = 0xA4;

        payload = ngx_media_buf_alloc(11);
        ngx_memcpy(ngx_media_buf_data(payload), adts_frame, 11);
        (void) ngx_media_buf_freeze(payload, 11);

        ngx_media_frame_init(&frame);
        frame.media_type = NGX_MEDIA_TYPE_AUDIO;
        frame.codec = NGX_MEDIA_CODEC_AAC;
        frame.payload_format = NGX_MEDIA_PAYLOAD_ADTS;
        frame.pts = frame.dts = 90 * 40;
        ngx_media_frame_adopt(&frame, payload);

        CHECK(ngx_media_rtmp_prepare_frame(&prep, &frame) == NGX_OK,
              "audio frame prepared");

        unit = ngx_media_rtmp_fanout_next(&prep.fan, 2);
        CHECK(unit != NULL && unit->type == NGX_MEDIA_RTMP_MSG_AUDIO,
              "audio unit");
        CHECK(unit != NULL && ngx_media_buf_size(unit->payload) == 2 + 4,
              "ADTS header stripped: %lu",
              unit != NULL ? ngx_media_buf_size(unit->payload) : 0);

        if (unit != NULL) {
            const u_char  *b = ngx_media_buf_data(unit->payload);

            CHECK(b[0] == 0xAF && b[1] == NGX_MEDIA_RTMP_AAC_RAW,
                  "FLV audio prefix: %02x %02x", b[0], b[1]);
            CHECK(b[2] == 0xA1 && b[5] == 0xA4, "raw AAC payload preserved");
        }

        ngx_media_frame_release(&frame);
    }

    ngx_media_rtmp_prepare_destroy(&prep);
    ngx_media_trackset_destroy(&tracks);
}

int
main(void)
{
    printf("== rtmp adapter\n");

    test_avcc();
    test_reframing();
    test_asc();
    test_flv_framing();
    test_publisher();
    test_enhanced_rtmp();
    test_fanout();
    test_player_sharing();
    test_prepare();

    TEST_LEAKS();
    TEST_MAIN_END();
}

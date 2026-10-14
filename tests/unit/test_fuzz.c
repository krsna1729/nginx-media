/*
 * Parser fuzzing: every parser that consumes bytes from the network must
 * survive arbitrary input.  Deterministic pseudo-random data and mutations of
 * valid inputs are fed to each parser; under ASan/UBSan a crash, an overrun or
 * a leak fails the test.  Parsers may reject, ignore or truncate - they may
 * never read out of bounds or allocate without bound.
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_ipc.h"
#include "ngx_media_rtmp_wire.h"
#include "ngx_media_srt_streamid.h"
#include "ngx_media_ts_demux.h"
#include "ngx_media_aac.h"
#include "ngx_media_nal.h"
#include "ngx_media_rtmp_adapter.h"
#include "ngx_media_ts_mux.h"

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

/* deterministic xorshift so a failure is reproducible */
static uint64_t  fuzz_state = 0x2545F4914F6CDD1DULL;

static uint32_t
fuzz_next(void)
{
    fuzz_state ^= fuzz_state << 13;
    fuzz_state ^= fuzz_state >> 7;
    fuzz_state ^= fuzz_state << 17;

    return (uint32_t) (fuzz_state >> 16);
}

/*
 * How many rounds each generator runs.  A nightly job wants far more than a
 * developer waiting on a build, so the multiplier comes from the
 * environment: NGX_MEDIA_FUZZ_SCALE=20 is twenty times the default work.
 *
 * Scale multiplies iterations only.  The length of any input a generator
 * produces is a property of its storage, never of the iteration count, and
 * every generator that assembles bytes by hand proves it with FUZZ_BUF_WRITE
 * so that an unbounded generator fails the suite instead of scribbling past
 * the buffer it was given.
 */
static ngx_uint_t  fuzz_scale = 1;

#define FUZZ_ROUNDS(n)  ((ngx_uint_t) (n) * fuzz_scale)

#define FUZZ_BUF_WRITE(buf, cap, off, need)                                   \
    do {                                                                      \
        if ((size_t) (off) + (size_t) (need) > (size_t) (cap)) {              \
            ngx_media_test_failures++;                                        \
            printf("FAIL %s:%d: generator write of %zu bytes at offset %zu "  \
                   "exceeds the %zu-byte buffer\n", __FILE__, __LINE__,       \
                   (size_t) (need), (size_t) (off), (size_t) (cap));          \
            return 0;                                                         \
        }                                                                     \
    } while (0)

static void
fuzz_fill(u_char *buf, size_t len)
{
    size_t  i;

    for (i = 0; i < len; i++) {
        buf[i] = (u_char) fuzz_next();
    }
}

/* mutates a valid input: bit flips, byte splices and truncations */
static size_t
fuzz_mutate(u_char *buf, size_t len)
{
    ngx_uint_t  rounds = 1 + (fuzz_next() % 8);
    ngx_uint_t  i;

    for (i = 0; i < rounds; i++) {
        uint32_t  op = fuzz_next() % 4;

        if (len == 0) {
            break;
        }

        if (op == 0) {
            buf[fuzz_next() % len] ^= (u_char) (1u << (fuzz_next() % 8));

        } else if (op == 1) {
            buf[fuzz_next() % len] = (u_char) fuzz_next();

        } else if (op == 2) {
            /* corrupt a length-like field */
            size_t  at = fuzz_next() % len;

            buf[at] = 0xFF;
            if (at + 1 < len) {
                buf[at + 1] = 0xFF;
            }

        } else {
            len = fuzz_next() % (len + 1);
        }
    }

    return len;
}

static ngx_int_t
fuzz_message(void *ctx, ngx_uint_t type, ngx_uint_t stream_id,
    uint32_t timestamp, ngx_media_buf_t *payload)
{
    ngx_uint_t  *count = ctx;

    (void) type;
    (void) stream_id;
    (void) timestamp;
    (void) payload;

    (*count)++;

    return NGX_OK;
}

static void
test_rtmp_reader(void)
{
    ngx_media_rtmp_reader_t  reader;
    ngx_uint_t               messages;
    u_char                   buf[4096];
    ngx_uint_t               i;
    size_t                   consumed;

    TEST_CASE("fuzz: rtmp chunk reader");

    for (i = 0; i < FUZZ_ROUNDS(2000); i++) {
        size_t  len = 1 + (fuzz_next() % sizeof(buf));

        fuzz_fill(buf, len);

        messages = 0;

        ngx_media_rtmp_reader_init(&reader);
        reader.max_message = 64 * 1024;   /* keep the bound tight */

        (void) ngx_media_rtmp_reader_feed(&reader, buf, len, &consumed,
                                          fuzz_message, &messages);

        /* whatever happened, the reader must stay consistent */
        CHECK(consumed <= len, "consumed within the input");

        ngx_media_rtmp_reader_reset(&reader);
    }

    /* and mutations of a valid stream */
    {
        ngx_media_rtmp_writer_t  writer;
        ngx_media_rtmp_packet_t  packet;
        ngx_media_buf_t         *payload = ngx_media_buf_alloc(512);
        u_char                   valid[2048];
        size_t                   valid_len = 0, part;

        CHECK(payload != NULL, "payload allocated");
        fuzz_fill(ngx_media_buf_data(payload), 512);
        (void) ngx_media_buf_freeze(payload, 512);

        ngx_media_rtmp_writer_init(&writer, 128, NGX_MEDIA_RTMP_MAX_MESSAGE);

        ngx_media_rtmp_packet_init(&packet);

        if (ngx_media_rtmp_writer_message(&writer, &packet, 4,
                                          NGX_MEDIA_RTMP_MSG_VIDEO, 1, 0,
                                          payload, 0, 512) == NGX_OK)
        {
            ngx_uint_t  p;

            for (p = 0; p < packet.nparts; p++) {

                if (valid_len + packet.parts[p].len > sizeof(valid)) {
                    break;
                }

                memcpy(valid + valid_len, packet.parts[p].data,
                       packet.parts[p].len);
                valid_len += packet.parts[p].len;
            }
        }

        ngx_media_rtmp_packet_destroy(&packet);
        ngx_media_buf_unref(payload);

        CHECK(valid_len > 0, "a valid stream was produced");

        for (i = 0; i < FUZZ_ROUNDS(2000) && valid_len > 0; i++) {
            u_char  copy[2048];

            memcpy(copy, valid, valid_len);

            part = fuzz_mutate(copy, valid_len);

            messages = 0;

            ngx_media_rtmp_reader_init(&reader);
            reader.max_message = 64 * 1024;

            (void) ngx_media_rtmp_reader_feed(&reader, copy, part, &consumed,
                                              fuzz_message, &messages);

            ngx_media_rtmp_reader_reset(&reader);
        }
    }
}

/*
 * Builds `depth` nested AMF0 objects, each with one member, terminated at
 * every level: 4 bytes of object header and 3 bytes of object end per level,
 * plus a 4-byte string as the innermost value.
 *
 * The depth is an argument, not a loop count, so the input can never outgrow
 * the buffer that holds it however many rounds the caller runs.
 */
static size_t
fuzz_amf_nest(u_char *buf, size_t cap, size_t depth)
{
    size_t  i, pos = 0;

    for (i = 0; i < depth; i++) {
        FUZZ_BUF_WRITE(buf, cap, pos, 4);

        buf[pos++] = NGX_MEDIA_AMF_OBJECT;
        buf[pos++] = 0;
        buf[pos++] = 1;
        buf[pos++] = 'a';
    }

    FUZZ_BUF_WRITE(buf, cap, pos, 4);

    buf[pos++] = NGX_MEDIA_AMF_STRING;
    buf[pos++] = 0;
    buf[pos++] = 1;
    buf[pos++] = 'x';

    for (i = 0; i < depth; i++) {
        FUZZ_BUF_WRITE(buf, cap, pos, 3);

        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = NGX_MEDIA_AMF_OBJECT_END;
    }

    return pos;
}

#define AMF_NEST_LEVEL_BYTES  7   /* 4 header + 3 terminator */
#define AMF_NEST_LEAF_BYTES   4   /* the innermost string value */

/* The deepest input this buffer can hold: never derived from fuzz_scale. */
#define AMF_NEST_CAPACITY                                                     \
    ((sizeof(amf_nest) - AMF_NEST_LEAF_BYTES) / AMF_NEST_LEVEL_BYTES)

static void
test_amf(void)
{
    ngx_media_amf_value_t  value;
    u_char                 buf[512];
    u_char                 amf_nest[8 * 1024];
    ngx_uint_t             i;
    size_t                 consumed;

    /*
     * The generator must be able to build inputs deeper than the parser
     * accepts, otherwise the rejection path is never exercised.
     */
    _Static_assert(AMF_NEST_CAPACITY > NGX_MEDIA_AMF_MAX_DEPTH,
                   "the deep-nesting buffer must encode more levels than the "
                   "parser accepts");

    TEST_CASE("fuzz: amf0 decoder");

    for (i = 0; i < FUZZ_ROUNDS(4000); i++) {
        size_t  len = 1 + (fuzz_next() % sizeof(buf));

        fuzz_fill(buf, len);

        (void) ngx_media_amf_read(buf, len, &value, &consumed);

        CHECK(consumed <= len, "consumed within the input");
    }

    /*
     * Deep nesting must be rejected rather than recursing without bound.  The
     * depths are the parser's limit and its immediate neighbours: at the limit
     * the input is well formed and must be accepted, one level past it the
     * parser must refuse instead of recursing, and the deepest input the
     * buffer can hold must be refused too.  Every round draws another depth
     * from the whole encodable range, so coverage grows with the iteration
     * count while the generated input stays inside amf_nest.
     */
    for (i = 0; i < FUZZ_ROUNDS(64); i++) {
        size_t     depth = 1 + (fuzz_next() % AMF_NEST_CAPACITY);
        size_t     len;
        ngx_int_t  rc;

        len = fuzz_amf_nest(amf_nest, sizeof(amf_nest), depth);
        rc = ngx_media_amf_read(amf_nest, len, &value, &consumed);

        CHECK(consumed <= len, "consumed within the nesting input");
        CHECK(rc == (depth <= NGX_MEDIA_AMF_MAX_DEPTH ? NGX_OK : NGX_ERROR),
              "depth %zu: expected %s, got %ld", depth,
              depth <= NGX_MEDIA_AMF_MAX_DEPTH ? "NGX_OK" : "NGX_ERROR",
              (long) rc);
    }

    {
        static const size_t  boundary[] = {
            NGX_MEDIA_AMF_MAX_DEPTH - 1,
            NGX_MEDIA_AMF_MAX_DEPTH,
            NGX_MEDIA_AMF_MAX_DEPTH + 1,
            AMF_NEST_CAPACITY,
        };

        for (i = 0; i < sizeof(boundary) / sizeof(boundary[0]); i++) {
            size_t     len = fuzz_amf_nest(amf_nest, sizeof(amf_nest),
                                           boundary[i]);
            ngx_int_t  rc = ngx_media_amf_read(amf_nest, len, &value,
                                               &consumed);

            CHECK(consumed <= len, "consumed within the boundary input");
            CHECK(rc == (boundary[i] <= NGX_MEDIA_AMF_MAX_DEPTH
                             ? NGX_OK : NGX_ERROR),
                  "boundary depth %zu: got %ld", boundary[i], (long) rc);
        }
    }
}

static void
test_srt_streamid(void)
{
    ngx_media_srt_streamid_t  id;
    u_char                    buf[512];
    ngx_uint_t                i;

    TEST_CASE("fuzz: srt stream id parser");

    for (i = 0; i < FUZZ_ROUNDS(4000); i++) {
        size_t  len = fuzz_next() % sizeof(buf);

        fuzz_fill(buf, len);

        (void) ngx_media_srt_streamid_parse(buf, len, &id);
    }

    /* mutations of a valid stream id */
    {
        const char  *valid = "#!::r=live/news,m=publish,s=encoder-a";
        size_t       valid_len = strlen(valid);

        for (i = 0; i < FUZZ_ROUNDS(2000); i++) {
            u_char  copy[128];

            memcpy(copy, valid, valid_len);

            (void) ngx_media_srt_streamid_parse(copy,
                                                fuzz_mutate(copy, valid_len),
                                                &id);
        }
    }
}

static void
fuzz_ts_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_uint_t  *count = ctx;

    (void) frame;

    (*count)++;
}
static void
fuzz_ts_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_uint_t *count = ctx;
    (void) tracks;
    if (count != NULL) {
        count[1]++;
    }
}

static size_t
build_valid_ts(u_char *out, size_t cap)
{
    ngx_media_ts_mux_conf_t  mconf;
    ngx_media_ts_mux_t       mux;
    ngx_media_trackset_t     tracks;
    ngx_media_track_t        t;
    ngx_media_ts_burst_t     burst;
    ngx_media_frame_t        vframe, aframe;
    ngx_media_buf_t         *vbuf, *abuf;
    size_t                   burst_sz = 0;

    ngx_media_ts_mux_conf_default(&mconf);
    if (ngx_media_ts_mux_init(&mux, &mconf, NULL) != NGX_OK) {
        return 0;
    }

    if (ngx_media_trackset_init(&tracks, 2, NULL) != NGX_OK) {
        ngx_media_ts_mux_destroy(&mux);
        return 0;
    }

    ngx_memzero(&t, sizeof(t));
    t.media_type = NGX_MEDIA_TYPE_VIDEO;
    t.codec = NGX_MEDIA_CODEC_H264;
    (void) ngx_media_trackset_add(&tracks, &t);

    t.media_type = NGX_MEDIA_TYPE_AUDIO;
    t.codec = NGX_MEDIA_CODEC_AAC;
    (void) ngx_media_trackset_add(&tracks, &t);

    (void) ngx_media_ts_mux_set_tracks(&mux, &tracks);

    if (ngx_media_ts_mux_burst_init(&mux, &burst, cap) == NGX_OK) {
        static const u_char au[] = {
            0x00, 0x00, 0x00, 0x01, 0x09, 0xF0,
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E,
            0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80,
            0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21
        };
        vbuf = ngx_media_buf_alloc(sizeof(au));
        if (vbuf != NULL) {
            memcpy(ngx_media_buf_data(vbuf), au, sizeof(au));
            (void) ngx_media_buf_freeze(vbuf, sizeof(au));
            ngx_media_frame_init(&vframe);
            vframe.media_type = NGX_MEDIA_TYPE_VIDEO;
            vframe.codec = NGX_MEDIA_CODEC_H264;
            vframe.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
            vframe.keyframe = 1;
            vframe.pts = 90000;
            vframe.dts = 90000;
            vframe.payload = vbuf;
            (void) ngx_media_ts_mux_write_frame(&mux, &burst, &vframe, 1);
            ngx_media_buf_unref(vbuf);
        }

        static const u_char adts[] = {
            0xFF, 0xF1, 0x50, 0x80, 0x01, 0x3F, 0xFC, 0x21, 0x43
        };
        abuf = ngx_media_buf_alloc(sizeof(adts));
        if (abuf != NULL) {
            memcpy(ngx_media_buf_data(abuf), adts, sizeof(adts));
            (void) ngx_media_buf_freeze(abuf, sizeof(adts));
            ngx_media_frame_init(&aframe);
            aframe.media_type = NGX_MEDIA_TYPE_AUDIO;
            aframe.codec = NGX_MEDIA_CODEC_AAC;
            aframe.payload_format = NGX_MEDIA_PAYLOAD_ADTS;
            aframe.keyframe = 1;
            aframe.pts = 90000;
            aframe.dts = 90000;
            aframe.payload = abuf;
            (void) ngx_media_ts_mux_write_frame(&mux, &burst, &aframe, 0);
            ngx_media_buf_unref(abuf);
        }

        (void) ngx_media_ts_mux_burst_end(&mux, &burst);
        burst_sz = ngx_media_ts_burst_size(&burst);
        if (burst_sz <= cap && burst.backing != NULL) {
            memcpy(out, ngx_media_buf_data(burst.backing), burst_sz);
        } else {
            burst_sz = 0;
        }
        ngx_media_ts_mux_burst_destroy(&burst);
    }

    ngx_media_trackset_destroy(&tracks);
    ngx_media_ts_mux_destroy(&mux);
    return burst_sz;
}
static void
test_ts_demux(void)
{
    ngx_media_ts_demux_conf_t  conf;
    ngx_media_ts_sink_t        sink;
    ngx_media_ts_demux_t       demux;
    ngx_media_ts_demux_stats_t stats;
    u_char                     buf[4096];
    ngx_uint_t                 sink_counts[2], i;

    TEST_CASE("fuzz: mpeg-ts demux");

    conf.max_tracks = 8;
    conf.max_au_bytes = 256 * 1024;

    sink.tracks = fuzz_ts_tracks;
    sink.frame = fuzz_ts_frame;

    for (i = 0; i < FUZZ_ROUNDS(300); i++) {
        size_t  len = 188 * (1 + (fuzz_next() % 20));

        fuzz_fill(buf, len);

        sink_counts[0] = 0;
        sink_counts[1] = 0;

        if (ngx_media_ts_demux_init(&demux, &conf, &sink, sink_counts, NULL)
            != NGX_OK)
        {
            continue;
        }

        (void) ngx_media_ts_demux_feed(&demux, buf, len);
        (void) ngx_media_ts_demux_flush(&demux);
        ngx_media_ts_demux_stats(&demux, &stats);

        /* random bytes must never produce a valid frame */
        CHECK(sink_counts[0] == 0, "random ts produced %lu frames", sink_counts[0]);

        ngx_media_ts_demux_destroy(&demux);
    }

    /* valid PAT/PMT/PES, AVC and ADTS stream seeds and stateful partitions */
    {
        u_char  valid[4096];
        size_t  valid_len = build_valid_ts(valid, sizeof(valid));
        static const size_t chunk_sizes[] = { 1, 7, 64, 187, 188, 189, 512 };

        CHECK(valid_len >= 188 * 3, "valid ts burst generated: %lu", valid_len);

        for (size_t s = 0; s < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); s++) {
            size_t      chunk = chunk_sizes[s];
            ngx_uint_t  sink_counts[2] = { 0, 0 };

            if (ngx_media_ts_demux_init(&demux, &conf, &sink, sink_counts, NULL)
                == NGX_OK)
            {
                for (size_t at = 0; at < valid_len; at += chunk) {
                    size_t  step = (at + chunk <= valid_len) ? chunk : (valid_len - at);
                    (void) ngx_media_ts_demux_feed(&demux, valid + at, step);
                }
                ngx_media_ts_demux_flush(&demux);

                CHECK(sink_counts[0] > 0, "chunk %lu produced frames: %lu",
                      chunk, sink_counts[0]);
                CHECK(sink_counts[1] > 0, "chunk %lu notified tracks: %lu",
                      chunk, sink_counts[1]);

                ngx_media_ts_demux_destroy(&demux);
            }
        }

        for (i = 0; i < FUZZ_ROUNDS(2000) && valid_len > 0; i++) {
            u_char      copy[4096];
            ngx_uint_t  sink_counts[2] = { 0, 0 };
            size_t      mutated_len, chunk, at;

            memcpy(copy, valid, valid_len);
            mutated_len = fuzz_mutate(copy, valid_len);
            chunk = 1 + (fuzz_next() % 256);

            if (ngx_media_ts_demux_init(&demux, &conf, &sink, sink_counts, NULL)
                == NGX_OK)
            {
                for (at = 0; at < mutated_len; at += chunk) {
                    size_t  step = (at + chunk <= mutated_len)
                                       ? chunk : (mutated_len - at);
                    (void) ngx_media_ts_demux_feed(&demux, copy + at, step);
                }
                ngx_media_ts_demux_flush(&demux);
                ngx_media_ts_demux_destroy(&demux);
            }
        }
    }
}

static void
test_ipc_reassembly(void)
{
    ngx_media_ipc_frame_t    frame;
    ngx_media_ipc_message_t  message;
    u_char                   buf[256];
    ngx_uint_t               i;

    TEST_CASE("fuzz: ipc frame reassembly");

    memset(&frame, 0, sizeof(frame));

    for (i = 0; i < FUZZ_ROUNDS(4000); i++) {
        size_t  len = fuzz_next() % sizeof(buf);

        fuzz_fill(buf, len);

        memset(&message, 0, sizeof(message));

        message.header.total = fuzz_next() % (NGX_MEDIA_IPC_MAX_FRAME + 1);
        message.header.offset = fuzz_next() % (len + 1);
        message.header.flags = (fuzz_next() & 1) ? NGX_MEDIA_IPC_FLAG_MORE : 0;
        message.length = len;

        if (len > 0) {
            ngx_media_buf_t  *payload = ngx_media_buf_alloc(len);

            CHECK(payload != NULL, "payload allocated");
            memcpy(ngx_media_buf_data(payload), buf, len);
            (void) ngx_media_buf_freeze(payload, len);

            message.payload = payload;
        }

        (void) ngx_media_ipc_frame_feed(&frame, &message);

        if (message.payload != NULL) {
            ngx_media_buf_unref(message.payload);
        }

        /* the reassembler must never hold more than one frame's bound */
        CHECK(frame.capacity <= NGX_MEDIA_IPC_MAX_FRAME,
              "reassembly bound respected: %lu", frame.capacity);
    }

    ngx_media_ipc_frame_reset(&frame);
}

static ngx_int_t
fuzz_pub_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_uint_t *count = ctx;
    (void) frame;
    if (count != NULL) {
        (*count)++;
    }
    return NGX_OK;
}

static ngx_int_t
fuzz_pub_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    (void) ctx;
    (void) tracks;
    return NGX_OK;
}

static void
test_flv_publisher(void)
{
    ngx_media_rtmp_publisher_t  pub;
    ngx_media_rtmp_avcc_t       avcc;
    ngx_media_rtmp_asc_t        asc;
    u_char                      avcc_rec[128], asc_rec[16];
    size_t                      avcc_len = 0, asc_len = 0;
    u_char                      vtag[256], atag[64];
    ngx_uint_t                  frames = 0, i;

    TEST_CASE("fuzz: flv publisher adapter");

    static const u_char sps[] = { 0x67, 0x42, 0xC0, 0x1E, 0xD9 };
    static const u_char pps[] = { 0x68, 0xCE, 0x3C, 0x80 };

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

    CHECK(ngx_media_rtmp_avcc_build(&avcc, avcc_rec, sizeof(avcc_rec),
                                    &avcc_len) == NGX_OK,
          "avcC built");

    ngx_memzero(&asc, sizeof(asc));
    asc.object_type = 2;
    asc.sample_rate_index = 3;
    asc.sample_rate = 48000;
    asc.channels = 2;
    CHECK(ngx_media_rtmp_asc_build(&asc, asc_rec, sizeof(asc_rec),
                                    &asc_len) == NGX_OK,
          "ASC built");

    vtag[0] = (NGX_MEDIA_RTMP_FRAME_KEYFRAME << 4) | NGX_MEDIA_RTMP_CODEC_AVC;
    vtag[1] = NGX_MEDIA_RTMP_AVC_SEQUENCE;
    vtag[2] = vtag[3] = vtag[4] = 0;
    memcpy(vtag + 5, avcc_rec, avcc_len);
    size_t vtag_len = 5 + avcc_len;

    atag[0] = 0xAF;
    atag[1] = NGX_MEDIA_RTMP_AAC_SEQUENCE;
    memcpy(atag + 2, asc_rec, asc_len);
    size_t atag_len = 2 + asc_len;

    CHECK(ngx_media_rtmp_publisher_init(&pub, NULL) == NGX_OK, "pub init");
    CHECK(ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_VIDEO, 0,
                                        vtag, vtag_len,
                                        fuzz_pub_frame, fuzz_pub_tracks,
                                        &frames) == NGX_OK,
          "vtag feed");
    CHECK(ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_AUDIO, 0,
                                        atag, atag_len,
                                        fuzz_pub_frame, fuzz_pub_tracks,
                                        &frames) == NGX_OK,
          "atag feed");
    CHECK(pub.tracks.count == 2, "two tracks registered: %lu", pub.tracks.count);
    ngx_media_rtmp_publisher_destroy(&pub);

    for (i = 0; i < FUZZ_ROUNDS(2000); i++) {
        u_char  vcopy[256], acopy[64];
        size_t  mv_len, ma_len;

        memcpy(vcopy, vtag, vtag_len);
        memcpy(acopy, atag, atag_len);
        mv_len = fuzz_mutate(vcopy, vtag_len);
        ma_len = fuzz_mutate(acopy, atag_len);
        frames = 0;

        if (ngx_media_rtmp_publisher_init(&pub, NULL) == NGX_OK) {
            (void) ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_VIDEO,
                                                fuzz_next() % 1000,
                                                vcopy, mv_len,
                                                fuzz_pub_frame, fuzz_pub_tracks,
                                                &frames);
            (void) ngx_media_rtmp_publisher_feed(&pub, NGX_MEDIA_RTMP_MSG_AUDIO,
                                                fuzz_next() % 1000,
                                                acopy, ma_len,
                                                fuzz_pub_frame, fuzz_pub_tracks,
                                                &frames);
            ngx_media_rtmp_publisher_destroy(&pub);
        }
    }
}

int
main(void)
{
    {
        const char  *env = getenv("NGX_MEDIA_FUZZ_SCALE");

        if (env != NULL && atoi(env) > 0) {
            fuzz_scale = (ngx_uint_t) atoi(env);
        }
    }

    printf("== parser fuzzing (scale %lu)\n", (unsigned long) fuzz_scale);

    test_rtmp_reader();
    test_amf();
    test_srt_streamid();
    test_ts_demux();
    test_ipc_reassembly();
    test_flv_publisher();

    TEST_LEAKS();
    TEST_MAIN_END();
}

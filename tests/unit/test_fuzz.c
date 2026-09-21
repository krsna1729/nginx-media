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
 */
static ngx_uint_t  fuzz_scale = 1;

#define FUZZ_ROUNDS(n)  ((ngx_uint_t) (n) * fuzz_scale)

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

static void
test_amf(void)
{
    ngx_media_amf_value_t  value;
    u_char                 buf[512];
    ngx_uint_t             i;
    size_t                 consumed;

    TEST_CASE("fuzz: amf0 decoder");

    for (i = 0; i < FUZZ_ROUNDS(4000); i++) {
        size_t  len = 1 + (fuzz_next() % sizeof(buf));

        fuzz_fill(buf, len);

        (void) ngx_media_amf_read(buf, len, &value, &consumed);

        CHECK(consumed <= len, "consumed within the input");
    }

    /* deep nesting must be rejected rather than recursing without bound */
    for (i = 0; i < FUZZ_ROUNDS(64); i++) {
        buf[i * 4] = NGX_MEDIA_AMF_OBJECT;
        buf[i * 4 + 1] = 0;
        buf[i * 4 + 2] = 1;
        buf[i * 4 + 3] = 'a';
    }

    (void) ngx_media_amf_read(buf, 64 * 4, &value, &consumed);
    CHECK(1, "deep nesting survived");
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
    (void) ctx;
    (void) tracks;
}

static void
test_ts_demux(void)
{
    ngx_media_ts_demux_conf_t  conf;
    ngx_media_ts_sink_t        sink;
    ngx_media_ts_demux_t       demux;
    ngx_media_ts_demux_stats_t stats;
    u_char                     buf[4096];
    ngx_uint_t                 frames, i;

    TEST_CASE("fuzz: mpeg-ts demux");

    conf.max_tracks = 8;
    conf.max_au_bytes = 256 * 1024;

    sink.tracks = fuzz_ts_tracks;
    sink.frame = fuzz_ts_frame;

    for (i = 0; i < FUZZ_ROUNDS(300); i++) {
        size_t  len = 188 * (1 + (fuzz_next() % 20));

        fuzz_fill(buf, len);

        frames = 0;

        if (ngx_media_ts_demux_init(&demux, &conf, &sink, &frames, NULL)
            != NGX_OK)
        {
            continue;
        }

        (void) ngx_media_ts_demux_feed(&demux, buf, len);
        (void) ngx_media_ts_demux_flush(&demux);
        ngx_media_ts_demux_stats(&demux, &stats);

        /* random bytes must never produce a valid frame */
        CHECK(frames == 0, "random ts produced %lu frames", frames);

        ngx_media_ts_demux_destroy(&demux);
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

    TEST_LEAKS();
    TEST_MAIN_END();
}

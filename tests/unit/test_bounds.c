/*
 * Boundary and allocation-failure behaviour.
 *
 * Every bound in the system is exercised exactly at its limit and one step
 * past it: the limit is accepted, the step past it is rejected or handled
 * without unbounded growth.  Allocations are also driven to failure through
 * the shim so the callers' NULL handling is proven rather than assumed.
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_feed.h"
#include "ngx_media_hls_segmenter.h"
#include "ngx_media_ipc.h"
#include "ngx_media_rtmp_wire.h"
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

static ngx_int_t
count_message(void *ctx, ngx_uint_t type, ngx_uint_t stream_id,
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

static ngx_media_buf_t *
payload(size_t len)
{
    ngx_media_buf_t  *buf = ngx_media_buf_alloc(len);

    if (buf == NULL) {
        return NULL;
    }

    memset(ngx_media_buf_data(buf), 0x47, len);
    (void) ngx_media_buf_freeze(buf, len);

    return buf;
}

static void
test_ipc_limits(void)
{
    ngx_media_ipc_endpoint_t  *local = NULL, *peer = NULL;
    ngx_media_ipc_frame_t      frame;
    ngx_media_ipc_message_t    message;
    ngx_media_ipc_header_t     header;
    ngx_media_buf_t           *buf;
    ngx_int_t                  rc;

    TEST_CASE("ipc: exactly the frame limit is accepted, one past is not");

    CHECK(ngx_media_ipc_pair_create(&local, &peer, NULL) == NGX_OK,
          "socket pair created");

    memset(&header, 0, sizeof(header));
    header.version = NGX_MEDIA_IPC_VERSION;
    header.type = NGX_MEDIA_IPC_MSG_VIDEO;

    /* a frame one byte past the reassembly bound must be refused */
    memset(&frame, 0, sizeof(frame));
    memset(&message, 0, sizeof(message));

    message.header.total = NGX_MEDIA_IPC_MAX_FRAME + 1;
    message.header.offset = 0;
    message.length = 0;

    CHECK(ngx_media_ipc_frame_feed(&frame, &message) == NGX_ERROR,
          "oversized frame refused");
    CHECK(frame.active == 0 && frame.payload == NULL,
          "nothing was allocated for it");

    /* a frame exactly at the bound is accepted */
    memset(&frame, 0, sizeof(frame));
    memset(&message, 0, sizeof(message));

    message.header.total = NGX_MEDIA_IPC_MAX_FRAME;
    message.header.offset = 0;
    message.header.flags = NGX_MEDIA_IPC_FLAG_MORE;
    message.length = 0;

    CHECK(ngx_media_ipc_frame_feed(&frame, &message) == NGX_AGAIN,
          "frame at the bound accepted");
    CHECK(frame.capacity == NGX_MEDIA_IPC_MAX_FRAME,
          "capacity equals the bound: %lu", frame.capacity);

    ngx_media_ipc_frame_reset(&frame);

    /* a chunk one byte past the datagram payload is split, not rejected */
    {
        size_t  len = NGX_MEDIA_IPC_MAX_PAYLOAD + 1;

        buf = payload(len);
        CHECK(buf != NULL, "payload allocated");

        rc = ngx_media_ipc_send(peer, &header, buf, 0, len);
        CHECK(rc == NGX_OK || rc == NGX_AGAIN,
              "oversized chunk handled: %ld", (long) rc);

        ngx_media_buf_unref(buf);
    }

    /* a payload larger than the message bound is refused, not truncated */
    buf = payload(128);
    CHECK(buf != NULL, "payload allocated");

    rc = ngx_media_ipc_send(peer, &header, buf, 0, NGX_MEDIA_IPC_MAX_FRAME + 1);
    CHECK(rc == NGX_ERROR, "over-length send refused: %ld", (long) rc);

    ngx_media_buf_unref(buf);

    ngx_media_ipc_close(local);
    ngx_media_ipc_close(peer);
}

static void
test_rtmp_message_limit(void)
{
    ngx_media_rtmp_reader_t  reader;
    ngx_media_rtmp_writer_t  writer;
    ngx_media_rtmp_packet_t  packet;
    ngx_media_buf_t         *buf;
    u_char                   wire[4096];
    size_t                   wire_len = 0, consumed;
    ngx_uint_t               messages, i;

    TEST_CASE("rtmp: the message bound is enforced at the reader");

    /* a message exactly at the reader's bound is delivered */
    buf = payload(1024);
    CHECK(buf != NULL, "payload allocated");

    /* the reader keeps its default chunk size, so the writer uses it too */
    ngx_media_rtmp_writer_init(&writer, NGX_MEDIA_RTMP_DEFAULT_CHUNK,
                               NGX_MEDIA_RTMP_MAX_MESSAGE);
    ngx_media_rtmp_packet_init(&packet);

    CHECK(ngx_media_rtmp_writer_message(&writer, &packet, 4,
                                        NGX_MEDIA_RTMP_MSG_VIDEO, 1, 0, buf, 0,
                                        1024) == NGX_OK, "message written");

    for (i = 0; i < packet.nparts; i++) {

        if (wire_len + packet.parts[i].len > sizeof(wire)) {
            break;
        }

        memcpy(wire + wire_len, packet.parts[i].data, packet.parts[i].len);
        wire_len += packet.parts[i].len;
    }

    ngx_media_rtmp_packet_destroy(&packet);

    messages = 0;
    ngx_media_rtmp_reader_init(&reader);
    reader.max_message = 1024;   /* exactly the message size */

    CHECK(ngx_media_rtmp_reader_feed(&reader, wire, wire_len, &consumed,
                                     count_message, &messages) == NGX_OK,
          "message at the bound accepted");
    CHECK(messages == 1, "delivered: %lu", messages);

    ngx_media_rtmp_reader_reset(&reader);

    /* the same message with a tighter bound is rejected */
    messages = 0;
    ngx_media_rtmp_reader_init(&reader);
    reader.max_message = 1023;

    CHECK(ngx_media_rtmp_reader_feed(&reader, wire, wire_len, &consumed,
                                     count_message, &messages) == NGX_ERROR,
          "message past the bound rejected");
    CHECK(messages == 0, "nothing delivered: %lu", messages);

    ngx_media_rtmp_reader_reset(&reader);
    ngx_media_buf_unref(buf);
}

static void
test_allocation_failure(void)
{
    ngx_media_buf_t  *buf;

    TEST_CASE("allocation failure is reported, never assumed away");

    /* the shim fails every allocation while the flag is set */
    ngx_media_test_fail_alloc = 1;

    buf = ngx_media_buf_alloc(64);
    CHECK(buf == NULL, "buffer allocation failure reported");

    {
        ngx_media_ipc_endpoint_t  *local = NULL, *peer = NULL;
        ngx_media_rtmp_reader_t    reader;
        u_char                     wire[64];

        CHECK(ngx_media_ipc_pair_create(&local, &peer, NULL) == NGX_ERROR,
              "endpoint allocation failure reported");

        /* fmt 0, csid 3, timestamp 0, length 4, type 8, stream id 1 */
        memset(wire, 0, sizeof(wire));
        wire[0] = 0x03;
        wire[4] = 0x00; wire[5] = 0x00; wire[6] = 0x04;
        wire[7] = NGX_MEDIA_RTMP_MSG_AUDIO;
        wire[8] = 0x00; wire[9] = 0x00; wire[10] = 0x00; wire[11] = 0x01;

        ngx_media_rtmp_reader_init(&reader);

        CHECK(ngx_media_rtmp_reader_feed(&reader, wire, sizeof(wire), &(size_t){0},
                                         count_message, &(ngx_uint_t){0})
              == NGX_ERROR,
              "reader reports a failed payload allocation");

        ngx_media_rtmp_reader_reset(&reader);

        ngx_media_ipc_close(local);
        ngx_media_ipc_close(peer);
    }

    ngx_media_test_fail_alloc = 0;

    buf = ngx_media_buf_alloc(64);
    CHECK(buf != NULL, "allocations work again");

    if (buf != NULL) {
        ngx_media_buf_unref(buf);
    }
}

/*
 * Bounded program feed: the ring is a fixed window.  A producer that runs far
 * ahead of its consumer evicts the oldest units, the retained window never
 * exceeds the unit, byte or age ceiling, and the consumer is told it overran
 * rather than handed a partial window (goal doc 13, 34 items 11 and 17).
 */

static void
feed_frame(ngx_media_frame_t *frame, int64_t dts, ngx_media_buf_t *buf)
{
    ngx_media_frame_init(frame);

    frame->media_type = NGX_MEDIA_TYPE_VIDEO;
    frame->codec = NGX_MEDIA_CODEC_H264;
    frame->payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    frame->pts = dts;
    frame->dts = dts;
    frame->payload = buf;   /* the frame owns the caller's reference */
}

static ngx_int_t
feed_publish(ngx_media_feed_t *feed, size_t len, int64_t dts, ngx_msec_t now)
{
    ngx_media_frame_t  frame;
    ngx_media_buf_t   *buf;
    ngx_int_t          rc;

    buf = payload(len);

    if (buf == NULL) {
        return NGX_ERROR;
    }

    feed_frame(&frame, dts, buf);
    rc = ngx_media_feed_publish(feed, &frame, now);
    ngx_media_frame_release(&frame);

    return rc;
}

static void
test_feed_ceilings(void)
{
    ngx_media_feed_t        feed;
    ngx_media_feed_conf_t   conf;
    ngx_media_cursor_t      cursor;
    ngx_media_frame_t       out[64];
    ngx_uint_t              i, count;
    uint64_t                frames;

    TEST_CASE("feed: a producer ahead of its consumer is evicted at the unit "
              "ceiling, never unbounded");

    conf.max_units = 8;
    conf.max_bytes = 0;
    conf.max_age = 0;

    CHECK(ngx_media_feed_init(&feed, &conf, NULL) == NGX_OK, "feed init");

    for (i = 0; i < 100; i++) {
        CHECK(feed_publish(&feed, 40, (int64_t) i * 100, i) == NGX_OK,
              "unit %lu published", i);
    }

    CHECK(ngx_media_feed_head(&feed) == 100,
          "every unit was published: %llu", (unsigned long long) ngx_media_feed_head(&feed));
    CHECK(ngx_media_feed_units(&feed) == 8,
          "the retained window is the ceiling: %lu", ngx_media_feed_units(&feed));
    CHECK(ngx_media_feed_bytes(&feed) == 8 * 40,
          "retained bytes: %lu", (unsigned long) ngx_media_feed_bytes(&feed));
    CHECK(ngx_media_feed_tail(&feed) == 92,
          "the rest were evicted, not kept: %llu", (unsigned long long) ngx_media_feed_tail(&feed));

    /* a consumer that fell behind is told, not given a partial window */
    cursor.generation = ngx_media_feed_generation(&feed);
    cursor.next_sequence = 0;

    CHECK(ngx_media_feed_read(&feed, &cursor, 8, 0, 0, out, &count)
          == NGX_MEDIA_FEED_OVERRUN, "the overrun is reported");
    CHECK(cursor.next_sequence == 0, "the cursor is left untouched");

    cursor.next_sequence = ngx_media_feed_tail(&feed);
    frames = 0;

    while (ngx_media_feed_read(&feed, &cursor, 8, 0, 0, out, &count)
           == NGX_MEDIA_FEED_BATCH)
    {
        frames += count;
        ngx_media_feed_release(out, count);
    }

    CHECK(frames == 8, "only the retained window is readable: %llu", (unsigned long long) frames);

    ngx_media_feed_destroy(&feed);

    TEST_CASE("feed: the byte ceiling bounds retained bytes; an oversized unit "
              "is kept alone and evicted next");

    conf.max_units = 64;
    conf.max_bytes = 1000;
    conf.max_age = 0;

    CHECK(ngx_media_feed_init(&feed, &conf, NULL) == NGX_OK, "feed init");

    for (i = 0; i < 6; i++) {
        CHECK(feed_publish(&feed, 400, (int64_t) i * 100, i) == NGX_OK,
              "unit %lu published", i);
        CHECK(ngx_media_feed_bytes(&feed) <= 1000,
              "retained bytes stay under the ceiling: %lu",
              (unsigned long) ngx_media_feed_bytes(&feed));
    }

    CHECK(ngx_media_feed_units(&feed) == 2, "two units retained: %lu",
          ngx_media_feed_units(&feed));
    CHECK(ngx_media_feed_bytes(&feed) == 800, "800 bytes retained: %lu",
          (unsigned long) ngx_media_feed_bytes(&feed));
    CHECK(ngx_media_feed_tail(&feed) == 4, "four units evicted: %llu",
          (unsigned long long) ngx_media_feed_tail(&feed));

    /* a unit larger than the ceiling is never split: it is kept alone, so
     * bytes are bounded by max(max_bytes, largest unit), and the next publish
     * evicts it */
    CHECK(feed_publish(&feed, 2000, 700, 7) == NGX_OK, "oversized unit kept");
    CHECK(ngx_media_feed_units(&feed) == 1, "kept alone: %lu",
          ngx_media_feed_units(&feed));
    CHECK(ngx_media_feed_bytes(&feed) == 2000, "bounded by the unit itself: %lu",
          (unsigned long) ngx_media_feed_bytes(&feed));

    CHECK(feed_publish(&feed, 40, 800, 8) == NGX_OK, "next unit published");
    CHECK(ngx_media_feed_bytes(&feed) == 40, "the oversized unit was evicted: %lu",
          (unsigned long) ngx_media_feed_bytes(&feed));

    ngx_media_feed_destroy(&feed);

    TEST_CASE("feed: retained media age is bounded");

    conf.max_units = 64;
    conf.max_bytes = 0;
    conf.max_age = 1000;

    CHECK(ngx_media_feed_init(&feed, &conf, NULL) == NGX_OK, "feed init");

    for (i = 0; i < 20; i++) {
        CHECK(feed_publish(&feed, 40, (int64_t) i * 100, (ngx_msec_t) i * 500)
              == NGX_OK, "unit %lu published", i);
    }

    /* publishes at 0, 500, ... 9500 ms: only the last 1 s is retained */
    CHECK(ngx_media_feed_units(&feed) == 3, "three units retained: %lu",
          ngx_media_feed_units(&feed));
    CHECK(ngx_media_feed_tail(&feed) == 17, "older units evicted: %llu",
          (unsigned long long) ngx_media_feed_tail(&feed));
    CHECK(9500 - feed.slots[feed.tail & (feed.capacity - 1)].publish_time
          <= 1000, "the oldest retained unit is inside the age ceiling");

    ngx_media_feed_destroy(&feed);
}

/*
 * In-progress HLS segments carry the same hard ceilings: a cut at the byte
 * ceiling and a cut at the wall-clock ceiling, so neither a stalled receiver
 * nor a producer that never sends a keyframe can grow one segment without
 * bound (goal doc 19, 34 item 17).
 */

typedef struct {
    ngx_media_ts_mux_t    mux;
    ngx_media_trackset_t  tracks;
} bounds_fixture_t;

static void
bounds_fixture_init(bounds_fixture_t *f)
{
    ngx_media_track_t  track;

    ngx_memzero(f, sizeof(*f));

    CHECK(ngx_media_ts_mux_init(&f->mux, NULL, NULL) == NGX_OK, "mux init");
    CHECK(ngx_media_trackset_init(&f->tracks, 4, NULL) == NGX_OK,
          "trackset init");

    ngx_memzero(&track, sizeof(track));
    track.media_type = NGX_MEDIA_TYPE_VIDEO;
    track.codec = NGX_MEDIA_CODEC_H264;
    track.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    CHECK(ngx_media_trackset_add(&f->tracks, &track) >= 0, "video track");

    CHECK(ngx_media_ts_mux_set_tracks(&f->mux, &f->tracks) == NGX_OK,
          "tracks set");
}

static ngx_int_t
bounds_burst(bounds_fixture_t *f, ngx_media_ts_burst_t *burst,
    ngx_uint_t frames, int64_t first_dts, int64_t step,
    unsigned keyframe_every)
{
    ngx_media_frame_t  frame;
    ngx_media_buf_t   *buf;
    ngx_uint_t         i;
    u_char            *p;

    if (ngx_media_ts_mux_burst_init(&f->mux, burst, 256 * 1024) != NGX_OK) {
        return NGX_ERROR;
    }

    for (i = 0; i < frames; i++) {
        unsigned  keyframe = keyframe_every && (i % keyframe_every) == 0;

        buf = ngx_media_buf_alloc(400);

        if (buf == NULL) {
            return NGX_ERROR;
        }

        p = ngx_media_buf_data(buf);
        p[0] = 0x00; p[1] = 0x00; p[2] = 0x00; p[3] = 0x01;
        p[4] = keyframe ? 0x65 : 0x41;
        memset(p + 5, 0x5A, 395);
        (void) ngx_media_buf_freeze(buf, 400);

        ngx_media_frame_init(&frame);
        frame.media_type = NGX_MEDIA_TYPE_VIDEO;
        frame.codec = NGX_MEDIA_CODEC_H264;
        frame.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
        frame.pts = first_dts + (int64_t) i * step;
        frame.dts = frame.pts;
        frame.keyframe = keyframe ? 1 : 0;
        frame.payload = buf;

        if (ngx_media_ts_mux_write_frame(&f->mux, burst, &frame, 0) != NGX_OK) {
            ngx_media_frame_release(&frame);
            return NGX_ERROR;
        }

        ngx_media_frame_release(&frame);
    }

    return ngx_media_ts_mux_burst_end(&f->mux, burst);
}

static void
test_hls_segment_ceilings(void)
{
    bounds_fixture_t       f;
    ngx_media_hls_t        hls;
    ngx_media_hls_conf_t   conf;
    ngx_media_ts_burst_t   burst;

    bounds_fixture_init(&f);

    ngx_media_hls_conf_default(&conf);
    conf.path.data = (u_char *) ".build/bounds-hls-time";
    conf.path.len = sizeof(".build/bounds-hls-time") - 1;
    conf.target_duration = 1000;
    conf.min_duration = 1000;
    conf.max_duration = 1000;
    conf.max_segment_bytes = 100000;   /* roomy: this case is about the clock */
    conf.max_segments = 4;
    conf.max_retained_bytes = 1 << 20;

    TEST_CASE("hls: an in-progress segment is cut at the time ceiling");

    CHECK(ngx_media_hls_init(&hls, &conf, NULL) == NGX_OK, "segmenter init");

    /* one keyframe at the start, then inter frames only: with no further
     * keyframe, only max_duration can close the segment */
    CHECK(bounds_burst(&f, &burst, 50, 900000, 3600, 50) == NGX_OK,
          "burst built");
    CHECK(ngx_media_hls_add_burst(&hls, &burst) == NGX_OK, "burst added");
    ngx_media_ts_mux_burst_destroy(&burst);

    CHECK(hls.forced_cuts >= 1, "the clock forced a cut: %llu",
          (unsigned long long) hls.forced_cuts);
    CHECK(ngx_media_hls_pending_duration(&hls) <= conf.max_duration,
          "an in-progress segment never exceeds max_duration: %lu",
          (unsigned long) ngx_media_hls_pending_duration(&hls));
    CHECK(hls.segments_written >= 1, "a segment was closed: %llu",
          (unsigned long long) hls.segments_written);

    ngx_media_hls_destroy(&hls);

    TEST_CASE("hls: an in-progress segment is cut at the byte ceiling");

    conf.path.data = (u_char *) ".build/bounds-hls-bytes";
    conf.path.len = sizeof(".build/bounds-hls-bytes") - 1;
    conf.max_segment_bytes = 2000;   /* five times below one burst */

    CHECK(ngx_media_hls_init(&hls, &conf, NULL) == NGX_OK, "segmenter init");

    CHECK(bounds_burst(&f, &burst, 25, 900000, 3600, 10) == NGX_OK,
          "burst built");
    CHECK(ngx_media_hls_add_burst(&hls, &burst) == NGX_OK, "burst added");
    ngx_media_ts_mux_burst_destroy(&burst);

    CHECK(hls.forced_cuts >= 1, "the byte ceiling forced a cut: %llu",
          (unsigned long long) hls.forced_cuts);
    CHECK(hls.bytes <= conf.max_segment_bytes + 400,
          "an in-progress segment is bounded by the ceiling plus one slice: "
          "%lu", (unsigned long) hls.bytes);
    CHECK(hls.bytes_written > 0, "segments were written: %llu",
          (unsigned long long) hls.bytes_written);

    ngx_media_hls_destroy(&hls);

    ngx_media_ts_mux_destroy(&f.mux);
    ngx_media_trackset_destroy(&f.tracks);
}

int
main(void)
{
    printf("== bounds and allocation failure\n");

    test_ipc_limits();
    test_rtmp_message_limit();
    test_allocation_failure();
    test_feed_ceilings();
    test_hls_segment_ceilings();

    TEST_LEAKS();
    TEST_MAIN_END();
}

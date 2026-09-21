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

#include "ngx_media_ipc.h"
#include "ngx_media_rtmp_wire.h"

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

int
main(void)
{
    printf("== bounds and allocation failure\n");

    test_ipc_limits();
    test_rtmp_message_limit();
    test_allocation_failure();

    TEST_LEAKS();
    TEST_MAIN_END();
}

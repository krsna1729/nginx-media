/*
 * Bounded inter-worker transport: typed messages, chunked frames, backpressure
 * and malformed input handling.  The endpoints are a real SOCK_SEQPACKET pair,
 * so the test exercises the same code path nginx workers use.
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_ipc.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if (!(cond)) {                                                        \
            ngx_media_test_failures++;                                        \
            printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,               \
                   ##__VA_ARGS__);                                            \
        }                                                                     \
    } while (0)

static ngx_media_ipc_header_t
header(ngx_uint_t type, uint64_t hash)
{
    ngx_media_ipc_header_t  h;

    memset(&h, 0, sizeof(h));

    h.version = NGX_MEDIA_IPC_VERSION;
    h.type = (uint8_t) type;
    h.hash = hash;
    h.media_type = NGX_MEDIA_TYPE_VIDEO;
    h.codec = NGX_MEDIA_CODEC_H264;
    h.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;

    return h;
}

static ngx_media_buf_t *
payload(size_t len, u_char seed)
{
    ngx_media_buf_t  *buf = ngx_media_buf_alloc(len);
    size_t            i;

    if (buf == NULL) {
        return NULL;
    }

    for (i = 0; i < len; i++) {
        ngx_media_buf_data(buf)[i] = (u_char) (seed + i);
    }

    (void) ngx_media_buf_freeze(buf, len);

    return buf;
}

static void
test_messages(void)
{
    ngx_media_ipc_endpoint_t  *local = NULL, *peer = NULL;
    ngx_media_ipc_message_t    message;
    ngx_media_ipc_header_t     h;
    ngx_media_buf_t           *buf;

    TEST_CASE("typed messages round trip");

    CHECK(ngx_media_ipc_pair_create(&local, &peer, NULL) == NGX_OK,
          "socket pair created");
    CHECK(local != NULL && peer != NULL, "both endpoints exist");

    /* header-only message */
    h = header(NGX_MEDIA_IPC_MSG_OPEN, 0x1234);
    h.source_type = NGX_MEDIA_SOURCE_RTMP;
    h.priority = 100;

    CHECK(ngx_media_ipc_send_header(peer, &h) == NGX_OK, "open sent");
    CHECK(ngx_media_ipc_recv(local, &message) == NGX_OK, "open received");
    CHECK(message.header.type == NGX_MEDIA_IPC_MSG_OPEN, "type preserved");
    CHECK(message.header.hash == 0x1234, "hash preserved");
    CHECK(message.header.priority == 100, "priority preserved");
    CHECK(message.payload == NULL, "no payload");
    ngx_media_ipc_message_release(&message);

    /* a small frame */
    buf = payload(64, 0x10);
    h = header(NGX_MEDIA_IPC_MSG_VIDEO, 0x1234);
    h.keyframe = 1;
    h.dts = 9000;
    h.pts = 9360;

    CHECK(ngx_media_ipc_send(peer, &h, buf, 0, 64) == NGX_OK, "frame sent");
    CHECK(ngx_media_ipc_recv(local, &message) == NGX_OK, "frame received");
    CHECK(message.header.keyframe == 1, "keyframe flag preserved");
    CHECK(message.header.pts == 9360 && message.header.dts == 9000,
          "timestamps preserved");
    CHECK(message.length == 64, "payload length: %lu", message.length);
    CHECK(message.payload != NULL
          && ngx_media_buf_size(message.payload) == 64
          && ngx_media_buf_data(message.payload)[0] == 0x10
          && ngx_media_buf_data(message.payload)[63] == (u_char) (0x10 + 63),
          "payload bytes preserved");
    ngx_media_ipc_message_release(&message);

    ngx_media_buf_unref(buf);

    CHECK(ngx_media_ipc_recv(local, &message) == NGX_AGAIN,
          "nothing else pending");

    ngx_media_ipc_close(local);
    ngx_media_ipc_close(peer);
}

static void
test_chunked_frame(void)
{
    ngx_media_ipc_endpoint_t  *local = NULL, *peer = NULL;
    ngx_media_ipc_message_t    message;
    ngx_media_ipc_frame_t      frame;
    ngx_media_ipc_header_t     h;
    ngx_media_buf_t           *buf;
    size_t                     len = NGX_MEDIA_IPC_MAX_PAYLOAD * 2 + 1000;
    ngx_uint_t                 chunks = 0;
    ngx_int_t                  rc;

    TEST_CASE("a frame larger than the transport bound is chunked");

    CHECK(ngx_media_ipc_pair_create(&local, &peer, NULL) == NGX_OK,
          "socket pair created");

    buf = payload(len, 0x40);
    h = header(NGX_MEDIA_IPC_MSG_VIDEO, 0x99);

    /*
     * A frame larger than the socket buffer cannot be handed over in one go:
     * the sender reports backpressure and the caller drains and retries, which
     * is exactly the behaviour a worker needs (goal doc 34 item 11).
     */
    memset(&frame, 0, sizeof(frame));

    {
        size_t     offset = 0;
        ngx_int_t  src;
        ngx_uint_t attempts = 0;

        while (offset < len) {
            src = ngx_media_ipc_send(peer, &h, buf, offset, len - offset);

            CHECK(src == NGX_OK || src == NGX_AGAIN,
                  "send reports success or backpressure");

            if (src == NGX_OK) {
                offset = len;
            }

            /* drain whatever the receiver can take, then try again */
            for ( ;; ) {
                rc = ngx_media_ipc_recv(local, &message);

                if (rc != NGX_OK) {
                    break;
                }

                chunks++;

                rc = ngx_media_ipc_frame_feed(&frame, &message);
                ngx_media_ipc_message_release(&message);

                if (rc == NGX_OK) {
                    offset = len;
                    break;
                }

                CHECK(rc == NGX_AGAIN, "chunk accepted");
            }

            if (src == NGX_OK) {
                break;
            }

            if (++attempts > 1000) {
                break;
            }
        }
    }

    CHECK(chunks == 3, "three chunks for %lu bytes: %lu", len, chunks);
    CHECK(frame.active == 1 && frame.received == len && frame.payload != NULL,
          "frame reassembled: %lu of %lu", frame.received, len);
    CHECK(frame.payload != NULL
          && ngx_media_buf_size(frame.payload) == len,
          "reassembled length: %lu",
          frame.payload != NULL ? ngx_media_buf_size(frame.payload) : 0);
    CHECK(frame.payload != NULL
          && ngx_media_buf_data(frame.payload)[len - 1]
             == (u_char) (0x40 + (len - 1) % 256),
          "last byte matches");
    CHECK(frame.header.keyframe == h.keyframe, "header kept from the first chunk");

    ngx_media_ipc_frame_reset(&frame);
    ngx_media_buf_unref(buf);

    ngx_media_ipc_close(local);
    ngx_media_ipc_close(peer);
}

static void
test_reassembly_rejects(void)
{
    ngx_media_ipc_frame_t      frame;
    ngx_media_ipc_message_t    message;
    ngx_media_buf_t           *buf;

    TEST_CASE("reassembly rejects inconsistent chunks");

    memset(&frame, 0, sizeof(frame));

    /* a middle chunk without its start */
    memset(&message, 0, sizeof(message));
    message.header.total = 100;
    message.header.offset = 50;
    message.length = 50;

    CHECK(ngx_media_ipc_frame_feed(&frame, &message) == NGX_ERROR,
          "orphan continuation rejected");
    CHECK(frame.active == 0, "no frame was started");

    /* a chunk that does not continue where the previous one ended */
    memset(&message, 0, sizeof(message));
    message.header.total = 100;
    message.header.offset = 0;
    message.header.flags = NGX_MEDIA_IPC_FLAG_MORE;
    message.length = 10;
    buf = payload(10, 1);
    message.payload = buf;

    CHECK(ngx_media_ipc_frame_feed(&frame, &message) == NGX_AGAIN,
          "first chunk accepted");
    CHECK(frame.received == 10, "ten bytes so far");

    memset(&message, 0, sizeof(message));
    message.header.total = 100;
    message.header.offset = 40;   /* a hole */
    message.length = 10;
    message.payload = buf;

    CHECK(ngx_media_ipc_frame_feed(&frame, &message) == NGX_ERROR,
          "gap rejected");
    CHECK(frame.active == 0, "frame reset after the gap");

    ngx_media_buf_unref(buf);

    /* a frame that ends short */
    memset(&frame, 0, sizeof(frame));
    memset(&message, 0, sizeof(message));
    message.header.total = 100;
    message.header.offset = 0;
    message.length = 10;
    buf = payload(10, 2);
    message.payload = buf;

    CHECK(ngx_media_ipc_frame_feed(&frame, &message) == NGX_ERROR,
          "short frame rejected");
    CHECK(frame.active == 0, "frame reset");

    ngx_media_buf_unref(buf);
}

static void
test_malformed(void)
{
    ngx_media_ipc_endpoint_t  *local = NULL, *peer = NULL;
    ngx_media_ipc_message_t    message;
    u_char                     bad[128];
    ssize_t                    n;

    TEST_CASE("malformed datagrams are rejected");

    CHECK(ngx_media_ipc_pair_create(&local, &peer, NULL) == NGX_OK,
          "socket pair created");

    /* shorter than a header */
    n = send(ngx_media_ipc_fd(peer), bad, 8, MSG_NOSIGNAL);
    CHECK(n == 8, "short datagram sent");
    CHECK(ngx_media_ipc_recv(local, &message) == NGX_DECLINED,
          "short datagram dropped, endpoint still usable");

    /* a valid header with a wrong version */
    memset(bad, 0, sizeof(bad));
    bad[0] = 99;
    n = send(ngx_media_ipc_fd(peer), bad, sizeof(ngx_media_ipc_header_t),
             MSG_NOSIGNAL);
    CHECK(n > 0, "bad version datagram sent");
    CHECK(ngx_media_ipc_recv(local, &message) == NGX_DECLINED,
          "bad version dropped, endpoint still usable");

    /* a header claiming more payload than the datagram carries */
    memset(bad, 0, sizeof(bad));
    bad[0] = NGX_MEDIA_IPC_VERSION;
    bad[1] = NGX_MEDIA_IPC_MSG_VIDEO;
    bad[4] = 0xFF;   /* length field, low bytes of the little endian value */
    bad[5] = 0xFF;
    n = send(ngx_media_ipc_fd(peer), bad, sizeof(ngx_media_ipc_header_t),
             MSG_NOSIGNAL);
    CHECK(n > 0, "truncated datagram sent");
    CHECK(ngx_media_ipc_recv(local, &message) == NGX_DECLINED,
          "length mismatch dropped, endpoint still usable");

    /* the valid message behind the bad ones is still delivered */
    memset(&message, 0, sizeof(message));
    memset(bad, 0, sizeof(bad));
    {
        ngx_media_ipc_header_t  ok;

        memset(&ok, 0, sizeof(ok));
        ok.version = NGX_MEDIA_IPC_VERSION;
        ok.type = NGX_MEDIA_IPC_MSG_CLOSE;
        ok.hash = 77;
        CHECK(ngx_media_ipc_send_header(peer, &ok) == NGX_OK, "valid sent");
    }
    CHECK(ngx_media_ipc_recv(local, &message) == NGX_OK
          && message.header.hash == 77,
          "a valid message after malformed ones is received");

    /* only a closed peer makes the endpoint unusable */
    ngx_media_ipc_close(peer);
    CHECK(ngx_media_ipc_recv(local, &message) == NGX_ERROR,
          "a closed peer is reported as an error");

    ngx_media_ipc_close(local);
}

static void
test_abandoned_frame_and_flow(void)
{
    ngx_media_ipc_frame_t      frame;
    ngx_media_ipc_message_t    message;
    ngx_media_ipc_flow_t       flow;
    ngx_media_buf_t           *buf;

    TEST_CASE("a frame abandoned mid-send does not cost the next frame");

    memset(&frame, 0, sizeof(frame));
    memset(&message, 0, sizeof(message));
    message.header.total = 100;
    message.header.flags = NGX_MEDIA_IPC_FLAG_MORE;
    message.length = 10;
    buf = payload(10, 1);
    message.payload = buf;
    CHECK(ngx_media_ipc_frame_feed(&frame, &message) == NGX_AGAIN,
          "the first chunk of a large frame is accepted");
    ngx_media_buf_unref(buf);

    /* the sender hit backpressure and moved on: a new frame starts at 0 */
    memset(&message, 0, sizeof(message));
    message.header.total = 20;
    message.length = 20;
    buf = payload(20, 2);
    message.payload = buf;
    CHECK(ngx_media_ipc_frame_feed(&frame, &message) == NGX_OK,
          "the next frame is delivered whole");
    CHECK(frame.restarts == 1, "the abandoned frame was counted: %lu",
          (unsigned long) frame.restarts);
    CHECK(frame.payload != NULL && ngx_media_buf_size(frame.payload) == 20,
          "its payload is the new frame's");
    ngx_media_buf_unref(buf);
    ngx_media_ipc_frame_reset(&frame);

    TEST_CASE("a failed send drops video up to the next keyframe");

    memset(&flow, 0, sizeof(flow));
    CHECK(ngx_media_ipc_flow_admit(&flow, 1, 1) == 1, "a keyframe is sent");
    ngx_media_ipc_flow_sent(&flow, 1, 1, NGX_OK);
    CHECK(ngx_media_ipc_flow_admit(&flow, 1, 0) == 1, "delta frames follow");
    ngx_media_ipc_flow_sent(&flow, 1, 0, NGX_AGAIN);
    CHECK(flow.resync == 1 && flow.resyncs == 1, "the failure starts a resync");
    CHECK(ngx_media_ipc_flow_admit(&flow, 1, 0) == 0,
          "delta video is held back");
    CHECK(ngx_media_ipc_flow_admit(&flow, 0, 0) == 1,
          "audio is not held back");
    ngx_media_ipc_flow_sent(&flow, 0, 0, NGX_OK);
    CHECK(flow.resync == 1, "audio does not end the resync");
    CHECK(ngx_media_ipc_flow_admit(&flow, 1, 1) == 1, "the next keyframe goes");
    ngx_media_ipc_flow_sent(&flow, 1, 1, NGX_OK);
    CHECK(flow.resync == 0, "a delivered keyframe ends the resync");
    CHECK(flow.dropped == 1, "one frame was held back: %lu",
          (unsigned long) flow.dropped);
    CHECK(flow.sequence == 5, "every frame was numbered: %lu",
          (unsigned long) flow.sequence);
    CHECK(ngx_media_ipc_flow_admit(&flow, 1, 0) == 1,
          "delta video flows again");
}

int
main(void)
{
    printf("== inter-worker ipc\n");

    test_messages();
    test_chunked_frame();
    test_reassembly_rejects();
    test_malformed();
    test_abandoned_frame_and_flow();

    TEST_LEAKS();
    TEST_MAIN_END();
}

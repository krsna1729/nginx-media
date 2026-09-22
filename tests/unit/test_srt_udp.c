/*
 * Transport backend conformance: the same contract checks run against a
 * backend, so a second implementation can be qualified without touching the
 * media core (goal doc 11.1, phase 10).
 *
 * The reference backend is exercised here; the Haivision backend is qualified
 * by the integration tests that publish with real encoders.
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_srt_udp.h"
#include "ngx_media_srt_transport.h"

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

static void
test_listen_connect_streamid(void)
{
    ngx_media_srt_listener_t  *listener;
    ngx_media_srt_session_t   *caller, *accepted;
    u_char                     id[256];
    ngx_int_t                  len;
    const u_char              *streamid = (const u_char *) "#!::r=live/x,s=peer";

    TEST_CASE("backend: listen, connect, accept and stream id");

    listener = ngx_media_srt_listen((const u_char *) "127.0.0.1", 24571, NULL,
                                     NULL);
    CHECK(listener != NULL, "listener created");

    caller = ngx_media_srt_connect((const u_char *) "127.0.0.1", 24571,
                                   streamid, strlen((const char *) streamid),
                                   1000, NULL, NULL);
    CHECK(caller != NULL, "caller connected");

    accepted = ngx_media_srt_accept(listener, 1000, NULL);
    CHECK(accepted != NULL, "session accepted");

    if (accepted != NULL) {
        len = ngx_media_srt_session_streamid(accepted, id, sizeof(id));

        CHECK(len == (ngx_int_t) strlen((const char *) streamid),
              "stream id length: %ld", (long) len);
        CHECK(len > 0 && memcmp(id, streamid, (size_t) len) == 0,
              "stream id preserved");
    }

    if (caller != NULL) {
        ngx_media_srt_session_close(caller);
    }

    if (accepted != NULL) {
        ngx_media_srt_session_close(accepted);
    }

    ngx_media_srt_listen_close(listener);
}

static void
test_send_receive(void)
{
    ngx_media_srt_listener_t  *listener;
    ngx_media_srt_session_t   *caller = NULL, *accepted = NULL;
    u_char                     buf[2048], got[2048];
    ngx_int_t                  n;
    ngx_uint_t                 i;

    TEST_CASE("backend: send and receive keep the bytes");

    listener = ngx_media_srt_listen((const u_char *) "127.0.0.1", 24572, NULL,
                                     NULL);
    CHECK(listener != NULL, "listener created");

    caller = ngx_media_srt_connect((const u_char *) "127.0.0.1", 24572,
                                   (const u_char *) "id", 2, 1000, NULL, NULL);
    CHECK(caller != NULL, "caller connected");

    accepted = ngx_media_srt_accept(listener, 1000, NULL);
    CHECK(accepted != NULL, "session accepted");

    if (caller != NULL && accepted != NULL) {
        size_t  sent = 0;

        for (i = 0; i < sizeof(buf); i++) {
            buf[i] = (u_char) (i * 7);
        }

        /* the caller sends; the listener receives */
        while (sent < sizeof(buf)) {
            size_t  take = sizeof(buf) - sent;

            if (take > 1316) {
                take = 1316;
            }

            n = ngx_media_srt_session_send(caller, buf + sent, take, 100);
            CHECK(n > 0, "send returned %ld", (long) n);

            if (n <= 0) {
                break;
            }

            sent += (size_t) n;
        }

        {
            size_t  received = 0;

            while (received < sizeof(buf)) {
                n = ngx_media_srt_session_recv(accepted, got + received,
                                               sizeof(got) - received, 200);

                if (n <= 0) {
                    break;
                }

                received += (size_t) n;
            }

            CHECK(received == sizeof(buf), "received %lu of %lu bytes",
                  received, sizeof(buf));
            CHECK(received == sizeof(buf)
                  && memcmp(got, buf, sizeof(buf)) == 0,
                  "payload bytes preserved");
        }
    }

    if (caller != NULL) {
        ngx_media_srt_session_close(caller);
    }

    if (accepted != NULL) {
        ngx_media_srt_session_close(accepted);
    }

    ngx_media_srt_listen_close(listener);
}
static void
test_multi_session_demux(void)
{
    ngx_media_srt_listener_t  *listener;
    ngx_media_srt_session_t   *caller_a = NULL, *caller_b = NULL;
    ngx_media_srt_session_t   *accepted_a = NULL, *accepted_b = NULL;
    ngx_media_srt_poll_t      *poll = NULL;
    ngx_media_srt_poll_event_t events[4];
    ngx_uint_t                 count = 0, i;
    u_char                     a[] = { 'a', '1', '!' };
    u_char                     b[] = { 'b', '2', '?' };
    u_char                     got_a[sizeof(a)], got_b[sizeof(b)];
    ngx_int_t                  n;
    ngx_uint_t                 saw_a, saw_b;

    TEST_CASE("backend: one listener demultiplexes interleaved sessions");

    listener = ngx_media_srt_listen((const u_char *) "127.0.0.1", 24574, NULL,
                                    NULL);
    CHECK(listener != NULL, "listener created");

    caller_a = ngx_media_srt_connect((const u_char *) "127.0.0.1", 24574,
                                     (const u_char *) "a", 1, 1000, NULL,
                                     NULL);
    caller_b = ngx_media_srt_connect((const u_char *) "127.0.0.1", 24574,
                                     (const u_char *) "b", 1, 1000, NULL,
                                     NULL);
    CHECK(caller_a != NULL && caller_b != NULL, "two callers connected");

    accepted_a = ngx_media_srt_accept(listener, 1000, NULL);
    accepted_b = ngx_media_srt_accept(listener, 1000, NULL);
    CHECK(accepted_a != NULL && accepted_b != NULL, "two sessions accepted");

    poll = ngx_media_srt_poll_create(NULL);
    CHECK(poll != NULL, "poll created");

    if (poll != NULL) {
        CHECK(ngx_media_srt_poll_add_listener(poll, listener) == NGX_OK,
              "listener added");
        CHECK(ngx_media_srt_poll_add_session(poll, accepted_a) == NGX_OK,
              "first session added");
        CHECK(ngx_media_srt_poll_add_session(poll, accepted_b) == NGX_OK,
              "second session added");
    }

    if (caller_a != NULL && caller_b != NULL && accepted_a != NULL
        && accepted_b != NULL && poll != NULL)
    {
        CHECK(ngx_media_srt_session_send(caller_a, a, sizeof(a), 100) > 0,
              "first peer sent");
        CHECK(ngx_media_srt_session_send(caller_b, b, sizeof(b), 100) > 0,
              "second peer sent");

        CHECK(ngx_media_srt_poll_wait(poll, 1000, events, 4, &count) == NGX_OK,
              "poll demux completed");

        saw_a = 0;
        saw_b = 0;

        for (i = 0; i < count; i++) {
            saw_a |= events[i].session == accepted_a;
            saw_b |= events[i].session == accepted_b;
        }

        CHECK(saw_a && saw_b, "both established peers reported: %lu", count);

        n = ngx_media_srt_session_recv(accepted_a, got_a, sizeof(got_a), 100);
        CHECK(n == (ngx_int_t) sizeof(got_a)
              && memcmp(got_a, a, sizeof(a)) == 0,
              "first peer received its own datagram");

        n = ngx_media_srt_session_recv(accepted_b, got_b, sizeof(got_b), 100);
        CHECK(n == (ngx_int_t) sizeof(got_b)
              && memcmp(got_b, b, sizeof(b)) == 0,
              "second peer received its own datagram");
    }

    if (poll != NULL) {
        ngx_media_srt_poll_remove_session(poll, accepted_a);
        ngx_media_srt_poll_remove_session(poll, accepted_b);
        ngx_media_srt_poll_remove_listener(poll, listener);
        ngx_media_srt_poll_destroy(poll);
    }

    if (caller_a != NULL) {
        ngx_media_srt_session_close(caller_a);
    }

    if (caller_b != NULL) {
        ngx_media_srt_session_close(caller_b);
    }

    if (accepted_a != NULL) {
        ngx_media_srt_session_close(accepted_a);
    }

    if (accepted_b != NULL) {
        ngx_media_srt_session_close(accepted_b);
    }

    if (listener != NULL) {
        ngx_media_srt_listen_close(listener);
    }
}


static void
test_stats_and_teardown(void)
{
    ngx_media_srt_listener_t  *listener;
    ngx_media_srt_session_t   *caller = NULL, *accepted = NULL;
    ngx_media_srt_stats_t      stats;
    ngx_media_srt_poll_t      *poll;
    ngx_media_srt_poll_event_t events[4];
    ngx_uint_t                 count = 0;
    u_char                     buf[64];
    ngx_int_t                  n;

    TEST_CASE("backend: stats, poll and teardown");

    listener = ngx_media_srt_listen((const u_char *) "127.0.0.1", 24573, NULL,
                                     NULL);
    CHECK(listener != NULL, "listener created");

    poll = ngx_media_srt_poll_create(NULL);
    CHECK(poll != NULL, "poll created");

    if (poll != NULL && listener != NULL) {
        CHECK(ngx_media_srt_poll_add_listener(poll, listener) == NGX_OK,
              "listener added to the poll");
    }

    caller = ngx_media_srt_connect((const u_char *) "127.0.0.1", 24573,
                                   (const u_char *) "id", 2, 1000, NULL, NULL);
    CHECK(caller != NULL, "caller connected");

    if (poll != NULL) {
        CHECK(ngx_media_srt_poll_wait(poll, 500, events, 4, &count) == NGX_OK,
              "poll waited");
        CHECK(count >= 1, "a pending session was reported: %lu", count);
        CHECK(count >= 1 && events[0].listener == listener,
              "the listener was reported");
    }

    accepted = ngx_media_srt_accept(listener, 500, NULL);
    CHECK(accepted != NULL, "session accepted");

    if (caller != NULL && accepted != NULL) {
        memset(buf, 0x47, sizeof(buf));

        CHECK(ngx_media_srt_session_send(caller, buf, sizeof(buf), 100) > 0,
              "bytes sent");

        if (poll != NULL) {
            CHECK(ngx_media_srt_poll_add_session(poll, accepted) == NGX_OK,
                  "session added to the poll");
        }

        n = ngx_media_srt_session_recv(accepted, buf, sizeof(buf), 500);
        CHECK(n == (ngx_int_t) sizeof(buf), "bytes received: %ld", (long) n);

        ngx_media_srt_session_stats(accepted, &stats);
        CHECK(stats.bytes_received >= sizeof(buf),
              "stats counted the bytes: %llu",
              (unsigned long long) stats.bytes_received);
    }

    if (poll != NULL) {
        ngx_media_srt_poll_remove_session(poll, accepted);
        ngx_media_srt_poll_destroy(poll);
    }

    if (caller != NULL) {
        ngx_media_srt_session_close(caller);
    }

    if (accepted != NULL) {
        ngx_media_srt_session_close(accepted);
    }

    ngx_media_srt_listen_close(listener);
    ngx_media_srt_shutdown();
}

int
main(void)
{
    printf("== srt reference backend conformance\n");

    ngx_media_srt_set_backend(&ngx_media_srt_udp_ops);

    test_listen_connect_streamid();
    test_send_receive();
    test_multi_session_demux();
    test_stats_and_teardown();

    TEST_LEAKS();
    TEST_MAIN_END();
}

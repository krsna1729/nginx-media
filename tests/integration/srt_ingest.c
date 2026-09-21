#define _POSIX_C_SOURCE 200809L

/*
 * Deterministic SRT ingest harness (phase 1 exit criteria).
 *
 * Listens for one SRT publisher, extracts and parses the Stream ID, receives
 * MPEG-TS bytes through the transport adapter into the bounded TS ingest
 * queue, drains that queue the way a worker would, and reports counters.
 *
 * Usage: srt_ingest <port> <idle-timeout-ms>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ngx_media_srt_transport.h"
#include "ngx_media_srt_streamid.h"
#include "ngx_media_ts_ingest.h"

static ngx_msec_t
now_ms(void)
{
    struct timespec  ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (ngx_msec_t) ((uint64_t) ts.tv_sec * 1000
                         + (uint64_t) ts.tv_nsec / 1000000);
}

int
main(int argc, char **argv)
{
    ngx_media_srt_listener_t    *listener = NULL;
    ngx_media_srt_session_t     *session = NULL;
    ngx_media_srt_streamid_t     id;
    ngx_media_ts_ingest_t        ingest;
    ngx_media_ts_ingest_conf_t   conf;
    ngx_media_ts_ingest_chunk_t  chunks[16];
    ngx_media_ts_ingest_stats_t  stats;
    ngx_media_srt_stats_t        srt_stats;
    u_char                       rbuf[65536];
    u_char                       sid[NGX_MEDIA_SRT_STREAMID_MAX + 1];
    ngx_uint_t                   port, idle_ms, count, i;
    uint64_t                     drained;
    ngx_msec_t                   start, last_data;
    ngx_int_t                    n;
    int                          rc = 1;

    port = (argc > 1) ? (ngx_uint_t) strtoul(argv[1], NULL, 10) : 19000;
    idle_ms = (argc > 2) ? (ngx_uint_t) strtoul(argv[2], NULL, 10) : 1500;

    conf.max_chunks = 256;
    conf.max_bytes = 8 * 1024 * 1024;

    if (ngx_media_ts_ingest_init(&ingest, &conf, NULL) != NGX_OK) {
        fprintf(stderr, "ingest init failed\n");
        return 1;
    }

    listener = ngx_media_srt_listen((const u_char *) "127.0.0.1", port, NULL);
    if (listener == NULL) {
        fprintf(stderr, "srt listen failed on port %lu\n",
                (unsigned long) port);
        goto done;
    }

    printf("LISTENING port=%lu\n", (unsigned long) port);
    fflush(stdout);

    session = ngx_media_srt_accept(listener, 15000, NULL);
    if (session == NULL) {
        fprintf(stderr, "accept timed out\n");
        goto done;
    }

    n = ngx_media_srt_session_streamid(session, sid,
                                       NGX_MEDIA_SRT_STREAMID_MAX);
    if (n < 0) {
        fprintf(stderr, "stream id unavailable\n");
        goto done;
    }

    sid[n] = '\0';

    printf("STREAMID raw=%s\n", sid);

    if (ngx_media_srt_streamid_parse(sid, (size_t) n, &id) != NGX_OK) {
        fprintf(stderr, "stream id parse failed\n");
        goto done;
    }

    printf("SOURCE app=%.*s stream=%.*s source=%.*s mode=%s\n",
           (int) id.application.len, id.application.data,
           (int) id.stream.len, id.stream.data,
           (int) id.source.len, id.source.data,
           id.mode_kind == NGX_MEDIA_SRT_MODE_PUBLISH ? "publish" :
           id.mode_kind == NGX_MEDIA_SRT_MODE_REQUEST ? "request" : "unknown");
    fflush(stdout);

    if (id.mode_kind != NGX_MEDIA_SRT_MODE_PUBLISH) {
        fprintf(stderr, "unexpected access-control mode\n");
        goto done;
    }

    start = now_ms();
    last_data = start;
    drained = 0;

    for ( ;; ) {

        n = ngx_media_srt_session_recv(session, rbuf, sizeof(rbuf), 200);

        if (n > 0) {
            (void) ngx_media_ts_ingest_write(&ingest, rbuf, (size_t) n,
                                             now_ms());
            last_data = now_ms();

            /* sample live transport stats while the session is up */
            ngx_media_srt_session_stats(session, &srt_stats);

        } else if (n < 0) {
            break;
        }

        for ( ;; ) {
            count = ngx_media_ts_ingest_read(&ingest, chunks, 16);
            if (count == 0) {
                break;
            }

            for (i = 0; i < count; i++) {
                drained += chunks[i].len;
            }

            ngx_media_ts_ingest_release(chunks, count);
        }

        if (n == 0 && now_ms() - last_data > idle_ms) {
            break;
        }

        if (now_ms() - start > 60000) {
            break;
        }
    }

    for ( ;; ) {
        count = ngx_media_ts_ingest_read(&ingest, chunks, 16);
        if (count == 0) {
            break;
        }

        for (i = 0; i < count; i++) {
            drained += chunks[i].len;
        }

        ngx_media_ts_ingest_release(chunks, count);
    }

    ngx_media_ts_ingest_stats(&ingest, &stats);

    printf("SRTSTATS bytes_received=%llu packets_received=%lld "
           "packets_lost=%lld ms_rtt=%.2f\n",
           (unsigned long long) srt_stats.bytes_received,
           (long long) srt_stats.packets_received,
           (long long) srt_stats.packets_lost,
           srt_stats.ms_rtt);

    printf("STATS bytes_in=%llu chunks_in=%llu drained_bytes=%llu "
           "dropped_chunks=%llu pending_chunks=%llu pending_bytes=%llu\n",
           (unsigned long long) stats.bytes_in,
           (unsigned long long) stats.chunks_in,
           (unsigned long long) drained,
           (unsigned long long) stats.chunks_dropped,
           (unsigned long long) ngx_media_ts_ingest_pending(&ingest),
           (unsigned long long) ngx_media_ts_ingest_bytes(&ingest));
    fflush(stdout);

    rc = (stats.bytes_in > 0
          && stats.chunks_in > 0
          && stats.chunks_dropped == 0
          && drained == stats.bytes_in
          && ngx_media_ts_ingest_pending(&ingest) == 0)
         ? 0 : 1;

    if (rc != 0) {
        fprintf(stderr, "ingest verification failed\n");
    }

done:

    if (session != NULL) {
        ngx_media_srt_session_close(session);
    }

    if (listener != NULL) {
        ngx_media_srt_listen_close(listener);
    }

    ngx_media_srt_shutdown();
    ngx_media_ts_ingest_destroy(&ingest);

    return rc;
}

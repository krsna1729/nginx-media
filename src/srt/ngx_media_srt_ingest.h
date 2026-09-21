#ifndef NGX_MEDIA_SRT_INGEST_H
#define NGX_MEDIA_SRT_INGEST_H

#ifdef NGX_MEDIA_UNIT_TEST
#define _POSIX_C_SOURCE 200809L
#endif

#include "ngx_media_platform.h"
#include "ngx_media_srt_streamid.h"
#include "ngx_media_srt_transport.h"
#include "ngx_media_ts_ingest.h"

#include <pthread.h>

/*
 * SRT ingest runtime (goal doc 11.2).
 *
 *   SRT transport helper thread          NGINX worker
 *   -------------------------           --------------------------
 *   accept / recv / enqueue   ---->     bounded TS ingest queue
 *   compact events            ---->     bounded event ring
 *                             eventfd   worker event handler drains both
 *
 * The helper thread performs transport progress only: it never parses Stream
 * IDs, never touches logical streams and never allocates media beyond the
 * bounded queue.  It hands the raw Stream ID to the worker in a compact
 * event, and the worker owns parsing, validation and registration.
 *
 * Phase 1 accepts one publisher at a time per listener; the shared transport
 * scheduler that owns many sessions (goal doc 11.3) arrives with the fanout
 * phases.
 */

#define NGX_MEDIA_SRT_EVENT_READY  1
#define NGX_MEDIA_SRT_EVENT_FAILED 2
#define NGX_MEDIA_SRT_EVENT_OPEN   3
#define NGX_MEDIA_SRT_EVENT_CLOSE  4

typedef struct {
    ngx_uint_t  type;          /* NGX_MEDIA_SRT_EVENT_* */
    uint64_t    session_id;
    uint64_t    bytes;         /* CLOSE: bytes carried by the session */
    uint64_t    chunks;        /* CLOSE: chunks carried by the session */
    ngx_uint_t  streamid_len;
    u_char      streamid[NGX_MEDIA_SRT_STREAMID_MAX + 1];
} ngx_media_srt_event_t;

typedef struct {
    ngx_str_t   host;          /* listener address, copied from config */
    ngx_uint_t  port;
    ngx_uint_t  max_chunks;    /* payload queue ceilings */
    size_t      max_bytes;
    ngx_uint_t  max_events;    /* event ring capacity */
} ngx_media_srt_ingest_conf_t;

typedef struct {
    ngx_media_srt_ingest_conf_t  conf;

    ngx_media_ts_ingest_t        payload;   /* worker drains this */

    ngx_media_srt_event_t       *events;
    ngx_uint_t                   events_capacity;
    uint64_t                     events_head;
    uint64_t                     events_tail;
    ngx_atomic_t                 events_lock;

    int                          notify_fd; /* eventfd, worker-owned loop */
    ngx_atomic_t                 stop;
    ngx_atomic_t                 ready;
    ngx_atomic_t                 failed;

    pthread_t                    thread;
    ngx_uint_t                   thread_started;

    ngx_atomic_t                 sessions_accepted;
    ngx_atomic_t                 sessions_rejected;
    ngx_atomic_t                 events_dropped;
} ngx_media_srt_ingest_t;

ngx_int_t ngx_media_srt_ingest_start(ngx_media_srt_ingest_t *ingest,
    const ngx_media_srt_ingest_conf_t *conf, ngx_log_t *log);
void ngx_media_srt_ingest_stop(ngx_media_srt_ingest_t *ingest);

/* pops up to max events; returns how many were copied */
ngx_uint_t ngx_media_srt_ingest_event_read(ngx_media_srt_ingest_t *ingest,
    ngx_media_srt_event_t *out, ngx_uint_t max);

#endif /* NGX_MEDIA_SRT_INGEST_H */

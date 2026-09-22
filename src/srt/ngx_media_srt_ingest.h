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
 * SRT ingest runtime (goal doc 11.2, 11.3).
 *
 *   SRT transport helper thread          NGINX worker
 *   -------------------------           --------------------------
 *   one shared poll owns the        ---> bounded TS ingest queue
 *   listener and every session           (chunks carry a session id)
 *   compact events                  ---> bounded event ring
 *                            eventfd    worker event handler drains both
 *
 * The helper thread performs transport progress only: it never parses Stream
 * IDs, never touches logical streams and never allocates media beyond the
 * bounded queue.  It hands the raw Stream ID to the worker in a compact
 * event; the worker owns parsing, validation and registration.
 *
 * One poll owns many sessions (goal doc 11.3): protocol progress is shared
 * schedulable work instead of one blocking loop per receiver.
 */

#define NGX_MEDIA_SRT_EVENT_READY  1
#define NGX_MEDIA_SRT_EVENT_FAILED 2
#define NGX_MEDIA_SRT_EVENT_OPEN   3
#define NGX_MEDIA_SRT_EVENT_CLOSE  4

#define NGX_MEDIA_SRT_MAX_SESSIONS 16

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
    ngx_uint_t  max_sessions;  /* concurrent publishers */

    /*
     * The second local address of a bonded listener (goal doc 11.4), or empty
     * for a plain one.  Both legs of a group caller are accepted as a single
     * session, so the session this queue reports is the bond, not a member.
     */
    ngx_str_t   bond_host;

    /*
     * Encryption for the listener, or NULL for none.  The passphrase points
     * into the configuration, which outlives the listener.
     */
    const ngx_media_srt_params_t  *params;
} ngx_media_srt_ingest_conf_t;

typedef struct {
    ngx_media_srt_session_t  *session;
    uint64_t                  id;
    uint64_t                  bytes;
    uint64_t                  chunks;

    /*
     * Set by the worker when the source behind this session is removed
     * through the control API.  The session belongs to the ingest thread, so
     * the worker cannot close it; it asks, and the ingest loop acts.  Without
     * this the publisher stays attached and re-creates the source, so a
     * delete looks like it did nothing.
     */
    ngx_atomic_t              close_requested;
} ngx_media_srt_session_state_t;

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

    /*
     * Set by the worker when a graceful shutdown begins.  The ingest thread
     * stops accepting at once - a publisher taken now would be dropped when
     * this worker exits - and keeps serving the sessions already accepted.
     */
    ngx_atomic_t                 draining;

    ngx_atomic_t                 ready;
    ngx_atomic_t                 failed;

    ngx_log_t                   *log;       /* the worker's; used by the thread */

    pthread_t                    thread;
    ngx_uint_t                   thread_started;

    /* transport sessions owned by the shared poll */
    ngx_media_srt_session_state_t  sessions[NGX_MEDIA_SRT_MAX_SESSIONS];
    ngx_uint_t                     sessions_opened;

    ngx_atomic_t                 sessions_accepted;
    ngx_atomic_t                 sessions_dropped;
    ngx_atomic_t                 events_dropped;
} ngx_media_srt_ingest_t;

/*
 * Asks the ingest thread to close a session, by session id.  Returns NGX_OK
 * when the request was recorded, NGX_DECLINED when no such session exists.
 */
ngx_int_t ngx_media_srt_ingest_close_session(ngx_media_srt_ingest_t *ingest,
    uint64_t session_id);

ngx_int_t ngx_media_srt_ingest_start(ngx_media_srt_ingest_t *ingest,
    const ngx_media_srt_ingest_conf_t *conf, ngx_log_t *log);
void ngx_media_srt_ingest_stop(ngx_media_srt_ingest_t *ingest);

/*
 * Asks the ingest thread to stop accepting and give the listening sockets up.
 * The sessions already accepted are not touched; the port is free for the
 * worker a reload started as soon as no session is left, and while one is
 * live it is held by the transport, which is why the replacement worker
 * retries its bind.
 */
void ngx_media_srt_ingest_stop_accepting(ngx_media_srt_ingest_t *ingest);

/* pops up to max events; returns how many were copied */
ngx_uint_t ngx_media_srt_ingest_event_read(ngx_media_srt_ingest_t *ingest,
    ngx_media_srt_event_t *out, ngx_uint_t max);

#endif /* NGX_MEDIA_SRT_INGEST_H */

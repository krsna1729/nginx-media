#ifndef NGX_MEDIA_SRT_TRANSPORT_H
#define NGX_MEDIA_SRT_TRANSPORT_H

#include "ngx_media_platform.h"

/*
 * SRT transport adapter (goal doc 11.1).
 *
 * The media core and the SRT ingest code talk to this interface only; the
 * Haivision libsrt implementation is one backend behind it and the Robotweax
 * backend (phase 10) must qualify against the same contract.
 *
 * The adapter owns sockets and transport progress.  It does not parse Stream
 * IDs, mutate logical streams, fan out media or perform large allocations:
 * received bytes are handed to the caller in caller-provided buffers.
 */

typedef struct ngx_media_srt_listener_s ngx_media_srt_listener_t;
typedef struct ngx_media_srt_session_s ngx_media_srt_session_t;
typedef struct ngx_media_srt_poll_s ngx_media_srt_poll_t;

#define NGX_MEDIA_SRT_POLL_MAX 16

typedef struct {
    ngx_media_srt_listener_t  *listener;   /* the listener has a pending accept */
    ngx_media_srt_session_t   *session;    /* the session has readable bytes */
} ngx_media_srt_poll_event_t;

typedef struct {
    uint64_t   bytes_received;
    int64_t    packets_received;
    int64_t    packets_lost;
    int64_t    packets_retransmitted;
    int64_t    packets_dropped_too_late;
    double     ms_rtt;
    double     mbps_recv_rate;
} ngx_media_srt_stats_t;

/*
 * Encryption parameters (goal doc 11).  A NULL params pointer means "no
 * encryption"; an empty passphrase means the same.  Haivision/srt and
 * robotweax/srt both take these through SRTO_PASSPHRASE, SRTO_PBKEYLEN,
 * SRTO_CRYPTOMODE and SRTO_ENFORCEDENCRYPTION, so nothing here is specific to
 * either library.
 */
#define NGX_MEDIA_SRT_CRYPTO_CTR  0
#define NGX_MEDIA_SRT_CRYPTO_GCM  1

typedef struct {
    const u_char  *passphrase;      /* NULL or empty: no encryption */
    size_t         passphrase_len;  /* the SRT library wants 10..79 */
    ngx_uint_t     pbkeylen;        /* 0: library default, else 16, 24, 32 */
    ngx_uint_t     cryptomode;      /* NGX_MEDIA_SRT_CRYPTO_CTR or _GCM */
    ngx_uint_t     enforced;        /* reject peers whose secret does not match */
} ngx_media_srt_params_t;

typedef struct {
    const char *name;

    ngx_media_srt_listener_t *(*listen)(const u_char *host, ngx_uint_t port,
        const ngx_media_srt_params_t *params, ngx_log_t *log);
    void (*listen_close)(ngx_media_srt_listener_t *listener);

    /*
     * Stops accepting: closes the listening sockets and leaves every session
     * already accepted running.  listen_close() is still what releases the
     * listener itself.
     *
     * An SRT session keeps the listening socket's UDP socket alive, so with a
     * publisher attached the port stays bound until the session ends; with no
     * session left the port is free as soon as this returns.  A backend whose
     * sessions share the listening socket cannot release it at all and says
     * so in its own comment.
     */
    void (*listen_stop)(ngx_media_srt_listener_t *listener);

    /* accepted session, or NULL on timeout or error */
    ngx_media_srt_session_t *(*accept)(ngx_media_srt_listener_t *listener,
        ngx_msec_t timeout_ms, ngx_log_t *log);

    /*
     * Accepts the connection the poll just reported as ready.  Only valid
     * after a poll event for this listener; it never blocks.
     */
    ngx_media_srt_session_t *(*accept_ready)(ngx_media_srt_listener_t *listener,
        ngx_log_t *log);

    /* copies at most cap bytes of the peer stream id; returns length or -1 */
    ngx_int_t (*streamid)(ngx_media_srt_session_t *session, u_char *buf,
        size_t cap);

    /* > 0 bytes received, 0 when no data arrived before the timeout,
     * -1 when the session is closed or failed */
    ngx_int_t (*recv)(ngx_media_srt_session_t *session, u_char *buf,
        size_t cap, ngx_msec_t timeout_ms);

    /*
     * Caller side (goal doc 11, SRT output): connects to a destination and
     * sends already prepared transport bytes.  The adapter never prepares or
     * fans out media itself.
     */
    ngx_media_srt_session_t *(*connect)(const u_char *host, ngx_uint_t port,
        const u_char *streamid, size_t streamid_len, ngx_msec_t timeout_ms,
        const ngx_media_srt_params_t *params, ngx_log_t *log);

    /*
     * > 0 bytes sent; 0 when the transport could not take the buffer yet
     * (a timeout or a full send queue: the caller keeps the buffer and
     * retries); -1 when the session is broken and must be reopened.
     */
    ngx_int_t (*send)(ngx_media_srt_session_t *session, const u_char *buf,
        size_t len, ngx_msec_t timeout_ms);

    void (*stats)(ngx_media_srt_session_t *session,
        ngx_media_srt_stats_t *out);

    /*
     * Makes a send or receive already in progress on this session return
     * promptly, without releasing the session, so a sender parked in a
     * transport call is not left to wait out a send timeout while its thread
     * is being joined.  The session stays valid and is still the caller's to
     * close, once no thread can be inside a call on it.  A backend whose
     * sessions share a socket says in its own comment what it can and cannot
     * shut down.
     */
    void (*session_shutdown)(ngx_media_srt_session_t *session);

    /*
     * Releases the session.  Only called when no sender or receiver can be
     * inside a call on it: a caller that has just shut one down closes it
     * after it has joined the threads that use it.
     */
    void (*session_close)(ngx_media_srt_session_t *session);

    /*
     * Shared transport scheduler (goal doc 11.3): one poll owns the listener
     * and many sessions, so protocol progress is shared schedulable work
     * instead of one blocking loop per receiver.
     */
    ngx_media_srt_poll_t *(*poll_create)(ngx_log_t *log);
    void (*poll_destroy)(ngx_media_srt_poll_t *poll);
    ngx_int_t (*poll_add_listener)(ngx_media_srt_poll_t *poll,
        ngx_media_srt_listener_t *listener);
    void (*poll_remove_listener)(ngx_media_srt_poll_t *poll,
        ngx_media_srt_listener_t *listener);
    ngx_int_t (*poll_add_session)(ngx_media_srt_poll_t *poll,
        ngx_media_srt_session_t *session);
    void (*poll_remove_session)(ngx_media_srt_poll_t *poll,
        ngx_media_srt_session_t *session);
    ngx_int_t (*poll_wait)(ngx_media_srt_poll_t *poll, ngx_msec_t timeout_ms,
        ngx_media_srt_poll_event_t *events, ngx_uint_t max, ngx_uint_t *count);

    /*
     * Implementation identity, reported once at startup: which backend is in
     * use ("srt" for the SRT library, "udp" for the test double) and which
     * library provides it (Haivision/srt or robotweax/srt, both of which
     * expose the same C API and are therefore indistinguishable in code).
     */
    const char *(*library_version)(void);

    /*
     * The last transport failure as a printable string.  The adapter never
     * logs: the caller reports the detail through its own logger.
     */
    const char *(*last_error)(void);

    /* releases backend-global state once no listener remains */
    void (*shutdown)(void);

    /*
     * Group (bonded) acceptance (goal doc 11.4).  Creates a listener that
     * also accepts group callers: the second local address is bound too and
     * group acceptance is enabled on both, so the legs of one bonded caller
     * arrive as one socket and the core and the selector see one source and
     * never a bond member.  NULL for a backend that cannot bond.
     */
    ngx_media_srt_listener_t *(*listen_bond)(const u_char *host,
        ngx_uint_t port, const u_char *bond_host,
        const ngx_media_srt_params_t *params, ngx_log_t *log);
} ngx_media_srt_ops_t;

extern ngx_media_srt_ops_t ngx_media_srt_haivision_ops;

/* selected backend; defaults to the Haivision libsrt backend */
/* the backend in use: media_srt_backend selection, or the linked default */
ngx_media_srt_ops_t *ngx_media_srt_backend(void);

void ngx_media_srt_set_backend(ngx_media_srt_ops_t *ops);

ngx_media_srt_listener_t *ngx_media_srt_listen(const u_char *host,
    ngx_uint_t port, const ngx_media_srt_params_t *params, ngx_log_t *log);

/*
 * The listener that also accepts group (bonded) callers: the same listen
 * address plus a second local address, with group acceptance enabled on
 * both.  Both legs of one bonded caller are accepted as a single session, so
 * nothing above this layer ever learns that a source has members.
 */
ngx_media_srt_listener_t *ngx_media_srt_listen_bond(const u_char *host,
    ngx_uint_t port, const u_char *bond_host,
    const ngx_media_srt_params_t *params, ngx_log_t *log);

void ngx_media_srt_listen_close(ngx_media_srt_listener_t *listener);

/*
 * Stops accepting without touching the sessions already accepted.  The worker
 * that is shutting down calls this the moment the graceful shutdown starts,
 * so it stops taking publishers it is about to drop and, when it has no
 * session left, hands the port straight over; a binding publisher keeps the
 * port until it ends, which the replacement worker waits out by retrying.
 */
void ngx_media_srt_listen_stop(ngx_media_srt_listener_t *listener);

ngx_media_srt_session_t *ngx_media_srt_accept(ngx_media_srt_listener_t *listener,
    ngx_msec_t timeout_ms, ngx_log_t *log);
ngx_media_srt_session_t *ngx_media_srt_accept_ready(
    ngx_media_srt_listener_t *listener, ngx_log_t *log);
ngx_int_t ngx_media_srt_session_streamid(ngx_media_srt_session_t *session,
    u_char *buf, size_t cap);
ngx_int_t ngx_media_srt_session_recv(ngx_media_srt_session_t *session,
    u_char *buf, size_t cap, ngx_msec_t timeout_ms);

/* caller side: connects to a destination, optionally announcing a stream id */
ngx_media_srt_session_t *ngx_media_srt_connect(const u_char *host,
    ngx_uint_t port, const u_char *streamid, size_t streamid_len,
    ngx_msec_t timeout_ms, const ngx_media_srt_params_t *params,
    ngx_log_t *log);
ngx_int_t ngx_media_srt_session_send(ngx_media_srt_session_t *session,
    const u_char *buf, size_t len, ngx_msec_t timeout_ms);
void ngx_media_srt_session_stats(ngx_media_srt_session_t *session,
    ngx_media_srt_stats_t *out);

/*
 * Wakes a call in progress without releasing the session; close it after the
 * threads that were using it have been joined.
 */
void ngx_media_srt_session_shutdown(ngx_media_srt_session_t *session);
void ngx_media_srt_session_close(ngx_media_srt_session_t *session);

ngx_media_srt_poll_t *ngx_media_srt_poll_create(ngx_log_t *log);
void ngx_media_srt_poll_destroy(ngx_media_srt_poll_t *poll);
ngx_int_t ngx_media_srt_poll_add_listener(ngx_media_srt_poll_t *poll,
    ngx_media_srt_listener_t *listener);
void ngx_media_srt_poll_remove_listener(ngx_media_srt_poll_t *poll,
    ngx_media_srt_listener_t *listener);
ngx_int_t ngx_media_srt_poll_add_session(ngx_media_srt_poll_t *poll,
    ngx_media_srt_session_t *session);
void ngx_media_srt_poll_remove_session(ngx_media_srt_poll_t *poll,
    ngx_media_srt_session_t *session);
ngx_int_t ngx_media_srt_poll_wait(ngx_media_srt_poll_t *poll,
    ngx_msec_t timeout_ms, ngx_media_srt_poll_event_t *events,
    ngx_uint_t max, ngx_uint_t *count);

/* the detail of the last transport failure, or "" when there is none */
const char *ngx_media_srt_last_error(void);

/* closes every listener and session and releases backend-global state */
void ngx_media_srt_shutdown(void);

#endif /* NGX_MEDIA_SRT_TRANSPORT_H */

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

typedef struct {
    uint64_t   bytes_received;
    int64_t    packets_received;
    int64_t    packets_lost;
    int64_t    packets_retransmitted;
    int64_t    packets_dropped_too_late;
    double     ms_rtt;
    double     mbps_recv_rate;
} ngx_media_srt_stats_t;

typedef struct {
    const char *name;

    ngx_media_srt_listener_t *(*listen)(const u_char *host, ngx_uint_t port,
        ngx_log_t *log);
    void (*listen_close)(ngx_media_srt_listener_t *listener);

    /* accepted session, or NULL on timeout or error */
    ngx_media_srt_session_t *(*accept)(ngx_media_srt_listener_t *listener,
        ngx_msec_t timeout_ms, ngx_log_t *log);

    /* copies at most cap bytes of the peer stream id; returns length or -1 */
    ngx_int_t (*streamid)(ngx_media_srt_session_t *session, u_char *buf,
        size_t cap);

    /* > 0 bytes received, 0 when no data arrived before the timeout,
     * -1 when the session is closed or failed */
    ngx_int_t (*recv)(ngx_media_srt_session_t *session, u_char *buf,
        size_t cap, ngx_msec_t timeout_ms);

    void (*stats)(ngx_media_srt_session_t *session,
        ngx_media_srt_stats_t *out);

    void (*session_close)(ngx_media_srt_session_t *session);

    /* releases backend-global state once no listener remains */
    void (*shutdown)(void);
} ngx_media_srt_ops_t;

extern ngx_media_srt_ops_t ngx_media_srt_haivision_ops;

/* selected backend; defaults to the Haivision libsrt backend */
extern ngx_media_srt_ops_t *ngx_media_srt_backend;

void ngx_media_srt_set_backend(ngx_media_srt_ops_t *ops);

ngx_media_srt_listener_t *ngx_media_srt_listen(const u_char *host,
    ngx_uint_t port, ngx_log_t *log);
void ngx_media_srt_listen_close(ngx_media_srt_listener_t *listener);

ngx_media_srt_session_t *ngx_media_srt_accept(ngx_media_srt_listener_t *listener,
    ngx_msec_t timeout_ms, ngx_log_t *log);
ngx_int_t ngx_media_srt_session_streamid(ngx_media_srt_session_t *session,
    u_char *buf, size_t cap);
ngx_int_t ngx_media_srt_session_recv(ngx_media_srt_session_t *session,
    u_char *buf, size_t cap, ngx_msec_t timeout_ms);
void ngx_media_srt_session_stats(ngx_media_srt_session_t *session,
    ngx_media_srt_stats_t *out);
void ngx_media_srt_session_close(ngx_media_srt_session_t *session);

/* closes every listener and session and releases backend-global state */
void ngx_media_srt_shutdown(void);

#endif /* NGX_MEDIA_SRT_TRANSPORT_H */

/*
 * Haivision libsrt backend for the SRT transport adapter (goal doc 11.1).
 *
 * Everything that is specific to libsrt lives here.  The adapter's callers
 * see only ngx_media_srt_* and never an SRTSOCKET.
 */

#include "ngx_media_srt_transport.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <srt/srt.h>

struct ngx_media_srt_listener_s {
    SRTSOCKET                 sock;
    ngx_media_srt_session_t  *sessions;
    ngx_media_srt_listener_t *next;
};

struct ngx_media_srt_session_s {
    SRTSOCKET                 sock;
    ngx_media_srt_listener_t *listener;
    ngx_media_srt_session_t  *next;
};

static ngx_media_srt_listener_t *ngx_media_srt_listeners;
static ngx_uint_t                ngx_media_srt_started;

static ngx_media_srt_listener_t *ngx_media_srt_haivision_listen(
    const u_char *host, ngx_uint_t port, ngx_log_t *log);
static void ngx_media_srt_haivision_listen_close(
    ngx_media_srt_listener_t *listener);
static ngx_media_srt_session_t *ngx_media_srt_haivision_accept(
    ngx_media_srt_listener_t *listener, ngx_msec_t timeout_ms,
    ngx_log_t *log);
static ngx_int_t ngx_media_srt_haivision_streamid(
    ngx_media_srt_session_t *session, u_char *buf, size_t cap);
static ngx_int_t ngx_media_srt_haivision_recv(
    ngx_media_srt_session_t *session, u_char *buf, size_t cap,
    ngx_msec_t timeout_ms);
static void ngx_media_srt_haivision_stats(ngx_media_srt_session_t *session,
    ngx_media_srt_stats_t *out);
static void ngx_media_srt_haivision_session_close(
    ngx_media_srt_session_t *session);
static void ngx_media_srt_haivision_shutdown(void);

ngx_media_srt_ops_t ngx_media_srt_haivision_ops = {
    "haivision",
    ngx_media_srt_haivision_listen,
    ngx_media_srt_haivision_listen_close,
    ngx_media_srt_haivision_accept,
    ngx_media_srt_haivision_streamid,
    ngx_media_srt_haivision_recv,
    ngx_media_srt_haivision_stats,
    ngx_media_srt_haivision_session_close,
    ngx_media_srt_haivision_shutdown
};

static ngx_media_srt_listener_t *
ngx_media_srt_haivision_listen(const u_char *host, ngx_uint_t port,
    ngx_log_t *log)
{
    ngx_media_srt_listener_t  *listener;
    struct sockaddr_in         addr;
    SRTSOCKET                  sock;
    int                        transtype, yes, timeout;

    if (host == NULL || host[0] == '\0' || port == 0 || port > 65535) {
        return NULL;
    }

    if (!ngx_media_srt_started) {
        if (srt_startup() == SRT_ERROR) {
            return NULL;
        }

        ngx_media_srt_started = 1;
    }

    sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        return NULL;
    }

    transtype = SRTT_LIVE;
    yes = 1;
    timeout = 1000;

    if (srt_setsockopt(sock, 0, SRTO_TRANSTYPE, &transtype,
                       sizeof(transtype)) == SRT_ERROR
        || srt_setsockopt(sock, 0, SRTO_REUSEADDR, &yes, sizeof(yes))
           == SRT_ERROR
        || srt_setsockopt(sock, 0, SRTO_RCVTIMEO, &timeout,
                          sizeof(timeout)) == SRT_ERROR)
    {
        (void) srt_close(sock);
        return NULL;
    }

    ngx_memzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);

    if (inet_pton(AF_INET, (const char *) host, &addr.sin_addr) != 1) {
        (void) srt_close(sock);
        return NULL;
    }

    if (srt_bind(sock, (struct sockaddr *) &addr, sizeof(addr)) == SRT_ERROR
        || srt_listen(sock, 8) == SRT_ERROR)
    {
        (void) srt_close(sock);
        return NULL;
    }

    listener = ngx_alloc(sizeof(ngx_media_srt_listener_t), log);
    if (listener == NULL) {
        (void) srt_close(sock);
        return NULL;
    }

    ngx_memzero(listener, sizeof(ngx_media_srt_listener_t));

    listener->sock = sock;
    listener->next = ngx_media_srt_listeners;
    ngx_media_srt_listeners = listener;

    return listener;
}

static void
ngx_media_srt_haivision_listen_close(ngx_media_srt_listener_t *listener)
{
    ngx_media_srt_session_t   *session, *next;
    ngx_media_srt_listener_t **pp;

    if (listener == NULL) {
        return;
    }

    /* closing a listener closes every session it accepted */
    for (session = listener->sessions; session != NULL; session = next) {
        next = session->next;
        (void) srt_close(session->sock);
        ngx_free(session);
    }

    listener->sessions = NULL;

    (void) srt_close(listener->sock);

    for (pp = &ngx_media_srt_listeners; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == listener) {
            *pp = listener->next;
            break;
        }
    }

    ngx_free(listener);
}

static ngx_media_srt_session_t *
ngx_media_srt_haivision_accept(ngx_media_srt_listener_t *listener,
    ngx_msec_t timeout_ms, ngx_log_t *log)
{
    ngx_media_srt_session_t  *session;
    struct sockaddr_storage   addr;
    SRTSOCKET                 sock;
    int                       addrlen, timeout;

    if (listener == NULL || timeout_ms > (ngx_msec_t) INT_MAX) {
        return NULL;
    }

    timeout = (int) timeout_ms;

    if (srt_setsockopt(listener->sock, 0, SRTO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == SRT_ERROR)
    {
        return NULL;
    }

    addrlen = sizeof(addr);

    sock = srt_accept(listener->sock, (struct sockaddr *) &addr, &addrlen);
    if (sock == SRT_INVALID_SOCK) {
        return NULL;
    }

    session = ngx_alloc(sizeof(ngx_media_srt_session_t), log);
    if (session == NULL) {
        (void) srt_close(sock);
        return NULL;
    }

    ngx_memzero(session, sizeof(ngx_media_srt_session_t));

    session->sock = sock;
    session->listener = listener;
    session->next = listener->sessions;
    listener->sessions = session;

    return session;
}

static ngx_int_t
ngx_media_srt_haivision_streamid(ngx_media_srt_session_t *session, u_char *buf,
    size_t cap)
{
    int  len;

    if (session == NULL || buf == NULL || cap == 0 || cap > (size_t) INT_MAX) {
        return -1;
    }

    len = (int) cap;

    if (srt_getsockopt(session->sock, 0, SRTO_STREAMID, buf, &len)
        == SRT_ERROR)
    {
        return -1;
    }

    return (ngx_int_t) len;
}

static ngx_int_t
ngx_media_srt_haivision_recv(ngx_media_srt_session_t *session, u_char *buf,
    size_t cap, ngx_msec_t timeout_ms)
{
    int  n, timeout;

    if (session == NULL || buf == NULL || cap == 0 || cap > (size_t) INT_MAX
        || timeout_ms > (ngx_msec_t) INT_MAX)
    {
        return -1;
    }

    timeout = (int) timeout_ms;

    if (srt_setsockopt(session->sock, 0, SRTO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == SRT_ERROR)
    {
        return -1;
    }

    n = srt_recvmsg(session->sock, (char *) buf, (int) cap);

    if (n >= 0) {
        return (ngx_int_t) n;
    }

    if (srt_getlasterror(NULL) == SRT_ETIMEOUT) {
        return 0;
    }

    return -1;
}

static void
ngx_media_srt_haivision_stats(ngx_media_srt_session_t *session,
    ngx_media_srt_stats_t *out)
{
    SRT_TRACEBSTATS  stats;

    if (session == NULL || out == NULL) {
        return;
    }

    ngx_memzero(&stats, sizeof(stats));

    if (srt_bstats(session->sock, &stats, 0) == SRT_ERROR) {
        return;
    }

    out->bytes_received = stats.byteRecvTotal;
    out->packets_received = stats.pktRecvTotal;
    out->packets_lost = stats.pktRcvLossTotal;
    out->packets_retransmitted = stats.pktRetransTotal;
    out->packets_dropped_too_late = stats.pktRcvDropTotal;
    out->ms_rtt = stats.msRTT;
    out->mbps_recv_rate = stats.mbpsRecvRate;
}

static void
ngx_media_srt_haivision_session_close(ngx_media_srt_session_t *session)
{
    ngx_media_srt_session_t **pp;

    if (session == NULL) {
        return;
    }

    (void) srt_close(session->sock);

    if (session->listener != NULL) {
        for (pp = &session->listener->sessions; *pp != NULL;
             pp = &(*pp)->next)
        {
            if (*pp == session) {
                *pp = session->next;
                break;
            }
        }
    }

    ngx_free(session);
}

static void
ngx_media_srt_haivision_shutdown(void)
{
    while (ngx_media_srt_listeners != NULL) {
        ngx_media_srt_haivision_listen_close(ngx_media_srt_listeners);
    }

    if (ngx_media_srt_started) {
        (void) srt_cleanup();
        ngx_media_srt_started = 0;
    }
}

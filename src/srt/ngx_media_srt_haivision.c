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

struct ngx_media_srt_poll_s {
    int                       eid;
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
static ngx_media_srt_session_t *ngx_media_srt_haivision_accept_ready(
    ngx_media_srt_listener_t *listener, ngx_log_t *log);
static ngx_media_srt_session_t *ngx_media_srt_session_wrap(
    ngx_media_srt_listener_t *listener, SRTSOCKET sock, ngx_log_t *log);
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

static ngx_media_srt_poll_t *ngx_media_srt_haivision_poll_create(
    ngx_log_t *log);
static void ngx_media_srt_haivision_poll_destroy(ngx_media_srt_poll_t *poll);
static ngx_int_t ngx_media_srt_haivision_poll_add_listener(
    ngx_media_srt_poll_t *poll, ngx_media_srt_listener_t *listener);
static ngx_int_t ngx_media_srt_haivision_poll_add_session(
    ngx_media_srt_poll_t *poll, ngx_media_srt_session_t *session);
static void ngx_media_srt_haivision_poll_remove_session(
    ngx_media_srt_poll_t *poll, ngx_media_srt_session_t *session);
static ngx_int_t ngx_media_srt_haivision_poll_wait(
    ngx_media_srt_poll_t *poll, ngx_msec_t timeout_ms,
    ngx_media_srt_poll_event_t *events, ngx_uint_t max, ngx_uint_t *count);
static ngx_media_srt_session_t *ngx_media_srt_session_by_socket(
    SRTSOCKET sock);

ngx_media_srt_ops_t ngx_media_srt_haivision_ops = {
    "haivision",
    ngx_media_srt_haivision_listen,
    ngx_media_srt_haivision_listen_close,
    ngx_media_srt_haivision_accept,
    ngx_media_srt_haivision_accept_ready,
    ngx_media_srt_haivision_streamid,
    ngx_media_srt_haivision_recv,
    ngx_media_srt_haivision_stats,
    ngx_media_srt_haivision_session_close,
    ngx_media_srt_haivision_poll_create,
    ngx_media_srt_haivision_poll_destroy,
    ngx_media_srt_haivision_poll_add_listener,
    ngx_media_srt_haivision_poll_add_session,
    ngx_media_srt_haivision_poll_remove_session,
    ngx_media_srt_haivision_poll_wait,
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
    SRTSOCKET  sock;

    if (listener == NULL || timeout_ms > (ngx_msec_t) INT_MAX) {
        return NULL;
    }

    /*
     * srt_accept() ignores SRTO_RCVTIMEO and blocks forever, which would pin
     * the helper thread (and the worker shutdown that joins it);
     * srt_accept_bond() accepts with a real timeout.
     */
    sock = srt_accept_bond(&listener->sock, 1, (int64_t) timeout_ms);
    if (sock == SRT_INVALID_SOCK) {
        return NULL;
    }

    return ngx_media_srt_session_wrap(listener, sock, log);
}

static ngx_media_srt_session_t *
ngx_media_srt_haivision_accept_ready(ngx_media_srt_listener_t *listener,
    ngx_log_t *log)
{
    SRTSOCKET  sock;
    int        zero = 0, one = 1;

    if (listener == NULL) {
        return NULL;
    }

    /*
     * A listener that is driven by a poll must be non-blocking: accept is
     * only called after the poll reported a pending connection, and it may
     * never block the shared scheduler.
     */
    if (srt_setsockopt(listener->sock, 0, SRTO_RCVSYN, &zero, sizeof(zero))
        == SRT_ERROR)
    {
        return NULL;
    }

    sock = srt_accept(listener->sock, NULL, NULL);

    if (sock == SRT_INVALID_SOCK) {
        return NULL;
    }

    /* the session itself blocks with an explicit receive timeout */
    if (srt_setsockopt(sock, 0, SRTO_RCVSYN, &one, sizeof(one)) == SRT_ERROR) {
        (void) srt_close(sock);
        return NULL;
    }

    return ngx_media_srt_session_wrap(listener, sock, log);
}

static ngx_media_srt_session_t *
ngx_media_srt_session_wrap(ngx_media_srt_listener_t *listener, SRTSOCKET sock,
    ngx_log_t *log)
{
    ngx_media_srt_session_t  *session;

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

static ngx_media_srt_session_t *
ngx_media_srt_session_by_socket(SRTSOCKET sock)
{
    ngx_media_srt_listener_t  *listener;
    ngx_media_srt_session_t   *session;

    for (listener = ngx_media_srt_listeners; listener != NULL;
         listener = listener->next)
    {
        if (listener->sock == sock) {
            return NULL;
        }

        for (session = listener->sessions; session != NULL;
             session = session->next)
        {
            if (session->sock == sock) {
                return session;
            }
        }
    }

    return NULL;
}

static ngx_media_srt_listener_t *
ngx_media_srt_listener_by_socket(SRTSOCKET sock)
{
    ngx_media_srt_listener_t  *listener;

    for (listener = ngx_media_srt_listeners; listener != NULL;
         listener = listener->next)
    {
        if (listener->sock == sock) {
            return listener;
        }
    }

    return NULL;
}

static ngx_media_srt_poll_t *
ngx_media_srt_haivision_poll_create(ngx_log_t *log)
{
    ngx_media_srt_poll_t  *poll;
    int                    eid;

    eid = srt_epoll_create();
    if (eid == SRT_ERROR) {
        return NULL;
    }

    poll = ngx_alloc(sizeof(ngx_media_srt_poll_t), log);
    if (poll == NULL) {
        (void) srt_epoll_release(eid);
        return NULL;
    }

    poll->eid = eid;

    return poll;
}

static void
ngx_media_srt_haivision_poll_destroy(ngx_media_srt_poll_t *poll)
{
    if (poll == NULL) {
        return;
    }

    (void) srt_epoll_release(poll->eid);

    ngx_free(poll);
}

static ngx_int_t
ngx_media_srt_poll_register(ngx_media_srt_poll_t *poll, SRTSOCKET sock)
{
    int  events = SRT_EPOLL_IN | SRT_EPOLL_ERR;

    if (poll == NULL || sock == SRT_INVALID_SOCK) {
        return NGX_ERROR;
    }

    if (srt_epoll_add_usock(poll->eid, sock, &events) == SRT_ERROR) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_media_srt_haivision_poll_add_listener(ngx_media_srt_poll_t *poll,
    ngx_media_srt_listener_t *listener)
{
    return ngx_media_srt_poll_register(poll, listener->sock);
}

static ngx_int_t
ngx_media_srt_haivision_poll_add_session(ngx_media_srt_poll_t *poll,
    ngx_media_srt_session_t *session)
{
    return ngx_media_srt_poll_register(poll, session->sock);
}

static void
ngx_media_srt_haivision_poll_remove_session(ngx_media_srt_poll_t *poll,
    ngx_media_srt_session_t *session)
{
    if (poll == NULL || session == NULL) {
        return;
    }

    (void) srt_epoll_remove_usock(poll->eid, session->sock);
}

static ngx_int_t
ngx_media_srt_haivision_poll_wait(ngx_media_srt_poll_t *poll,
    ngx_msec_t timeout_ms, ngx_media_srt_poll_event_t *events, ngx_uint_t max,
    ngx_uint_t *count)
{
    SRT_EPOLL_EVENT           fds[NGX_MEDIA_SRT_POLL_MAX];
    ngx_media_srt_listener_t *listener;
    ngx_media_srt_session_t  *session;
    int                       n, i;

    if (poll == NULL || events == NULL || count == NULL) {
        return NGX_ERROR;
    }

    if (max > NGX_MEDIA_SRT_POLL_MAX) {
        max = NGX_MEDIA_SRT_POLL_MAX;
    }

    n = srt_epoll_uwait(poll->eid, fds, (int) max, (int64_t) timeout_ms);

    if (n == SRT_ERROR) {
        return NGX_ERROR;
    }

    *count = 0;

    for (i = 0; i < n; i++) {

        listener = ngx_media_srt_listener_by_socket(fds[i].fd);

        if (listener != NULL) {
            events[*count].listener = listener;
            events[*count].session = NULL;
            (*count)++;
            continue;
        }

        session = ngx_media_srt_session_by_socket(fds[i].fd);

        if (session == NULL) {
            continue;
        }

        events[*count].listener = NULL;
        events[*count].session = session;
        (*count)++;
    }

    return NGX_OK;
}

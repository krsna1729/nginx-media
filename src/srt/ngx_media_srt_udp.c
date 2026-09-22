/*
 * Reference SRT backend (goal doc 11.1, phase 10).
 *
 * This is a second, independent implementation of the transport contract in
 * ngx_media_srt_transport.h, written over plain UDP sockets.  It exists to
 * prove that the media core and the SRT ingest/output runtimes are genuinely
 * backend-agnostic, and to give the qualification suite something to compare
 * Haivision against without the commercial Robotweax SDK.
 *
 * It is NOT SRT: there is no handshake, no retransmission, no congestion
 * control and no encryption.  The stream id travels as one leading packet,
 * which is enough for the contract (a peer identity the worker can trust) but
 * is not the SRT stream id negotiation.  Anything that needs SRT semantics
 * must use the Haivision backend.
 */

#include "ngx_media_srt_streamid.h"
#include "ngx_media_srt_transport.h"

#include <pthread.h>

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define NGX_MEDIA_SRT_REFERENCE_MAX_SESSIONS 32
#define NGX_MEDIA_SRT_REFERENCE_STREAMID_MAGIC 0x53494431u   /* "SID1" */

struct ngx_media_srt_listener_s {
    int                        fd;
    ngx_uint_t                 port;
    ngx_media_srt_session_t   *sessions;
    ngx_media_srt_listener_t  *next;
};

struct ngx_media_srt_session_s {
    int                        fd;
    struct sockaddr_in         peer;
    ngx_media_srt_listener_t  *listener;
    ngx_media_srt_session_t   *next;

    u_char                     streamid[NGX_MEDIA_SRT_STREAMID_MAX + 1];
    size_t                     streamid_len;
    unsigned                   have_streamid:1;
    unsigned                   caller:1;

    uint64_t                   bytes_received;
    uint64_t                   bytes_sent;
    uint64_t                   packets_received;
    uint64_t                   packets_lost;
};

struct ngx_media_srt_poll_s {
    ngx_media_srt_listener_t  *listener;
    ngx_media_srt_session_t   *session;
    /* poll(2) is used directly, so the type never shadows it */
};

static ngx_media_srt_listener_t *ngx_media_srt_udp_listeners;
static ngx_media_srt_session_t  *ngx_media_srt_udp_callers;

/*
 * The caller and listener session lists are walked and mutated from two
 * threads: a sender reconnecting, and the event loop stopping or shutting
 * down.  Without this lock the two can unlink and free the same session.
 * ThreadSanitizer reported exactly that as a race in session_close, with the
 * free in the shim as its follow-on symptom.
 */
static pthread_mutex_t           ngx_media_srt_udp_lock =
                                     PTHREAD_MUTEX_INITIALIZER;
static ngx_uint_t                ngx_media_srt_udp_started;

static ngx_media_srt_listener_t *ngx_media_srt_udp_listen(
    const u_char *host, ngx_uint_t port,
    const ngx_media_srt_params_t *params, ngx_log_t *log);
static void ngx_media_srt_udp_listen_close(
    ngx_media_srt_listener_t *listener);
static void ngx_media_srt_udp_listen_stop(
    ngx_media_srt_listener_t *listener);
static ngx_media_srt_session_t *ngx_media_srt_udp_accept(
    ngx_media_srt_listener_t *listener, ngx_msec_t timeout_ms, ngx_log_t *log);
static ngx_media_srt_session_t *ngx_media_srt_udp_accept_ready(
    ngx_media_srt_listener_t *listener, ngx_log_t *log);
static ngx_int_t ngx_media_srt_udp_streamid(
    ngx_media_srt_session_t *session, u_char *buf, size_t cap);
static ngx_int_t ngx_media_srt_udp_recv(ngx_media_srt_session_t *session,
    u_char *buf, size_t cap, ngx_msec_t timeout_ms);
static ngx_media_srt_session_t *ngx_media_srt_udp_connect(
    const u_char *host, ngx_uint_t port, const u_char *streamid,
    size_t streamid_len, ngx_msec_t timeout_ms,
    const ngx_media_srt_params_t *params, ngx_log_t *log);
static ngx_int_t ngx_media_srt_udp_send(ngx_media_srt_session_t *session,
    const u_char *buf, size_t len, ngx_msec_t timeout_ms);
static void ngx_media_srt_udp_stats(ngx_media_srt_session_t *session,
    ngx_media_srt_stats_t *out);
static void ngx_media_srt_udp_session_shutdown(
    ngx_media_srt_session_t *session);
static void ngx_media_srt_udp_session_close(
    ngx_media_srt_session_t *session);
static void ngx_media_srt_udp_shutdown(void);

/* the double is not SRT and says so wherever the implementation is reported */
static const char *
ngx_media_srt_udp_library_version(void)
{
    return "udp test double (not SRT)";
}

static ngx_media_srt_poll_t *ngx_media_srt_udp_poll_create(
    ngx_log_t *log);
static void ngx_media_srt_udp_poll_destroy(ngx_media_srt_poll_t *poll);
static ngx_int_t ngx_media_srt_udp_poll_add_listener(
    ngx_media_srt_poll_t *poll, ngx_media_srt_listener_t *listener);
static void ngx_media_srt_udp_poll_remove_listener(
    ngx_media_srt_poll_t *poll, ngx_media_srt_listener_t *listener);
static ngx_int_t ngx_media_srt_udp_poll_add_session(
    ngx_media_srt_poll_t *poll, ngx_media_srt_session_t *session);
static void ngx_media_srt_udp_poll_remove_session(
    ngx_media_srt_poll_t *poll, ngx_media_srt_session_t *session);
static ngx_int_t ngx_media_srt_udp_poll_wait(
    ngx_media_srt_poll_t *poll, ngx_msec_t timeout_ms,
    ngx_media_srt_poll_event_t *events, ngx_uint_t max, ngx_uint_t *count);

ngx_media_srt_ops_t ngx_media_srt_udp_ops = {
    "udp",
    ngx_media_srt_udp_listen,
    ngx_media_srt_udp_listen_close,
    ngx_media_srt_udp_listen_stop,
    ngx_media_srt_udp_accept,
    ngx_media_srt_udp_accept_ready,
    ngx_media_srt_udp_streamid,
    ngx_media_srt_udp_recv,
    ngx_media_srt_udp_connect,
    ngx_media_srt_udp_send,
    ngx_media_srt_udp_stats,
    ngx_media_srt_udp_session_shutdown,
    ngx_media_srt_udp_session_close,
    ngx_media_srt_udp_poll_create,
    ngx_media_srt_udp_poll_destroy,
    ngx_media_srt_udp_poll_add_listener,
    ngx_media_srt_udp_poll_remove_listener,
    ngx_media_srt_udp_poll_add_session,
    ngx_media_srt_udp_poll_remove_session,
    ngx_media_srt_udp_poll_wait,
    ngx_media_srt_udp_library_version,
    NULL,                                  /* no printable last error */
    ngx_media_srt_udp_shutdown,
    NULL,                                  /* no group acceptance */
    NULL                                   /* no shared port */
};

static ngx_int_t
ngx_media_srt_udp_bind(const u_char *host, ngx_uint_t port,
    struct sockaddr_in *addr)
{
    ngx_memzero(addr, sizeof(struct sockaddr_in));

    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t) port);

    if (host == NULL || host[0] == '\0') {
        addr->sin_addr.s_addr = htonl(INADDR_ANY);

    } else if (inet_pton(AF_INET, (const char *) host, &addr->sin_addr) != 1) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_media_srt_udp_socket(struct sockaddr_in *addr, int *fd)
{
    int  sock;

    sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (sock < 0) {
        return NGX_ERROR;
    }

    if (bind(sock, (struct sockaddr *) addr, sizeof(struct sockaddr_in)) < 0) {
        (void) close(sock);
        return NGX_ERROR;
    }

    *fd = sock;

    return NGX_OK;
}

static ngx_media_srt_listener_t *
ngx_media_srt_udp_listen(const u_char *host, ngx_uint_t port,
    const ngx_media_srt_params_t *params, ngx_log_t *log)
{
    ngx_media_srt_listener_t  *listener;
    struct sockaddr_in         addr;
    int                        fd;

    /* encryption is an SRT feature; this double speaks plain UDP */
    (void) params;

    if (port == 0 || port > 65535
        || ngx_media_srt_udp_bind(host, port, &addr) != NGX_OK
        || ngx_media_srt_udp_socket(&addr, &fd) != NGX_OK)
    {
        return NULL;
    }

    listener = ngx_alloc(sizeof(ngx_media_srt_listener_t), log);

    if (listener == NULL) {
        (void) close(fd);
        return NULL;
    }

    ngx_memzero(listener, sizeof(ngx_media_srt_listener_t));

    listener->fd = fd;
    listener->port = port;
    listener->next = ngx_media_srt_udp_listeners;
    ngx_media_srt_udp_listeners = listener;
    ngx_media_srt_udp_started = 1;

    return listener;
}

static void
ngx_media_srt_udp_listen_stop(ngx_media_srt_listener_t *listener)
{
    /*
     * The double multiplexes every peer it has accepted on the listener's own
     * socket, so there is no separate listening socket to release: closing
     * the fd would break the sessions the caller asked to keep.  Stopping the
     * accept loop is all this backend can do, and the ingest thread does it
     * by taking the listener out of the poll.
     */
    (void) listener;
}

static void
ngx_media_srt_udp_listen_close(ngx_media_srt_listener_t *listener)
{
    ngx_media_srt_session_t   *session, *next;
    ngx_media_srt_listener_t **pp;

    if (listener == NULL) {
        return;
    }

    for (session = listener->sessions; session != NULL; session = next) {
        next = session->next;
        (void) close(session->fd);
        ngx_free(session);
    }

    (void) close(listener->fd);

    for (pp = &ngx_media_srt_udp_listeners; *pp != NULL;
         pp = &(*pp)->next)
    {
        if (*pp == listener) {
            *pp = listener->next;
            break;
        }
    }

    ngx_free(listener);
}

/*
 * The first datagram a peer sends carries its stream id: one magic, one
 * length and the bytes.  It is consumed by accept() and never handed to the
 * media path.
 */
static ngx_int_t
ngx_media_srt_udp_read_streamid(ngx_media_srt_session_t *session,
    ngx_msec_t timeout_ms)
{
    u_char  buf[NGX_MEDIA_SRT_STREAMID_MAX + 16];
    ssize_t n;

    (void) timeout_ms;

    n = recv(session->fd, buf, sizeof(buf), 0);

    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? NGX_AGAIN
                                                         : NGX_ERROR;
    }

    if ((size_t) n < 8) {
        return NGX_ERROR;
    }

    {
        uint32_t  magic;
        uint32_t  len;

        ngx_memcpy(&magic, buf, 4);
        ngx_memcpy(&len, buf + 4, 4);

        if (magic != NGX_MEDIA_SRT_REFERENCE_STREAMID_MAGIC
            || len > NGX_MEDIA_SRT_STREAMID_MAX
            || (size_t) n != 8 + len)
        {
            return NGX_ERROR;
        }

        if (len > 0) {
            ngx_memcpy(session->streamid, buf + 8, len);
        }

        session->streamid[len] = '\0';
        session->streamid_len = len;
    }

    session->have_streamid = 1;

    return NGX_OK;
}

static ngx_media_srt_session_t *
ngx_media_srt_udp_accept(ngx_media_srt_listener_t *listener,
    ngx_msec_t timeout_ms, ngx_log_t *log)
{
    struct pollfd  pfd;
    int            rc;

    if (listener == NULL) {
        return NULL;
    }

    pfd.fd = listener->fd;
    pfd.events = POLLIN;

    rc = poll(&pfd, 1, (timeout_ms > (ngx_msec_t) INT_MAX) ? INT_MAX
                                                          : (int) timeout_ms);

    if (rc <= 0) {
        return NULL;
    }

    return ngx_media_srt_udp_accept_ready(listener, log);
}

/* the session that already belongs to this peer, if any */
static ngx_media_srt_session_t *
ngx_media_srt_udp_session_for(ngx_media_srt_listener_t *listener,
    const struct sockaddr_in *peer)
{
    ngx_media_srt_session_t  *session;

    for (session = listener->sessions; session != NULL;
         session = session->next)
    {
        if (session->peer.sin_addr.s_addr == peer->sin_addr.s_addr
            && session->peer.sin_port == peer->sin_port)
        {
            return session;
        }
    }

    return NULL;
}

static ngx_media_srt_session_t *
ngx_media_srt_udp_accept_ready(ngx_media_srt_listener_t *listener,
    ngx_log_t *log)
{
    ngx_media_srt_session_t  *session;
    struct sockaddr_in        peer;
    socklen_t                 peer_len = sizeof(peer);
    u_char                    probe[1];
    ssize_t                   n;

    if (listener == NULL) {
        return NULL;
    }

    /* peek so the datagram itself is still there for the stream id reader */
    n = recvfrom(listener->fd, probe, sizeof(probe), MSG_PEEK,
                 (struct sockaddr *) &peer, &peer_len);

    if (n < 0) {
        return NULL;
    }

    /*
     * A peer that already has a session is sending media, not a handshake:
     * accept must leave it for the session's reader.
     */
    if (ngx_media_srt_udp_session_for(listener, &peer) != NULL) {
        return NULL;
    }

    session = ngx_alloc(sizeof(ngx_media_srt_session_t), log);

    if (session == NULL) {
        return NULL;
    }

    ngx_memzero(session, sizeof(ngx_media_srt_session_t));

    session->fd = listener->fd;
    session->peer = peer;
    session->listener = listener;
    session->next = listener->sessions;
    listener->sessions = session;

    if (ngx_media_srt_udp_read_streamid(session, 0) != NGX_OK) {
        ngx_media_srt_udp_session_close(session);
        return NULL;
    }

    return session;
}

static ngx_int_t
ngx_media_srt_udp_streamid(ngx_media_srt_session_t *session, u_char *buf,
    size_t cap)
{
    if (session == NULL || !session->have_streamid) {
        return -1;
    }

    if (session->streamid_len > cap) {
        return -1;
    }

    if (session->streamid_len > 0) {
        ngx_memcpy(buf, session->streamid, session->streamid_len);
    }

    return (ngx_int_t) session->streamid_len;
}

static ngx_int_t
ngx_media_srt_udp_recv(ngx_media_srt_session_t *session, u_char *buf,
    size_t cap, ngx_msec_t timeout_ms)
{
    struct pollfd  pfd;
    ssize_t        n;
    int            rc;

    if (session == NULL) {
        return -1;
    }

    pfd.fd = session->fd;
    pfd.events = POLLIN;

    rc = poll(&pfd, 1, (timeout_ms > (ngx_msec_t) INT_MAX) ? INT_MAX
                                                          : (int) timeout_ms);

    if (rc == 0) {
        return 0;
    }

    if (rc < 0) {
        return (errno == EINTR) ? 0 : -1;
    }

    for ( ;; ) {
        struct sockaddr_in  from;
        socklen_t           from_len = sizeof(from);

        n = recvfrom(session->fd, buf, cap, 0, (struct sockaddr *) &from,
                     &from_len);

        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }

        /* every session shares the listener socket: keep only our peer */
        if (from.sin_addr.s_addr != session->peer.sin_addr.s_addr
            || from.sin_port != session->peer.sin_port)
        {
            continue;
        }

        break;
    }

    if (n == 0) {
        return 0;
    }

    session->bytes_received += (uint64_t) n;
    session->packets_received++;

    return (ngx_int_t) n;
}

static ngx_media_srt_session_t *
ngx_media_srt_udp_connect(const u_char *host, ngx_uint_t port,
    const u_char *streamid, size_t streamid_len, ngx_msec_t timeout_ms,
    const ngx_media_srt_params_t *params, ngx_log_t *log)
{
    ngx_media_srt_session_t  *session;
    struct sockaddr_in        local, peer;
    u_char                    packet[NGX_MEDIA_SRT_STREAMID_MAX + 8];
    uint32_t                  magic = NGX_MEDIA_SRT_REFERENCE_STREAMID_MAGIC;
    uint32_t                  len = (uint32_t) streamid_len;
    int                       fd;

    (void) timeout_ms;
    (void) params;

    if (port == 0 || port > 65535 || streamid_len > NGX_MEDIA_SRT_STREAMID_MAX
        || ngx_media_srt_udp_bind(NULL, 0, &local) != NGX_OK
        || ngx_media_srt_udp_bind(host, port, &peer) != NGX_OK
        || ngx_media_srt_udp_socket(&local, &fd) != NGX_OK)
    {
        return NULL;
    }

    /*
     * A caller session has exactly one peer for its whole life, so the socket
     * is connected to it: sends and receives then only ever involve that
     * peer, and - the reason it is done here - the socket can be shut down,
     * which is how a stop releases a sender parked in a call on it.
     * shutdown(2) on an unconnected datagram socket is refused with ENOTCONN
     * and leaves the socket exactly as it was.
     */
    if (connect(fd, (struct sockaddr *) &peer, sizeof(peer)) < 0) {
        (void) close(fd);
        return NULL;
    }

    session = ngx_alloc(sizeof(ngx_media_srt_session_t), log);

    if (session == NULL) {
        (void) close(fd);
        return NULL;
    }

    ngx_memzero(session, sizeof(ngx_media_srt_session_t));

    session->fd = fd;
    session->peer = peer;
    session->caller = 1;
    (void) pthread_mutex_lock(&ngx_media_srt_udp_lock);
    session->next = ngx_media_srt_udp_callers;
    ngx_media_srt_udp_callers = session;
    (void) pthread_mutex_unlock(&ngx_media_srt_udp_lock);

    /* announce the stream id so the listener can name the source */
    ngx_memcpy(packet, &magic, 4);
    ngx_memcpy(packet + 4, &len, 4);

    if (streamid_len > 0) {
        ngx_memcpy(packet + 8, streamid, streamid_len);
    }

    if (sendto(fd, packet, 8 + streamid_len, 0,
               (struct sockaddr *) &peer, sizeof(peer)) < 0)
    {
        ngx_media_srt_udp_session_close(session);
        return NULL;
    }

    return session;
}

static ngx_int_t
ngx_media_srt_udp_send(ngx_media_srt_session_t *session,
    const u_char *buf, size_t len, ngx_msec_t timeout_ms)
{
    ssize_t  n;

    /*
     * The socket is non-blocking, so a send never waits for this timeout: a
     * full send buffer is reported as backpressure and the caller retries.
     */
    (void) timeout_ms;

    if (session == NULL || buf == NULL || len == 0) {
        return NGX_ERROR;
    }

    n = sendto(session->fd, buf, len, 0, (struct sockaddr *) &session->peer,
               sizeof(session->peer));

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
            return 0;   /* backpressure, retried by the caller */
        }

        return NGX_ERROR;
    }

    session->bytes_sent += (uint64_t) n;

    return (ngx_int_t) n;
}

static void
ngx_media_srt_udp_stats(ngx_media_srt_session_t *session,
    ngx_media_srt_stats_t *out)
{
    if (session == NULL || out == NULL) {
        return;
    }

    ngx_memzero(out, sizeof(ngx_media_srt_stats_t));

    out->bytes_received = session->bytes_received;
    out->packets_received = (int64_t) session->packets_received;
    out->packets_lost = (int64_t) session->packets_lost;
}

static void
ngx_media_srt_udp_session_close(ngx_media_srt_session_t *session)
{
    ngx_media_srt_session_t **pp;
    ngx_uint_t                unlinked = 0;

    if (session == NULL) {
        return;
    }

    /*
     * The list decides who closes: the thread that unlinks the session is the
     * one that frees it, and a second caller that finds it already gone
     * returns without touching the memory.  Two threads do reach here for the
     * same session - a sender reconnecting and the event loop stopping - and
     * an unconditional free after an unlink that matched nothing is a double
     * free, which is what ThreadSanitizer was reporting.
     */
    (void) pthread_mutex_lock(&ngx_media_srt_udp_lock);

    for (pp = (session->listener != NULL) ? &session->listener->sessions
                                          : &ngx_media_srt_udp_callers;
         *pp != NULL; pp = &(*pp)->next)
    {
        if (*pp == session) {
            *pp = session->next;
            unlinked = 1;
            break;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_srt_udp_lock);

    if (!unlinked) {
        return;
    }

    /*
     * The fd is closed here and nowhere else, and only when no thread can be
     * inside a call on it.  A caller that wants to take a session out of
     * service while a sender may be using it shuts the socket down first
     * (ngx_media_srt_udp_session_shutdown) and closes the session once the
     * threads that use it have been joined: close(2) while another thread is
     * inside sendto(2) on the same fd is a race ThreadSanitizer reports, and
     * one a raw socket cannot make safe the way libsrt's srt_close does for
     * the real backend.
     */
    if (session->caller) {
        (void) close(session->fd);
    }

    ngx_free(session);
}

/*
 * The caller owns its socket, so it can be shut down while a sender is using
 * it: sends and receives on it fail at once from then on, instead of being
 * offered to a session that is on its way out.  (The double's socket is
 * non-blocking as well, so a send never waits in the kernel to begin with -
 * see ngx_media_srt_udp_send.)  An accepted session cannot be treated the same
 * way - every session of one listener shares the listener's socket, and
 * shutting that down would take the other sessions on that listener with it.
 * What ends those is the listener's own close, and each receive is a poll with
 * a bounded timeout, so it returns anyway.
 *
 * The session object is untouched here: shutdown and close are separate steps
 * because the caller must be able to join the threads using the session while
 * the session is still valid.
 */
static void
ngx_media_srt_udp_session_shutdown(ngx_media_srt_session_t *session)
{
    if (session == NULL || !session->caller) {
        return;
    }

    (void) shutdown(session->fd, SHUT_RDWR);
}

static void
ngx_media_srt_udp_shutdown(void)
{
    while (ngx_media_srt_udp_listeners != NULL) {
        ngx_media_srt_udp_listen_close(
            ngx_media_srt_udp_listeners);
    }

    for ( ;; ) {
        ngx_media_srt_session_t  *head;

        (void) pthread_mutex_lock(&ngx_media_srt_udp_lock);
        head = ngx_media_srt_udp_callers;
        (void) pthread_mutex_unlock(&ngx_media_srt_udp_lock);

        if (head == NULL) {
            break;
        }

        ngx_media_srt_udp_session_close(head);
    }

    ngx_media_srt_udp_started = 0;
}

static ngx_media_srt_poll_t *
ngx_media_srt_udp_poll_create(ngx_log_t *log)
{
    ngx_media_srt_poll_t  *scheduler;

    scheduler = ngx_alloc(sizeof(ngx_media_srt_poll_t), log);

    if (scheduler == NULL) {
        return NULL;
    }

    ngx_memzero(scheduler, sizeof(ngx_media_srt_poll_t));

    return scheduler;
}

static void
ngx_media_srt_udp_poll_destroy(ngx_media_srt_poll_t *scheduler)
{
    ngx_free(scheduler);
}

static ngx_int_t
ngx_media_srt_udp_poll_add_listener(ngx_media_srt_poll_t *scheduler,
    ngx_media_srt_listener_t *listener)
{
    if (scheduler == NULL || listener == NULL) {
        return NGX_ERROR;
    }

    scheduler->listener = listener;

    return NGX_OK;
}

static void
ngx_media_srt_udp_poll_remove_listener(ngx_media_srt_poll_t *scheduler,
    ngx_media_srt_listener_t *listener)
{
    if (scheduler != NULL && scheduler->listener == listener) {
        scheduler->listener = NULL;
    }
}

static ngx_int_t
ngx_media_srt_udp_poll_add_session(ngx_media_srt_poll_t *scheduler,
    ngx_media_srt_session_t *session)
{
    if (scheduler == NULL || session == NULL) {
        return NGX_ERROR;
    }

    scheduler->session = session;

    return NGX_OK;
}

static void
ngx_media_srt_udp_poll_remove_session(ngx_media_srt_poll_t *scheduler,
    ngx_media_srt_session_t *session)
{
    if (scheduler != NULL && scheduler->session == session) {
        scheduler->session = NULL;
    }
}

/*
 * The reference transport keeps its sessions on the listener socket, so the
 * runtime only ever polls one listener and one session: that is all the ingest
 * path asks of a backend.
 */
static ngx_int_t
ngx_media_srt_udp_poll_wait(ngx_media_srt_poll_t *scheduler,
    ngx_msec_t timeout_ms, ngx_media_srt_poll_event_t *events, ngx_uint_t max,
    ngx_uint_t *count)
{
    struct pollfd  pfd[2];
    ngx_uint_t     n = 0, at;
    int            rc;

    if (scheduler == NULL || events == NULL || count == NULL || max == 0) {
        return NGX_ERROR;
    }

    *count = 0;

    if (scheduler->listener != NULL) {
        pfd[n].fd = scheduler->listener->fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
    }

    if (scheduler->session != NULL) {
        pfd[n].fd = scheduler->session->fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
    }

    if (n == 0) {
        return NGX_OK;
    }

    rc = poll(pfd, n, (timeout_ms > (ngx_msec_t) INT_MAX) ? INT_MAX
                                                         : (int) timeout_ms);

    if (rc <= 0) {
        return (rc < 0 && errno != EINTR) ? NGX_ERROR : NGX_OK;
    }

    if (scheduler->listener != NULL && (pfd[0].revents & POLLIN)) {
        struct sockaddr_in  peer;
        socklen_t           peer_len = sizeof(peer);
        u_char              probe[1];
        ngx_uint_t          known = 0;

        if (recvfrom(scheduler->listener->fd, probe, sizeof(probe), MSG_PEEK,
                     (struct sockaddr *) &peer, &peer_len) >= 0)
        {
            known = (ngx_media_srt_udp_session_for(scheduler->listener,
                                                         &peer) != NULL);
        }

        if (known && scheduler->session != NULL) {
            /* media from the established peer: report its session */
            events[*count].listener = NULL;
            events[*count].session = scheduler->session;
            (*count)++;

        } else {
            events[*count].listener = scheduler->listener;
            events[*count].session = NULL;
            (*count)++;
        }
    }

    at = (scheduler->listener != NULL) ? 1 : 0;

    if (scheduler->session != NULL && *count < max
        && (pfd[at].revents & POLLIN))
    {
        events[*count].listener = NULL;
        events[*count].session = scheduler->session;
        (*count)++;
    }

    return NGX_OK;
}

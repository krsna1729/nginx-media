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
#define NGX_MEDIA_SRT_REFERENCE_MAX_PACKET   65536
#define NGX_MEDIA_SRT_REFERENCE_PENDING      16
#define NGX_MEDIA_SRT_REFERENCE_STREAMID_MAGIC 0x53494431u   /* "SID1" */

typedef struct {
    u_char  *data;
    size_t   len;
} ngx_media_srt_udp_datagram_t;

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

    ngx_media_srt_udp_datagram_t pending[
        NGX_MEDIA_SRT_REFERENCE_PENDING];
    ngx_uint_t                 pending_head;
    ngx_uint_t                 pending_count;

    uint64_t                   bytes_received;
    uint64_t                   bytes_sent;
    uint64_t                   packets_received;
    uint64_t                   packets_lost;
};

struct ngx_media_srt_poll_s {
    ngx_media_srt_listener_t  *listener;
    ngx_media_srt_session_t   *sessions[NGX_MEDIA_SRT_REFERENCE_MAX_SESSIONS];
    ngx_uint_t                 session_count;
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
static void ngx_media_srt_udp_pending_clear(
    ngx_media_srt_session_t *session);
static ngx_int_t ngx_media_srt_udp_pending_push(
    ngx_media_srt_session_t *session, const u_char *data, size_t len);
static ngx_int_t ngx_media_srt_udp_pending_pop(
    ngx_media_srt_session_t *session, u_char *buf, size_t cap);
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
    NULL,                                  /* no shared port */
    NULL                                   /* no multiplexer groups */
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
        ngx_media_srt_udp_pending_clear(session);
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
static void
ngx_media_srt_udp_pending_clear(ngx_media_srt_session_t *session)
{
    ngx_uint_t  i;

    if (session == NULL) {
        return;
    }

    for (i = 0; i < NGX_MEDIA_SRT_REFERENCE_PENDING; i++) {
        if (session->pending[i].data != NULL) {
            ngx_free(session->pending[i].data);
            session->pending[i].data = NULL;
            session->pending[i].len = 0;
        }
    }

    session->pending_head = 0;
    session->pending_count = 0;
}

static ngx_int_t
ngx_media_srt_udp_pending_push(ngx_media_srt_session_t *session,
    const u_char *data, size_t len)
{
    ngx_media_srt_udp_datagram_t  *slot;
    u_char                        *copy;
    ngx_uint_t                     index;

    if (session == NULL || data == NULL || len == 0) {
        return NGX_ERROR;
    }

    if (session->pending_count >= NGX_MEDIA_SRT_REFERENCE_PENDING) {
        session->packets_lost++;
        return NGX_AGAIN;
    }

    copy = ngx_alloc(len, NULL);
    if (copy == NULL) {
        session->packets_lost++;
        return NGX_ERROR;
    }

    ngx_memcpy(copy, data, len);

    index = (session->pending_head + session->pending_count)
            % NGX_MEDIA_SRT_REFERENCE_PENDING;
    slot = &session->pending[index];
    slot->data = copy;
    slot->len = len;
    session->pending_count++;

    return NGX_OK;
}

static ngx_int_t
ngx_media_srt_udp_pending_pop(ngx_media_srt_session_t *session, u_char *buf,
    size_t cap)
{
    ngx_media_srt_udp_datagram_t  *slot;
    size_t                         take;

    if (session == NULL || buf == NULL || cap == 0
        || session->pending_count == 0)
    {
        return NGX_AGAIN;
    }

    slot = &session->pending[session->pending_head];
    take = (slot->len < cap) ? slot->len : cap;

    if (take > 0) {
        ngx_memcpy(buf, slot->data, take);
    }

    ngx_free(slot->data);
    slot->data = NULL;
    slot->len = 0;
    session->pending_head = (session->pending_head + 1)
                            % NGX_MEDIA_SRT_REFERENCE_PENDING;
    session->pending_count--;
    session->bytes_received += (uint64_t) take;
    session->packets_received++;

    return (ngx_int_t) take;
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
    int            rc;

    if (session == NULL || buf == NULL || cap == 0) {
        return -1;
    }

    if (session->pending_count > 0) {
        return ngx_media_srt_udp_pending_pop(session, buf, cap);
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

    if (session->listener == NULL) {
        ssize_t  n;

        n = recv(session->fd, buf, cap, 0);

        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }

        if (n == 0) {
            return 0;
        }

        session->bytes_received += (uint64_t) n;
        session->packets_received++;

        return (ngx_int_t) n;
    }

    for ( ;; ) {
        struct sockaddr_in  from;
        socklen_t           from_len = sizeof(from);
        u_char              packet[NGX_MEDIA_SRT_REFERENCE_MAX_PACKET];
        ssize_t             n;
        ngx_media_srt_session_t *other;
        size_t              take;

        /*
         * Do not consume a new peer's handshake here.  accept_ready() owns
         * that datagram; consuming it would make the peer undiscoverable.
         */
        if (recvfrom(session->fd, packet, 1, MSG_PEEK,
                     (struct sockaddr *) &from, &from_len) < 0)
        {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }

        if (from.sin_addr.s_addr != session->peer.sin_addr.s_addr
            || from.sin_port != session->peer.sin_port)
        {
            other = ngx_media_srt_udp_session_for(session->listener, &from);

            if (other == NULL) {
                return 0;
            }

            from_len = sizeof(from);
            n = recvfrom(session->fd, packet, sizeof(packet), 0,
                         (struct sockaddr *) &from, &from_len);

            if (n < 0) {
                return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
            }

            if (n > 0) {
                (void) ngx_media_srt_udp_pending_push(other, packet,
                                                       (size_t) n);
            }

            continue;
        }

        from_len = sizeof(from);
        n = recvfrom(session->fd, packet, sizeof(packet), 0,
                     (struct sockaddr *) &from, &from_len);

        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }

        if (n == 0) {
            return 0;
        }

        take = ((size_t) n < cap) ? (size_t) n : cap;
        ngx_memcpy(buf, packet, take);
        session->bytes_received += (uint64_t) take;
        session->packets_received++;

        return (ngx_int_t) take;
    }
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

    ngx_media_srt_udp_pending_clear(session);
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
    ngx_uint_t  i;

    if (scheduler == NULL || session == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < scheduler->session_count; i++) {
        if (scheduler->sessions[i] == session) {
            return NGX_OK;
        }
    }

    if (scheduler->session_count >= NGX_MEDIA_SRT_REFERENCE_MAX_SESSIONS) {
        return NGX_ERROR;
    }

    scheduler->sessions[scheduler->session_count++] = session;

    return NGX_OK;
}

static void
ngx_media_srt_udp_poll_remove_session(ngx_media_srt_poll_t *scheduler,
    ngx_media_srt_session_t *session)
{
    ngx_uint_t  i;

    if (scheduler == NULL || session == NULL) {
        return;
    }

    for (i = 0; i < scheduler->session_count; i++) {
        if (scheduler->sessions[i] != session) {
            continue;
        }

        scheduler->session_count--;
        scheduler->sessions[i] =
            scheduler->sessions[scheduler->session_count];
        scheduler->sessions[scheduler->session_count] = NULL;
        return;
    }
}

/*
 * The listener socket is shared by every accepted peer.  poll_wait therefore
 * drains established-peer datagrams into per-session bounded queues before it
 * reports readiness; otherwise one busy peer can consume and discard another
 * peer's packet.
 */
static ngx_int_t
ngx_media_srt_udp_poll_wait(ngx_media_srt_poll_t *scheduler,
    ngx_msec_t timeout_ms, ngx_media_srt_poll_event_t *events, ngx_uint_t max,
    ngx_uint_t *count)
{
    struct pollfd               pfd[
        NGX_MEDIA_SRT_REFERENCE_MAX_SESSIONS + 1];
    ngx_media_srt_session_t     *fd_sessions[
        NGX_MEDIA_SRT_REFERENCE_MAX_SESSIONS + 1];
    ngx_uint_t                   n, i, listener_at, pending;
    int                          rc;
    ngx_msec_t                   wait_ms;

    if (scheduler == NULL || events == NULL || count == NULL || max == 0) {
        return NGX_ERROR;
    }

    *count = 0;
    n = 0;
    listener_at = NGX_MEDIA_SRT_REFERENCE_MAX_SESSIONS + 1;
    pending = 0;
    wait_ms = timeout_ms;
    ngx_memzero(fd_sessions, sizeof(fd_sessions));

    if (scheduler->listener != NULL) {
        listener_at = n;
        pfd[n].fd = scheduler->listener->fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
    }

    for (i = 0; i < scheduler->session_count; i++) {
        ngx_media_srt_session_t  *session = scheduler->sessions[i];

        if (session->pending_count > 0) {
            pending = 1;
        }

        /*
         * Accepted sessions share the listener fd and are represented by
         * pending queues.  Only caller sessions contribute another poll fd.
         */
        if (session->listener != NULL) {
            continue;
        }

        pfd[n].fd = session->fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        fd_sessions[n] = session;
        n++;
    }

    if (pending) {
        wait_ms = 0;
    }

    if (n > 0) {
        rc = poll(pfd, n, (wait_ms > (ngx_msec_t) INT_MAX)
                              ? INT_MAX : (int) wait_ms);

        if (rc < 0 && errno != EINTR) {
            return NGX_ERROR;
        }
    }

    if (listener_at < n && (pfd[listener_at].revents & POLLIN)) {
        for (i = 0; i < NGX_MEDIA_SRT_REFERENCE_MAX_SESSIONS; i++) {
            struct sockaddr_in       peer;
            socklen_t                 peer_len = sizeof(peer);
            u_char                    probe[1];
            u_char                    packet[NGX_MEDIA_SRT_REFERENCE_MAX_PACKET];
            ssize_t                   peeked, received;
            ngx_media_srt_session_t  *session;

            peeked = recvfrom(scheduler->listener->fd, probe, sizeof(probe),
                              MSG_PEEK, (struct sockaddr *) &peer, &peer_len);

            if (peeked < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }

                return NGX_ERROR;
            }

            session = ngx_media_srt_udp_session_for(scheduler->listener, &peer);

            if (session == NULL) {
                if (*count < max) {
                    events[*count].listener = scheduler->listener;
                    events[*count].session = NULL;
                    (*count)++;
                }

                /*
                 * Leave a new peer's stream-id datagram at the head for
                 * accept_ready(); later established packets cannot pass it.
                 */
                break;
            }

            peer_len = sizeof(peer);
            received = recvfrom(scheduler->listener->fd, packet,
                                sizeof(packet), 0,
                                (struct sockaddr *) &peer, &peer_len);

            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }

                return NGX_ERROR;
            }

            if (received > 0) {
                (void) ngx_media_srt_udp_pending_push(session, packet,
                                                       (size_t) received);
            }
        }
    }

    /*
     * A poll event can have queued several peers.  Emit one event per ready
     * session so the event loop drains each peer without another demux pass.
     */
    for (i = 0; i < scheduler->session_count && *count < max; i++) {
        ngx_media_srt_session_t  *session = scheduler->sessions[i];

        if (session->pending_count > 0) {
            events[*count].listener = NULL;
            events[*count].session = session;
            (*count)++;
        }
    }

    for (i = 0; i < n && *count < max; i++) {
        if (fd_sessions[i] != NULL && (pfd[i].revents & POLLIN)) {
            events[*count].listener = NULL;
            events[*count].session = fd_sessions[i];
            (*count)++;
        }
    }

    return NGX_OK;
}

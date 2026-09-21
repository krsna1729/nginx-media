#include "ngx_media_ipc.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * The endpoint owns one socket of a SOCK_SEQPACKET pair.  Every message is one
 * datagram, so a receiver never sees a half message; frames larger than the
 * transport bound are split into ordered chunks by the sender.
 */

struct ngx_media_ipc_endpoint_s {
    int          fd;
    ngx_uint_t   sent_messages;
    ngx_uint_t   sent_bytes;
    ngx_uint_t   dropped;
    ngx_log_t   *log;
};

/* sizes both directions so a full datagram is never rejected for space */
static void
ngx_media_ipc_tune(int fd)
{
    int  size = NGX_MEDIA_IPC_SOCKBUF;

    (void) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    (void) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

static ngx_int_t
ngx_media_ipc_socket_pair(int fds[2])
{
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                   fds) == 0)
    {
        ngx_media_ipc_tune(fds[0]);
        ngx_media_ipc_tune(fds[1]);

        return NGX_OK;
    }

    return NGX_ERROR;
}

static ngx_media_ipc_endpoint_t *
ngx_media_ipc_endpoint_new(int fd, ngx_log_t *log)
{
    /* a payload chunk must leave room for the header in one datagram */
    if (sizeof(ngx_media_ipc_header_t) > NGX_MEDIA_IPC_HEADER_MAX) {
        return NULL;
    }

    ngx_media_ipc_endpoint_t  *endpoint;

    endpoint = ngx_alloc(sizeof(ngx_media_ipc_endpoint_t), log);

    if (endpoint == NULL) {
        return NULL;
    }

    ngx_memzero(endpoint, sizeof(ngx_media_ipc_endpoint_t));

    endpoint->fd = fd;
    endpoint->log = log;

    return endpoint;
}

ngx_int_t
ngx_media_ipc_pair_create(ngx_media_ipc_endpoint_t **local,
    ngx_media_ipc_endpoint_t **peer, ngx_log_t *log)
{
    ngx_media_ipc_endpoint_t  *a, *b;
    int                        fds[2];

    if (local == NULL || peer == NULL) {
        return NGX_ERROR;
    }

    if (ngx_media_ipc_socket_pair(fds) != NGX_OK) {
        return NGX_ERROR;
    }

    a = ngx_media_ipc_endpoint_new(fds[0], log);
    b = ngx_media_ipc_endpoint_new(fds[1], log);

    if (a == NULL || b == NULL) {
        ngx_free(a);
        ngx_free(b);
        (void) close(fds[0]);
        (void) close(fds[1]);
        return NGX_ERROR;
    }

    *local = a;
    *peer = b;

    return NGX_OK;
}

ngx_int_t
ngx_media_ipc_adopt(int fd, ngx_media_ipc_endpoint_t **endpoint, ngx_log_t *log)
{
    ngx_media_ipc_endpoint_t  *e;

    if (fd < 0 || endpoint == NULL) {
        return NGX_ERROR;
    }

    e = ngx_media_ipc_endpoint_new(fd, log);

    if (e == NULL) {
        return NGX_ERROR;
    }

    ngx_media_ipc_tune(fd);

    *endpoint = e;

    return NGX_OK;
}

int
ngx_media_ipc_fd(const ngx_media_ipc_endpoint_t *endpoint)
{
    return (endpoint != NULL) ? endpoint->fd : -1;
}

void
ngx_media_ipc_close(ngx_media_ipc_endpoint_t *endpoint)
{
    if (endpoint == NULL) {
        return;
    }

    if (endpoint->fd >= 0) {
        (void) close(endpoint->fd);
        endpoint->fd = -1;
    }

    ngx_free(endpoint);
}

/* writes one datagram; NGX_AGAIN when the peer's buffer is full */
static ngx_int_t
ngx_media_ipc_write(ngx_media_ipc_endpoint_t *endpoint, const void *header,
    size_t header_len, const u_char *payload, size_t payload_len)
{
    struct iovec  iov[2];
    struct msghdr msg;
    ssize_t       n;

    ngx_memzero(&msg, sizeof(msg));

    iov[0].iov_base = (void *) header;
    iov[0].iov_len = header_len;

    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    if (payload_len > 0) {
        iov[1].iov_base = (void *) payload;
        iov[1].iov_len = payload_len;
        msg.msg_iovlen = 2;
    }

    n = sendmsg(endpoint->fd, &msg, MSG_NOSIGNAL);

    if (n < 0) {

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
            endpoint->dropped++;
            return NGX_AGAIN;
        }

        if (errno == EINTR) {
            return NGX_AGAIN;
        }

        return NGX_ERROR;
    }

    endpoint->sent_messages++;
    endpoint->sent_bytes += (ngx_uint_t) n;

    return NGX_OK;
}

ngx_int_t
ngx_media_ipc_send(ngx_media_ipc_endpoint_t *endpoint,
    const ngx_media_ipc_header_t *header, ngx_media_buf_t *payload,
    size_t offset, size_t length)
{
    ngx_media_ipc_header_t  chunk;
    size_t                  pos = 0;

    if (endpoint == NULL || header == NULL || endpoint->fd < 0) {
        return NGX_ERROR;
    }

    if (length > 0 && payload == NULL) {
        return NGX_ERROR;
    }

    if (length > 0 && ngx_media_buf_size(payload) < offset + length) {
        return NGX_ERROR;
    }

    if (length == 0) {
        return ngx_media_ipc_write(endpoint, header,
                                   sizeof(ngx_media_ipc_header_t), NULL, 0);
    }

    /*
     * One frame may need several datagrams.  Every chunk repeats the frame
     * header with its own offset and carries the continuation flag, so a
     * receiver can reassemble or reject the frame deterministically.
     */
    while (pos < length) {
        size_t  take = length - pos;

        if (take > NGX_MEDIA_IPC_MAX_PAYLOAD) {
            take = NGX_MEDIA_IPC_MAX_PAYLOAD;
        }

        chunk = *header;
        chunk.length = (uint32_t) take;
        chunk.total = (uint32_t) length;
        chunk.offset = (uint32_t) pos;
        chunk.flags = (pos + take < length)
                          ? (uint16_t) (header->flags
                                        | NGX_MEDIA_IPC_FLAG_MORE)
                          : (uint16_t) (header->flags & ~NGX_MEDIA_IPC_FLAG_MORE);

        if (ngx_media_ipc_write(endpoint, &chunk, sizeof(chunk),
                                ngx_media_buf_data(payload) + offset + pos,
                                take) != NGX_OK)
        {
            return NGX_AGAIN;
        }

        pos += take;
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_ipc_send_header(ngx_media_ipc_endpoint_t *endpoint,
    const ngx_media_ipc_header_t *header)
{
    if (endpoint == NULL || header == NULL || endpoint->fd < 0) {
        return NGX_ERROR;
    }

    return ngx_media_ipc_write(endpoint, header, sizeof(ngx_media_ipc_header_t),
                               NULL, 0);
}

ngx_int_t
ngx_media_ipc_recv(ngx_media_ipc_endpoint_t *endpoint,
    ngx_media_ipc_message_t *message)
{
    u_char                   buf[NGX_MEDIA_IPC_MAX_DATAGRAM];
    ngx_media_ipc_header_t   header;
    ngx_media_buf_t         *payload;
    ssize_t                  n;

    if (endpoint == NULL || message == NULL || endpoint->fd < 0) {
        return NGX_ERROR;
    }

    ngx_memzero(message, sizeof(ngx_media_ipc_message_t));

    n = recv(endpoint->fd, buf, sizeof(buf), 0);

    if (n < 0) {

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return NGX_AGAIN;
        }

        if (errno == EINTR) {
            return NGX_AGAIN;
        }

        return NGX_ERROR;
    }

    if (n == 0) {
        return NGX_ERROR;   /* peer closed */
    }

    if ((size_t) n < sizeof(ngx_media_ipc_header_t)) {
        return NGX_ERROR;
    }

    ngx_memcpy(&header, buf, sizeof(header));

    if (header.version != NGX_MEDIA_IPC_VERSION
        || header.length > NGX_MEDIA_IPC_MAX_PAYLOAD
        || (size_t) n != sizeof(header) + header.length)
    {
        return NGX_ERROR;
    }

    message->header = header;
    message->offset = 0;
    message->length = header.length;

    if (header.length > 0) {
        payload = ngx_media_buf_alloc(header.length);

        if (payload == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(ngx_media_buf_data(payload),
                   buf + sizeof(ngx_media_ipc_header_t), header.length);
        (void) ngx_media_buf_freeze(payload, header.length);

        message->payload = payload;
    }

    return NGX_OK;
}

void
ngx_media_ipc_message_release(ngx_media_ipc_message_t *message)
{
    if (message == NULL) {
        return;
    }

    if (message->payload != NULL) {
        ngx_media_buf_unref(message->payload);
        message->payload = NULL;
    }
}

ngx_int_t
ngx_media_ipc_frame_feed(ngx_media_ipc_frame_t *frame,
    const ngx_media_ipc_message_t *message)
{
    if (frame == NULL || message == NULL) {
        return NGX_ERROR;
    }

    /* a frame starts with the first chunk that is not a continuation */
    if (!frame->active) {

        if (message->header.offset != 0) {
            return NGX_ERROR;   /* a middle chunk without its start */
        }

        if (message->header.total == 0
            || message->header.total > NGX_MEDIA_IPC_MAX_FRAME)
        {
            return NGX_ERROR;
        }

        frame->payload = ngx_media_buf_alloc(message->header.total);

        if (frame->payload == NULL) {
            return NGX_ERROR;
        }

        frame->header = message->header;
        frame->capacity = message->header.total;
        frame->received = 0;
        frame->active = 1;
    }

    if (message->header.offset != frame->received
        || frame->received + message->length > frame->capacity)
    {
        ngx_media_ipc_frame_reset(frame);
        return NGX_ERROR;
    }

    if (message->length > 0) {
        ngx_memcpy(ngx_media_buf_data(frame->payload) + frame->received,
                   ngx_media_buf_data(message->payload), message->length);
    }

    frame->received += message->length;

    if (message->header.flags & NGX_MEDIA_IPC_FLAG_MORE) {
        return NGX_AGAIN;
    }

    if (frame->received != frame->capacity) {
        ngx_media_ipc_frame_reset(frame);
        return NGX_ERROR;
    }

    (void) ngx_media_buf_freeze(frame->payload, frame->capacity);

    return NGX_OK;
}

void
ngx_media_ipc_frame_reset(ngx_media_ipc_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    if (frame->payload != NULL) {
        ngx_media_buf_unref(frame->payload);
        frame->payload = NULL;
    }

    frame->capacity = 0;
    frame->received = 0;
    frame->active = 0;
}

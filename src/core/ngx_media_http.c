#include "ngx_media_hls_profile.h"
#include "ngx_media_http.h"

#include <openssl/ssl.h>

/*
 * Callers may have no logger - a worker thread that was handed none, a test
 * harness - and a library that crashes on that is a library with a landmine.
 * Every diagnostic goes through this.
 */
#define NGX_MEDIA_HTTP_LOG(level, log, ...)                                   \
    do {                                                                      \
        if ((log) != NULL) {                                                  \
            ngx_log_error((level), (log), 0, __VA_ARGS__);                    \
        }                                                                     \
    } while (0)

#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <poll.h>

#define NGX_MEDIA_HTTP_PATH_MAX  512

/*
 * Every blocking wait in this client is bounded.
 *
 * Both callers run on a thread whose whole job is one HTTP operation: an HLS
 * pull reader fetching a playlist or a segment, and a push uploader moving a
 * segment to a remote.  Without a deadline the wait is the operating system's
 * own - minutes on a connect, none at all on a socket that was accepted and
 * then abandoned - so one remote that accepts and never answers parks that
 * thread for as long as it likes.  On the push side that is worse than it
 * sounds: four such remotes occupy the whole upload pool, and every other
 * destination stops being served.
 *
 * The connect bound is per address; the I/O bound is per wait, not per
 * transfer, so a large segment to a slow-but-progressing remote is not cut
 * off - it is a remote that has stopped moving for this long that is given up
 * on.
 */
#define NGX_MEDIA_HTTP_CONNECT_TIMEOUT_MS  5000
#define NGX_MEDIA_HTTP_IO_TIMEOUT_MS      10000

/*
 * Split an http:// URL into host, port and target path.  Only plain HTTP is
 * accepted: TLS would need a client context, and both callers say so rather
 * than failing obscurely.
 */
static ngx_int_t
ngx_media_http_split(const ngx_str_t *url, ngx_str_t *host, ngx_int_t *port,
    ngx_str_t *target, ngx_uint_t *tls, ngx_log_t *log)
{
    u_char  *colon, *slash;

    if (url->len >= 8 && ngx_memcmp(url->data, "https://", 8) == 0) {
        *tls = 1;
        *port = 443;
        host->data = url->data + 8;
        host->len = url->len - 8;

    } else if (url->len >= 8 && ngx_memcmp(url->data, "http://", 7) == 0) {
        *tls = 0;
        *port = 80;
        host->data = url->data + 7;
        host->len = url->len - 7;

    } else {
        NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                      "media: endpoint must be http:// or https://");
        return NGX_DECLINED;
    }

    slash = ngx_strlchr(host->data, host->data + host->len, '/');

    if (slash == NULL) {
        target->data = (u_char *) "/";
        target->len = 1;

    } else {
        target->data = slash;
        target->len = host->len - (slash - host->data);
        host->len = slash - host->data;
    }

    /*
     * The target and host go verbatim into the request line.  Reject
     * controls and spaces so a configured URL cannot smuggle extra headers
     * or a second request into the socket write.
     */
    {
        u_char  *p;

        for (p = target->data; p < target->data + target->len; p++) {
            if (*p <= 0x20 || *p == 0x7F) {
                return NGX_ERROR;
            }
        }

        for (p = host->data; p < host->data + host->len; p++) {
            if (*p <= 0x20 || *p == 0x7F || *p == '#') {
                return NGX_ERROR;
            }
        }

        if (target->data[0] != '/') {
            return NGX_ERROR;
        }
    }

    colon = ngx_strlchr(host->data, host->data + host->len, ':');

    if (colon != NULL) {
        u_char  text[8];
        size_t  len = host->len - (colon - host->data) - 1;

        if (len == 0 || len >= sizeof(text)) {
            return NGX_ERROR;
        }

        ngx_memcpy(text, colon + 1, len);
        text[len] = '\0';
        *port = atoi((char *) text);

        if (*port <= 0 || *port > 65535) {
            return NGX_ERROR;
        }

        host->len = colon - host->data;
    }

    if (host->len == 0) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t ngx_media_http_connect_one(ngx_int_t fd,
    const struct addrinfo *rp);
static void ngx_media_http_deadline(ngx_int_t fd);

static ngx_int_t
ngx_media_http_connect(const ngx_str_t *host, ngx_int_t port, ngx_log_t *log)
{
    struct addrinfo  hints, *res, *rp;
    char             host_z[256];
    char             port_z[16];
    ngx_int_t        fd = -1;

    if (host->len >= sizeof(host_z)) {
        return NGX_ERROR;
    }

    ngx_memcpy(host_z, host->data, host->len);
    host_z[host->len] = '\0';

    snprintf(port_z, sizeof(port_z), "%d", (int) port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host_z, port_z, &hints, &res) != 0) {
        NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                      "media: could not resolve host %V", host);
        return NGX_ERROR;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (ngx_media_http_connect_one(fd, rp) == NGX_OK) {
            break;
        }

        (void) close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    return fd;
}

/*
 * One address, connected with a deadline.
 *
 * A blocking connect() waits for the kernel's own timeout, which is minutes,
 * and it cannot be shortened portably through an option.  So the socket is put
 * in non-blocking mode for the handshake, waited for with poll(), and put back
 * afterwards: the rest of this client is written for a blocking socket, and a
 * send or a read that returns EAGAIN must mean the I/O deadline rather than
 * "the connect is still going".
 */
static ngx_int_t
ngx_media_http_connect_one(ngx_int_t fd, const struct addrinfo *rp)
{
    struct pollfd  pfd;
    socklen_t      len;
    ngx_int_t      flags;
    int            err = 0;

    flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return NGX_ERROR;
    }

    if (connect(fd, rp->ai_addr, rp->ai_addrlen) != 0
        && errno != EINPROGRESS)
    {
        return NGX_ERROR;
    }

    pfd.fd = fd;
    pfd.events = POLLOUT;

    if (poll(&pfd, 1, NGX_MEDIA_HTTP_CONNECT_TIMEOUT_MS) <= 0) {
        return NGX_ERROR;   /* no answer, or no room to wait for one */
    }

    len = sizeof(err);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        return NGX_ERROR;
    }

    if (fcntl(fd, F_SETFL, flags) == -1) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

/*
 * The I/O deadline, on the socket itself: a read or a write that finds no
 * progress for this long fails with EAGAIN, which is what both callers treat
 * as a dead peer.  It is set before the TLS handshake, so a handshake that
 * stalls is bounded the same way.
 */
static void
ngx_media_http_deadline(ngx_int_t fd)
{
    struct timeval  tv;

    tv.tv_sec = NGX_MEDIA_HTTP_IO_TIMEOUT_MS / 1000;
    tv.tv_usec = (NGX_MEDIA_HTTP_IO_TIMEOUT_MS % 1000) * 1000;

    (void) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void) setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/*
 * Wraps a connected socket in TLS, with kTLS requested where the kernel
 * offers it.  Verification is on unless ca_file says otherwise, so a private
 * CA or a test certificate is trusted deliberately rather than by disabling
 * the check.
 */
static void *
ngx_media_http_tls_start(ngx_int_t fd, const ngx_str_t *host,
    const ngx_str_t *ca_file, ngx_log_t *log)
{
    SSL_CTX  *ctx;
    SSL      *ssl;
    char      host_z[256];

    if (host->len >= sizeof(host_z)) {
        return NULL;
    }

    ngx_memcpy(host_z, host->data, host->len);
    host_z[host->len] = '\0';

    ctx = SSL_CTX_new(TLS_client_method());

    if (ctx == NULL) {
        return NULL;
    }

    /*
     * kTLS: the kernel does the record layer, which is what lets sendfile be
     * used on a TLS connection at all.  Asking is not a guarantee - it needs
     * a kernel with the tls module - so the transfer path checks whether it
     * took effect rather than assuming.
     */
    (void) SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
#ifdef SSL_OP_ENABLE_KTLS_TX_ZEROCOPY_SENDFILE
    (void) SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS_TX_ZEROCOPY_SENDFILE);
#endif

    if (ca_file != NULL && ca_file->len > 0) {
        char  ca_z[512];

        if (ca_file->len >= sizeof(ca_z)) {
            SSL_CTX_free(ctx);
            return NULL;
        }

        ngx_memcpy(ca_z, ca_file->data, ca_file->len);
        ca_z[ca_file->len] = '\0';

        if (SSL_CTX_load_verify_locations(ctx, ca_z, NULL) != 1) {
            NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                          "media: could not load ca file %V", ca_file);
            SSL_CTX_free(ctx);
            return NULL;
        }

    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);

    if (ssl == NULL) {
        return NULL;
    }

    if (SSL_set1_host(ssl, host_z) != 1) {
        NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                      "media: could not set TLS verification hostname %V", host);
        SSL_free(ssl);
        return NULL;
    }

    if (SSL_set_fd(ssl, (int) fd) != 1
        || SSL_set_tlsext_host_name(ssl, host_z) != 1
        || SSL_connect(ssl) != 1)
    {
        NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                      "media: TLS handshake with %V failed", host);
        SSL_free(ssl);
        return NULL;
    }

    return ssl;
}

/*
 * A deadline shows up as EAGAIN, which is what the callers branch on.  OpenSSL
 * reports a socket that timed out as a retryable condition rather than as
 * EAGAIN, so the distinction is put back here: a caller that treated
 * WANT_READ as "wait a little longer" would spin on a peer that has stopped
 * answering for as long as the socket stays open.
 */
static ssize_t
ngx_media_http_read(ngx_int_t fd, void *ssl, u_char *buf, size_t len)
{
    ssize_t  n;
    int      err;

    if (ssl == NULL) {
        return read(fd, buf, len);
    }

    n = (ssize_t) SSL_read((SSL *) ssl, buf, (int) len);

    if (n > 0) {
        return n;
    }

    err = SSL_get_error((SSL *) ssl, (int) n);

    if ((err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        errno = EAGAIN;
    }

    return n;
}

static ssize_t
ngx_media_http_write(ngx_int_t fd, void *ssl, const u_char *buf, size_t len)
{
    ssize_t  n;
    int      err;

    if (ssl == NULL) {
        /* a kept connection the remote closed must not raise SIGPIPE */
        return send(fd, buf, len, MSG_NOSIGNAL);
    }

    n = (ssize_t) SSL_write((SSL *) ssl, buf, (int) len);

    if (n > 0) {
        return n;
    }

    err = SSL_get_error((SSL *) ssl, (int) n);

    if ((err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        errno = EAGAIN;
    }

    return n;
}

/* whether kTLS is actually carrying this connection's records */
static ngx_uint_t
ngx_media_http_ktls_active(void *ssl)
{
    if (ssl == NULL) {
        return 0;
    }

#ifdef BIO_get_ktls_send
    if (BIO_get_ktls_send(SSL_get_wbio((SSL *) ssl)) == 1) {
        return 1;
    }
#endif

    return 0;
}

ngx_int_t
ngx_media_http_get(const ngx_str_t *url, const ngx_str_t *ca_file,
    u_char *buf, size_t cap, size_t *len, ngx_log_t *log)
{
    ngx_str_t   host, target;
    ngx_int_t   port, fd, rc;
    ngx_uint_t  tls = 0;
    void       *ssl = NULL;
    u_char      header[1024];
    int         header_len;
    size_t      body = 0;
    ssize_t     n;
    u_char     *status;

    *len = 0;

    rc = ngx_media_http_split(url, &host, &port, &target, &tls, log);

    if (rc != NGX_OK) {
        return rc;
    }

    fd = ngx_media_http_connect(&host, port, log);

    if (fd < 0) {
        return NGX_ERROR;
    }

    ngx_media_http_deadline(fd);

    if (tls) {
        ssl = ngx_media_http_tls_start(fd, &host, ca_file, log);

        if (ssl == NULL) {
            (void) close(fd);
            return NGX_ERROR;
        }
    }

    header_len = snprintf((char *) header, sizeof(header),
                          "GET %.*s HTTP/1.1\r\n"
                          "Host: %.*s\r\n"
                          "Connection: close\r\n\r\n",
                          (int) target.len, (char *) target.data,
                          (int) host.len, (char *) host.data);

    if (header_len < 0 || (size_t) header_len >= sizeof(header)) {
        if (ssl != NULL) {
            SSL_free((SSL *) ssl);
        }
        (void) close(fd);
        return NGX_ERROR;
    }

    if (ngx_media_http_write(fd, ssl, header, (size_t) header_len)
        != header_len)
    {
        if (ssl != NULL) {
            SSL_free((SSL *) ssl);
        }

        (void) close(fd);
        return NGX_ERROR;
    }

    while (body + 1 < cap) {

        n = ngx_media_http_read(fd, ssl, buf + body, cap - body - 1);

        if (n > 0) {
            body += (size_t) n;
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;   /* a signal, not the peer */
        }

        /*
         * EAGAIN is the socket deadline: nothing arrived for
         * NGX_MEDIA_HTTP_IO_TIMEOUT_MS.  What was read so far is what the
         * origin sent before it stopped.
         */
        break;
    }

    if (ssl != NULL) {
        SSL_free((SSL *) ssl);
    }

    (void) close(fd);

    buf[body] = '\0';

    status = (u_char *) ngx_strlchr(buf, buf + body, ' ');

    if (status == NULL || status + 1 >= buf + body || status[1] != '2') {
        return NGX_ERROR;
    }

    /* the body starts after the header terminator */
    {
        u_char  *sep = (u_char *) strstr((char *) buf, "\r\n\r\n");

        if (sep != NULL) {
            size_t  skip = (size_t) (sep + 4 - buf);
            size_t  remain = body - skip;

            ngx_memmove(buf, buf + skip, remain);
            buf[remain] = '\0';
            *len = remain;
        }
    }

    return NGX_OK;
}

/* the whole buffer, or the count written before the connection failed */
static ssize_t
ngx_media_http_write_all(ngx_int_t fd, void *ssl, const u_char *buf,
    size_t len)
{
    size_t   done = 0;
    ssize_t  n;

    while (done < len) {
        n = ngx_media_http_write(fd, ssl, buf + done, len - done);

        if (n > 0) {
            done += (size_t) n;
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        break;
    }

    return (ssize_t) done;
}

/* --- requests on a kept connection ------------------------------------- */

void
ngx_media_http_conn_init(ngx_media_http_conn_t *c)
{
    ngx_memzero(c, sizeof(*c));
    c->fd = -1;
}

void
ngx_media_http_conn_close(ngx_media_http_conn_t *c)
{
    if (c->ssl != NULL) {
        SSL_free((SSL *) c->ssl);
        c->ssl = NULL;
    }

    if (c->fd >= 0) {
        (void) close(c->fd);
        c->fd = -1;
    }

    c->key[0] = '\0';
}

/*
 * The body of a PUT, in the fewest copies this connection allows.
 *
 * Plain or kTLS: sendfile, so the page cache goes straight to the socket
 * and the kernel encrypts if there is anything to encrypt.  sendfile is not
 * a server-side privilege - it works on any socket, including this client
 * connection - and with kTLS it is the only way to keep a TLS upload out of
 * user space, which is what TLS_TX_ZEROCOPY_RO exists for.
 *
 * Userspace TLS: SSL_write, because OpenSSL needs plaintext in user space
 * and cannot be handed a file descriptor.  That is one extra copy per
 * destination, and it is reported rather than pretended away.
 */
static off_t
ngx_media_http_send_body(ngx_int_t fd, void *ssl, int file_fd, off_t size)
{
    off_t    sent = 0, offset = 0;
    ssize_t  n;

    if (ssl == NULL || ngx_media_http_ktls_active(ssl)) {

        while (sent < size) {

            n = sendfile(fd, file_fd, &offset, (size_t) (size - sent));

            if (n > 0) {
                sent += n;
                continue;
            }

            if (n < 0 && errno == EINTR) {
                continue;   /* a signal, not the remote */
            }

            /*
             * EAGAIN is the socket deadline, and on a send it means the
             * remote stopped reading for NGX_MEDIA_HTTP_IO_TIMEOUT_MS.  The
             * transfer is then short, which the caller fails: a truncated
             * segment must not be published.
             */
            break;
        }

        return sent;
    }

    {
        u_char  buf[16384];
        size_t  want;

        while (sent < size) {
            want = (size - sent > (off_t) sizeof(buf))
                       ? sizeof(buf) : (size_t) (size - sent);
            n = pread(file_fd, buf, want, offset);
            if (n > 0) {
                if (ngx_media_http_write_all(fd, ssl, buf, (size_t) n) != n) {
                    break;
                }
                sent += n;
                offset += n;
                continue;
            }

            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    return sent;
}

/* a header's value, case-insensitively, or NULL; hdr is NUL-terminated */
static const char *
ngx_media_http_header(const char *hdr, const char *name)
{
    size_t       len = strlen(name);
    const char  *p = strstr(hdr, "\r\n");

    while (p != NULL && p[2] != '\r' && p[2] != '\0') {
        p += 2;
        if (strncasecmp(p, name, len) == 0 && p[len] == ':') {
            p += len + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            return p;
        }
        p = strstr(p, "\r\n");
    }

    return NULL;
}

/*
 * Reads one response: the header block, then as much body as the header
 * says, so the next request on the connection starts clean.  Returns the
 * status, or NGX_ERROR; *reusable says whether the connection may carry
 * another request.
 */
static ngx_int_t
ngx_media_http_response(ngx_int_t fd, void *ssl, ngx_uint_t *reusable)
{
    char         buf[4096];
    size_t       have = 0, header_len;
    ssize_t      n;
    char        *end;
    const char  *v;
    long long    length = -1, remain;
    ngx_int_t    status;

    *reusable = 0;

    for ( ;; ) {
        if (have + 1 >= sizeof(buf)) {
            return NGX_ERROR;           /* a header larger than we accept */
        }

        n = ngx_media_http_read(fd, ssl, (u_char *) buf + have,
                                sizeof(buf) - have - 1);
        if (n > 0) {
            have += (size_t) n;
            buf[have] = '\0';
            if ((end = strstr(buf, "\r\n\r\n")) != NULL) {
                break;
            }
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        return NGX_ERROR;               /* closed or timed out, no status */
    }

    header_len = (size_t) (end - buf) + 4;
    end[2] = '\0';                      /* headers end at the blank line */

    if (have < 12 || strncmp(buf, "HTTP/1.", 7) != 0
        || buf[9] < '1' || buf[9] > '5')
    {
        return NGX_ERROR;
    }

    status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');

    if ((v = ngx_media_http_header(buf, "Content-Length")) != NULL) {
        length = atoll(v);
    }

    *reusable = (buf[7] == '1');        /* HTTP/1.1 defaults to keep-alive */

    if ((v = ngx_media_http_header(buf, "Connection")) != NULL
        && strncasecmp(v, "close", 5) == 0)
    {
        *reusable = 0;
    }

    if (ngx_media_http_header(buf, "Transfer-Encoding") != NULL) {
        *reusable = 0;                  /* not decoded: the body ends at close */
        return status;
    }

    if (status == 204 || status == 304 || (status >= 100 && status < 200)) {
        return status;
    }

    if (length < 0) {
        *reusable = 0;                  /* the body runs to close */
        return status;
    }

    /* discard the body: what is already buffered, then the rest */
    remain = length - (long long) (have - header_len);

    while (remain > 0) {
        n = ngx_media_http_read(fd, ssl, (u_char *) buf,
                                (size_t) (remain < (long long) sizeof(buf)
                                          ? remain : (long long) sizeof(buf)));
        if (n > 0) {
            remain -= n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        *reusable = 0;
        break;
    }

    return status;
}

ngx_int_t
ngx_media_http_send(ngx_media_http_conn_t *c, const ngx_str_t *url,
    const ngx_str_t *ca_file, const char *method, const u_char *name,
    int file_fd, const u_char *buf, off_t size, const char *content_type,
    ngx_log_t *log)
{
    ngx_str_t   host, target;
    ngx_int_t   port = 80, rc, status;
    ngx_uint_t  tls = 0, attempt, reused, reusable;
    u_char      header[1200];
    int         header_len;
    u_char      full[NGX_MEDIA_HTTP_PATH_MAX];
    u_char      key[sizeof(c->key)];
    ngx_uint_t  body = (file_fd >= 0 || buf != NULL);

    if (name == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_media_http_split(url, &host, &port, &target, &tls, log);

    if (rc != NGX_OK) {
        return rc;
    }

    /*
     * Where the object's name goes.  A path endpoint is a directory: the
     * name follows it after a slash.  An endpoint with a query string names
     * the object in a parameter, which is YouTube's form
     * (http_upload_hls?cid=KEY&copy=0&file=NAME): the name completes a
     * trailing "file=" or "=", or is appended as "&file=NAME".
     */
    if (target.len + 8 + strlen((char *) name) >= sizeof(full)) {
        return NGX_ERROR;
    }

    ngx_memcpy(full, target.data, target.len);
    full[target.len] = '\0';

    if (ngx_strlchr(target.data, target.data + target.len, '?') != NULL) {
        if (target.data[target.len - 1] != '=') {
            strcat((char *) full,
                   (target.data[target.len - 1] == '?'
                    || target.data[target.len - 1] == '&')
                       ? "file=" : "&file=");
        }

    } else if (target.data[target.len - 1] != '/') {
        strcat((char *) full, "/");
    }

    strcat((char *) full, (char *) name);

    target.data = full;
    target.len = strlen((char *) full);

    (void) snprintf((char *) key, sizeof(key), "%u|%.*s|%d|%.*s",
                    (unsigned) tls, (int) host.len, (char *) host.data,
                    (int) port,
                    (ca_file != NULL) ? (int) ca_file->len : 0,
                    (ca_file != NULL && ca_file->data != NULL)
                        ? (char *) ca_file->data : "");

    if (body) {
        header_len = snprintf((char *) header, sizeof(header),
                              "%s %.*s HTTP/1.1\r\n"
                              "Host: %.*s\r\n"
                              "User-Agent: nginx-media\r\n"
                              "Content-Length: %lld\r\n"
                              "Content-Type: %s\r\n"
                              "Connection: keep-alive\r\n\r\n",
                              method, (int) target.len, (char *) target.data,
                              (int) host.len, (char *) host.data,
                              (long long) size,
                              content_type != NULL ? content_type
                                                   : "application/octet-stream");
    } else {
        header_len = snprintf((char *) header, sizeof(header),
                              "%s %.*s HTTP/1.1\r\n"
                              "Host: %.*s\r\n"
                              "User-Agent: nginx-media\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: keep-alive\r\n\r\n",
                              method, (int) target.len, (char *) target.data,
                              (int) host.len, (char *) host.data);
    }

    if (header_len < 0 || (size_t) header_len >= sizeof(header)) {
        return NGX_ERROR;
    }

    /*
     * A kept connection may have been closed by the remote since its last
     * request; that is found out only by using it, so a request that fails
     * on a reused connection before any status arrives is sent once more on
     * a new one.  A request on a new connection is not repeated.
     */
    for (attempt = 0; attempt < 2; attempt++) {

        reused = (c->fd >= 0 && strcmp((char *) c->key, (char *) key) == 0);

        if (!reused) {
            ngx_media_http_conn_close(c);

            c->fd = (int) ngx_media_http_connect(&host, port, log);
            if (c->fd < 0) {
                c->fd = -1;
                return NGX_ERROR;
            }

            ngx_media_http_deadline(c->fd);

            if (tls) {
                c->ssl = ngx_media_http_tls_start(c->fd, &host, ca_file, log);
                if (c->ssl == NULL) {
                    ngx_media_http_conn_close(c);
                    return NGX_ERROR;
                }

                NGX_MEDIA_HTTP_LOG(NGX_LOG_INFO, log,
                              ngx_media_http_ktls_active(c->ssl)
                                  ? "media: upload used kTLS sendfile (no "
                                    "user-space copy) to %V"
                                  : "media: upload to %V is userspace TLS; "
                                    "kTLS would remove a copy per object",
                              url);
            }

            ngx_memcpy(c->key, key, sizeof(key));
            c->connects++;
        }

        if (ngx_media_http_write_all(c->fd, c->ssl, header,
                                     (size_t) header_len)
            != header_len)
        {
            ngx_media_http_conn_close(c);
            if (reused) {
                continue;
            }
            return NGX_ERROR;
        }

        if (body) {
            off_t  sent;

            if (file_fd >= 0) {
                sent = ngx_media_http_send_body(c->fd, c->ssl, file_fd, size);

            } else {
                sent = ngx_media_http_write_all(c->fd, c->ssl, buf,
                                                (size_t) size);
            }


            /* a short transfer would publish a truncated object */
            if (sent != size) {
                NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                              "media: hls push sent %O of %O bytes of %s",
                              sent, size, name);
                ngx_media_http_conn_close(c);
                if (reused && sent == 0) {
                    continue;
                }
                return NGX_ERROR;
            }
        }

        status = ngx_media_http_response(c->fd, c->ssl, &reusable);

        if (status == NGX_ERROR) {
            ngx_media_http_conn_close(c);
            if (reused) {
                continue;
            }
            return NGX_ERROR;
        }

        c->requests++;

        if (!reusable) {
            ngx_media_http_conn_close(c);
        }

        if (status < 200 || status >= 300) {
            NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                          "media: hls push %s %s got %i from the endpoint",
                          method, name, status);
        }

        return status;
    }

    return NGX_ERROR;
}

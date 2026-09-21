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

#define NGX_MEDIA_HTTP_PATH_MAX  512

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
        host->len = colon - host->data;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_media_http_connect(const ngx_str_t *host, ngx_int_t port, ngx_log_t *log)
{
    struct sockaddr_in  addr;
    ngx_int_t           fd;
    char                host_z[256];

    if (host->len >= sizeof(host_z)) {
        return NGX_ERROR;
    }

    ngx_memcpy(host_z, host->data, host->len);
    host_z[host->len] = '\0';

    ngx_memzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);

    if (inet_pton(AF_INET, host_z, &addr.sin_addr) != 1) {
        return NGX_ERROR;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return NGX_ERROR;
    }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        (void) close(fd);
        return NGX_ERROR;
    }

    return fd;
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

static ssize_t
ngx_media_http_read(ngx_int_t fd, void *ssl, u_char *buf, size_t len)
{
    if (ssl != NULL) {
        return (ssize_t) SSL_read((SSL *) ssl, buf, (int) len);
    }

    return read(fd, buf, len);
}

static ssize_t
ngx_media_http_write(ngx_int_t fd, void *ssl, const u_char *buf, size_t len)
{
    if (ssl != NULL) {
        return (ssize_t) SSL_write((SSL *) ssl, buf, (int) len);
    }

    return write(fd, buf, len);
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

        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }

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

/*
 * One blocking PUT of a whole file.  The body goes out with sendfile: the
 * page cache hands its pages straight to the socket, with no user-space
 * bounce, so an upload costs one kernel-side copy per destination instead of
 * two.  sendfile is not a server-side privilege - it works on any socket,
 * including this client connection.
 */
ngx_int_t
ngx_media_http_put_file(const ngx_str_t *url, const ngx_str_t *ca_file,
    const u_char *path, off_t size, ngx_log_t *log)
{
    ngx_str_t   host, target;
    ngx_int_t   port = 80;
    ngx_int_t   fd, rc;
    ngx_uint_t  tls = 0;
    void       *ssl = NULL;
    struct sockaddr_in  addr;
    ngx_int_t   file_fd;
    off_t       sent = 0, offset = 0;
    ssize_t     n;
    u_char      header[1024];
    int         header_len;
    u_char      response[512];
    ssize_t     rn;

    rc = ngx_media_http_split(url, &host, &port, &target, &tls, log);

    if (rc != NGX_OK) {
        return rc;
    }

    /* the destination directory is the endpoint's prefix */
    {
        u_char  full[NGX_MEDIA_HTTP_PATH_MAX];
        u_char *base = (u_char *) strrchr((char *) path, '/');

        if (target.len + 1 + (base != NULL ? strlen((char *) base + 1) : 0)
            >= sizeof(full))
        {
            return NGX_ERROR;
        }

        ngx_memcpy(full, target.data, target.len);
        full[target.len] = '\0';

        if (target.data[target.len - 1] != '/') {
            full[target.len] = '/';
            full[target.len + 1] = '\0';
        }

        if (base != NULL) {
            strncat((char *) full, (char *) base + 1,
                    sizeof(full) - strlen((char *) full) - 1);
        }

        target.data = ngx_pnalloc(ngx_cycle->pool, strlen((char *) full));
        if (target.data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(target.data, full, strlen((char *) full));
        target.len = strlen((char *) full);
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return NGX_ERROR;
    }

    ngx_memzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);

    {
        char  host_z[256];

        if (host.len >= sizeof(host_z)) {
            (void) close(fd);
            return NGX_ERROR;
        }

        ngx_memcpy(host_z, host.data, host.len);
        host_z[host.len] = '\0';

        if (inet_pton(AF_INET, host_z, &addr.sin_addr) != 1) {
            (void) close(fd);
            return NGX_ERROR;
        }
    }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        (void) close(fd);
        return NGX_ERROR;
    }

    if (tls) {
        ssl = ngx_media_http_tls_start(fd, &host, ca_file, log);

        if (ssl == NULL) {
            (void) close(fd);
            return NGX_ERROR;
        }
    }

    file_fd = open((char *) path, O_RDONLY);
    if (file_fd < 0) {
        (void) close(fd);
        return NGX_ERROR;
    }

    header_len = snprintf((char *) header, sizeof(header),
                          "PUT %.*s HTTP/1.1\r\n"
                          "Host: %.*s\r\n"
                          "Content-Length: %lld\r\n"
                          "Content-Type: video/mp2t\r\n"
                          "Connection: close\r\n\r\n",
                          (int) target.len, (char *) target.data,
                          (int) host.len, (char *) host.data,
                          (long long) size);

    if (ngx_media_http_write(fd, ssl, header, (size_t) header_len)
        != header_len)
    {
        (void) close(file_fd);

        if (ssl != NULL) {
            SSL_free((SSL *) ssl);
        }

        (void) close(fd);
        return NGX_ERROR;
    }

    /*
     * The body takes the fewest copies this connection allows.
     *
     * Plain or kTLS: sendfile, so the page cache goes straight to the socket
     * and the kernel encrypts if there is anything to encrypt.  sendfile is
     * not a server-side privilege - it works on any socket, including this
     * client connection - and with kTLS it is the only way to keep a TLS
     * upload out of user space, which is what TLS_TX_ZEROCOPY_RO exists for.
     *
     * Userspace TLS: SSL_write, because OpenSSL needs plaintext in user space
     * and cannot be handed a file descriptor.  That is one extra copy per
     * destination, and it is reported rather than pretended away.
     */
    if (ssl == NULL || ngx_media_http_ktls_active(ssl)) {

        while (sent < size) {

            n = sendfile(fd, file_fd, &offset, (size_t) (size - sent));

            if (n > 0) {
                sent += n;
                continue;
            }

            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            }

            break;
        }

        if (ssl != NULL && sent == size) {
            NGX_MEDIA_HTTP_LOG(NGX_LOG_INFO, log,
                          "media: upload used kTLS sendfile (no user-space "
                          "copy) to %V", url);
        }

    } else {
        u_char  buf[16384];
        size_t  got;

        NGX_MEDIA_HTTP_LOG(NGX_LOG_INFO, log,
                      "media: upload to %V is userspace TLS, so the body "
                      "goes through user space; kTLS would remove that copy",
                      url);

        while ((got = (size_t) read(file_fd, buf, sizeof(buf))) > 0) {

            if (ngx_media_http_write(fd, ssl, buf, got) != (ssize_t) got) {
                break;
            }

            sent += (off_t) got;
        }
    }

    /* a short transfer would publish a truncated segment: fail rather than lie */
    if (sent != size) {
        NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                      "media: hls push sent %O of %O bytes from %s",
                      sent, size, path);
        (void) close(file_fd);

        if (ssl != NULL) {
            SSL_free((SSL *) ssl);
        }

        (void) close(fd);
        return NGX_ERROR;
    }

    (void) close(file_fd);

    rn = ngx_media_http_read(fd, ssl, response, sizeof(response) - 1);

    if (ssl != NULL) {
        SSL_free((SSL *) ssl);
    }

    (void) close(fd);

    if (rn <= 0) {
        return NGX_ERROR;
    }

    response[rn] = '\0';

    /* "HTTP/1.1 2xx" is the whole contract */
    if (response[9] != '2') {
        NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                      "media: hls push got %.3s from the endpoint",
                      response + 9);
        return NGX_ERROR;
    }

    return NGX_OK;
}


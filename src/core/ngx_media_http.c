#include "ngx_media_http.h"

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
    ngx_str_t *target, ngx_log_t *log)
{
    u_char  *colon, *slash;

    if (url->len < 8 || ngx_memcmp(url->data, "http://", 7) != 0) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: only http:// endpoints are supported");
        return NGX_DECLINED;
    }

    *port = 80;

    host->data = url->data + 7;
    host->len = url->len - 7;

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

ngx_int_t
ngx_media_http_get(const ngx_str_t *url, u_char *buf, size_t cap, size_t *len,
    ngx_log_t *log)
{
    ngx_str_t   host, target;
    ngx_int_t   port, fd, rc;
    u_char      header[1024];
    int         header_len;
    size_t      body = 0;
    ssize_t     n;
    u_char     *status;

    *len = 0;

    rc = ngx_media_http_split(url, &host, &port, &target, log);

    if (rc != NGX_OK) {
        return rc;
    }

    fd = ngx_media_http_connect(&host, port, log);

    if (fd < 0) {
        return NGX_ERROR;
    }

    header_len = snprintf((char *) header, sizeof(header),
                          "GET %.*s HTTP/1.1\r\n"
                          "Host: %.*s\r\n"
                          "Connection: close\r\n\r\n",
                          (int) target.len, (char *) target.data,
                          (int) host.len, (char *) host.data);

    if (write(fd, header, (size_t) header_len) != header_len) {
        (void) close(fd);
        return NGX_ERROR;
    }

    while (body + 1 < cap) {

        n = read(fd, buf + body, cap - body - 1);

        if (n > 0) {
            body += (size_t) n;
            continue;
        }

        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }

        break;
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
ngx_media_http_put_file(const ngx_str_t *url, const u_char *path,
    off_t size, ngx_log_t *log)
{
    ngx_str_t   host, target;
    u_char     *colon, *slash;
    ngx_int_t   port = 80;
    ngx_int_t   fd;
    struct sockaddr_in  addr;
    ngx_int_t   file_fd;
    off_t       sent = 0, offset = 0;
    ssize_t     n;
    u_char      header[1024];
    int         header_len;
    u_char      response[512];
    ssize_t     rn;

    if (url->len < 8 || ngx_memcmp(url->data, "http://", 7) != 0) {
        /* only plain HTTP for now; TLS uploads need a client context */
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: hls push supports http:// endpoints only");
        return NGX_ERROR;
    }

    host.data = url->data + 7;
    host.len = url->len - 7;

    slash = ngx_strlchr(host.data, host.data + host.len, '/');
    if (slash == NULL) {
        target.data = (u_char *) "/";
        target.len = 1;

    } else {
        target.data = slash;
        target.len = host.len - (slash - host.data);
        host.len = slash - host.data;
    }

    colon = ngx_strlchr(host.data, host.data + host.len, ':');
    if (colon != NULL) {
        u_char  port_text[8];
        size_t  len = host.len - (colon - host.data) - 1;

        if (len == 0 || len >= sizeof(port_text)) {
            return NGX_ERROR;
        }

        ngx_memcpy(port_text, colon + 1, len);
        port_text[len] = '\0';
        port = atoi((char *) port_text);
        host.len = colon - host.data;
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

    if (write(fd, header, (size_t) header_len) != header_len) {
        (void) close(file_fd);
        (void) close(fd);
        return NGX_ERROR;
    }

    /*
     * The body goes out with sendfile: the page cache hands its pages
     * straight to the socket, with no user-space bounce.  sendfile is not a
     * server-side privilege - it works on any socket, including this client
     * connection - so an upload costs one kernel-side copy per destination
     * instead of two.  It is also why the body is not buffered here: a
     * fread/write loop would undo exactly the saving this is for.
     */
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

    /* a short transfer would publish a truncated segment: fail rather than lie */
    if (sent != size) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: hls push sent %O of %O bytes from %s",
                      sent, size, path);
        (void) close(file_fd);
        (void) close(fd);
        return NGX_ERROR;
    }

    (void) close(file_fd);

    rn = read(fd, response, sizeof(response) - 1);
    (void) close(fd);

    if (rn <= 0) {
        return NGX_ERROR;
    }

    response[rn] = '\0';

    /* "HTTP/1.1 2xx" is the whole contract */
    if (response[9] != '2') {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: hls push got %.3s from the endpoint",
                      response + 9);
        return NGX_ERROR;
    }

    return NGX_OK;
}


#include "ngx_media_http.h"

#include <ares.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NGX_MEDIA_HTTP_PATH_MAX       512
#define NGX_MEDIA_HTTP_HEADER_MAX     16384
#define NGX_MEDIA_HTTP_LINE_MAX       8192
#define NGX_MEDIA_HTTP_RESPONSE_MAX   (1024 * 1024)
#define NGX_MEDIA_HTTP_TIMEOUT_MS     10000
#define NGX_MEDIA_HTTP_CANCEL_MS      50
#define NGX_MEDIA_HTTP_DNS_SOCKETS    16

#define NGX_MEDIA_HTTP_LOG(level, log, ...)                                   \
    do {                                                                      \
        if ((log) != NULL) {                                                   \
            ngx_log_error((level), (log), 0, __VA_ARGS__);                      \
        }                                                                     \
    } while (0)

typedef struct {
    ngx_media_http_conn_t  *conn;
    uint64_t                end_ns;
} ngx_media_http_operation_t;

typedef struct {
    ngx_media_http_operation_t  *op;
    u_char                       data[8192];
    size_t                       pos, end;
} ngx_media_http_reader_t;

typedef struct {
    struct pollfd          sockets[NGX_MEDIA_HTTP_DNS_SOCKETS];
    struct ares_addrinfo  *result;
    int                    done, failed;
} ngx_media_http_dns_t;

static pthread_once_t ngx_media_http_dns_once = PTHREAD_ONCE_INIT;
static int ngx_media_http_dns_init_status;

static uint64_t
ngx_media_http_now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t) ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static ngx_int_t
ngx_media_http_remaining(ngx_media_http_operation_t *op)
{
    uint64_t now, ms;

    if (ngx_atomic_fetch_add(&op->conn->cancelled, 0)) {
        errno = ECANCELED;
        return NGX_ERROR;
    }
    now = ngx_media_http_now_ns();
    if (now == 0 || now >= op->end_ns) {
        errno = ETIMEDOUT;
        return NGX_ERROR;
    }
    ms = (op->end_ns - now + 999999) / 1000000;
    return (ngx_int_t) (ms > NGX_MEDIA_HTTP_CANCEL_MS
                       ? NGX_MEDIA_HTTP_CANCEL_MS : ms);
}

static ngx_int_t
ngx_media_http_begin(ngx_media_http_operation_t *op, ngx_media_http_conn_t *c)
{
    uint64_t now = ngx_media_http_now_ns();

    op->conn = c;
    if (now == 0 || c->timeout_ms == 0
        || c->timeout_ms > (UINT64_MAX - now) / 1000000)
    {
        return NGX_ERROR;
    }
    op->end_ns = now + (uint64_t) c->timeout_ms * 1000000;
    return ngx_media_http_remaining(op) < 0 ? NGX_ERROR : NGX_OK;
}

static ngx_int_t
ngx_media_http_wait(ngx_media_http_operation_t *op, short events)
{
    struct pollfd pfd;
    int rc;
    ngx_int_t ms;

    pfd.fd = op->conn->fd;
    pfd.events = events;
    for ( ;; ) {
        ms = ngx_media_http_remaining(op);
        if (ms < 0) {
            return NGX_ERROR;
        }
        rc = poll(&pfd, 1, (int) ms);
        if (rc > 0) {
            return (pfd.revents & POLLNVAL) ? NGX_ERROR : NGX_OK;
        }
        if (rc < 0 && errno != EINTR) {
            return NGX_ERROR;
        }
    }
}

static ngx_int_t
ngx_media_http_split(const ngx_str_t *url, ngx_str_t *host, ngx_int_t *port,
    ngx_str_t *target, ngx_uint_t *tls)
{
    u_char *authority, *end, *slash, *colon, *p;

    if (url == NULL || url->data == NULL) {
        return NGX_ERROR;
    }
    if (url->len >= 8 && ngx_memcmp(url->data, "https://", 8) == 0) {
        *tls = 1;
        *port = 443;
        authority = url->data + 8;
    } else if (url->len >= 7 && ngx_memcmp(url->data, "http://", 7) == 0) {
        *tls = 0;
        *port = 80;
        authority = url->data + 7;
    } else {
        return NGX_DECLINED;
    }
    end = url->data + url->len;
    slash = ngx_strlchr(authority, end, '/');
    target->data = slash != NULL ? slash : (u_char *) "/";
    target->len = slash != NULL ? (size_t) (end - slash) : 1;
    end = slash != NULL ? slash : end;
    for (p = authority; p < end; p++) {
        if (*p <= 0x20 || *p >= 0x7f || *p == '#' || *p == '?' || *p == '@') {
            return NGX_ERROR;
        }
    }
    for (p = target->data; p < target->data + target->len; p++) {
        if (*p <= 0x20 || *p == 0x7f || *p == '#') {
            return NGX_ERROR;
        }
    }
    host->data = authority;
    if (authority < end && *authority == '[') {
        p = ngx_strlchr(authority, end, ']');
        if (p == NULL) {
            return NGX_ERROR;
        }
        host->data++;
        host->len = (size_t) (p - host->data);
        colon = p + 1;
        if (colon < end && *colon != ':') {
            return NGX_ERROR;
        }
    } else {
        colon = ngx_strlchr(authority, end, ':');
        if (colon == NULL) {
            colon = end;
        }
        host->len = (size_t) (colon - authority);
    }
    if (host->len == 0 || host->len >= 256) {
        return NGX_ERROR;
    }
    if (colon < end) {
        *port = 0;
        for (p = colon + 1; p < end; p++) {
            if (*p < '0' || *p > '9' || *port > 6553) {
                return NGX_ERROR;
            }
            *port = *port * 10 + (*p - '0');
        }
        if (*port == 0 || *port > 65535) {
            return NGX_ERROR;
        }
    }
    return NGX_OK;
}

static void
ngx_media_http_dns_init(void)
{
    /* Process-wide initialization only; channels and queries are owner-local.
     * No resolver/event threads are enabled, including during cancellation. */
    ngx_media_http_dns_init_status = ares_library_init(ARES_LIB_INIT_ALL);
}

static void
ngx_media_http_dns_socket(void *data, ares_socket_t fd, int rd, int wr)
{
    ngx_media_http_dns_t *dns = data;
    size_t i, slot = NGX_MEDIA_HTTP_DNS_SOCKETS;

    for (i = 0; i < NGX_MEDIA_HTTP_DNS_SOCKETS; i++) {
        if (dns->sockets[i].fd == fd) {
            slot = i;
            break;
        }
        if (dns->sockets[i].fd == -1) {
            slot = i;
        }
    }
    if (slot == NGX_MEDIA_HTTP_DNS_SOCKETS) {
        dns->failed = 1;
        return;
    }
    dns->sockets[slot].fd = (rd || wr) ? fd : -1;
    dns->sockets[slot].events = (rd ? POLLIN : 0) | (wr ? POLLOUT : 0);
}

static void
ngx_media_http_dns_result(void *data, int status, int timeouts,
    struct ares_addrinfo *result)
{
    ngx_media_http_dns_t *dns = data;

    (void) timeouts;
    dns->done = 1;
    dns->failed |= (status != ARES_SUCCESS);
    dns->result = result;
}

static ngx_int_t
ngx_media_http_connect(ngx_media_http_operation_t *op, const ngx_str_t *host,
    ngx_int_t port)
{
    ares_channel channel;
    struct ares_options options;
    struct ares_addrinfo_hints hints;
    struct ares_addrinfo_node *rp;
    ngx_media_http_dns_t dns;
    ngx_media_http_operation_t attempt;
    uint64_t now;
    size_t addresses;
    struct pollfd ready[NGX_MEDIA_HTTP_DNS_SOCKETS];
    char host_z[256], port_z[8];
    struct timeval limit, tv, *wait;
    size_t i;
    int rc, err;
    socklen_t errlen;
    ngx_int_t ms;

    if (ngx_media_http_remaining(op) < 0) {
        return NGX_ERROR;
    }
    (void) pthread_once(&ngx_media_http_dns_once, ngx_media_http_dns_init);
    if (ngx_media_http_dns_init_status != ARES_SUCCESS) {
        return NGX_ERROR;
    }
    ngx_memzero(&dns, sizeof(dns));
    for (i = 0; i < NGX_MEDIA_HTTP_DNS_SOCKETS; i++) {
        dns.sockets[i].fd = -1;
    }
    ngx_memzero(&options, sizeof(options));
    options.sock_state_cb = ngx_media_http_dns_socket;
    options.sock_state_cb_data = &dns;
    if (ares_init_options(&channel, &options, ARES_OPT_SOCK_STATE_CB)
        != ARES_SUCCESS)
    {
        return NGX_ERROR;
    }
    ngx_memcpy(host_z, host->data, host->len);
    host_z[host->len] = '\0';
    (void) snprintf(port_z, sizeof(port_z), "%d", (int) port);
    ngx_memzero(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = ARES_AI_NUMERICSERV | ARES_AI_NOSORT;
    ares_getaddrinfo(channel, host_z, port_z, &hints,
                     ngx_media_http_dns_result, &dns);
    while (!dns.done && !dns.failed) {
        ms = ngx_media_http_remaining(op);
        if (ms < 0) {
            dns.failed = 1;
            break;
        }
        limit.tv_sec = 0;
        limit.tv_usec = ms * 1000;
        wait = ares_timeout(channel, &limit, &tv);
        ms = (ngx_int_t) (wait->tv_sec * 1000 + (wait->tv_usec + 999) / 1000);
        ngx_memcpy(ready, dns.sockets, sizeof(ready));
        rc = poll(ready, NGX_MEDIA_HTTP_DNS_SOCKETS, (int) ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            dns.failed = 1;
            break;
        }
        if (rc == 0) {
            ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
        } else {
            for (i = 0; i < NGX_MEDIA_HTTP_DNS_SOCKETS && !dns.done; i++) {
                if (ready[i].revents != 0) {
                    ares_process_fd(channel,
                        (ready[i].revents & (POLLIN | POLLERR | POLLHUP))
                            ? ready[i].fd : ARES_SOCKET_BAD,
                        (ready[i].revents & POLLOUT)
                            ? ready[i].fd : ARES_SOCKET_BAD);
                }
            }
        }
    }
    /* Cancellation callbacks finish synchronously before stack storage dies. */
    ares_cancel(channel);
    ares_destroy(channel);
    if (!dns.failed && dns.result != NULL) {
        addresses = 0;
        for (rp = dns.result->nodes; rp != NULL; rp = rp->ai_next) {
            addresses++;
        }
        for (rp = dns.result->nodes; rp != NULL; rp = rp->ai_next, addresses--) {
            if (ngx_media_http_remaining(op) < 0) {
                break;
            }
            now = ngx_media_http_now_ns();
            if (now == 0 || now >= op->end_ns) {
                break;
            }
            attempt = *op;
            attempt.end_ns = now + (op->end_ns - now) / addresses;
            op->conn->fd = socket(rp->ai_family,
                rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, rp->ai_protocol);
            if (op->conn->fd < 0) {
                continue;
            }
            rc = connect(op->conn->fd, rp->ai_addr, rp->ai_addrlen);
            if (rc == 0 || (errno == EINPROGRESS
                           && ngx_media_http_wait(&attempt, POLLOUT) == NGX_OK))
            {
                err = 0;
                errlen = sizeof(err);
                if (getsockopt(op->conn->fd, SOL_SOCKET, SO_ERROR,
                               &err, &errlen) == 0 && err == 0)
                {
                    break;
                }
            }
            (void) close(op->conn->fd);
            op->conn->fd = -1;
        }
    }
    if (dns.result != NULL) {
        ares_freeaddrinfo(dns.result);
    }
    return op->conn->fd < 0 ? NGX_ERROR : NGX_OK;
}

static ngx_int_t
ngx_media_http_tls_start(ngx_media_http_operation_t *op, const ngx_str_t *host,
    const ngx_str_t *ca_file, ngx_log_t *log)
{
    SSL_CTX *ctx;
    SSL *ssl;
    char host_z[256], ca_z[512];
    unsigned char ip[16];
    int rc, err;

    ngx_memcpy(host_z, host->data, host->len);
    host_z[host->len] = '\0';
    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        return NGX_ERROR;
    }
#ifdef SSL_OP_ENABLE_KTLS
    (void) SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
#endif
#ifdef SSL_OP_ENABLE_KTLS_TX_ZEROCOPY_SENDFILE
    (void) SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS_TX_ZEROCOPY_SENDFILE);
#endif
    if (ca_file != NULL && ca_file->len > 0) {
        if (ca_file->len >= sizeof(ca_z)) {
            SSL_CTX_free(ctx);
            return NGX_ERROR;
        }
        ngx_memcpy(ca_z, ca_file->data, ca_file->len);
        ca_z[ca_file->len] = '\0';
        rc = SSL_CTX_load_verify_locations(ctx, ca_z, NULL);
    } else {
        rc = SSL_CTX_set_default_verify_paths(ctx);
    }
    if (rc != 1) {
        SSL_CTX_free(ctx);
        return NGX_ERROR;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);
    if (ssl == NULL) {
        return NGX_ERROR;
    }
    op->conn->ssl = ssl;
    if (inet_pton(AF_INET, host_z, ip) == 1
        || inet_pton(AF_INET6, host_z, ip) == 1)
    {
        rc = X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host_z);
    } else {
        rc = SSL_set1_host(ssl, host_z);
        if (rc == 1) {
            rc = SSL_set_tlsext_host_name(ssl, host_z);
        }
    }
    if (rc != 1 || SSL_set_fd(ssl, op->conn->fd) != 1) {
        return NGX_ERROR;
    }
    for ( ;; ) {
        if (ngx_media_http_remaining(op) < 0) {
            return NGX_ERROR;
        }
        ERR_clear_error();
        errno = 0;
        rc = SSL_connect(ssl);
        if (rc == 1) {
            return NGX_OK;
        }
        err = SSL_get_error(ssl, rc);
        if (err == SSL_ERROR_SYSCALL && errno == EINTR) {
            continue;
        }
        if ((err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
            || ngx_media_http_wait(op, err == SSL_ERROR_WANT_READ
                                      ? POLLIN : POLLOUT) != NGX_OK)
        {
            NGX_MEDIA_HTTP_LOG(NGX_LOG_WARN, log,
                              "media: TLS handshake failed");
            return NGX_ERROR;
        }
    }
}

static ssize_t
ngx_media_http_io(ngx_media_http_operation_t *op, u_char *buf, size_t len,
    ngx_uint_t writing)
{
    ssize_t n;
    int err;
    short events;
    SSL *ssl = op->conn->ssl;

    if (len > INT_MAX) {
        len = INT_MAX;
    }
    for ( ;; ) {
        if (ngx_media_http_remaining(op) < 0) {
            return -1;
        }
        events = writing ? POLLOUT : POLLIN;
        if (ssl == NULL) {
            n = writing ? send(op->conn->fd, buf, len, MSG_NOSIGNAL)
                        : recv(op->conn->fd, buf, len, 0);
            if (n >= 0) {
                return n;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return -1;
            }
        } else {
            ERR_clear_error();
            errno = 0;
            n = writing ? SSL_write(ssl, buf, (int) len)
                        : SSL_read(ssl, buf, (int) len);
            if (n > 0) {
                return n;
            }
            err = SSL_get_error(ssl, (int) n);
            if (err == SSL_ERROR_SYSCALL && errno == EINTR) {
                continue;
            }
            if (!writing && err == SSL_ERROR_ZERO_RETURN) {
                return 0;
            }
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                return -1;
            }
            events = err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        }
        if (ngx_media_http_wait(op, events) != NGX_OK) {
            return -1;
        }
    }
}

static ngx_int_t
ngx_media_http_write_all(ngx_media_http_operation_t *op, const u_char *buf,
    size_t len)
{
    ssize_t n;

    while (len != 0) {
        n = ngx_media_http_io(op, (u_char *) buf, len, 1);
        if (n <= 0) {
            return NGX_ERROR;
        }
        buf += n;
        len -= (size_t) n;
    }
    return NGX_OK;
}

static ngx_uint_t
ngx_media_http_ktls_active(void *ssl)
{
#ifdef BIO_get_ktls_send
    if (ssl != NULL && BIO_get_ktls_send(SSL_get_wbio((SSL *) ssl)) == 1) {
        return 1;
    }
#else
    (void) ssl;
#endif
    return 0;
}

void
ngx_media_http_conn_init(ngx_media_http_conn_t *c)
{
    ngx_memzero(c, sizeof(*c));
    c->fd = -1;
    c->timeout_ms = NGX_MEDIA_HTTP_TIMEOUT_MS;
}

void
ngx_media_http_conn_cancel(ngx_media_http_conn_t *c)
{
    (void) ngx_atomic_cmp_set(&c->cancelled, 0, 1);
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

static ngx_int_t
ngx_media_http_open(ngx_media_http_operation_t *op, const ngx_str_t *host,
    ngx_int_t port, ngx_uint_t tls, const ngx_str_t *ca_file, ngx_log_t *log)
{
    if (ngx_media_http_connect(op, host, port) != NGX_OK
        || (tls && ngx_media_http_tls_start(op, host, ca_file, log) != NGX_OK))
    {
        ngx_media_http_conn_close(op->conn);
        return NGX_ERROR;
    }
    return NGX_OK;
}

static ngx_int_t
ngx_media_http_fill(ngx_media_http_reader_t *r)
{
    ssize_t n;

    if (r->pos < r->end) {
        return 1;
    }
    n = ngx_media_http_io(r->op, r->data, sizeof(r->data), 0);
    if (n <= 0) {
        return n == 0 ? 0 : NGX_ERROR;
    }
    r->pos = 0;
    r->end = (size_t) n;
    return 1;
}

static ngx_int_t
ngx_media_http_line(ngx_media_http_reader_t *r, char *line, size_t cap,
    size_t *budget)
{
    size_t have = 0;
    u_char c;

    for ( ;; ) {
        if (*budget == 0 || ngx_media_http_fill(r) != 1) {
            return NGX_ERROR;
        }
        c = r->data[r->pos++];
        (*budget)--;
        if (c == '\r') {
            if (*budget == 0 || ngx_media_http_fill(r) != 1
                || r->data[r->pos++] != '\n')
            {
                return NGX_ERROR;
            }
            (*budget)--;
            line[have] = '\0';
            return NGX_OK;
        }
        if (c == '\n' || c == '\0' || have + 1 >= cap) {
            return NGX_ERROR;
        }
        line[have++] = (char) c;
    }
}

static ngx_uint_t
ngx_media_http_token(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9')
           || (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static char *
ngx_media_http_field(char *line)
{
    char *p = line, *v, *end;

    while (ngx_media_http_token((unsigned char) *p)) {
        p++;
    }
    if (p == line || *p != ':') {
        return NULL;
    }
    *p++ = '\0';
    for (v = p; *v; v++) {
        if (((unsigned char) *v < 0x20 && *v != '\t') || *v == 0x7f) {
            return NULL;
        }
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return p;
}

static ngx_int_t
ngx_media_http_content_length(char *v, uint64_t *length, ngx_uint_t *seen)
{
    uint64_t n;

    for ( ;; ) {
        n = 0;
        if (*v < '0' || *v > '9') {
            return NGX_ERROR;
        }
        do {
            if (n > (UINT64_MAX - (unsigned) (*v - '0')) / 10) {
                return NGX_ERROR;
            }
            n = n * 10 + (unsigned) (*v++ - '0');
        } while (*v >= '0' && *v <= '9');
        if (*seen && *length != n) {
            return NGX_ERROR;
        }
        *length = n;
        *seen = 1;
        while (*v == ' ' || *v == '\t') {
            v++;
        }
        if (*v == '\0') {
            return NGX_OK;
        }
        if (*v++ != ',') {
            return NGX_ERROR;
        }
        while (*v == ' ' || *v == '\t') {
            v++;
        }
    }
}

static ngx_int_t
ngx_media_http_connection(char *v, ngx_uint_t *close_conn,
    ngx_uint_t *keepalive)
{
    char *p;
    size_t n;

    for ( ;; ) {
        p = v;
        while (ngx_media_http_token((unsigned char) *v)) {
            v++;
        }
        n = (size_t) (v - p);
        if (n == 0) {
            return NGX_ERROR;
        }
        if (n == 5 && strncasecmp(p, "close", n) == 0) {
            *close_conn = 1;
        }
        if (n == 10 && strncasecmp(p, "keep-alive", n) == 0) {
            *keepalive = 1;
        }
        while (*v == ' ' || *v == '\t') {
            v++;
        }
        if (*v == '\0') {
            return NGX_OK;
        }
        if (*v++ != ',') {
            return NGX_ERROR;
        }
        while (*v == ' ' || *v == '\t') {
            v++;
        }
    }
}

static ngx_int_t
ngx_media_http_bytes(ngx_media_http_reader_t *r, uint64_t count,
    u_char *buf, size_t cap, size_t *have)
{
    size_t n;

    if (count > cap - *have) {
        return NGX_ERROR;
    }
    while (count != 0) {
        if (ngx_media_http_fill(r) != 1) {
            return NGX_ERROR;
        }
        n = r->end - r->pos;
        if ((uint64_t) n > count) {
            n = (size_t) count;
        }
        if (buf != NULL) {
            ngx_memcpy(buf + *have, r->data + r->pos, n);
        }
        r->pos += n;
        *have += n;
        count -= n;
    }
    return NGX_OK;
}

static ngx_int_t
ngx_media_http_chunk_size(char *p, uint64_t *size)
{
    unsigned digit;
    char *start = p;

    *size = 0;
    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')
           || (*p >= 'A' && *p <= 'F'))
    {
        digit = (*p <= '9') ? (unsigned) (*p - '0')
                           : (unsigned) ((*p | 0x20) - 'a' + 10);
        if (*size > (UINT64_MAX - digit) / 16) {
            return NGX_ERROR;
        }
        *size = *size * 16 + digit;
        p++;
    }
    if (p == start) {
        return NGX_ERROR;
    }
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p++ != ';') {
            return NGX_ERROR;
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        start = p;
        while (ngx_media_http_token((unsigned char) *p)) {
            p++;
        }
        if (p == start) {
            return NGX_ERROR;
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '=') {
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '"') {
            p++;
            while (*p != '"') {
                if (*p == '\\') {
                    p++;
                }
                if ((unsigned char) *p < 0x20 && *p != '\t') {
                    return NGX_ERROR;
                }
                if (*p == 0x7f) {
                    return NGX_ERROR;
                }
                p++;
            }
            p++;
        } else {
            start = p;
            while (ngx_media_http_token((unsigned char) *p)) {
                p++;
            }
            if (p == start) {
                return NGX_ERROR;
            }
        }
    }
    return NGX_OK;
}

static ngx_int_t
ngx_media_http_response(ngx_media_http_operation_t *op, u_char *buf,
    size_t cap, size_t *len, ngx_uint_t *reusable, ngx_uint_t *received)
{
    ngx_media_http_reader_t r;
    char line[NGX_MEDIA_HTTP_LINE_MAX], *v;
    size_t budget = NGX_MEDIA_HTTP_HEADER_MAX, chunk_budget, have = 0;
    uint64_t length, chunk;
    ngx_uint_t cl, te, close_conn, keepalive, version, interim = 0;
    ngx_int_t status, rc;

    ngx_memzero(&r, sizeof(r));
    r.op = op;
    *reusable = 0;
    *received = 0;
    *len = 0;
    for ( ;; ) {
        if (ngx_media_http_line(&r, line, sizeof(line), &budget) != NGX_OK) {
            *received = (r.end != 0);
            return NGX_ERROR;
        }
        *received = 1;
        if (strlen(line) < 13 || strncmp(line, "HTTP/1.", 7) != 0
            || (line[7] != '0' && line[7] != '1') || line[8] != ' '
            || line[9] < '1' || line[9] > '5'
            || line[10] < '0' || line[10] > '9'
            || line[11] < '0' || line[11] > '9' || line[12] != ' ')
        {
            return NGX_ERROR;
        }
        for (v = line + 13; *v; v++) {
            if (((unsigned char) *v < 0x20 && *v != '\t') || *v == 0x7f) {
                return NGX_ERROR;
            }
        }
        version = (line[7] == '1');
        status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
        cl = te = close_conn = keepalive = 0;
        length = 0;
        for ( ;; ) {
            if (ngx_media_http_line(&r, line, sizeof(line), &budget) != NGX_OK) {
                return NGX_ERROR;
            }
            if (line[0] == '\0') {
                break;
            }
            v = ngx_media_http_field(line);
            if (v == NULL) {
                return NGX_ERROR;
            }
            if (strcasecmp(line, "Content-Length") == 0) {
                if (ngx_media_http_content_length(v, &length, &cl) != NGX_OK) {
                    return NGX_ERROR;
                }
            } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
                /* No content decoder is provided: only chunked is supported. */
                if (te || !version || strcasecmp(v, "chunked") != 0) {
                    return NGX_ERROR;
                }
                te = 1;
            } else if (strcasecmp(line, "Connection") == 0) {
                if (ngx_media_http_connection(v, &close_conn, &keepalive)
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }
            }
        }
        /* These statuses terminate at the header boundary regardless of
         * representation-length metadata supplied by the origin. */
        if (status == 204 || status == 304) {
            cl = te = 0;
        }
        if ((cl && te) || (status < 200 && (cl || te))) {
            return NGX_ERROR;
        }
        if (status >= 200) {
            break;
        }
        if (status == 101 || ++interim > 8) {
            return NGX_ERROR;
        }
    }
    *reusable = !close_conn && (version || keepalive);
    if (status == 204 || status == 304) {
        goto complete;
    }
    if (te) {
        for ( ;; ) {
            chunk_budget = NGX_MEDIA_HTTP_LINE_MAX;
            if (ngx_media_http_line(&r, line, sizeof(line), &chunk_budget)
                != NGX_OK || ngx_media_http_chunk_size(line, &chunk) != NGX_OK)
            {
                return NGX_ERROR;
            }
            if (chunk == 0) {
                break;
            }
            if (ngx_media_http_bytes(&r, chunk, buf, cap, &have) != NGX_OK) {
                return NGX_ERROR;
            }
            chunk_budget = 2;
            if (ngx_media_http_line(&r, line, sizeof(line), &chunk_budget)
                != NGX_OK || line[0] != '\0')
            {
                return NGX_ERROR;
            }
        }
        budget = NGX_MEDIA_HTTP_HEADER_MAX;
        for ( ;; ) {
            if (ngx_media_http_line(&r, line, sizeof(line), &budget) != NGX_OK) {
                return NGX_ERROR;
            }
            if (line[0] == '\0') {
                break;
            }
            if (ngx_media_http_field(line) == NULL
                || strcasecmp(line, "Content-Length") == 0
                || strcasecmp(line, "Transfer-Encoding") == 0
                || strcasecmp(line, "Connection") == 0)
            {
                return NGX_ERROR;
            }
        }
    } else if (cl) {
        if (ngx_media_http_bytes(&r, length, buf, cap, &have) != NGX_OK) {
            return NGX_ERROR;
        }
    } else {
        *reusable = 0;
        for ( ;; ) {
            rc = ngx_media_http_fill(&r);
            if (rc == 0) {
                break;
            }
            if (rc < 0 || ngx_media_http_bytes(&r, r.end - r.pos, buf, cap,
                                              &have) != NGX_OK)
            {
                return NGX_ERROR;
            }
        }
    }
complete:
    /* No pipelined request was sent, so buffered surplus is not another reply. */
    if (r.pos != r.end || ngx_media_http_remaining(op) < 0) {
        return NGX_ERROR;
    }
    *len = have;
    return status;
}

ngx_int_t
ngx_media_http_get(ngx_media_http_conn_t *c, const ngx_str_t *url,
    const ngx_str_t *ca_file, u_char *buf, size_t cap, size_t *len,
    ngx_log_t *log)
{
    ngx_media_http_operation_t op;
    ngx_str_t host, target;
    ngx_int_t port, rc;
    ngx_uint_t tls, reusable, received;
    char header[1024];
    int n;
    size_t have = 0;

    *len = 0;
    if (cap == 0 || buf == NULL) {
        return NGX_ERROR;
    }
    buf[0] = '\0';
    ngx_media_http_conn_close(c);
    rc = ngx_media_http_split(url, &host, &port, &target, &tls);
    if (rc != NGX_OK || ngx_media_http_begin(&op, c) != NGX_OK) {
        return rc != NGX_OK ? rc : NGX_ERROR;
    }
    n = snprintf(header, sizeof(header),
                 "GET %.*s HTTP/1.1\r\nHost: %s%.*s%s:%d\r\n"
                 "Connection: close\r\n\r\n", (int) target.len, target.data,
                 memchr(host.data, ':', host.len) ? "[" : "",
                 (int) host.len, host.data,
                 memchr(host.data, ':', host.len) ? "]" : "", (int) port);
    if (n < 0 || (size_t) n >= sizeof(header)
        || ngx_media_http_open(&op, &host, port, tls, ca_file, log) != NGX_OK
        || ngx_media_http_write_all(&op, (u_char *) header, (size_t) n) != NGX_OK)
    {
        ngx_media_http_conn_close(c);
        return NGX_ERROR;
    }
    rc = ngx_media_http_response(&op, buf, cap - 1, &have, &reusable, &received);
    ngx_media_http_conn_close(c);
    if (rc < 200 || rc >= 300) {
        buf[0] = '\0';
        return NGX_ERROR;
    }
    buf[have] = '\0';
    *len = have;
    return NGX_OK;
}

static ngx_int_t
ngx_media_http_send_body(ngx_media_http_operation_t *op, int file_fd, off_t size)
{
    off_t offset = 0;
    ssize_t n;
    size_t want;
    u_char buf[16384];

    while (offset < size) {
        if (ngx_media_http_remaining(op) < 0) {
            return NGX_ERROR;
        }
        want = size - offset > INT_MAX ? INT_MAX : (size_t) (size - offset);
        if (op->conn->ssl == NULL || ngx_media_http_ktls_active(op->conn->ssl)) {
            n = sendfile(op->conn->fd, file_fd, &offset, want);
            if (n > 0 || (n < 0 && errno == EINTR)) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)
                && ngx_media_http_wait(op, POLLOUT) == NGX_OK)
            {
                continue;
            }
            return NGX_ERROR;
        }
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        n = pread(file_fd, buf, want, offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0 || ngx_media_http_write_all(op, buf, (size_t) n) != NGX_OK) {
            return NGX_ERROR;
        }
        offset += n;
    }
    return NGX_OK;
}

ngx_int_t
ngx_media_http_send(ngx_media_http_conn_t *c, const ngx_str_t *url,
    const ngx_str_t *ca_file, const char *method, const u_char *name,
    int file_fd, const u_char *buf, off_t size, const char *content_type,
    ngx_log_t *log)
{
    ngx_media_http_operation_t op;
    ngx_str_t host, target;
    ngx_int_t port, rc, status;
    ngx_uint_t tls, attempt, reused, reusable, received;
    char header[1200];
    int n;
    u_char full[NGX_MEDIA_HTTP_PATH_MAX], key[sizeof(c->key)];
    size_t response_len;
    const u_char *p;

    if (name == NULL || size < 0 || (uint64_t) size > SIZE_MAX
        || method == NULL || (strcmp(method, "PUT") != 0
                              && strcmp(method, "POST") != 0
                              && strcmp(method, "DELETE") != 0))
    {
        return NGX_ERROR;
    }
    for (p = name; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f || *p == '#') {
            return NGX_ERROR;
        }
    }
    if (content_type != NULL) {
        for (p = (const u_char *) content_type; *p; p++) {
            if (*p < 0x20 || *p == 0x7f) {
                return NGX_ERROR;
            }
        }
    }
    if (file_fd < 0 && buf == NULL && size != 0) {
        return NGX_ERROR;
    }
    rc = ngx_media_http_split(url, &host, &port, &target, &tls);
    if (rc != NGX_OK || ngx_media_http_begin(&op, c) != NGX_OK) {
        return rc != NGX_OK ? rc : NGX_ERROR;
    }
    if (target.len + 8 + strlen((const char *) name) >= sizeof(full)) {
        return NGX_ERROR;
    }
    ngx_memcpy(full, target.data, target.len);
    full[target.len] = '\0';
    if (ngx_strlchr(target.data, target.data + target.len, '?') != NULL) {
        if (target.data[target.len - 1] != '=') {
            strcat((char *) full, (target.data[target.len - 1] == '?'
                                  || target.data[target.len - 1] == '&')
                                  ? "file=" : "&file=");
        }
    } else if (target.data[target.len - 1] != '/') {
        strcat((char *) full, "/");
    }
    strcat((char *) full, (const char *) name);
    n = snprintf((char *) key, sizeof(key), "%u|%.*s|%d|%.*s",
                 (unsigned) tls, (int) host.len, host.data, (int) port,
                 ca_file != NULL ? (int) ca_file->len : 0,
                 ca_file != NULL && ca_file->data != NULL ? ca_file->data
                                                         : (u_char *) "");
    if (n < 0 || (size_t) n >= sizeof(key)) {
        return NGX_ERROR;
    }
    n = snprintf(header, sizeof(header),
                 "%s %s HTTP/1.1\r\nHost: %s%.*s%s:%d\r\n"
                 "User-Agent: nginx-media\r\nContent-Length: %lld\r\n"
                 "Content-Type: %s\r\nConnection: keep-alive\r\n\r\n",
                 method, full, memchr(host.data, ':', host.len) ? "[" : "",
                 (int) host.len, host.data,
                 memchr(host.data, ':', host.len) ? "]" : "", (int) port,
                 (long long) size, content_type != NULL ? content_type
                                                      : "application/octet-stream");
    if (n < 0 || (size_t) n >= sizeof(header)) {
        return NGX_ERROR;
    }
    for (attempt = 0; attempt < 2; attempt++) {
        reused = c->fd >= 0 && strcmp((char *) c->key, (char *) key) == 0;
        if (!reused) {
            ngx_media_http_conn_close(c);
            if (ngx_media_http_open(&op, &host, port, tls, ca_file, log) != NGX_OK) {
                return NGX_ERROR;
            }
            if (tls) {
                NGX_MEDIA_HTTP_LOG(NGX_LOG_INFO, log,
                    ngx_media_http_ktls_active(c->ssl)
                    ? "media: upload used kTLS sendfile (no user-space copy)"
                    : "media: upload uses userspace TLS; kTLS would remove "
                      "a copy per object");
            }
            memcpy(c->key, key, (size_t) strlen((char *) key) + 1);
            c->connects++;
        }
        if (ngx_media_http_write_all(&op, (u_char *) header, (size_t) n) != NGX_OK) {
            ngx_media_http_conn_close(c);
            if (reused && ngx_media_http_remaining(&op) >= 0) {
                continue;
            }
            return NGX_ERROR;
        }
        if ((file_fd >= 0 && ngx_media_http_send_body(&op, file_fd, size)
                            != NGX_OK)
            || (file_fd < 0 && buf != NULL
                && ngx_media_http_write_all(&op, buf, (size_t) size) != NGX_OK))
        {
            ngx_media_http_conn_close(c);
            return NGX_ERROR;
        }
        status = ngx_media_http_response(&op, NULL, NGX_MEDIA_HTTP_RESPONSE_MAX,
                                         &response_len, &reusable, &received);
        if (status == NGX_ERROR) {
            ngx_media_http_conn_close(c);
            if (reused && !received && ngx_media_http_remaining(&op) >= 0) {
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
                "media: hls push %s got HTTP status %i", method, status);
        }
        return status;
    }
    return NGX_ERROR;
}

#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include "ngx_media_test.h"
#include "ngx_media_http.h"
#include "ngx_media_hls_pull.h"
#include "ngx_media_hls_push.h"
#include "ngx_media_stream.h"
#include "ngx_media_destination.h"

#include <ares.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define S(s) (&(ngx_str_t) { sizeof(s) - 1, (u_char *) (s) })

/* The runtime sink is outside the unit link; no test asserts through it. */
void
ngx_media_runtime_iso_source(ngx_media_stream_t *stream,
    ngx_media_source_t *source, const ngx_media_frame_t *frame)
{
    (void) stream;
    (void) source;
    (void) frame;
}

/* Send actual c-ares queries to a local silent UDP listener, without changing
 * machine resolver configuration or mocking resolver completion. Set only
 * before starting clients and reset only after every client has joined. */
static unsigned short dns_port;
int __real_ares_init_options(ares_channel *channel,
    const struct ares_options *options, int mask);

int
__wrap_ares_init_options(ares_channel *channel,
    const struct ares_options *options, int mask)
{
    struct ares_options local = *options;
    struct in_addr server = { .s_addr = htonl(INADDR_LOOPBACK) };

    if (dns_port != 0) {
        local.servers = &server;
        local.nservers = 1;
        local.udp_port = dns_port;
        local.tcp_port = dns_port;
        local.lookups = (char *) "b";
        local.flags = ARES_FLAG_NOSEARCH | ARES_FLAG_NOALIASES;
        mask |= ARES_OPT_SERVERS | ARES_OPT_UDP_PORT | ARES_OPT_TCP_PORT
                | ARES_OPT_LOOKUPS | ARES_OPT_FLAGS;
    }
    return __real_ares_init_options(channel, &local, mask);
}

static uint64_t
now_ms(void)
{
    struct timespec ts;
    (void) clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct {
    int listen_fd;
    unsigned short port;
    pthread_t thread;
    SSL_CTX *tls;
    const char *responses[2];
    size_t response_sizes[2];
    unsigned nresponses;
    unsigned fragment;
    unsigned dribble;
    unsigned stall;
    unsigned response_delay_ms;
    atomic_uint stop;
    atomic_uint accepted;
    atomic_uint requests;
    char body[32];
    size_t body_len;
} origin_t;

static ssize_t
origin_io(int fd, SSL *ssl, void *buf, size_t len, int writing)
{
    if (ssl != NULL) {
        return writing ? SSL_write(ssl, buf, (int) len)
                       : SSL_read(ssl, buf, (int) len);
    }
    return writing ? send(fd, buf, len, MSG_NOSIGNAL) : recv(fd, buf, len, 0);
}

static int
origin_request(origin_t *o, int fd, SSL *ssl)
{
    char header[4096], *end, *cl;
    size_t have = 0, total, nbody;
    ssize_t n;
    long length = 0;

    do {
        if (atomic_load(&o->stop) || have + 1 >= sizeof(header)) {
            return -1;
        }
        n = origin_io(fd, ssl, header + have, sizeof(header) - have - 1, 0);
        if (n <= 0) {
            return -1;
        }
        have += (size_t) n;
        header[have] = '\0';
        end = strstr(header, "\r\n\r\n");
    } while (end == NULL);
    total = (size_t) (end + 4 - header);
    cl = strstr(header, "\r\nContent-Length: ");
    if (cl != NULL) {
        length = strtol(cl + strlen("\r\nContent-Length: "), NULL, 10);
    }
    if (length < 0 || (size_t) length > sizeof(o->body)) {
        return -1;
    }
    nbody = have - total;
    if (nbody > (size_t) length) {
        return -1;
    }
    memcpy(o->body, header + total, nbody);
    while (nbody < (size_t) length) {
        n = origin_io(fd, ssl, o->body + nbody, (size_t) length - nbody, 0);
        if (n <= 0) {
            return -1;
        }
        nbody += (size_t) n;
    }
    o->body_len = nbody;
    atomic_fetch_add(&o->requests, 1);
    return 0;
}

static void *
origin_thread(void *data)
{
    origin_t *o = data;
    struct pollfd pollfd = { .fd = o->listen_fd, .events = POLLIN };
    struct timeval timeout = { .tv_sec = 1 };
    SSL *ssl;
    int fd;
    unsigned response = 0;
    size_t sent, want;
    ssize_t n;

    while (!atomic_load(&o->stop) && response < o->nresponses) {
        if (poll(&pollfd, 1, 50) <= 0) {
            continue;
        }
        fd = accept(o->listen_fd, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        atomic_fetch_add(&o->accepted, 1);
        (void) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void) setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (o->stall) {
            while (!atomic_load(&o->stop)) {
                usleep(1000);
            }
            (void) close(fd);
            break;
        }
        ssl = o->tls != NULL ? SSL_new(o->tls) : NULL;
        if (ssl != NULL && (SSL_set_fd(ssl, fd) != 1 || SSL_accept(ssl) != 1)) {
            SSL_free(ssl);
            (void) close(fd);
            continue;
        }
        do {
            if (origin_request(o, fd, ssl) != 0) {
                break;
            }
            uint64_t response_at = now_ms() + o->response_delay_ms;
            while (!atomic_load(&o->stop) && now_ms() < response_at) {
                usleep(1000);
            }
            sent = 0;
            while (sent < o->response_sizes[response] && !atomic_load(&o->stop)) {
                want = o->response_sizes[response] - sent;
                if (o->fragment && want > o->fragment) {
                    want = o->fragment;
                }
                n = origin_io(fd, ssl, (void *) (o->responses[response] + sent),
                              want, 1);
                if (n <= 0) {
                    break;
                }
                sent += (size_t) n;
            }
            if (o->dribble) {
                while (!atomic_load(&o->stop)) {
                    n = origin_io(fd, ssl, (void *) "x", 1, 1);
                    if (n <= 0) {
                        break;
                    }
                    usleep(10000);
                }
            }
            response++;
        } while (response < o->nresponses && !atomic_load(&o->stop));
        if (ssl != NULL) {
            (void) SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        (void) close(fd);
    }
    return NULL;
}

static void
origin_start(origin_t *o, SSL_CTX *tls, const char *response, size_t size)
{
    struct sockaddr_in addr = { .sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(addr);

    o->tls = tls;
    o->responses[0] = response;
    o->response_sizes[0] = size;
    if (o->nresponses == 0) {
        o->nresponses = 1;
    }
    o->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(o->listen_fd >= 0);
    TEST_ASSERT_EQ_INT(bind(o->listen_fd, (struct sockaddr *) &addr, sizeof(addr)), 0);
    TEST_ASSERT_EQ_INT(getsockname(o->listen_fd, (struct sockaddr *) &addr, &len), 0);
    o->port = ntohs(addr.sin_port);
    TEST_ASSERT_EQ_INT(listen(o->listen_fd, 8), 0);
    TEST_ASSERT_EQ_INT(pthread_create(&o->thread, NULL, origin_thread, o), 0);
}

static void
origin_stop(origin_t *o)
{
    atomic_store(&o->stop, 1);
    (void) pthread_join(o->thread, NULL);
    (void) close(o->listen_fd);
}

static ngx_str_t
origin_url(origin_t *o, char *url, size_t cap, int tls, const char *host)
{
    int n = snprintf(url, cap, "%s://%s:%u/live/", tls ? "https" : "http",
                     host, o->port);
    return (ngx_str_t) { .len = (size_t) n, .data = (u_char *) url };
}

static int
wait_count(atomic_uint *count)
{
    uint64_t end = now_ms() + 2000;
    while (atomic_load(count) == 0 && now_ms() < end) {
        usleep(1000);
    }
    return atomic_load(count) != 0;
}

static void
framing_cases(SSL_CTX *tls, const ngx_str_t *ca)
{
    static const struct {
        const char *response;
        const char *body;
        size_t cap;
    } cases[] = {
        { "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", "hello", 6 },
        { "HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\ncontent-length: 5\r\n\r\nhello", "hello", 6 },
        { "HTTP/1.0 200 OK\r\n\r\nhello", "hello", 6 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
          "2;tag=\"a\\\"b\"\r\nhe\r\n3\r\nllo\r\n0\r\nX-End: yes\r\n\r\n", "hello", 6 },
        { "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", "", 1 },
        { "HTTP/1.1 204 No Content\r\n\r\n", "", 1 },
        { "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", "", 1 },
        { "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 4\r\n\r\nhello", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nContent-Length: 5, 4\r\n\r\nhello", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\nhello", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nhello", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", NULL, 5 },
        { "HTTP/1.0 200 OK\r\n\r\nhello", NULL, 5 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\nhello", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nx\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;tag=\"unfinished\r\nx\r\n0\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", NULL, 5 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nContent-Length: 0\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 2xx Bad\r\nContent-Length: 0\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 2000 Bad\r\nContent-Length: 0\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nBroken Header\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\n Content-Length: 0\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nContent-Length : 0\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nX-Value: bad\001value\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 200 OK\nContent-Length: 0\n\n", NULL, 32 },
        { "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n", NULL, 32 },
        { "HTTP/1.1 101 Switching Protocols\r\n\r\n", NULL, 32 },
        { "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\n\r\n", NULL, 32 },
    };
    size_t i, len;
    char url[128];
    u_char buf[64];
    ngx_media_http_conn_t c;
    ngx_str_t endpoint;
    ngx_int_t rc;

    TEST_CASE(tls != NULL ? "TLS response framing and trust" : "HTTP response framing");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        origin_t o = { .fragment = 3 };
        origin_start(&o, tls, cases[i].response, strlen(cases[i].response));
        endpoint = origin_url(&o, url, sizeof(url), tls != NULL, "localhost");
        ngx_media_http_conn_init(&c);
        c.timeout_ms = 2000;
        len = 99;
        memset(buf, 0xa5, sizeof(buf));
        rc = ngx_media_http_get(&c, &endpoint, ca, buf, cases[i].cap, &len, NULL);
        if (cases[i].body != NULL) {
            TEST_ASSERT_EQ_INT(rc, NGX_OK);
            TEST_ASSERT_EQ_U64(len, strlen(cases[i].body));
            TEST_ASSERT(strcmp((char *) buf, cases[i].body) == 0);
        } else {
            TEST_ASSERT_EQ_INT(rc, NGX_ERROR);
            TEST_ASSERT_EQ_U64(len, 0);
        }
        TEST_ASSERT_EQ_INT(buf[cases[i].cap], 0xa5);
        TEST_ASSERT_EQ_INT(c.fd, -1);
        origin_stop(&o);
    }
}

static SSL_CTX *
certificate(char *path)
{
    EVP_PKEY_CTX *keyctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    EVP_PKEY *key = NULL;
    X509 *cert = X509_new();
    X509_EXTENSION *san;
    X509_NAME *name;
    SSL_CTX *ctx;
    FILE *file;
    int fd;

    TEST_ASSERT_NOT_NULL(keyctx);
    TEST_ASSERT_EQ_INT(EVP_PKEY_keygen_init(keyctx), 1);
    TEST_ASSERT_EQ_INT(EVP_PKEY_CTX_set_rsa_keygen_bits(keyctx, 2048), 1);
    TEST_ASSERT_EQ_INT(EVP_PKEY_keygen(keyctx, &key), 1);
    EVP_PKEY_CTX_free(keyctx);
    TEST_ASSERT_NOT_NULL(cert);
    TEST_ASSERT_EQ_INT(X509_set_version(cert, 2), 1);
    TEST_ASSERT_EQ_INT(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1), 1);
    TEST_ASSERT_NOT_NULL(X509_gmtime_adj(X509_getm_notBefore(cert), -60));
    TEST_ASSERT_NOT_NULL(X509_gmtime_adj(X509_getm_notAfter(cert), 3600));
    TEST_ASSERT_EQ_INT(X509_set_pubkey(cert, key), 1);
    name = X509_get_subject_name(cert);
    TEST_ASSERT_EQ_INT(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                      (unsigned char *) "localhost", -1, -1, 0), 1);
    TEST_ASSERT_EQ_INT(X509_set_issuer_name(cert, name), 1);
    san = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, (char *) "DNS:localhost");
    TEST_ASSERT_NOT_NULL(san);
    TEST_ASSERT_EQ_INT(X509_add_ext(cert, san, -1), 1);
    X509_EXTENSION_free(san);
    TEST_ASSERT(X509_sign(cert, key, EVP_sha256()) > 0);
    fd = mkstemp(path);
    TEST_ASSERT(fd >= 0);
    file = fdopen(fd, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQ_INT(PEM_write_X509(file, cert), 1);
    (void) fclose(file);
    ctx = SSL_CTX_new(TLS_server_method());
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_EQ_INT(SSL_CTX_use_certificate(ctx, cert), 1);
    TEST_ASSERT_EQ_INT(SSL_CTX_use_PrivateKey(ctx, key), 1);
    X509_free(cert);
    EVP_PKEY_free(key);
    return ctx;
}

static void
upload_cases(SSL_CTX *tls, ngx_str_t *ca)
{
    static const char *responses[] = {
        "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",
        "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n",
        "HTTP/1.0 201 Created\r\n\r\nok",
        "HTTP/1.1 201 Created\r\nContent-Length: 3\r\n\r\nok",
        "HTTP/1.1 201 Created\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\nok",
        "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n",
        "HTTP/1.1 201 Created\r\nContent-Length: 1048577\r\n\r\n",
    };
    size_t i;
    char url[128], file[] = "/tmp/nginx-media-http-upload-XXXXXX";
    int fd = mkstemp(file);
    ngx_str_t endpoint;
    ngx_media_http_conn_t c;

    TEST_ASSERT(fd >= 0);
    TEST_ASSERT_EQ_INT(write(fd, "data", 4), 4);

    TEST_CASE(tls != NULL ? "TLS upload drains framing before success" : "HTTP upload drains framing before success");
    for (i = 0; i < sizeof(responses) / sizeof(responses[0]); i++) {
        origin_t o = { .fragment = 2 };
        origin_start(&o, tls, responses[i], strlen(responses[i]));
        endpoint = origin_url(&o, url, sizeof(url), tls != NULL, "localhost");
        ngx_media_http_conn_init(&c);
        c.timeout_ms = 2000;
        TEST_ASSERT_EQ_INT(ngx_media_http_send(&c, &endpoint, ca, "PUT",
            (u_char *) "segment.ts", (i % 2) ? fd : -1,
            (i % 2) ? NULL : (u_char *) "data", 4, "video/mp2t", NULL),
            i < 3 ? 201 : NGX_ERROR);
        ngx_media_http_conn_close(&c);
        origin_stop(&o);
        TEST_ASSERT_EQ_U64(o.body_len, 4);
        TEST_ASSERT(memcmp(o.body, "data", 4) == 0);
    }
    {
        const char *first = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\n"
                            "Transfer-Encoding: chunked\r\nConnection: upgrade, keep-alive\r\n"
                            "\r\n2\r\nok\r\n0\r\nX-End: yes\r\n\r\n";
        const char *second = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: keep-alive, close\r\n\r\n";
        origin_t o = { .nresponses = 2, .responses = { NULL, second },
                       .response_sizes = { 0, strlen(second) }, .fragment = 3 };
        origin_start(&o, tls, first, strlen(first));
        endpoint = origin_url(&o, url, sizeof(url), tls != NULL, "localhost");
        ngx_media_http_conn_init(&c);
        c.timeout_ms = 2000;
        TEST_ASSERT_EQ_INT(ngx_media_http_send(&c, &endpoint, ca, "PUT", (u_char *) "a.ts",
            -1, (u_char *) "data", 4, NULL, NULL), 201);
        TEST_ASSERT(c.fd >= 0);
        TEST_ASSERT_EQ_INT(ngx_media_http_send(&c, &endpoint, ca, "DELETE", (u_char *) "a.ts",
            -1, NULL, 0, NULL, NULL), 204);
        TEST_ASSERT_EQ_INT(c.fd, -1);
        TEST_ASSERT_EQ_U64(c.connects, 1);
        TEST_ASSERT_EQ_U64(c.requests, 2);
        origin_stop(&o);
        TEST_ASSERT_EQ_U64(atomic_load(&o.requests), 2);
        TEST_ASSERT_EQ_U64(atomic_load(&o.accepted), 1);
    }
    (void) close(fd);
    (void) unlink(file);
}

typedef struct {
    ngx_media_http_conn_t conn;
    ngx_str_t url;
    ngx_str_t *ca;
    int upload;
    ngx_int_t result;
    size_t len;
} client_t;

static void *
client_thread(void *data)
{
    client_t *client = data;
    u_char buf[1024];
    if (client->upload) {
        client->result = ngx_media_http_send(&client->conn, &client->url,
            client->ca, "PUT", (u_char *) "a.ts", -1, (u_char *) "data", 4,
            NULL, NULL);
    } else {
        client->result = ngx_media_http_get(&client->conn, &client->url,
            client->ca, buf, sizeof(buf), &client->len, NULL);
    }
    return NULL;
}

static void
deadline_and_cancel(SSL_CTX *tls, ngx_str_t *ca)
{
    const char *header = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n";
    unsigned upload, cancel;
    uint64_t start;
    pthread_t thread;
    char url[128];

    TEST_CASE(tls != NULL ? "TLS dribble: total deadline and cancellation" : "HTTP dribble: total deadline and cancellation");
    for (upload = 0; upload < 2; upload++) {
        for (cancel = 0; cancel < 2; cancel++) {
            origin_t o = { .dribble = 1 };
            client_t client = { .ca = ca, .upload = (int) upload };
            origin_start(&o, tls, header, strlen(header));
            client.url = origin_url(&o, url, sizeof(url), tls != NULL, "localhost");
            ngx_media_http_conn_init(&client.conn);
            client.conn.timeout_ms = cancel ? 5000 : 200;
            start = now_ms();
            TEST_ASSERT_EQ_INT(pthread_create(&thread, NULL, client_thread, &client), 0);
            if (cancel) {
                TEST_ASSERT(wait_count(&o.requests));
                start = now_ms();
                ngx_media_http_conn_cancel(&client.conn);
            }
            (void) pthread_join(thread, NULL);
            TEST_ASSERT_EQ_INT(client.result, NGX_ERROR);
            TEST_ASSERT(now_ms() - start < 1000);
            if (!cancel) {
                TEST_ASSERT(now_ms() - start >= 150);
            }
            TEST_ASSERT_EQ_U64(client.len, 0);
            ngx_media_http_conn_close(&client.conn);
            origin_stop(&o);
        }
    }
    {
        origin_t o = { .stall = 1 };
        client_t client = { .ca = ca };
        origin_start(&o, NULL, "", 0);
        client.url = origin_url(&o, url, sizeof(url), 1, "localhost");
        ngx_media_http_conn_init(&client.conn);
        TEST_ASSERT_EQ_INT(pthread_create(&thread, NULL, client_thread, &client), 0);
        TEST_ASSERT(wait_count(&o.accepted));
        start = now_ms();
        ngx_media_http_conn_cancel(&client.conn);
        (void) pthread_join(thread, NULL);
        TEST_ASSERT_EQ_INT(client.result, NGX_ERROR);
        TEST_ASSERT(now_ms() - start < 500);
        origin_stop(&o);
    }
}

static int
dns_blackhole(void)
{
    struct sockaddr_in addr = { .sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT_EQ_INT(bind(fd, (struct sockaddr *) &addr, sizeof(addr)), 0);
    TEST_ASSERT_EQ_INT(getsockname(fd, (struct sockaddr *) &addr, &len), 0);
    dns_port = ntohs(addr.sin_port);
    return fd;
}

static void
wait_dns(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    char packet[1024];
    TEST_ASSERT(poll(&pfd, 1, 2000) > 0);
    TEST_ASSERT(recv(fd, packet, sizeof(packet), MSG_DONTWAIT) > 0);
}

static void
dns_operations(void)
{
    int fd = dns_blackhole();
    unsigned i;
    pthread_t thread;
    uint64_t start;
    char packet[1024];

    TEST_CASE("DNS: actual unanswered queries expire and repeatedly cancel safely");
    for (i = 0; i < 12; i++) {
        client_t client = { .url = *S("http://bounded-http-test.invalid/live"),
                            .upload = (int) (i % 2) };
        ngx_media_http_conn_init(&client.conn);
        client.conn.timeout_ms = i == 0 ? 150 : 5000;
        while (recv(fd, packet, sizeof(packet), MSG_DONTWAIT) > 0) { }
        start = now_ms();
        TEST_ASSERT_EQ_INT(pthread_create(&thread, NULL, client_thread, &client), 0);
        wait_dns(fd);
        if (i != 0) {
            start = now_ms();
            ngx_media_http_conn_cancel(&client.conn);
        }
        (void) pthread_join(thread, NULL);
        TEST_ASSERT_EQ_INT(client.result, NGX_ERROR);
        TEST_ASSERT(now_ms() - start < 500);
        TEST_ASSERT_EQ_INT(client.conn.fd, -1);
        /* A cancelled owner cannot quietly start a fresh operation. */
        if (i != 0) {
            while (recv(fd, packet, sizeof(packet), MSG_DONTWAIT) > 0) { }
            TEST_ASSERT_EQ_INT(ngx_media_http_send(&client.conn, &client.url,
                NULL, "DELETE", (u_char *) "a.ts", -1, NULL, 0, NULL, NULL), NGX_ERROR);
            TEST_ASSERT_EQ_INT(poll(&(struct pollfd) { .fd = fd, .events = POLLIN },
                                    1, 10), 0);
        }
    }
    dns_port = 0;
    (void) close(fd);
}

typedef struct {
    int fd;
    atomic_int stop;
    pthread_t thread;
} dns_answers_t;

static void *
dns_answers(void *data)
{
    dns_answers_t *server = data;
    unsigned char packet[512], reply[512];
    struct sockaddr_storage peer;
    struct pollfd pfd = { .fd = server->fd, .events = POLLIN };
    socklen_t peer_len;
    ssize_t received;
    size_t end, size, count;

    while (!atomic_load(&server->stop)) {
        if (poll(&pfd, 1, 20) <= 0) {
            continue;
        }
        peer_len = sizeof(peer);
        received = recvfrom(server->fd, packet, sizeof(packet), 0,
                            (struct sockaddr *) &peer, &peer_len);
        if (received < 17 || packet[4] != 0 || packet[5] != 1) {
            continue;
        }
        end = 12;
        while (end < (size_t) received && packet[end] != 0) {
            if (packet[end] > 63 || packet[end] > (size_t) received - end - 1) {
                break;
            }
            end += (size_t) packet[end] + 1;
        }
        if (end + 5 > (size_t) received || packet[end] != 0) {
            continue;
        }
        count = packet[end + 1] == 0 && packet[end + 2] == 1 ? 2 : 0;
        size = end + 5;
        memcpy(reply, packet, size);
        reply[2] = 0x81;
        reply[3] = 0x80;
        reply[6] = 0;
        reply[7] = (unsigned char) count;
        memset(reply + 8, 0, 4);
        for (size_t i = 0; i < count; i++) {
            static const unsigned char answer[] = {
                0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 1, 0, 4, 127, 0, 0, 2
            };
            memcpy(reply + size, answer, sizeof(answer));
            reply[size + sizeof(answer) - 1] = (unsigned char) (i == 0 ? 2 : 1);
            size += sizeof(answer);
        }
        (void) sendto(server->fd, reply, size, 0, (struct sockaddr *) &peer, peer_len);
    }
    return NULL;
}

static void
connect_address_fallback(void)
{
    const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    origin_t origin = { 0 };
    dns_answers_t dns = { .fd = dns_blackhole() };
    struct sockaddr_in blocked = { .sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(0x7f000002) };
    ngx_media_http_conn_t conn;
    ngx_str_t endpoint;
    char url[256];
    u_char body[16];
    size_t length;
    int listener, filler, probe;
    struct pollfd stalled;

    TEST_CASE("connect: a full first-address backlog cannot consume the next address's deadline");
    origin_start(&origin, NULL, response, strlen(response));
    blocked.sin_port = htons(origin.port);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(listener >= 0);
    TEST_ASSERT_EQ_INT(bind(listener, (struct sockaddr *) &blocked, sizeof(blocked)), 0);
    TEST_ASSERT_EQ_INT(listen(listener, 0), 0);
    filler = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(filler >= 0);
    TEST_ASSERT_EQ_INT(connect(filler, (struct sockaddr *) &blocked, sizeof(blocked)), 0);
    probe = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    TEST_ASSERT(probe >= 0);
    TEST_ASSERT_EQ_INT(connect(probe, (struct sockaddr *) &blocked, sizeof(blocked)), -1);
    TEST_ASSERT_EQ_INT(errno, EINPROGRESS);
    stalled = (struct pollfd) { .fd = probe, .events = POLLOUT };
    TEST_ASSERT_EQ_INT(poll(&stalled, 1, 100), 0);
    (void) close(probe);
    atomic_init(&dns.stop, 0);
    TEST_ASSERT_EQ_INT(pthread_create(&dns.thread, NULL, dns_answers, &dns), 0);
    endpoint = origin_url(&origin, url, sizeof(url), 0, "fallback-http-test.invalid");
    ngx_media_http_conn_init(&conn);
    conn.timeout_ms = 1000;
    TEST_ASSERT_EQ_INT(ngx_media_http_get(&conn, &endpoint, NULL, body, sizeof(body),
                                         &length, NULL), NGX_OK);
    TEST_ASSERT_EQ_U64(length, 2);
    TEST_ASSERT(memcmp(body, "ok", 2) == 0);
    atomic_store(&dns.stop, 1);
    (void) pthread_join(dns.thread, NULL);
    dns_port = 0;
    (void) close(dns.fd);
    (void) close(filler);
    (void) close(listener);
    origin_stop(&origin);
}

static ngx_pool_t *
stream_start(ngx_media_stream_t *stream)
{
    ngx_media_feed_conf_t conf = { .max_units = 16 };
    ngx_pool_t *pool = ngx_create_pool(4096, NULL);
    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_EQ_INT(ngx_media_stream_init(stream, pool, NULL,
                                           S("live"), S("http"), &conf), NGX_OK);
    return pool;
}

static void
hls_lifecycle(void)
{
    ngx_media_stream_t stream;
    ngx_pool_t *pool;
    ngx_media_hls_pull_t *pull;
    ngx_media_destination_t *dest;
    ngx_str_t endpoint, directory = *S("/tmp"), path;
    char url[128], file[] = "/tmp/nginx-media-http-segment-XXXXXX", packet[1024];
    int fd, resolver;
    unsigned dns, remove;
    uint64_t start;

    TEST_CASE("HLS pull: destruction cancels response and DNS before pool release");
    for (dns = 0; dns < 3; dns++) {
        origin_t o = { .dribble = 1 };
        resolver = -1;
        if (dns) {
            resolver = dns_blackhole();
            endpoint = *S("http://bounded-http-test.invalid/live/index.m3u8");
        } else {
            origin_start(&o, NULL, "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n", 41);
            endpoint = origin_url(&o, url, sizeof(url), 0, "localhost");
        }
        pool = stream_start(&stream);
        pull = ngx_media_hls_pull_open(&stream, S("pull"), &endpoint, NULL, NULL);
        TEST_ASSERT_NOT_NULL(pull);
        if (dns) {
            wait_dns(resolver);
        } else {
            TEST_ASSERT(wait_count(&o.requests));
        }
        start = now_ms();
        if (dns == 2) {
            ngx_media_stream_source_remove(&stream,
                ngx_media_stream_source_find(&stream, S("pull")));
            do {
                ngx_media_hls_pull_reap(NULL);
                usleep(1000);
            } while (ngx_media_hls_pull_stream_readers(&stream) != 0
                     && now_ms() - start < 500);
        } else {
            ngx_media_hls_pull_close(pull);
        }
        TEST_ASSERT(now_ms() - start < 500);
        TEST_ASSERT_EQ_U64(ngx_media_hls_pull_stream_readers(&stream), 0);
        ngx_media_stream_destroy(&stream);
        ngx_destroy_pool(pool);
        if (dns) {
            dns_port = 0;
            (void) close(resolver);
        } else {
            origin_stop(&o);
        }
    }
    TEST_CASE("HLS push: removal/stop cancels response and DNS with in-flight refs");
    fd = mkstemp(file);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT_EQ_INT(write(fd, "data", 4), 4);
    (void) close(fd);
    path = (ngx_str_t) { .len = strlen(file), .data = (u_char *) file };
    for (dns = 0; dns < 2; dns++) {
        for (remove = 0; remove < 2; remove++) {
            origin_t o = { .dribble = 1 };
            resolver = -1;
            if (dns) {
                resolver = dns_blackhole();
                endpoint = *S("http://bounded-http-test.invalid/live/");
            } else {
                origin_start(&o, NULL, "HTTP/1.1 201 Created\r\nContent-Length: 1000\r\n\r\n", 46);
                endpoint = origin_url(&o, url, sizeof(url), 0, "localhost");
            }
            pool = stream_start(&stream);
            TEST_ASSERT_EQ_INT(ngx_media_hls_push_register(NULL), NGX_OK);
            dest = ngx_media_destination_add(&stream, S("push"), NGX_MEDIA_DEST_HLS_PUSH, NULL);
            TEST_ASSERT_NOT_NULL(dest);
            TEST_ASSERT_EQ_INT(ngx_media_string_set(&dest->host, &endpoint, NULL),
                               NGX_OK);
            TEST_ASSERT_EQ_INT(ngx_media_string_set(&dest->path, &directory, NULL),
                               NGX_OK);
            dest->segment_duration_ms = 10000;
            TEST_ASSERT_EQ_INT(ngx_media_destination_start(&stream, dest, NULL), NGX_OK);
            ngx_media_hls_push_sealed(&directory, &path, NULL);
            if (dns) {
                wait_dns(resolver);
            } else {
                TEST_ASSERT(wait_count(&o.requests));
            }
            start = now_ms();
            if (remove) {
                ngx_media_destination_remove(&stream, dest);
            }
            ngx_media_hls_push_stop();
            TEST_ASSERT(now_ms() - start < 500);
            if (!remove) {
                ngx_media_destination_remove(&stream, dest);
            }
            TEST_ASSERT_EQ_U64(ngx_media_hls_push_count(), 0);
            ngx_media_stream_destroy(&stream);
            ngx_destroy_pool(pool);
            if (dns) {
                while (recv(resolver, packet, sizeof(packet), MSG_DONTWAIT) > 0) { }
                dns_port = 0;
                (void) close(resolver);
            } else {
                origin_stop(&o);
            }
        }
    }
    (void) unlink(file);
}

static void
hls_push_request_budget(void)
{
    const char *response = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n";
    origin_t origin = { .response_delay_ms = 1200 };
    ngx_media_stream_t stream;
    ngx_media_destination_t *destination;
    ngx_pool_t *pool;
    ngx_str_t directory = *S("/tmp"), path, endpoint;
    char url[128], file[] = "/tmp/nginx-media-http-budget-XXXXXX";
    uint64_t started;
    int fd;

    TEST_CASE("HLS push: request completion is not capped by the segment retry budget");
    fd = mkstemp(file);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT_EQ_INT(write(fd, "data", 4), 4);
    TEST_ASSERT_EQ_INT(close(fd), 0);
    path = (ngx_str_t) { .len = strlen(file), .data = (u_char *) file };
    origin_start(&origin, NULL, response, strlen(response));
    endpoint = origin_url(&origin, url, sizeof(url), 0, "localhost");
    pool = stream_start(&stream);
    TEST_ASSERT_EQ_INT(ngx_media_hls_push_register(NULL), NGX_OK);
    destination = ngx_media_destination_add(&stream, S("push"),
                                             NGX_MEDIA_DEST_HLS_PUSH, NULL);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_EQ_INT(ngx_media_string_set(&destination->host, &endpoint, NULL),
                       NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_string_set(&destination->path, &directory, NULL),
                       NGX_OK);
    destination->segment_duration_ms = 1000;
    TEST_ASSERT_EQ_INT(ngx_media_destination_start(&stream, destination, NULL), NGX_OK);
    started = now_ms();
    ngx_media_hls_push_sealed(&directory, &path, NULL);
    while (ngx_media_hls_push_uploaded_total() == 0 && now_ms() - started < 3000) {
        usleep(1000);
    }
    TEST_ASSERT_EQ_U64(ngx_media_hls_push_uploaded_total(), 1);
    TEST_ASSERT_EQ_U64(ngx_media_hls_push_failed_total(), 0);
    TEST_ASSERT(now_ms() - started >= 1200);
    ngx_media_destination_remove(&stream, destination);
    ngx_media_hls_push_stop();
    ngx_media_stream_destroy(&stream);
    ngx_destroy_pool(pool);
    origin_stop(&origin);
    TEST_ASSERT_EQ_INT(unlink(file), 0);
}


static void
input_boundaries(void)
{
    ngx_media_http_conn_t c;
    ngx_str_t endpoint;
    char url[2400], response[20000];
    u_char buf[16];
    size_t len, have;
    unsigned i;
    const char *valid = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    const char binary[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na\0b";
    const char nul_header[] = "HTTP/1.1 200 OK\r\nX-Test: a\0b\r\nContent-Length: 0\r\n\r\n";

    TEST_CASE("HTTP: binary bodies, NUL headers and bounded header blocks");
    for (i = 0; i < 4; i++) {
        origin_t o = { 0 };
        if (i == 0) {
            memcpy(response, binary, sizeof(binary) - 1);
            have = sizeof(binary) - 1;
        } else if (i == 1) {
            memcpy(response, nul_header, sizeof(nul_header) - 1);
            have = sizeof(nul_header) - 1;
        } else if (i == 2) {
            have = (size_t) snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nX-Large: ");
            memset(response + have, 'a', 8192);
            have += 8192;
            memcpy(response + have, "\r\n\r\n", 4);
            have += 4;
        } else {
            have = (size_t) snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\n");
            for (unsigned j = 0; j < 70; j++) {
                memcpy(response + have, "X-Test: ", 8);
                have += 8;
                memset(response + have, 'a', 240);
                have += 240;
                memcpy(response + have, "\r\n", 2);
                have += 2;
            }
            memcpy(response + have, "\r\n", 2);
            have += 2;
        }
        origin_start(&o, NULL, response, have);
        endpoint = origin_url(&o, url, sizeof(url), 0, "localhost");
        ngx_media_http_conn_init(&c);
        TEST_ASSERT_EQ_INT(ngx_media_http_get(&c, &endpoint, NULL, buf, sizeof(buf),
                                             &len, NULL), i == 0 ? NGX_OK : NGX_ERROR);
        if (i == 0) {
            TEST_ASSERT_EQ_U64(len, 3);
            TEST_ASSERT(memcmp(buf, "a\0b", 3) == 0);
            TEST_ASSERT_EQ_INT(buf[3], 0);
        } else {
            TEST_ASSERT_EQ_U64(len, 0);
        }
        origin_stop(&o);
    }
    TEST_CASE("HTTP: invalid endpoint and request truncation never reach the origin");
    {
        origin_t o = { 0 };
        origin_start(&o, NULL, valid, strlen(valid));
        for (i = 0; i < 5; i++) {
            if (i == 0) {
                (void) snprintf(url, sizeof(url), "http://localhost:%ux/live", o.port);
            } else if (i == 1) {
                (void) snprintf(url, sizeof(url), "http://localhost:%u/a\r\nX-Injected: yes", o.port);
            } else if (i == 2) {
                (void) snprintf(url, sizeof(url), "http://localhost:%u/a b", o.port);
            } else if (i == 3) {
                (void) snprintf(url, sizeof(url), "http://localhost:%u/a#fragment", o.port);
            } else {
                have = (size_t) snprintf(url, sizeof(url), "http://localhost:%u/", o.port);
                memset(url + have, 'a', 2000);
                url[have + 2000] = '\0';
            }
            endpoint = (ngx_str_t) { .len = strlen(url), .data = (u_char *) url };
            ngx_media_http_conn_init(&c);
            c.timeout_ms = 500;
            TEST_ASSERT_EQ_INT(ngx_media_http_get(&c, &endpoint, NULL, buf, sizeof(buf),
                                                 &len, NULL), NGX_ERROR);
            TEST_ASSERT_EQ_U64(len, 0);
        }
        TEST_ASSERT_EQ_U64(atomic_load(&o.accepted), 0);
        origin_stop(&o);
    }
}

int
main(void)
{
    char certpath[] = "/tmp/nginx-media-http-cert-XXXXXX";
    SSL_CTX *tls;
    ngx_str_t ca;
    char url[128];
    size_t len;
    u_char buf[16];
    ngx_media_http_conn_t c;

    (void) signal(SIGPIPE, SIG_IGN);
    framing_cases(NULL, NULL);
    upload_cases(NULL, NULL);
    deadline_and_cancel(NULL, NULL);
    input_boundaries();
    tls = certificate(certpath);
    ca = (ngx_str_t) { .len = strlen(certpath), .data = (u_char *) certpath };
    framing_cases(tls, &ca);
    upload_cases(tls, &ca);
    deadline_and_cancel(tls, &ca);
    TEST_CASE("TLS: trusted certificate for wrong host is rejected");
    {
        const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        origin_t o = { 0 };
        ngx_str_t endpoint;
        origin_start(&o, tls, response, strlen(response));
        endpoint = origin_url(&o, url, sizeof(url), 1, "127.0.0.1");
        ngx_media_http_conn_init(&c);
        TEST_ASSERT_EQ_INT(ngx_media_http_get(&c, &endpoint, &ca, buf, sizeof(buf),
                                             &len, NULL), NGX_ERROR);
        TEST_ASSERT_EQ_U64(len, 0);
        origin_stop(&o);
    }
    {
        const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        origin_t o = { 0 };
        ngx_str_t endpoint;
        origin_start(&o, tls, response, strlen(response));
        endpoint = origin_url(&o, url, sizeof(url), 1, "localhost");
        ngx_media_http_conn_init(&c);
        TEST_ASSERT_EQ_INT(ngx_media_http_get(&c, &endpoint, &ca, buf, sizeof(buf),
                                             &len, NULL), NGX_OK);
        TEST_ASSERT_EQ_U64(len, 2);
        TEST_ASSERT(memcmp(buf, "ok", 2) == 0);
        origin_stop(&o);
    }
    SSL_CTX_free(tls);
    (void) unlink(certpath);
    dns_operations();
    connect_address_fallback();
    hls_lifecycle();
    hls_push_request_budget();
    TEST_LEAKS();
    TEST_MAIN_END();
}

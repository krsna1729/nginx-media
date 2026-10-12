#ifndef NGX_MEDIA_HTTP_H
#define NGX_MEDIA_HTTP_H

#include "ngx_media.h"

#include <sys/types.h>

/*
 * A bounded, synchronous HTTP/1.1 client.
 *
 * Callers run on their own threads, never the event loop. Nonblocking socket
 * and resolver I/O share one monotonic deadline per operation. Cancellation
 * is sticky until conn_init(), and can be requested by another thread; close
 * and all other connection access remain the operation owner's responsibility.
 *
 * Both http:// and https:// are supported.  A module that could only speak
 * plaintext would be a capability blocker rather than a media subsystem, so
 * TLS is a first-class path here, not a later phase.
 *
 * Where the kernel supports kTLS, it is enabled and the transfer takes the
 * fewest copies the platform allows:
 *
 *   plain    sendfile()                       page cache -> socket
 *   kTLS     sendfile()                       page cache -> socket, kernel
 *                                             encrypts, no user space
 *   OpenSSL  SSL_write()                      page cache -> user -> socket
 *
 * The middle row is the one worth having: OpenSSL cannot be handed a file
 * descriptor, but with kTLS the kernel does the encryption and sendfile works
 * again, which is exactly what TLS_TX_ZEROCOPY_RO is for.  Without kTLS the
 * copy through user space is unavoidable, and that is reported rather than
 * pretended otherwise.
 *
 * Verification is on by default against the system trust store.  ca_file
 * overrides it, which is what a private CA or a test needs.
 */

/*
 * A connection kept for one destination's requests (HTTP/1.1 keep-alive).
 * An ingest source SHOULD use persistent connections (DASH-IF Live Media
 * Ingest; Akamai asks for one per stream): a connection per object costs a
 * TCP handshake and, over HTTPS, a TLS handshake per segment and playlist.
 * Owned by one thread at a time; the caller serialises its use.
 */
typedef struct {
    int           fd;
    void         *ssl;
    u_char        key[640];       /* scheme, host, port and trust anchor */
    uint64_t      connects;
    uint64_t      requests;
    ngx_atomic_t  cancelled;      /* only field accessed by non-owner */
    ngx_msec_t    timeout_ms;     /* total operation budget, default 10000 */
} ngx_media_http_conn_t;

void ngx_media_http_conn_init(ngx_media_http_conn_t *c);
void ngx_media_http_conn_close(ngx_media_http_conn_t *c);
void ngx_media_http_conn_cancel(ngx_media_http_conn_t *c);

/*
 * GET a complete 2xx body into buf, including space for its NUL terminator.
 * Header/chunk framing is decoded separately and does not consume body space.
 * On malformed framing, premature EOF, cap exhaustion, deadline or cancellation,
 * returns NGX_ERROR with *len == 0; NGX_DECLINED means unsupported scheme.
 * GET closes the connection on every exit.
 */
ngx_int_t ngx_media_http_get(ngx_media_http_conn_t *c, const ngx_str_t *url,
    const ngx_str_t *ca_file, u_char *buf, size_t cap, size_t *len,
    ngx_log_t *log);

/*
 * One request for the object `name` below url, on c's connection when it is
 * still open to the same endpoint, else on a new one; a stale kept
 * connection is replaced once.  method is "PUT", "POST" or "DELETE".  The
 * body is size bytes of file_fd (caller-owned, read with an explicit offset)
 * when file_fd >= 0, else size bytes of buf, else there is none. Returns the
 * final HTTP status only after its complete response body has been drained
 * (at most 1 MiB), or NGX_ERROR on any operation/framing failure,
 * NGX_DECLINED when the endpoint is not supported.
 */
ngx_int_t ngx_media_http_send(ngx_media_http_conn_t *c, const ngx_str_t *url,
    const ngx_str_t *ca_file, const char *method, const u_char *name,
    int file_fd, const u_char *buf, off_t size, const char *content_type,
    ngx_log_t *log);


#endif /* NGX_MEDIA_HTTP_H */

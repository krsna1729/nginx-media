#ifndef NGX_MEDIA_HTTP_H
#define NGX_MEDIA_HTTP_H

#include "ngx_media.h"

#include <sys/types.h>

/*
 * A minimal blocking HTTP/1.1 client.
 *
 * A core module has no upstream machinery, and both users here - fetching HLS
 * segments and uploading them - need a fixed, known request shape rather than
 * a general client.  Both callers run on their own threads, so blocking is
 * deliberate: it costs a worker thread, never the event loop.
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
 * GET url into buf.  Returns NGX_OK and sets *len on a 2xx, NGX_DECLINED
 * when the endpoint is not supported, NGX_ERROR otherwise.  buf is
 * NUL-terminated so text bodies can be used directly.
 */
ngx_int_t ngx_media_http_get(const ngx_str_t *url, const ngx_str_t *ca_file,
    u_char *buf, size_t cap, size_t *len, ngx_log_t *log);

/*
 * PUT an already-open snapshot of a file to url.  path supplies the remote
 * basename; file_fd remains caller-owned so sealed files can be shared across
 * destination queues and remain readable after unlink or rename retention.
 */
ngx_int_t ngx_media_http_put_file(const ngx_str_t *url,
    const ngx_str_t *ca_file, const u_char *path, int file_fd, off_t size,
    ngx_log_t *log);

/* whether kTLS is engaged on this connection, for diagnostics */
ngx_uint_t ngx_media_http_ktls_supported(void);

#endif /* NGX_MEDIA_HTTP_H */

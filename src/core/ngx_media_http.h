#ifndef NGX_MEDIA_HTTP_H
#define NGX_MEDIA_HTTP_H

#include "ngx_media.h"

/*
 * A minimal blocking HTTP/1.1 client.
 *
 * A core module has no upstream machinery, and both users here - fetching HLS
 * segments and uploading them - need a fixed, known request shape rather than
 * a general client.  Both callers run on their own threads, so blocking is
 * deliberate: it costs a worker thread, never the event loop.
 *
 * Only plain http:// is handled.  TLS would need a client context, and the
 * callers report that rather than failing obscurely.
 */

/*
 * GET url into buf.  Returns NGX_OK and sets *len on a 2xx, NGX_DECLINED
 * when the endpoint is not supported, NGX_ERROR otherwise.  buf is
 * NUL-terminated so text bodies can be used directly.
 */
ngx_int_t ngx_media_http_get(const ngx_str_t *url, u_char *buf, size_t cap,
    size_t *len, ngx_log_t *log);

/*
 * PUT the contents of a file to url.  The body is sent with sendfile, so the
 * page cache goes straight to the socket with no user-space copy; a short
 * transfer is reported rather than published.
 */
ngx_int_t ngx_media_http_put_file(const ngx_str_t *url, const u_char *path,
    off_t size, ngx_log_t *log);

#endif /* NGX_MEDIA_HTTP_H */

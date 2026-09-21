#ifndef NGX_MEDIA_DESTINATION_H
#define NGX_MEDIA_DESTINATION_H

#include "ngx_media.h"

/*
 * Runtime destinations (goal doc normative revision).
 *
 * A destination is a runtime object with a stable logical id, exactly like a
 * stream and a source.  It is not a transport connection: the id survives
 * reconnects, and it is what a controller addresses.
 *
 * The core owns the model and the list.  Starting and stopping the actual
 * transport is a backend's job, reached through ngx_media_destination_ops_t,
 * which mirrors the way the SRT transport is a contract rather than a library
 * call.  A backend registers itself once and the core calls it for every
 * destination of the types it handles.
 */

#define NGX_MEDIA_DEST_SRT        1
#define NGX_MEDIA_DEST_RTMP       2
#define NGX_MEDIA_DEST_HLS_PUSH   3
#define NGX_MEDIA_DEST_RECORD     4

typedef struct ngx_media_destination_s {
    ngx_media_stream_t     *stream;

    ngx_str_t               id;         /* stable logical id */
    ngx_uint_t              type;       /* NGX_MEDIA_DEST_* */

    ngx_str_t               host;       /* network destinations */
    ngx_uint_t              port;
    ngx_str_t               streamid;   /* optional publish stream id */
    ngx_str_t               path;       /* filesystem destinations */
    ngx_str_t               ca_file;    /* TLS endpoints: trust anchor */

    /*
     * Platform profile (normative revision): a named set of rules layered on
     * the generic HLS publisher.  Empty means no profile, which is the plain
     * generic publisher.
     */
    ngx_str_t               profile;
    ngx_uint_t              segment_duration_ms;
    ngx_uint_t              playlist_window;
    ngx_uint_t              http_post;

    uint64_t                revision;   /* bumped by every mutation */
    unsigned                enabled:1;  /* desired state */

    void                   *impl;       /* backend handle, opaque here */
    ngx_queue_t             queue;
} ngx_media_destination_t;

/*
 * What a backend has to provide.  add() starts the transport for a
 * destination that is enabled; remove() stops it and releases impl.  Both are
 * called with the stream lock held by the caller's convention (the control
 * API runs on the owner worker).
 */
typedef struct {
    ngx_uint_t  type;

    ngx_int_t (*add)(ngx_media_stream_t *stream,
        ngx_media_destination_t *destination, ngx_log_t *log);
    void (*remove)(ngx_media_stream_t *stream,
        ngx_media_destination_t *destination);
} ngx_media_destination_ops_t;

/* one backend per type; registering twice replaces the previous entry */
ngx_int_t ngx_media_destination_register(const ngx_media_destination_ops_t *ops);

/* CRUD on a stream's destination list; all of these bump the revision */
ngx_media_destination_t *ngx_media_destination_add(ngx_media_stream_t *stream,
    const ngx_str_t *id, ngx_uint_t type, ngx_log_t *log);
ngx_media_destination_t *ngx_media_destination_find(ngx_media_stream_t *stream,
    const ngx_str_t *id);
ngx_int_t ngx_media_destination_start(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination, ngx_log_t *log);
void ngx_media_destination_remove(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination);
ngx_uint_t ngx_media_destination_count(const ngx_media_stream_t *stream);

/* pool copy of a string field; NULL when src is empty or allocation fails */
ngx_str_t *ngx_media_destination_strdup(ngx_pool_t *pool, const ngx_str_t *src);

/* bumps the object revision: call it from every desired-state mutation */
void ngx_media_destination_touch(ngx_media_destination_t *destination);

#endif /* NGX_MEDIA_DESTINATION_H */

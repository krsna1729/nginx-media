#ifndef NGX_MEDIA_RTMP_DESTINATION_H
#define NGX_MEDIA_RTMP_DESTINATION_H

#include "ngx_media.h"
#include "ngx_media_destination.h"

/*
 * RTMP destination backend (goal doc 12, 16).
 *
 * The core owns the destination model; this is the transport that carries a
 * destination of type NGX_MEDIA_DEST_RTMP.  The module registers it once in
 * init_process, so the control API can add an RTMP destination at any time --
 * including on an instance that has no media_rtmp_listen at all, which is the
 * shape a publishing-only deployment has.
 */
ngx_int_t ngx_media_rtmp_destination_add(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination, ngx_log_t *log);
void ngx_media_rtmp_destination_remove(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination);

/* Wake publishing destinations after the shared FLV feed gains media. */
void ngx_media_rtmp_destination_media_ready(ngx_media_stream_t *stream);

/*
 * Stops every live destination.  Called from exit_process before the program
 * runtime shuts the shared FLV preparation down, so the scheduler queue and
 * maintenance heap cannot revisit a destination whose prepare slot is freed.
 */
void ngx_media_rtmp_destination_stop_all(void);

#endif /* NGX_MEDIA_RTMP_DESTINATION_H */

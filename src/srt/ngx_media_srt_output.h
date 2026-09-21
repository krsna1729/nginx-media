#ifndef NGX_MEDIA_SRT_OUTPUT_H
#define NGX_MEDIA_SRT_OUTPUT_H

#ifdef NGX_MEDIA_UNIT_TEST
#define _POSIX_C_SOURCE 200809L
#endif

#include "ngx_media_platform.h"
#include "ngx_media_buffer.h"
#include "ngx_media_srt_output_queue.h"
#include "ngx_media_srt_transport.h"

#include <pthread.h>

/*
 * SRT output runtime (goal doc 11, 16, phase 7).
 *
 * One destination is one SRT caller session plus a bounded subscriber queue.
 * The worker never sends: it pushes references to the transport bursts the
 * program runtime already prepared for HLS and recording, so preparation is
 * shared and a destination costs one reference per burst, not a copy.  A
 * sender thread per destination performs transport progress, which keeps a
 * slow or stalled receiver from delaying any other output (goal doc 34 items
 * 11 and 14).
 */

#define NGX_MEDIA_SRT_MAX_OUTPUTS 8
#define NGX_MEDIA_SRT_OUT_MAX_EVENTS 64

#define NGX_MEDIA_SRT_OUT_EVENT_CONNECTED 1
#define NGX_MEDIA_SRT_OUT_EVENT_FAILED    2

typedef struct {
    ngx_str_t   application;
    ngx_str_t   stream;
    ngx_str_t   host;
    ngx_uint_t  port;
    ngx_str_t   streamid;          /* optional publish stream id */
    ngx_uint_t  max_units;
    size_t      max_bytes;
    ngx_msec_t  connect_timeout;
    ngx_msec_t  send_timeout;
} ngx_media_srt_output_conf_t;

typedef struct {
    ngx_uint_t  type;              /* NGX_MEDIA_SRT_OUT_EVENT_* */
    ngx_uint_t  index;
    uint64_t    sent_bytes;
    uint64_t    sent_bursts;
    uint64_t    dropped;
    uint64_t    reconnects;
} ngx_media_srt_out_event_t;

typedef struct ngx_media_srt_outputs_s ngx_media_srt_outputs_t;

ngx_int_t ngx_media_srt_outputs_start(ngx_media_srt_outputs_t **out,
    const ngx_media_srt_output_conf_t *confs, ngx_uint_t count,
    ngx_uint_t max_events, ngx_log_t *log);
void ngx_media_srt_outputs_stop(ngx_media_srt_outputs_t *outs);

/*
 * Offers one prepared burst to every destination bound to application/stream.
 * The burst is referenced, never copied and never owned by the destination.
 */
ngx_int_t ngx_media_srt_outputs_push(ngx_media_srt_outputs_t *outs,
    const ngx_str_t *application, const ngx_str_t *stream,
    ngx_media_buf_t *burst, size_t len, ngx_uint_t keyframe);

/* the eventfd the worker registers in its own event loop */
int ngx_media_srt_outputs_notify_fd(ngx_media_srt_outputs_t *outs);

ngx_uint_t ngx_media_srt_outputs_event_read(ngx_media_srt_outputs_t *outs,
    ngx_media_srt_out_event_t *out, ngx_uint_t max);

#endif /* NGX_MEDIA_SRT_OUTPUT_H */

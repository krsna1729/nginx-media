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
 * SRT output runtime.  The program owner publishes immutable prepared bursts
 * to the bounded feeds of matching egress shards; each shard fans the shared
 * reference into its assigned destination queues and services them with
 * nonblocking transport sends.
 */

#define NGX_MEDIA_SRT_MAX_OUTPUTS 1000
#define NGX_MEDIA_SRT_EGRESS_SHARDS 16
#define NGX_MEDIA_SRT_OUT_MAX_EVENTS 2048

#define NGX_MEDIA_SRT_OUT_EVENT_CONNECTED 1
#define NGX_MEDIA_SRT_OUT_EVENT_FAILED    2

typedef struct {
    ngx_str_t   application;
    ngx_str_t   stream;
    uintptr_t   program_identity;  /* zero for a statically configured route */
    uint64_t    incarnation;       /* stream object lifetime */
    ngx_str_t   host;
    ngx_uint_t  port;
    ngx_str_t   streamid;          /* optional publish stream id */
    ngx_uint_t  max_units;
    size_t      max_bytes;
    ngx_msec_t  connect_timeout;
    ngx_msec_t  send_timeout;

    /*
     * Encryption for this destination, or NULL for none.  Resolved when the
     * outputs start, so the per-stream overrides apply regardless of the
     * order the directives appear in.
     */
    const ngx_media_srt_params_t  *params;
} ngx_media_srt_output_conf_t;

typedef struct {
    ngx_uint_t  type;              /* NGX_MEDIA_SRT_OUT_EVENT_* */
    ngx_uint_t  index;
    uint64_t    sent_bytes;
    uint64_t    sent_bursts;
    uint64_t    dropped;
    uint64_t    reconnects;
} ngx_media_srt_out_event_t;

typedef struct {
    ngx_uint_t  shard;
    ngx_uint_t  destinations;
    ngx_uint_t  feed_queue_units;
    size_t      feed_queue_bytes;
    uint64_t    feed_queue_dropped;
    ngx_uint_t  output_queue_units;
    size_t      output_queue_bytes;
    uint64_t    output_queue_dropped;
    uint64_t    sent_bytes;
    uint64_t    sent_bursts;
    uint64_t    blocked_sends;
    uint64_t    retransmitted_packets;
} ngx_media_srt_egress_stats_t;

typedef struct ngx_media_srt_outputs_s ngx_media_srt_outputs_t;

ngx_int_t ngx_media_srt_outputs_start(ngx_media_srt_outputs_t **out,
    const ngx_media_srt_output_conf_t *confs, ngx_uint_t count,
    ngx_uint_t max_events, ngx_log_t *log);
void ngx_media_srt_outputs_stop(ngx_media_srt_outputs_t *outs);

/*
 * Runtime destinations share the fixed egress shards.  add() takes a free
 * slot; remove() stops it and releases it after any in-flight send completes.
 */
ngx_int_t ngx_media_srt_outputs_add(ngx_media_srt_outputs_t *outs,
    const ngx_media_srt_output_conf_t *conf, ngx_uint_t *index,
    ngx_log_t *log);
void ngx_media_srt_outputs_remove(ngx_media_srt_outputs_t *outs,
    ngx_uint_t index);

/*
 * Publishes one immutable prepared burst to the shards owning matching
 * destinations.  The shard queues and destination queues take references;
 * neither copies nor owns the caller's buffer.
 */
ngx_int_t ngx_media_srt_outputs_push(ngx_media_srt_outputs_t *outs,
    const ngx_str_t *application, const ngx_str_t *stream,
    uintptr_t program_identity, uint64_t incarnation, ngx_media_buf_t *burst,
    size_t len, ngx_uint_t keyframe);

ngx_uint_t ngx_media_srt_outputs_stats_get(ngx_media_srt_outputs_t *outs,
    ngx_media_srt_egress_stats_t *stats, ngx_uint_t max);
ngx_uint_t ngx_media_srt_module_stats_get(ngx_media_srt_egress_stats_t *stats,
    ngx_uint_t max);

/* the eventfd the worker registers in its own event loop */
int ngx_media_srt_outputs_notify_fd(ngx_media_srt_outputs_t *outs);

ngx_uint_t ngx_media_srt_outputs_event_read(ngx_media_srt_outputs_t *outs,
    ngx_media_srt_out_event_t *out, ngx_uint_t max);

#endif /* NGX_MEDIA_SRT_OUTPUT_H */

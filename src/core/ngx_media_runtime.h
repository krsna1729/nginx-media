#ifndef NGX_MEDIA_RUNTIME_H
#define NGX_MEDIA_RUNTIME_H

#include "ngx_media.h"
#include "ngx_media_rtmp_adapter.h"

/*
 * Shared program runtime (goal doc 22 and 33).
 *
 * One timer in the owning worker drives everything that belongs to the
 * *program*: the selection tick, the per-stream outputs (HLS, PROGRAM/ISO/RAW
 * recording) and the FLV preparation that RTMP players share.  Transport
 * modules only register sources and publish frames, so SRT, RTMP and file
 * input all feed the same logical stream abstraction (goal doc 34 item 1).
 */

#define NGX_MEDIA_RUNTIME_INTERVAL        100
#define NGX_MEDIA_RUNTIME_MAX_OUTPUTS     16
#define NGX_MEDIA_RUNTIME_MAX_PREPARE     16
#define NGX_MEDIA_RUNTIME_MAX_FRAMES_TICK 64
#define NGX_MEDIA_RUNTIME_HLS_TARGET      6000

/* idempotent: arms the runtime timer in this worker, 1 when it did */
ngx_uint_t ngx_media_runtime_arm(ngx_cycle_t *cycle, ngx_log_t *log);
void ngx_media_runtime_stop(void);

/* one scheduler visit: selector, outputs and player preparation */
void ngx_media_runtime_tick(ngx_log_t *log);

/* stops and releases per-stream outputs; called from exit_process */
void ngx_media_runtime_shutdown(ngx_log_t *log);

/* the shared FLV preparation of a stream, created on demand */
ngx_media_rtmp_prepare_t *ngx_media_runtime_prepare(ngx_media_stream_t *stream,
    ngx_log_t *log);

/* ISO tap: one named source's frames before the selector */
void ngx_media_runtime_iso_source(ngx_media_stream_t *stream,
    ngx_media_source_t *source, const ngx_media_frame_t *frame);

/* RAW tap: transport bytes before normalization */
ngx_uint_t ngx_media_runtime_raw_ready(void);
ngx_int_t ngx_media_runtime_raw_bytes(const u_char *data, size_t len);

#endif /* NGX_MEDIA_RUNTIME_H */

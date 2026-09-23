#ifndef NGX_MEDIA_EXECUTOR_H
#define NGX_MEDIA_EXECUTOR_H

#include "ngx_media_platform.h"
#include "ngx_media_buffer.h"

#include <sys/types.h>

#define NGX_MEDIA_EXECUTOR_MAX_INPUT_CHUNKS 8
#define NGX_MEDIA_EXECUTOR_DEFAULT_INPUT_BYTES (8 * 1024 * 1024)
#define NGX_MEDIA_EXECUTOR_IO_BUDGET       (256 * 1024)
#define NGX_MEDIA_EXECUTOR_MAX_PROFILE_ARG 64
#define NGX_MEDIA_EXECUTOR_MAX_EXECUTABLE 4096

#define NGX_MEDIA_EXECUTOR_STOPPED         0
#define NGX_MEDIA_EXECUTOR_RUNNING         1
#define NGX_MEDIA_EXECUTOR_RESTARTING      2
#define NGX_MEDIA_EXECUTOR_FAILED          3

#define NGX_MEDIA_EXECUTOR_EVENT_STARTED   1
#define NGX_MEDIA_EXECUTOR_EVENT_RESTARTED 2
#define NGX_MEDIA_EXECUTOR_EVENT_FAILED    3
#define NGX_MEDIA_EXECUTOR_EVENT_STOPPED   4
#define NGX_MEDIA_EXECUTOR_EVENT_EPOCH     5

typedef struct {
    ngx_str_t   executable;       /* direct exec path; never passed to a shell */
    ngx_uint_t  width;
    ngx_uint_t  height;
    ngx_uint_t  video_bitrate;
    ngx_uint_t  audio_bitrate;
    ngx_uint_t  sample_rate;
    ngx_uint_t  channels;
    ngx_uint_t  gop_frames;
    ngx_uint_t  max_input_chunks;
    size_t      max_input_bytes;
    ngx_msec_t  restart_min;
    ngx_msec_t  restart_max;
} ngx_media_executor_conf_t;

typedef struct {
    ngx_uint_t  type;
    uint64_t    epoch;
    int         status;
} ngx_media_executor_event_t;

typedef void (*ngx_media_executor_output_pt)(void *ctx, const u_char *data,
    size_t len, uint64_t epoch);
typedef void (*ngx_media_executor_event_pt)(void *ctx,
    const ngx_media_executor_event_t *event);

typedef struct {
    ngx_media_buf_t *buf;
    size_t           offset;
    size_t           len;
} ngx_media_executor_chunk_t;

typedef struct {
    ngx_media_executor_conf_t  conf;
    ngx_media_executor_output_pt output;
    ngx_media_executor_event_pt  event;
    void                        *ctx;

    ngx_media_executor_chunk_t  input[NGX_MEDIA_EXECUTOR_MAX_INPUT_CHUNKS];
    ngx_uint_t                  input_head;
    ngx_uint_t                  input_tail;
    ngx_uint_t                  input_count;
    size_t                      input_bytes;

    pid_t                       pid;
    int                         input_fd;
    int                         output_fd;
    int                         error_fd;
    ngx_uint_t                  state;
    uint64_t                    epoch;
    ngx_msec_t                  started_at;
    ngx_msec_t                  restart_at;
    ngx_msec_t                  backoff;
    uint64_t                    restarts;
    uint64_t                    input_dropped;
    uint64_t                    output_bytes;
    uint64_t                    error_bytes;
    char                        executable[NGX_MEDIA_EXECUTOR_MAX_EXECUTABLE];
} ngx_media_executor_t;

void ngx_media_executor_conf_default(ngx_media_executor_conf_t *conf);
ngx_int_t ngx_media_executor_init(ngx_media_executor_t *executor,
    const ngx_media_executor_conf_t *conf,
    ngx_media_executor_output_pt output, ngx_media_executor_event_pt event,
    void *ctx, ngx_log_t *log);

/* Starts one direct-exec ffmpeg child.  No worker thread is created. */
ngx_int_t ngx_media_executor_start(ngx_media_executor_t *executor,
    ngx_msec_t now, ngx_log_t *log);

/* Queues one immutable MPEG-TS burst without blocking the caller. */
ngx_int_t ngx_media_executor_feed(ngx_media_executor_t *executor,
    ngx_media_buf_t *buf, size_t offset, size_t len);

/* Drains child I/O, reaps exits, and performs bounded restart work. */
void ngx_media_executor_tick(ngx_media_executor_t *executor, ngx_msec_t now,
    ngx_log_t *log);

/* Kills/reaps the complete process group and releases queued payload refs. */
void ngx_media_executor_stop(ngx_media_executor_t *executor, ngx_log_t *log);

ngx_uint_t ngx_media_executor_state(const ngx_media_executor_t *executor);
uint64_t ngx_media_executor_epoch(const ngx_media_executor_t *executor);
ngx_uint_t ngx_media_executor_pending(const ngx_media_executor_t *executor);
size_t ngx_media_executor_pending_bytes(const ngx_media_executor_t *executor);

#endif /* NGX_MEDIA_EXECUTOR_H */

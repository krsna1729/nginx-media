#include "ngx_media_executor.h"
/*
 * The worker never accepts control text from the executor.  It feeds only
 * bounded MPEG-TS bursts and validates output through the TS demux contract.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define NGX_MEDIA_EXECUTOR_CLOSE_MAX 65536

static void ngx_media_executor_close_fd(int *fd);
static void ngx_media_executor_close_inherited(void);
static ngx_int_t ngx_media_executor_nonblocking(int fd);
static void ngx_media_executor_clear_input(ngx_media_executor_t *executor);
static void ngx_media_executor_event(ngx_media_executor_t *executor,
    ngx_uint_t type, int status);
static ngx_int_t ngx_media_executor_spawn(ngx_media_executor_t *executor,
    ngx_msec_t now, ngx_log_t *log);
static void ngx_media_executor_dead(ngx_media_executor_t *executor,
    ngx_msec_t now, int status, ngx_log_t *log);
static void ngx_media_executor_read_output(ngx_media_executor_t *executor,
    ngx_log_t *log);
static void ngx_media_executor_read_error(ngx_media_executor_t *executor,
    ngx_log_t *log);
static void ngx_media_executor_write_input(ngx_media_executor_t *executor,
    ngx_log_t *log);

void
ngx_media_executor_conf_default(ngx_media_executor_conf_t *conf)
{
    if (conf == NULL) {
        return;
    }

    ngx_memzero(conf, sizeof(*conf));
    conf->width = 1280;
    conf->height = 720;
    conf->video_bitrate = 3000000;
    conf->audio_bitrate = 128000;
    conf->sample_rate = 48000;
    conf->channels = 2;
    conf->gop_frames = 60;
    conf->max_input_chunks = NGX_MEDIA_EXECUTOR_MAX_INPUT_CHUNKS;
    conf->max_input_bytes = NGX_MEDIA_EXECUTOR_DEFAULT_INPUT_BYTES;
    conf->restart_min = 250;
    conf->restart_max = 5000;
}

ngx_int_t
ngx_media_executor_init(ngx_media_executor_t *executor,
    const ngx_media_executor_conf_t *conf,
    ngx_media_executor_output_pt output, ngx_media_executor_event_pt event,
    void *ctx, ngx_log_t *log)
{
    ngx_media_executor_conf_t defaults;

    (void) log;

    if (executor == NULL || conf == NULL || conf->executable.len == 0
        || conf->executable.len >= sizeof(executor->executable)
        || output == NULL)
    {
        return NGX_ERROR;
    }

    ngx_media_executor_conf_default(&defaults);
    ngx_memzero(executor, sizeof(*executor));

    executor->conf = *conf;

    if (executor->conf.max_input_chunks == 0
        || executor->conf.max_input_chunks > NGX_MEDIA_EXECUTOR_MAX_INPUT_CHUNKS)
    {
        executor->conf.max_input_chunks = defaults.max_input_chunks;
    }

    if (executor->conf.max_input_bytes == 0) {
        executor->conf.max_input_bytes = defaults.max_input_bytes;
    }

    if (executor->conf.restart_min == 0) {
        executor->conf.restart_min = defaults.restart_min;
    }

    if (executor->conf.restart_max < executor->conf.restart_min) {
        executor->conf.restart_max = defaults.restart_max;
    }

    ngx_memcpy(executor->executable, conf->executable.data,
               conf->executable.len);
    executor->executable[conf->executable.len] = '\0';

    executor->output = output;
    executor->event = event;
    executor->ctx = ctx;
    executor->pid = -1;
    executor->input_fd = -1;
    executor->output_fd = -1;
    executor->error_fd = -1;
    executor->state = NGX_MEDIA_EXECUTOR_STOPPED;
    executor->backoff = executor->conf.restart_min;

    return NGX_OK;
}

static void
ngx_media_executor_close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void) close(*fd);
        *fd = -1;
    }
}

static void
ngx_media_executor_close_inherited(void)
{
    long limit;
    int  fd;

    limit = sysconf(_SC_OPEN_MAX);
    if (limit < 3 || limit > NGX_MEDIA_EXECUTOR_CLOSE_MAX) {
        limit = NGX_MEDIA_EXECUTOR_CLOSE_MAX;
    }

    for (fd = 3; fd < limit; fd++) {
        (void) close(fd);
    }
}

static ngx_int_t
ngx_media_executor_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static void
ngx_media_executor_clear_input(ngx_media_executor_t *executor)
{
    ngx_media_executor_chunk_t *chunk;

    while (executor->input_count > 0) {
        chunk = &executor->input[executor->input_tail];
        ngx_media_buf_unref(chunk->buf);
        ngx_memzero(chunk, sizeof(*chunk));
        executor->input_tail = (executor->input_tail + 1)
                               % NGX_MEDIA_EXECUTOR_MAX_INPUT_CHUNKS;
        executor->input_count--;
    }

    executor->input_bytes = 0;
    executor->input_head = 0;
    executor->input_tail = 0;
}

static void
ngx_media_executor_event(ngx_media_executor_t *executor, ngx_uint_t type,
    int status)
{
    ngx_media_executor_event_t event;

    if (executor->event == NULL) {
        return;
    }

    event.type = type;
    event.epoch = executor->epoch;
    event.status = status;
    executor->event(executor->ctx, &event);
}

static ngx_int_t
ngx_media_executor_spawn(ngx_media_executor_t *executor, ngx_msec_t now,
    ngx_log_t *log)
{
    int     input_pipe[2], output_pipe[2], error_pipe[2];
    pid_t   pid;
    int     i;
    char    width[16], height[16], video_bitrate[32], audio_bitrate[32];
    char    sample_rate[16], channels[16], gop[16], scale[64];
    char   *argv[NGX_MEDIA_EXECUTOR_MAX_PROFILE_ARG];
    ngx_uint_t n;

    (void) log;

    input_pipe[0] = input_pipe[1] = -1;
    output_pipe[0] = output_pipe[1] = -1;
    error_pipe[0] = error_pipe[1] = -1;

    if (pipe(input_pipe) == -1 || pipe(output_pipe) == -1
        || pipe(error_pipe) == -1)
    {
        goto failed;
    }

    if (ngx_media_executor_nonblocking(input_pipe[1]) != NGX_OK
        || ngx_media_executor_nonblocking(output_pipe[0]) != NGX_OK
        || ngx_media_executor_nonblocking(error_pipe[0]) != NGX_OK)
    {
        goto failed;
    }

    (void) snprintf(width, sizeof(width), "%lu",
                    (unsigned long) executor->conf.width);
    (void) snprintf(height, sizeof(height), "%lu",
                    (unsigned long) executor->conf.height);
    (void) snprintf(video_bitrate, sizeof(video_bitrate), "%lu",
                    (unsigned long) executor->conf.video_bitrate);
    (void) snprintf(audio_bitrate, sizeof(audio_bitrate), "%lu",
                    (unsigned long) executor->conf.audio_bitrate);
    (void) snprintf(sample_rate, sizeof(sample_rate), "%lu",
                    (unsigned long) executor->conf.sample_rate);
    (void) snprintf(channels, sizeof(channels), "%lu",
                    (unsigned long) executor->conf.channels);
    (void) snprintf(gop, sizeof(gop), "%lu",
                    (unsigned long) executor->conf.gop_frames);
    (void) snprintf(scale, sizeof(scale), "scale=%s:%s", width, height);

    n = 0;
    argv[n++] = executor->executable;
    argv[n++] = (char *) "-hide_banner";
    argv[n++] = (char *) "-loglevel";
    argv[n++] = (char *) "error";
    argv[n++] = (char *) "-nostdin";
    argv[n++] = (char *) "-fflags";
    argv[n++] = (char *) "+genpts";
    argv[n++] = (char *) "-f";
    argv[n++] = (char *) "mpegts";
    argv[n++] = (char *) "-i";
    argv[n++] = (char *) "pipe:0";
    argv[n++] = (char *) "-map";
    argv[n++] = (char *) "0:v:0?";
    argv[n++] = (char *) "-map";
    argv[n++] = (char *) "0:a:0?";

    if (executor->conf.width != 0 && executor->conf.height != 0) {
        argv[n++] = (char *) "-vf";
        argv[n++] = scale;
    }

    argv[n++] = (char *) "-c:v";
    argv[n++] = (char *) "libx264";
    argv[n++] = (char *) "-preset";
    argv[n++] = (char *) "veryfast";
    argv[n++] = (char *) "-tune";
    argv[n++] = (char *) "zerolatency";
    argv[n++] = (char *) "-b:v";
    argv[n++] = video_bitrate;
    argv[n++] = (char *) "-g";
    argv[n++] = gop;
    argv[n++] = (char *) "-keyint_min";
    argv[n++] = gop;
    argv[n++] = (char *) "-sc_threshold";
    argv[n++] = (char *) "0";
    argv[n++] = (char *) "-c:a";
    argv[n++] = (char *) "aac";
    argv[n++] = (char *) "-b:a";
    argv[n++] = audio_bitrate;
    argv[n++] = (char *) "-ar";
    argv[n++] = sample_rate;
    argv[n++] = (char *) "-ac";
    argv[n++] = channels;
    argv[n++] = (char *) "-f";
    argv[n++] = (char *) "mpegts";
    argv[n++] = (char *) "-mpegts_flags";
    argv[n++] = (char *) "+resend_headers";
    argv[n++] = (char *) "-flush_packets";
    argv[n++] = (char *) "1";
    argv[n++] = (char *) "pipe:1";
    argv[n] = NULL;

    if (n >= NGX_MEDIA_EXECUTOR_MAX_PROFILE_ARG) {
        goto failed;
    }

    pid = fork();

    if (pid == -1) {
        goto failed;
    }

    if (pid == 0) {
        (void) setpgid(0, 0);

        if (dup2(input_pipe[0], STDIN_FILENO) == -1
            || dup2(output_pipe[1], STDOUT_FILENO) == -1
            || dup2(error_pipe[1], STDERR_FILENO) == -1)
        {
            _exit(126);
        }

        for (i = 0; i < 2; i++) {
            (void) close(input_pipe[i]);
            (void) close(output_pipe[i]);
            (void) close(error_pipe[i]);
        }
        ngx_media_executor_close_inherited();

        (void) execv(executor->executable, argv);
        _exit(127);
    }

    (void) setpgid(pid, pid);

    (void) close(input_pipe[0]);
    (void) close(output_pipe[1]);
    (void) close(error_pipe[1]);

    executor->pid = pid;
    executor->input_fd = input_pipe[1];
    executor->output_fd = output_pipe[0];
    executor->error_fd = error_pipe[0];
    executor->state = NGX_MEDIA_EXECUTOR_RUNNING;
    executor->started_at = now;
    executor->restart_at = 0;
    executor->epoch++;

    ngx_media_executor_event(executor,
                             (executor->restarts == 0)
                             ? NGX_MEDIA_EXECUTOR_EVENT_STARTED
                             : NGX_MEDIA_EXECUTOR_EVENT_RESTARTED,
                             0);
    ngx_media_executor_event(executor, NGX_MEDIA_EXECUTOR_EVENT_EPOCH, 0);

    return NGX_OK;

failed:
    for (i = 0; i < 2; i++) {
        if (input_pipe[i] >= 0) {
            (void) close(input_pipe[i]);
        }
        if (output_pipe[i] >= 0) {
            (void) close(output_pipe[i]);
        }
        if (error_pipe[i] >= 0) {
            (void) close(error_pipe[i]);
        }
    }

    return NGX_ERROR;
}

ngx_int_t
ngx_media_executor_start(ngx_media_executor_t *executor, ngx_msec_t now,
    ngx_log_t *log)
{
    if (executor == NULL || executor->executable[0] == '\0') {
        return NGX_ERROR;
    }

    if (executor->state == NGX_MEDIA_EXECUTOR_RUNNING) {
        return NGX_OK;
    }

    if (executor->state == NGX_MEDIA_EXECUTOR_FAILED) {
        return NGX_ERROR;
    }

    if (executor->restart_at != 0 && now < executor->restart_at) {
        return NGX_AGAIN;
    }

    if (ngx_media_executor_spawn(executor, now, log) != NGX_OK) {
        executor->state = NGX_MEDIA_EXECUTOR_RESTARTING;
        executor->restart_at = now + executor->backoff;
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_executor_feed(ngx_media_executor_t *executor,
    ngx_media_buf_t *buf, size_t offset, size_t len)
{
    ngx_media_executor_chunk_t *chunk;

    if (executor == NULL || buf == NULL || len == 0
        || offset > buf->len || len > buf->len - offset)
    {
        return NGX_ERROR;
    }

    if (executor->state != NGX_MEDIA_EXECUTOR_RUNNING
        || executor->input_count >= executor->conf.max_input_chunks
        || len > executor->conf.max_input_bytes
        || executor->input_bytes > executor->conf.max_input_bytes - len)
    {
        executor->input_dropped++;
        return NGX_AGAIN;
    }

    chunk = &executor->input[executor->input_head];
    chunk->buf = ngx_media_buf_ref(buf);
    chunk->offset = offset;
    chunk->len = len;
    executor->input_head = (executor->input_head + 1)
                           % NGX_MEDIA_EXECUTOR_MAX_INPUT_CHUNKS;
    executor->input_count++;
    executor->input_bytes += len;

    return NGX_OK;
}

static void
ngx_media_executor_read_output(ngx_media_executor_t *executor, ngx_log_t *log)
{
    u_char  buffer[NGX_MEDIA_EXECUTOR_IO_BUDGET];
    ssize_t n;
    size_t  total = 0;

    (void) log;

    while (total < sizeof(buffer)) {
        n = read(executor->output_fd, buffer,
                 sizeof(buffer) - total);

        if (n > 0) {
            executor->output_bytes += (uint64_t) n;
            executor->output(executor->ctx, buffer, (size_t) n,
                            executor->epoch);
            total += (size_t) n;
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        break;
    }
}

static void
ngx_media_executor_read_error(ngx_media_executor_t *executor, ngx_log_t *log)
{
    u_char  buffer[4096];
    ssize_t n;

    while (executor->error_fd >= 0) {
        n = read(executor->error_fd, buffer, sizeof(buffer) - 1);

        if (n > 0) {
            executor->error_bytes += (uint64_t) n;
            buffer[n] = '\0';
            ngx_log_error(NGX_LOG_WARN, log, 0, "media executor: %s", buffer);
            continue;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        break;
    }
}

static void
ngx_media_executor_write_input(ngx_media_executor_t *executor, ngx_log_t *log)
{
    ngx_media_executor_chunk_t *chunk;
    ssize_t                       n;
    size_t                        budget = NGX_MEDIA_EXECUTOR_IO_BUDGET;

    (void) log;

    while (executor->input_count > 0 && budget > 0) {
        chunk = &executor->input[executor->input_tail];

        if (chunk->len > budget) {
            n = write(executor->input_fd,
                      chunk->buf->data + chunk->offset, budget);
        } else {
            n = write(executor->input_fd,
                      chunk->buf->data + chunk->offset, chunk->len);
        }

        if (n > 0) {
            chunk->offset += (size_t) n;
            chunk->len -= (size_t) n;
            executor->input_bytes -= (size_t) n;
            budget -= (size_t) n;

            if (chunk->len == 0) {
                ngx_media_buf_unref(chunk->buf);
                ngx_memzero(chunk, sizeof(*chunk));
                executor->input_tail = (executor->input_tail + 1)
                                       % NGX_MEDIA_EXECUTOR_MAX_INPUT_CHUNKS;
                executor->input_count--;
            }

            continue;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        executor->state = NGX_MEDIA_EXECUTOR_FAILED;
        break;
    }
}

static void
ngx_media_executor_dead(ngx_media_executor_t *executor, ngx_msec_t now,
    int status, ngx_log_t *log)
{
    pid_t pid;

    if (executor->pid > 0) {
        pid = executor->pid;
        (void) kill(-pid, SIGKILL);
        (void) waitpid(pid, NULL, 0);
    }
    ngx_media_executor_close_fd(&executor->input_fd);
    ngx_media_executor_close_fd(&executor->output_fd);
    ngx_media_executor_close_fd(&executor->error_fd);
    ngx_media_executor_clear_input(executor);
    executor->pid = -1;

    ngx_media_executor_event(executor, NGX_MEDIA_EXECUTOR_EVENT_FAILED, status);

    if (executor->restarts >= 32) {
        executor->state = NGX_MEDIA_EXECUTOR_FAILED;
        ngx_media_executor_event(executor, NGX_MEDIA_EXECUTOR_EVENT_STOPPED,
                                 status);
        return;
    }

    executor->state = NGX_MEDIA_EXECUTOR_RESTARTING;
    executor->restart_at = now + executor->backoff;

    if (executor->backoff < executor->conf.restart_max) {
        executor->backoff *= 2;
        if (executor->backoff > executor->conf.restart_max) {
            executor->backoff = executor->conf.restart_max;
        }
    }

    executor->restarts++;

    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "media executor exited; restart at %M",
                  executor->restart_at);
}

void
ngx_media_executor_tick(ngx_media_executor_t *executor, ngx_msec_t now,
    ngx_log_t *log)
{
    int     status;
    pid_t   result;

    if (executor == NULL) {
        return;
    }

    if (executor->state == NGX_MEDIA_EXECUTOR_RESTARTING) {
        (void) ngx_media_executor_start(executor, now, log);
        return;
    }

    if (executor->state != NGX_MEDIA_EXECUTOR_RUNNING) {
        return;
    }

    ngx_media_executor_read_output(executor, log);
    ngx_media_executor_read_error(executor, log);
    ngx_media_executor_write_input(executor, log);

    if (executor->state == NGX_MEDIA_EXECUTOR_FAILED) {
        ngx_media_executor_dead(executor, now, EPIPE, log);
        return;
    }

    result = waitpid(executor->pid, &status, WNOHANG);

    if (result == executor->pid) {
        ngx_media_executor_dead(executor, now, status, log);
        return;
    }

    if (result == -1 && errno != EINTR) {
        ngx_media_executor_dead(executor, now, errno, log);
        return;
    }

    if (executor->started_at != 0 && now - executor->started_at > 10000) {
        executor->restarts = 0;
        executor->backoff = executor->conf.restart_min;
    }
}

void
ngx_media_executor_stop(ngx_media_executor_t *executor, ngx_log_t *log)
{
    pid_t pid;

    (void) log;

    if (executor == NULL) {
        return;
    }

    pid = executor->pid;

    if (pid > 0) {
        (void) kill(-pid, SIGTERM);
        (void) kill(-pid, SIGKILL);
        (void) waitpid(pid, NULL, 0);
    }

    ngx_media_executor_close_fd(&executor->input_fd);
    ngx_media_executor_close_fd(&executor->output_fd);
    ngx_media_executor_close_fd(&executor->error_fd);
    ngx_media_executor_clear_input(executor);
    executor->pid = -1;
    executor->state = NGX_MEDIA_EXECUTOR_STOPPED;
    ngx_media_executor_event(executor, NGX_MEDIA_EXECUTOR_EVENT_STOPPED, 0);
}

ngx_uint_t
ngx_media_executor_state(const ngx_media_executor_t *executor)
{
    return (executor != NULL) ? executor->state : NGX_MEDIA_EXECUTOR_STOPPED;
}

uint64_t
ngx_media_executor_epoch(const ngx_media_executor_t *executor)
{
    return (executor != NULL) ? executor->epoch : 0;
}

ngx_uint_t
ngx_media_executor_pending(const ngx_media_executor_t *executor)
{
    return (executor != NULL) ? executor->input_count : 0;
}

size_t
ngx_media_executor_pending_bytes(const ngx_media_executor_t *executor)
{
    return (executor != NULL) ? executor->input_bytes : 0;
}

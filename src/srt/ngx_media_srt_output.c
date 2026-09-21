#include "ngx_media_srt_output.h"

/* the transport payload size of a live SRT stream */
#define NGX_MEDIA_SRT_SEND_CHUNK 1316

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

/*
 * A destination: a bounded queue of prepared bursts, one SRT caller session
 * and the thread that drains the queue.  Transport progress is per
 * destination so a stalled receiver can never delay another output; the
 * preparation itself is shared by every destination of the same program.
 */
typedef struct {
    ngx_media_srt_output_conf_t  conf;

    ngx_media_srt_queue_t        queue;
    ngx_media_srt_session_t     *session;

    pthread_mutex_t              mutex;
    pthread_cond_t               cond;
    ngx_uint_t                   running;

    uint64_t                     cursor;
    uint64_t                     sent_bytes;
    uint64_t                     sent_bursts;
    uint64_t                     reconnects;

    pthread_t                    thread;
    ngx_uint_t                   thread_started;

    /* a runtime destination occupies a slot; removed ones are reusable */
    ngx_uint_t                   used;
} ngx_media_srt_output_t;

struct ngx_media_srt_outputs_s {
    ngx_media_srt_output_t  *destinations;
    ngx_uint_t               count;

    ngx_media_srt_out_event_t *events;
    ngx_uint_t                events_capacity;
    ngx_uint_t                stopping;
    uint64_t                  events_head;
    uint64_t                  events_tail;
    pthread_mutex_t           events_mutex;

    int                       notify_fd;
    ngx_log_t                *log;
};

static void
ngx_media_srt_out_notify(ngx_media_srt_outputs_t *outs)
{
    uint64_t  one = 1;
    ssize_t   n;

    if (outs->notify_fd < 0) {
        return;
    }

    /*
     * A wakeup for the event loop.  A full pipe means the loop has not run
     * yet and is about to see the event anyway, so EAGAIN is success here -
     * but the result has to be read: glibc marks write() as warn_unused_result
     * and a build with -Werror rejects discarding it, which is how this was
     * found (GCC 13 on Ubuntu 24.04, the same toolchain CI uses).
     */
    n = write(outs->notify_fd, &one, sizeof(one));

    if (n < 0 && errno != EAGAIN && errno != EINTR && outs->log != NULL) {
        ngx_log_error(NGX_LOG_WARN, outs->log, ngx_errno,
                      "media: srt output wakeup failed");
    }
}

static void
ngx_media_srt_out_event_push(ngx_media_srt_outputs_t *outs,
    const ngx_media_srt_out_event_t *event)
{
    ngx_media_srt_out_event_t  *slot;

    (void) pthread_mutex_lock(&outs->events_mutex);

    if (outs->events_head - outs->events_tail < outs->events_capacity) {
        slot = &outs->events[outs->events_head % outs->events_capacity];
        *slot = *event;
        outs->events_head++;
    }

    (void) pthread_mutex_unlock(&outs->events_mutex);

    ngx_media_srt_out_notify(outs);
}

ngx_uint_t
ngx_media_srt_outputs_event_read(ngx_media_srt_outputs_t *outs,
    ngx_media_srt_out_event_t *out, ngx_uint_t max)
{
    ngx_uint_t  count = 0;

    if (outs == NULL || out == NULL || max == 0) {
        return 0;
    }

    (void) pthread_mutex_lock(&outs->events_mutex);

    while (count < max && outs->events_tail < outs->events_head) {
        out[count++] = outs->events[outs->events_tail % outs->events_capacity];
        outs->events_tail++;
    }

    (void) pthread_mutex_unlock(&outs->events_mutex);

    return count;
}

int
ngx_media_srt_outputs_notify_fd(ngx_media_srt_outputs_t *outs)
{
    return (outs != NULL) ? outs->notify_fd : -1;
}

/* reports a status change to the worker */
static void
ngx_media_srt_out_report(ngx_media_srt_outputs_t *outs, ngx_uint_t index,
    ngx_uint_t type)
{
    ngx_media_srt_out_event_t  event;

    memset(&event, 0, sizeof(event));

    event.type = type;
    event.index = index;

    (void) pthread_mutex_lock(&outs->destinations[index].mutex);

    event.sent_bytes = outs->destinations[index].sent_bytes;
    event.sent_bursts = outs->destinations[index].sent_bursts;
    event.reconnects = outs->destinations[index].reconnects;
    event.dropped = outs->destinations[index].queue.dropped;

    (void) pthread_mutex_unlock(&outs->destinations[index].mutex);

    ngx_media_srt_out_event_push(outs, &event);
}

static void *
ngx_media_srt_out_thread(void *data)
{
    ngx_media_srt_outputs_t  *outs = data;
    ngx_media_srt_output_t   *dest;
    const ngx_media_srt_unit_t  *unit;
    ngx_uint_t                i;
    ngx_int_t                 rc;

    for ( ;; ) {

        if (outs->stopping) {
            return NULL;
        }

        for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
            dest = &outs->destinations[i];

            /*
             * An unused slot is skipped, not an exit: destinations are added
             * and removed at runtime, and a slot being idle says nothing
             * about whether the pool should live.  Only outs->stopping ends
             * a sender.
             */
            if (!dest->used) {
                continue;
            }

            (void) pthread_mutex_lock(&dest->mutex);

            if (!dest->running) {
                (void) pthread_mutex_unlock(&dest->mutex);
                continue;
            }

            /* (re)connect when needed, with a one second backoff */
            if (dest->session == NULL) {

                if (dest->reconnects > 0) {
                    struct timespec  ts;

                    (void) clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_sec += 1;

                    (void) pthread_cond_timedwait(&dest->cond, &dest->mutex,
                                                  &ts);
                }

                if (!dest->running) {
                    (void) pthread_mutex_unlock(&dest->mutex);
                    return NULL;
                }

                dest->session = ngx_media_srt_connect(
                    dest->conf.host.data, dest->conf.port,
                    dest->conf.streamid.len ? dest->conf.streamid.data : NULL,
                    dest->conf.streamid.len, dest->conf.connect_timeout,
                    dest->conf.params, outs->log);

                if (dest->session == NULL) {
                    dest->reconnects++;

                    (void) pthread_mutex_unlock(&dest->mutex);
                    ngx_media_srt_out_report(outs, i,
                                             NGX_MEDIA_SRT_OUT_EVENT_FAILED);
                    continue;
                }

                dest->reconnects++;
                dest->cursor = 0;

                /*
                 * A fresh receiver must start at a sync boundary, never in
                 * the middle of a GOP (goal doc 34 item 6).
                 */
                ngx_media_srt_queue_resync(&dest->queue);

                (void) pthread_mutex_unlock(&dest->mutex);
                ngx_media_srt_out_report(outs, i,
                                         NGX_MEDIA_SRT_OUT_EVENT_CONNECTED);
                continue;
            }

            unit = ngx_media_srt_queue_next(&dest->queue, &dest->cursor);

            if (unit == NULL) {
                struct timespec  ts;

                (void) clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 20 * 1000 * 1000;

                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }

                (void) pthread_cond_timedwait(&dest->cond, &dest->mutex, &ts);

                (void) pthread_mutex_unlock(&dest->mutex);
                continue;
            }

            /*
             * A live SRT sender hands the transport payload-sized pieces: one
             * 256 KB burst would exceed the transport's message budget.
             */
            {
                size_t  off = 0;

                rc = 1;

                while (off < unit->len) {
                    size_t  take = unit->len - off;

                    if (take > NGX_MEDIA_SRT_SEND_CHUNK) {
                        take = NGX_MEDIA_SRT_SEND_CHUNK;
                    }

                    rc = ngx_media_srt_session_send(
                        dest->session, ngx_media_buf_data(unit->burst) + off,
                        take, dest->conf.send_timeout);

                    if (rc <= 0) {
                        break;
                    }

                    off += take;
                }

                if (rc > 0) {
                    rc = (ngx_int_t) off;
                }
            }

            if (rc == 0) {
                /*
                 * Backpressure: the transport could not take the buffer.
                 * Nothing is lost; wait briefly and offer it again.
                 */
                struct timespec  ts;

                (void) clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 20 * 1000 * 1000;

                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }

                (void) pthread_cond_timedwait(&dest->cond, &dest->mutex, &ts);

                (void) pthread_mutex_unlock(&dest->mutex);
                continue;
            }

            if (rc < 0) {
                ngx_media_srt_session_close(dest->session);
                dest->session = NULL;
                dest->reconnects++;

                /* the next connection resumes at a sync boundary */
                ngx_media_srt_queue_resync(&dest->queue);

                (void) pthread_mutex_unlock(&dest->mutex);

                ngx_media_srt_out_report(outs, i,
                                         NGX_MEDIA_SRT_OUT_EVENT_FAILED);
                continue;
            }

            ngx_media_srt_queue_advance(&dest->queue, &dest->cursor,
                                        unit->sequence);

            dest->sent_bytes += (uint64_t) rc;
            dest->sent_bursts++;

            (void) pthread_mutex_unlock(&dest->mutex);
        }
    }
}

ngx_int_t
ngx_media_srt_outputs_start(ngx_media_srt_outputs_t **out,
    const ngx_media_srt_output_conf_t *confs, ngx_uint_t count,
    ngx_uint_t max_events, ngx_log_t *log)
{
    ngx_media_srt_outputs_t  *outs;
    ngx_uint_t                declared = count;
    ngx_uint_t                i;

    if (out == NULL || confs == NULL || count > NGX_MEDIA_SRT_MAX_OUTPUTS) {
        return NGX_ERROR;
    }

    outs = ngx_alloc(sizeof(ngx_media_srt_outputs_t), log);

    if (outs == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(outs, sizeof(ngx_media_srt_outputs_t));

    outs->log = log;
    outs->notify_fd = -1;
    outs->count = count;

    outs->events_capacity = max_events ? max_events
                                       : NGX_MEDIA_SRT_OUT_MAX_EVENTS;
    outs->events = ngx_alloc(outs->events_capacity
                             * sizeof(ngx_media_srt_out_event_t), log);

    if (outs->events == NULL) {
        ngx_free(outs);
        return NGX_ERROR;
    }

    ngx_memzero(outs->events,
                outs->events_capacity * sizeof(ngx_media_srt_out_event_t));

    (void) pthread_mutex_init(&outs->events_mutex, NULL);

    /*
     * The full capacity is allocated up front: destinations can be added and
     * removed at runtime (normative revision), and a slot is reusable.
     */
    outs->destinations = ngx_alloc(NGX_MEDIA_SRT_MAX_OUTPUTS
                                   * sizeof(ngx_media_srt_output_t), log);

    if (outs->destinations == NULL) {
        ngx_free(outs->events);
        ngx_free(outs);
        return NGX_ERROR;
    }

    ngx_memzero(outs->destinations, NGX_MEDIA_SRT_MAX_OUTPUTS
                                    * sizeof(ngx_media_srt_output_t));

    for (i = 0; i < declared; i++) {
        ngx_media_srt_output_t  *dest = &outs->destinations[i];

        dest->conf = confs[i];
        dest->running = 1;
        dest->used = 1;

        ngx_media_srt_queue_init(&dest->queue, dest->conf.max_units,
                                 dest->conf.max_bytes);

        (void) pthread_mutex_init(&dest->mutex, NULL);
        (void) pthread_cond_init(&dest->cond, NULL);
    }

    outs->notify_fd = eventfd(0, EFD_NONBLOCK);

    if (outs->notify_fd < 0) {
        goto failed;
    }

    /*
     * The senders are a pool that walks the table, and a runtime destination
     * needs at least one of them.  A configuration with no declared
     * destinations therefore still gets one sender, or adding a destination
     * through the control API would have nothing to carry it.
     */
    if (count == 0) {
        count = 1;
    }

    for (i = 0; i < count; i++) {

        if (pthread_create(&outs->destinations[i].thread, NULL,
                           ngx_media_srt_out_thread, outs) != 0)
        {
            goto failed;
        }

        outs->destinations[i].thread_started = 1;
    }

    *out = outs;

    return NGX_OK;

failed:

    ngx_media_srt_outputs_stop(outs);

    return NGX_ERROR;
}

/*
 * Adds a destination while the program is running.  The slot is reusable, so
 * add/remove cycles do not exhaust the table.
 */
ngx_int_t
ngx_media_srt_outputs_add(ngx_media_srt_outputs_t *outs,
    const ngx_media_srt_output_conf_t *conf, ngx_uint_t *index, ngx_log_t *log)
{
    ngx_media_srt_output_t  *dest = NULL;
    ngx_uint_t               i;

    if (outs == NULL || conf == NULL || conf->host.data == NULL
        || conf->port == 0)
    {
        return NGX_ERROR;
    }

    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {

        if (!outs->destinations[i].used) {
            dest = &outs->destinations[i];
            break;
        }
    }

    if (dest == NULL) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: no free srt destination slot");
        return NGX_ERROR;
    }

    dest->conf = *conf;
    dest->running = 1;
    dest->used = 1;
    dest->cursor = 0;
    dest->sent_bytes = 0;
    dest->sent_bursts = 0;
    dest->reconnects = 0;
    dest->thread_started = 0;

    ngx_media_srt_queue_init(&dest->queue, dest->conf.max_units,
                             dest->conf.max_bytes);

    (void) pthread_mutex_init(&dest->mutex, NULL);
    (void) pthread_cond_init(&dest->cond, NULL);

    /*
     * No thread is created here.  The senders are a pool that walks the whole
     * table, so a new slot is picked up by threads that already exist; giving
     * a runtime destination its own thread would mean a thread whose exit
     * condition is another destination's, which cannot be joined cleanly.
     */
    if (outs->count < NGX_MEDIA_SRT_MAX_OUTPUTS) {
        outs->count++;
    }

    if (index != NULL) {
        *index = i;
    }

    return NGX_OK;
}

/*
 * Removes a destination: the sender thread is told to stop and joined before
 * the slot is released, so it is never left writing into an object that no
 * longer exists.
 */
void
ngx_media_srt_outputs_remove(ngx_media_srt_outputs_t *outs, ngx_uint_t index)
{
    ngx_media_srt_output_t  *dest;

    if (outs == NULL || index >= NGX_MEDIA_SRT_MAX_OUTPUTS) {
        return;
    }

    dest = &outs->destinations[index];

    if (!dest->used) {
        return;
    }

    /*
     * Stopping the slot is enough: the pool skips it, and closing the session
     * makes any in-flight transport call return.  There is no thread to join
     * because the slot never had one of its own.
     */
    (void) pthread_mutex_lock(&dest->mutex);
    dest->running = 0;
    (void) pthread_cond_broadcast(&dest->cond);

    if (dest->session != NULL) {
        ngx_media_srt_session_close(dest->session);
        dest->session = NULL;
    }

    (void) pthread_mutex_unlock(&dest->mutex);

    dest->used = 0;

    if (outs->count > 0) {
        outs->count--;
    }
}

void
ngx_media_srt_outputs_stop(ngx_media_srt_outputs_t *outs)
{
    ngx_uint_t  i;

    if (outs == NULL) {
        return;
    }

    outs->stopping = 1;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        ngx_media_srt_output_t  *dest = &outs->destinations[i];

        if (!dest->thread_started) {
            continue;
        }

        /*
         * The sender may hold the mutex inside a blocking transport call:
         * closing the session first makes that call return immediately, so
         * the join below cannot wait on a transport timeout.
         */
        if (dest->session != NULL) {
            ngx_media_srt_session_close(dest->session);
            dest->session = NULL;
        }

        (void) pthread_mutex_lock(&dest->mutex);
        dest->running = 0;
        (void) pthread_cond_broadcast(&dest->cond);
        (void) pthread_mutex_unlock(&dest->mutex);
    }

    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        ngx_media_srt_output_t  *dest = &outs->destinations[i];

        if (dest->thread_started) {
            (void) pthread_join(dest->thread, NULL);
            dest->thread_started = 0;
        }

        if (dest->session != NULL) {
            ngx_media_srt_session_close(dest->session);
            dest->session = NULL;
        }

        ngx_media_srt_queue_destroy(&dest->queue);
        (void) pthread_cond_destroy(&dest->cond);
        (void) pthread_mutex_destroy(&dest->mutex);
    }

    if (outs->notify_fd >= 0) {
        (void) close(outs->notify_fd);
        outs->notify_fd = -1;
    }

    if (outs->destinations != NULL) {
        ngx_free(outs->destinations);
    }

    if (outs->events != NULL) {
        ngx_free(outs->events);
    }

    (void) pthread_mutex_destroy(&outs->events_mutex);

    ngx_free(outs);
}

ngx_int_t
ngx_media_srt_outputs_push(ngx_media_srt_outputs_t *outs,
    const ngx_str_t *application, const ngx_str_t *stream,
    ngx_media_buf_t *burst, size_t len, ngx_uint_t keyframe)
{
    ngx_uint_t  i, matched = 0;

    if (outs == NULL || application == NULL || stream == NULL
        || burst == NULL || len == 0)
    {
        return NGX_ERROR;
    }

    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        ngx_media_srt_output_t  *dest = &outs->destinations[i];

        if (dest->conf.application.len != application->len
            || dest->conf.stream.len != stream->len
            || ngx_memcmp(dest->conf.application.data, application->data,
                          application->len) != 0
            || ngx_memcmp(dest->conf.stream.data, stream->data,
                          stream->len) != 0)
        {
            continue;
        }

        matched++;

        (void) pthread_mutex_lock(&dest->mutex);
        (void) ngx_media_srt_queue_push(&dest->queue, burst, len, keyframe);
        (void) pthread_cond_broadcast(&dest->cond);
        (void) pthread_mutex_unlock(&dest->mutex);
    }

    return matched ? NGX_OK : NGX_DECLINED;
}

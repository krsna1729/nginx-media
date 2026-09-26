#include "ngx_media_srt_output.h"
#include "ngx_media_egress_manager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

/*
 * A destination owns one bounded subscriber queue and one caller session.
 * Egress shards are assigned by slot index modulo NGX_MEDIA_SRT_EGRESS_SHARDS.
 */
#define NGX_MEDIA_SRT_SEND_CHUNK          1316
#define NGX_MEDIA_SRT_FEED_MAX_UNITS      64
#define NGX_MEDIA_SRT_FEED_MAX_BYTES      (8 * 1024 * 1024)

#define NGX_MEDIA_SRT_DEST_RETRY          1000
typedef struct {
    ngx_media_buf_t  *burst;
    size_t            len;
    uint64_t          route;
    unsigned          keyframe:1;
} ngx_media_srt_feed_unit_t;

typedef struct {
    ngx_media_srt_feed_unit_t  units[NGX_MEDIA_SRT_FEED_MAX_UNITS];
    uint64_t                   head;
    uint64_t                   tail;
    size_t                     bytes;
    uint64_t                   dropped;
    unsigned                   resync:1;
    pthread_mutex_t            mutex;
} ngx_media_srt_feed_queue_t;

typedef struct {
    uintptr_t    program;
    uint64_t     incarnation;
    uint64_t     token;
    ngx_uint_t   destinations;
    ngx_uint_t   per_shard[NGX_MEDIA_SRT_EGRESS_SHARDS];
    unsigned     used:1;
    unsigned     static_bound:1;
} ngx_media_srt_program_route_t;

typedef struct {
    ngx_media_srt_output_conf_t  conf;
    ngx_media_srt_queue_t        queue;
    ngx_media_srt_session_t     *session;
    ngx_media_buf_t             *inflight_burst;
    pthread_mutex_t              mutex;
    ngx_uint_t                   shard;
    ngx_uint_t                   running;
    ngx_uint_t                   used;
    ngx_uint_t                   sending;
    ngx_uint_t                   connecting;
    uint64_t                     route;
    uint64_t                     epoch;
    uint64_t                     cursor;
    uint64_t                     inflight_sequence;
    size_t                       inflight_len;
    size_t                       inflight_offset;
    uint64_t                     sent_bytes;
    uint64_t                     sent_bursts;
    uint64_t                     blocked_sends;
    uint64_t                     retransmitted_packets;
    uint64_t                     reconnects;
    uint64_t                     transport_errors;
    uint64_t                     reported_bytes;
    uint64_t                     reported_dropped;
    uint64_t                     reported_errors;
    uint64_t                     reported_blocked;
    uint64_t                     reported_reconnects;
    ngx_msec_t                   retry_at;
    ngx_msec_t                   stats_at;
    ngx_msec_t                   egress_at;
} ngx_media_srt_output_t;
typedef struct {
    ngx_media_srt_outputs_t     *outs;
    ngx_media_srt_feed_queue_t   feed;
    pthread_t                    thread;
    ngx_uint_t                   id;
    uint64_t                     last_cpu_ns;
    pthread_mutex_t              lane_mutex;
    pthread_mutex_t              wake_mutex;
    pthread_cond_t               wake_cond;
    uint64_t                     wake_generation;
} ngx_media_srt_sender_t;

struct ngx_media_srt_outputs_s {
    ngx_media_srt_output_t         *destinations;
    ngx_media_srt_program_route_t   routes[NGX_MEDIA_SRT_MAX_OUTPUTS];
    ngx_uint_t                      count;
    ngx_uint_t                      static_slots[NGX_MEDIA_SRT_MAX_OUTPUTS];
    ngx_uint_t                      static_count;
    uint64_t                        route_next_token;
    pthread_rwlock_t                destinations_lock;
    ngx_media_srt_sender_t          shards[NGX_MEDIA_SRT_EGRESS_SHARDS];
    ngx_uint_t                      nthreads;
    ngx_atomic_t                  active_senders;
    uint64_t                        last_adapt_ns;
    ngx_uint_t                      cpu_sampled;

    ngx_media_srt_out_event_t      *events;
    ngx_uint_t                      events_capacity;
    ngx_atomic_t                    stopping;
    uint64_t                        events_head;
    uint64_t                        events_tail;
    pthread_mutex_t                 events_mutex;

    int                             notify_fd;
    ngx_log_t                      *log;
};

static uint64_t ngx_media_srt_now_ns(void);

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


static ngx_media_srt_program_route_t *
ngx_media_srt_route_find(ngx_media_srt_outputs_t *outs, uintptr_t program,
    uint64_t incarnation)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        if (outs->routes[i].used
            && outs->routes[i].program == program
            && outs->routes[i].incarnation == incarnation)
        {
            return &outs->routes[i];
        }
    }

    return NULL;
}

static ngx_media_srt_program_route_t *
ngx_media_srt_route_token(ngx_media_srt_outputs_t *outs, uint64_t token)
{
    ngx_uint_t  i;

    if (token == 0) {
        return NULL;
    }

    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        if (outs->routes[i].used && outs->routes[i].token == token) {
            return &outs->routes[i];
        }
    }

    return NULL;
}

static ngx_media_srt_program_route_t *
ngx_media_srt_route_create(ngx_media_srt_outputs_t *outs, uintptr_t program,
    uint64_t incarnation)
{
    ngx_media_srt_program_route_t  *route;
    ngx_uint_t                      i;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        route = &outs->routes[i];

        if (!route->used
            || (route->destinations == 0 && !route->static_bound))
        {
            ngx_memzero(route, sizeof(*route));

            outs->route_next_token++;
            if (outs->route_next_token == 0) {
                outs->route_next_token++;
            }

            route->program = program;
            route->incarnation = incarnation;
            route->token = outs->route_next_token;
            route->used = 1;

            return route;
        }
    }

    return NULL;
}

static void
ngx_media_srt_route_add_destination(ngx_media_srt_outputs_t *outs,
    ngx_media_srt_output_t *dest)
{
    ngx_media_srt_program_route_t  *route;

    route = ngx_media_srt_route_token(outs, dest->route);
    if (route == NULL) {
        return;
    }

    route->destinations++;
    route->per_shard[dest->shard]++;
}

static void
ngx_media_srt_route_remove_destination(ngx_media_srt_outputs_t *outs,
    ngx_media_srt_output_t *dest)
{
    ngx_media_srt_program_route_t  *route;

    route = ngx_media_srt_route_token(outs, dest->route);
    if (route == NULL) {
        return;
    }

    if (route->destinations > 0) {
        route->destinations--;
    }
    if (route->per_shard[dest->shard] > 0) {
        route->per_shard[dest->shard]--;
    }
    if (route->destinations == 0) {
        ngx_memzero(route, sizeof(*route));
    }

    dest->route = 0;
}

static void
ngx_media_srt_feed_drop_all(ngx_media_srt_feed_queue_t *queue)
{
    ngx_media_srt_feed_unit_t  *unit;

    while (queue->tail < queue->head) {
        unit = &queue->units[queue->tail % NGX_MEDIA_SRT_FEED_MAX_UNITS];

        if (unit->burst != NULL) {
            queue->bytes -= unit->len;
            ngx_media_buf_unref(unit->burst);
            unit->burst = NULL;
            unit->len = 0;
        }

        queue->tail++;
        queue->dropped++;
    }
}

static ngx_int_t
ngx_media_srt_feed_push(ngx_media_srt_sender_t *sender, uint64_t route,
    ngx_media_buf_t *burst, size_t len, ngx_uint_t keyframe)
{
    ngx_media_srt_feed_queue_t  *queue = &sender->feed;
    ngx_media_srt_feed_unit_t  *unit;
    ngx_uint_t                  index;

    (void) pthread_mutex_lock(&queue->mutex);

    if (len > NGX_MEDIA_SRT_FEED_MAX_BYTES) {
        ngx_media_srt_feed_drop_all(queue);
        queue->dropped++;
        queue->resync = 1;
        (void) pthread_mutex_unlock(&queue->mutex);
        return NGX_OK;
    }

    if (queue->resync && !keyframe) {
        queue->dropped++;
        (void) pthread_mutex_unlock(&queue->mutex);
        return NGX_OK;
    }

    if (queue->head - queue->tail >= NGX_MEDIA_SRT_FEED_MAX_UNITS
        || queue->bytes > NGX_MEDIA_SRT_FEED_MAX_BYTES - len)
    {
        ngx_media_srt_feed_drop_all(queue);
        queue->resync = 1;

        if (!keyframe) {
            queue->dropped++;
            (void) pthread_mutex_unlock(&queue->mutex);
            return NGX_OK;
        }
    }

    queue->resync = 0;
    index = (ngx_uint_t) (queue->head % NGX_MEDIA_SRT_FEED_MAX_UNITS);
    unit = &queue->units[index];

    if (unit->burst != NULL) {
        ngx_media_srt_feed_drop_all(queue);
        queue->resync = 0;
        index = (ngx_uint_t) (queue->head % NGX_MEDIA_SRT_FEED_MAX_UNITS);
        unit = &queue->units[index];
    }

    unit->burst = ngx_media_buf_ref(burst);
    unit->len = len;
    unit->route = route;
    unit->keyframe = keyframe ? 1 : 0;
    queue->bytes += len;
    queue->head++;

    (void) pthread_mutex_unlock(&queue->mutex);

    return NGX_OK;
}

static ngx_uint_t
ngx_media_srt_feed_pop(ngx_media_srt_sender_t *sender,
    ngx_media_srt_feed_unit_t *out)
{
    ngx_media_srt_feed_queue_t  *queue = &sender->feed;
    ngx_media_srt_feed_unit_t  *unit;

    (void) pthread_mutex_lock(&queue->mutex);

    if (queue->tail == queue->head) {
        (void) pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    unit = &queue->units[queue->tail % NGX_MEDIA_SRT_FEED_MAX_UNITS];
    *out = *unit;
    ngx_memzero(unit, sizeof(*unit));
    queue->tail++;
    queue->bytes -= out->len;

    (void) pthread_mutex_unlock(&queue->mutex);

    return 1;
}

static void
ngx_media_srt_feed_destroy(ngx_media_srt_feed_queue_t *queue)
{
    (void) pthread_mutex_lock(&queue->mutex);
    ngx_media_srt_feed_drop_all(queue);
    (void) pthread_mutex_unlock(&queue->mutex);
    (void) pthread_mutex_destroy(&queue->mutex);
}

static void
ngx_media_srt_sender_wake(ngx_media_srt_sender_t *sender)
{
    (void) pthread_mutex_lock(&sender->wake_mutex);
    sender->wake_generation++;
    (void) pthread_cond_signal(&sender->wake_cond);
    (void) pthread_mutex_unlock(&sender->wake_mutex);
}

static ngx_uint_t
ngx_media_srt_outputs_active_senders(ngx_media_srt_outputs_t *outs)
{
    return (outs != NULL)
           ? (ngx_uint_t) ngx_atomic_fetch_add(&outs->active_senders, 0) : 0;
}

static void
ngx_media_srt_out_wake_lane(ngx_media_srt_outputs_t *outs, ngx_uint_t lane)
{
    ngx_uint_t  active;

    active = ngx_media_srt_outputs_active_senders(outs);
    if (active == 0) {
        active = 1;
    }

    ngx_media_srt_sender_wake(&outs->shards[lane % active]);
}

static ngx_msec_t
ngx_media_srt_now(void)
{
    struct timespec  ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);

    return (ngx_msec_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
ngx_media_srt_out_egress_report(ngx_media_srt_output_t *dest,
    ngx_msec_t now)
{
    ngx_media_egress_report_t   report;
    const ngx_media_srt_unit_t *unit;
    uint64_t                    cursor, sequence;

    if (dest->conf.egress_token == 0 || now - dest->egress_at < 1000) {
        return;
    }
    ngx_memzero(&report, sizeof(report));
    report.delivered_bytes = dest->sent_bytes - dest->reported_bytes;
    report.dropped_units = dest->queue.dropped - dest->reported_dropped;
    report.transport_errors = dest->transport_errors - dest->reported_errors;
    report.backpressure_events =
        dest->blocked_sends - dest->reported_blocked;
    report.reconnects = dest->reconnects - dest->reported_reconnects;
    report.placement = dest->shard;
    cursor = dest->cursor;
    unit = ngx_media_srt_queue_next(&dest->queue, &cursor);
    if (unit != NULL) {
        if (now >= unit->enqueue_msec) {
            report.queue_lag_msec = now - unit->enqueue_msec;
        }

        for (sequence = unit->sequence; sequence < dest->queue.head;
             sequence++)
        {
            unit = &dest->queue.units[
                       sequence % dest->queue.capacity];
            if (unit->burst == NULL || unit->sequence != sequence) {
                continue;
            }

            if (dest->inflight_burst != NULL
                && sequence == dest->inflight_sequence)
            {
                report.queue_bytes +=
                    (unit->len > dest->inflight_offset)
                        ? unit->len - dest->inflight_offset : 0;
            } else {
                report.queue_bytes += unit->len;
            }
        }
    }

    dest->reported_bytes = dest->sent_bytes;
    dest->reported_dropped = dest->queue.dropped;
    dest->reported_errors = dest->transport_errors;
    dest->reported_blocked = dest->blocked_sends;
    dest->reported_reconnects = dest->reconnects;
    dest->egress_at = now;
    ngx_media_egress_manager_report(dest->conf.egress_token, &report);
}

static ngx_uint_t
ngx_media_srt_out_slot_matches(const ngx_media_srt_output_t *dest,
    const ngx_str_t *application, const ngx_str_t *stream)
{
    return dest->conf.application.len == application->len
           && dest->conf.stream.len == stream->len
           && ngx_memcmp(dest->conf.application.data, application->data,
                         application->len) == 0
           && ngx_memcmp(dest->conf.stream.data, stream->data,
                         stream->len) == 0;
}

static ngx_uint_t
ngx_media_srt_out_service(ngx_media_srt_outputs_t *outs,
    ngx_media_srt_output_t *dest, ngx_uint_t index, ngx_msec_t now)
{
    ngx_media_srt_output_conf_t  connect_conf;
    ngx_media_srt_session_t     *session;
    ngx_media_srt_stats_t        transport_stats;
    ngx_media_buf_t             *burst;
    const ngx_media_srt_unit_t  *unit;
    uint64_t                     epoch;
    size_t                       take, offset, remaining;
    ngx_int_t                    rc;

    (void) pthread_mutex_lock(&dest->mutex);

    if (!dest->used || !dest->running || dest->sending || dest->connecting) {
        (void) pthread_mutex_unlock(&dest->mutex);
        return 0;
    }

    ngx_media_srt_out_egress_report(dest, now);


    if (dest->session == NULL) {
        if (dest->retry_at > now) {
            (void) pthread_mutex_unlock(&dest->mutex);
            return 0;
        }

        dest->connecting = 1;
        connect_conf = dest->conf;
        epoch = dest->epoch;
        (void) pthread_mutex_unlock(&dest->mutex);

        /*
         * The destination's logical lane is its multiplexer group: a lane's
         * destinations share one transport endpoint and one pair of library
         * threads, so the transport's thread count follows the lanes and not
         * the fanout.  Lanes are stable for a destination's lifetime, which
         * a shared endpoint needs - a session cannot move between them.
         */
        session = ngx_media_srt_connect_shared(
            connect_conf.host.data, connect_conf.port,
            connect_conf.streamid.len ? connect_conf.streamid.data : NULL,
            connect_conf.streamid.len, connect_conf.connect_timeout,
            connect_conf.params, dest->shard + 1, outs->log);

        (void) pthread_mutex_lock(&dest->mutex);

        if (dest->epoch != epoch || !dest->used || !dest->running) {
            dest->connecting = 0;
            if (session != NULL) {
                ngx_media_srt_session_close(session);
            }
            (void) pthread_mutex_unlock(&dest->mutex);
            return 1;
        }

        dest->connecting = 0;

        if (session == NULL) {
            dest->reconnects++;
            dest->transport_errors++;
            dest->retry_at = now + NGX_MEDIA_SRT_DEST_RETRY;
            (void) pthread_mutex_unlock(&dest->mutex);
            ngx_media_srt_out_report(outs, index,
                                     NGX_MEDIA_SRT_OUT_EVENT_FAILED);
            return 1;
        }

        dest->session = session;
        dest->reconnects++;
        dest->retry_at = 0;
        dest->cursor = 0;
        dest->stats_at = 0;
        ngx_media_srt_queue_resync(&dest->queue);
        (void) pthread_mutex_unlock(&dest->mutex);

        ngx_media_srt_out_report(outs, index,
                                 NGX_MEDIA_SRT_OUT_EVENT_CONNECTED);
        return 1;
    }

    if (now - dest->stats_at >= 1000) {
        ngx_memzero(&transport_stats, sizeof(transport_stats));
        ngx_media_srt_session_stats(dest->session, &transport_stats);
        dest->retransmitted_packets =
            (transport_stats.packets_retransmitted > 0)
                ? (uint64_t) transport_stats.packets_retransmitted : 0;
        dest->stats_at = now;
    }

    if (dest->inflight_burst == NULL) {
        unit = ngx_media_srt_queue_next(&dest->queue, &dest->cursor);
        if (unit == NULL) {
            (void) pthread_mutex_unlock(&dest->mutex);
            return 0;
        }

        dest->inflight_burst = ngx_media_buf_ref(unit->burst);
        dest->inflight_sequence = unit->sequence;
        dest->inflight_len = unit->len;
        dest->inflight_offset = 0;
    }

    offset = dest->inflight_offset;
    remaining = dest->inflight_len - offset;
    take = (remaining > NGX_MEDIA_SRT_SEND_CHUNK)
               ? NGX_MEDIA_SRT_SEND_CHUNK : remaining;
    burst = dest->inflight_burst;
    session = dest->session;
    epoch = dest->epoch;
    dest->sending = 1;

    (void) pthread_mutex_unlock(&dest->mutex);

    rc = ngx_media_srt_session_send(session,
                                    ngx_media_buf_data(burst) + offset,
                                    take, 0);

    (void) pthread_mutex_lock(&dest->mutex);
    dest->sending = 0;

    if (dest->epoch != epoch) {
        if (dest->session == session) {
            ngx_media_srt_session_close(dest->session);
            dest->session = NULL;
        }
        if (dest->inflight_burst != NULL) {
            ngx_media_buf_unref(dest->inflight_burst);
            dest->inflight_burst = NULL;
        }
        dest->inflight_len = 0;
        dest->inflight_offset = 0;
        (void) pthread_mutex_unlock(&dest->mutex);
        return 1;
    }

    if (rc == 0) {
        dest->blocked_sends++;
        (void) pthread_mutex_unlock(&dest->mutex);
        return 0;
    }

    if (rc < 0 || (size_t) rc > take) {
        dest->transport_errors++;
        if (dest->session != NULL) {
            ngx_media_srt_session_close(dest->session);
            dest->session = NULL;
        }
        ngx_media_srt_queue_resync(&dest->queue);
        if (dest->inflight_burst != NULL) {
            ngx_media_buf_unref(dest->inflight_burst);
            dest->inflight_burst = NULL;
        }
        dest->inflight_len = 0;
        dest->inflight_offset = 0;
        (void) pthread_mutex_unlock(&dest->mutex);
        ngx_media_srt_out_report(outs, index, NGX_MEDIA_SRT_OUT_EVENT_FAILED);
        return 1;
    }

    dest->sent_bytes += (uint64_t) rc;
    dest->inflight_offset += (size_t) rc;

    if (dest->inflight_offset == dest->inflight_len) {
        ngx_media_srt_queue_advance(&dest->queue, &dest->cursor,
                                    dest->inflight_sequence);
        dest->sent_bursts++;
        ngx_media_buf_unref(dest->inflight_burst);
        dest->inflight_burst = NULL;
        dest->inflight_len = 0;
        dest->inflight_offset = 0;
    }

    (void) pthread_mutex_unlock(&dest->mutex);
    return 1;
}

static void
ngx_media_srt_out_dispatch(ngx_media_srt_sender_t *sender,
    ngx_media_srt_feed_unit_t *unit)
{
    ngx_media_srt_outputs_t  *outs = sender->outs;
    ngx_media_srt_output_t   *dest;
    ngx_uint_t                i;

    (void) pthread_rwlock_rdlock(&outs->destinations_lock);

    for (i = sender->id; i < NGX_MEDIA_SRT_MAX_OUTPUTS;
         i += NGX_MEDIA_SRT_EGRESS_SHARDS)
    {
        dest = &outs->destinations[i];
        (void) pthread_mutex_lock(&dest->mutex);

        if (dest->used && dest->running && dest->route == unit->route) {
            (void) ngx_media_srt_queue_push(&dest->queue, unit->burst,
                                             unit->len, unit->keyframe,
                                             ngx_media_srt_now());
        }

        (void) pthread_mutex_unlock(&dest->mutex);
    }

    (void) pthread_rwlock_unlock(&outs->destinations_lock);
}

static void *
ngx_media_srt_out_thread(void *data)
{
    ngx_media_srt_sender_t      *sender = data;
    ngx_media_srt_outputs_t     *outs = sender->outs;
    ngx_media_srt_sender_t      *lane_sender;
    ngx_media_srt_feed_unit_t    unit;
    ngx_media_srt_output_t      *dest;
    ngx_msec_t                   now;
    uint64_t                     observed;
    ngx_uint_t                   active, lane, i, worked;
    char                         name[16];

    (void) snprintf(name, sizeof(name), "srt-egress-%02lu",
                    (unsigned long) sender->id);
    (void) prctl(PR_SET_NAME, name, 0, 0, 0);

    for ( ;; ) {
        (void) pthread_mutex_lock(&sender->wake_mutex);
        observed = sender->wake_generation;
        (void) pthread_mutex_unlock(&sender->wake_mutex);

        if (ngx_atomic_fetch_add(&outs->stopping, 0) != 0) {
            return NULL;
        }

        worked = 0;
        active = ngx_media_srt_outputs_active_senders(outs);

        if (sender->id < active) {
            for (lane = sender->id; lane < NGX_MEDIA_SRT_EGRESS_SHARDS;
                 lane += active)
            {
                lane_sender = &outs->shards[lane];

                if (pthread_mutex_trylock(&lane_sender->lane_mutex) != 0) {
                    continue;
                }

                active = ngx_media_srt_outputs_active_senders(outs);
                if (active == 0 || lane % active != sender->id) {
                    (void) pthread_mutex_unlock(&lane_sender->lane_mutex);
                    continue;
                }

                while (ngx_media_srt_feed_pop(lane_sender, &unit)) {
                    ngx_media_srt_out_dispatch(lane_sender, &unit);
                    ngx_media_buf_unref(unit.burst);
                    worked = 1;
                }

                now = ngx_media_srt_now();

                for (i = lane; i < NGX_MEDIA_SRT_MAX_OUTPUTS;
                     i += NGX_MEDIA_SRT_EGRESS_SHARDS)
                {
                    dest = &outs->destinations[i];
                    if (ngx_media_srt_out_service(outs, dest, i, now)) {
                        worked = 1;
                    }
                }

                (void) pthread_mutex_unlock(&lane_sender->lane_mutex);
            }
        }

        if (worked) {
            continue;
        }

        (void) pthread_mutex_lock(&sender->wake_mutex);
        if (sender->wake_generation == observed
            && ngx_atomic_fetch_add(&outs->stopping, 0) == 0)
        {
            struct timespec  ts;

            (void) clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }

            (void) pthread_cond_timedwait(&sender->wake_cond,
                                          &sender->wake_mutex, &ts);
        }
        (void) pthread_mutex_unlock(&sender->wake_mutex);
    }
}

ngx_int_t
ngx_media_srt_outputs_start(ngx_media_srt_outputs_t **out,
    const ngx_media_srt_output_conf_t *confs, ngx_uint_t count,
    ngx_uint_t max_events, ngx_log_t *log)
{
    ngx_media_srt_outputs_t  *outs;
    ngx_media_srt_sender_t   *sender;
    ngx_media_srt_output_t   *dest;
    ngx_uint_t                i;

    if (out == NULL || (count != 0 && confs == NULL)
        || count > NGX_MEDIA_SRT_MAX_OUTPUTS)
    {
        return NGX_ERROR;
    }

    *out = NULL;
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

    if (outs->events_capacity > SIZE_MAX / sizeof(ngx_media_srt_out_event_t)) {
        ngx_free(outs);
        return NGX_ERROR;
    }

    outs->events = ngx_alloc(outs->events_capacity
                             * sizeof(ngx_media_srt_out_event_t), log);
    outs->destinations = ngx_alloc(NGX_MEDIA_SRT_MAX_OUTPUTS
                                   * sizeof(ngx_media_srt_output_t), log);

    if (outs->events == NULL || outs->destinations == NULL) {
        if (outs->events != NULL) {
            ngx_free(outs->events);
        }
        if (outs->destinations != NULL) {
            ngx_free(outs->destinations);
        }
        ngx_free(outs);
        return NGX_ERROR;
    }

    ngx_memzero(outs->events,
                outs->events_capacity * sizeof(ngx_media_srt_out_event_t));
    ngx_memzero(outs->destinations,
                NGX_MEDIA_SRT_MAX_OUTPUTS * sizeof(ngx_media_srt_output_t));

    (void) pthread_mutex_init(&outs->events_mutex, NULL);
    (void) pthread_rwlock_init(&outs->destinations_lock, NULL);
    outs->active_senders = ngx_media_egress_manager_fixed_workers(
                               NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD);
    if (outs->active_senders == 0) {
        outs->active_senders = 1;
    }

    for (i = 0; i < NGX_MEDIA_SRT_EGRESS_SHARDS; i++) {
        sender = &outs->shards[i];
        sender->outs = outs;
        sender->id = i;
        (void) pthread_mutex_init(&sender->feed.mutex, NULL);
        (void) pthread_mutex_init(&sender->wake_mutex, NULL);
        (void) pthread_mutex_init(&sender->lane_mutex, NULL);
        (void) pthread_cond_init(&sender->wake_cond, NULL);
    }

    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        dest = &outs->destinations[i];
        dest->shard = i % NGX_MEDIA_SRT_EGRESS_SHARDS;
        (void) pthread_mutex_init(&dest->mutex, NULL);

        if (i < count) {
            dest->conf = confs[i];
            dest->running = 1;
            dest->used = 1;
            ngx_media_srt_queue_init(&dest->queue, dest->conf.max_units,
                                     dest->conf.max_bytes);
            if (dest->conf.program_identity == 0) {
                outs->static_slots[outs->static_count++] = i;
            }
        }
    }

    outs->notify_fd = eventfd(0, EFD_NONBLOCK);
    if (outs->notify_fd < 0) {
        goto failed;
    }

    for (i = 0; i < NGX_MEDIA_SRT_EGRESS_SHARDS; i++) {
        sender = &outs->shards[i];
        if (pthread_create(&sender->thread, NULL, ngx_media_srt_out_thread,
                           sender) != 0)
        {
            goto failed;
        }
        outs->nthreads++;
    }
    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 0,
        ngx_media_srt_outputs_active_senders(outs));

    *out = outs;
    return NGX_OK;

failed:
    ngx_media_srt_outputs_stop(outs);
    return NGX_ERROR;
}

ngx_int_t
ngx_media_srt_outputs_set_concurrency(ngx_media_srt_outputs_t *outs,
    ngx_uint_t active_senders)
{
    ngx_uint_t  current, i;

    if (outs == NULL || active_senders == 0
        || active_senders > NGX_MEDIA_SRT_EGRESS_SHARDS
        || ngx_atomic_fetch_add(&outs->stopping, 0) != 0)
    {
        return NGX_ERROR;
    }

    for ( ;; ) {
        current = ngx_media_srt_outputs_active_senders(outs);
        if (current == active_senders
            || ngx_atomic_cmp_set(&outs->active_senders, current,
                                  active_senders))
        {
            break;
        }
    }

    for (i = 0; i < NGX_MEDIA_SRT_EGRESS_SHARDS; i++) {
        ngx_media_srt_sender_wake(&outs->shards[i]);
    }

    return NGX_OK;
}

ngx_uint_t
ngx_media_srt_outputs_concurrency(ngx_media_srt_outputs_t *outs)
{
    return ngx_media_srt_outputs_active_senders(outs);
}

void
ngx_media_srt_outputs_adapt(ngx_media_srt_outputs_t *outs)
{
    ngx_media_srt_sender_t  *sender;
    struct timespec          cpu_time;
    clockid_t                cpu_clock;
    uint64_t                 wall_ns, wall_delta, cpu_ns, cpu_delta, busy;
    ngx_uint_t               active, peak_busy, i, desired, fixed;

    if (outs == NULL || ngx_atomic_fetch_add(&outs->stopping, 0) != 0) {
        return;
    }

    wall_ns = ngx_media_srt_now_ns();
    if (wall_ns == 0
        || (outs->last_adapt_ns != 0
            && wall_ns - outs->last_adapt_ns < UINT64_C(1000000000)))
    {
        return;
    }

    wall_delta = (outs->last_adapt_ns != 0)
                     ? wall_ns - outs->last_adapt_ns : 0;
    active = ngx_media_srt_outputs_active_senders(outs);
    peak_busy = 0;

    for (i = 0; i < NGX_MEDIA_SRT_EGRESS_SHARDS; i++) {
        sender = &outs->shards[i];

        if (pthread_getcpuclockid(sender->thread, &cpu_clock) != 0
            || clock_gettime(cpu_clock, &cpu_time) != 0)
        {
            sender->last_cpu_ns = 0;
            continue;
        }

        cpu_ns = (uint64_t) cpu_time.tv_sec * UINT64_C(1000000000)
                 + (uint64_t) cpu_time.tv_nsec;

        if (i < active && outs->cpu_sampled && sender->last_cpu_ns != 0
            && cpu_ns >= sender->last_cpu_ns && wall_delta != 0)
        {
            cpu_delta = cpu_ns - sender->last_cpu_ns;
            busy = cpu_delta * 1000 / wall_delta;
            if (busy > peak_busy) {
                peak_busy = (busy > 1000) ? 1000 : (ngx_uint_t) busy;
            }
        }

        sender->last_cpu_ns = cpu_ns;
    }

    outs->last_adapt_ns = wall_ns;
    if (!outs->cpu_sampled) {
        outs->cpu_sampled = 1;
        return;
    }

    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, peak_busy, active);
    fixed = ngx_media_egress_manager_fixed_workers(
                NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD);
    desired = (fixed != 0)
                  ? fixed
                  : ngx_media_egress_manager_recommend_workers(
                        NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, active, 1,
                        NGX_MEDIA_SRT_EGRESS_SHARDS);

    if (desired != active
        && ngx_media_srt_outputs_set_concurrency(outs, desired) == NGX_OK)
    {
        ngx_media_egress_manager_engine_load(
            NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, peak_busy, desired);
    }
}

static uint64_t
ngx_media_srt_now_ns(void)
{
    struct timespec  ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t) ts.tv_sec * UINT64_C(1000000000)
           + (uint64_t) ts.tv_nsec;
}
ngx_int_t
ngx_media_srt_outputs_add(ngx_media_srt_outputs_t *outs,
    const ngx_media_srt_output_conf_t *conf, ngx_uint_t *index, ngx_log_t *log)
{
    ngx_media_srt_program_route_t  *route = NULL;
    ngx_media_srt_output_t         *dest = NULL;
    ngx_uint_t                      i;

    if (outs == NULL || conf == NULL || conf->host.data == NULL
        || conf->port == 0 || conf->application.data == NULL
        || conf->stream.data == NULL
        || (conf->program_identity != 0 && conf->incarnation == 0)
        || ngx_atomic_fetch_add(&outs->stopping, 0) != 0)
    {
        return NGX_ERROR;
    }

    (void) pthread_rwlock_wrlock(&outs->destinations_lock);

    if (outs->count >= NGX_MEDIA_SRT_MAX_OUTPUTS) {
        (void) pthread_rwlock_unlock(&outs->destinations_lock);
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: no free srt destination slot");
        return NGX_ERROR;
    }

    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        dest = &outs->destinations[i];
        (void) pthread_mutex_lock(&dest->mutex);
        if (!dest->used && !dest->sending && !dest->connecting
            && dest->session == NULL && dest->inflight_burst == NULL)
        {
            break;
        }
        (void) pthread_mutex_unlock(&dest->mutex);
    }

    if (i == NGX_MEDIA_SRT_MAX_OUTPUTS) {
        (void) pthread_rwlock_unlock(&outs->destinations_lock);
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: no reusable srt destination slot");
        return NGX_ERROR;
    }

    if (conf->program_identity != 0) {
        route = ngx_media_srt_route_find(outs, conf->program_identity,
                                         conf->incarnation);
        if (route == NULL) {
            route = ngx_media_srt_route_create(outs, conf->program_identity,
                                               conf->incarnation);
        }
        if (route == NULL) {
            (void) pthread_mutex_unlock(&dest->mutex);
            (void) pthread_rwlock_unlock(&outs->destinations_lock);
            ngx_log_error(NGX_LOG_WARN, log, 0,
                          "media: no free srt program route");
            return NGX_ERROR;
        }
    }

    ngx_media_srt_queue_destroy(&dest->queue);
    ngx_media_srt_queue_init(&dest->queue, conf->max_units, conf->max_bytes);
    dest->conf = *conf;
    dest->route = (route != NULL) ? route->token : 0;
    dest->running = 1;
    dest->used = 1;
    dest->cursor = 0;
    dest->inflight_sequence = 0;
    dest->inflight_len = 0;
    dest->inflight_offset = 0;
    dest->sent_bytes = 0;
    dest->sent_bursts = 0;
    dest->blocked_sends = 0;
    dest->retransmitted_packets = 0;
    dest->reconnects = 0;
    dest->transport_errors = 0;
    dest->reported_bytes = 0;
    dest->reported_dropped = 0;
    dest->reported_errors = 0;
    dest->reported_blocked = 0;
    dest->reported_reconnects = 0;
    dest->egress_at = 0;
    dest->retry_at = 0;
    dest->stats_at = 0;
    dest->connecting = 0;
    dest->epoch++;
    if (route != NULL) {
        ngx_media_srt_route_add_destination(outs, dest);
    }
    if (conf->program_identity == 0) {
        outs->static_slots[outs->static_count++] = i;
    }

    (void) pthread_mutex_unlock(&dest->mutex);
    outs->count++;
    (void) pthread_rwlock_unlock(&outs->destinations_lock);

    ngx_media_srt_out_wake_lane(outs, dest->shard);

    if (index != NULL) {
        *index = i;
    }

    return NGX_OK;
}
void
ngx_media_srt_outputs_remove(ngx_media_srt_outputs_t *outs, ngx_uint_t index)
{
    ngx_media_srt_output_t  *dest;
    ngx_uint_t              j;

    if (outs == NULL || index >= NGX_MEDIA_SRT_MAX_OUTPUTS) {
        return;
    }

    (void) pthread_rwlock_wrlock(&outs->destinations_lock);
    dest = &outs->destinations[index];
    (void) pthread_mutex_lock(&dest->mutex);

    if (!dest->used) {
        (void) pthread_mutex_unlock(&dest->mutex);
        (void) pthread_rwlock_unlock(&outs->destinations_lock);
        return;
    }

    dest->running = 0;
    dest->used = 0;
    dest->epoch++;
    if (dest->conf.program_identity == 0) {
        for (j = 0; j < outs->static_count; j++) {
            if (outs->static_slots[j] == index) {
                outs->static_slots[j] =
                    outs->static_slots[--outs->static_count];
                break;
            }
        }
    }
    ngx_media_srt_route_remove_destination(outs, dest);

    if (dest->session != NULL && !dest->sending) {
        ngx_media_srt_session_close(dest->session);
        dest->session = NULL;
    }

    if (!dest->sending && dest->inflight_burst != NULL) {
        ngx_media_buf_unref(dest->inflight_burst);
        dest->inflight_burst = NULL;
        dest->inflight_len = 0;
        dest->inflight_offset = 0;
    }

    ngx_media_srt_queue_destroy(&dest->queue);
    if (outs->count > 0) {
        outs->count--;
    }

    (void) pthread_mutex_unlock(&dest->mutex);
    (void) pthread_rwlock_unlock(&outs->destinations_lock);

    ngx_media_srt_out_wake_lane(outs, dest->shard);
}
void
ngx_media_srt_outputs_stop(ngx_media_srt_outputs_t *outs)
{
    ngx_media_srt_output_t  *dest;
    ngx_uint_t               i;

    if (outs == NULL) {
        return;
    }

    (void) ngx_atomic_fetch_add(&outs->stopping, 1);

    for (i = 0; i < NGX_MEDIA_SRT_EGRESS_SHARDS; i++) {
        ngx_media_srt_sender_wake(&outs->shards[i]);
    }

    (void) pthread_rwlock_wrlock(&outs->destinations_lock);
    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        dest = &outs->destinations[i];
        (void) pthread_mutex_lock(&dest->mutex);
        dest->running = 0;
        dest->used = 0;
        dest->epoch++;
        if (dest->session != NULL) {
            ngx_media_srt_session_shutdown(dest->session);
        }
        (void) pthread_mutex_unlock(&dest->mutex);
    }
    (void) pthread_rwlock_unlock(&outs->destinations_lock);

    for (i = 0; i < NGX_MEDIA_SRT_EGRESS_SHARDS; i++) {
        ngx_media_srt_sender_wake(&outs->shards[i]);
    }
    for (i = 0; i < outs->nthreads; i++) {
        (void) pthread_join(outs->shards[i].thread, NULL);
    }
    outs->nthreads = 0;
    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 0, 0);
    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        dest = &outs->destinations[i];
        if (dest->conf.program_identity == 0
            && dest->conf.egress_token != 0)
        {
            ngx_media_egress_manager_release(dest->conf.egress_token);
            dest->conf.egress_token = 0;
        }
    }


    for (i = 0; i < NGX_MEDIA_SRT_MAX_OUTPUTS; i++) {
        dest = &outs->destinations[i];
        if (dest->session != NULL) {
            ngx_media_srt_session_close(dest->session);
            dest->session = NULL;
        }
        if (dest->inflight_burst != NULL) {
            ngx_media_buf_unref(dest->inflight_burst);
            dest->inflight_burst = NULL;
        }
        ngx_media_srt_queue_destroy(&dest->queue);
        (void) pthread_mutex_destroy(&dest->mutex);
    }

    for (i = 0; i < NGX_MEDIA_SRT_EGRESS_SHARDS; i++) {
        ngx_media_srt_feed_destroy(&outs->shards[i].feed);
        (void) pthread_cond_destroy(&outs->shards[i].wake_cond);
        (void) pthread_mutex_destroy(&outs->shards[i].wake_mutex);
        (void) pthread_mutex_destroy(&outs->shards[i].lane_mutex);
    }

    (void) pthread_rwlock_destroy(&outs->destinations_lock);

    if (outs->notify_fd >= 0) {
        (void) close(outs->notify_fd);
        outs->notify_fd = -1;
    }
    ngx_free(outs->destinations);
    ngx_free(outs->events);
    (void) pthread_mutex_destroy(&outs->events_mutex);
    ngx_free(outs);
}


static ngx_int_t
ngx_media_srt_route_publish(ngx_media_srt_outputs_t *outs,
    ngx_media_srt_program_route_t *route, ngx_media_buf_t *burst, size_t len,
    ngx_uint_t keyframe)
{
    ngx_uint_t  i, matched = 0;

    for (i = 0; i < NGX_MEDIA_SRT_EGRESS_SHARDS; i++) {
        if (route->per_shard[i] == 0) {
            continue;
        }

        (void) ngx_media_srt_feed_push(&outs->shards[i], route->token, burst,
                                        len, keyframe);
        ngx_media_srt_out_wake_lane(outs, i);
        matched++;
    }

    return matched ? NGX_OK : NGX_DECLINED;
}

ngx_int_t
ngx_media_srt_outputs_push(ngx_media_srt_outputs_t *outs,
    const ngx_str_t *application, const ngx_str_t *stream,
    uintptr_t program_identity, uint64_t incarnation, ngx_media_buf_t *burst,
    size_t len, ngx_uint_t keyframe)
{
    ngx_media_srt_program_route_t  *route;
    ngx_media_srt_output_t         *dest;
    ngx_uint_t                      i, slot, matched, deferred;
    ngx_int_t                       rc;

    if (outs == NULL || application == NULL || stream == NULL
        || program_identity == 0 || incarnation == 0 || burst == NULL
        || len == 0)
    {
        return NGX_ERROR;
    }

    if (ngx_atomic_fetch_add(&outs->stopping, 0) != 0) {
        return NGX_ERROR;
    }

    (void) pthread_rwlock_rdlock(&outs->destinations_lock);
    route = ngx_media_srt_route_find(outs, program_identity, incarnation);
    if (route != NULL && route->static_bound) {
        rc = ngx_media_srt_route_publish(outs, route, burst, len, keyframe);
        (void) pthread_rwlock_unlock(&outs->destinations_lock);
        return rc;
    }

    if (route == NULL && outs->static_count == 0) {
        (void) pthread_rwlock_unlock(&outs->destinations_lock);
        return NGX_DECLINED;
    }
    (void) pthread_rwlock_unlock(&outs->destinations_lock);

    (void) pthread_rwlock_wrlock(&outs->destinations_lock);
    if (ngx_atomic_fetch_add(&outs->stopping, 0) != 0) {
        (void) pthread_rwlock_unlock(&outs->destinations_lock);
        return NGX_ERROR;
    }

    route = ngx_media_srt_route_find(outs, program_identity, incarnation);
    if (route == NULL) {
        matched = 0;

        for (i = 0; i < outs->static_count; i++) {
            slot = outs->static_slots[i];
            dest = &outs->destinations[slot];
            if (dest->used && dest->conf.program_identity == 0
                && dest->conf.incarnation == 0
                && ngx_media_srt_out_slot_matches(dest, application, stream))
            {
                matched = 1;
                break;
            }
        }

        if (!matched) {
            (void) pthread_rwlock_unlock(&outs->destinations_lock);
            return NGX_DECLINED;
        }

        route = ngx_media_srt_route_create(outs, program_identity,
                                           incarnation);
        if (route == NULL) {
            (void) pthread_rwlock_unlock(&outs->destinations_lock);
            return NGX_ERROR;
        }
    }

    if (!route->static_bound) {
        deferred = 0;

        for (i = 0; i < outs->static_count; i++) {
            slot = outs->static_slots[i];
            dest = &outs->destinations[slot];

            if (!dest->used || dest->conf.program_identity != 0
                || dest->conf.incarnation != 0
                || !ngx_media_srt_out_slot_matches(dest, application, stream))
            {
                continue;
            }

            (void) pthread_mutex_lock(&dest->mutex);
            if (!dest->used) {
                (void) pthread_mutex_unlock(&dest->mutex);
                continue;
            }

            if (dest->route != route->token) {
                if (dest->sending) {
                    deferred = 1;
                    (void) pthread_mutex_unlock(&dest->mutex);
                    continue;
                }

                ngx_media_srt_route_remove_destination(outs, dest);
                if (dest->inflight_burst != NULL) {
                    ngx_media_buf_unref(dest->inflight_burst);
                    dest->inflight_burst = NULL;
                    dest->inflight_len = 0;
                    dest->inflight_offset = 0;
                }
                ngx_media_srt_queue_destroy(&dest->queue);
                ngx_media_srt_queue_init(&dest->queue, dest->conf.max_units,
                                         dest->conf.max_bytes);
                dest->cursor = 0;
                dest->route = route->token;
                ngx_media_srt_route_add_destination(outs, dest);
            }

            (void) pthread_mutex_unlock(&dest->mutex);
        }

        if (!deferred) {
            route->static_bound = 1;
        }
    }

    rc = ngx_media_srt_route_publish(outs, route, burst, len, keyframe);
    (void) pthread_rwlock_unlock(&outs->destinations_lock);
    return rc;
}

ngx_uint_t
ngx_media_srt_outputs_stats_get(ngx_media_srt_outputs_t *outs,
    ngx_media_srt_egress_stats_t *stats, ngx_uint_t max)
{
    ngx_media_srt_egress_stats_t  *row;
    ngx_media_srt_output_t         *dest;
    ngx_uint_t                      count, i, slot;

    if (outs == NULL || stats == NULL || max == 0) {
        return 0;
    }

    count = (max < NGX_MEDIA_SRT_EGRESS_SHARDS)
                ? max : NGX_MEDIA_SRT_EGRESS_SHARDS;
    ngx_memzero(stats, count * sizeof(ngx_media_srt_egress_stats_t));

    (void) pthread_rwlock_rdlock(&outs->destinations_lock);
    for (i = 0; i < count; i++) {
        ngx_media_srt_sender_t  *sender = &outs->shards[i];

        row = &stats[i];
        row->shard = i;

        (void) pthread_mutex_lock(&sender->feed.mutex);
        row->feed_queue_units =
            (ngx_uint_t) (sender->feed.head - sender->feed.tail);
        row->feed_queue_bytes = sender->feed.bytes;
        row->feed_queue_dropped = sender->feed.dropped;
        (void) pthread_mutex_unlock(&sender->feed.mutex);

        for (slot = i; slot < NGX_MEDIA_SRT_MAX_OUTPUTS;
             slot += NGX_MEDIA_SRT_EGRESS_SHARDS)
        {
            dest = &outs->destinations[slot];
            (void) pthread_mutex_lock(&dest->mutex);

            if (dest->used) {
                row->destinations++;
                row->output_queue_units +=
                    (ngx_uint_t) (dest->queue.head - dest->queue.tail);
                row->output_queue_bytes += dest->queue.bytes;
                row->output_queue_dropped += dest->queue.dropped;
                row->sent_bytes += dest->sent_bytes;
                row->sent_bursts += dest->sent_bursts;
                row->blocked_sends += dest->blocked_sends;
                row->retransmitted_packets += dest->retransmitted_packets;
            }

            (void) pthread_mutex_unlock(&dest->mutex);
        }
    }
    (void) pthread_rwlock_unlock(&outs->destinations_lock);

    return count;
}

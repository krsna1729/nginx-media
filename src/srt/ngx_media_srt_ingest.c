#include "ngx_media_srt_ingest.h"

#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

/*
 * A readiness byte lands on the eventfd no more than once per millisecond;
 * the worker re-reads both queues until they are empty, so coalescing wakes
 * is safe (goal doc 15: clear and re-check).
 */
#define NGX_MEDIA_SRT_NOTIFY_INTERVAL 1

typedef struct {
    ngx_media_srt_ingest_t  *ingest;
    ngx_msec_t               last_notify;
} ngx_media_srt_notify_t;

static ngx_msec_t ngx_media_srt_now(void);
static void ngx_media_srt_notify(ngx_media_srt_notify_t *notify);
static ngx_uint_t ngx_media_srt_event_push(ngx_media_srt_ingest_t *ingest,
    const ngx_media_srt_event_t *event);
static void *ngx_media_srt_thread(void *data);

static ngx_msec_t
ngx_media_srt_now(void)
{
    struct timespec  ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (ngx_msec_t) ((uint64_t) ts.tv_sec * 1000
                         + (uint64_t) ts.tv_nsec / 1000000);
}

static void
ngx_media_srt_notify(ngx_media_srt_notify_t *notify)
{
    ngx_msec_t  now;
    uint64_t    one = 1;
    ssize_t     n;

    now = ngx_media_srt_now();

    if (now - notify->last_notify < NGX_MEDIA_SRT_NOTIFY_INTERVAL) {
        return;
    }

    notify->last_notify = now;

    n = write(notify->ingest->notify_fd, &one, sizeof(one));
    (void) n;
}

static ngx_uint_t
ngx_media_srt_event_push(ngx_media_srt_ingest_t *ingest,
    const ngx_media_srt_event_t *event)
{
    ngx_media_srt_event_t  *slot;

    while (!ngx_atomic_cmp_set(&ingest->events_lock, 0, 1)) {
        /* spin: the critical section is a single descriptor copy */
    }

    if (ingest->events_head - ingest->events_tail >= ingest->events_capacity) {
        (void) ngx_atomic_fetch_add(&ingest->events_dropped, 1);
        (void) ngx_atomic_cmp_set(&ingest->events_lock, 1, 0);
        return NGX_AGAIN;
    }

    slot = &ingest->events[ingest->events_head
                           & (ingest->events_capacity - 1)];
    *slot = *event;

    ingest->events_head++;

    (void) ngx_atomic_cmp_set(&ingest->events_lock, 1, 0);

    return NGX_OK;
}

static ngx_media_srt_session_state_t *
ngx_media_srt_session_state_alloc(ngx_media_srt_ingest_t *ingest)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
        if (ingest->sessions[i].session == NULL) {
            return &ingest->sessions[i];
        }
    }

    return NULL;
}

static ngx_media_srt_session_state_t *
ngx_media_srt_session_state_find(ngx_media_srt_ingest_t *ingest,
    ngx_media_srt_session_t *session)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
        if (ingest->sessions[i].session == session) {
            return &ingest->sessions[i];
        }
    }

    return NULL;
}

static void
ngx_media_srt_session_state_release(ngx_media_srt_session_state_t *state)
{
    state->session = NULL;
    state->id = 0;
    state->bytes = 0;
    state->chunks = 0;
}

static void
ngx_media_srt_session_open(ngx_media_srt_ingest_t *ingest,
    ngx_media_srt_session_t *session, ngx_media_srt_notify_t *notify)
{
    ngx_media_srt_event_t         event;
    ngx_media_srt_session_state_t *state;
    ngx_int_t                     len;

    state = ngx_media_srt_session_state_alloc(ingest);

    if (state == NULL) {
        /* hard session ceiling: refuse rather than degrade the scheduler */
        (void) ngx_atomic_fetch_add(&ingest->sessions_dropped, 1);
        ngx_media_srt_session_close(session);
        return;
    }

    ingest->sessions_opened++;
    state->session = session;
    state->id = ingest->sessions_opened;
    state->bytes = 0;
    state->chunks = 0;

    ngx_memzero(&event, sizeof(event));
    event.type = NGX_MEDIA_SRT_EVENT_OPEN;
    event.session_id = state->id;

    len = ngx_media_srt_session_streamid(session, event.streamid,
                                         NGX_MEDIA_SRT_STREAMID_MAX);

    if (len > 0) {
        event.streamid[len] = '\0';
        event.streamid_len = (ngx_uint_t) len;
    }

    (void) ngx_media_srt_event_push(ingest, &event);

    (void) ngx_atomic_fetch_add(&ingest->sessions_accepted, 1);

    ngx_media_srt_notify(notify);
}

static void
ngx_media_srt_session_finish(ngx_media_srt_ingest_t *ingest,
    ngx_media_srt_session_state_t *state, ngx_media_srt_notify_t *notify)
{
    ngx_media_srt_event_t  event;

    ngx_memzero(&event, sizeof(event));
    event.type = NGX_MEDIA_SRT_EVENT_CLOSE;
    event.session_id = state->id;
    event.bytes = state->bytes;
    event.chunks = state->chunks;

    (void) ngx_media_srt_event_push(ingest, &event);

    ngx_media_srt_session_close(state->session);

    ngx_media_srt_session_state_release(state);

    ngx_media_srt_notify(notify);
}

static void *
ngx_media_srt_thread(void *data)
{
    ngx_media_srt_ingest_t        *ingest = data;
    ngx_media_srt_notify_t         notify;
    ngx_media_srt_listener_t      *listener;
    ngx_media_srt_session_t       *session;
    ngx_media_srt_session_state_t *state;
    ngx_media_srt_poll_t          *poll;
    ngx_media_srt_poll_event_t     events[NGX_MEDIA_SRT_POLL_MAX];
    ngx_media_srt_event_t          event;
    u_char                         buf[65536];
    ngx_uint_t                     i, count;
    ngx_int_t                      n;

    notify.ingest = ingest;
    notify.last_notify = 0;

    listener = ngx_media_srt_listen(ingest->conf.host.data, ingest->conf.port,
                                    ingest->conf.params, NULL);

    if (listener == NULL) {
        ngx_memzero(&event, sizeof(event));
        event.type = NGX_MEDIA_SRT_EVENT_FAILED;

        (void) ngx_media_srt_event_push(ingest, &event);
        (void) ngx_atomic_fetch_add(&ingest->failed, 1);

        ngx_media_srt_notify(&notify);

        return NULL;
    }

    poll = ngx_media_srt_poll_create(NULL);

    if (poll == NULL
        || ngx_media_srt_poll_add_listener(poll, listener) != NGX_OK)
    {
        ngx_media_srt_listen_close(listener);

        ngx_memzero(&event, sizeof(event));
        event.type = NGX_MEDIA_SRT_EVENT_FAILED;

        (void) ngx_media_srt_event_push(ingest, &event);
        (void) ngx_atomic_fetch_add(&ingest->failed, 1);

        ngx_media_srt_notify(&notify);

        return NULL;
    }

    (void) ngx_atomic_fetch_add(&ingest->ready, 1);

    ngx_memzero(&event, sizeof(event));
    event.type = NGX_MEDIA_SRT_EVENT_READY;

    (void) ngx_media_srt_event_push(ingest, &event);
    ngx_media_srt_notify(&notify);

    while (!ingest->stop) {

        if (ngx_media_srt_poll_wait(poll, 200, events, NGX_MEDIA_SRT_POLL_MAX,
                                    &count) != NGX_OK)
        {
            break;
        }

        for (i = 0; i < count; i++) {

            if (events[i].listener != NULL) {

                session = ngx_media_srt_accept_ready(events[i].listener, NULL);

                if (session == NULL) {
                    continue;
                }

                if (ngx_media_srt_poll_add_session(poll, session) != NGX_OK) {
                    ngx_media_srt_session_close(session);
                    continue;
                }

                ngx_media_srt_session_open(ingest, session, &notify);
                continue;
            }

            if (events[i].session == NULL) {
                continue;
            }

            state = ngx_media_srt_session_state_find(ingest,
                                                     events[i].session);

            if (state == NULL) {
                continue;
            }

            if (state->close_requested) {
                ngx_media_srt_poll_remove_session(poll, state->session);
                ngx_media_srt_session_finish(ingest, state, &notify);
                continue;
            }

            n = ngx_media_srt_session_recv(state->session, buf, sizeof(buf),
                                           200);

            if (n > 0) {
                (void) ngx_media_ts_ingest_write(&ingest->payload, state->id,
                                                 buf, (size_t) n,
                                                 ngx_media_srt_now());
                state->bytes += (uint64_t) n;
                state->chunks++;

                ngx_media_srt_notify(&notify);
                continue;
            }

            if (n == 0) {
                continue;
            }

            /* the publisher is gone */
            ngx_media_srt_poll_remove_session(poll, state->session);
            ngx_media_srt_session_finish(ingest, state, &notify);
        }
    }

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
        if (ingest->sessions[i].session != NULL) {
            ngx_media_srt_session_close(ingest->sessions[i].session);
            ngx_media_srt_session_state_release(&ingest->sessions[i]);
        }
    }

    ngx_media_srt_poll_destroy(poll);
    ngx_media_srt_listen_close(listener);

    return NULL;
}

ngx_int_t
ngx_media_srt_ingest_close_session(ngx_media_srt_ingest_t *ingest,
    uint64_t session_id)
{
    ngx_uint_t  i;

    if (ingest == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {

        if (ingest->sessions[i].session != NULL
            && ingest->sessions[i].id == session_id)
        {
            (void) ngx_atomic_cmp_set(&ingest->sessions[i].close_requested,
                                      0, 1);
            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}

ngx_int_t
ngx_media_srt_ingest_start(ngx_media_srt_ingest_t *ingest,
    const ngx_media_srt_ingest_conf_t *conf, ngx_log_t *log)
{
    ngx_uint_t  capacity;

    if (ingest == NULL || conf == NULL || conf->host.data == NULL
        || conf->port == 0 || conf->max_events == 0)
    {
        return NGX_ERROR;
    }

    ngx_memzero(ingest, sizeof(ngx_media_srt_ingest_t));

    ingest->conf = *conf;

    if (ingest->conf.max_chunks == 0) {
        ingest->conf.max_chunks = 256;
    }

    if (ingest->conf.max_bytes == 0) {
        ingest->conf.max_bytes = 8 * 1024 * 1024;
    }

    capacity = 1;

    while (capacity < ingest->conf.max_events) {
        if (capacity > ((ngx_uint_t) -1 >> 1)) {
            return NGX_ERROR;
        }

        capacity <<= 1;
    }

    if (capacity > (size_t) -1 / sizeof(ngx_media_srt_event_t)) {
        return NGX_ERROR;
    }

    ingest->events = ngx_alloc(capacity * sizeof(ngx_media_srt_event_t), log);
    if (ingest->events == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(ingest->events, capacity * sizeof(ngx_media_srt_event_t));
    ingest->events_capacity = capacity;

    {
        ngx_media_ts_ingest_conf_t  qconf;

        qconf.max_chunks = ingest->conf.max_chunks;
        qconf.max_bytes = ingest->conf.max_bytes;

        if (ngx_media_ts_ingest_init(&ingest->payload, &qconf, log) != NGX_OK) {
            ngx_free(ingest->events);
            ingest->events = NULL;
            return NGX_ERROR;
        }
    }

    ingest->notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ingest->notify_fd == -1) {
        ngx_media_ts_ingest_destroy(&ingest->payload);
        ngx_free(ingest->events);
        ingest->events = NULL;
        return NGX_ERROR;
    }

    if (pthread_create(&ingest->thread, NULL, ngx_media_srt_thread, ingest)
        != 0)
    {
        close(ingest->notify_fd);
        ingest->notify_fd = -1;
        ngx_media_ts_ingest_destroy(&ingest->payload);
        ngx_free(ingest->events);
        ingest->events = NULL;
        return NGX_ERROR;
    }

    ingest->thread_started = 1;

    return NGX_OK;
}

void
ngx_media_srt_ingest_stop(ngx_media_srt_ingest_t *ingest)
{
    if (ingest == NULL || ingest->events == NULL) {
        return;
    }

    (void) ngx_atomic_cmp_set(&ingest->stop, 0, 1);

    if (ingest->thread_started) {
        (void) pthread_join(ingest->thread, NULL);
        ingest->thread_started = 0;
    }

    if (ingest->notify_fd != -1) {
        close(ingest->notify_fd);
        ingest->notify_fd = -1;
    }

    ngx_media_ts_ingest_destroy(&ingest->payload);

    ngx_free(ingest->events);
    ingest->events = NULL;
    ingest->events_capacity = 0;
}

ngx_uint_t
ngx_media_srt_ingest_event_read(ngx_media_srt_ingest_t *ingest,
    ngx_media_srt_event_t *out, ngx_uint_t max)
{
    ngx_media_srt_event_t  *slot;
    ngx_uint_t              count;

    if (ingest == NULL || ingest->events == NULL || out == NULL || max == 0) {
        return 0;
    }

    while (!ngx_atomic_cmp_set(&ingest->events_lock, 0, 1)) {
        /* spin */
    }

    count = 0;

    while (count < max && ingest->events_tail < ingest->events_head) {
        slot = &ingest->events[ingest->events_tail
                               & (ingest->events_capacity - 1)];

        out[count] = *slot;
        ingest->events_tail++;
        count++;
    }

    (void) ngx_atomic_cmp_set(&ingest->events_lock, 1, 0);

    return count;
}

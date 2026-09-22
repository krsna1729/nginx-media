#include "ngx_media_srt_ingest.h"

#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

/*
 * The wake-up is one byte on the eventfd, and the worker re-reads both queues
 * until they are empty (goal doc 15: clear and re-check), so a byte the worker
 * has not taken yet covers every event pushed after it: no second write is
 * needed while one is pending.  What a byte may not do is go missing - see
 * ngx_media_srt_notify().
 */

/*
 * Binding the listener can legitimately fail on a reload: the worker being
 * replaced still holds the port - an SRT listener owns the UDP socket the
 * port is bound to, and a publisher attached to it keeps that socket alive
 * until the worker is gone.  The bind is therefore retried until it succeeds,
 * with the retries reported only when they stop looking like a reload.
 */
#define NGX_MEDIA_SRT_LISTEN_RETRY_MS   100
#define NGX_MEDIA_SRT_LISTEN_WARN_EVERY 50

static ngx_msec_t ngx_media_srt_now(void);
static void ngx_media_srt_notify(ngx_media_srt_ingest_t *ingest);
static ngx_uint_t ngx_media_srt_event_push(ngx_media_srt_ingest_t *ingest,
    const ngx_media_srt_event_t *event);
static ngx_media_srt_listener_t *ngx_media_srt_thread_listen(
    ngx_media_srt_ingest_t *ingest);
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
ngx_media_srt_notify(ngx_media_srt_ingest_t *ingest)
{
    uint64_t  one = 1;
    ssize_t   n;

    /*
     * A byte the worker has not read yet is a wake-up it has not taken, and
     * when it takes it, it drains everything queued by then - so a pending
     * byte covers what is being pushed now and the write can be skipped.
     *
     * With nothing pending the write is not optional.  Skipping it to save a
     * syscall is what stranded an event: the ingest pushes an OPEN or a
     * CLOSE, finds the rate limit still in force from a push the worker has
     * already consumed, writes nothing, and no byte is left for the worker to
     * wake on.  The session's OPEN is then never registered (its media is
     * read and dropped) or its CLOSE is never seen (its slot is never freed,
     * and the worker logs nothing either way).
     *
     * `notified` is cleared by the worker before it drains, so a push that
     * races the drain writes its own byte rather than relying on the one
     * being read at that moment.
     */
    if (ingest->notified) {
        return;
    }

    ingest->notified = 1;

    n = write(ingest->notify_fd, &one, sizeof(one));

    if (n != (ssize_t) sizeof(one)) {
        /* nothing was left pending: the next push has to write again */
        ingest->notified = 0;
    }
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
    /*
     * The whole slot goes back to its free state, close_requested included.
     * A slot that held a session the worker asked to close (the control API
     * removed its source) kept that request, and alloc hands the slot to the
     * first publisher that needs one - whose first poll event then took the
     * close_requested branch in the loop below and finished the session
     * before a byte was read: accepted, bytes=0, and no line in the log
     * saying why.  That is why it took a source delete among a few session
     * churn cycles to show up.
     */
    /*
     * The whole slot goes back to its free state, close_requested included.
     * A slot that held a session the worker asked to close (the control API
     * removed its source) kept that request, and alloc hands the slot to the
     * first publisher that needs one - whose first poll event then took the
     * close_requested branch in the loop below and finished the session
     * before a byte was read: accepted, bytes=0, and no line in the log
     * saying why.  That is why it took a source delete among a few session
     * churn cycles to show up.
     */
    ngx_memzero(state, sizeof(ngx_media_srt_session_state_t));
}

static void
ngx_media_srt_session_open(ngx_media_srt_ingest_t *ingest,
    ngx_media_srt_session_t *session)
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

    ngx_media_srt_notify(ingest);
}

static void
ngx_media_srt_session_finish(ngx_media_srt_ingest_t *ingest,
    ngx_media_srt_session_state_t *state)
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

    ngx_media_srt_notify(ingest);
}

/*
 * Binds the listener, waiting out the worker a reload is replacing.
 *
 * nginx starts the replacement worker before the worker it replaces has
 * exited, and an SRT listener owns the UDP socket the port is bound to: while
 * that worker still holds it - with a publisher attached, until the process
 * is gone - no second bind can succeed.  Giving up on the first failure is
 * what left the instance with no listener at all once the old worker exited.
 *
 * So the bind is retried until it succeeds.  There is no attempt ceiling: the
 * only correct outcome for a configured listener is to have one, and a port
 * held by something that is not going away is reported periodically instead
 * of being fatal.  Each retry is cheap (one socket, one failed bind), and the
 * worker's exit still interrupts it at once - ngx_media_srt_ingest_stop()
 * joins this thread.
 */
static ngx_media_srt_listener_t *
ngx_media_srt_thread_listen(ngx_media_srt_ingest_t *ingest)
{
    ngx_media_srt_listener_t  *listener;
    ngx_uint_t                 attempt;

    for (attempt = 0; ; attempt++) {

        if (attempt > 0) {
            struct timespec  pause;

            pause.tv_sec = 0;
            pause.tv_nsec = (long) NGX_MEDIA_SRT_LISTEN_RETRY_MS * 1000000;

            (void) nanosleep(&pause, NULL);

            /* the worker may be exiting: give the join something to see */
            if (ingest->stop) {
                return NULL;
            }
        }

        /*
         * A bonded listener covers two local addresses and accepts group
         * callers; the plain one covers a single address.  Which is used is a
         * property of the configuration, and the session that comes back is
         * the same kind either way, so nothing below this line knows about
         * bond members.
         */
        if (ingest->conf.bond_host.len > 0) {
            listener = ngx_media_srt_listen_bond(ingest->conf.host.data,
                                                 ingest->conf.port,
                                                 ingest->conf.bond_host.data,
                                                 ingest->conf.params, NULL);

        } else {
            listener = ngx_media_srt_listen(ingest->conf.host.data,
                                            ingest->conf.port,
                                            ingest->conf.params, NULL);
        }

        if (listener != NULL) {

            if (attempt > 0 && ingest->log != NULL) {
                ngx_log_error(NGX_LOG_NOTICE, ingest->log, 0,
                              "media: srt listener bound %V:%ui after %ui "
                              "attempt(s): the port was released",
                              &ingest->conf.host, ingest->conf.port,
                              attempt + 1);
            }

            return listener;
        }

        if (ingest->log == NULL) {
            continue;
        }

        if (attempt == 0) {
            /*
             * The expected way to arrive here is the replacement worker of a
             * reload, racing the worker it replaces.  Say so, and say what
             * the transport reported, so a port held by something else is
             * diagnosable from the first line.
             */
            ngx_log_error(NGX_LOG_NOTICE, ingest->log, 0,
                          "media: srt listener %V:%ui is not available yet "
                          "(%s); retrying until the port is free",
                          &ingest->conf.host, ingest->conf.port,
                          ngx_media_srt_last_error());

        } else if (attempt % NGX_MEDIA_SRT_LISTEN_WARN_EVERY == 0) {
            ngx_log_error(NGX_LOG_WARN, ingest->log, 0,
                          "media: srt listener %V:%ui is still bound after "
                          "%ui s (%s); another process may be holding the "
                          "port",
                          &ingest->conf.host, ingest->conf.port,
                          (ngx_uint_t) (attempt
                                        * NGX_MEDIA_SRT_LISTEN_RETRY_MS / 1000),
                          ngx_media_srt_last_error());
        }
    }
}

static void *
ngx_media_srt_thread(void *data)
{
    ngx_media_srt_ingest_t        *ingest = data;
    ngx_media_srt_listener_t      *listener;
    ngx_media_srt_session_t       *session;
    ngx_media_srt_session_state_t *state;
    ngx_media_srt_poll_t          *poll;
    ngx_media_srt_poll_event_t     events[NGX_MEDIA_SRT_POLL_MAX];
    ngx_media_srt_event_t          event;
    u_char                         buf[65536];
    ngx_uint_t                     i, count;
    ngx_uint_t                     accepting = 1;
    ngx_int_t                      n;

    listener = ngx_media_srt_thread_listen(ingest);

    if (listener == NULL) {
        /*
         * The retry only stops when the worker is going away, so there is no
         * listener to report either way.
         */
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

        ngx_media_srt_notify(ingest);

        return NULL;
    }

    (void) ngx_atomic_fetch_add(&ingest->ready, 1);

    ngx_memzero(&event, sizeof(event));
    event.type = NGX_MEDIA_SRT_EVENT_READY;

    (void) ngx_media_srt_event_push(ingest, &event);
    ngx_media_srt_notify(ingest);

    while (!ingest->stop) {

        if (ingest->draining && accepting) {
            ngx_uint_t  draining = 0;

            /*
             * A graceful shutdown has begun.  Stop accepting: a publisher
             * taken now would be dropped when this worker exits.  The
             * sessions already accepted are left alone - they keep running,
             * and with them the transport's socket on the port, so the
             * worker a reload started may have to wait for this one to go.
             */
            accepting = 0;

            ngx_media_srt_poll_remove_listener(poll, listener);
            ngx_media_srt_listen_stop(listener);

            for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
                if (ingest->sessions[i].session != NULL) {
                    draining++;
                }
            }

            if (ingest->log != NULL) {
                ngx_log_error(NGX_LOG_NOTICE, ingest->log, 0,
                              "media: srt listener %V:%ui released for the "
                              "worker the reload started, %ui session(s) "
                              "still draining",
                              &ingest->conf.host, ingest->conf.port, draining);
            }
        }

        if (ngx_media_srt_poll_wait(poll, 200, events, NGX_MEDIA_SRT_POLL_MAX,
                                    &count) != NGX_OK)
        {
            if (ingest->log != NULL) {
                ngx_log_error(NGX_LOG_ERR, ingest->log, 0,
                              "media: srt ingest poll failed (%s); the "
                              "listener is stopping", ngx_media_srt_last_error());
            }

            break;
        }

        for (i = 0; i < count; i++) {

            if (events[i].listener != NULL) {

                if (!accepting) {
                    /* released for the reload: nothing is accepted here */
                    continue;
                }

                session = ngx_media_srt_accept_ready(events[i].listener, NULL);

                if (session == NULL) {
                    continue;
                }

                if (ngx_media_srt_poll_add_session(poll, session) != NGX_OK) {
                    /*
                     * The session is closed, so the publisher sees a
                     * transport that connects and then carries nothing.  Say
                     * so: without this line the only evidence is on the
                     * client, and the cause looks like the client's.
                     */
                    if (ingest->log != NULL) {
                        ngx_log_error(NGX_LOG_WARN, ingest->log, 0,
                                      "media: closing an accepted srt session: "
                                      "it could not be polled (%s)",
                                      ngx_media_srt_last_error());
                    }

                    ngx_media_srt_session_close(session);
                    continue;
                }

                ngx_media_srt_session_open(ingest, session);
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
                ngx_media_srt_session_finish(ingest, state);
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

                ngx_media_srt_notify(ingest);
                continue;
            }

            if (n == 0) {
                continue;
            }

            /* the publisher is gone */
            ngx_media_srt_poll_remove_session(poll, state->session);
            ngx_media_srt_session_finish(ingest, state);
        }
    }

    if (accepting) {
        /*
         * The worker is going away.  Give the listening sockets up before
         * the sessions are closed, not after: the port has to be free for the
         * worker a reload replaced this one with, and closing sessions is the
         * slow part (the library runs its closing handshake first).
         */
        ngx_media_srt_poll_remove_listener(poll, listener);
        ngx_media_srt_listen_stop(listener);
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
    ingest->log = log;

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
ngx_media_srt_ingest_stop_accepting(ngx_media_srt_ingest_t *ingest)
{
    if (ingest == NULL) {
        return;
    }

    (void) ngx_atomic_cmp_set(&ingest->draining, 0, 1);
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

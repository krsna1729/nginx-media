#include "ngx_media_route.h"
#include "ngx_media_graph.h"

#ifndef NGX_MEDIA_UNIT_TEST

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <sys/mman.h>

/*
 * Each configuration cycle owns its matrix until the master destroys that
 * cycle's pool.  Forked workers keep their own descriptors and memory, so a
 * reload can close the master's old matrix without disturbing draining peers.
 */
typedef struct {
    ngx_uint_t  workers;
    int         fds[NGX_MEDIA_ROUTE_MAX_WORKERS][NGX_MEDIA_ROUTE_MAX_WORKERS];
    ngx_atomic_t *pids;
} ngx_media_route_generation_t;

static ngx_media_ipc_endpoint_t
    *ngx_media_route_endpoints[NGX_MEDIA_ROUTE_MAX_WORKERS];

static ngx_connection_t      *ngx_media_route_connections[
                                  NGX_MEDIA_ROUTE_MAX_WORKERS];

static ngx_uint_t             ngx_media_route_workers;
static ngx_media_route_frame_pt  ngx_media_route_sink;
static void                  *ngx_media_route_sink_ctx;
static ngx_media_route_status_pt ngx_media_route_status_sinks[3];
static ngx_media_route_generation_t *ngx_media_route_active_generation;

static void
ngx_media_route_generation_cleanup(void *data)
{
    ngx_media_route_generation_t  *generation = data;
    ngx_uint_t                    i, j;

    for (i = 0; i < generation->workers; i++) {
        for (j = 0; j < generation->workers; j++) {
            if (generation->fds[i][j] >= 0) {
                (void) close(generation->fds[i][j]);
                generation->fds[i][j] = -1;
            }
        }
    }
    if (generation->pids != NULL) {
        (void) munmap((void *) generation->pids,
                      generation->workers * sizeof(ngx_atomic_t));
        generation->pids = NULL;
    }
}

static ngx_media_route_generation_t *
ngx_media_route_generation(ngx_cycle_t *cycle)
{
    ngx_pool_cleanup_t  *cleanup;

    for (cleanup = cycle->pool->cleanup; cleanup != NULL;
         cleanup = cleanup->next)
    {
        if (cleanup->handler == ngx_media_route_generation_cleanup) {
            return cleanup->data;
        }
    }

    return NULL;
}

ngx_int_t
ngx_media_route_master_init(ngx_cycle_t *cycle, ngx_log_t *log)
{
    ngx_media_route_generation_t  *generation;
    ngx_pool_cleanup_t            *cleanup;
    ngx_uint_t                     i, j, workers;

    workers = ngx_media_owner_worker_count(cycle);

    /* Reject before touching the active cycle's endpoints. Configuration
     * validation normally catches this; the master keeps its own guard. */
    if (workers > NGX_MEDIA_ROUTE_MAX_WORKERS) {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "media: worker_processes %ui exceeds routing limit %ui",
                      workers, (ngx_uint_t) NGX_MEDIA_ROUTE_MAX_WORKERS);
        return NGX_ERROR;
    }

    if (ngx_media_route_generation(cycle) != NULL) {
        return NGX_ERROR;
    }

    cleanup = ngx_pool_cleanup_add(cycle->pool,
                                  sizeof(ngx_media_route_generation_t));
    if (cleanup == NULL) {
        return NGX_ERROR;
    }

    generation = cleanup->data;
    generation->workers = workers;
    generation->pids = NULL;
    ngx_memset(generation->fds, -1, sizeof(generation->fds));
    cleanup->handler = ngx_media_route_generation_cleanup;

    generation->pids = mmap(NULL, workers * sizeof(ngx_atomic_t),
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (generation->pids == MAP_FAILED) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "media: could not allocate generation-local "
                      "routing peer identities");
        generation->pids = NULL;
        return NGX_ERROR;
    }
    for (i = 0; i < workers; i++) {

        for (j = i + 1; j < workers; j++) {
            int  fds[2];

            if (socketpair(AF_UNIX,
                           SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                           fds) != 0)
            {
                ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                              "media: could not create the routing socket pair "
                              "for workers %ui and %ui", i, j);
                ngx_media_route_generation_cleanup(generation);
                return NGX_ERROR;
            }

            generation->fds[i][j] = fds[0];
            generation->fds[j][i] = fds[1];
        }
    }

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: routing socket pairs created for %ui workers",
                  workers);

    return NGX_OK;
}

/*
 * Ownership is derived, never published: the answer is the deterministic slot
 * for application/stream, so every worker computes the same one on its own.
 *
 * The shared owner-directory record deliberately does not decide this.  It
 * used to win whenever it held a live record, and that record is written by
 * whichever worker takes the API request (ngx_media_runtime_claim) - not the
 * worker that drives the program.  Measured on this tree (`make incarnation`,
 * two workers, log line numbers from an instrumented run): live/inc-2 hashes
 * to worker 1 on both workers, the POST landed on worker 0 and claimed the
 * stream for worker 0, and the SRT listener worker then read that record as
 * its own ownership - it opened the publisher's source locally while the
 * graph reported worker 1, and neither answer named the driver.  The record
 * is liveness and progress now, not identity: it can only ever repeat this
 * answer, because ngx_media_owner_dir_claim refuses to publish a slot that is
 * not the one returned here.
 */
ngx_uint_t
ngx_media_route_owner(ngx_cycle_t *cycle, uint64_t hash)
{
    return ngx_media_owner_for(cycle, hash);
}

ngx_uint_t
ngx_media_route_is_owner(ngx_cycle_t *cycle, uint64_t hash)
{
    return (ngx_media_route_owner(cycle, hash) == (ngx_uint_t) ngx_worker);
}

static uint64_t   ngx_media_route_bad_datagrams;
static ngx_msec_t ngx_media_route_bad_logged;

static void
ngx_media_route_endpoint_close(ngx_uint_t peer)
{
    ngx_connection_t  *c = ngx_media_route_connections[peer];

    if (c != NULL) {
        if (c->read->active) {
            (void) ngx_del_event(c->read, NGX_READ_EVENT, NGX_CLOSE_EVENT);
        }
        if (c->read->posted) {
            ngx_delete_posted_event(c->read);
        }
        if (c->write->posted) {
            ngx_delete_posted_event(c->write);
        }

        /* Keep fd valid until nginx removes it from its connection index. */
        ngx_free_connection(c);
        c->fd = (ngx_socket_t) -1;
        ngx_media_route_connections[peer] = NULL;
    }

    if (ngx_media_route_endpoints[peer] != NULL) {
        ngx_media_ipc_close(ngx_media_route_endpoints[peer]);
        ngx_media_route_endpoints[peer] = NULL;
    }
}

/* feeds frames received from another worker into the local program */
static void
ngx_media_route_read_handler(ngx_event_t *ev)
{
    ngx_connection_t         *c = ev->data;
    ngx_media_ipc_endpoint_t *endpoint = c->data;
    ngx_media_ipc_message_t   message;
    ngx_int_t                 rc;
    ngx_media_ipc_header_t     reply;
    ngx_uint_t                peer;

    for (peer = 0; peer < ngx_media_route_workers; peer++) {
        if (ngx_media_route_endpoints[peer] == endpoint) {
            break;
        }
    }

    if (peer == ngx_media_route_workers) {
        return;
    }

    for ( ;; ) {
        rc = (c->close || c->error) ? NGX_ERROR
                                   : ngx_media_ipc_recv(endpoint, &message);

        if (rc == NGX_AGAIN) {
            return;
        }

        if (rc == NGX_DECLINED) {
            /* one malformed datagram: count it, say so at most once a second */
            ngx_media_route_bad_datagrams++;

            if (ngx_current_msec - ngx_media_route_bad_logged >= 1000
                || ngx_media_route_bad_logged == 0)
            {
                ngx_media_route_bad_logged = ngx_current_msec;
                ngx_log_error(NGX_LOG_WARN, c->log, 0,
                              "media: worker %ui dropped a malformed routing "
                              "datagram (%uL so far)", (ngx_uint_t) ngx_worker,
                              ngx_media_route_bad_datagrams);
            }

            continue;
        }

        if (rc != NGX_OK) {
            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "media: worker %ui routing endpoint closed",
                          (ngx_uint_t) ngx_worker);
            if (ngx_media_route_sink != NULL) {
                (void) ngx_media_route_sink(ngx_media_route_sink_ctx, peer,
                                            0, NULL, NULL);
            }
            for (ngx_uint_t type = NGX_MEDIA_SOURCE_SRT;
                 type <= NGX_MEDIA_SOURCE_RTMP; type++)
            {
                if (ngx_media_route_status_sinks[type] != NULL) {
                    ngx_media_route_status_sinks[type](peer, NULL);
                }
            }
            ngx_media_route_endpoint_close(peer);
            return;
        }

        if (message.header.type == NGX_MEDIA_IPC_MSG_ACCEPT
            || message.header.type == NGX_MEDIA_IPC_MSG_REJECT)
        {
            ngx_uint_t type = message.header.source_type;

            if (peer == ngx_media_route_owner((ngx_cycle_t *) ngx_cycle,
                                              message.header.hash)
                && type >= NGX_MEDIA_SOURCE_SRT
                && type <= NGX_MEDIA_SOURCE_RTMP
                && message.header.length == 0
                && ngx_media_route_status_sinks[type] != NULL)
            {
                ngx_media_route_status_sinks[type](peer, &message.header);
            }
            ngx_media_ipc_message_release(&message);
            continue;
        }

        if (ngx_media_route_sink != NULL) {
            /*
             * The header carries the full 64-bit identity; truncating it here
             * gave the receiver a different hash than the one the sender
             * addressed, so a routed source could resolve to another stream's
             * slot (or to none) while the two workers believed they agreed.
             */
            rc = ngx_media_route_sink(ngx_media_route_sink_ctx, peer,
                                      message.header.hash,
                                      &message.header, message.payload);

            if (message.header.type == NGX_MEDIA_IPC_MSG_OPEN) {
                ngx_memzero(&reply, sizeof(reply));
                reply.version = NGX_MEDIA_IPC_VERSION;
                reply.type = rc == NGX_OK ? NGX_MEDIA_IPC_MSG_ACCEPT
                                          : NGX_MEDIA_IPC_MSG_REJECT;
                reply.hash = message.header.hash;
                reply.incarnation = message.header.incarnation;
                reply.session = message.header.session;
                reply.source_type = message.header.source_type;

                if (ngx_media_ipc_send_header(endpoint, &reply) != NGX_OK
                    && rc == NGX_OK)
                {
                    /* No ingress can commit this admission; undo its slot. */
                    reply.type = NGX_MEDIA_IPC_MSG_CLOSE;
                    (void) ngx_media_route_sink(ngx_media_route_sink_ctx, peer,
                                                reply.hash, &reply, NULL);
                }
            }
        }

        ngx_media_ipc_message_release(&message);
    }
}

ngx_int_t
ngx_media_route_worker_init(ngx_cycle_t *cycle, ngx_log_t *log)
{
    ngx_media_route_generation_t  *generation;
    ngx_media_ipc_endpoint_t      *endpoint;
    ngx_connection_t             *c;
    ngx_uint_t                    slot, i, j;

    slot = (ngx_uint_t) ngx_worker;
    generation = ngx_media_route_generation(cycle);
    if (generation == NULL || slot >= generation->workers) {
        return NGX_ERROR;
    }

    ngx_media_route_workers = generation->workers;
    ngx_media_route_active_generation = generation;
    ngx_atomic_t *pid_slot = &generation->pids[slot];
    while (!ngx_atomic_cmp_set(pid_slot, ngx_atomic_fetch_add(pid_slot, 0),
                               (ngx_atomic_uint_t) ngx_pid))
    {
        /* A prior worker may be publishing its clean-exit release. */
    }

    /* Each worker owns only its row.  Keeping peers' copies open masks peer
     * exit and leaks O(workers squared) descriptors in every worker. */
    for (i = 0; i < generation->workers; i++) {
        if (i == slot) {
            continue;
        }
        for (j = 0; j < generation->workers; j++) {
            if (generation->fds[i][j] >= 0) {
                (void) close(generation->fds[i][j]);
                generation->fds[i][j] = -1;
            }
        }
    }

    if (ngx_media_route_workers < 2) {
        return NGX_OK;
    }

    for (i = 0; i < ngx_media_route_workers; i++) {

        if (i == slot || generation->fds[slot][i] < 0) {
            continue;
        }

        if (ngx_media_ipc_adopt(generation->fds[slot][i], &endpoint, log)
            != NGX_OK)
        {
            ngx_log_error(NGX_LOG_EMERG, log, 0,
                          "media: could not adopt the routing endpoint to "
                          "worker %ui", i);
            goto failed;
        }

        ngx_media_route_endpoints[i] = endpoint;
        generation->fds[slot][i] = -1; /* endpoint owns it from here */

        c = ngx_get_connection(ngx_media_ipc_fd(endpoint), log);

        if (c == NULL) {
            goto failed;
        }

        c->data = endpoint;
        c->read->handler = ngx_media_route_read_handler;
        c->read->log = log;

        ngx_media_route_connections[i] = c;

        if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
            ngx_media_route_endpoint_close(i);
            goto failed;
        }

    }

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: worker %ui routing ready", slot);

    return NGX_OK;
failed:
    ngx_media_route_worker_shutdown(log);
    ngx_media_route_generation_cleanup(generation);
    return NGX_ERROR;
}


void
ngx_media_route_worker_shutdown(ngx_log_t *log)
{
    ngx_uint_t  i;

    (void) log;
    if (ngx_media_route_active_generation != NULL
        && ngx_media_route_active_generation->pids != NULL)
    {
        (void) ngx_atomic_cmp_set(
            &ngx_media_route_active_generation->pids[ngx_worker], ngx_pid, 0);
    }

    for (i = 0; i < ngx_media_route_workers; i++) {

        ngx_media_route_endpoint_close(i);
    }
}

void
ngx_media_route_set_sink(ngx_media_route_frame_pt cb, void *ctx)
{
    ngx_media_route_sink = cb;
    ngx_media_route_sink_ctx = ctx;
}

void
ngx_media_route_set_status_sink(ngx_uint_t source_type,
    ngx_media_route_status_pt cb)
{
    if (source_type >= NGX_MEDIA_SOURCE_SRT
        && source_type <= NGX_MEDIA_SOURCE_RTMP)
    {
        ngx_media_route_status_sinks[source_type] = cb;
    }
}

uint64_t
ngx_media_route_peer_pid(ngx_uint_t peer)
{
    ngx_pid_t  pid;

    if (ngx_media_route_active_generation == NULL
        || ngx_media_route_active_generation->pids == NULL
        || peer >= ngx_media_route_active_generation->workers)
    {
        return 0;
    }
    pid = (ngx_pid_t) ngx_atomic_fetch_add(
                         &ngx_media_route_active_generation->pids[peer], 0);
    if (pid == 0 || (kill(pid, 0) == -1 && ngx_errno == NGX_ESRCH)) {
        return 0;
    }
    return pid;
}

/*
 * The control plane's fan-out.  Every worker's row of endpoint pairs is
 * already there, so a broadcast is a walk of that row, and each send is a
 * non-blocking write to a socket that either has room or reports NGX_AGAIN.
 * Nothing here waits, retries or allocates: a graph operation is small and
 * bounded, and a worker that cannot take it is skipped and counted.
 */
static uint64_t  ngx_media_route_undelivered;
static ngx_msec_t  ngx_media_route_last_broadcast_log;

ngx_uint_t
ngx_media_route_broadcast(ngx_cycle_t *cycle, const ngx_media_ipc_header_t *header,
    ngx_media_buf_t *payload, size_t length, ngx_uint_t *peers)
{
    ngx_media_ipc_endpoint_t  *endpoint;
    ngx_uint_t                 i, slot, addressed = 0, delivered = 0;

    (void) cycle;

    if (header == NULL) {
        return 0;
    }

    slot = (ngx_uint_t) ngx_worker;

    if (ngx_media_route_workers < 2 || slot >= ngx_media_route_workers) {
        /* a single worker is its own replica: there is nobody to tell */
        if (peers != NULL) {
            *peers = 0;
        }

        return 0;
    }

    for (i = 0; i < ngx_media_route_workers; i++) {

        if (i == slot) {
            continue;
        }

        endpoint = ngx_media_route_endpoints[i];

        if (endpoint == NULL) {
            continue;
        }

        addressed++;

        if (ngx_media_ipc_send(endpoint, header, payload, 0, length) == NGX_OK)
        {
            delivered++;

        } else {
            ngx_media_route_undelivered++;
        }
    }

    if (delivered != addressed
        && ngx_current_msec - ngx_media_route_last_broadcast_log >= 1000)
    {
        /*
         * Say so rather than pretending: this worker has the mutation and a
         * peer does not, so that peer's replica is behind until the next
         * operation for the same object - or until desired state is replayed
         * through the API.
         *
         * The cumulative count is the exact record (it is a metric); the log
         * line is a notice, so a peer that is stuck does not turn every later
         * mutation into a write to the error log.
         */
        ngx_media_route_last_broadcast_log = ngx_current_msec;

        ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                      "media: graph operation reached %ui of %ui workers; a "
                      "worker that is gone or behind keeps an older replica "
                      "until the next mutation for the same object "
                      "(undelivered total: %uL)",
                      delivered, addressed, ngx_media_route_undelivered);
    }

    if (peers != NULL) {
        *peers = addressed;
    }

    return delivered;
}

uint64_t
ngx_media_route_broadcast_undelivered(void)
{
    return ngx_media_route_undelivered;
}

static ngx_media_ipc_endpoint_t *
ngx_media_route_endpoint_for(ngx_cycle_t *cycle, uint64_t hash)
{
    ngx_uint_t  owner = ngx_media_route_owner(cycle, hash);

    if (owner >= NGX_MEDIA_ROUTE_MAX_WORKERS) {
        return NULL;
    }

    return ngx_media_route_endpoints[owner];
}

/*
 * A publisher connection's identity across workers and protocols.  SRT's
 * transport ids and RTMP's local counters can coincide in one worker, so the
 * route assigns one sequence shared by both and includes the worker PID.
 * Late frames or CLOSE cannot name another protocol's or superseded session.
 */
uint64_t
ngx_media_route_session_id(void)
{
    static uint64_t  sequence;

    return ((uint64_t) ngx_pid << 32) | (++sequence & 0xffffffffULL);
}

ngx_int_t
ngx_media_route_open(ngx_cycle_t *cycle, uint64_t hash,
    uint64_t incarnation, uint64_t session, const ngx_str_t *application,
    const ngx_str_t *stream, const ngx_str_t *source_id,
    ngx_uint_t source_type, ngx_uint_t priority)
{
    ngx_media_ipc_endpoint_t  *endpoint;
    ngx_media_ipc_header_t     header;
    ngx_media_buf_t           *payload;
    size_t                     len;
    u_char                    *p;
    ngx_int_t                  rc;
    uint32_t                  lengths[3];

    endpoint = ngx_media_route_endpoint_for(cycle, hash);

    if (endpoint == NULL) {
        return NGX_ERROR;
    }

    /* Lengths preserve the complete graph identity, including separators. */
    if (application->len == 0 || stream->len == 0 || source_id->len == 0
        || application->len > NGX_MEDIA_GRAPH_MAX_NAME
        || stream->len > NGX_MEDIA_GRAPH_MAX_NAME
        || source_id->len > NGX_MEDIA_GRAPH_MAX_NAME)
    {
        return NGX_ERROR;
    }
    lengths[0] = (uint32_t) application->len;
    lengths[1] = (uint32_t) stream->len;
    lengths[2] = (uint32_t) source_id->len;
    len = sizeof(lengths) + application->len + stream->len + source_id->len;

    payload = ngx_media_buf_alloc(len);

    if (payload == NULL) {
        return NGX_ERROR;
    }

    p = ngx_media_buf_data(payload);
    ngx_memcpy(p, lengths, sizeof(lengths));
    p += sizeof(lengths);

    ngx_memcpy(p, application->data, application->len);
    p += application->len;
    ngx_memcpy(p, stream->data, stream->len);
    p += stream->len;
    ngx_memcpy(p, source_id->data, source_id->len);

    (void) ngx_media_buf_freeze(payload, len);

    ngx_memzero(&header, sizeof(header));
    header.version = NGX_MEDIA_IPC_VERSION;
    header.type = NGX_MEDIA_IPC_MSG_OPEN;
    header.hash = hash;
    header.incarnation = incarnation;
    header.session = session;
    header.source_type = (uint32_t) source_type;
    header.priority = (uint32_t) priority;
    rc = ngx_media_ipc_send(endpoint, &header, payload, 0, len);

    ngx_media_buf_unref(payload);

    return rc;
}

ngx_int_t
ngx_media_route_close(ngx_cycle_t *cycle, uint64_t hash,
    uint64_t incarnation, uint64_t session)
{
    ngx_media_ipc_endpoint_t  *endpoint;
    ngx_media_ipc_header_t     header;

    endpoint = ngx_media_route_endpoint_for(cycle, hash);

    if (endpoint == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&header, sizeof(header));
    header.version = NGX_MEDIA_IPC_VERSION;
    header.type = NGX_MEDIA_IPC_MSG_CLOSE;
    header.hash = hash;
    header.incarnation = incarnation;
    header.session = session;

    return ngx_media_ipc_send_header(endpoint, &header);
}

ngx_int_t
ngx_media_route_frame(ngx_cycle_t *cycle, uint64_t hash,
    uint64_t incarnation, uint64_t session, const ngx_media_frame_t *frame,
    uint64_t sequence)
{
    ngx_media_ipc_endpoint_t  *endpoint;
    ngx_media_ipc_header_t     header;

    if (frame == NULL || frame->payload == NULL) {
        return NGX_ERROR;
    }

    endpoint = ngx_media_route_endpoint_for(cycle, hash);

    if (endpoint == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&header, sizeof(header));

    header.version = NGX_MEDIA_IPC_VERSION;
    header.type = (frame->media_type == NGX_MEDIA_TYPE_AUDIO)
                      ? NGX_MEDIA_IPC_MSG_AUDIO
                      : ((frame->media_type == NGX_MEDIA_TYPE_DATA)
                             ? NGX_MEDIA_IPC_MSG_DATA
                             : NGX_MEDIA_IPC_MSG_VIDEO);
    header.hash = hash;
    header.incarnation = incarnation;
    header.session = session;
    header.sequence = sequence;
    header.pts = frame->pts;
    header.dts = frame->dts;
    header.media_type = (uint32_t) frame->media_type;
    header.codec = (uint32_t) frame->codec;
    header.payload_format = (uint32_t) frame->payload_format;
    header.track_index = (uint32_t) frame->track_index;
    header.keyframe = frame->keyframe ? 1 : 0;
    header.config = frame->config ? 1 : 0;

    return ngx_media_ipc_send(endpoint, &header, frame->payload, 0,
                              ngx_media_buf_size(frame->payload));
}

#endif /* !NGX_MEDIA_UNIT_TEST */

/*
 * The track contract travels as a compact payload: a count, then fixed fields
 * per track and the codec configuration blob where the source has one.
 */
ngx_int_t
ngx_media_route_tracks(ngx_cycle_t *cycle, uint64_t hash,
    uint64_t incarnation, uint64_t session, const ngx_media_trackset_t *tracks)
{
    ngx_media_ipc_endpoint_t  *endpoint;
    ngx_media_ipc_header_t     header;
    ngx_media_buf_t           *payload;
    ngx_uint_t                 i;
    size_t                     len = sizeof(uint32_t), offset;
    u_char                    *p;

    if (tracks == NULL || tracks->count == 0) {
        return NGX_ERROR;
    }

    endpoint = ngx_media_route_endpoint_for(cycle, hash);

    if (endpoint == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < tracks->count; i++) {
        len += 10 * sizeof(uint32_t);

        if (tracks->tracks[i].config != NULL) {
            len += ngx_media_buf_size(tracks->tracks[i].config);
        }
    }

    if (len > NGX_MEDIA_IPC_MAX_PAYLOAD) {
        /* same contract as graph_encode and route_open: one datagram, one
         * message, never chunked fragments the sink would parse as new
         * track sets */
        return NGX_ERROR;
    }

    payload = ngx_media_buf_alloc(len);

    if (payload == NULL) {
        return NGX_ERROR;
    }

    p = ngx_media_buf_data(payload);

    ngx_memcpy(p, &tracks->count, sizeof(uint32_t));
    p += sizeof(uint32_t);

    for (i = 0; i < tracks->count; i++) {
        uint32_t  fields[10];
        uint32_t  config_len = 0;

        fields[0] = (uint32_t) tracks->tracks[i].media_type;
        fields[1] = (uint32_t) tracks->tracks[i].codec;
        fields[2] = (uint32_t) tracks->tracks[i].payload_format;
        fields[3] = (uint32_t) tracks->tracks[i].sample_rate;
        fields[4] = (uint32_t) tracks->tracks[i].channels;
        fields[5] = (uint32_t) tracks->tracks[i].profile;
        fields[6] = (uint32_t) tracks->tracks[i].level;
        fields[7] = (uint32_t) tracks->tracks[i].width;
        fields[8] = (uint32_t) tracks->tracks[i].height;

        if (tracks->tracks[i].config != NULL) {
            config_len = (uint32_t) ngx_media_buf_size(tracks->tracks[i].config);
        }

        fields[9] = config_len;

        ngx_memcpy(p, fields, sizeof(fields));
        p += sizeof(fields);

        if (config_len > 0) {
            ngx_memcpy(p, ngx_media_buf_data(tracks->tracks[i].config),
                       config_len);
            p += config_len;
        }
    }

    offset = (size_t) (p - ngx_media_buf_data(payload));

    (void) ngx_media_buf_freeze(payload, offset);

    ngx_memzero(&header, sizeof(header));
    header.version = NGX_MEDIA_IPC_VERSION;
    header.type = NGX_MEDIA_IPC_MSG_TRACKS;
    header.hash = hash;
    header.incarnation = incarnation;
    header.session = session;

    i = ngx_media_ipc_send(endpoint, &header, payload, 0, offset);

    ngx_media_buf_unref(payload);

    return i;
}

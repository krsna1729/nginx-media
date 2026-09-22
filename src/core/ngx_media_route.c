#include "ngx_media_route.h"

#ifndef NGX_MEDIA_UNIT_TEST

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

/*
 * One row of endpoints per worker: row[i][j] is worker i's end of the pair it
 * shares with worker j.  The master creates every pair before forking, so a
 * worker only has to adopt the fds it inherits.
 */

static int                    ngx_media_route_fds[NGX_MEDIA_ROUTE_MAX_WORKERS]
                                               [NGX_MEDIA_ROUTE_MAX_WORKERS];

static ngx_media_ipc_endpoint_t
    *ngx_media_route_endpoints[NGX_MEDIA_ROUTE_MAX_WORKERS];

static ngx_connection_t      *ngx_media_route_connections[
                                  NGX_MEDIA_ROUTE_MAX_WORKERS];

static ngx_uint_t             ngx_media_route_workers;
static ngx_media_route_frame_pt  ngx_media_route_sink;
static void                  *ngx_media_route_sink_ctx;

ngx_int_t
ngx_media_route_master_init(ngx_cycle_t *cycle, ngx_log_t *log)
{
    ngx_uint_t  i, j, workers;

    workers = ngx_media_owner_worker_count(cycle);

    if (ngx_media_route_workers == workers) {
        return NGX_OK;   /* already configured for this worker count */
    }

    if (ngx_media_route_workers > 0) {
        /* close stale socket pairs when worker count changes on reload */
        for (i = 0; i < ngx_media_route_workers; i++) {
            for (j = i + 1; j < ngx_media_route_workers; j++) {
                if (ngx_media_route_fds[i][j] > 0) {
                    (void) close(ngx_media_route_fds[i][j]);
                    ngx_media_route_fds[i][j] = -1;
                }
                if (ngx_media_route_fds[j][i] > 0) {
                    (void) close(ngx_media_route_fds[j][i]);
                    ngx_media_route_fds[j][i] = -1;
                }
            }
        }
        ngx_media_route_workers = 0;
    }

    if (workers < 2 || workers > NGX_MEDIA_ROUTE_MAX_WORKERS) {
        /* a single worker needs no routing at all */
        ngx_media_route_workers = workers;
        return NGX_OK;
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
                return NGX_ERROR;
            }

            ngx_media_route_fds[i][j] = fds[0];
            ngx_media_route_fds[j][i] = fds[1];
        }
    }

    ngx_media_route_workers = workers;

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: routing socket pairs created for %ui workers",
                  workers);

    return NGX_OK;
}

ngx_uint_t
ngx_media_route_owner(ngx_cycle_t *cycle, uint64_t hash)
{
    ngx_media_owner_dir_t  *dir = ngx_media_runtime_owner_dir();
    ngx_uint_t              fallback = ngx_media_owner_for(cycle, hash);

    if (dir == NULL) {
        return fallback;
    }

    /* a live owner record wins; otherwise the deterministic slot does */
    return ngx_media_owner_dir_slot(dir, hash, fallback);
}

ngx_uint_t
ngx_media_route_is_owner(ngx_cycle_t *cycle, uint64_t hash)
{
    return (ngx_media_route_owner(cycle, hash) == (ngx_uint_t) ngx_worker);
}

/* feeds frames received from another worker into the local program */
static void
ngx_media_route_read_handler(ngx_event_t *ev)
{
    ngx_connection_t         *c = ev->data;
    ngx_media_ipc_endpoint_t *endpoint = c->data;
    ngx_media_ipc_message_t   message;
    ngx_int_t                 rc;

    for ( ;; ) {
        rc = ngx_media_ipc_recv(endpoint, &message);

        if (rc == NGX_AGAIN) {
            return;
        }

        if (rc != NGX_OK) {
            /* the peer is gone: nothing sensible to do but stop reading */
            (void) ngx_del_event(c->read, NGX_READ_EVENT, 0);
            return;
        }

        if (ngx_media_route_sink != NULL) {
            (void) ngx_media_route_sink(ngx_media_route_sink_ctx,
                                        (uint32_t) message.header.hash,
                                        &message.header, message.payload);
        }

        ngx_media_ipc_message_release(&message);
    }
}

ngx_int_t
ngx_media_route_worker_init(ngx_cycle_t *cycle, ngx_log_t *log)
{
    ngx_media_ipc_endpoint_t  *endpoint;
    ngx_connection_t          *c;
    ngx_uint_t                 slot, i;

    slot = (ngx_uint_t) ngx_worker;

    if (ngx_media_route_workers < 2 || slot >= ngx_media_route_workers) {
        return NGX_OK;
    }

    for (i = 0; i < ngx_media_route_workers; i++) {

        if (i == slot || ngx_media_route_fds[slot][i] < 0) {
            continue;
        }

        if (ngx_media_ipc_adopt(ngx_media_route_fds[slot][i], &endpoint, log)
            != NGX_OK)
        {
            ngx_log_error(NGX_LOG_EMERG, log, 0,
                          "media: could not adopt the routing endpoint to "
                          "worker %ui", i);
            return NGX_ERROR;
        }

        ngx_media_route_endpoints[i] = endpoint;

        c = ngx_get_connection(ngx_media_ipc_fd(endpoint), log);

        if (c == NULL) {
            return NGX_ERROR;
        }

        c->data = endpoint;
        c->read->handler = ngx_media_route_read_handler;
        c->read->log = log;

        if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
            ngx_free_connection(c);
            return NGX_ERROR;
        }

        ngx_media_route_connections[i] = c;
    }

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: worker %ui routing ready", slot);

    return NGX_OK;
}

void
ngx_media_route_worker_shutdown(ngx_log_t *log)
{
    ngx_uint_t  i;

    (void) log;

    for (i = 0; i < ngx_media_route_workers; i++) {

        if (ngx_media_route_connections[i] != NULL) {
            (void) ngx_del_event(ngx_media_route_connections[i]->read,
                                 NGX_READ_EVENT, 0);
            ngx_media_route_connections[i]->fd = (ngx_socket_t) -1;
            ngx_free_connection(ngx_media_route_connections[i]);
            ngx_media_route_connections[i] = NULL;
        }

        if (ngx_media_route_endpoints[i] != NULL) {
            ngx_media_ipc_close(ngx_media_route_endpoints[i]);
            ngx_media_route_endpoints[i] = NULL;
        }
    }
}

void
ngx_media_route_set_sink(ngx_media_route_frame_pt cb, void *ctx)
{
    ngx_media_route_sink = cb;
    ngx_media_route_sink_ctx = ctx;
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

ngx_int_t
ngx_media_route_open(ngx_cycle_t *cycle, uint64_t hash,
    const ngx_str_t *application, const ngx_str_t *stream,
    const ngx_str_t *source_id, ngx_uint_t source_type, ngx_uint_t priority)
{
    ngx_media_ipc_endpoint_t  *endpoint;
    ngx_media_ipc_header_t     header;
    ngx_media_buf_t           *payload;
    size_t                     len;
    u_char                    *p;
    ngx_int_t                  rc;

    endpoint = ngx_media_route_endpoint_for(cycle, hash);

    if (endpoint == NULL) {
        return NGX_ERROR;
    }

    /* "application/stream/source": exactly two separators */
    len = application->len + stream->len + source_id->len + 2;

    if (len > NGX_MEDIA_IPC_MAX_PAYLOAD) {
        /* the sink parses one datagram as one message: refuse rather than
         * truncate into chunked fragments it would misread as new opens */
        return NGX_ERROR;
    }

    payload = ngx_media_buf_alloc(len);

    if (payload == NULL) {
        return NGX_ERROR;
    }

    p = ngx_media_buf_data(payload);

    ngx_memcpy(p, application->data, application->len);
    p += application->len;
    *p++ = '/';
    ngx_memcpy(p, stream->data, stream->len);
    p += stream->len;
    *p++ = '/';
    ngx_memcpy(p, source_id->data, source_id->len);
    p += source_id->len;

    (void) ngx_media_buf_freeze(payload, len);

    ngx_memzero(&header, sizeof(header));

    header.version = NGX_MEDIA_IPC_VERSION;
    header.type = NGX_MEDIA_IPC_MSG_OPEN;
    header.hash = hash;
    header.source_type = (uint32_t) source_type;
    header.priority = (uint32_t) priority;

    rc = ngx_media_ipc_send(endpoint, &header, payload, 0, len);

    ngx_media_buf_unref(payload);

    return rc;
}

ngx_int_t
ngx_media_route_close(ngx_cycle_t *cycle, uint64_t hash)
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

    return ngx_media_ipc_send_header(endpoint, &header);
}

ngx_int_t
ngx_media_route_frame(ngx_cycle_t *cycle, uint64_t hash,
    const ngx_media_frame_t *frame, uint64_t sequence)
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
    const ngx_media_trackset_t *tracks)
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

    i = ngx_media_ipc_send(endpoint, &header, payload, 0, offset);

    ngx_media_buf_unref(payload);

    return i;
}

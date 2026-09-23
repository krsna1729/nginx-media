#ifndef NGX_MEDIA_IPC_H
#define NGX_MEDIA_IPC_H

#if defined(NGX_MEDIA_UNIT_TEST) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ngx_media.h"
#include "ngx_media_buffer.h"

/*
 * Bounded inter-worker transport (goal doc 23).
 *
 * When a publisher lands on a worker that does not own its stream, the frames
 * cross to the owner once per source over a versioned, typed message stream.
 * This is intra-host ownership routing, not distributed consensus: messages
 * are small, fixed-header records and payloads are batched where practical.
 *
 * The transport is a pair of Unix domain SOCK_SEQPACKET sockets.  A frame
 * larger than one message is split into ordered chunks with an explicit
 * continuation flag, so a receiver can always reassemble it, and a message
 * that does not fit the bound is rejected instead of being truncated.
 */

#define NGX_MEDIA_IPC_VERSION        2

#define NGX_MEDIA_IPC_MSG_OPEN       1
#define NGX_MEDIA_IPC_MSG_CLOSE      2
#define NGX_MEDIA_IPC_MSG_TRACKS     3
#define NGX_MEDIA_IPC_MSG_VIDEO      4
#define NGX_MEDIA_IPC_MSG_AUDIO      5
#define NGX_MEDIA_IPC_MSG_DATA       6

/*
 * 7, 8 and 9 are reserved and not sent by either side.  Control does not need
 * a message of its own: a selection change is a mutation of the graph, which
 * travels as NGX_MEDIA_IPC_MSG_GRAPH to every worker, and the owner of a
 * routed source learns that it ended from the CLOSE of its session rather than
 * from a health or EOF message.  The numbers are kept so a version 1 peer is
 * never handed a type it would have to interpret.
 */
#define NGX_MEDIA_IPC_MSG_RESERVED7  7
#define NGX_MEDIA_IPC_MSG_RESERVED8  8
#define NGX_MEDIA_IPC_MSG_RESERVED9  9

/*
 * A graph operation: one desired-state mutation of the control API, broadcast
 * to every worker so each holds a replica of the graph.  It travels the same
 * one-way transport as a routed publisher, but in the other direction: OPEN,
 * TRACKS and CLOSE carry one stream's media to its owner, this carries the
 * shape of the graph to everyone.  The payload is the operation (see
 * ngx_media_graph.h), not media.
 */
#define NGX_MEDIA_IPC_MSG_GRAPH      10

/*
 * The largest single datagram, header included.  A SOCK_SEQPACKET datagram is
 * delivered atomically: a receiver that offers a smaller buffer gets a
 * truncated one, so both sides use this exact bound and a payload chunk leaves
 * room for the header.
 */
#define NGX_MEDIA_IPC_MAX_DATAGRAM   (64 * 1024)
#define NGX_MEDIA_IPC_HEADER_MAX     96
#define NGX_MEDIA_IPC_MAX_PAYLOAD    (NGX_MEDIA_IPC_MAX_DATAGRAM \
                                      - NGX_MEDIA_IPC_HEADER_MAX)

/* socket buffers are sized so a full datagram always fits */
#define NGX_MEDIA_IPC_SOCKBUF        (4 * 1024 * 1024)

/* how many messages may be queued before the transport reports backpressure */
#define NGX_MEDIA_IPC_MAX_QUEUE      256

/* the largest frame a receiver will reassemble */
#define NGX_MEDIA_IPC_MAX_FRAME      (4 * 1024 * 1024)

typedef struct {
    uint8_t     version;
    uint8_t     type;           /* NGX_MEDIA_IPC_MSG_* */
    uint16_t    flags;          /* NGX_MEDIA_IPC_FLAG_* */
    uint32_t    length;         /* payload bytes carried by this message */
    uint32_t    total;          /* whole frame length, for chunked frames */
    uint32_t    offset;         /* this chunk's offset in the frame */
    uint64_t    hash;           /* stream hash the message belongs to */
    uint64_t    incarnation;    /* stream object lifetime identity */
    uint64_t    sequence;       /* frame sequence within the source */
    int64_t     pts;
    int64_t     dts;
    uint32_t    media_type;
    uint32_t    codec;
    uint32_t    payload_format;
    uint32_t    track_index;
    uint32_t    keyframe;
    uint32_t    config;
    uint32_t    source_type;
    uint32_t    priority;
} ngx_media_ipc_header_t;

#define NGX_MEDIA_IPC_FLAG_MORE      1   /* more chunks follow for this frame */

/* a decoded message: the header plus a view of its payload */
typedef struct {
    ngx_media_ipc_header_t  header;
    ngx_media_buf_t        *payload;   /* owned reference, may be NULL */
    size_t                  offset;
    size_t                  length;
} ngx_media_ipc_message_t;

typedef struct ngx_media_ipc_endpoint_s ngx_media_ipc_endpoint_t;

/*
 * Creates a connected pair of endpoints.  The first is meant for the local
 * worker, the second is handed to the peer (a thread or, in nginx, another
 * worker through a pre-fork socket pair).
 */
ngx_int_t ngx_media_ipc_pair_create(ngx_media_ipc_endpoint_t **local,
    ngx_media_ipc_endpoint_t **peer, ngx_log_t *log);
ngx_int_t ngx_media_ipc_adopt(int fd, ngx_media_ipc_endpoint_t **endpoint,
    ngx_log_t *log);
int ngx_media_ipc_fd(const ngx_media_ipc_endpoint_t *endpoint);
void ngx_media_ipc_close(ngx_media_ipc_endpoint_t *endpoint);

/*
 * Sends one message.  Payloads larger than the transport bound are split into
 * ordered chunks automatically.  Returns NGX_AGAIN when the peer is not
 * keeping up, so the caller can drop to the next sync boundary instead of
 * queueing without bound (goal doc 34 item 11).
 */
ngx_int_t ngx_media_ipc_send(ngx_media_ipc_endpoint_t *endpoint,
    const ngx_media_ipc_header_t *header, ngx_media_buf_t *payload,
    size_t offset, size_t length);

/* sends a header-only message (open, close, tracks, health, switch, eof) */
ngx_int_t ngx_media_ipc_send_header(ngx_media_ipc_endpoint_t *endpoint,
    const ngx_media_ipc_header_t *header);

/* receives at most one message; NGX_AGAIN when nothing is pending */
ngx_int_t ngx_media_ipc_recv(ngx_media_ipc_endpoint_t *endpoint,
    ngx_media_ipc_message_t *message);

void ngx_media_ipc_message_release(ngx_media_ipc_message_t *message);

/*
 * Reassembles chunked frames.  Feed every received message here: NGX_AGAIN
 * means more chunks are needed, NGX_OK means the whole frame is available in
 * frame->payload (the caller consumes it and calls reset), and NGX_ERROR means
 * the sequence was inconsistent and the frame was discarded.
 */
typedef struct {
    ngx_media_ipc_header_t  header;
    ngx_media_buf_t        *payload;
    size_t                  capacity;
    size_t                  received;
    unsigned                active:1;
} ngx_media_ipc_frame_t;

ngx_int_t ngx_media_ipc_frame_feed(ngx_media_ipc_frame_t *frame,
    const ngx_media_ipc_message_t *message);
void ngx_media_ipc_frame_reset(ngx_media_ipc_frame_t *frame);

#endif /* NGX_MEDIA_IPC_H */

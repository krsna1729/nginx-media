#ifndef NGX_MEDIA_GRAPH_H
#define NGX_MEDIA_GRAPH_H

#include "ngx_media.h"
#include "ngx_media_ipc.h"
#include "ngx_media_registry.h"

/*
 * Replicated graph control plane (goal doc 22, 23).
 *
 * A program has exactly one owner worker, and the owner holds the mutable
 * program state: sources, selector, timeline, program feed and outputs.  That
 * is what the owner gate in the runtime tick enforces, and it does not
 * change here.
 *
 * The *graph*, on the other hand, is desired state: which streams exist,
 * which sources they have, what those sources ask for.  An operator's control
 * request can land on any worker, and a worker that has never heard of a
 * stream cannot answer a read or accept a source for it - which is why a
 * deployment with two workers carried no media at all.  So every accepted
 * mutation is broadcast over the transport that already exists between
 * workers and applied to every worker's registry.
 *
 * What a replica is, and what it is not:
 *
 *   - it is a *read* of the graph: streams, sources, their desired fields and
 *     their revisions are the same on every worker, so any worker can answer
 *     any read;
 *   - it is not a second program.  A transport is materialised only on the
 *     worker that owns the stream: a replica registers a file source as a
 *     source with no reader, and the owner opens it.  Otherwise four workers
 *     would read the same file, fetch the same origin or watch the same
 *     directory, and the program - not just the graph - would be replicated.
 *     The owner publishes its progress in the shared directory, and a replica
 *     reports that, so "how far has this program got" has one answer.
 *
 * This is a bounded, best-effort control plane, not distributed consensus
 * (goal doc 23).  A mutation is applied locally, then offered to every peer
 * with a non-blocking send.  If a peer is behind, the encoded operation is
 * retained in a bounded repair journal and retried from the runtime tick until
 * every currently connected peer has accepted it.  A journal overflow is
 * logged and remains a bounded loss; the controller's desired document is the
 * recovery path after a worker or a master is gone.
 *
 * A replica that misses an operation stays behind until the repair retry or
 * until desired state is replayed through the API - which is safe because every
 * create is idempotent, and re-creating is how a controller heals a worker
 * anyway (goal doc 25).
 *
 * Ordering and idempotency come from the object revisions the API already
 * assigns: an operation carries the stream revision it produces, and a
 * replica applies it only when it is not older than what that stream has
 * already seen.  Delivery between two workers is ordered by the transport
 * itself (SOCK_SEQPACKET), so reordering can only come from two workers
 * mutating the same stream at once; the revision then makes the loser a
 * no-op instead of a duplicate or a torn object, and every object operation is
 * idempotent, so applying one twice changes nothing.
 */
/*
 * The journal is deliberately finite: a blocked peer cannot turn a control
 * plane into an unbounded allocator.  Each entry owns one immutable IPC
 * payload reference until all addressed peers take it.
 */
#define NGX_MEDIA_GRAPH_REPAIR_CAPACITY 256
#define NGX_MEDIA_GRAPH_REPAIR_PER_TICK  4

/* operations on the wire; the numbers are the protocol, not an enum */
#define NGX_MEDIA_GRAPH_STREAM_SET      1   /* create a stream or update it */
#define NGX_MEDIA_GRAPH_STREAM_DELETE   2
#define NGX_MEDIA_GRAPH_SOURCE_SET      3   /* create a source or update it */
#define NGX_MEDIA_GRAPH_SOURCE_DELETE   4

/*
 * Bounds on what a replica will accept from a peer.  A stream name is a URL
 * segment and a source path is a file, a directory or a URL; both are checked
 * before anything is allocated, so a malformed or hostile message cannot ask a
 * worker for an unbounded string.
 */
#define NGX_MEDIA_GRAPH_MAX_NAME      256
#define NGX_MEDIA_GRAPH_MAX_PATH      4096

/*
 * The fixed part of an operation, followed by its strings in field order:
 * application, name, source id, path, trust anchor.  Native byte order, like
 * every other payload on this transport: it never leaves the host.
 */
typedef struct {
    uint32_t   kind;              /* NGX_MEDIA_GRAPH_* */
    uint32_t   flags;             /* reserved, senders write zero */
    uint64_t   revision;          /* the stream's revision after the change */
    uint64_t   source_revision;   /* the source's revision, source operations */
    uint32_t   type;              /* source type, source operations */
    uint32_t   priority;          /* source priority, source operations */
    uint32_t   enabled;           /* source desired state, source operations */
    uint32_t   failure_timeout;   /* ms, stream operations */
    uint32_t   recovery_timeout;  /* ms, stream operations */
    uint32_t   application_len;
    uint32_t   name_len;
    uint32_t   id_len;            /* source id, zero for a stream operation */
    uint32_t   path_len;          /* file path, URL or directory, else zero */
    uint32_t   ca_file_len;       /* HLS pull trust anchor, else zero */
} ngx_media_graph_wire_t;

/* a decoded operation; the strings point into the message payload */
typedef struct {
    ngx_uint_t  kind;
    uint64_t    revision;
    uint64_t    source_revision;
    ngx_uint_t  type;
    ngx_uint_t  priority;
    ngx_uint_t  enabled;
    ngx_uint_t  failure_timeout;
    ngx_uint_t  recovery_timeout;
    ngx_str_t   application;
    ngx_str_t   name;
    ngx_str_t   id;
    ngx_str_t   path;
    ngx_str_t   ca_file;
} ngx_media_graph_op_t;

/*
 * Broadcasts one mutation.  Each is applied locally by the caller before it is
 * offered to the peers, so the caller's own worker is never waiting on the
 * transport.  Returns NGX_OK when the operation reached every peer, NGX_AGAIN
 * when at least one could not take it (the caller's request still succeeds;
 * the failure is logged and counted) and NGX_ERROR when the operation could
 * not be built.
 */
ngx_int_t ngx_media_graph_stream_set(const ngx_media_stream_t *stream);
ngx_int_t ngx_media_graph_stream_delete(const ngx_str_t *application,
    const ngx_str_t *name, uint64_t revision);
ngx_int_t ngx_media_graph_source_set(const ngx_media_stream_t *stream,
    const ngx_media_source_t *source, const ngx_str_t *path,
    const ngx_str_t *ca_file);
ngx_int_t ngx_media_graph_source_delete(const ngx_media_stream_t *stream,
    const ngx_str_t *id, uint64_t revision);

/* applies one received operation to this worker's registry */
ngx_int_t ngx_media_graph_apply(const ngx_media_ipc_header_t *header,
    ngx_media_buf_t *payload);

/* retries bounded graph operations that a peer could not accept earlier */
void ngx_media_graph_repair_tick(ngx_log_t *log);

/*
 * Registers a desired source on this worker's stream: the reader on the
 * stream's owner (open the file, watch the directory, pull the origin), a
 * plain label everywhere else.  Shared by the API request that creates a
 * source and by the replica that applies it.
 */
/*
 * log is kept by the reader this may open, which outlives the request that
 * asked for it: pass the worker's log, never a connection's.
 */
ngx_media_source_t *ngx_media_graph_source_open(ngx_media_stream_t *stream,
    const ngx_str_t *id, ngx_uint_t type, ngx_uint_t priority,
    const ngx_str_t *path, const ngx_str_t *ca_file, ngx_log_t *log);

/* true when this worker drives the program of application/name */
ngx_uint_t ngx_media_graph_owns(const ngx_str_t *application,
    const ngx_str_t *name);

#endif /* NGX_MEDIA_GRAPH_H */

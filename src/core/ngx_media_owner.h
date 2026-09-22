#ifndef NGX_MEDIA_OWNER_H
#define NGX_MEDIA_OWNER_H

#include "ngx_media.h"

/*
 * Program ownership (goal doc 22).
 *
 * A logical program has exactly one owner worker for its mutable state:
 * sources, selector, timeline, program feed, HLS state and program recording.
 * Ownership is derived deterministically from application/stream, so every
 * worker computes the same answer without coordination, and the shared
 * metadata directory only carries small bookkeeping (owner slot and pid,
 * generation, state, heartbeat) - never mutable media state.
 *
 * Transport sockets may be owned by a different worker; that case is handled
 * by the bounded internal routing escape hatch (goal doc 23).
 */

/*
 * FNV-1a over "application/stream": stable across processes and builds.
 *
 * 64 bits, not 32.  The value is the identity of a stream everywhere the
 * workers address one - the owner directory's records, the routed slots that
 * carry a publisher's media to its owner, the tombstones that remember a
 * deletion - so two distinct streams that hashed alike would be one stream to
 * the deployment: a frame routed to the wrong program, an owner record shared
 * by two programs, a delete that removed both.  32 bits collide at a few tens
 * of thousands of streams by the birthday bound; 64 does not collide in
 * practice.
 */
uint64_t ngx_media_owner_hash(const ngx_str_t *application,
    const ngx_str_t *stream);

/* the worker slot that owns this stream, given the worker count */
ngx_uint_t ngx_media_owner_slot(uint64_t hash, ngx_uint_t workers);

/* the configured worker count (1 when there is no configuration) */
ngx_uint_t ngx_media_owner_worker_count(ngx_cycle_t *cycle);

/* the slot that owns a stream under the configured worker count */
ngx_uint_t ngx_media_owner_for(ngx_cycle_t *cycle, uint64_t hash);

/* the state a worker publishes about a stream it owns */
#define NGX_MEDIA_OWNER_STATE_FREE      0
#define NGX_MEDIA_OWNER_STATE_OWNED     1
#define NGX_MEDIA_OWNER_STATE_DELETED   2
typedef struct {
    uint64_t    hash;           /* stream identity this record describes */
    uint32_t    slot;           /* owner worker slot */
    int32_t     pid;            /* owner process id */
    uint64_t    generation;     /* program generation the owner is serving */
    uint32_t    state;          /* NGX_MEDIA_OWNER_STATE_* */
    uint32_t    sources;        /* sources the owner knows about */
    uint64_t    heartbeat;      /* last update, milliseconds */
    uint64_t    frames;         /* frames published into the program */
} ngx_media_owner_record_t;

#endif /* NGX_MEDIA_OWNER_H */

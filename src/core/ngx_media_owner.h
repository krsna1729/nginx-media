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

/* FNV-1a over "application/stream": stable across processes and builds */
uint32_t ngx_media_owner_hash(const ngx_str_t *application,
    const ngx_str_t *stream);

/* the worker slot that owns this stream, given the worker count */
ngx_uint_t ngx_media_owner_slot(uint32_t hash, ngx_uint_t workers);

/* the configured worker count (1 when there is no configuration) */
ngx_uint_t ngx_media_owner_worker_count(ngx_cycle_t *cycle);

/* the slot that owns a stream under the configured worker count */
ngx_uint_t ngx_media_owner_for(ngx_cycle_t *cycle, uint32_t hash);

/* the state a worker publishes about a stream it owns */
#define NGX_MEDIA_OWNER_STATE_FREE      0
#define NGX_MEDIA_OWNER_STATE_OWNED     1

typedef struct {
    uint32_t    hash;           /* stream hash this record describes */
    uint32_t    slot;           /* owner worker slot */
    int32_t     pid;            /* owner process id */
    uint64_t    generation;     /* program generation the owner is serving */
    uint32_t    state;          /* NGX_MEDIA_OWNER_STATE_* */
    uint32_t    sources;        /* sources the owner knows about */
    uint64_t    heartbeat;      /* last update, milliseconds */
    uint64_t    frames;         /* frames published into the program */
} ngx_media_owner_record_t;

#endif /* NGX_MEDIA_OWNER_H */

#ifndef NGX_MEDIA_TOMBSTONE_H
#define NGX_MEDIA_TOMBSTONE_H

#include "ngx_media_platform.h"

/*
 * Deleted identities, as the replication guard remembers them.
 *
 * A graph operation that was already in flight when a stream was deleted must
 * not bring it back: without this, an operation that reaches a replica after
 * the delete finds no stream, so it creates one, and the deployment has a
 * stream the operator removed.  The revision is what decides - an operation
 * the deletion has already moved past is refused, and a newer one, a create
 * that legitimately follows the delete, is applied.
 *
 * The ring is bounded, so the window covers the operations that can plausibly
 * still be in flight rather than every deletion ever made: an operation older
 * than the window is the same best-effort case as an operation a worker never
 * received.  Only the hash and the revision are kept.  Names would be a copy
 * per deletion, which grows with churn, and the operation that is refused is
 * logged with the names it carries.
 */

#define NGX_MEDIA_TOMBSTONES  256

typedef struct {
    uint64_t  hash;
    uint64_t  revision;
} ngx_media_tombstone_t;

typedef struct {
    ngx_media_tombstone_t  slots[NGX_MEDIA_TOMBSTONES];
    ngx_uint_t             next;    /* the slot the next deletion reuses */
    ngx_uint_t             count;   /* slots in use, up to NGX_MEDIA_TOMBSTONES */
} ngx_media_tombstone_ring_t;

void ngx_media_tombstone_init(ngx_media_tombstone_ring_t *ring);

/* remembers a deletion: nothing at this revision or below may recreate it */
void ngx_media_tombstone_add(ngx_media_tombstone_ring_t *ring, uint64_t hash,
    uint64_t revision);

/* 1 when an operation at this revision is not newer than a recorded deletion */
ngx_uint_t ngx_media_tombstone_has(const ngx_media_tombstone_ring_t *ring,
    uint64_t hash, uint64_t revision);

#endif /* NGX_MEDIA_TOMBSTONE_H */

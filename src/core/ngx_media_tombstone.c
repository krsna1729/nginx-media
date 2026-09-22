#include "ngx_media_tombstone.h"

void
ngx_media_tombstone_init(ngx_media_tombstone_ring_t *ring)
{
    if (ring == NULL) {
        return;
    }

    ngx_memzero(ring, sizeof(ngx_media_tombstone_ring_t));
}

void
ngx_media_tombstone_add(ngx_media_tombstone_ring_t *ring, uint64_t hash,
    uint64_t revision)
{
    if (ring == NULL) {
        return;
    }

    ring->slots[ring->next].hash = hash;
    ring->slots[ring->next].revision = revision;

    ring->next = (ring->next + 1) % NGX_MEDIA_TOMBSTONES;

    if (ring->count < NGX_MEDIA_TOMBSTONES) {
        ring->count++;
    }
}

ngx_uint_t
ngx_media_tombstone_has(const ngx_media_tombstone_ring_t *ring, uint64_t hash,
    uint64_t revision)
{
    ngx_uint_t  i;

    if (ring == NULL) {
        return 0;
    }

    for (i = 0; i < ring->count; i++) {

        if (ring->slots[i].hash != hash) {
            continue;
        }

        /*
         * One identity can have been deleted more than once - a stream that
         * was created and removed again - so every entry is considered, and
         * the operation has to be newer than all of them to be applied.
         */
        if (revision <= ring->slots[i].revision) {
            return 1;
        }
    }

    return 0;
}

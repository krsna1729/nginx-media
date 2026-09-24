#include "ngx_media_srt_output_queue.h"

void
ngx_media_srt_queue_init(ngx_media_srt_queue_t *q, ngx_uint_t capacity,
    size_t max_bytes)
{
    if (q == NULL) {
        return;
    }

    ngx_memzero(q, sizeof(ngx_media_srt_queue_t));

    q->capacity = (capacity > 0 && capacity <= NGX_MEDIA_SRT_QUEUE_MAX_UNITS)
                      ? capacity
                      : NGX_MEDIA_SRT_QUEUE_MAX_UNITS;
    q->max_bytes = max_bytes;
}

void
ngx_media_srt_queue_destroy(ngx_media_srt_queue_t *q)
{
    ngx_uint_t  i;

    if (q == NULL) {
        return;
    }

    for (i = 0; i < q->capacity; i++) {

        if (q->units[i].burst != NULL) {
            ngx_media_buf_unref(q->units[i].burst);
            q->units[i].burst = NULL;
        }
    }

    ngx_memzero(q, sizeof(ngx_media_srt_queue_t));
}

/* releases the oldest retained unit */
static void
ngx_media_srt_queue_evict(ngx_media_srt_queue_t *q)
{
    ngx_media_srt_unit_t  *unit;
    ngx_uint_t             index;

    if (q->tail >= q->head) {
        return;
    }

    index = (ngx_uint_t) (q->tail % q->capacity);
    unit = &q->units[index];

    if (unit->burst != NULL) {
        q->bytes -= unit->len;
        ngx_media_buf_unref(unit->burst);
        unit->burst = NULL;
    }

    unit->len = 0;

    q->tail++;
    q->dropped++;
    q->resync = 1;
}

ngx_int_t
ngx_media_srt_queue_push(ngx_media_srt_queue_t *q, ngx_media_buf_t *burst,
    size_t len, ngx_uint_t keyframe)
{
    ngx_media_srt_unit_t  *unit;
    ngx_uint_t             index;

    if (q == NULL || burst == NULL || len == 0) {
        return NGX_ERROR;
    }

    /*
     * A burst larger than the whole byte ceiling can never be delivered: it
     * is dropped instead of evicting everything else for nothing.
     */
    if (q->max_bytes != 0 && len > q->max_bytes) {
        q->dropped++;
        q->resync = 1;

        return NGX_OK;
    }

    while (q->tail < q->head
           && (q->head - q->tail >= q->capacity
               || (q->max_bytes != 0 && q->bytes + len > q->max_bytes)))
    {
        ngx_media_srt_queue_evict(q);
    }

    index = (ngx_uint_t) (q->head % q->capacity);
    unit = &q->units[index];

    if (unit->burst != NULL) {
        /* the ring is full of live units: drop the oldest one */
        q->bytes -= unit->len;
        ngx_media_buf_unref(unit->burst);
        unit->burst = NULL;
        q->dropped++;
        q->resync = 1;
        q->tail++;
    }

    unit->burst = ngx_media_buf_ref(burst);
    unit->len = len;
    unit->sequence = q->head;
    unit->keyframe = keyframe ? 1 : 0;

    q->bytes += len;
    q->head++;
    q->pushed++;

    return NGX_OK;
}

const ngx_media_srt_unit_t *
ngx_media_srt_queue_next(const ngx_media_srt_queue_t *q, uint64_t *cursor)
{
    uint64_t                     sequence;
    const ngx_media_srt_unit_t  *unit;
    ngx_uint_t                   index;

    if (q == NULL || cursor == NULL || q->tail >= q->head) {
        return NULL;
    }

    sequence = (*cursor < q->tail) ? q->tail : *cursor;

    /* after a drop the consumer waits for a sync boundary */
    if (((ngx_media_srt_queue_t *) q)->resync) {

        while (sequence < q->head) {
            index = (ngx_uint_t) (sequence % q->capacity);
            unit = &q->units[index];

            if (unit->burst != NULL && unit->keyframe) {
                break;
            }

            sequence++;
        }

        if (sequence >= q->head) {
            return NULL;
        }
    }

    if (sequence >= q->head) {
        return NULL;
    }

    index = (ngx_uint_t) (sequence % q->capacity);
    unit = &q->units[index];

    if (unit->burst == NULL) {
        return NULL;
    }

    return unit;
}

void
ngx_media_srt_queue_advance(ngx_media_srt_queue_t *q, uint64_t *cursor,
    uint64_t sequence)
{
    ngx_media_srt_unit_t  *unit;
    ngx_uint_t             index;
    uint64_t               next;

    if (q == NULL || cursor == NULL) {
        return;
    }

    next = sequence + 1;

    /* A resync can skip retained units before the next keyframe. */
    if (q->resync && sequence >= q->tail) {
        q->dropped += sequence - q->tail;
        q->resync = 0;
    }

    *cursor = next;

    /* Retire every unit already accepted by the transport. */
    while (q->tail < next && q->tail < q->head) {
        index = (ngx_uint_t) (q->tail % q->capacity);
        unit = &q->units[index];

        if (unit->burst != NULL) {
            q->bytes -= unit->len;
            ngx_media_buf_unref(unit->burst);
            unit->burst = NULL;
        }

        unit->len = 0;
        q->tail++;
    }
}

void
ngx_media_srt_queue_resync(ngx_media_srt_queue_t *q)
{
    if (q != NULL) {
        q->resync = 1;
    }
}

uint64_t
ngx_media_srt_queue_head(const ngx_media_srt_queue_t *q)
{
    return (q != NULL) ? q->head : 0;
}

#ifndef NGX_MEDIA_SRT_OUTPUT_QUEUE_H
#define NGX_MEDIA_SRT_OUTPUT_QUEUE_H

#include "ngx_media.h"
#include "ngx_media_buffer.h"

/*
 * Bounded subscriber queue for one SRT destination (goal doc 16, 34 items 11
 * and 12).
 *
 * A unit references an already prepared transport burst, so a destination
 * never copies media: the preparation is shared by HLS, recording and every
 * SRT destination of the same program.  The queue has hard unit and byte
 * ceilings; when a slow receiver falls behind, whole bursts are dropped and
 * the consumer resumes at the next unit that starts at a sync boundary, so it
 * never starts mid-GOP.
 */

#define NGX_MEDIA_SRT_QUEUE_MAX_UNITS 256

typedef struct {
    ngx_media_buf_t  *burst;      /* prepared burst, one reference */
    size_t            len;        /* bytes of the burst */
    uint64_t          sequence;
    ngx_msec_t        enqueue_msec; /* monotonic insertion time */
    unsigned          keyframe:1; /* burst starts at a sync boundary */
} ngx_media_srt_unit_t;

typedef struct {
    ngx_media_srt_unit_t  units[NGX_MEDIA_SRT_QUEUE_MAX_UNITS];
    ngx_uint_t            capacity;
    size_t                max_bytes;

    uint64_t              head;       /* next sequence to write */
    uint64_t              tail;       /* oldest retained sequence */
    size_t                bytes;
    uint64_t              dropped;    /* bursts dropped under overrun */
    uint64_t              pushed;
    unsigned              resync;     /* dropped: resume at a keyframe */
} ngx_media_srt_queue_t;

void ngx_media_srt_queue_init(ngx_media_srt_queue_t *q, ngx_uint_t capacity,
    size_t max_bytes);
void ngx_media_srt_queue_destroy(ngx_media_srt_queue_t *q);

/* takes a reference on the burst; drops whole bursts on overrun */
ngx_int_t ngx_media_srt_queue_push(ngx_media_srt_queue_t *q,
    ngx_media_buf_t *burst, size_t len, ngx_uint_t keyframe,
    ngx_msec_t enqueue_msec);

/*
 * The next unit for a consumer positioned at *cursor, or NULL when nothing is
 * available.  Dropped units are skipped, and after a drop the consumer
 * resumes at the next sync boundary.  The cursor is not advanced: the caller
 * advances it only after the unit was delivered, so a failed send does not
 * lose media.
 */
const ngx_media_srt_unit_t *ngx_media_srt_queue_next(
    const ngx_media_srt_queue_t *q, uint64_t *cursor);

/* advances the cursor past a delivered unit */
void ngx_media_srt_queue_advance(ngx_media_srt_queue_t *q, uint64_t *cursor,
    uint64_t sequence);

/* asks a consumer to resume at the next sync boundary */
void ngx_media_srt_queue_resync(ngx_media_srt_queue_t *q);

uint64_t ngx_media_srt_queue_head(const ngx_media_srt_queue_t *q);

#endif /* NGX_MEDIA_SRT_OUTPUT_QUEUE_H */

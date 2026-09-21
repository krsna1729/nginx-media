#include "ngx_media_feed.h"

static ngx_uint_t ngx_media_feed_pow2(ngx_uint_t n);
static void ngx_media_feed_evict(ngx_media_feed_t *feed);
static void ngx_media_feed_prune(ngx_media_feed_t *feed, ngx_msec_t now,
    size_t incoming);

static ngx_uint_t
ngx_media_feed_pow2(ngx_uint_t n)
{
    ngx_uint_t  p;

    for (p = 1; p < n; p <<= 1) {
        if (p > ((ngx_uint_t) -1 >> 1)) {
            return 0;
        }
    }

    return p;
}

static void
ngx_media_feed_evict(ngx_media_feed_t *feed)
{
    ngx_media_feed_slot_t  *slot;

    slot = &feed->slots[feed->tail & (feed->capacity - 1)];

    if (slot->frame.payload != NULL) {
        feed->bytes -= slot->frame.payload->len;
    }

    ngx_media_frame_release(&slot->frame);
    slot->publish_time = 0;

    feed->tail++;
}

/*
 * Evict the oldest retained units until the incoming frame fits within the
 * unit, byte and age ceilings.  A unit that is larger than max_bytes is never
 * split; it is retained alone and evicted by the next publish, so retained
 * bytes are bounded by max(max_bytes, largest unit).
 */
static void
ngx_media_feed_prune(ngx_media_feed_t *feed, ngx_msec_t now, size_t incoming)
{
    ngx_media_feed_slot_t  *oldest;

    while (feed->head > feed->tail) {

        if (feed->head - feed->tail >= feed->max_units) {
            ngx_media_feed_evict(feed);
            continue;
        }

        if (feed->max_bytes != 0
            && feed->bytes + incoming > feed->max_bytes)
        {
            ngx_media_feed_evict(feed);
            continue;
        }

        if (feed->max_age != 0) {
            oldest = &feed->slots[feed->tail & (feed->capacity - 1)];

            if (now - oldest->publish_time > feed->max_age) {
                ngx_media_feed_evict(feed);
                continue;
            }
        }

        break;
    }
}

ngx_int_t
ngx_media_feed_init(ngx_media_feed_t *feed, const ngx_media_feed_conf_t *conf,
    ngx_log_t *log)
{
    if (feed == NULL || conf == NULL || conf->max_units == 0) {
        return NGX_ERROR;
    }

    ngx_memzero(feed, sizeof(ngx_media_feed_t));

    feed->capacity = ngx_media_feed_pow2(conf->max_units);
    if (feed->capacity == 0) {
        return NGX_ERROR;
    }

    if (feed->capacity > (size_t) -1 / sizeof(ngx_media_feed_slot_t)) {
        return NGX_ERROR;
    }

    feed->slots = ngx_alloc(feed->capacity * sizeof(ngx_media_feed_slot_t),
                            log);
    if (feed->slots == NULL) {
        feed->capacity = 0;
        return NGX_ERROR;
    }

    ngx_memzero(feed->slots,
                feed->capacity * sizeof(ngx_media_feed_slot_t));

    feed->max_units = conf->max_units;
    feed->max_bytes = conf->max_bytes;
    feed->max_age = conf->max_age;
    feed->generation = 1;

    return NGX_OK;
}

void
ngx_media_feed_destroy(ngx_media_feed_t *feed)
{
    if (feed == NULL || feed->slots == NULL) {
        return;
    }

    while (feed->head > feed->tail) {
        ngx_media_feed_evict(feed);
    }

    ngx_free(feed->slots);

    ngx_memzero(feed, sizeof(ngx_media_feed_t));
}

ngx_int_t
ngx_media_feed_publish(ngx_media_feed_t *feed, const ngx_media_frame_t *frame,
    ngx_msec_t now)
{
    ngx_media_feed_slot_t  *slot;
    size_t                  len;

    if (feed == NULL || feed->slots == NULL || frame == NULL) {
        return NGX_ERROR;
    }

    len = (frame->payload != NULL) ? frame->payload->len : 0;

    ngx_media_feed_prune(feed, now, len);

    slot = &feed->slots[feed->head & (feed->capacity - 1)];

    if (slot->frame.payload != NULL) {
        /* ring accounting invariant: the slot must have been evicted first */
        return NGX_ERROR;
    }

    if (ngx_media_frame_copy(&slot->frame, frame) != NGX_OK) {
        return NGX_ERROR;
    }

    slot->publish_time = now;
    feed->bytes += len;

    if (frame->keyframe) {
        feed->has_keyframe = 1;
        feed->last_keyframe = feed->head;
    }

    feed->head++;

    return NGX_OK;
}

static void
ngx_media_feed_hist_record(ngx_media_feed_hist_t *hist, ngx_msec_t delay)
{
    ngx_uint_t  bucket = 0;
    ngx_msec_t  bound;

    /*
     * A delay far beyond any plausible one means the clock moved, not that
     * the fanout stalled for a minute.  Recording it in the top bucket would
     * make the percentile useless, so it is counted at the ceiling.
     */
    if (delay > 60000) {
        delay = 60000;
    }

    for (bound = 1; bound < delay && bucket < NGX_MEDIA_FEED_HIST_BUCKETS - 1;
         bound <<= 1)
    {
        bucket++;
    }

    hist->buckets[bucket]++;
    hist->count++;

    if (delay > hist->max) {
        hist->max = delay;
    }
}

ngx_uint_t
ngx_media_feed_read(ngx_media_feed_t *feed, ngx_media_cursor_t *cursor,
    ngx_uint_t max_units, size_t max_bytes, ngx_msec_t now,
    ngx_media_frame_t *out, ngx_uint_t *out_count)
{
    uint64_t                     seq;
    size_t                       bytes;
    ngx_uint_t                   count;
    const ngx_media_feed_slot_t *slot;
    size_t                       len;

    if (feed == NULL || feed->slots == NULL || cursor == NULL || out == NULL
        || out_count == NULL)
    {
        return NGX_MEDIA_FEED_ERROR;
    }

    *out_count = 0;

    if (cursor->generation != feed->generation) {
        return NGX_MEDIA_FEED_GENERATION_MISMATCH;
    }

    if (cursor->next_sequence < feed->tail) {
        return NGX_MEDIA_FEED_OVERRUN;
    }

    if (cursor->next_sequence > feed->head) {
        /* a cursor ahead of the producer belongs to a previous ring */
        return NGX_MEDIA_FEED_GENERATION_MISMATCH;
    }

    seq = cursor->next_sequence;
    count = 0;
    bytes = 0;

    while (count < max_units && seq < feed->head) {

        slot = &feed->slots[seq & (feed->capacity - 1)];
        len = (slot->frame.payload != NULL) ? slot->frame.payload->len : 0;

        if (count > 0 && max_bytes != 0 && bytes + len > max_bytes) {
            break;
        }

        out[count] = slot->frame;
        ngx_media_buf_ref(slot->frame.payload);

        if (now >= slot->publish_time) {
            ngx_media_feed_hist_record(&feed->fanout, now - slot->publish_time);
        }

        bytes += len;
        count++;
        seq++;
    }

    if (count == 0) {
        return NGX_MEDIA_FEED_EMPTY;
    }

    cursor->next_sequence = seq;
    *out_count = count;

    return NGX_MEDIA_FEED_BATCH;
}

void
ngx_media_feed_release(ngx_media_frame_t *frames, ngx_uint_t count)
{
    ngx_uint_t  i;

    if (frames == NULL) {
        return;
    }

    for (i = 0; i < count; i++) {
        ngx_media_frame_release(&frames[i]);
    }
}

void
ngx_media_feed_discontinuity(ngx_media_feed_t *feed)
{
    if (feed == NULL) {
        return;
    }

    feed->generation++;
}

void
ngx_media_feed_cursor_init(const ngx_media_feed_t *feed,
    ngx_media_cursor_t *cursor)
{
    if (feed == NULL || cursor == NULL) {
        return;
    }

    cursor->generation = feed->generation;
    cursor->next_sequence = feed->head;
}

ngx_int_t
ngx_media_feed_resync(const ngx_media_feed_t *feed, ngx_media_cursor_t *cursor,
    ngx_uint_t mode)
{
    uint64_t  seq;

    if (feed == NULL || feed->slots == NULL || cursor == NULL) {
        return NGX_ERROR;
    }

    switch (mode) {

    case NGX_MEDIA_FEED_RESYNC_LATEST:
        seq = feed->head;
        break;

    case NGX_MEDIA_FEED_RESYNC_KEYFRAME:
        seq = feed->head;

        if (feed->has_keyframe
            && feed->last_keyframe >= feed->tail
            && feed->last_keyframe < feed->head)
        {
            seq = feed->last_keyframe;
        }

        break;

    default:
        return NGX_ERROR;
    }

    cursor->generation = feed->generation;
    cursor->next_sequence = seq;

    return NGX_OK;
}

uint64_t
ngx_media_feed_generation(const ngx_media_feed_t *feed)
{
    return (feed != NULL) ? feed->generation : 0;
}

uint64_t
ngx_media_feed_head(const ngx_media_feed_t *feed)
{
    return (feed != NULL) ? feed->head : 0;
}

uint64_t
ngx_media_feed_tail(const ngx_media_feed_t *feed)
{
    return (feed != NULL) ? feed->tail : 0;
}

ngx_uint_t
ngx_media_feed_units(const ngx_media_feed_t *feed)
{
    return (feed != NULL) ? (ngx_uint_t) (feed->head - feed->tail) : 0;
}

size_t
ngx_media_feed_bytes(const ngx_media_feed_t *feed)
{
    return (feed != NULL) ? feed->bytes : 0;
}

uint64_t
ngx_media_feed_last_keyframe(const ngx_media_feed_t *feed)
{
    if (feed == NULL || !feed->has_keyframe || feed->last_keyframe < feed->tail
        || feed->last_keyframe >= feed->head)
    {
        return NGX_MEDIA_FEED_NO_KEYFRAME;
    }

    return feed->last_keyframe;
}

ngx_msec_t
ngx_media_feed_fanout_percentile(const ngx_media_feed_t *feed,
    ngx_uint_t permille)
{
    uint64_t    target, seen;
    ngx_uint_t  i;
    ngx_msec_t  bound;

    if (feed == NULL || feed->fanout.count == 0 || permille == 0) {
        return 0;
    }

    if (permille > 1000) {
        permille = 1000;
    }

    target = (feed->fanout.count * permille + 999) / 1000;
    seen = 0;

    /*
     * The answer is the upper bound of the bucket the percentile falls in,
     * which is the conservative reading: the true value is somewhere inside
     * that bucket and this never reports better than reality.
     */
    for (i = 0; i < NGX_MEDIA_FEED_HIST_BUCKETS; i++) {

        seen += feed->fanout.buckets[i];

        if (seen >= target) {
            bound = 1;

            for (ngx_uint_t b = 0; b < i; b++) {
                bound <<= 1;
            }

            return bound;
        }
    }

    return feed->fanout.max;
}

uint64_t
ngx_media_feed_fanout_count(const ngx_media_feed_t *feed)
{
    return (feed != NULL) ? feed->fanout.count : 0;
}

uint64_t
ngx_media_feed_fanout_max(const ngx_media_feed_t *feed)
{
    return (feed != NULL) ? feed->fanout.max : 0;
}

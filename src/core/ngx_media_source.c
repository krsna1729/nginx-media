#include "ngx_media_source.h"

static ngx_uint_t ngx_media_source_pow2(ngx_uint_t n);

static ngx_uint_t
ngx_media_source_pow2(ngx_uint_t n)
{
    ngx_uint_t  p;

    for (p = 1; p < n; p <<= 1) {
        if (p > ((ngx_uint_t) -1 >> 1)) {
            return 0;
        }
    }

    return p;
}

ngx_int_t
ngx_media_source_preroll_init(ngx_media_source_t *source, ngx_uint_t max_units,
    size_t max_bytes, ngx_log_t *log)
{
    ngx_media_preroll_t  *preroll;

    if (source == NULL || max_units == 0 || max_bytes == 0) {
        return NGX_ERROR;
    }

    preroll = &source->preroll;

    ngx_memzero(preroll, sizeof(ngx_media_preroll_t));

    preroll->capacity = ngx_media_source_pow2(max_units + 1);
    if (preroll->capacity == 0
        || preroll->capacity > (size_t) -1 / sizeof(ngx_media_frame_t))
    {
        return NGX_ERROR;
    }

    preroll->units = ngx_alloc(
        preroll->capacity * sizeof(ngx_media_frame_t), log);
    if (preroll->units == NULL) {
        preroll->capacity = 0;
        return NGX_ERROR;
    }

    ngx_memzero(preroll->units,
                preroll->capacity * sizeof(ngx_media_frame_t));

    preroll->max_units = max_units;
    preroll->max_bytes = max_bytes;

    return NGX_OK;
}

void
ngx_media_source_preroll_destroy(ngx_media_source_t *source)
{
    if (source == NULL || source->preroll.units == NULL) {
        return;
    }

    ngx_media_source_preroll_reset(source);

    ngx_free(source->preroll.units);

    ngx_memzero(&source->preroll, sizeof(ngx_media_preroll_t));
}

void
ngx_media_source_preroll_reset(ngx_media_source_t *source)
{
    ngx_media_preroll_t  *preroll = &source->preroll;

    while (preroll->tail < preroll->head) {
        ngx_media_frame_release(
            &preroll->units[preroll->tail & (preroll->capacity - 1)]);
        preroll->tail++;
    }

    preroll->head = 0;
    preroll->tail = 0;
    preroll->bytes = 0;
    preroll->have_boundary = 0;
}

ngx_int_t
ngx_media_source_preroll_push(ngx_media_source_t *source,
    const ngx_media_frame_t *frame)
{
    ngx_media_preroll_t  *preroll = &source->preroll;
    ngx_media_frame_t    *slot;
    size_t                len;

    if (source == NULL || frame == NULL || preroll->units == NULL) {
        return NGX_ERROR;
    }

    len = (frame->payload != NULL) ? frame->payload->len : 0;

    if (frame->media_type == NGX_MEDIA_TYPE_VIDEO) {

        if (frame->keyframe) {
            /* a new keyframe replaces the cached GOP wholesale */
            ngx_media_source_preroll_reset(source);
            preroll->have_boundary = 1;

        } else if (!preroll->have_boundary) {
            /* not decodable from here */
            return NGX_OK;
        }

    } else if (!preroll->have_boundary) {

        if (source->has_video) {
            /* wait for the video keyframe that opens a decodable GOP */
            return NGX_OK;
        }

        /* explicit rule for sources without a video track */
        preroll->have_boundary = 1;
    }

    if (preroll->head - preroll->tail >= preroll->capacity) {
        /* defensive: the ring must always have room for one more unit */
        preroll->overflows++;
        ngx_media_source_preroll_reset(source);
        return NGX_AGAIN;
    }

    slot = &preroll->units[preroll->head & (preroll->capacity - 1)];

    if (ngx_media_frame_copy(slot, frame) != NGX_OK) {
        return NGX_ERROR;
    }

    preroll->head++;
    preroll->bytes += len;

    if (preroll->head - preroll->tail > preroll->max_units
        || preroll->bytes > preroll->max_bytes)
    {
        /*
         * Overflow clears the cache: an incomplete GOP must never be kept
         * for a later promotion.
         */
        preroll->overflows++;
        ngx_media_source_preroll_reset(source);
        return NGX_AGAIN;
    }

    return NGX_OK;
}

void
ngx_media_source_preroll_replay(ngx_media_source_t *source,
    ngx_media_preroll_replay_pt cb, void *ctx)
{
    ngx_media_preroll_t  *preroll;
    uint64_t              seq;

    if (source == NULL || cb == NULL) {
        return;
    }

    preroll = &source->preroll;

    for (seq = preroll->tail; seq < preroll->head; seq++) {
        (void) cb(ctx, &preroll->units[seq & (preroll->capacity - 1)]);
    }
}

ngx_uint_t
ngx_media_source_preroll_units(const ngx_media_source_t *source)
{
    if (source == NULL || source->preroll.units == NULL) {
        return 0;
    }

    return (ngx_uint_t) (source->preroll.head - source->preroll.tail);
}

size_t
ngx_media_source_preroll_bytes(const ngx_media_source_t *source)
{
    return (source != NULL) ? source->preroll.bytes : 0;
}

ngx_uint_t
ngx_media_source_preroll_ready(const ngx_media_source_t *source)
{
    if (source == NULL || source->preroll.units == NULL) {
        return 0;
    }

    return (source->preroll.have_boundary
            && source->preroll.head > source->preroll.tail) ? 1 : 0;
}

void
ngx_media_source_lease_begin(ngx_media_source_t *source)
{
    if (source != NULL) {
        source->writers++;
    }
}

void
ngx_media_source_lease_end(ngx_media_source_t *source)
{
    if (source != NULL && source->writers > 0) {
        source->writers--;
    }
}

ngx_int_t
ngx_media_source_tracks_set(ngx_media_source_t *source,
    const ngx_media_trackset_t *tracks, ngx_log_t *log)
{
    ngx_media_trackset_t  *copy;
    ngx_uint_t             i;

    if (source == NULL || tracks == NULL) {
        return NGX_ERROR;
    }

    ngx_media_source_tracks_destroy(source);

    copy = ngx_alloc(sizeof(ngx_media_trackset_t), log);
    if (copy == NULL) {
        return NGX_ERROR;
    }

    if (ngx_media_trackset_init(copy, tracks->count ? tracks->count : 1, log)
        != NGX_OK)
    {
        ngx_free(copy);
        return NGX_ERROR;
    }

    for (i = 0; i < tracks->count; i++) {
        if (ngx_media_trackset_add(copy, &tracks->tracks[i]) < 0) {
            ngx_media_trackset_destroy(copy);
            ngx_free(copy);
            return NGX_ERROR;
        }
    }

    source->tracks = copy;

    return NGX_OK;
}

void
ngx_media_source_tracks_destroy(ngx_media_source_t *source)
{
    if (source == NULL || source->tracks == NULL) {
        return;
    }

    ngx_media_trackset_destroy(source->tracks);
    ngx_free(source->tracks);
    source->tracks = NULL;
}

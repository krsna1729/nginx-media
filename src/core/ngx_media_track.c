#include "ngx_media_track.h"

ngx_int_t
ngx_media_trackset_init(ngx_media_trackset_t *set, ngx_uint_t capacity,
    ngx_log_t *log)
{
    if (set == NULL || capacity == 0) {
        return NGX_ERROR;
    }

    ngx_memzero(set, sizeof(ngx_media_trackset_t));

    if (capacity > (size_t) -1 / sizeof(ngx_media_track_t)) {
        return NGX_ERROR;
    }

    set->tracks = ngx_alloc(capacity * sizeof(ngx_media_track_t), log);
    if (set->tracks == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(set->tracks, capacity * sizeof(ngx_media_track_t));

    set->capacity = capacity;

    return NGX_OK;
}

void
ngx_media_trackset_destroy(ngx_media_trackset_t *set)
{
    ngx_uint_t  i;

    if (set == NULL || set->tracks == NULL) {
        return;
    }

    for (i = 0; i < set->count; i++) {
        ngx_media_buf_unref(set->tracks[i].config);
    }

    ngx_free(set->tracks);

    ngx_memzero(set, sizeof(ngx_media_trackset_t));
}

ngx_int_t
ngx_media_trackset_add(ngx_media_trackset_t *set, const ngx_media_track_t *track)
{
    ngx_uint_t  index;

    if (set == NULL || set->tracks == NULL || track == NULL) {
        return NGX_ERROR;
    }

    if (set->count >= set->capacity) {
        return NGX_ERROR;
    }

    index = set->count;

    set->tracks[index] = *track;
    ngx_media_buf_ref(set->tracks[index].config);

    set->count++;

    return (ngx_int_t) index;
}

const ngx_media_track_t *
ngx_media_trackset_get(const ngx_media_trackset_t *set, ngx_uint_t index)
{
    if (set == NULL || set->tracks == NULL || index >= set->count) {
        return NULL;
    }

    return &set->tracks[index];
}

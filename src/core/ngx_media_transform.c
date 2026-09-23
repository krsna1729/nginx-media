#include "ngx_media_transform.h"

#include <string.h>

static uint64_t
ngx_media_transform_hash_bytes(uint64_t hash, const void *data, size_t len)
{
    const u_char *p = data;
    size_t        i;

    for (i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}

static uint64_t
ngx_media_transform_hash_spec(const ngx_media_transform_spec_t *spec,
    uint64_t implementation)
{
    uint64_t hash;

    hash = UINT64_C(1469598103934665603);
    hash = ngx_media_transform_hash_bytes(hash, &implementation,
                                          sizeof(implementation));
    hash = ngx_media_transform_hash_bytes(hash, &spec->video_codec,
                                          sizeof(spec->video_codec));
    hash = ngx_media_transform_hash_bytes(hash, &spec->audio_codec,
                                          sizeof(spec->audio_codec));
    hash = ngx_media_transform_hash_bytes(hash, &spec->width,
                                          sizeof(spec->width));
    hash = ngx_media_transform_hash_bytes(hash, &spec->height,
                                          sizeof(spec->height));
    hash = ngx_media_transform_hash_bytes(hash, &spec->frame_rate_num,
                                          sizeof(spec->frame_rate_num));
    hash = ngx_media_transform_hash_bytes(hash, &spec->frame_rate_den,
                                          sizeof(spec->frame_rate_den));
    hash = ngx_media_transform_hash_bytes(hash, &spec->video_bitrate,
                                          sizeof(spec->video_bitrate));
    hash = ngx_media_transform_hash_bytes(hash, &spec->audio_bitrate,
                                          sizeof(spec->audio_bitrate));
    hash = ngx_media_transform_hash_bytes(hash, &spec->sample_rate,
                                          sizeof(spec->sample_rate));
    hash = ngx_media_transform_hash_bytes(hash, &spec->channels,
                                          sizeof(spec->channels));
    hash = ngx_media_transform_hash_bytes(hash, &spec->executor,
                                          sizeof(spec->executor));
    hash = ngx_media_transform_hash_bytes(hash, &spec->profile_version,
                                          sizeof(spec->profile_version));
    hash = ngx_media_transform_hash_bytes(hash, &spec->color_policy,
                                          sizeof(spec->color_policy));
    hash = ngx_media_transform_hash_bytes(hash, &spec->gop_frames,
                                          sizeof(spec->gop_frames));
    hash = ngx_media_transform_hash_bytes(hash, &spec->input_track_count,
                                          sizeof(spec->input_track_count));
    hash = ngx_media_transform_hash_bytes(hash, spec->input_track_ids,
                                          sizeof(spec->input_track_ids));

    return hash;
}

void
ngx_media_logical_track_init(ngx_media_logical_track_t *track,
    ngx_uint_t media_type, const char *language, ngx_uint_t role,
    ngx_uint_t channels, ngx_uint_t sample_rate, uint32_t logical_id)
{
    size_t len;

    if (track == NULL) {
        return;
    }

    ngx_memzero(track, sizeof(*track));

    track->media_type = media_type;
    track->role = role;
    track->channels = channels;
    track->sample_rate = sample_rate;
    track->logical_id = logical_id;

    if (language == NULL) {
        return;
    }

    len = strlen(language);
    if (len >= sizeof(track->language)) {
        len = sizeof(track->language) - 1;
    }

    ngx_memcpy(track->language, language, len);
    track->language[len] = '\0';
}

ngx_int_t
ngx_media_logical_track_equal(const ngx_media_logical_track_t *left,
    const ngx_media_logical_track_t *right)
{
    if (left == NULL || right == NULL) {
        return NGX_ERROR;
    }

    return left->media_type == right->media_type
           && left->role == right->role
           && left->channels == right->channels
           && left->sample_rate == right->sample_rate
           && left->logical_id == right->logical_id
           && ngx_memcmp(left->language, right->language,
                         sizeof(left->language)) == 0
           ? NGX_OK : NGX_DECLINED;
}

ngx_int_t
ngx_media_logical_track_select(const ngx_media_logical_track_t *available,
    ngx_uint_t available_count, const ngx_media_logical_track_t *requested,
    ngx_uint_t requested_count, ngx_uint_t *selected,
    ngx_uint_t *selected_count)
{
    ngx_uint_t  indexes[NGX_MEDIA_TRANSFORM_MAX_JOURNAL];
    ngx_uint_t  i, j, found;

    if ((available_count > 0 && available == NULL)
        || (requested_count > 0 && requested == NULL)
        || (requested_count > NGX_MEDIA_TRANSFORM_MAX_JOURNAL)
        || selected == NULL || selected_count == NULL)
    {
        return NGX_ERROR;
    }

    for (i = 0; i < requested_count; i++) {
        found = 0;

        for (j = 0; j < available_count; j++) {
            if (ngx_media_logical_track_equal(&available[j], &requested[i])
                == NGX_OK)
            {
                found = 1;
                indexes[i] = j;
                break;
            }
        }

        if (!found) {
            return NGX_DECLINED;
        }

        for (j = 0; j < i; j++) {
            if (indexes[j] == indexes[i]) {
                return NGX_ERROR;
            }
        }
    }

    for (i = 0; i < requested_count; i++) {
        selected[i] = indexes[i];
    }

    *selected_count = requested_count;
    return NGX_OK;
}

uint64_t
ngx_media_transform_key_hash(const ngx_media_transform_spec_t *spec,
    uint64_t implementation)
{
    if (spec == NULL) {
        return 0;
    }

    return ngx_media_transform_hash_spec(spec, implementation);
}

void
ngx_media_transform_key_init(ngx_media_transform_key_t *key,
    const ngx_media_transform_spec_t *spec, uint64_t implementation)
{
    if (key == NULL) {
        return;
    }

    ngx_memzero(key, sizeof(*key));

    if (spec == NULL) {
        return;
    }

    key->implementation = implementation;
    key->spec = *spec;
    key->digest = ngx_media_transform_hash_spec(spec, implementation);
}

ngx_int_t
ngx_media_transform_key_equal(const ngx_media_transform_key_t *left,
    const ngx_media_transform_key_t *right)
{
    if (left == NULL || right == NULL) {
        return NGX_ERROR;
    }

    return left->digest == right->digest
           && left->implementation == right->implementation
           && ngx_memcmp(&left->spec, &right->spec, sizeof(left->spec)) == 0
           ? NGX_OK : NGX_DECLINED;
}

ngx_int_t
ngx_media_track_journal_init(ngx_media_track_journal_t *journal,
    uint32_t logical_id, const ngx_media_feed_conf_t *conf, ngx_log_t *log)
{
    if (journal == NULL || conf == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(journal, sizeof(*journal));

    if (ngx_media_feed_init(&journal->feed, conf, log) != NGX_OK) {
        return NGX_ERROR;
    }

    journal->logical_id = logical_id;

    return NGX_OK;
}

void
ngx_media_track_journal_destroy(ngx_media_track_journal_t *journal)
{
    if (journal == NULL) {
        return;
    }

    ngx_media_feed_destroy(&journal->feed);
    ngx_memzero(journal, sizeof(*journal));
}

ngx_int_t
ngx_media_track_journal_publish(ngx_media_track_journal_t *journal,
    const ngx_media_frame_t *frame, ngx_msec_t now)
{
    if (journal == NULL || frame == NULL) {
        return NGX_ERROR;
    }

    return ngx_media_feed_publish(&journal->feed, frame, now);
}

ngx_int_t
ngx_media_prepared_feed_init(ngx_media_prepared_feed_t *feed, uint64_t id,
    uint64_t epoch, const ngx_media_feed_conf_t *conf, ngx_log_t *log)
{
    if (feed == NULL || conf == NULL || id == 0 || epoch == 0) {
        return NGX_ERROR;
    }

    ngx_memzero(feed, sizeof(*feed));

    if (ngx_media_feed_init(&feed->feed, conf, log) != NGX_OK) {
        return NGX_ERROR;
    }

    feed->id = id;
    feed->epoch = epoch;
    feed->active = 1;
    feed->refs = 1;

    return NGX_OK;
}

void
ngx_media_prepared_feed_destroy(ngx_media_prepared_feed_t *feed)
{
    if (feed == NULL) {
        return;
    }

    ngx_media_feed_destroy(&feed->feed);
    ngx_memzero(feed, sizeof(*feed));
}

ngx_int_t
ngx_media_prepared_feed_publish(ngx_media_prepared_feed_t *feed,
    const ngx_media_frame_t *frame, ngx_msec_t now)
{
    if (feed == NULL || !feed->active || frame == NULL) {
        return NGX_ERROR;
    }

    return ngx_media_feed_publish(&feed->feed, frame, now);
}

void
ngx_media_prepared_feed_ref(ngx_media_prepared_feed_t *feed)
{
    if (feed == NULL || !feed->active) {
        return;
    }

    (void) ngx_atomic_fetch_add(&feed->refs, 1);
}

void
ngx_media_prepared_feed_unref(ngx_media_prepared_feed_t *feed)
{
    ngx_atomic_uint_t refs;

    if (feed == NULL) {
        return;
    }

    refs = __atomic_load_n(&feed->refs, __ATOMIC_ACQUIRE);

    while (refs > 0
           && !__atomic_compare_exchange_n(&feed->refs, &refs, refs - 1,
                                           0, __ATOMIC_ACQ_REL,
                                           __ATOMIC_ACQUIRE))
    {
        /* refs is refreshed by compare_exchange on contention. */
    }

    if (refs == 1) {
        feed->active = 0;
    }
}

void
ngx_media_transform_registry_init(ngx_media_transform_registry_t *registry)
{
    if (registry == NULL) {
        return;
    }

    ngx_memzero(registry, sizeof(*registry));
    registry->next_feed_id = 1;
}

ngx_media_prepared_feed_t *
ngx_media_transform_stage_acquire(ngx_media_transform_registry_t *registry,
    const ngx_media_transform_key_t *key,
    const ngx_media_feed_conf_t *feed_conf, ngx_log_t *log)
{
    ngx_media_transform_stage_t *stage;
    ngx_media_prepared_feed_t   *feed;
    ngx_uint_t                   i;

    if (registry == NULL || key == NULL || feed_conf == NULL) {
        return NULL;
    }

    for (i = 0; i < NGX_MEDIA_TRANSFORM_MAX_STAGES; i++) {
        stage = &registry->stages[i];

        if (!stage->used) {
            continue;
        }

        if (ngx_media_transform_key_equal(&stage->key, key) == NGX_OK) {
            stage->refs++;
            ngx_media_prepared_feed_ref(stage->feed);
            return stage->feed;
        }
    }

    if (registry->count >= NGX_MEDIA_TRANSFORM_MAX_STAGES) {
        return NULL;
    }

    stage = NULL;

    for (i = 0; i < NGX_MEDIA_TRANSFORM_MAX_STAGES; i++) {
        if (!registry->stages[i].used) {
            stage = &registry->stages[i];
            break;
        }
    }

    if (stage == NULL) {
        return NULL;
    }

    feed = ngx_alloc(sizeof(*feed), log);
    if (feed == NULL) {
        return NULL;
    }

    if (ngx_media_prepared_feed_init(feed, registry->next_feed_id++, 1,
                                     feed_conf, log)
        != NGX_OK)
    {
        ngx_free(feed);
        return NULL;
    }

    stage->key = *key;
    stage->feed = feed;
    stage->refs = 1;
    stage->used = 1;
    registry->count++;

    return feed;
}

void
ngx_media_transform_stage_release(ngx_media_transform_registry_t *registry,
    const ngx_media_transform_key_t *key)
{
    ngx_media_transform_stage_t *stage;
    ngx_uint_t                   i;

    if (registry == NULL || key == NULL) {
        return;
    }

    for (i = 0; i < NGX_MEDIA_TRANSFORM_MAX_STAGES; i++) {
        stage = &registry->stages[i];

        if (!stage->used
            || ngx_media_transform_key_equal(&stage->key, key) != NGX_OK)
        {
            continue;
        }

        if (stage->refs > 0) {
            stage->refs--;
        }

        ngx_media_prepared_feed_unref(stage->feed);

        if (stage->refs == 0) {
            ngx_media_prepared_feed_destroy(stage->feed);
            ngx_free(stage->feed);
            ngx_memzero(stage, sizeof(*stage));
            registry->count--;
        }

        return;
    }
}

ngx_uint_t
ngx_media_transform_stage_count(const ngx_media_transform_registry_t *registry)
{
    return (registry != NULL) ? registry->count : 0;
}

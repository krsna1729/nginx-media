#ifndef NGX_MEDIA_TRANSFORM_H
#define NGX_MEDIA_TRANSFORM_H

#include "ngx_media.h"

/*
 * Logical media and prepared-feed primitives shared by the external executor
 * bridge and the protocol packages.  These types deliberately contain no
 * codec-library state: encoded payloads cross the process boundary, while
 * decoded surfaces never leave the executor.
 */

#define NGX_MEDIA_TRANSFORM_MAX_LANGUAGE 16
#define NGX_MEDIA_TRANSFORM_MAX_ROLE     16
#define NGX_MEDIA_TRANSFORM_MAX_STAGES   64
#define NGX_MEDIA_TRANSFORM_MAX_JOURNAL  8

#define NGX_MEDIA_TRACK_ROLE_MAIN        1
#define NGX_MEDIA_TRACK_ROLE_ALTERNATE   2
#define NGX_MEDIA_TRACK_ROLE_COMMENTARY  3
#define NGX_MEDIA_TRACK_ROLE_DESCRIPTIVE 4

#define NGX_MEDIA_TRANSFORM_EXECUTOR_AUTO             0
#define NGX_MEDIA_TRANSFORM_EXECUTOR_CPU              1
#define NGX_MEDIA_TRANSFORM_EXECUTOR_HARDWARE         2
#define NGX_MEDIA_TRANSFORM_EXECUTOR_HARDWARE_REQUIRED 3

typedef struct {
    ngx_uint_t  media_type;
    ngx_uint_t  role;
    ngx_uint_t  channels;
    ngx_uint_t  sample_rate;
    uint32_t    logical_id;
    u_char      language[NGX_MEDIA_TRANSFORM_MAX_LANGUAGE];
} ngx_media_logical_track_t;

typedef struct {
    ngx_uint_t  video_codec;
    ngx_uint_t  audio_codec;
    ngx_uint_t  width;
    ngx_uint_t  height;
    ngx_uint_t  frame_rate_num;
    ngx_uint_t  frame_rate_den;
    ngx_uint_t  video_bitrate;
    ngx_uint_t  audio_bitrate;
    ngx_uint_t  sample_rate;
    ngx_uint_t  channels;
    ngx_uint_t  executor;
    ngx_uint_t  profile_version;
    ngx_uint_t  color_policy;
    ngx_uint_t  gop_frames;
    ngx_uint_t  input_track_count;
    uint32_t    input_track_ids[NGX_MEDIA_TRANSFORM_MAX_JOURNAL];
} ngx_media_transform_spec_t;

typedef struct {
    uint64_t  digest;
    uint64_t  implementation;
    ngx_media_transform_spec_t spec;
} ngx_media_transform_key_t;

typedef struct {
    ngx_media_feed_t  feed;
    uint32_t           logical_id;
} ngx_media_track_journal_t;

typedef struct {
    uint64_t              id;
    uint64_t              epoch;
    ngx_media_feed_t      feed;
    ngx_atomic_t           refs;
    unsigned               active:1;
} ngx_media_prepared_feed_t;

typedef struct {
    ngx_media_transform_key_t key;
    ngx_media_prepared_feed_t *feed;
    ngx_uint_t                 refs;
    unsigned                   used:1;
} ngx_media_transform_stage_t;

typedef struct {
    ngx_media_transform_stage_t stages[NGX_MEDIA_TRANSFORM_MAX_STAGES];
    uint64_t                    next_feed_id;
    ngx_uint_t                  count;
} ngx_media_transform_registry_t;

void ngx_media_logical_track_init(ngx_media_logical_track_t *track,
    ngx_uint_t media_type, const char *language, ngx_uint_t role,
    ngx_uint_t channels, ngx_uint_t sample_rate, uint32_t logical_id);
ngx_int_t ngx_media_logical_track_equal(const ngx_media_logical_track_t *left,
    const ngx_media_logical_track_t *right);

/*
 * Selection returns indexes into the source track array.  It never copies
 * payloads or track objects, so a destination view is metadata-only.
 */
ngx_int_t ngx_media_logical_track_select(
    const ngx_media_logical_track_t *available, ngx_uint_t available_count,
    const ngx_media_logical_track_t *requested, ngx_uint_t requested_count,
    ngx_uint_t *selected, ngx_uint_t *selected_count);

uint64_t ngx_media_transform_key_hash(const ngx_media_transform_spec_t *spec,
    uint64_t implementation);
void ngx_media_transform_key_init(ngx_media_transform_key_t *key,
    const ngx_media_transform_spec_t *spec, uint64_t implementation);
ngx_int_t ngx_media_transform_key_equal(const ngx_media_transform_key_t *left,
    const ngx_media_transform_key_t *right);

ngx_int_t ngx_media_track_journal_init(ngx_media_track_journal_t *journal,
    uint32_t logical_id, const ngx_media_feed_conf_t *conf, ngx_log_t *log);
void ngx_media_track_journal_destroy(ngx_media_track_journal_t *journal);
ngx_int_t ngx_media_track_journal_publish(ngx_media_track_journal_t *journal,
    const ngx_media_frame_t *frame, ngx_msec_t now);

ngx_int_t ngx_media_prepared_feed_init(ngx_media_prepared_feed_t *feed,
    uint64_t id, uint64_t epoch, const ngx_media_feed_conf_t *conf,
    ngx_log_t *log);
void ngx_media_prepared_feed_destroy(ngx_media_prepared_feed_t *feed);
ngx_int_t ngx_media_prepared_feed_publish(ngx_media_prepared_feed_t *feed,
    const ngx_media_frame_t *frame, ngx_msec_t now);
void ngx_media_prepared_feed_ref(ngx_media_prepared_feed_t *feed);
void ngx_media_prepared_feed_unref(ngx_media_prepared_feed_t *feed);

void ngx_media_transform_registry_init(ngx_media_transform_registry_t *registry);
ngx_media_prepared_feed_t *ngx_media_transform_stage_acquire(
    ngx_media_transform_registry_t *registry,
    const ngx_media_transform_key_t *key,
    const ngx_media_feed_conf_t *feed_conf, ngx_log_t *log);
void ngx_media_transform_stage_release(ngx_media_transform_registry_t *registry,
    const ngx_media_transform_key_t *key);
ngx_uint_t ngx_media_transform_stage_count(
    const ngx_media_transform_registry_t *registry);

#endif /* NGX_MEDIA_TRANSFORM_H */

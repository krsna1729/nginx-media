#include "ngx_media_test.h"
#include "ngx_media_transform.h"

static ngx_media_frame_t
make_frame(void)
{
    ngx_media_frame_t frame;

    ngx_media_frame_init(&frame);
    frame.media_type = NGX_MEDIA_TYPE_VIDEO;
    frame.codec = NGX_MEDIA_CODEC_H264;
    frame.track_index = 0;
    frame.keyframe = 1;
    frame.payload = ngx_media_buf_alloc(4);
    TEST_ASSERT_NOT_NULL(frame.payload);
    (void) ngx_media_buf_freeze(frame.payload, 4);

    return frame;
}

int
main(void)
{
    ngx_media_logical_track_t  english, same, french;
    ngx_media_logical_track_t  available[2], requested[2];
    ngx_media_transform_spec_t spec;
    ngx_media_transform_key_t  key, same_key, different_key;
    ngx_media_track_journal_t  journal;
    ngx_media_prepared_feed_t *prepared, *same_prepared;
    ngx_media_transform_registry_t registry;
    ngx_media_feed_conf_t      conf;
    ngx_media_frame_t          frame, out;
    ngx_media_cursor_t         cursor;
    ngx_uint_t                  selected[2], selected_count, count;

    TEST_CASE("logical track identity is stable and metadata-sensitive");
    ngx_media_logical_track_init(&english, NGX_MEDIA_TYPE_AUDIO, "en",
                                 NGX_MEDIA_TRACK_ROLE_MAIN, 2, 48000, 7);
    ngx_media_logical_track_init(&same, NGX_MEDIA_TYPE_AUDIO, "en",
                                 NGX_MEDIA_TRACK_ROLE_MAIN, 2, 48000, 7);
    ngx_media_logical_track_init(&french, NGX_MEDIA_TYPE_AUDIO, "fr",
                                 NGX_MEDIA_TRACK_ROLE_MAIN, 2, 48000, 8);
    TEST_ASSERT_EQ_INT(ngx_media_logical_track_equal(&english, &same), NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_logical_track_equal(&english, &french),
                       NGX_DECLINED);

    TEST_CASE("track selection returns metadata indexes without copying");
    available[0] = english;
    available[1] = french;
    requested[0] = french;
    requested[1] = english;
    TEST_ASSERT_EQ_INT(ngx_media_logical_track_select(
                           available, 2, requested, 2, selected,
                           &selected_count),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(selected_count, 2);
    TEST_ASSERT_EQ_U64(selected[0], 1);
    TEST_ASSERT_EQ_U64(selected[1], 0);
    requested[1] = requested[0];
    TEST_ASSERT_EQ_INT(ngx_media_logical_track_select(
                           available, 2, requested, 2, selected,
                           &selected_count),
                       NGX_ERROR);

    TEST_CASE("transform key includes result-affecting profile fields");
    ngx_memzero(&spec, sizeof(spec));
    spec.video_codec = NGX_MEDIA_CODEC_H264;
    spec.audio_codec = NGX_MEDIA_CODEC_AAC;
    spec.width = 1280;
    spec.height = 720;
    spec.video_bitrate = 3000000;
    spec.audio_bitrate = 128000;
    spec.executor = NGX_MEDIA_TRANSFORM_EXECUTOR_CPU;
    spec.profile_version = 1;
    spec.input_track_count = 2;
    spec.input_track_ids[0] = 1;
    spec.input_track_ids[1] = 2;
    ngx_media_transform_key_init(&key, &spec, 11);
    ngx_media_transform_key_init(&same_key, &spec, 11);
    spec.width = 1920;
    ngx_media_transform_key_init(&different_key, &spec, 11);
    TEST_ASSERT_EQ_INT(ngx_media_transform_key_equal(&key, &same_key), NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_transform_key_equal(&key, &different_key),
                       NGX_DECLINED);
    TEST_ASSERT(ngx_media_transform_key_hash(&key.spec, key.implementation)
                == key.digest);

    conf.max_units = 4;
    conf.max_bytes = 1024;
    conf.max_age = 1000;
    frame = make_frame();

    TEST_CASE("track journal remains bounded and cursor-readable");
    TEST_ASSERT_EQ_INT(ngx_media_track_journal_init(&journal, 1, &conf, NULL),
                       NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_track_journal_publish(&journal, &frame, 10),
                       NGX_OK);
    ngx_media_feed_cursor_init(&journal.feed, &cursor);
    cursor.next_sequence = ngx_media_feed_tail(&journal.feed);
    TEST_ASSERT_EQ_INT(ngx_media_feed_read(&journal.feed, &cursor, 1, 0, 11,
                                           &out, &count),
                       NGX_MEDIA_FEED_BATCH);
    TEST_ASSERT_EQ_U64(count, 1);
    ngx_media_feed_release(&out, count);
    ngx_media_track_journal_destroy(&journal);

    TEST_CASE("equal physical stages share one prepared feed");
    ngx_media_transform_registry_init(&registry);
    prepared = ngx_media_transform_stage_acquire(&registry, &key, &conf, NULL);
    same_prepared = ngx_media_transform_stage_acquire(&registry, &same_key, &conf,
                                                      NULL);
    TEST_ASSERT_NOT_NULL(prepared);
    TEST_ASSERT(prepared == same_prepared);
    TEST_ASSERT_EQ_U64(ngx_media_transform_stage_count(&registry), 1);
    TEST_ASSERT_EQ_U64(prepared->id, 1);
    TEST_ASSERT_EQ_U64(prepared->epoch, 1);

    TEST_CASE("different physical stages do not alias");
    {
        ngx_media_prepared_feed_t *other;

        other = ngx_media_transform_stage_acquire(&registry, &different_key,
                                                  &conf, NULL);
        TEST_ASSERT_NOT_NULL(other);
        TEST_ASSERT(other != prepared);
        TEST_ASSERT_EQ_U64(ngx_media_transform_stage_count(&registry), 2);
        ngx_media_transform_stage_release(&registry, &different_key);
    }

    ngx_media_transform_stage_release(&registry, &key);
    TEST_ASSERT_EQ_U64(ngx_media_transform_stage_count(&registry), 1);
    ngx_media_transform_stage_release(&registry, &same_key);
    TEST_ASSERT_EQ_U64(ngx_media_transform_stage_count(&registry), 0);

    ngx_media_frame_release(&frame);
    TEST_LEAKS();
    TEST_MAIN_END();
}

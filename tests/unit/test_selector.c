#include "ngx_media_test.h"
#include "ngx_media_selector.h"
#include "ngx_media_stream.h"

static ngx_media_source_t *
add_source(ngx_media_stream_t *stream, const char *id, ngx_uint_t priority,
    const ngx_media_trackset_t *tracks)
{
    ngx_media_source_t *source;
    ngx_str_t           name;
    ngx_uint_t          i;

    name.data = (u_char *) id;
    name.len = strlen(id);

    source = ngx_media_stream_source_add(stream, &name, NGX_MEDIA_SOURCE_SRT,
                                         priority, NULL);
    TEST_ASSERT_NOT_NULL(source);

    if (tracks != NULL) {
        source->tracks = ngx_alloc(sizeof(ngx_media_trackset_t), NULL);
        TEST_ASSERT_NOT_NULL(source->tracks);
        TEST_ASSERT_EQ_INT(ngx_media_trackset_init(source->tracks, 4, NULL),
                           NGX_OK);

        for (i = 0; i < tracks->count; i++) {
            TEST_ASSERT(ngx_media_trackset_add(source->tracks,
                                               &tracks->tracks[i]) >= 0);
        }
    }

    return source;
}

/*
 * Drives a source's health to a definite state: a healthy source has fresh
 * evidence of every layer, an unhealthy one has a closed transport (hard
 * evidence).
 */
static void
set_health(ngx_media_stream_t *stream, ngx_media_source_t *source,
    ngx_uint_t healthy, ngx_msec_t now)
{
    ngx_media_health_init(&source->health, &stream->selector, now);

    ngx_media_health_transport(&source->health, healthy ? 1 : 0, now);

    if (healthy) {
        ngx_media_health_container(&source->health, 0, now);
        ngx_media_health_media(&source->health,
                               source->health.last_dts + 90000, now);
        ngx_media_health_evaluate(&source->health, now);
    }
}

/* keeps a standby hot with a decodable boundary */
static void
give_keyframe(ngx_media_stream_t *stream, ngx_media_source_t *source,
    int64_t dts)
{
    ngx_media_frame_t  frame;
    ngx_media_buf_t   *payload;

    ngx_media_frame_init(&frame);

    frame.media_type = NGX_MEDIA_TYPE_VIDEO;
    frame.codec = NGX_MEDIA_CODEC_H264;
    frame.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    frame.pts = dts;
    frame.dts = dts;
    frame.keyframe = 1;

    payload = ngx_media_buf_alloc(32);
    TEST_ASSERT_NOT_NULL(payload);
    memset(ngx_media_buf_data(payload), 0x42, 32);
    (void) ngx_media_buf_freeze(payload, 32);
    ngx_media_frame_adopt(&frame, payload);

    TEST_ASSERT_EQ_INT(ngx_media_stream_publish(stream, source, &frame,
                                                (ngx_msec_t) (dts / 90)),
                       NGX_OK);

    ngx_media_frame_release(&frame);
}

static void
free_source_tracks(ngx_media_source_t *source)
{
    if (source->tracks != NULL) {
        ngx_media_trackset_destroy(source->tracks);
        ngx_free(source->tracks);
        source->tracks = NULL;
    }
}

static void
track(ngx_media_trackset_t *set, ngx_uint_t media_type, ngx_uint_t codec)
{
    ngx_media_track_t  t;

    ngx_memzero(&t, sizeof(t));

    t.media_type = media_type;
    t.codec = codec;
    t.payload_format = (media_type == NGX_MEDIA_TYPE_AUDIO)
                       ? NGX_MEDIA_PAYLOAD_ADTS : NGX_MEDIA_PAYLOAD_ANNEXB;
    t.sample_rate = (media_type == NGX_MEDIA_TYPE_AUDIO) ? 48000 : 0;
    t.channels = (media_type == NGX_MEDIA_TYPE_AUDIO) ? 2 : 0;

    TEST_ASSERT(ngx_media_trackset_add(set, &t) >= 0);
}

int
main(void)
{
    ngx_pool_t                  *pool;
    ngx_media_stream_t           stream;
    ngx_media_source_t          *a, *b, *c;
    ngx_media_feed_conf_t        feed_conf;
    ngx_media_policy_t           policy;
    ngx_media_selector_result_t  res;
    ngx_media_trackset_t         prog_tracks, alt_tracks, aac_only;

    pool = ngx_create_pool(8192, NULL);
    TEST_ASSERT_NOT_NULL(pool);

    feed_conf.max_units = 64;
    feed_conf.max_bytes = 0;
    feed_conf.max_age = 0;

    ngx_media_policy_init(&policy);
    policy.failure_timeout = 1000;
    policy.recovery_timeout = 500;
    policy.switchback = NGX_MEDIA_SWITCHBACK_AUTO;

    TEST_ASSERT_EQ_INT(ngx_media_stream_init(&stream, pool, NULL,
                                             &(ngx_str_t) { 4, (u_char *) "live" },
                                             &(ngx_str_t) { 4, (u_char *) "news" },
                                             &feed_conf), NGX_OK);
    ngx_media_stream_set_policy(&stream, &policy);

    TEST_ASSERT_EQ_U64(stream.selector.failure_timeout, 1000);
    TEST_ASSERT_EQ_U64(stream.selector.switchback, NGX_MEDIA_SWITCHBACK_AUTO);

    TEST_ASSERT_EQ_INT(ngx_media_trackset_init(&prog_tracks, 4, NULL), NGX_OK);
    track(&prog_tracks, NGX_MEDIA_TYPE_VIDEO, NGX_MEDIA_CODEC_H264);
    track(&prog_tracks, NGX_MEDIA_TYPE_AUDIO, NGX_MEDIA_CODEC_AAC);

    TEST_ASSERT_EQ_INT(ngx_media_trackset_init(&alt_tracks, 4, NULL), NGX_OK);
    track(&alt_tracks, NGX_MEDIA_TYPE_VIDEO, NGX_MEDIA_CODEC_H264);
    track(&alt_tracks, NGX_MEDIA_TYPE_AUDIO, NGX_MEDIA_CODEC_AAC);
    prog_tracks.tracks[0].width = 1920;
    alt_tracks.tracks[0].width = 1280;

    TEST_ASSERT_EQ_INT(ngx_media_trackset_init(&aac_only, 4, NULL), NGX_OK);
    track(&aac_only, NGX_MEDIA_TYPE_AUDIO, NGX_MEDIA_CODEC_AAC);

    a = add_source(&stream, "encoder-a", 100, &prog_tracks);
    b = add_source(&stream, "encoder-b", 90, &alt_tracks);
    c = add_source(&stream, "encoder-c", 10, &aac_only);

    TEST_CASE("compatibility and priority are reported");
    stream.active = a;
    a->state = NGX_MEDIA_SOURCE_ACTIVE;
    a->active = 1;

    ngx_media_selector_evaluate(&stream, 0, &res);
    TEST_ASSERT(res.best == b);
    TEST_ASSERT(res.emergency == c);
    TEST_ASSERT_EQ_U64(res.eligible, 2);
    TEST_ASSERT_EQ_U64(res.active_eligible, 1);
    TEST_ASSERT_EQ_U64(b->compat, NGX_MEDIA_COMPAT_DEGRADED);
    TEST_ASSERT_EQ_U64(c->compat, NGX_MEDIA_COMPAT_INCOMPATIBLE);

    give_keyframe(&stream, b, 90000);
    give_keyframe(&stream, c, 90000);

    TEST_CASE("a healthy active source keeps the program");
    policy.switchback = NGX_MEDIA_SWITCHBACK_AUTO;
    ngx_media_stream_set_policy(&stream, &policy);

    TEST_ASSERT_EQ_INT(ngx_media_selector_run(&stream, 100, &res), NGX_OK);
    TEST_ASSERT(stream.active == a);

    TEST_CASE("a failed active source fails over to the best standby");
    set_health(&stream, a, 0, 200);
    set_health(&stream, b, 1, 200);
    set_health(&stream, c, 1, 200);

    TEST_ASSERT_EQ_INT(ngx_media_selector_run(&stream, 200, &res), NGX_OK);
    TEST_ASSERT(stream.active == b);
    TEST_ASSERT_EQ_U64(stream.switches, 1);
    TEST_ASSERT_EQ_U64(a->state, NGX_MEDIA_SOURCE_STANDBY);

    TEST_CASE("switchback policy manual keeps the current source");
    policy.switchback = NGX_MEDIA_SWITCHBACK_MANUAL;
    ngx_media_stream_set_policy(&stream, &policy);

    /* a recovers and outranks b, but the policy is manual */
    set_health(&stream, a, 1, 300);
    give_keyframe(&stream, a, 80000);
    TEST_ASSERT_EQ_INT(ngx_media_selector_run(&stream, 300, &res), NGX_OK);
    TEST_ASSERT(stream.active == b);

    TEST_CASE("an explicit switchback request moves back");
    TEST_ASSERT_EQ_INT(ngx_media_selector_switchback(&stream, 400), NGX_OK);
    TEST_ASSERT(stream.active == a);
    TEST_ASSERT_EQ_U64(stream.switches, 2);

    TEST_CASE("switchback policy never refuses");
    policy.switchback = NGX_MEDIA_SWITCHBACK_NEVER;
    ngx_media_stream_set_policy(&stream, &policy);

    set_health(&stream, b, 1, 500);
    give_keyframe(&stream, b, 190000);
    b->priority = 500;

    TEST_ASSERT_EQ_INT(ngx_media_selector_run(&stream, 500, &res), NGX_OK);
    TEST_ASSERT(stream.active == a);
    TEST_ASSERT_EQ_INT(ngx_media_selector_switchback(&stream, 600),
                       NGX_DECLINED);

    TEST_CASE("an incompatible-only fallback is emergency-switched");
    set_health(&stream, a, 0, 700);
    set_health(&stream, b, 0, 700);
    set_health(&stream, c, 1, 700);
    give_keyframe(&stream, c, 280000);

    TEST_ASSERT_EQ_INT(ngx_media_selector_run(&stream, 700, &res), NGX_OK);
    TEST_ASSERT(stream.active == c);
    TEST_ASSERT_EQ_U64(stream.emergency_switches, 1);
    TEST_ASSERT_EQ_U64(stream.switches, 3);

    TEST_CASE("no eligible source leaves the program idle");
    set_health(&stream, c, 0, 800);
    TEST_ASSERT_EQ_INT(ngx_media_selector_run(&stream, 800, &res), NGX_OK);
    TEST_ASSERT(stream.active == c);

    ngx_media_stream_source_remove(&stream, c);
    TEST_ASSERT_NULL(stream.active);
    TEST_ASSERT_EQ_U64(stream.emergency_switches, 1);

    TEST_ASSERT_EQ_INT(ngx_media_selector_run(&stream, 900, &res), NGX_OK);
    TEST_ASSERT_NULL(stream.active);

    TEST_CASE("a recovered source is promoted again");
    set_health(&stream, a, 1, 1000);
    give_keyframe(&stream, a, 370000);
    TEST_ASSERT_EQ_INT(ngx_media_selector_run(&stream, 1000, &res), NGX_OK);
    TEST_ASSERT(stream.active == a);

    TEST_CASE("health hysteresis holds a recovering source back");
    set_health(&stream, b, 1, 2000);
    give_keyframe(&stream, b, 460000);

    /* b is the only standby and outranks nothing: no switch expected here */
    TEST_ASSERT_EQ_INT(ngx_media_selector_run(&stream, 2000, &res), NGX_OK);
    TEST_ASSERT(stream.active == a);

    TEST_CASE("NULL tolerance");
    ngx_media_selector_run(NULL, 0, NULL);
    ngx_media_selector_evaluate(NULL, 0, NULL);
    TEST_ASSERT_EQ_INT(ngx_media_selector_switchback(NULL, 0), NGX_ERROR);
    ngx_media_stream_set_policy(NULL, NULL);

    free_source_tracks(a);
    free_source_tracks(b);
    free_source_tracks(c);

    ngx_media_trackset_destroy(&prog_tracks);
    ngx_media_trackset_destroy(&alt_tracks);
    ngx_media_trackset_destroy(&aac_only);

    ngx_media_stream_destroy(&stream);
    ngx_destroy_pool(pool);

    TEST_LEAKS();

    TEST_MAIN_END();
}

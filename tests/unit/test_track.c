#include "ngx_media_test.h"
#include "ngx_media_track.h"

int
main(void)
{
    ngx_media_trackset_t     set;
    ngx_media_track_t        track;
    ngx_media_buf_t         *config;
    const ngx_media_track_t *got;

    TEST_CASE("init validates capacity");
    TEST_ASSERT_EQ_INT(ngx_media_trackset_init(&set, 0, NULL), NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_trackset_init(NULL, 1, NULL), NGX_ERROR);

    TEST_CASE("add keeps codec config by reference");
    TEST_ASSERT_EQ_INT(ngx_media_trackset_init(&set, 2, NULL), NGX_OK);
    TEST_ASSERT_EQ_U64(set.count, 0);

    config = ngx_media_buf_alloc(8);
    TEST_ASSERT_NOT_NULL(config);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(config), 1);

    ngx_memzero(&track, sizeof(track));
    track.media_type = 1;
    track.codec = 1;
    track.width = 1920;
    track.height = 1080;
    track.config = config;

    TEST_ASSERT_EQ_INT(ngx_media_trackset_add(&set, &track), 0);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(config), 2);

    got = ngx_media_trackset_get(&set, 0);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_U64(got->width, 1920);
    TEST_ASSERT(got->config == config);

    TEST_CASE("capacity is a hard limit");
    TEST_ASSERT_EQ_INT(ngx_media_trackset_add(&set, &track), 1);
    TEST_ASSERT_EQ_INT(ngx_media_trackset_add(&set, &track), NGX_ERROR);
    TEST_ASSERT_EQ_U64(set.count, 2);
    TEST_ASSERT_NULL(ngx_media_trackset_get(&set, 2));

    TEST_CASE("destroy releases config references");
    ngx_media_trackset_destroy(&set);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(config), 1);
    TEST_ASSERT_NULL(set.tracks);

    ngx_media_buf_unref(config);
    TEST_ASSERT_EQ_U64(ngx_media_test_allocs, ngx_media_test_frees);

    TEST_CASE("NULL tolerance");
    TEST_ASSERT_EQ_INT(ngx_media_trackset_add(NULL, &track), NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_trackset_add(&set, NULL), NGX_ERROR);
    TEST_ASSERT_NULL(ngx_media_trackset_get(NULL, 0));

    ngx_media_trackset_destroy(&set);
    ngx_media_trackset_destroy(NULL);

    TEST_LEAKS();

    TEST_MAIN_END();
}

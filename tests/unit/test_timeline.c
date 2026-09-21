#include "ngx_media_test.h"
#include "ngx_media_timeline.h"

int
main(void)
{
    ngx_media_timeline_t  tl;
    int64_t               pts, dts;

    TEST_CASE("init and first frame anchoring");
    ngx_media_timeline_init(&tl);
    ngx_media_timeline_init(NULL);

    TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 900000, 900000, &pts, &dts),
                       NGX_OK);
    TEST_ASSERT_EQ_I64(dts, 0);
    TEST_ASSERT_EQ_I64(pts, 0);
    TEST_ASSERT_EQ_I64(tl.offset, -900000);
    TEST_ASSERT_EQ_U64(tl.switches, 0);
    TEST_ASSERT_EQ_U64(tl.resyncs, 0);

    TEST_CASE("monotonic mapping within a generation");
    TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 903600, 903600, &pts, &dts),
                       NGX_OK);
    TEST_ASSERT_EQ_I64(dts, 3600);
    TEST_ASSERT_EQ_I64(pts, 3600);
    TEST_ASSERT_EQ_U64(tl.resyncs, 0);

    TEST_CASE("composition offset is preserved, including negative offsets");
    /* B-frame style: pts < dts */
    TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 905000, 907200, &pts, &dts),
                       NGX_OK);
    TEST_ASSERT_EQ_I64(dts, 7200);
    TEST_ASSERT_EQ_I64(pts - dts, -2200);

    TEST_CASE("a switch anchors the next source after the last program DTS");
    ngx_media_timeline_discontinuity(&tl);
    TEST_ASSERT_EQ_U64(tl.switches, 1);

    /* the new source starts its own clock anywhere */
    TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 1000, 1000, &pts, &dts),
                       NGX_OK);
    TEST_ASSERT_EQ_I64(dts, 7201);
    TEST_ASSERT_EQ_I64(pts, 7201);
    TEST_ASSERT_EQ_I64(tl.offset, 7201 - 1000);

    TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 4000, 4000, &pts, &dts),
                       NGX_OK);
    TEST_ASSERT_EQ_I64(dts, 10201);

    TEST_CASE("a source clock regression re-anchors instead of going back");
    TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 10, 10, &pts, &dts), NGX_OK);
    TEST_ASSERT_EQ_I64(dts, 10202);
    TEST_ASSERT_EQ_U64(tl.resyncs, 1);
    TEST_ASSERT_EQ_I64(pts - dts, 0);

    TEST_CASE("a forward clock jump is followed; a wrap re-anchors");
    {
        int64_t  after_jump;

        /* 2^33 - 1 is the largest PES timestamp value */
        TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 8589934591LL,
                                                  8589934591LL, &pts, &dts),
                           NGX_OK);
        TEST_ASSERT(dts > 10202);
        after_jump = dts;

        /* the next frame wraps to a small value: re-anchor, never regress */
        TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 100, 100, &pts, &dts),
                           NGX_OK);
        TEST_ASSERT_EQ_I64(dts, after_jump + 1);
        TEST_ASSERT_EQ_U64(tl.resyncs, 2);
    }

    TEST_CASE("NULL tolerance");
    TEST_ASSERT_EQ_INT(ngx_media_timeline_map(NULL, 0, 0, &pts, &dts),
                       NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 0, 0, NULL, &dts),
                       NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_timeline_map(&tl, 0, 0, &pts, NULL),
                       NGX_ERROR);
    ngx_media_timeline_discontinuity(NULL);

    TEST_LEAKS();

    TEST_MAIN_END();
}

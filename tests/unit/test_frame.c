#include "ngx_media_test.h"

#include "ngx_media.h" /* compile check: aggregate core object model */

int
main(void)
{
    ngx_media_frame_t  frame, copy;
    ngx_media_buf_t   *buf, *second;

    TEST_CASE("init zeroes the frame");
    ngx_media_frame_init(&frame);
    TEST_ASSERT_NULL(frame.payload);
    TEST_ASSERT_EQ_I64(frame.pts, 0);
    TEST_ASSERT(!frame.keyframe);

    TEST_CASE("adopt takes ownership and releases the previous payload");
    buf = ngx_media_buf_alloc(16);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(buf), 1);

    ngx_media_frame_adopt(&frame, buf);
    TEST_ASSERT(frame.payload == buf);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(buf), 1);

    second = ngx_media_buf_alloc(8);
    TEST_ASSERT_NOT_NULL(second);
    ngx_media_frame_adopt(&frame, second);
    TEST_ASSERT(frame.payload == second);
    TEST_ASSERT_EQ_U64(ngx_media_test_allocs - ngx_media_test_frees, 1);

    TEST_CASE("copy shares the payload by reference");
    frame.media_type = NGX_MEDIA_TYPE_VIDEO;
    frame.keyframe = 1;

    TEST_ASSERT_EQ_INT(ngx_media_frame_copy(&copy, &frame), NGX_OK);
    TEST_ASSERT(copy.payload == second);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(second), 2);
    TEST_ASSERT_EQ_U64(copy.media_type, NGX_MEDIA_TYPE_VIDEO);
    TEST_ASSERT(copy.keyframe);

    TEST_CASE("release unrefs and zeroes");
    ngx_media_frame_release(&copy);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(second), 1);
    TEST_ASSERT_NULL(copy.payload);
    TEST_ASSERT(!copy.keyframe);

    ngx_media_frame_release(&frame);
    TEST_ASSERT_EQ_U64(ngx_media_test_allocs, ngx_media_test_frees);

    TEST_CASE("NULL tolerance");
    TEST_ASSERT_EQ_INT(ngx_media_frame_copy(NULL, &frame), NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_frame_copy(&copy, NULL), NGX_ERROR);
    ngx_media_frame_release(NULL);
    ngx_media_frame_init(NULL);
    ngx_media_frame_adopt(NULL, NULL);

    TEST_LEAKS();

    TEST_MAIN_END();
}

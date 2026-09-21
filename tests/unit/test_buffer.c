#include "ngx_media_test.h"
#include "ngx_media_buffer.h"

int
main(void)
{
    ngx_media_buf_t  *buf, *ref;
    u_char           *data;

    TEST_CASE("alloc: capacity, refs and inline writable data");

    buf = ngx_media_buf_alloc(128);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQ_U64(ngx_media_buf_capacity(buf), 128);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(buf), 0);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(buf), 1);

    data = ngx_media_buf_data(buf);
    TEST_ASSERT_NOT_NULL(data);
    data[0] = 0xAB;
    data[127] = 0xCD;

    TEST_CASE("freeze: rejects overflow, sets length");
    TEST_ASSERT_EQ_INT(ngx_media_buf_freeze(buf, 129), NGX_ERROR);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(buf), 0);
    TEST_ASSERT_EQ_INT(ngx_media_buf_freeze(buf, 128), NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(buf), 128);

    TEST_CASE("ref/unref: count and final free");
    ref = ngx_media_buf_ref(buf);
    TEST_ASSERT(ref == buf);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(buf), 2);

    ngx_media_buf_unref(ref);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(buf), 1);

    ngx_media_buf_unref(buf);
    TEST_ASSERT_EQ_U64(ngx_media_test_allocs, ngx_media_test_frees);

    TEST_CASE("zero-capacity buffer and NULL tolerance");
    buf = ngx_media_buf_alloc(0);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQ_INT(ngx_media_buf_freeze(buf, 1), NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_buf_freeze(buf, 0), NGX_OK);
    ngx_media_buf_unref(buf);

    TEST_ASSERT_NULL(ngx_media_buf_ref(NULL));
    TEST_ASSERT_EQ_INT(ngx_media_buf_freeze(NULL, 0), NGX_ERROR);
    ngx_media_buf_unref(NULL);

    TEST_CASE("impossible allocation is rejected");
    TEST_ASSERT_NULL(ngx_media_buf_alloc((size_t) -1));

    TEST_CASE("allocation balance");
    TEST_LEAKS();

    TEST_MAIN_END();
}

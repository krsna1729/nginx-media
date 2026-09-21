#include "ngx_media_test.h"
#include "ngx_media_ts_ingest.h"

static int
ingest_setup(ngx_media_ts_ingest_t *ingest, ngx_uint_t max_chunks,
    size_t max_bytes)
{
    ngx_media_ts_ingest_conf_t  conf;

    conf.max_chunks = max_chunks;
    conf.max_bytes = max_bytes;

    return (ngx_media_ts_ingest_init(ingest, &conf, NULL) == NGX_OK) ? 0 : -1;
}

int
main(void)
{
    ngx_media_ts_ingest_t        ingest;
    ngx_media_ts_ingest_stats_t  stats;
    ngx_media_ts_ingest_chunk_t  out[8];
    u_char                       data[64];
    ngx_uint_t                   count;

    ngx_memzero(&ingest, sizeof(ingest));
    memset(data, 0x11, sizeof(data));

    TEST_CASE("init validates the ceilings");
    TEST_ASSERT_EQ_INT(ingest_setup(&ingest, 0, 1024), -1);
    TEST_ASSERT_EQ_INT(ingest_setup(&ingest, 4, 0), -1);
    TEST_ASSERT_EQ_INT(ngx_media_ts_ingest_init(NULL, NULL, NULL), NGX_ERROR);

    TEST_CASE("write/read round trip keeps order and bytes");
    TEST_ASSERT_EQ_INT(ingest_setup(&ingest, 8, 4096), 0);
    TEST_ASSERT_EQ_INT(ngx_media_ts_ingest_write(&ingest, data, 10, 100),
                       NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_ts_ingest_write(&ingest, data, 20, 200),
                       NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_ts_ingest_write(&ingest, data, 30, 300),
                       NGX_OK);

    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_pending(&ingest), 3);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_bytes(&ingest), 60);

    count = ngx_media_ts_ingest_read(&ingest, out, 2);
    TEST_ASSERT_EQ_U64(count, 2);
    TEST_ASSERT_EQ_U64(out[0].len, 10);
    TEST_ASSERT_EQ_U64(out[1].len, 20);
    TEST_ASSERT_EQ_U64(out[0].time, 100);
    TEST_ASSERT_EQ_U64(out[1].time, 200);
    TEST_ASSERT_EQ_U64(out[0].data[0], 0x11);
    ngx_media_ts_ingest_release(out, count);

    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_pending(&ingest), 1);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_bytes(&ingest), 30);

    count = ngx_media_ts_ingest_read(&ingest, out, 8);
    TEST_ASSERT_EQ_U64(count, 1);
    TEST_ASSERT_EQ_U64(out[0].len, 30);
    ngx_media_ts_ingest_release(out, count);

    count = ngx_media_ts_ingest_read(&ingest, out, 8);
    TEST_ASSERT_EQ_U64(count, 0);

    ngx_media_ts_ingest_stats(&ingest, &stats);
    TEST_ASSERT_EQ_U64(stats.bytes_in, 60);
    TEST_ASSERT_EQ_U64(stats.chunks_in, 3);
    TEST_ASSERT_EQ_U64(stats.chunks_dropped, 0);
    TEST_ASSERT_EQ_U64(stats.last_write, 300);

    TEST_CASE("chunk ceiling drops the newest chunk and counts it");
    ngx_media_ts_ingest_destroy(&ingest);
    TEST_ASSERT_EQ_INT(ingest_setup(&ingest, 2, 4096), 0);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_write(&ingest, data, 10, 400),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_write(&ingest, data, 10, 500),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_write(&ingest, data, 10, 600),
                       NGX_AGAIN);

    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_pending(&ingest), 2);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_bytes(&ingest), 20);

    ngx_media_ts_ingest_stats(&ingest, &stats);
    TEST_ASSERT_EQ_U64(stats.chunks_dropped, 1);
    TEST_ASSERT_EQ_U64(stats.bytes_dropped, 10);
    TEST_ASSERT_EQ_U64(stats.chunks_in, 3);

    count = ngx_media_ts_ingest_read(&ingest, out, 8);
    TEST_ASSERT_EQ_U64(count, 2);
    TEST_ASSERT_EQ_U64(out[0].time, 400);
    TEST_ASSERT_EQ_U64(out[1].time, 500);
    ngx_media_ts_ingest_release(out, count);

    TEST_CASE("byte ceiling bounds queued bytes");
    ngx_media_ts_ingest_destroy(&ingest);
    TEST_ASSERT_EQ_INT(ingest_setup(&ingest, 64, 100), 0);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_write(&ingest, data, 60, 100),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_write(&ingest, data, 60, 200),
                       NGX_AGAIN);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_bytes(&ingest), 60);

    TEST_CASE("per-chunk and argument validation");
    TEST_ASSERT_EQ_INT(ngx_media_ts_ingest_write(&ingest, data, 0, 0),
                       NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_ts_ingest_write(&ingest, NULL, 10, 0),
                       NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_ts_ingest_write(&ingest, data,
                       NGX_MEDIA_TS_INGEST_CHUNK_MAX + 1, 0), NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_ts_ingest_write(NULL, data, 10, 0),
                       NGX_ERROR);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_read(NULL, out, 1), 0);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_read(&ingest, out, 0), 0);
    ngx_media_ts_ingest_release(NULL, 0);
    ngx_media_ts_ingest_stats(NULL, &stats);
    TEST_ASSERT_EQ_U64(stats.bytes_in, 0);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_pending(NULL), 0);
    TEST_ASSERT_EQ_U64(ngx_media_ts_ingest_bytes(NULL), 0);

    TEST_CASE("destroy releases queued payloads and is idempotent");
    count = ngx_media_ts_ingest_read(&ingest, out, 8);
    TEST_ASSERT_EQ_U64(count, 1);
    ngx_media_ts_ingest_release(out, count);

    TEST_ASSERT_EQ_INT(ngx_media_ts_ingest_write(&ingest, data, 32, 300),
                       NGX_OK);
    ngx_media_ts_ingest_destroy(&ingest);
    TEST_ASSERT_NULL(ingest.chunks);
    ngx_media_ts_ingest_destroy(&ingest);
    ngx_media_ts_ingest_destroy(NULL);

    TEST_ASSERT_EQ_U64(ngx_media_test_allocs, ngx_media_test_frees);

    TEST_LEAKS();

    TEST_MAIN_END();
}

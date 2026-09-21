#include "ngx_media_test.h"
#include "ngx_media_feed.h"
#include "ngx_media_frame.h"

static ngx_media_frame_t
make_frame(size_t len, int64_t pts, unsigned keyframe)
{
    ngx_media_frame_t  frame;
    ngx_media_buf_t   *payload;

    ngx_media_frame_init(&frame);

    frame.media_type = 1;
    frame.codec = 1;
    frame.payload_format = 1;
    frame.pts = pts;
    frame.dts = pts;
    frame.keyframe = keyframe ? 1 : 0;

    if (len > 0) {
        payload = ngx_media_buf_alloc(len);
        if (payload == NULL) {
            return frame;
        }

        memset(ngx_media_buf_data(payload), 0x5A, len);
        (void) ngx_media_buf_freeze(payload, len);

        frame.payload = payload; /* the frame owns the only reference */
    }

    return frame;
}

static int
feed_setup(ngx_media_feed_t *feed, ngx_uint_t max_units, size_t max_bytes,
    ngx_msec_t max_age)
{
    ngx_media_feed_conf_t  conf;

    conf.max_units = max_units;
    conf.max_bytes = max_bytes;
    conf.max_age = max_age;

    return (ngx_media_feed_init(feed, &conf, NULL) == NGX_OK) ? 0 : -1;
}

static int
feed_reset(ngx_media_feed_t *feed, ngx_uint_t max_units, size_t max_bytes,
    ngx_msec_t max_age)
{
    ngx_media_feed_destroy(feed);
    return feed_setup(feed, max_units, max_bytes, max_age);
}

static int
publish_n(ngx_media_feed_t *feed, ngx_uint_t n, size_t len, ngx_msec_t start,
    ngx_msec_t step, ngx_uint_t keyframe_every)
{
    ngx_uint_t         i;
    ngx_media_frame_t  frame;

    for (i = 0; i < n; i++) {
        frame = make_frame(len, (int64_t) i * 1000,
                           keyframe_every != 0 && (i % keyframe_every) == 0);

        if (len > 0 && frame.payload == NULL) {
            return -1;
        }

        if (ngx_media_feed_publish(feed, &frame, start + i * step) != NGX_OK) {
            ngx_media_frame_release(&frame);
            return -1;
        }

        ngx_media_frame_release(&frame);
    }

    return 0;
}

static void
cursor_from_tail(ngx_media_feed_t *feed, ngx_media_cursor_t *cursor)
{
    ngx_media_feed_cursor_init(feed, cursor);
    cursor->next_sequence = ngx_media_feed_tail(feed);
}

int
main(void)
{
    ngx_media_feed_t       feed;
    ngx_media_cursor_t     cursor;
    ngx_media_frame_t      out[32];
    ngx_media_frame_t      frame;
    ngx_media_buf_t       *payload;
    ngx_uint_t             status, count;

    ngx_memzero(&feed, sizeof(feed));

    TEST_CASE("init validates configuration");
    TEST_ASSERT_EQ_INT(feed_setup(&feed, 0, 0, 0), -1);
    TEST_ASSERT_EQ_INT(ngx_media_feed_init(NULL, NULL, NULL), NGX_ERROR);

    TEST_CASE("fresh feed: generation 1, empty, cursor at head");
    TEST_ASSERT_EQ_INT(feed_setup(&feed, 16, 0, 0), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_generation(&feed), 1);
    TEST_ASSERT_EQ_U64(ngx_media_feed_head(&feed), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_tail(&feed), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&feed), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_bytes(&feed), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_last_keyframe(&feed),
                       NGX_MEDIA_FEED_NO_KEYFRAME);

    ngx_media_feed_cursor_init(&feed, &cursor);
    TEST_ASSERT_EQ_U64(cursor.generation, 1);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 0);

    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_EMPTY);
    TEST_ASSERT_EQ_U64(count, 0);

    TEST_CASE("an uninitialised cursor is rejected");
    cursor.generation = 0;
    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_GENERATION_MISMATCH);

    TEST_CASE("publish/read round trip preserves order, advances cursor");
    ngx_media_feed_cursor_init(&feed, &cursor);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 4, 10, 0, 100, 4), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&feed), 4);
    TEST_ASSERT_EQ_U64(ngx_media_feed_bytes(&feed), 40);
    TEST_ASSERT_EQ_U64(ngx_media_feed_last_keyframe(&feed), 0);

    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_BATCH);
    TEST_ASSERT_EQ_U64(count, 4);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 4);
    TEST_ASSERT_EQ_I64(out[0].pts, 0);
    TEST_ASSERT_EQ_I64(out[1].pts, 1000);
    TEST_ASSERT_EQ_I64(out[2].pts, 2000);
    TEST_ASSERT_EQ_I64(out[3].pts, 3000);
    TEST_ASSERT_NOT_NULL(out[3].payload);
    TEST_ASSERT_EQ_U64(ngx_media_buf_size(out[3].payload), 10);
    TEST_ASSERT(out[0].keyframe);
    ngx_media_feed_release(out, count);

    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_EMPTY);

    TEST_CASE("batch limits: max_units, then max_bytes with progress");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 16, 0, 0), 0);
    ngx_media_feed_cursor_init(&feed, &cursor);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 3, 20, 0, 100, 0), 0);

    status = ngx_media_feed_read(&feed, &cursor, 2, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_BATCH);
    TEST_ASSERT_EQ_U64(count, 2);
    ngx_media_feed_release(out, count);

    status = ngx_media_feed_read(&feed, &cursor, 2, 0, out, &count);
    TEST_ASSERT_EQ_U64(count, 1);
    ngx_media_feed_release(out, count);

    status = ngx_media_feed_read(&feed, &cursor, 2, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_EMPTY);

    /* budget smaller than one frame: the first unit is always returned */
    cursor_from_tail(&feed, &cursor);
    status = ngx_media_feed_read(&feed, &cursor, 8, 10, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_BATCH);
    TEST_ASSERT_EQ_U64(count, 1);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 1);
    ngx_media_feed_release(out, count);

    /* two frames fit into the budget */
    status = ngx_media_feed_read(&feed, &cursor, 8, 40, out, &count);
    TEST_ASSERT_EQ_U64(count, 2);
    ngx_media_feed_release(out, count);

    TEST_CASE("unit ceiling evicts the oldest media");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 4, 0, 0), 0);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 6, 10, 0, 100, 0), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&feed), 4);
    TEST_ASSERT_EQ_U64(ngx_media_feed_tail(&feed), 2);
    TEST_ASSERT_EQ_U64(ngx_media_feed_head(&feed), 6);
    TEST_ASSERT_EQ_U64(ngx_media_feed_bytes(&feed), 40);

    cursor.generation = ngx_media_feed_generation(&feed);
    cursor.next_sequence = 0;
    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_OVERRUN);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 0);

    TEST_CASE("resync to latest recovers from overrun");
    TEST_ASSERT_EQ_INT(ngx_media_feed_resync(&feed, &cursor,
                                             NGX_MEDIA_FEED_RESYNC_LATEST),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 6);
    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_EMPTY);

    TEST_CASE("byte ceiling bounds retained payload bytes");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 64, 100, 0), 0);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 4, 30, 0, 100, 0), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&feed), 3);
    TEST_ASSERT_EQ_U64(ngx_media_feed_bytes(&feed), 90);
    TEST_ASSERT_EQ_U64(ngx_media_feed_tail(&feed), 1);

    TEST_CASE("a unit larger than the byte ceiling is retained alone");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 8, 50, 0), 0);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 1, 30, 0, 100, 0), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_bytes(&feed), 30);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 1, 100, 100, 100, 0), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&feed), 1);
    TEST_ASSERT_EQ_U64(ngx_media_feed_bytes(&feed), 100);
    TEST_ASSERT_EQ_U64(ngx_media_feed_tail(&feed), 1);

    TEST_CASE("age ceiling expires retained media");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 16, 0, 1000), 0);

    frame = make_frame(10, 0, 0);
    TEST_ASSERT_EQ_INT(ngx_media_feed_publish(&feed, &frame, 0), NGX_OK);
    ngx_media_frame_release(&frame);

    frame = make_frame(10, 1000, 0);
    TEST_ASSERT_EQ_INT(ngx_media_feed_publish(&feed, &frame, 400), NGX_OK);
    ngx_media_frame_release(&frame);

    frame = make_frame(10, 2000, 0);
    TEST_ASSERT_EQ_INT(ngx_media_feed_publish(&feed, &frame, 800), NGX_OK);
    ngx_media_frame_release(&frame);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&feed), 3);

    frame = make_frame(10, 3000, 0);
    TEST_ASSERT_EQ_INT(ngx_media_feed_publish(&feed, &frame, 1500), NGX_OK);
    ngx_media_frame_release(&frame);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&feed), 2);
    TEST_ASSERT_EQ_U64(ngx_media_feed_tail(&feed), 2);

    TEST_CASE("discontinuity invalidates cursors, resync restores reading");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 16, 0, 0), 0);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 3, 10, 0, 100, 1), 0);
    ngx_media_feed_cursor_init(&feed, &cursor);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 3);

    ngx_media_feed_discontinuity(&feed);
    TEST_ASSERT_EQ_U64(ngx_media_feed_generation(&feed), 2);

    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_GENERATION_MISMATCH);
    TEST_ASSERT_EQ_U64(cursor.generation, 1);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 3);

    TEST_ASSERT_EQ_INT(ngx_media_feed_resync(&feed, &cursor,
                                             NGX_MEDIA_FEED_RESYNC_KEYFRAME),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(cursor.generation, 2);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 2);

    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_BATCH);
    TEST_ASSERT_EQ_U64(count, 1);
    TEST_ASSERT(out[0].keyframe);
    ngx_media_feed_release(out, count);

    TEST_CASE("keyframe resync selects the newest retained keyframe");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 16, 0, 0), 0);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 8, 10, 0, 100, 4), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_last_keyframe(&feed), 4);

    ngx_media_feed_cursor_init(&feed, &cursor);
    ngx_media_feed_discontinuity(&feed);
    TEST_ASSERT_EQ_INT(ngx_media_feed_resync(&feed, &cursor,
                                             NGX_MEDIA_FEED_RESYNC_KEYFRAME),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 4);

    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(count, 4);
    TEST_ASSERT(out[0].keyframe);
    TEST_ASSERT_EQ_I64(out[0].pts, 4000);
    ngx_media_feed_release(out, count);

    TEST_CASE("keyframe resync falls back to head once evicted");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 4, 0, 0), 0);

    frame = make_frame(10, 0, 1);
    TEST_ASSERT_EQ_INT(ngx_media_feed_publish(&feed, &frame, 0), NGX_OK);
    ngx_media_frame_release(&frame);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 7, 10, 100, 100, 0), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_tail(&feed), 4);
    TEST_ASSERT_EQ_U64(ngx_media_feed_last_keyframe(&feed),
                       NGX_MEDIA_FEED_NO_KEYFRAME);

    ngx_media_feed_cursor_init(&feed, &cursor);
    ngx_media_feed_discontinuity(&feed);
    TEST_ASSERT_EQ_INT(ngx_media_feed_resync(&feed, &cursor,
                                             NGX_MEDIA_FEED_RESYNC_KEYFRAME),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(cursor.next_sequence, 8);
    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_EMPTY);

    TEST_CASE("a cursor ahead of the feed requires resync");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 8, 0, 0), 0);
    TEST_ASSERT_EQ_INT(publish_n(&feed, 2, 10, 0, 100, 0), 0);
    cursor.generation = ngx_media_feed_generation(&feed);
    cursor.next_sequence = ngx_media_feed_head(&feed) + 5;
    status = ngx_media_feed_read(&feed, &cursor, 8, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_GENERATION_MISMATCH);

    TEST_CASE("readers hold payload references across feed destruction");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 4, 0, 0), 0);

    frame = make_frame(16, 0, 1);
    payload = frame.payload;
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(payload), 1);

    TEST_ASSERT_EQ_INT(ngx_media_feed_publish(&feed, &frame, 0), NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(payload), 2);
    ngx_media_frame_release(&frame);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(payload), 1);

    cursor_from_tail(&feed, &cursor);
    status = ngx_media_feed_read(&feed, &cursor, 4, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_BATCH);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(payload), 2);

    ngx_media_feed_destroy(&feed);
    TEST_ASSERT_EQ_U64(ngx_media_buf_refs(payload), 1);

    ngx_media_feed_release(out, count);
    TEST_ASSERT_EQ_U64(ngx_media_test_allocs, ngx_media_test_frees);

    TEST_CASE("frames without payload are allowed and cost no bytes");
    TEST_ASSERT_EQ_INT(feed_reset(&feed, 4, 0, 0), 0);
    ngx_media_frame_init(&frame);
    frame.pts = 42;
    TEST_ASSERT_EQ_INT(ngx_media_feed_publish(&feed, &frame, 0), NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&feed), 1);
    TEST_ASSERT_EQ_U64(ngx_media_feed_bytes(&feed), 0);

    cursor_from_tail(&feed, &cursor);
    status = ngx_media_feed_read(&feed, &cursor, 4, 0, out, &count);
    TEST_ASSERT_EQ_U64(status, NGX_MEDIA_FEED_BATCH);
    TEST_ASSERT_EQ_U64(count, 1);
    TEST_ASSERT_NULL(out[0].payload);
    ngx_media_feed_release(out, count);

    TEST_CASE("destroy is idempotent, invalid arguments are rejected");
    ngx_media_feed_destroy(&feed);
    ngx_media_feed_destroy(&feed);
    ngx_media_feed_destroy(NULL);

    TEST_ASSERT_EQ_INT(ngx_media_feed_publish(NULL, &frame, 0), NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_feed_publish(&feed, NULL, 0), NGX_ERROR);
    TEST_ASSERT_EQ_U64(ngx_media_feed_read(NULL, &cursor, 1, 0, out, &count),
                       NGX_MEDIA_FEED_ERROR);
    TEST_ASSERT_EQ_U64(ngx_media_feed_read(&feed, NULL, 1, 0, out, &count),
                       NGX_MEDIA_FEED_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_feed_resync(&feed, &cursor, 99), NGX_ERROR);

    ngx_media_feed_discontinuity(NULL);
    ngx_media_feed_cursor_init(NULL, &cursor);
    ngx_media_feed_release(NULL, 0);

    TEST_LEAKS();

    TEST_MAIN_END();
}

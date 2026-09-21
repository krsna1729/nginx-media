#include "ngx_media_test.h"
#include "ngx_media_stream.h"

#define STREAM_UNITS 64

#define S(lit) (&(ngx_str_t) { .len = sizeof(lit) - 1, \
                               .data = (u_char *) (lit) })

static ngx_media_frame_t
make_frame(ngx_uint_t media_type, int64_t dts, unsigned keyframe, size_t len)
{
    ngx_media_frame_t  frame;
    ngx_media_buf_t   *payload;

    ngx_media_frame_init(&frame);

    frame.media_type = media_type;
    frame.codec = (media_type == NGX_MEDIA_TYPE_AUDIO) ? NGX_MEDIA_CODEC_AAC
                                                       : NGX_MEDIA_CODEC_H264;
    frame.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    frame.pts = dts;
    frame.dts = dts;
    frame.keyframe = keyframe ? 1 : 0;

    if (len > 0) {
        payload = ngx_media_buf_alloc(len);
        if (payload != NULL) {
            memset(ngx_media_buf_data(payload), 0x42, len);
            (void) ngx_media_buf_freeze(payload, len);
            frame.payload = payload;   /* the frame owns the only reference */
        }
    }

    return frame;
}

static ngx_int_t
publish(ngx_media_stream_t *stream, ngx_media_source_t *source,
    ngx_uint_t media_type, int64_t dts, unsigned keyframe, size_t len)
{
    ngx_media_frame_t  frame;
    ngx_int_t          rc;

    frame = make_frame(media_type, dts, keyframe, len);
    rc = ngx_media_stream_publish(stream, source, &frame, dts);
    ngx_media_frame_release(&frame);

    return rc;
}

typedef struct {
    ngx_uint_t  frames;
    int64_t     last_dts;
    uint64_t    regressions;
    uint64_t    keyframes;
} feed_view_t;

static void
drain_program(ngx_media_stream_t *stream, feed_view_t *view)
{
    ngx_media_cursor_t  cursor;
    ngx_media_frame_t   out[32];
    ngx_uint_t          count, i, status;

    ngx_media_feed_cursor_init(&stream->program_feed, &cursor);
    cursor.next_sequence = ngx_media_feed_tail(&stream->program_feed);

    for ( ;; ) {
        status = ngx_media_feed_read(&stream->program_feed, &cursor, 32, 0,
                                     1000, out, &count);

        if (status == NGX_MEDIA_FEED_GENERATION_MISMATCH) {
            (void) ngx_media_feed_resync(&stream->program_feed, &cursor,
                                         NGX_MEDIA_FEED_RESYNC_LATEST);
            continue;
        }

        if (status != NGX_MEDIA_FEED_BATCH) {
            break;
        }

        for (i = 0; i < count; i++) {
            if (view->frames > 0 && out[i].dts <= view->last_dts) {
                view->regressions++;
            }

            view->last_dts = out[i].dts;
            view->frames++;

            if (out[i].keyframe) {
                view->keyframes++;
            }
        }

        ngx_media_feed_release(out, count);
    }
}

int
main(void)
{
    ngx_pool_t             *pool;
    ngx_media_stream_t      stream;
    ngx_media_source_t     *a, *b;
    ngx_media_feed_conf_t   feed_conf;
    feed_view_t             view;

    pool = ngx_create_pool(4096, NULL);
    TEST_ASSERT_NOT_NULL(pool);

    feed_conf.max_units = STREAM_UNITS;
    feed_conf.max_bytes = 0;
    feed_conf.max_age = 0;

    TEST_CASE("stream registry");
    TEST_ASSERT_EQ_INT(ngx_media_stream_init(&stream, pool, NULL, S("live"),
                                             S("news"), &feed_conf),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(stream.generation, 1);

    a = ngx_media_stream_source_add(&stream, S("encoder-a"),
                                    NGX_MEDIA_SOURCE_SRT, 100, NULL);
    b = ngx_media_stream_source_add(&stream, S("encoder-b"),
                                    NGX_MEDIA_SOURCE_SRT, 90, NULL);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQ_U64(ngx_media_stream_source_count(&stream), 2);
    TEST_ASSERT(ngx_media_stream_source_find(&stream, S("encoder-a")) == a);
    TEST_ASSERT_NULL(ngx_media_stream_source_find(&stream, S("nobody")));
    TEST_ASSERT_NULL(ngx_media_stream_source_add(&stream, S("encoder-a"),
                                                 NGX_MEDIA_SOURCE_SRT, 1,
                                                 NULL));

    a->has_video = 1;
    b->has_video = 1;

    TEST_CASE("standby frames only fill the complete-GOP cache");
    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 1000, 0, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_units(a), 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_head(&stream.program_feed), 0);

    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 2000, 1, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_AUDIO, 2100, 1, 10),
                       NGX_OK);
    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 3000, 0, 40),
                       NGX_OK);

    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_units(a), 3);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_bytes(a), 90);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_ready(a), 1);
    TEST_ASSERT_EQ_U64(ngx_media_feed_head(&stream.program_feed), 0);

    /* a newer keyframe replaces the cached GOP wholesale */
    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 4000, 1, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_units(a), 1);

    TEST_CASE("the ceilings clear the cache instead of keeping a partial GOP");
    ngx_media_source_preroll_destroy(a);
    TEST_ASSERT_EQ_INT(ngx_media_source_preroll_init(a, 4, 100, NULL), NGX_OK);

    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 5000, 1, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_units(a), 1);

    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 6000, 0, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_units(a), 2);

    /* the third 40 byte frame crosses the 100 byte ceiling */
    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 7000, 0, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_units(a), 0);
    TEST_ASSERT_EQ_U64(a->preroll.overflows, 1);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_ready(a), 0);

    /* the source level API reports the overflow to callers that care */
    {
        ngx_media_frame_t  frame;
        ngx_int_t          rc;

        frame = make_frame(NGX_MEDIA_TYPE_VIDEO, 7500, 1, 40);
        TEST_ASSERT_EQ_INT(ngx_media_source_preroll_push(a, &frame), NGX_OK);
        rc = ngx_media_source_preroll_push(a, &frame);
        ngx_media_frame_release(&frame);
        TEST_ASSERT_EQ_INT(rc, NGX_OK);

        frame = make_frame(NGX_MEDIA_TYPE_VIDEO, 7600, 0, 40);
        (void) ngx_media_source_preroll_push(a, &frame);
        rc = ngx_media_source_preroll_push(a, &frame);
        ngx_media_frame_release(&frame);
        TEST_ASSERT_EQ_INT(rc, NGX_AGAIN);
        TEST_ASSERT_EQ_U64(a->preroll.overflows, 2);
        ngx_media_source_preroll_reset(a);
    }

    TEST_CASE("promotion without a cached boundary waits for a keyframe");
    TEST_ASSERT_EQ_INT(ngx_media_stream_promote(&stream, a), NGX_OK);
    TEST_ASSERT_EQ_U64(a->state, NGX_MEDIA_SOURCE_AWAITING_SYNC);
    TEST_ASSERT_NULL(stream.active);
    TEST_ASSERT_EQ_U64(stream.generation, 1);

    /* a non-key frame cannot complete the switch */
    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 8000, 0, 40),
                       NGX_OK);
    TEST_ASSERT_NULL(stream.active);

    /* the keyframe completes it, and the cached GOP becomes the program */
    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 9000, 1, 40),
                       NGX_OK);
    TEST_ASSERT(stream.active == a);
    TEST_ASSERT(a->active);
    TEST_ASSERT_EQ_U64(a->state, NGX_MEDIA_SOURCE_ACTIVE);
    /* the first activation of a stream is not a source switch */
    TEST_ASSERT_EQ_U64(stream.generation, 1);
    TEST_ASSERT_EQ_U64(stream.switches, 0);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&stream.program_feed), 1);

    TEST_CASE("the active source writes straight into the program");
    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 10000, 0, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&stream.program_feed), 2);

    TEST_CASE("manual promotion switches at the standby's cached keyframe");
    TEST_ASSERT_EQ_INT(publish(&stream, b, NGX_MEDIA_TYPE_VIDEO, 70000, 1, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_INT(publish(&stream, b, NGX_MEDIA_TYPE_VIDEO, 71000, 0, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_units(b), 2);

    TEST_ASSERT_EQ_INT(ngx_media_stream_promote(&stream, b), NGX_OK);
    TEST_ASSERT(stream.active == b);
    TEST_ASSERT(b->active);
    TEST_ASSERT_EQ_U64(a->state, NGX_MEDIA_SOURCE_STANDBY);
    TEST_ASSERT_EQ_U64(stream.generation, 2);
    TEST_ASSERT_EQ_U64(stream.switches, 1);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_units(b), 0);
    TEST_ASSERT_EQ_U64(b->frames_out, 2);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&stream.program_feed), 4);

    TEST_CASE("program time stays monotonic across the switch");
    ngx_memzero(&view, sizeof(view));
    drain_program(&stream, &view);
    TEST_ASSERT_EQ_U64(view.frames, 4);
    TEST_ASSERT_EQ_U64(view.regressions, 0);
    TEST_ASSERT_EQ_U64(view.keyframes, 2);

    /* the demoted source no longer writes to the program */
    TEST_ASSERT_EQ_INT(publish(&stream, a, NGX_MEDIA_TYPE_VIDEO, 11000, 1, 40),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_feed_units(&stream.program_feed), 4);
    TEST_ASSERT_EQ_U64(ngx_media_source_preroll_units(a), 1);

    TEST_CASE("readers see an explicit generation change on switch");
    {
        ngx_media_cursor_t  cursor;
        ngx_media_frame_t   out[8];
        ngx_uint_t          count;

        cursor.generation = 1;
        cursor.next_sequence = 0;

        TEST_ASSERT_EQ_U64(ngx_media_feed_read(&stream.program_feed, &cursor, 8,
                                               0, 1000, out, &count),
                           NGX_MEDIA_FEED_GENERATION_MISMATCH);
        TEST_ASSERT_EQ_INT(ngx_media_feed_resync(&stream.program_feed, &cursor,
                                                 NGX_MEDIA_FEED_RESYNC_KEYFRAME),
                           NGX_OK);
        TEST_ASSERT_EQ_U64(cursor.generation, 2);
        TEST_ASSERT_EQ_U64(cursor.next_sequence, 2);
    }

    TEST_CASE("a promotion waits for in-flight writer leases to drain");
    TEST_ASSERT_EQ_INT(ngx_media_stream_lease_begin(&stream, b), NGX_OK);

    /* a is hot again (keyframe cached above), but b still holds a lease */
    TEST_ASSERT_EQ_INT(ngx_media_stream_promote(&stream, a), NGX_AGAIN);
    TEST_ASSERT(stream.active == b);
    TEST_ASSERT_EQ_U64(a->pending_switch, 1);

    ngx_media_stream_lease_end(&stream, b);
    ngx_media_stream_switch_resolve(&stream);

    TEST_ASSERT(stream.active == a);
    TEST_ASSERT_EQ_U64(a->pending_switch, 0);
    TEST_ASSERT_EQ_U64(stream.generation, 3);

    TEST_CASE("removing the active source idles the program with a bump");
    {
        ngx_uint_t  generation = stream.generation;

        ngx_media_stream_source_remove(&stream, a);
        TEST_ASSERT_NULL(stream.active);
        TEST_ASSERT_EQ_U64(stream.generation, generation + 1);
        TEST_ASSERT_EQ_U64(ngx_media_stream_source_count(&stream), 1);
    }

    TEST_CASE("removal of a leased source is deferred until the lease drains");
    TEST_ASSERT_EQ_INT(ngx_media_stream_lease_begin(&stream, b), NGX_OK);
    ngx_media_stream_source_remove(&stream, b);
    TEST_ASSERT_EQ_U64(ngx_media_stream_source_count(&stream), 1);
    TEST_ASSERT_EQ_U64(b->pending_remove, 1);

    ngx_media_stream_lease_end(&stream, b);
    ngx_media_stream_switch_resolve(&stream);
    TEST_ASSERT_EQ_U64(ngx_media_stream_source_count(&stream), 0);

    TEST_CASE("NULL tolerance");
    TEST_ASSERT_EQ_INT(ngx_media_stream_publish(NULL, NULL, NULL, 0), NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_stream_promote(NULL, NULL), NGX_ERROR);
    ngx_media_stream_source_remove(NULL, NULL);
    TEST_ASSERT_NULL(ngx_media_stream_source_add(NULL, S("x"),
                                                 NGX_MEDIA_SOURCE_SRT, 0,
                                                 NULL));
    TEST_ASSERT_EQ_U64(ngx_media_stream_source_count(NULL), 0);
    ngx_media_stream_switch_resolve(NULL);
    ngx_media_stream_lease_end(NULL, NULL);
    ngx_media_source_preroll_replay(NULL, NULL, NULL);

    TEST_CASE("teardown");
    ngx_media_stream_destroy(&stream);
    ngx_media_stream_destroy(NULL);
    ngx_destroy_pool(pool);

    TEST_LEAKS();

    TEST_MAIN_END();
}

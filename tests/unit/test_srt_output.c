/*
 * Bounded subscriber queue for one SRT destination: unit and byte ceilings,
 * whole-burst drops under overrun, and keyframe resync so a slow receiver
 * never resumes in the middle of a GOP (goal doc 34 items 11 and 12).
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_srt_output_queue.h"

#include <stdio.h>

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if (!(cond)) {                                                        \
            ngx_media_test_failures++;                                        \
            printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,               \
                   ##__VA_ARGS__);                                            \
        }                                                                     \
    } while (0)

static ngx_media_buf_t *
burst(size_t len)
{
    ngx_media_buf_t  *buf = ngx_media_buf_alloc(len);

    if (buf == NULL) {
        return NULL;
    }

    memset(ngx_media_buf_data(buf), 0x47, len);
    (void) ngx_media_buf_freeze(buf, len);

    return buf;
}

static void
test_ordering(void)
{
    ngx_media_srt_queue_t  q;
    ngx_media_buf_t       *b;
    const ngx_media_srt_unit_t  *unit;
    uint64_t               cursor = 0;
    ngx_uint_t             i;

    TEST_CASE("queue order and cursor");

    ngx_media_srt_queue_init(&q, 8, 1024 * 1024);

    for (i = 0; i < 4; i++) {
        b = burst(100 + i);

        CHECK(ngx_media_srt_queue_push(&q, b, 100 + i, i == 0) == NGX_OK,
              "unit %lu pushed", i);
        CHECK(ngx_media_buf_refs(b) == 2, "queue took its own reference: %lu",
              ngx_media_buf_refs(b));
        ngx_media_buf_unref(b);
    }

    CHECK(ngx_media_srt_queue_head(&q) == 4, "head advanced");

    for (i = 0; i < 4; i++) {
        unit = ngx_media_srt_queue_next(&q, &cursor);

        CHECK(unit != NULL, "unit %lu available", i);
        CHECK(unit != NULL && unit->sequence == i, "sequence %lu", i);
        CHECK(unit != NULL && unit->len == 100 + i, "length %lu",
              unit != NULL ? unit->len : 0);

        if (unit != NULL) {
            /* peeking does not advance: the caller commits after delivery */
            CHECK(cursor == i, "cursor not advanced by next(): %lu", cursor);
            ngx_media_srt_queue_advance(&q, &cursor, unit->sequence);
            CHECK(cursor == i + 1, "cursor advanced after delivery");
        }
    }

    CHECK(ngx_media_srt_queue_next(&q, &cursor) == NULL, "queue drained");
    CHECK(q.dropped == 0, "nothing dropped: %lu", q.dropped);

    ngx_media_srt_queue_destroy(&q);
}

static void
test_unit_ceiling(void)
{
    ngx_media_srt_queue_t  q;
    ngx_media_buf_t       *b;
    const ngx_media_srt_unit_t  *unit;
    uint64_t               cursor = 0;
    ngx_uint_t             i;

    TEST_CASE("unit ceiling drops the oldest bursts");

    ngx_media_srt_queue_init(&q, 4, 0);

    for (i = 0; i < 10; i++) {
        b = burst(64);
        CHECK(ngx_media_srt_queue_push(&q, b, 64, 1) == NGX_OK, "push %lu", i);
        ngx_media_buf_unref(b);
    }

    CHECK(q.head == 10, "ten units pushed: %lu", q.head);
    CHECK(q.dropped == 6, "six dropped: %lu", q.dropped);
    CHECK(q.head - q.tail == 4, "four retained: %lu", q.head - q.tail);

    unit = ngx_media_srt_queue_next(&q, &cursor);
    CHECK(unit != NULL && unit->sequence == 6, "oldest retained is 6: %lu",
          unit != NULL ? unit->sequence : 0);

    if (unit != NULL) {
        ngx_media_srt_queue_advance(&q, &cursor, unit->sequence);
    }

    ngx_media_srt_queue_destroy(&q);
}

static void
test_byte_ceiling(void)
{
    ngx_media_srt_queue_t  q;
    ngx_media_buf_t       *b;
    uint64_t               cursor = 0;

    TEST_CASE("byte ceiling bounds retained bytes");

    ngx_media_srt_queue_init(&q, 64, 1000);

    {
        ngx_uint_t  i;

        for (i = 0; i < 6; i++) {
            b = burst(400);
            CHECK(ngx_media_srt_queue_push(&q, b, 400, 1) == NGX_OK,
                  "push %lu", i);
            ngx_media_buf_unref(b);

            CHECK(q.bytes <= 1000, "retained bytes bounded: %lu", q.bytes);
        }
    }

    CHECK(q.bytes == 800, "two bursts retained: %lu", q.bytes);
    CHECK(q.dropped == 4, "four dropped: %lu", q.dropped);

    /* a burst larger than the whole ceiling is dropped, not retained */
    b = burst(2000);
    CHECK(ngx_media_srt_queue_push(&q, b, 2000, 1) == NGX_OK,
          "oversized burst handled");
    ngx_media_buf_unref(b);

    CHECK(q.bytes == 800, "oversized burst not retained: %lu", q.bytes);
    CHECK(q.dropped == 5, "counted as dropped: %lu", q.dropped);

    (void) cursor;

    ngx_media_srt_queue_destroy(&q);
}

static void
test_keyframe_resync(void)
{
    ngx_media_srt_queue_t  q;
    ngx_media_buf_t       *b;
    const ngx_media_srt_unit_t  *unit;
    uint64_t               cursor = 0;
    ngx_uint_t             i;

    TEST_CASE("a slow consumer resumes at a sync boundary");

    ngx_media_srt_queue_init(&q, 4, 0);

    /* one keyframe followed by inter frames */
    for (i = 0; i < 3; i++) {
        b = burst(100);
        (void) ngx_media_srt_queue_push(&q, b, 100, i == 0);
        ngx_media_buf_unref(b);
    }

    unit = ngx_media_srt_queue_next(&q, &cursor);
    CHECK(unit != NULL && unit->sequence == 0, "consumer read the keyframe");

    if (unit != NULL) {
        ngx_media_srt_queue_advance(&q, &cursor, unit->sequence);
    }

    /* the consumer stalls while more inter frames and a keyframe arrive */
    for (i = 0; i < 8; i++) {
        b = burst(100);
        (void) ngx_media_srt_queue_push(&q, b, 100, i == 4);
        ngx_media_buf_unref(b);
    }

    CHECK(q.dropped > 0, "bursts were dropped while the consumer stalled");

    /* the stalled consumer resumes at the retained keyframe, not mid-GOP */
    unit = ngx_media_srt_queue_next(&q, &cursor);

    CHECK(unit != NULL, "a unit is available after the drop");
    CHECK(unit != NULL && unit->keyframe, "resumed at a keyframe");

    if (unit != NULL) {
        /* the retained window is the last four units: 7 is the keyframe */
        CHECK(unit->sequence == 7, "the retained keyframe is sequence 7: %lu",
              unit->sequence);
        ngx_media_srt_queue_advance(&q, &cursor, unit->sequence);
    }

    ngx_media_srt_queue_destroy(&q);
}

static void
test_references(void)
{
    ngx_media_srt_queue_t  q;
    ngx_media_buf_t       *b;

    TEST_CASE("queue holds references, never copies");

    ngx_media_srt_queue_init(&q, 4, 0);

    b = burst(128);

    CHECK(ngx_media_buf_refs(b) == 1, "one reference before push");

    (void) ngx_media_srt_queue_push(&q, b, 128, 1);

    CHECK(ngx_media_buf_refs(b) == 2, "queue took a reference: %lu",
          ngx_media_buf_refs(b));

    ngx_media_buf_unref(b);

    CHECK(ngx_media_buf_refs(b) == 1, "caller released its reference");

    /* destroy releases the last reference: TEST_LEAKS() proves it */
    ngx_media_srt_queue_destroy(&q);
}

int
main(void)
{
    printf("== srt output queue\n");

    test_ordering();
    test_unit_ceiling();
    test_byte_ceiling();
    test_keyframe_resync();
    test_references();

    TEST_LEAKS();
    TEST_MAIN_END();
}

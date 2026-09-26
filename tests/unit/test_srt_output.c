/*
 * Bounded subscriber queue for one SRT destination: unit and byte ceilings,
 * whole-burst drops under overrun, and keyframe resync so a slow receiver
 * never resumes in the middle of a GOP (goal doc 34 items 11 and 12).
 *
 * The destination runtime itself is exercised too: one sender pool serves
 * every destination of a program, so several SRT destinations cost one
 * schedulable sender, not one thread each (goal doc 11.3, 34 item 16).
 */

/*
 * The output header sets _POSIX_C_SOURCE itself for a unit build, so it has
 * to come first: defining _DEFAULT_SOURCE here would collide with it.
 */
#include "ngx_media_srt_output.h"

#include "ngx_media_test.h"

#include "ngx_media_srt_output_queue.h"

#include <dirent.h>
#include <stdio.h>
#include <time.h>

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

        CHECK(ngx_media_srt_queue_push(&q, b, 100 + i, i == 0,
                                       1000 + i) == NGX_OK,
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

        CHECK(unit != NULL && unit->enqueue_msec == 1000 + i,
              "enqueue time retained: %lu",
              unit != NULL ? unit->enqueue_msec : 0);
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
test_consumed_units_are_reclaimed(void)
{
    ngx_media_srt_queue_t  q;
    ngx_media_buf_t       *b;
    const ngx_media_srt_unit_t  *unit;
    uint64_t               cursor = 0;
    ngx_uint_t             i;

    TEST_CASE("consumed queue units release their capacity");

    ngx_media_srt_queue_init(&q, 4, 400);

    for (i = 0; i < 16; i++) {
        b = burst(100);
        CHECK(b != NULL, "burst %lu allocated", i);
        if (b == NULL) {
            break;
        }

        CHECK(ngx_media_srt_queue_push(&q, b, 100, i == 0, 2000 + i) == NGX_OK,
              "burst %lu queued", i);
        ngx_media_buf_unref(b);

        unit = ngx_media_srt_queue_next(&q, &cursor);
        CHECK(unit != NULL && unit->sequence == i,
              "consumer receives burst %lu", i);
        if (unit != NULL) {
            ngx_media_srt_queue_advance(&q, &cursor, unit->sequence);
        }

        CHECK(q.dropped == 0, "no consumed burst evicted: %lu", q.dropped);
        CHECK(q.bytes == 0, "consumed burst releases bytes: %lu", q.bytes);
    }

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
        CHECK(ngx_media_srt_queue_push(&q, b, 64, 1, 3000 + i) == NGX_OK,
              "push %lu", i);
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
            CHECK(ngx_media_srt_queue_push(&q, b, 400, 1, 4000 + i) == NGX_OK,
                  "push %lu", i);
            ngx_media_buf_unref(b);

            CHECK(q.bytes <= 1000, "retained bytes bounded: %lu", q.bytes);
        }
    }

    CHECK(q.bytes == 800, "two bursts retained: %lu", q.bytes);
    CHECK(q.dropped == 4, "four dropped: %lu", q.dropped);

    /* a burst larger than the whole ceiling is dropped, not retained */
    b = burst(2000);
    CHECK(ngx_media_srt_queue_push(&q, b, 2000, 1, 5000) == NGX_OK,
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
        (void) ngx_media_srt_queue_push(&q, b, 100, i == 0, 6000 + i);
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
        (void) ngx_media_srt_queue_push(&q, b, 100, i == 4, 7000 + i);
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

    (void) ngx_media_srt_queue_push(&q, b, 128, 1, 8000);

    CHECK(ngx_media_buf_refs(b) == 2, "queue took a reference: %lu",
          ngx_media_buf_refs(b));

    ngx_media_buf_unref(b);

    CHECK(ngx_media_buf_refs(b) == 1, "caller released its reference");

    /* destroy releases the last reference: TEST_LEAKS() proves it */
    ngx_media_srt_queue_destroy(&q);
}

static ngx_uint_t
thread_count(void)
{
    DIR           *dir;
    struct dirent *ent;
    ngx_uint_t     n = 0;

    dir = opendir("/proc/self/task");

    if (dir == NULL) {
        return 0;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] != '.') {
            n++;
        }
    }

    closedir(dir);

    return n;
}

/*
 * SRT destinations retain their logical shards while a bounded physical pool
 * changes the number of active senders.
 */
static void
test_shared_sender_pool(void)
{
    ngx_media_srt_listener_t    *listener;
    ngx_media_srt_outputs_t     *outs = NULL;
    ngx_media_srt_output_conf_t  conf;
    ngx_media_srt_out_event_t    events[16];
    ngx_media_buf_t             *b;
    ngx_uint_t                   seen[4] = { 0, 0, 0, 0 };
    ngx_media_srt_egress_stats_t  before_resize[NGX_MEDIA_SRT_EGRESS_SHARDS];
    ngx_media_srt_egress_stats_t  after_resize[NGX_MEDIA_SRT_EGRESS_SHARDS];
    ngx_uint_t                   connected, i, n, nstats, tries;
    ngx_uint_t                   base, after_start, after_adds, after_media;

    TEST_CASE("stable SRT shard placement across concurrency changes");

    listener = ngx_media_srt_listen((const u_char *) "127.0.0.1", 24590, NULL,
                                    NULL);
    CHECK(listener != NULL, "listener created");

    base = thread_count();
    CHECK(base > 0, "thread count readable: %lu", (unsigned long) base);

    memset(&conf, 0, sizeof(conf));
    conf.application.len = 4;
    conf.application.data = (u_char *) "live";
    conf.stream.len = 4;
    conf.stream.data = (u_char *) "news";
    conf.host.len = 9;
    conf.host.data = (u_char *) "127.0.0.1";
    conf.port = 24590;
    conf.max_units = 64;
    conf.max_bytes = 256 * 1024;
    conf.connect_timeout = 1000;
    conf.send_timeout = 1000;

    /* the manager starts one fixed egress thread for each shard */
    CHECK(ngx_media_srt_outputs_start(&outs, &conf, 1, 16, NULL) == NGX_OK,
          "outputs started");

    after_start = thread_count();
#ifndef __SANITIZE_THREAD__
    CHECK(after_start == base + NGX_MEDIA_SRT_EGRESS_SHARDS,
          "exactly %ui egress shard threads started: %lu -> %lu",
          NGX_MEDIA_SRT_EGRESS_SHARDS, (unsigned long) base,
          (unsigned long) after_start);
#else
    /*
     * ThreadSanitizer starts a background thread of its own the first time
     * the process creates one, so the count is at least the shards here;
     * "no thread per destination" is still checked exactly below, as the
     * difference between two counts taken after it exists.
     */
    CHECK(after_start >= base + NGX_MEDIA_SRT_EGRESS_SHARDS,
          "at least %ui egress shard threads started: %lu -> %lu",
          NGX_MEDIA_SRT_EGRESS_SHARDS, (unsigned long) base,
          (unsigned long) after_start);
#endif

    for (i = 0; i < 3; i++) {
        CHECK(ngx_media_srt_outputs_add(outs, &conf, NULL, NULL) == NGX_OK,
              "runtime destination %lu added", i + 1);
    }

    after_adds = thread_count();

    /*
     * Three added destinations are served by their assigned existing shards;
     * no per-destination thread is created.
     */
    CHECK(after_adds == after_start,
          "adding destinations created no sender: %lu -> %lu",
          (unsigned long) after_start, (unsigned long) after_adds);
    nstats = ngx_media_srt_outputs_stats_get(
        outs, before_resize, NGX_MEDIA_SRT_EGRESS_SHARDS);
    CHECK(nstats == NGX_MEDIA_SRT_EGRESS_SHARDS,
          "all logical shards are observable: %lu", nstats);
    for (i = 0; i < 4; i++) {
        CHECK(before_resize[i].destinations == 1,
              "destination remains assigned to logical shard %lu", i);
    }

    CHECK(ngx_media_srt_outputs_concurrency(outs) == 1,
          "start at one active sender");
    CHECK(ngx_media_srt_outputs_set_concurrency(outs, 4) == NGX_OK,
          "scale active senders up");
    CHECK(ngx_media_srt_outputs_concurrency(outs) == 4,
          "four active senders reported");

    nstats = ngx_media_srt_outputs_stats_get(
        outs, after_resize, NGX_MEDIA_SRT_EGRESS_SHARDS);
    CHECK(nstats == NGX_MEDIA_SRT_EGRESS_SHARDS,
          "all logical shards remain observable");
    for (i = 0; i < NGX_MEDIA_SRT_EGRESS_SHARDS; i++) {
        CHECK(after_resize[i].shard == before_resize[i].shard
              && after_resize[i].destinations
                 == before_resize[i].destinations,
              "logical shard %lu placement is unchanged", i);
    }

    /* and all four are bound to the same application/stream */
    b = burst(1024);
    CHECK(b != NULL, "burst allocated");

    for (i = 0; i < 8; i++) {
        CHECK(ngx_media_srt_outputs_push(outs, &conf.application, &conf.stream,
                                         1, 1, b, 1024, 1) == NGX_OK,
              "prepared burst offered to each shard");
    }
    CHECK(ngx_media_srt_outputs_set_concurrency(outs, 2) == NGX_OK,
          "scale active senders down with media queued");
    CHECK(ngx_media_srt_outputs_concurrency(outs) == 2,
          "two active senders reported after resize");

    ngx_media_buf_unref(b);

    connected = 0;
    tries = 0;

    while (connected < 4 && tries < 500) {
        n = ngx_media_srt_outputs_event_read(outs, events, 16);

        for (i = 0; i < n; i++) {
            if (events[i].type == NGX_MEDIA_SRT_OUT_EVENT_CONNECTED
                && events[i].index < 4)
            {
                seen[events[i].index] = 1;
            }
        }

        connected = seen[0] + seen[1] + seen[2] + seen[3];

        if (connected < 4) {
            struct timespec  ts = { 0, 10 * 1000 * 1000 };

            (void) nanosleep(&ts, NULL);
            tries++;
        }
    }

    CHECK(connected == 4,
          "all four destinations are served by the pool: %lu",
          (unsigned long) connected);
    CHECK(ngx_media_srt_outputs_set_concurrency(outs, 1) == NGX_OK,
          "return to one active sender");

    CHECK(ngx_media_srt_outputs_concurrency(outs) == 1,
          "one active sender reported after resize");
    after_media = thread_count();

    CHECK(after_media == after_adds,
          "serving media grew no senders: %lu -> %lu",
          (unsigned long) after_adds, (unsigned long) after_media);

    ngx_media_srt_outputs_stop(outs);
    ngx_media_srt_listen_close(listener);

    /*
     * Under ThreadSanitizer this check is skipped rather than weakened: TSan
     * runs threads of its own, so a kernel task count cannot say whether a
     * sender survived.  It is not needed there either - if the join did not
     * happen, the sender keeps walking the table after stop has freed it, and
     * TSan reports that as a use-after-free, which is a stronger statement
     * than this count.
     */
#ifndef __SANITIZE_THREAD__
    CHECK(thread_count() == base, "stopping joined every sender: %lu -> %lu",
          (unsigned long) after_media, (unsigned long) thread_count());
#endif
}

int
main(void)
{
    printf("== srt output queue\n");

    test_ordering();
    test_consumed_units_are_reclaimed();
    test_unit_ceiling();
    test_byte_ceiling();
    test_keyframe_resync();
    test_references();
    test_shared_sender_pool();

    TEST_LEAKS();
    TEST_MAIN_END();
}

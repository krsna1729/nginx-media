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
#include "ngx_media_srt_udp.h"
#include "ngx_media_stream.h"

#include <pthread.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>
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

/*
 * The module's own sender threads, by the name each gives itself
 * (srt-egress-NN).  Counting every task in the process instead made these
 * checks depend on whatever else runs in it: ThreadSanitizer starts a thread
 * of its own, and on Debian trixie a sanitizer or library thread appeared
 * between two counts in CI ("stopping joined every sender: 17 -> 2").
 */
static ngx_uint_t
thread_count(void)
{
    DIR           *dir;
    struct dirent *ent;
    ngx_uint_t     n = 0;
    char           path[320], comm[32];
    FILE          *f;

    dir = opendir("/proc/self/task");

    if (dir == NULL) {
        return 0;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        (void) snprintf(path, sizeof(path), "/proc/self/task/%s/comm",
                        ent->d_name);
        f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        if (fgets(comm, sizeof(comm), f) != NULL
            && strncmp(comm, "srt-egress-", 11) == 0)
        {
            n++;
        }
        (void) fclose(f);
    }

    closedir(dir);

    return n;
}

/* a sender names itself once it runs: give new ones a moment to */
static ngx_uint_t
thread_count_settled(ngx_uint_t want)
{
    ngx_uint_t  i, n = 0;

    for (i = 0; i < 200; i++) {
        n = thread_count();
        if (n == want) {
            break;
        }
        {
            struct timespec  ts = { 0, 5 * 1000 * 1000 };
            (void) nanosleep(&ts, NULL);
        }
    }

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
    CHECK(base == 0, "no sender threads before the pool starts: %lu",
          (unsigned long) base);

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

    after_start = thread_count_settled(base + NGX_MEDIA_SRT_EGRESS_SHARDS);
    CHECK(after_start == base + NGX_MEDIA_SRT_EGRESS_SHARDS,
          "exactly %ui egress shard threads started: %lu -> %lu",
          NGX_MEDIA_SRT_EGRESS_SHARDS, (unsigned long) base,
          (unsigned long) after_start);

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
     * stop joins its senders before it returns, but pthread_join returns
     * when the kernel clears the thread's tid (CLONE_CHILD_CLEARTID, in
     * mm_release) - a moment before the exiting task leaves
     * /proc/self/task.  So the count is allowed that moment to settle; a
     * sender stop did not end keeps running and never leaves, and still
     * fails this.
     */
    {
        ngx_uint_t  left = thread_count_settled(base);

        CHECK(left == base, "stopping joined every sender: %lu -> %lu",
              (unsigned long) after_media, (unsigned long) left);
    }
}

/*
 * A backend that records the multiplexer group each destination connects in
 * and otherwise behaves as the UDP test double.  Destinations are told apart
 * by their stream id, "dN".
 */
#define RECORD_MAX  32

static ngx_media_srt_ops_t  recording_ops;
static pthread_mutex_t      recording_lock = PTHREAD_MUTEX_INITIALIZER;
static ngx_int_t            recorded_group[RECORD_MAX];
static ngx_uint_t           plain_connects;

static ngx_media_srt_session_t *
recording_connect(const u_char *host, ngx_uint_t port,
    const u_char *streamid, size_t streamid_len, ngx_msec_t timeout_ms,
    const ngx_media_srt_params_t *params, ngx_log_t *log)
{
    (void) pthread_mutex_lock(&recording_lock);
    plain_connects++;
    (void) pthread_mutex_unlock(&recording_lock);

    return ngx_media_srt_udp_ops.connect(host, port, streamid, streamid_len,
                                         timeout_ms, params, log);
}

static ngx_media_srt_session_t *
recording_connect_shared(const u_char *host, ngx_uint_t port,
    const u_char *streamid, size_t streamid_len, ngx_msec_t timeout_ms,
    const ngx_media_srt_params_t *params, ngx_uint_t group, ngx_log_t *log)
{
    int  n;

    if (streamid != NULL && streamid_len > 1 && streamid[0] == 'd'
        && sscanf((const char *) streamid + 1, "%d", &n) == 1
        && n >= 0 && n < RECORD_MAX)
    {
        (void) pthread_mutex_lock(&recording_lock);
        recorded_group[n] = (ngx_int_t) group;
        (void) pthread_mutex_unlock(&recording_lock);
    }

    return ngx_media_srt_udp_ops.connect(host, port, streamid, streamid_len,
                                         timeout_ms, params, log);
}

/*
 * A destination's logical lane is its transport multiplexer group, so a
 * lane's destinations share one library endpoint and the library's thread
 * count follows the lanes rather than the fanout.  The group must be the same
 * for every destination of a lane and nonzero (zero means "private").
 */
static void
test_lane_multiplexer_groups(void)
{
    ngx_media_srt_listener_t    *listener;
    ngx_media_srt_outputs_t     *outs = NULL;
    ngx_media_srt_output_conf_t  conf;
    ngx_media_srt_out_event_t    events[64];
    ngx_media_srt_session_t     *session;
    u_char                       ids[RECORD_MAX][8];
    ngx_uint_t                   seen[RECORD_MAX];
    ngx_uint_t                   total = NGX_MEDIA_SRT_EGRESS_SHARDS + 2;
    ngx_uint_t                   connected, i, n, tries;

    TEST_CASE("destinations connect in their lane's multiplexer group");

    recording_ops = ngx_media_srt_udp_ops;
    recording_ops.connect = recording_connect;
    recording_ops.connect_shared = recording_connect_shared;
    for (i = 0; i < RECORD_MAX; i++) {
        recorded_group[i] = -1;
        seen[i] = 0;
    }
    plain_connects = 0;
    ngx_media_srt_set_backend(&recording_ops);

    listener = ngx_media_srt_listen((const u_char *) "127.0.0.1", 24591, NULL,
                                    NULL);
    CHECK(listener != NULL, "listener created");

    memset(&conf, 0, sizeof(conf));
    conf.application.len = 4;
    conf.application.data = (u_char *) "live";
    conf.stream.len = 4;
    conf.stream.data = (u_char *) "news";
    conf.host.len = 9;
    conf.host.data = (u_char *) "127.0.0.1";
    conf.port = 24591;
    conf.max_units = 64;
    conf.max_bytes = 256 * 1024;
    conf.connect_timeout = 1000;
    conf.send_timeout = 1000;

    CHECK(ngx_media_srt_outputs_start(&outs, NULL, 0, 64, NULL) == NGX_OK,
          "outputs started");

    for (i = 0; i < total; i++) {
        conf.streamid.len = (size_t) snprintf((char *) ids[i], sizeof(ids[i]),
                                              "d%lu", (unsigned long) i);
        conf.streamid.data = ids[i];
        CHECK(ngx_media_srt_outputs_add(outs, &conf, NULL, NULL) == NGX_OK,
              "destination %lu added", (unsigned long) i);
    }

    connected = 0;
    for (tries = 0; connected < total && tries < 500; tries++) {
        n = ngx_media_srt_outputs_event_read(outs, events, 64);
        for (i = 0; i < n; i++) {
            if (events[i].type == NGX_MEDIA_SRT_OUT_EVENT_CONNECTED
                && events[i].index < total && !seen[events[i].index])
            {
                seen[events[i].index] = 1;
                connected++;
            }
        }
        if (connected < total) {
            struct timespec  ts = { 0, 10 * 1000 * 1000 };

            (void) nanosleep(&ts, NULL);
        }
    }
    CHECK(connected == total, "every destination connected: %lu of %lu",
          (unsigned long) connected, (unsigned long) total);

    ngx_media_srt_outputs_stop(outs);

    for (i = 0; i < total; i++) {
        CHECK(recorded_group[i]
              == (ngx_int_t) (i % NGX_MEDIA_SRT_EGRESS_SHARDS) + 1,
              "destination %lu connected in its lane's group: %ld",
              (unsigned long) i, (long) recorded_group[i]);
    }
    CHECK(recorded_group[0] == recorded_group[NGX_MEDIA_SRT_EGRESS_SHARDS],
          "two destinations of one lane share a group");
    CHECK(plain_connects == 0, "no destination fell back to a private "
          "connect: %lu", (unsigned long) plain_connects);

    /* a backend without groups is still connected to, privately */
    recording_ops.connect_shared = NULL;
    session = ngx_media_srt_connect_shared((const u_char *) "127.0.0.1",
                                           24591, NULL, 0, 1000, NULL, 3,
                                           NULL);
    CHECK(session != NULL, "a backend without groups still connects");
    CHECK(plain_connects == 1, "and it does so through connect()");
    if (session != NULL) {
        ngx_media_srt_session_close(session);
    }

    /* group 0 is a private endpoint even when the backend has groups */
    recording_ops.connect_shared = recording_connect_shared;
    session = ngx_media_srt_connect_shared((const u_char *) "127.0.0.1",
                                           24591, NULL, 0, 1000, NULL, 0,
                                           NULL);
    CHECK(session != NULL && plain_connects == 2,
          "group 0 connects privately");
    if (session != NULL) {
        ngx_media_srt_session_close(session);
    }

    ngx_media_srt_listen_close(listener);
    ngx_media_srt_set_backend(&ngx_media_srt_udp_ops);
}

static pthread_mutex_t connect_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t connect_cond = PTHREAD_COND_INITIALIZER;
static int connect_entered, connect_continue, connect_strings_intact;

static ngx_media_srt_session_t *
held_connect(const u_char *host, ngx_uint_t port, const u_char *streamid,
    size_t streamid_len, ngx_msec_t timeout_ms,
    const ngx_media_srt_params_t *params, ngx_uint_t group, ngx_log_t *log)
{
    (void) group;
    (void) pthread_mutex_lock(&connect_mutex);
    connect_entered = 1;
    (void) pthread_cond_broadcast(&connect_cond);
    while (!connect_continue) {
        (void) pthread_cond_wait(&connect_cond, &connect_mutex);
    }
    connect_strings_intact = strcmp((const char *) host, "127.0.0.1") == 0
        && streamid_len == 5 && memcmp(streamid, "owned", 5) == 0;
    (void) pthread_mutex_unlock(&connect_mutex);
    return ngx_media_srt_udp_ops.connect(host, port, streamid, streamid_len,
                                         timeout_ms, params, log);
}

static void
test_connecting_removal_owns_strings_and_model(void)
{
    ngx_media_srt_ops_t ops = ngx_media_srt_udp_ops;
    ngx_media_srt_outputs_t *outs = NULL;
    ngx_media_srt_output_conf_t conf;
    ngx_media_stream_t stream;
    ngx_media_destination_t *destination;
    ngx_pool_t *pool;
    ngx_cycle_t cycle;
    ngx_media_policy_t *policy;
    ngx_media_feed_conf_t feed = { .max_units = 4 };
    ngx_str_t app = { 4, (u_char *) "live" };
    ngx_str_t name = { 4, (u_char *) "news" };
    ngx_str_t id = { 4, (u_char *) "edge" };
    ngx_str_t host = { 9, (u_char *) "127.0.0.1" };
    ngx_str_t sid = { 5, (u_char *) "owned" };
    ngx_uint_t index, old_limit, tries;
    struct timespec deadline;

    TEST_CASE("connecting removal is nonblocking and retains immutable strings");
    pool = ngx_create_pool(4096, NULL);
    TEST_ASSERT_NOT_NULL(pool);
    cycle.pool = pool;
    cycle.log = NULL;
    policy = ngx_media_policy_get(&cycle);
    TEST_ASSERT_NOT_NULL(policy);
    old_limit = policy->destination_limit;
    policy->destination_limit = 1;
    TEST_ASSERT_EQ_INT(ngx_media_stream_init(&stream, pool, NULL, &app, &name,
                                            &feed), NGX_OK);
    destination = ngx_media_destination_add(&stream, &id, NGX_MEDIA_DEST_SRT,
                                             NULL);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_EQ_INT(ngx_media_string_set(&destination->host, &host, NULL),
                       NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_string_set(&destination->streamid, &sid, NULL),
                       NGX_OK);
    memset(&conf, 0, sizeof(conf));
    conf.application = stream.application;
    conf.stream = stream.name;
    conf.host = destination->host;
    conf.streamid = destination->streamid;
    conf.port = 24592;
    conf.max_units = 4;
    conf.max_bytes = 4096;
    conf.connect_timeout = 100;
    conf.program_identity = (uintptr_t) &stream;
    conf.incarnation = stream.incarnation;
    conf.model = destination;
    connect_entered = connect_continue = connect_strings_intact = 0;
    ops.connect_shared = held_connect;
    ngx_media_srt_set_backend(&ops);
    TEST_ASSERT_EQ_INT(ngx_media_srt_outputs_start(&outs, NULL, 0, 16, NULL),
                       NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_srt_outputs_add(outs, &conf, &index, NULL),
                       NGX_OK);
    (void) clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    (void) pthread_mutex_lock(&connect_mutex);
    while (!connect_entered) {
        if (pthread_cond_timedwait(&connect_cond, &connect_mutex, &deadline)
            != 0)
        {
            break;
        }
    }
    TEST_ASSERT(connect_entered);
    (void) pthread_mutex_unlock(&connect_mutex);
    /* The paused connect must see the slot's immutable copy, not these bytes. */
    memset(destination->host.data, 'x', destination->host.len);
    memset(destination->streamid.data, 'y', destination->streamid.len);
    ngx_media_srt_outputs_remove(outs, index);
    ngx_media_destination_remove(&stream, destination);
    TEST_ASSERT_EQ_U64(ngx_media_model_count(NGX_MEDIA_MODEL_DESTINATION), 1);
    TEST_ASSERT_EQ_U64(stream.references, 1);
    TEST_ASSERT_NULL(ngx_media_destination_add(&stream, &id,
                                               NGX_MEDIA_DEST_SRT, NULL));
    ngx_media_stream_retire(&stream);
    (void) pthread_mutex_lock(&connect_mutex);
    connect_continue = 1;
    (void) pthread_cond_broadcast(&connect_cond);
    (void) pthread_mutex_unlock(&connect_mutex);
    for (tries = 0; tries < 600; tries++) {
        if (ngx_media_model_count(NGX_MEDIA_MODEL_DESTINATION) == 0) {
            break;
        }
        (void) nanosleep(&(struct timespec) { 0, 5000000 }, NULL);
    }
    ngx_media_srt_outputs_stop(outs);
    TEST_ASSERT(connect_strings_intact);
    TEST_ASSERT_EQ_U64(ngx_media_model_count(NGX_MEDIA_MODEL_DESTINATION), 0);
    TEST_ASSERT_EQ_U64(stream.references, 0);
    policy->destination_limit = old_limit;
    ngx_media_stream_destroy(&stream);
    ngx_destroy_pool(pool);
    ngx_media_srt_set_backend(&ngx_media_srt_udp_ops);
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
    test_lane_multiplexer_groups();
    test_connecting_removal_owns_strings_and_model();

    TEST_LEAKS();
    TEST_MAIN_END();
}

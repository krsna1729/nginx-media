#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include <stdio.h>
#include <time.h>

#include "ngx_media_egress_manager.h"

#include <string.h>

typedef struct {
    ngx_media_egress_stats_t stats;
    ngx_uint_t               count;
} ngx_media_egress_test_capture_t;

static ngx_int_t
capture_egress(const ngx_media_egress_stats_t *stats, void *ctx)
{
    ngx_media_egress_test_capture_t  *capture = ctx;

    capture->stats = *stats;
    capture->count++;

    return NGX_OK;
}

static void
test_manager_tracks_destination_lifecycle_and_quality(void)
{
    static const u_char  app_data[] = "studio";
    static const u_char  stream_data[] = "program";
    static const u_char  destination_data[] = "archive-a";
    ngx_media_egress_descriptor_t   descriptor;
    ngx_media_egress_report_t       report;
    ngx_media_egress_test_capture_t capture;
    uint64_t                        id = 0;

    TEST_CASE("egress manager reports per-destination lifecycle and quality");

    ngx_memzero(&descriptor, sizeof(descriptor));
    descriptor.application.data = (u_char *) app_data;
    descriptor.application.len = sizeof(app_data) - 1;
    descriptor.stream.data = (u_char *) stream_data;
    descriptor.stream.len = sizeof(stream_data) - 1;
    descriptor.destination.data = (u_char *) destination_data;
    descriptor.destination.len = sizeof(destination_data) - 1;
    descriptor.stream_incarnation = 17;
    descriptor.representation_id = 23;
    descriptor.representation_epoch = 4;
    descriptor.feed_id = 23;
    descriptor.feed_epoch = 8;
    descriptor.protocol = NGX_MEDIA_DEST_SRT;
    descriptor.engine = 1;
    descriptor.placement = 3;
    descriptor.deadline_msec = 750;
    descriptor.required_payload_bps = 8000000;
    descriptor.cpu_budget_usec = 12000;

    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id, NULL),
                       NGX_OK);
    TEST_ASSERT(id != 0);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_count(), 1);

    ngx_memzero(&report, sizeof(report));
    report.delivered_bytes = 8192;
    report.dropped_units = 2;
    report.transport_errors = 1;
    report.backpressure_events = 3;
    report.reconnects = 4;
    report.deadline_misses = 5;
    report.queue_bytes = 4096;
    report.queue_lag_msec = 125;
    report.placement = 7;
    ngx_media_egress_manager_report(id, &report);

    ngx_memzero(&capture, sizeof(capture));
    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_visit(capture_egress, &capture),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(capture.count, 1);
    TEST_ASSERT_EQ_U64(capture.stats.destination_id, id);
    TEST_ASSERT_EQ_U64(capture.stats.stream_incarnation, 17);
    TEST_ASSERT_EQ_U64(capture.stats.representation_id, 23);
    TEST_ASSERT_EQ_U64(capture.stats.representation_epoch, 4);
    TEST_ASSERT_EQ_U64(capture.stats.feed_id, 23);
    TEST_ASSERT_EQ_U64(capture.stats.feed_epoch, 8);
    TEST_ASSERT_EQ_U64(capture.stats.deadline_msec, 750);
    TEST_ASSERT_EQ_U64(capture.stats.required_payload_bps, 8000000);
    TEST_ASSERT_EQ_U64(capture.stats.cpu_budget_usec, 12000);
    TEST_ASSERT_EQ_U64(capture.stats.delivered_bytes, 8192);
    TEST_ASSERT_EQ_U64(capture.stats.dropped_units, 2);
    TEST_ASSERT_EQ_U64(capture.stats.transport_errors, 1);
    TEST_ASSERT_EQ_U64(capture.stats.backpressure_events, 3);
    TEST_ASSERT_EQ_U64(capture.stats.reconnects, 4);
    TEST_ASSERT_EQ_U64(capture.stats.deadline_misses, 5);
    TEST_ASSERT_EQ_U64(capture.stats.queue_bytes, 4096);
    TEST_ASSERT_EQ_U64(capture.stats.queue_lag_msec, 125);
    TEST_ASSERT_EQ_U64(capture.stats.placement, 7);
    TEST_ASSERT(capture.stats.application.len == sizeof(app_data) - 1
                && ngx_memcmp(capture.stats.application.data, app_data,
                              sizeof(app_data) - 1) == 0);
    TEST_ASSERT(capture.stats.stream.len == sizeof(stream_data) - 1
                && ngx_memcmp(capture.stats.stream.data, stream_data,
                              sizeof(stream_data) - 1) == 0);
    TEST_ASSERT(capture.stats.destination.len == sizeof(destination_data) - 1
                && ngx_memcmp(capture.stats.destination.data, destination_data,
                              sizeof(destination_data) - 1) == 0);

    ngx_media_egress_manager_release(id);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_count(), 0);

    ngx_media_egress_manager_report(id, &report);
    ngx_memzero(&capture, sizeof(capture));
    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_visit(capture_egress, &capture),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(capture.count, 0);
}

static void
test_manager_rejects_incomplete_identity(void)
{
    ngx_media_egress_descriptor_t  descriptor;
    uint64_t                       id = 0;

    TEST_CASE("egress manager rejects an incomplete destination identity");

    ngx_memzero(&descriptor, sizeof(descriptor));
    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id, NULL),
                       NGX_ERROR);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_count(), 0);
}

static void
test_rtmp_event_loop_activity_tracks_admission(void)
{
    static const u_char  app_data[] = "studio";
    static const u_char  stream_data[] = "program";
    static const u_char  destination_a[] = "rtmp-a";
    static const u_char  destination_b[] = "rtmp-b";
    ngx_media_egress_descriptor_t  descriptor;
    ngx_media_egress_resources_t   resources;
    uint64_t                       id_a = 0, id_b = 0;

    TEST_CASE("RTMP activity stays on its owning event-loop worker");

    ngx_memzero(&descriptor, sizeof(descriptor));
    descriptor.application.data = (u_char *) app_data;
    descriptor.application.len = sizeof(app_data) - 1;
    descriptor.stream.data = (u_char *) stream_data;
    descriptor.stream.len = sizeof(stream_data) - 1;
    descriptor.destination.data = (u_char *) destination_a;
    descriptor.destination.len = sizeof(destination_a) - 1;
    descriptor.protocol = NGX_MEDIA_DEST_RTMP;
    descriptor.engine = NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP;

    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id_a, NULL),
                       NGX_OK);
    ngx_media_egress_manager_resources_get(&resources);
    TEST_ASSERT_EQ_U64(resources.active_workers[
                           NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP], 1);

    descriptor.destination.data = (u_char *) destination_b;
    descriptor.destination.len = sizeof(destination_b) - 1;
    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id_b, NULL),
                       NGX_OK);
    ngx_media_egress_manager_release(id_a);
    ngx_media_egress_manager_resources_get(&resources);
    TEST_ASSERT_EQ_U64(resources.active_workers[
                           NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP], 1);

    ngx_media_egress_manager_release(id_b);
    ngx_media_egress_manager_resources_get(&resources);
    TEST_ASSERT_EQ_U64(resources.active_workers[
                           NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP], 0);
}

/* past the policy's two-second hysteresis between changes */
static void
settle(void)
{
    struct timespec  ts = { 2, 100 * 1000 * 1000 };

    (void) nanosleep(&ts, NULL);
}

static void
test_srt_senders_follow_lanes_up_to_granted_cpus(void)
{
    static const u_char  app_data[] = "studio";
    static const u_char  stream_data[] = "program";
    ngx_media_egress_descriptor_t  descriptor;
    uint64_t                       ids[6];
    u_char                         names[6][32];
    ngx_uint_t                     i;

    TEST_CASE("SRT senders follow the lanes in use, up to the granted CPUs");

    ngx_memzero(&descriptor, sizeof(descriptor));
    descriptor.application.data = (u_char *) app_data;
    descriptor.application.len = sizeof(app_data) - 1;
    descriptor.stream.data = (u_char *) stream_data;
    descriptor.stream.len = sizeof(stream_data) - 1;
    descriptor.protocol = NGX_MEDIA_DEST_SRT;
    descriptor.engine = NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD;

    /* six destinations on three lanes: 0, 1, 2, 0, 1, 2 */
    for (i = 0; i < 6; i++) {
        descriptor.destination.len = (size_t) snprintf(
            (char *) names[i], sizeof(names[i]), "srt://receiver-%lu",
            (unsigned long) i);
        descriptor.destination.data = names[i];
        descriptor.placement = i % 3;
        TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor,
                                                          &ids[i], NULL),
                           NGX_OK);
    }

    /* no pressure at all: the target is structural, not reactive */
    ngx_media_egress_manager_worker_resources(4000, 100, 100);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 1, 1, 16),
                       3);

    /* hysteresis: no second change inside two seconds */
    ngx_media_egress_manager_worker_resources(2000, 100, 100);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 3, 1, 16),
                       3);

    /* fewer CPUs granted (a tighter cgroup quota) cap it */
    settle();
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 3, 1, 16),
                       2);

    /* more CPUs than lanes: one sender per lane in use, no more */
    settle();
    ngx_media_egress_manager_worker_resources(16000, 100, 100);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 2, 1, 16),
                       3);

    /* the pool's own ceiling still holds */
    settle();
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 3, 1, 2),
                       2);

    /* lanes emptied: back to the minimum */
    for (i = 0; i < 6; i++) {
        ngx_media_egress_manager_release(ids[i]);
    }
    settle();
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 3, 1, 16),
                       1);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_count(), 0);
}

static void
test_idle_srt_senders_do_not_crowd_out_hls(void)
{
    static const u_char  app_data[] = "studio";
    static const u_char  stream_data[] = "program";
    static const u_char  destination_a[] = "https://edge-a/live";
    static const u_char  destination_b[] = "https://edge-b/live";
    ngx_media_egress_descriptor_t  descriptor;
    ngx_media_egress_report_t      report;
    uint64_t                       id_a = 0, id_b = 0;

    TEST_CASE("idle SRT senders count for the CPU they use, not one each");

    ngx_memzero(&descriptor, sizeof(descriptor));
    descriptor.application.data = (u_char *) app_data;
    descriptor.application.len = sizeof(app_data) - 1;
    descriptor.stream.data = (u_char *) stream_data;
    descriptor.stream.len = sizeof(stream_data) - 1;
    descriptor.protocol = NGX_MEDIA_DEST_HLS_PUSH;
    descriptor.engine = NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL;
    descriptor.destination.data = (u_char *) destination_a;
    descriptor.destination.len = sizeof(destination_a) - 1;
    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id_a, NULL),
                       NGX_OK);
    descriptor.destination.data = (u_char *) destination_b;
    descriptor.destination.len = sizeof(destination_b) - 1;
    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id_b, NULL),
                       NGX_OK);

    /*
     * Four CPUs, three of them for egress; four SRT senders active but each
     * at 5% of a core.  Counted as threads they would leave the HLS pool no
     * room at all; counted by use they take one CPU.
     */
    ngx_media_egress_manager_worker_resources(4000, 100, 100);
    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 50, 4);
    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 600, 1);

    ngx_memzero(&report, sizeof(report));
    report.queue_bytes = 1024 * 1024;
    report.queue_lag_msec = 100;
    ngx_media_egress_manager_report(id_a, &report);
    ngx_media_egress_manager_report(id_b, &report);
    (void) ngx_media_egress_manager_recommend_workers(
        NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4);

    /* backlogs growing on both: the pool may grow past one */
    report.queue_bytes = 2 * 1024 * 1024;
    report.queue_lag_msec = 200;
    ngx_media_egress_manager_report(id_a, &report);
    ngx_media_egress_manager_report(id_b, &report);
    (void) ngx_media_egress_manager_recommend_workers(
        NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4);
    report.queue_bytes = 3 * 1024 * 1024;
    report.queue_lag_msec = 300;
    ngx_media_egress_manager_report(id_a, &report);
    ngx_media_egress_manager_report(id_b, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       2);

    /* the same senders busy (90% each) do take the room */
    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 900, 4);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 2, 1, 4),
                       1);

    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 0, 0);
    ngx_media_egress_manager_release(id_a);
    ngx_media_egress_manager_release(id_b);
    settle();  /* the next case starts outside this one's change hold */
}

static void
test_hls_backlog_trend_requires_multiple_destinations(void)
{
    static const u_char  app_data[] = "studio";
    static const u_char  stream_data[] = "program";
    static const u_char  destination_a[] = "https://edge-a/live";
    static const u_char  destination_b[] = "https://edge-b/live";
    ngx_media_egress_descriptor_t  descriptor;
    ngx_media_egress_report_t      report;
    uint64_t                       id_a = 0, id_b = 0;
    ngx_uint_t                     i;

    TEST_CASE("HLS startup backlog drains before pool expansion");

    ngx_memzero(&descriptor, sizeof(descriptor));
    descriptor.application.data = (u_char *) app_data;
    descriptor.application.len = sizeof(app_data) - 1;
    descriptor.stream.data = (u_char *) stream_data;
    descriptor.stream.len = sizeof(stream_data) - 1;
    descriptor.destination.data = (u_char *) destination_a;
    descriptor.destination.len = sizeof(destination_a) - 1;
    descriptor.protocol = NGX_MEDIA_DEST_HLS_PUSH;
    descriptor.engine = NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL;

    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id_a, NULL),
                       NGX_OK);
    ngx_media_egress_manager_worker_resources(4000, 100, 100);
    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 100, 1);

    ngx_memzero(&report, sizeof(report));
    ngx_media_egress_manager_report(id_a, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       1);

    report.queue_bytes = 2 * 1024 * 1024;
    report.queue_lag_msec = 200;
    ngx_media_egress_manager_report(id_a, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       1);

    report.queue_bytes = 1536 * 1024;
    report.queue_lag_msec = 150;
    ngx_media_egress_manager_report(id_a, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       1);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       1);

    descriptor.destination.data = (u_char *) destination_b;
    descriptor.destination.len = sizeof(destination_b) - 1;
    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id_b, NULL),
                       NGX_OK);
    ngx_media_egress_manager_report(id_b, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       1);

    report.queue_bytes = 1600 * 1024;
    report.queue_lag_msec = 160;
    ngx_media_egress_manager_report(id_a, &report);
    report.queue_bytes = 2100 * 1024;
    report.queue_lag_msec = 210;
    ngx_media_egress_manager_report(id_b, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       1);

    report.queue_bytes = 1700 * 1024;
    report.queue_lag_msec = 170;
    ngx_media_egress_manager_report(id_a, &report);
    report.queue_bytes = 2200 * 1024;
    report.queue_lag_msec = 220;
    ngx_media_egress_manager_report(id_b, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       2);

    for (i = 0; i < 10; i++) {
        report.queue_bytes = (1700 + (i + 1) * 100) * 1024;
        report.queue_lag_msec = 170 + (i + 1) * 10;
        ngx_media_egress_manager_report(id_a, &report);
        report.queue_bytes = (2200 + (i + 1) * 100) * 1024;
        report.queue_lag_msec = 220 + (i + 1) * 10;
        ngx_media_egress_manager_report(id_b, &report);
        TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                               NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL,
                               2, 1, 4), 2);
    }

    ngx_media_egress_manager_worker_resources(1000, 100, 100);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 2, 1, 4),
                       1);

    ngx_media_egress_manager_release(id_a);
    ngx_media_egress_manager_release(id_b);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_count(), 0);
}

int
main(void)
{
    test_manager_tracks_destination_lifecycle_and_quality();
    test_manager_rejects_incomplete_identity();
    test_rtmp_event_loop_activity_tracks_admission();
    test_srt_senders_follow_lanes_up_to_granted_cpus();
    test_idle_srt_senders_do_not_crowd_out_hls();
    test_hls_backlog_trend_requires_multiple_destinations();
    TEST_LEAKS();
    TEST_MAIN_END();
}

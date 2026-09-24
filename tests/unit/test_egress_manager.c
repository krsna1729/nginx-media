#include "ngx_media_test.h"

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

static void
test_worker_budget_requires_local_pressure_and_cpu_headroom(void)
{
    static const u_char  app_data[] = "studio";
    static const u_char  stream_data[] = "program";
    static const u_char  destination_a[] = "srt://receiver-a";
    static const u_char  destination_b[] = "srt://receiver-b";
    ngx_media_egress_descriptor_t  descriptor;
    ngx_media_egress_report_t      report;
    uint64_t                       id_a = 0, id_b = 0;

    TEST_CASE("SRT scaling requires independent pressured shards");

    ngx_memzero(&descriptor, sizeof(descriptor));
    descriptor.application.data = (u_char *) app_data;
    descriptor.application.len = sizeof(app_data) - 1;
    descriptor.stream.data = (u_char *) stream_data;
    descriptor.stream.len = sizeof(stream_data) - 1;
    descriptor.destination.data = (u_char *) destination_a;
    descriptor.destination.len = sizeof(destination_a) - 1;
    descriptor.protocol = NGX_MEDIA_DEST_SRT;
    descriptor.engine = NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD;
    descriptor.placement = 0;

    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id_a, NULL),
                       NGX_OK);
    descriptor.destination.data = (u_char *) destination_b;
    descriptor.destination.len = sizeof(destination_b) - 1;
    descriptor.placement = 1;
    TEST_ASSERT_EQ_INT(ngx_media_egress_manager_admit(&descriptor, &id_b, NULL),
                       NGX_OK);

    ngx_media_egress_manager_worker_resources(4000, 100, 100);
    ngx_media_egress_manager_engine_load(
        NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 900, 1);

    ngx_memzero(&report, sizeof(report));
    report.transport_errors = 100;
    ngx_media_egress_manager_report(id_a, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 1, 1, 16),
                       1);

    report.queue_bytes = 2 * 1024 * 1024;
    report.queue_lag_msec = 200;
    report.backpressure_events = 1;
    ngx_media_egress_manager_report(id_a, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 1, 1, 16),
                       1);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 1, 1, 16),
                       1);

    report.placement = 1;
    ngx_media_egress_manager_report(id_b, &report);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 1, 1, 16),
                       1);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 1, 1, 16),
                       2);

    ngx_media_egress_manager_worker_resources(4000, 900, 100);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 2, 1, 16),
                       2);

    ngx_media_egress_manager_worker_resources(4000, 100, 300);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD, 2, 1, 16),
                       2);

    ngx_media_egress_manager_release(id_a);
    ngx_media_egress_manager_release(id_b);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_count(), 0);
}

static void
test_hls_budget_requires_multiple_pressured_destinations(void)
{
    static const u_char  app_data[] = "studio";
    static const u_char  stream_data[] = "program";
    static const u_char  destination_a[] = "https://edge-a/live";
    static const u_char  destination_b[] = "https://edge-b/live";
    ngx_media_egress_descriptor_t  descriptor;
    ngx_media_egress_report_t      report;
    uint64_t                       id_a = 0, id_b = 0;
    ngx_uint_t                     i;

    TEST_CASE("HLS pool scales only for multiple pressured destinations");

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
    report.queue_bytes = 2 * 1024 * 1024;
    report.queue_lag_msec = 200;
    report.backpressure_events = 1;
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
    ngx_media_egress_manager_worker_resources(3000, 100, 100);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       1);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       1);

    ngx_media_egress_manager_worker_resources(4000, 100, 100);
    TEST_ASSERT_EQ_U64(ngx_media_egress_manager_recommend_workers(
                           NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL, 1, 1, 4),
                       2);

    for (i = 0; i < 10; i++) {
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
    test_worker_budget_requires_local_pressure_and_cpu_headroom();
    test_hls_budget_requires_multiple_pressured_destinations();
    TEST_LEAKS();
    TEST_MAIN_END();
}

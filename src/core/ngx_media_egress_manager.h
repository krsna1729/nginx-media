#ifndef NGX_MEDIA_EGRESS_MANAGER_H
#define NGX_MEDIA_EGRESS_MANAGER_H

#include "ngx_media_destination.h"

#define NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD        1
#define NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP  2
#define NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL  3
#define NGX_MEDIA_EGRESS_ENGINE_SHARED           0

#define NGX_MEDIA_EGRESS_ENGINE_MAX  3

typedef struct {
    ngx_str_t   application;
    ngx_str_t   stream;
    ngx_str_t   destination;

    uint64_t    stream_incarnation;
    uint64_t    representation_id;
    uint64_t    representation_epoch;
    uint64_t    feed_id;
    uint64_t    feed_epoch;

    ngx_uint_t  protocol;
    ngx_uint_t  engine;
    ngx_uint_t  placement;

    ngx_msec_t  deadline_msec;
    uint64_t    required_payload_bps;
    uint64_t    cpu_budget_usec;
} ngx_media_egress_descriptor_t;

typedef struct {
    uint64_t    delivered_bytes;
    uint64_t    dropped_units;
    uint64_t    transport_errors;
    uint64_t    backpressure_events;
    uint64_t    reconnects;
    uint64_t    deadline_misses;

    size_t      queue_bytes;
    ngx_msec_t  queue_lag_msec;
    ngx_uint_t  placement;
} ngx_media_egress_report_t;

typedef struct {
    uint64_t    destination_id;
    uint64_t    program_id;
    uint64_t    stream_incarnation;
    uint64_t    representation_id;
    uint64_t    representation_epoch;
    uint64_t    feed_id;
    uint64_t    feed_epoch;

    ngx_str_t   application;
    ngx_str_t   stream;
    ngx_str_t   destination;

    ngx_uint_t  protocol;
    ngx_uint_t  engine;
    ngx_uint_t  placement;
    ngx_uint_t  active;

    ngx_msec_t  deadline_msec;
    uint64_t    required_payload_bps;
    uint64_t    cpu_budget_usec;

    uint64_t    delivered_bytes;
    uint64_t    dropped_units;
    uint64_t    transport_errors;
    uint64_t    backpressure_events;
    uint64_t    reconnects;
    uint64_t    deadline_misses;

    size_t      queue_bytes;
    ngx_msec_t  queue_lag_msec;
} ngx_media_egress_stats_t;

typedef struct {
    ngx_uint_t  runnable_destinations;
    ngx_uint_t  write_blocked_destinations;
    uint64_t    visit_destinations;
    uint64_t    visit_bytes_queued;
    uint64_t    visit_units_pumped;
    uint64_t    visit_service_usec;
    uint64_t    destinations_visited_total;
    uint64_t    bytes_queued_total;
    uint64_t    units_pumped_total;
    uint64_t    service_usec_total;
    uint64_t    reposts_total;
    ngx_msec_t  oldest_runnable_age_msec;
} ngx_media_rtmp_scheduler_stats_t;


typedef struct {
    ngx_uint_t  available_cpu_milli;
    ngx_uint_t  worker_cpu_permille;
    ngx_msec_t  event_loop_lag_msec;
    ngx_uint_t  sample_valid;
    ngx_uint_t  active_workers[NGX_MEDIA_EGRESS_ENGINE_MAX + 1];
    ngx_uint_t  engine_cpu_permille[NGX_MEDIA_EGRESS_ENGINE_MAX + 1];
    ngx_media_rtmp_scheduler_stats_t rtmp_scheduler;
} ngx_media_egress_resources_t;

typedef ngx_int_t (*ngx_media_egress_visit_pt)(
    const ngx_media_egress_stats_t *stats, void *ctx);

/*
 * Worker-local admission and telemetry.  The protocol adapters retain all
 * transport, connection, and event-loop ownership; this registry stores only
 * stable logical identity, placement, resource targets, and reported health.
 */
ngx_int_t ngx_media_egress_manager_admit(
    const ngx_media_egress_descriptor_t *descriptor, uint64_t *destination_id,
    ngx_log_t *log);
void ngx_media_egress_manager_release(uint64_t destination_id);
void ngx_media_egress_manager_report(uint64_t destination_id,
    const ngx_media_egress_report_t *report);
ngx_uint_t ngx_media_egress_manager_count(void);
ngx_int_t ngx_media_egress_manager_visit(ngx_media_egress_visit_pt visit,
    void *ctx);
void ngx_media_egress_manager_worker_sample(ngx_msec_t event_loop_lag_msec);
void ngx_media_egress_manager_worker_resources(ngx_uint_t available_cpu_milli,
    ngx_uint_t worker_cpu_permille, ngx_msec_t event_loop_lag_msec);
void ngx_media_egress_manager_engine_load(ngx_uint_t engine,
    ngx_uint_t cpu_permille, ngx_uint_t active_workers);
void ngx_media_egress_manager_fixed_workers_set(ngx_uint_t engine,
    ngx_uint_t workers);
ngx_uint_t ngx_media_egress_manager_fixed_workers(ngx_uint_t engine);
ngx_uint_t ngx_media_egress_manager_recommend_workers(ngx_uint_t engine,
    ngx_uint_t current, ngx_uint_t minimum, ngx_uint_t maximum);
void ngx_media_egress_manager_resources_get(
    ngx_media_egress_resources_t *resources);
void ngx_media_egress_manager_rtmp_scheduler_report(
    const ngx_media_rtmp_scheduler_stats_t *stats);

#endif /* NGX_MEDIA_EGRESS_MANAGER_H */

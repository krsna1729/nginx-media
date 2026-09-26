#define _GNU_SOURCE
#include "ngx_media_egress_manager.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef struct ngx_media_egress_record_s ngx_media_egress_record_t;

struct ngx_media_egress_record_s {
    ngx_media_egress_record_t  *next;
    ngx_media_egress_stats_t    stats;
    uint64_t                    sampled_dropped_units;
    uint64_t                    sampled_backpressure_events;
    size_t                      sampled_queue_bytes;
    ngx_msec_t                  sampled_queue_lag_msec;
    ngx_uint_t                  sampled_queue_valid;
    u_char                      labels[];
};

static ngx_media_egress_record_t  *ngx_media_egress_records;
static pthread_mutex_t             ngx_media_egress_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t                    ngx_media_egress_next_id;
static ngx_uint_t                  ngx_media_egress_count;

typedef struct {
    ngx_uint_t  pressure_samples;
    ngx_uint_t  quiet_samples;
    ngx_msec_t  last_change_msec;
} ngx_media_egress_engine_state_t;

static ngx_media_egress_resources_t      ngx_media_egress_resources;
static ngx_media_egress_engine_state_t   ngx_media_egress_engines[
    NGX_MEDIA_EGRESS_ENGINE_MAX + 1];
static uint64_t                          ngx_media_egress_last_wall_ns;
static ngx_uint_t                    ngx_media_egress_fixed_workers[
    NGX_MEDIA_EGRESS_ENGINE_MAX + 1];
static uint64_t                          ngx_media_egress_last_cpu_ns;
static pthread_mutex_t                   ngx_media_egress_capacity_mutex =
    PTHREAD_MUTEX_INITIALIZER;
static uint64_t                          ngx_media_egress_capacity_checked_ns;
static ngx_uint_t                        ngx_media_egress_capacity_cached;

static uint64_t
ngx_media_egress_hash(const ngx_str_t *application, const ngx_str_t *stream)
{
    uint64_t  hash = UINT64_C(14695981039346656037);
    size_t    i;

    for (i = 0; i < application->len; i++) {
        hash = (hash ^ application->data[i]) * UINT64_C(1099511628211);
    }

    hash = (hash ^ 0xff) * UINT64_C(1099511628211);

    for (i = 0; i < stream->len; i++) {
        hash = (hash ^ stream->data[i]) * UINT64_C(1099511628211);
    }

    return hash ? hash : 1;
}

static uint64_t
ngx_media_egress_counter_add(uint64_t current, uint64_t delta)
{
    return (UINT64_MAX - current < delta) ? UINT64_MAX : current + delta;
}

static uint64_t
ngx_media_egress_clock_ns(clockid_t clock_id)
{
    struct timespec  ts;

    if (clock_gettime(clock_id, &ts) != 0) {
        return 0;
    }

    return (uint64_t) ts.tv_sec * UINT64_C(1000000000)
           + (uint64_t) ts.tv_nsec;
}

static ngx_uint_t
ngx_media_egress_cpu_quota(const char *path)
{
    char      buffer[128], *end, *p;
    int       fd;
    ssize_t   n;
    uint64_t  quota, period, milli;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }

    n = read(fd, buffer, sizeof(buffer) - 1);
    (void) close(fd);

    if (n <= 0) {
        return 0;
    }

    buffer[n] = '\0';
    p = buffer;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (strncmp(p, "max", 3) == 0) {
        return 0;
    }

    errno = 0;
    quota = strtoull(p, &end, 10);
    if (errno != 0 || end == p) {
        return 0;
    }

    p = end;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    errno = 0;
    period = strtoull(p, &end, 10);
    if (errno != 0 || end == p || period == 0 || quota == 0) {
        return 0;
    }

    milli = (quota > UINT64_MAX / 1000)
                ? UINT64_MAX : quota * 1000 / period;

    if (milli == 0) {
        milli = 1;
    }

    return (milli > (uint64_t) (ngx_uint_t) -1)
               ? (ngx_uint_t) -1 : (ngx_uint_t) milli;
}

static ngx_uint_t
ngx_media_egress_cpu_quota_for_process(void)
{
    static const char  root[] = "/sys/fs/cgroup";
    static const char  suffix[] = "/cpu.max";
    char               cgroup[2048], path[4096];
    char              *line, *end;
    int                fd;
    ssize_t            n;
    size_t             root_len, path_len;
    ngx_uint_t         quota;

    fd = open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        n = read(fd, cgroup, sizeof(cgroup) - 1);
        (void) close(fd);

        if (n > 0) {
            cgroup[n] = '\0';
            root_len = sizeof(root) - 1;

            for (line = cgroup; *line != '\0'; line = end + 1) {
                end = strchr(line, '\n');
                if (end == NULL) {
                    end = line + strlen(line);
                }

                if (end - line >= 3 && memcmp(line, "0::", 3) == 0) {
                    path_len = (size_t) (end - line - 3);

                    if (root_len + path_len + sizeof(suffix)
                        <= sizeof(path))
                    {
                        ngx_memcpy(path, root, root_len);
                        ngx_memcpy(path + root_len, line + 3, path_len);
                        ngx_memcpy(path + root_len + path_len, suffix,
                                   sizeof(suffix));
                        quota = ngx_media_egress_cpu_quota(path);
                        if (quota != 0) {
                            return quota;
                        }
                    }

                    break;
                }

                if (*end == '\0') {
                    break;
                }
            }
        }
    }

    return ngx_media_egress_cpu_quota("/sys/fs/cgroup/cpu.max");
}

static ngx_uint_t
ngx_media_egress_cpu_capacity_milli(void)
{
    cpu_set_t     allowed;
    uint64_t      now;
    ngx_uint_t    affinity_milli, quota, i, count;
    long          online;

    now = ngx_media_egress_clock_ns(CLOCK_MONOTONIC);

    (void) pthread_mutex_lock(&ngx_media_egress_capacity_mutex);
    if (ngx_media_egress_capacity_cached != 0 && now != 0
        && now - ngx_media_egress_capacity_checked_ns
           < UINT64_C(1000000000))
    {
        affinity_milli = ngx_media_egress_capacity_cached;
        (void) pthread_mutex_unlock(&ngx_media_egress_capacity_mutex);
        return affinity_milli;
    }
    (void) pthread_mutex_unlock(&ngx_media_egress_capacity_mutex);

    CPU_ZERO(&allowed);
    count = 0;

    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        for (i = 0; i < CPU_SETSIZE; i++) {
            if (CPU_ISSET(i, &allowed)) {
                count++;
            }
        }
    }

    if (count == 0) {
        online = sysconf(_SC_NPROCESSORS_ONLN);
        count = (online > 0) ? (ngx_uint_t) online : 1;
    }

    affinity_milli = (count > (ngx_uint_t) -1 / 1000)
                         ? (ngx_uint_t) -1 : count * 1000;
    quota = ngx_media_egress_cpu_quota_for_process();
    if (quota != 0 && quota < affinity_milli) {
        affinity_milli = quota;
    }

    (void) pthread_mutex_lock(&ngx_media_egress_capacity_mutex);
    if (now != 0
        && now - ngx_media_egress_capacity_checked_ns
           < UINT64_C(1000000000)
        && ngx_media_egress_capacity_cached != 0)
    {
        affinity_milli = ngx_media_egress_capacity_cached;

    } else {
        ngx_media_egress_capacity_cached = affinity_milli;
        ngx_media_egress_capacity_checked_ns = now;
    }
    (void) pthread_mutex_unlock(&ngx_media_egress_capacity_mutex);

    return affinity_milli;
}

static ngx_msec_t
ngx_media_egress_clock_msec(void)
{
    uint64_t  now = ngx_media_egress_clock_ns(CLOCK_MONOTONIC);

    return (ngx_msec_t) (now / UINT64_C(1000000));
}

ngx_int_t
ngx_media_egress_manager_admit(const ngx_media_egress_descriptor_t *descriptor,
    uint64_t *destination_id, ngx_log_t *log)
{
    ngx_media_egress_record_t  *record;
    u_char                     *p;
    size_t                      labels_len, bytes;
    uint64_t                    id;

    if (descriptor == NULL || destination_id == NULL
        || descriptor->application.data == NULL
        || descriptor->application.len == 0
        || descriptor->stream.data == NULL || descriptor->stream.len == 0
        || descriptor->destination.data == NULL
        || descriptor->destination.len == 0
        || (descriptor->protocol != NGX_MEDIA_DEST_SRT
            && descriptor->protocol != NGX_MEDIA_DEST_RTMP
            && descriptor->protocol != NGX_MEDIA_DEST_HLS_PUSH)
        || descriptor->engine == 0
        || descriptor->stream.len > SIZE_MAX - descriptor->application.len
        || descriptor->destination.len > SIZE_MAX - 3)
    {
        return NGX_ERROR;
    }

    labels_len = descriptor->application.len + descriptor->stream.len;
    if (labels_len > SIZE_MAX - descriptor->destination.len - 3) {
        return NGX_ERROR;
    }
    labels_len += descriptor->destination.len + 3;

    if (labels_len > SIZE_MAX - sizeof(ngx_media_egress_record_t)) {
        return NGX_ERROR;
    }

    bytes = sizeof(ngx_media_egress_record_t) + labels_len;
    record = ngx_alloc(bytes, log);
    if (record == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(record, sizeof(ngx_media_egress_record_t));
    record->stats.program_id = ngx_media_egress_hash(&descriptor->application,
                                                      &descriptor->stream);
    record->stats.stream_incarnation = descriptor->stream_incarnation;
    record->stats.representation_id = descriptor->representation_id;
    record->stats.representation_epoch = descriptor->representation_epoch;
    record->stats.feed_id = descriptor->feed_id;
    record->stats.feed_epoch = descriptor->feed_epoch;
    record->stats.protocol = descriptor->protocol;
    record->stats.engine = descriptor->engine;
    record->stats.placement = descriptor->placement;
    record->stats.active = 1;
    record->stats.deadline_msec = descriptor->deadline_msec;
    record->stats.required_payload_bps = descriptor->required_payload_bps;
    record->stats.cpu_budget_usec = descriptor->cpu_budget_usec;

    p = record->labels;
    record->stats.application.data = p;
    record->stats.application.len = descriptor->application.len;
    ngx_memcpy(p, descriptor->application.data, descriptor->application.len);
    p += descriptor->application.len;
    *p++ = '\0';

    record->stats.stream.data = p;
    record->stats.stream.len = descriptor->stream.len;
    ngx_memcpy(p, descriptor->stream.data, descriptor->stream.len);
    p += descriptor->stream.len;
    *p++ = '\0';

    record->stats.destination.data = p;
    record->stats.destination.len = descriptor->destination.len;
    ngx_memcpy(p, descriptor->destination.data, descriptor->destination.len);
    p += descriptor->destination.len;
    *p = '\0';

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);

    id = ++ngx_media_egress_next_id;
    if (id == 0) {
        id = ++ngx_media_egress_next_id;
    }
    record->stats.destination_id = id;
    record->next = ngx_media_egress_records;
    ngx_media_egress_records = record;
    ngx_media_egress_count++;
    if (descriptor->engine == NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP) {
        ngx_media_egress_resources.active_workers[
            NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP] = 1;
    }

    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);

    *destination_id = id;
    return NGX_OK;
}

void
ngx_media_egress_manager_release(uint64_t destination_id)
{
    ngx_media_egress_record_t  **link, *record, *current;

    if (destination_id == 0) {
        return;
    }

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);

    for (link = &ngx_media_egress_records; *link != NULL;
         link = &(*link)->next)
    {
        if ((*link)->stats.destination_id == destination_id) {
            record = *link;
            *link = record->next;
            ngx_media_egress_count--;
            if (record->stats.engine
                == NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP)
            {
                ngx_media_egress_resources.active_workers[
                    NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP] = 0;
                for (current = ngx_media_egress_records; current != NULL;
                     current = current->next)
                {
                    if (current->stats.active
                        && current->stats.engine
                           == NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP)
                    {
                        ngx_media_egress_resources.active_workers[
                            NGX_MEDIA_EGRESS_ENGINE_RTMP_EVENT_LOOP] = 1;
                        break;
                    }
                }
            }
            (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
            ngx_free(record);
            return;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
}

void
ngx_media_egress_manager_report(uint64_t destination_id,
    const ngx_media_egress_report_t *report)
{
    ngx_media_egress_record_t  *record;

    if (destination_id == 0 || report == NULL) {
        return;
    }

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);

    for (record = ngx_media_egress_records; record != NULL;
         record = record->next)
    {
        if (record->stats.destination_id != destination_id) {
            continue;
        }

        record->stats.delivered_bytes = ngx_media_egress_counter_add(
            record->stats.delivered_bytes, report->delivered_bytes);
        record->stats.dropped_units = ngx_media_egress_counter_add(
            record->stats.dropped_units, report->dropped_units);
        record->stats.transport_errors = ngx_media_egress_counter_add(
            record->stats.transport_errors, report->transport_errors);
        record->stats.backpressure_events = ngx_media_egress_counter_add(
            record->stats.backpressure_events, report->backpressure_events);
        record->stats.reconnects = ngx_media_egress_counter_add(
            record->stats.reconnects, report->reconnects);
        record->stats.deadline_misses = ngx_media_egress_counter_add(
            record->stats.deadline_misses, report->deadline_misses);
        record->stats.queue_bytes = report->queue_bytes;
        record->stats.queue_lag_msec = report->queue_lag_msec;
        record->stats.placement = report->placement;
        break;
    }

    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
}

ngx_uint_t
ngx_media_egress_manager_count(void)
{
    ngx_uint_t  count;

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);
    count = ngx_media_egress_count;
    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);

    return count;
}

ngx_int_t
ngx_media_egress_manager_visit(ngx_media_egress_visit_pt visit, void *ctx)
{
    ngx_media_egress_record_t  *record;
    ngx_int_t                   rc = NGX_OK;

    if (visit == NULL) {
        return NGX_ERROR;
    }

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);

    for (record = ngx_media_egress_records; record != NULL;
         record = record->next)
    {
        rc = visit(&record->stats, ctx);
        if (rc != NGX_OK) {
            break;
        }
    }

    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);

    return rc;
}

void
ngx_media_egress_manager_worker_resources(ngx_uint_t available_cpu_milli,
    ngx_uint_t worker_cpu_permille, ngx_msec_t event_loop_lag_msec)
{
    (void) pthread_mutex_lock(&ngx_media_egress_mutex);

    ngx_media_egress_resources.available_cpu_milli = available_cpu_milli;
    ngx_media_egress_resources.worker_cpu_permille =
        (worker_cpu_permille > 1000) ? 1000 : worker_cpu_permille;
    ngx_media_egress_resources.event_loop_lag_msec = event_loop_lag_msec;
    ngx_media_egress_resources.sample_valid = available_cpu_milli != 0;

    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
}

void
ngx_media_egress_manager_worker_sample(ngx_msec_t event_loop_lag_msec)
{
    uint64_t      wall_ns, cpu_ns, wall_delta, cpu_delta, used_cpu_milli;
    ngx_uint_t    capacity, worker_permille;

    wall_ns = ngx_media_egress_clock_ns(CLOCK_MONOTONIC);
    cpu_ns = ngx_media_egress_clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    capacity = ngx_media_egress_cpu_capacity_milli();

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);

    ngx_media_egress_resources.event_loop_lag_msec = event_loop_lag_msec;
    if (capacity != 0) {
        ngx_media_egress_resources.available_cpu_milli = capacity;
    }

    if (wall_ns == 0 || cpu_ns == 0 || capacity == 0) {
        (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
        return;
    }

    if (ngx_media_egress_last_wall_ns == 0
        || wall_ns - ngx_media_egress_last_wall_ns < UINT64_C(1000000000))
    {
        if (ngx_media_egress_last_wall_ns == 0) {
            ngx_media_egress_last_wall_ns = wall_ns;
            ngx_media_egress_last_cpu_ns = cpu_ns;
        }

        (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
        return;
    }

    wall_delta = wall_ns - ngx_media_egress_last_wall_ns;
    cpu_delta = (cpu_ns >= ngx_media_egress_last_cpu_ns)
                    ? cpu_ns - ngx_media_egress_last_cpu_ns : 0;
    used_cpu_milli = cpu_delta * 1000 / wall_delta;
    worker_permille = (ngx_uint_t) (used_cpu_milli * 1000 / capacity);
    if (worker_permille > 1000) {
        worker_permille = 1000;
    }

    ngx_media_egress_resources.worker_cpu_permille = worker_permille;
    ngx_media_egress_resources.sample_valid = 1;
    ngx_media_egress_last_wall_ns = wall_ns;
    ngx_media_egress_last_cpu_ns = cpu_ns;

    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
}

void
ngx_media_egress_manager_engine_load(ngx_uint_t engine,
    ngx_uint_t cpu_permille, ngx_uint_t active_workers)
{
    if (engine == 0 || engine > NGX_MEDIA_EGRESS_ENGINE_MAX) {
        return;
    }

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);
    ngx_media_egress_resources.engine_cpu_permille[engine] =
        (cpu_permille > 1000) ? 1000 : cpu_permille;
    ngx_media_egress_resources.active_workers[engine] = active_workers;
    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
}
void
ngx_media_egress_manager_fixed_workers_set(ngx_uint_t engine,
    ngx_uint_t workers)
{
    if (engine != NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD
        && engine != NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL)
    {
        return;
    }

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);
    ngx_media_egress_fixed_workers[engine] = workers;
    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
}


ngx_uint_t
ngx_media_egress_manager_fixed_workers(ngx_uint_t engine)
{
    ngx_uint_t  workers;

    if (engine != NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD
        && engine != NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL)
    {
        return 0;
    }

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);
    workers = ngx_media_egress_fixed_workers[engine];
    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);

    return workers;
}

/*
 * The CPUs an engine's threads actually use - at most its active threads
 * times its busiest thread's load - rounded up; an engine whose threads are
 * idle uses none.  Counting a thread as a CPU whatever it does would let a
 * pool of mostly-sleeping senders crowd out an engine that is busy.
 */
static ngx_uint_t
ngx_media_egress_engine_cpus(ngx_uint_t engine)
{
    ngx_uint_t  used;

    used = ngx_media_egress_resources.active_workers[engine]
           * ngx_media_egress_resources.engine_cpu_permille[engine];

    return (used + 999) / 1000;
}

ngx_uint_t
ngx_media_egress_manager_recommend_workers(ngx_uint_t engine,
    ngx_uint_t current, ngx_uint_t minimum, ngx_uint_t maximum)
{
    ngx_media_egress_record_t        *record;
    ngx_media_egress_engine_state_t  *state;
    ngx_msec_t                        now;
    uint64_t                          placement_mask, lanes_mask, bit;
    ngx_uint_t                        pressure_units, cpu_limit, other_workers,
                                      next, record_pressure, lanes, cpus,
                                      target;

    if (engine == 0 || engine > NGX_MEDIA_EGRESS_ENGINE_MAX
        || minimum == 0 || maximum < minimum)
    {
        return current;
    }

    now = ngx_media_egress_clock_msec();
    (void) pthread_mutex_lock(&ngx_media_egress_mutex);

    if (!ngx_media_egress_resources.sample_valid) {
        (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
        return current;
    }

    state = &ngx_media_egress_engines[engine];
    pressure_units = 0;
    placement_mask = 0;
    lanes_mask = 0;

    for (record = ngx_media_egress_records; record != NULL;
         record = record->next)
    {
        if (!record->stats.active || record->stats.engine != engine) {
            continue;
        }

        if (engine == NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD
            && record->stats.placement < 64)
        {
            lanes_mask |= UINT64_C(1) << record->stats.placement;
        }

        record_pressure =
            record->stats.backpressure_events
                > record->sampled_backpressure_events
            || record->stats.dropped_units > record->sampled_dropped_units;

        if (engine == NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL) {
            if (record->sampled_queue_valid
                && (record->stats.queue_lag_msec
                        > record->sampled_queue_lag_msec
                    || record->stats.queue_bytes
                           > record->sampled_queue_bytes))
            {
                record_pressure = 1;
            }

        } else if (record->stats.queue_lag_msec >= 100
                   || record->stats.queue_bytes >= 1024 * 1024)
        {
            record_pressure = 1;
        }

        if (record_pressure) {
            /*
             * SRT lanes are stable logical placements.  One saturated shard
             * cannot be helped by adding physical senders; other engines use
             * one pressure unit per destination.
             */
            if (engine == NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD
                && record->stats.placement < 64)
            {
                bit = UINT64_C(1) << record->stats.placement;
                if ((placement_mask & bit) == 0) {
                    placement_mask |= bit;
                    pressure_units++;
                }

            } else {
                pressure_units++;
            }
        }

        record->sampled_backpressure_events =
            record->stats.backpressure_events;
        record->sampled_dropped_units = record->stats.dropped_units;
        record->sampled_queue_bytes = record->stats.queue_bytes;
        record->sampled_queue_lag_msec = record->stats.queue_lag_msec;
        record->sampled_queue_valid = 1;
    }

    /*
     * SRT senders follow the lanes, not pressure.  A sender hands its lanes'
     * bursts to their library endpoints and does little work itself (a few
     * percent of a core each), so what more of them buy is parallel
     * submission to the lane multiplexers: measured at 96 destinations on
     * 4 CPUs, CPU per delivered Gbit/s was 128% of a core with one active
     * sender, 106% with two, and 97% with four, eight or sixteen - flat from
     * one per CPU on.  Waiting for backpressure before adding one, and
     * counting each as a whole CPU, held the pool at one or two and cost a
     * quarter more CPU for the same delivery.  So the target is one sender
     * per lane in use, up to the CPUs this worker is granted (its affinity
     * and cgroup quota, measured at run time): a larger host gets more, a
     * smaller one fewer, and a program on one lane keeps one.
     */
    if (engine == NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD) {
        lanes = 0;
        for (bit = lanes_mask; bit != 0; bit &= bit - 1) {
            lanes++;
        }

        cpus = ngx_media_egress_resources.available_cpu_milli / 1000;
        if (cpus == 0) {
            cpus = 1;
        }

        target = (lanes < cpus) ? lanes : cpus;
        if (target > maximum) {
            target = maximum;
        }
        if (target < minimum) {
            target = minimum;
        }

        state->pressure_samples = 0;
        state->quiet_samples = 0;

        if (target != current
            && (state->last_change_msec == 0
                || now - state->last_change_msec >= 2000))
        {
            state->last_change_msec = now;
            (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
            return target;
        }

        (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
        return current;
    }

    cpu_limit = ngx_media_egress_resources.available_cpu_milli / 1000;
    if (cpu_limit > 0) {
        cpu_limit--;  /* reserve one CPU for the owning NGINX event loop */
    }

    /* the other engines count for the CPU their threads use */
    other_workers = 0;
    if (engine != NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD) {
        other_workers += ngx_media_egress_engine_cpus(
            NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD);
    }
    if (engine != NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL) {
        other_workers += ngx_media_egress_engine_cpus(
            NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL);
    }

    if (current + other_workers > cpu_limit) {
        state->pressure_samples = 0;
        state->quiet_samples = 0;
        if (current > minimum) {
            next = current - 1;
            state->last_change_msec = now;
            (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
            return next;
        }

        (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
        return current;
    }

    /*
     * Scale only when more independent pressured destinations/shards exist
     * than active workers; transport loss alone is not pressure.
     */
    if (pressure_units > current) {
        state->quiet_samples = 0;
        if (state->pressure_samples < 10) {
            state->pressure_samples++;
        }

        if (state->pressure_samples >= 2
            && ngx_media_egress_resources.worker_cpu_permille < 800
            && ngx_media_egress_resources.event_loop_lag_msec < 250
            && ngx_media_egress_resources.available_cpu_milli >= 2000
            && current < maximum
            && current + other_workers < cpu_limit
            && (state->last_change_msec == 0
                || now - state->last_change_msec >= 2000))
        {
            next = current + 1;
            state->last_change_msec = now;
            (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
            return next;
        }

        (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
        return current;
    }

    state->pressure_samples = 0;
    if (pressure_units != 0) {
        state->quiet_samples = 0;
        (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
        return current;
    }
    if (state->quiet_samples < 100) {
        state->quiet_samples++;
    }

    if (current > minimum
        && state->quiet_samples >= 8
        && ngx_media_egress_resources.engine_cpu_permille[engine] < 250
        && (state->last_change_msec == 0
            || now - state->last_change_msec >= 5000))
    {
        next = current - 1;
        state->last_change_msec = now;
        (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
        return next;
    }

    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
    return current;
}

void
ngx_media_egress_manager_resources_get(
    ngx_media_egress_resources_t *resources)
{
    if (resources == NULL) {
        return;
    }

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);
    *resources = ngx_media_egress_resources;
    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
}

void
ngx_media_egress_manager_rtmp_scheduler_report(
    const ngx_media_rtmp_scheduler_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    (void) pthread_mutex_lock(&ngx_media_egress_mutex);
    ngx_media_egress_resources.rtmp_scheduler = *stats;
    (void) pthread_mutex_unlock(&ngx_media_egress_mutex);
}

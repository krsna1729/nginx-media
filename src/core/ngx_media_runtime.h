#ifndef NGX_MEDIA_RUNTIME_H
#define NGX_MEDIA_RUNTIME_H

#include "ngx_media.h"
#include "ngx_media_owner_dir.h"
#include "ngx_media_route.h"
#include "ngx_media_rtmp_adapter.h"

/*
 * Shared program runtime (goal doc 22 and 33).
 *
 * The owning worker uses a periodic timer for selection, file-source pacing,
 * health and cleanup, plus coalesced posted wakeups for program outputs after
 * transport callbacks publish media.  SRT, RTMP and file input all feed the
 * same logical stream abstraction (goal doc 34 item 1).
 */

#define NGX_MEDIA_RUNTIME_INTERVAL        100
#define NGX_MEDIA_RUNTIME_MAX_OUTPUTS     16
#define NGX_MEDIA_RUNTIME_MAX_PREPARE     16
#define NGX_MEDIA_RUNTIME_MAX_FRAMES_TICK 64

/*
 * A visit is a fairness slice, not a drain-until-empty operation.  When work
 * remains, another posted media wakeup gives other nginx work a turn first.
 */
#define NGX_MEDIA_RUNTIME_VISIT_MAX_FRAMES 64
#define NGX_MEDIA_RUNTIME_VISIT_MAX_BYTES  (4 * 1024 * 1024)
#define NGX_MEDIA_RUNTIME_VISIT_MAX_MS     2
#define NGX_MEDIA_RUNTIME_HLS_TARGET      6000

/*
 * Worker initialisation: adopts the shared owner directory.  Every worker
 * calls this, not only the owner of a given stream.
 */
ngx_int_t ngx_media_runtime_init(ngx_cycle_t *cycle, ngx_log_t *log);

/* the shared owner directory of this worker, or NULL before init */
ngx_media_owner_dir_t *ngx_media_runtime_owner_dir(void);

/*
 * Where a stream's program progress is true from this worker's point of view.
 *
 * The graph is replicated, but the program is not: one worker drives a stream,
 * and only on that worker do the generation and the frame count describe the
 * media that was actually carried.  A read that lands on a replica reports the
 * numbers the owner published in the shared directory (goal doc 22) instead of
 * its own empty ones, and says which worker they came from - so "how far has
 * this program got" has one answer, whichever worker answers the request.
 */
typedef struct {
    ngx_uint_t  owner;          /* the worker that drives the program */
    ngx_uint_t  local;          /* 1 when the numbers are this worker's */
    uint64_t    generation;
    uint64_t    frames;
} ngx_media_runtime_progress_t;

void ngx_media_runtime_progress(const ngx_str_t *application,
    const ngx_str_t *name, uint64_t local_generation, uint64_t local_frames,
    ngx_media_runtime_progress_t *out);

/*
 * Ownership, derived rather than claimed.
 *
 * A stream's owner is the deterministic slot for its application/stream hash
 * (ngx_media_route_owner), so every worker computes the same answer on its
 * own.  This publishes the owner's shared-directory record - liveness,
 * generation and progress - and only the deterministic owner may write one:
 * ngx_media_owner_dir_claim refuses any other slot, so a record can only
 * repeat the hash answer and never introduce a third one.
 *
 * A worker that is not the owner writes nothing here.  The stream it created
 * is desired state that the graph carries to the owner, and publishing a
 * record naming itself is exactly what used to make the API and the driver
 * disagree about who owns the stream.
 */
void ngx_media_runtime_claim(const ngx_str_t *application,
    const ngx_str_t *name);
void ngx_media_runtime_release(const ngx_str_t *application,
    const ngx_str_t *name);

/* idempotent: arms the runtime timer in this worker, 1 when it did */
ngx_uint_t ngx_media_runtime_arm(ngx_cycle_t *cycle, ngx_log_t *log);
void ngx_media_runtime_stop(void);

/*
 * Post a coalesced media wakeup for this worker.  Transport callbacks call
 * this after publishing to an active program; the periodic timer remains the
 * health, file-source and cleanup cadence.
 */
void ngx_media_runtime_wakeup(void);

/* periodic maintenance plus one bounded media visit for this worker */
void ngx_media_runtime_periodic_visit(ngx_log_t *log);

/* posted media progress only; periodic maintenance never runs here */
void ngx_media_runtime_media_visit(ngx_log_t *log);

/*
 * Worker-level health, the numbers that say whether this worker still has
 * headroom (goal doc 32: event-loop delay is the first symptom of a worker
 * that is falling behind).
 *
 * `gap` is the interval between the last two timer-driven visits; posted
 * media wakeups deliberately do not affect it.  The timer asks for
 * NGX_MEDIA_RUNTIME_INTERVAL, so anything above it is time the event loop
 * spent unable to run this timer.  `late` counts timer visits that missed by
 * more than half an interval.
 */
typedef struct {
    uint64_t    ticks;          /* timer and posted scheduler visits */
    uint64_t    periodic_visits;    /* timer-driven maintenance visits */
    uint64_t    media_only_visits;  /* posted media-only visits */
    ngx_msec_t  last_gap;       /* timer-to-timer interval */
    ngx_msec_t  max_gap;
    uint64_t    late_ticks;     /* late periodic timer visits */
    /*
     * Both visit kinds are counted, while the timer cadence counters above
     * only describe periodic visits.
     */
    uint64_t    wakeups;
    uint64_t    wakeup_coalesced;
    uint64_t    budget_reposts;  /* budget exhaustion requeue requests */

    /*
     * Duration of the current runtime visit.  A periodic visit includes
     * maintenance and media progress; a posted visit includes media progress
     * only.
     */
    ngx_msec_t  last_service;
    ngx_msec_t  max_service;

    /* sources that are up but not yet carrying media, per doc 28 */
    ngx_uint_t  reconnecting;
    /* routed media drops, split by the bounded queue or protocol failure */
    uint64_t    routed_slot_overflows;
    uint64_t    routed_identity_mismatches;
    uint64_t    routed_no_slot;
    uint64_t    routed_no_payload;
    uint64_t    routed_reassembly_errors;
    uint64_t    routed_publish_errors;
} ngx_media_runtime_stats_t;


void ngx_media_runtime_stats_get(ngx_media_runtime_stats_t *out);

/* reserves a per-stream output slot before the owner accepts desired state */
ngx_int_t ngx_media_runtime_admit(ngx_media_stream_t *stream, ngx_log_t *log);

/* stops and releases per-stream outputs; called from exit_process */
void ngx_media_runtime_shutdown(ngx_log_t *log);
/*
 * Ordered teardown for everything the runtime keys by one stream: the outputs
 * (HLS, recording) flush and free their slots, the player preparation drops
 * the FLV conversion of the program, and the routed slots that would publish
 * into the stream are cleared, so a frame that arrives after the delete is
 * dropped instead of reaching memory that is going away.  Must run before the
 * stream's feed is destroyed, because the flush reads it.
 */
void ngx_media_runtime_stream_release(ngx_media_stream_t *stream);

/* runtime output slots in use; a leak in ordered teardown shows up here */
ngx_uint_t ngx_media_runtime_outputs_active(void);

/*
 * Sink for the prepared transport bursts.  The SRT output registers here, so
 * HLS, recording and SRT destinations all consume the same preparation and a
 * destination costs one reference per burst (goal doc 16, 34 item 11).
 */
typedef void (*ngx_media_runtime_sink_pt)(void *ctx, ngx_media_stream_t *stream,
    ngx_media_buf_t *burst, size_t len, ngx_uint_t keyframe);

void ngx_media_runtime_set_sink(ngx_media_runtime_sink_pt cb, void *ctx);

/* the shared FLV preparation of a stream, created on demand */
ngx_media_rtmp_prepare_t *ngx_media_runtime_prepare(ngx_media_stream_t *stream,
    ngx_log_t *log);

/* ISO tap: one named source's frames before the selector */
void ngx_media_runtime_iso_source(ngx_media_stream_t *stream,
    ngx_media_source_t *source, const ngx_media_frame_t *frame);

/* RAW tap: transport bytes before normalization */
ngx_uint_t ngx_media_runtime_raw_ready(void);
ngx_int_t ngx_media_runtime_raw_bytes(const u_char *data, size_t len);

#endif /* NGX_MEDIA_RUNTIME_H */

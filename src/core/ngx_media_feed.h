#ifndef NGX_MEDIA_FEED_H
#define NGX_MEDIA_FEED_H

#include "ngx_media_platform.h"
#include "ngx_media_frame.h"

/*
 * Bounded program feed (goal doc 13).
 *
 * A feed is a single-producer ring of frame descriptors with hard retention
 * ceilings.  Payloads are shared by reference: publishing and reading never
 * copy payload bytes.  Consumers hold a cursor, never a private media queue.
 *
 *   cursor = { generation, next_sequence }
 *
 * read(feed, cursor, max_units, max_bytes) returns one of
 *   BATCH                - descriptors copied to the output, payload refs held
 *   EMPTY                - the consumer has caught up
 *   OVERRUN              - the consumer fell behind the retained tail
 *   GENERATION_MISMATCH  - cursor belongs to another generation or ring
 *
 * OVERRUN and GENERATION_MISMATCH leave the cursor untouched; the consumer
 * recovers with ngx_media_feed_resync().  read() guarantees progress: the
 * first available unit is always returned even when it alone exceeds
 * max_bytes, so a budget smaller than one frame cannot livelock a consumer.
 *
 * Retained media is bounded by units, bytes and media age.  Age is measured
 * from the publish time of the newest unit, which the producer supplies, so
 * the feed itself never reads a wall clock.
 */

#define NGX_MEDIA_FEED_ERROR               0
#define NGX_MEDIA_FEED_BATCH               1
#define NGX_MEDIA_FEED_EMPTY               2
#define NGX_MEDIA_FEED_OVERRUN             3
#define NGX_MEDIA_FEED_GENERATION_MISMATCH 4

#define NGX_MEDIA_FEED_RESYNC_LATEST       0
#define NGX_MEDIA_FEED_RESYNC_KEYFRAME     1

#define NGX_MEDIA_FEED_NO_KEYFRAME         ((uint64_t) -1)

typedef struct {
    uint64_t  generation;
    uint64_t  next_sequence;
} ngx_media_cursor_t;

typedef struct {
    ngx_uint_t  max_units;  /* hard retained-unit ceiling, > 0 */
    size_t      max_bytes;  /* hard retained-byte ceiling, 0 = unbounded */
    ngx_msec_t  max_age;    /* retained media age, 0 = unbounded */
} ngx_media_feed_conf_t;

typedef struct {
    ngx_media_frame_t  frame;
    ngx_msec_t         publish_time;
} ngx_media_feed_slot_t;

/*
 * fanout_delay = dispatch_time - program_publish_time (goal doc 32).
 *
 * The primary capacity metric, and the one an operator can feel: how long a
 * unit of media waits after the program publishes it before a consumer takes
 * it.  Recorded as a log2 histogram, because percentiles are what matter and
 * a histogram costs two operations on the dispatch path.
 */
#define NGX_MEDIA_FEED_HIST_BUCKETS  16

typedef struct {
    uint64_t  buckets[NGX_MEDIA_FEED_HIST_BUCKETS];  /* 0ms, 1ms, 2-3, 4-7 ... */
    uint64_t  count;
    uint64_t  max;
} ngx_media_feed_hist_t;

typedef struct {
    ngx_media_feed_slot_t  *slots;
    ngx_uint_t              capacity;   /* power of two, >= max_units */
    ngx_uint_t              max_units;
    size_t                  max_bytes;
    ngx_msec_t              max_age;
    uint64_t                generation; /* starts at 1; 0 is never valid */
    uint64_t                head;       /* next publish sequence */
    uint64_t                tail;       /* oldest retained sequence */
    uint64_t                last_keyframe;
    size_t                  bytes;      /* retained payload bytes */
    unsigned                has_keyframe:1;

    ngx_media_feed_hist_t   fanout;     /* dispatch delay, all consumers */
} ngx_media_feed_t;

ngx_int_t ngx_media_feed_init(ngx_media_feed_t *feed,
    const ngx_media_feed_conf_t *conf, ngx_log_t *log);
void ngx_media_feed_destroy(ngx_media_feed_t *feed);

ngx_int_t ngx_media_feed_publish(ngx_media_feed_t *feed,
    const ngx_media_frame_t *frame, ngx_msec_t now);

/*
 * now is the caller's clock.  The feed never reads one, but it does record how
 * long each dispatched unit waited, which is the only place that interval is
 * observable.
 */
ngx_uint_t ngx_media_feed_read(ngx_media_feed_t *feed,
    ngx_media_cursor_t *cursor, ngx_uint_t max_units, size_t max_bytes,
    ngx_msec_t now, ngx_media_frame_t *out, ngx_uint_t *out_count);

void ngx_media_feed_release(ngx_media_frame_t *frames, ngx_uint_t count);

void ngx_media_feed_discontinuity(ngx_media_feed_t *feed);
void ngx_media_feed_cursor_init(const ngx_media_feed_t *feed,
    ngx_media_cursor_t *cursor);
ngx_int_t ngx_media_feed_resync(const ngx_media_feed_t *feed,
    ngx_media_cursor_t *cursor, ngx_uint_t mode);

uint64_t ngx_media_feed_generation(const ngx_media_feed_t *feed);
uint64_t ngx_media_feed_head(const ngx_media_feed_t *feed);
uint64_t ngx_media_feed_tail(const ngx_media_feed_t *feed);
ngx_uint_t ngx_media_feed_units(const ngx_media_feed_t *feed);
size_t ngx_media_feed_bytes(const ngx_media_feed_t *feed);
uint64_t ngx_media_feed_last_keyframe(const ngx_media_feed_t *feed);

/*
 * The delay at the given percentile, in ms; 0 when nothing was dispatched.
 *
 * The percentile is in per-mille so that p99.9 - which goal doc 28 asks for -
 * is expressible: 500 is p50, 999 is p99.9, 1000 is the maximum.
 */
ngx_msec_t ngx_media_feed_fanout_percentile(const ngx_media_feed_t *feed,
    ngx_uint_t permille);
uint64_t ngx_media_feed_fanout_count(const ngx_media_feed_t *feed);
uint64_t ngx_media_feed_fanout_max(const ngx_media_feed_t *feed);

#endif /* NGX_MEDIA_FEED_H */

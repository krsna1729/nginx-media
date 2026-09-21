#ifndef NGX_MEDIA_HLS_PUSH_H
#define NGX_MEDIA_HLS_PUSH_H

#include "ngx_media.h"

/*
 * HLS HTTP PUT/POST destination (goal doc normative revision).
 *
 * The segmenter writes files; this publishes them to a remote HTTP endpoint
 * by watching the output directory and uploading what appears.  Watching the
 * directory rather than tapping the segmenter keeps the two independent: the
 * upload can be slow, retry, or fail entirely without the segmenter knowing.
 *
 * Scaling shape, which is the part that matters at fanout: uploads are
 * I/O-bound per destination, so the cost of N destinations is N uploads of
 * the same bytes, not N times the CPU.  Two consequences shaped this:
 *
 *   - **A bounded pool, not a thread per destination.**  One thread per
 *     destination would be a hundred threads at a hundred destinations, each
 *     mostly waiting on a socket.  A fixed pool of uploaders drains every
 *     destination's queue, so the thread count is a configured ceiling rather
 *     than a function of how many endpoints a controller adds.
 *   - **A bounded queue per destination.**  A stalled remote must not stall
 *     the program or its neighbours: when a queue is full the oldest entry is
 *     dropped and counted, never waited on.
 *
 * The segment bytes are read once per destination from the page cache, which
 * is why a push destination is cheap to add and why a memory filesystem buys
 * nothing here either - the reads are already memory-speed.
 */

typedef struct ngx_media_hls_push_t ngx_media_hls_push_t;

/* registers the backend and starts the upload pool */
ngx_int_t ngx_media_hls_push_register(ngx_log_t *log);

/* stops the pool and joins its threads; called at shutdown */
void ngx_media_hls_push_stop(void);

/*
 * Offers newly written files to every push destination watching this
 * directory.  Called from the runtime tick, so it never blocks: it stats a
 * bounded number of names and enqueues those it has not seen.
 */
void ngx_media_hls_push_scan(const ngx_str_t *directory, ngx_log_t *log);

/* totals across destinations, for the control API */
ngx_uint_t ngx_media_hls_push_count(void);
uint64_t ngx_media_hls_push_uploaded_total(void);
uint64_t ngx_media_hls_push_dropped_total(void);
uint64_t ngx_media_hls_push_failed_total(void);

#endif /* NGX_MEDIA_HLS_PUSH_H */

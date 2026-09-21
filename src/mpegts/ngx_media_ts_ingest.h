#ifndef NGX_MEDIA_TS_INGEST_H
#define NGX_MEDIA_TS_INGEST_H

#include "ngx_media_platform.h"

/*
 * Bounded MPEG-TS ingest queue (goal doc 11.2, 13).
 *
 * This is the handoff between the SRT transport side (a helper thread in the
 * nginx integration) and the worker that demuxes and publishes frames.  The
 * queue is hard bounded in chunks and bytes; the producer never blocks: when
 * a ceiling is reached the new chunk is dropped and counted, which is the
 * correct behaviour for live media (a stalled transport must not stall the
 * rest of the system, and the demuxer counts continuity damage anyway).
 *
 * Reads hand out chunk descriptors carrying the only reference to each
 * payload; the consumer releases them with ngx_media_ts_ingest_release().
 *
 * All operations are serialized by an internal spinlock, so one transport
 * thread and one worker thread may use a queue concurrently.
 */

#define NGX_MEDIA_TS_INGEST_CHUNK_MAX   65536

typedef struct {
    ngx_uint_t  max_chunks;   /* hard queued-chunk ceiling, > 0 */
    size_t      max_bytes;    /* hard queued-byte ceiling, > 0 */
} ngx_media_ts_ingest_conf_t;

typedef struct {
    uint64_t    bytes_in;
    uint64_t    chunks_in;
    uint64_t    bytes_dropped;
    uint64_t    chunks_dropped;
    ngx_msec_t  last_write;
} ngx_media_ts_ingest_stats_t;

typedef struct {
    u_char      *data;
    size_t       len;
    ngx_msec_t   time;
} ngx_media_ts_ingest_chunk_t;

typedef struct {
    ngx_media_ts_ingest_chunk_t  *chunks;
    ngx_uint_t                    capacity;   /* power of two */
    ngx_uint_t                    max_chunks;
    size_t                        max_bytes;
    uint64_t                      head;       /* next write sequence */
    uint64_t                      tail;       /* oldest queued sequence */
    size_t                        bytes;      /* queued bytes */
    ngx_media_ts_ingest_stats_t   stats;
    ngx_atomic_t                  lock;
} ngx_media_ts_ingest_t;

ngx_int_t ngx_media_ts_ingest_init(ngx_media_ts_ingest_t *ingest,
    const ngx_media_ts_ingest_conf_t *conf, ngx_log_t *log);
void ngx_media_ts_ingest_destroy(ngx_media_ts_ingest_t *ingest);

/* NGX_OK when queued, NGX_AGAIN when dropped because a ceiling was reached */
ngx_int_t ngx_media_ts_ingest_write(ngx_media_ts_ingest_t *ingest,
    const u_char *data, size_t len, ngx_msec_t now);

ngx_uint_t ngx_media_ts_ingest_read(ngx_media_ts_ingest_t *ingest,
    ngx_media_ts_ingest_chunk_t *out, ngx_uint_t max_chunks);
void ngx_media_ts_ingest_release(ngx_media_ts_ingest_chunk_t *chunks,
    ngx_uint_t count);

void ngx_media_ts_ingest_stats(ngx_media_ts_ingest_t *ingest,
    ngx_media_ts_ingest_stats_t *out);
ngx_uint_t ngx_media_ts_ingest_pending(ngx_media_ts_ingest_t *ingest);
size_t ngx_media_ts_ingest_bytes(ngx_media_ts_ingest_t *ingest);

#endif /* NGX_MEDIA_TS_INGEST_H */

#ifndef NGX_MEDIA_HLS_INGEST_H
#define NGX_MEDIA_HLS_INGEST_H

#include "ngx_media.h"
#include "ngx_media_ts_demux.h"

/*
 * HLS push source (normative revision, acceptance case 7): someone else PUTs
 * segments to us, and they become a source.
 *
 * The two halves are deliberately separate.  An HTTP location accepts the
 * upload and stores it in a directory; a directory source reads what arrives
 * and demuxes it into the normal source gate.  Neither knows about the other
 * beyond the directory, which means an upload can arrive over any mechanism -
 * this endpoint, an rsync, a shared filesystem - and still be a source.
 *
 *   PUT /seg-42.ts -> directory -> TS demux -> frames -> source gate
 *
 * The reader is a thread, like the pull source: reading a directory and
 * demuxing segments is sequential work, and the event loop must never wait on
 * it.  Health comes from the normal model, so an upload source is promoted
 * exactly like a publisher.
 */

typedef struct ngx_media_hls_ingest_source_s ngx_media_hls_ingest_source_t;

/*
 * Watches directory and publishes what appears, in name order.  Returns NULL
 * if the directory cannot be read.
 */
/*
 * log is kept by the reader thread, so it has to outlive the request: pass the
 * worker's log, never a connection's.
 */
ngx_media_hls_ingest_source_t *ngx_media_hls_ingest_open(
    ngx_media_stream_t *stream, const ngx_str_t *id, const ngx_str_t *directory,
    ngx_log_t *log);

/* stops the reader, joins it, and removes the source; safe to repeat */
void ngx_media_hls_ingest_close(ngx_media_hls_ingest_source_t *source);

/*
 * Closes the readers whose source was removed through the control API and
 * whose thread has already stopped.  Called from the periodic runtime visit:
 * a delete has to take the reader with it, and the reader holds a thread.
 */
void ngx_media_hls_ingest_reap(ngx_log_t *log);

/*
 * How many ingest readers still reference this stream, which is the
 * pool-release gate: a reader is unlinked by its close, and close() joins its
 * thread first, so a count of zero means no thread can still be reading the
 * stream's memory.
 */
ngx_uint_t ngx_media_hls_ingest_stream_readers(
    const ngx_media_stream_t *stream);
/* transfers queued reader events on its eventfd; periodic maintenance also
 * drains all readers as a bounded shutdown/delete safety net */
void ngx_media_hls_ingest_drain_all(void);

/* stops and joins every reader; called at shutdown */
void ngx_media_hls_ingest_stop_all(void);

#ifdef NGX_MEDIA_UNIT_TEST
/*
 * The order a reader would take a directory's segments in, without reading
 * them: up to max names, each copied into names[i] (cap bytes).  Returns the
 * count.  Successive calls continue where the previous one stopped, as the
 * reader does between scans; state is the opaque handle it returns.
 */
ngx_uint_t ngx_media_hls_ingest_order(void **state, const char *directory,
    char names[][256], ngx_uint_t max);
void ngx_media_hls_ingest_order_free(void *state);
ngx_int_t ngx_media_hls_ingest_name_cmp(const char *a, const char *b);
#endif

#endif /* NGX_MEDIA_HLS_INGEST_H */

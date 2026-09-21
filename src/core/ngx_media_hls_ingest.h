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
ngx_media_hls_ingest_source_t *ngx_media_hls_ingest_open(
    ngx_media_stream_t *stream, const ngx_str_t *id, const ngx_str_t *directory,
    ngx_log_t *log);

void ngx_media_hls_ingest_close(ngx_media_hls_ingest_source_t *source);

/* stops and joins every reader; called at shutdown */
void ngx_media_hls_ingest_stop_all(void);

#endif /* NGX_MEDIA_HLS_INGEST_H */

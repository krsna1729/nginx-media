#ifndef NGX_MEDIA_HLS_PULL_H
#define NGX_MEDIA_HLS_PULL_H

#include "ngx_media.h"
#include "ngx_media_ts_demux.h"

/*
 * HLS pull source (normative revision, acceptance case 6).
 *
 * A source that fetches a media playlist, fetches the segments it names, and
 * demuxes them into the normal source gate.  Everything downstream - health,
 * compatibility, selection, promotion - treats it exactly like a publisher,
 * which is the point: adding one at runtime and promoting it is the same
 * operation as promoting any other source.
 *
 *   playlist -> segment -> TS demux -> media frames -> source gate
 *
 * Each source owns a reader thread rather than being paced by the runtime
 * tick, because fetching is sequential and blocking by nature: the thread
 * waits for the origin between refreshes, and the event loop never does.
 * The thread count is therefore the number of pull sources an operator adds,
 * which is theirs to bound.
 */

typedef struct ngx_media_hls_pull_s ngx_media_hls_pull_t;

/*
 * Opens url as a source on stream, and starts its reader.  log is kept by the
 * reader thread, so it has to outlive the request: pass the worker's log,
 * never a connection's.
 */
ngx_media_hls_pull_t *ngx_media_hls_pull_open(ngx_media_stream_t *stream,
    const ngx_str_t *id, const ngx_str_t *url, const ngx_str_t *ca_file,
    ngx_log_t *log);

/* stops the reader, joins it, and removes the source; safe to repeat */
void ngx_media_hls_pull_close(ngx_media_hls_pull_t *pull);

/*
 * Closes the readers whose source was removed through the control API and
 * whose thread has already stopped.  Called from the runtime tick: a delete
 * has to take the reader with it, and the reader holds a thread.
 */
void ngx_media_hls_pull_reap(ngx_log_t *log);

/*
 * How many pull readers still reference this stream, which is the pool-release
 * gate: a reader is unlinked by its close, and close() joins its thread first,
 * so a count of zero means no thread can still be reading the stream's memory.
 */
ngx_uint_t ngx_media_hls_pull_stream_readers(const ngx_media_stream_t *stream);
/* transfers queued reader events to the owning worker; called from its tick */
void ngx_media_hls_pull_drain_all(void);

/* stops and joins every reader; called at shutdown */
void ngx_media_hls_pull_stop_all(void);

#endif /* NGX_MEDIA_HLS_PULL_H */

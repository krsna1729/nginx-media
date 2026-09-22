#ifndef NGX_MEDIA_FILE_H
#define NGX_MEDIA_FILE_H

#include "ngx_media.h"
#include "ngx_media_ts_demux.h"

/*
 * File input (goal doc 21): a first-class source that reads an MPEG-TS file,
 * demuxes it and publishes the frames through the normal source gate, so
 * selection, health and compatibility treat it exactly like a publisher.
 *
 *   file read -> TS demux -> media frames -> pacing -> source gate
 *
 * Pacing is the runtime tick, never a sleep: each call to
 * ngx_media_file_advance() reads a bounded chunk and publishes what it
 * demuxed, so a slow file cannot stall a worker.
 */

#define NGX_MEDIA_FILE_ONCE    0
#define NGX_MEDIA_FILE_LOOP    1

typedef struct ngx_media_file_source_s {
    ngx_media_stream_t      *stream;
    ngx_media_source_t      *source;
    ngx_media_ts_demux_t     demux;

    ngx_file_t               file;
    ngx_str_t                path;
    ngx_uint_t               mode;          /* NGX_MEDIA_FILE_* */

    uint64_t                 bytes_read;
    uint64_t                 frames;
    ngx_uint_t               finished;

    /*
     * One read buffer for the life of the reader.  Allocating it per advance
     * meant a 64 KiB block from the never-reclaimed worker pool every 100 ms
     * tick, so reading a 2 GB file left 2 GB of pool behind.
     */
    u_char                  *chunk;

    /* the runtime tick advances every open file source */
    struct ngx_media_file_source_s  *next;
} ngx_media_file_source_t;

/*
 * Opens path and attaches it to stream as a source with the given identity.
 * The source is registered immediately, so it appears in the control API and
 * in health before any media arrives, which is what makes a file slate a
 * normal source rather than a special case.
 */
/*
 * log is kept by the reader and used by every read it performs, so it has to
 * outlive the request: pass the worker's log, never a connection's.
 */
ngx_media_file_source_t *ngx_media_file_open(ngx_media_stream_t *stream,
    const ngx_str_t *id, const ngx_str_t *path, ngx_uint_t mode,
    ngx_log_t *log);

/*
 * Reads a bounded chunk, demuxes it and publishes the frames.  Returns
 * NGX_OK while there is more to read, NGX_DONE when the file is finished (in
 * ONCE mode) and NGX_ERROR when it could not be read.
 */
ngx_int_t ngx_media_file_advance(ngx_media_file_source_t *source,
    ngx_log_t *log);

void ngx_media_file_close(ngx_media_file_source_t *source);

/*
 * Closes every file reader reading this stream.  A file reader has no thread
 * - the tick paces it - so this is a close, not a wait: each reader is
 * unlinked, its descriptor released and its source removed, which is what has
 * to happen before the stream's memory goes away.
 */
void ngx_media_file_close_stream(ngx_media_stream_t *stream);

/*
 * Advances every open file source by one bounded chunk.  Called from the
 * runtime tick: pacing is the tick, never a sleep, so a slow or large file
 * cannot stall a worker.
 *
 * A source removed through the control API is found here, because this is
 * where the reader runs: it is taken off the registry and closed, so a delete
 * stops the file instead of leaving it advanced by every later tick.
 */
void ngx_media_file_advance_all(ngx_log_t *log);

/* open file sources, for diagnostics */
ngx_uint_t ngx_media_file_count(void);

#endif /* NGX_MEDIA_FILE_H */

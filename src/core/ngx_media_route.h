#ifndef NGX_MEDIA_ROUTE_H
#define NGX_MEDIA_ROUTE_H

#include "ngx_media.h"
#include "ngx_media_ipc.h"
#include "ngx_media_owner.h"
#include "ngx_media_owner_dir.h"
#include "ngx_media_runtime.h"

/*
 * Owner routing (goal doc 22, 23).
 *
 * A publisher may land on any worker; the program lives on exactly one.  When
 * they differ, the transport hands each frame to the routing layer, which
 * forwards it over the bounded inter-worker transport to the owner.  The owner
 * rebuilds the source locally and feeds it through the normal path, so the
 * selector, feed, HLS and recordings only ever see local state.
 *
 * Socket pairs are created in the master before workers fork, one per ordered
 * worker pair, so a worker adopts its own row without any connection setup.
 */

#define NGX_MEDIA_ROUTE_MAX_WORKERS 64

/* master: creates every worker-to-worker pair before forking */
ngx_int_t ngx_media_route_master_init(ngx_cycle_t *cycle, ngx_log_t *log);

/* worker: adopts its row of endpoints and registers them with the event loop */
ngx_int_t ngx_media_route_worker_init(ngx_cycle_t *cycle, ngx_log_t *log);
void ngx_media_route_worker_shutdown(ngx_log_t *log);

/* the slot that owns a stream, from the shared directory when it knows */
ngx_uint_t ngx_media_route_owner(ngx_cycle_t *cycle, uint32_t hash);

/* true when this worker owns the stream */
ngx_uint_t ngx_media_route_is_owner(ngx_cycle_t *cycle, uint32_t hash);

/*
 * Forwards one frame to the owner.  OPEN/CLOSE are sent by the transport when
 * a source appears or disappears; frames follow.  Returns NGX_AGAIN when the
 * owner is behind, so the caller drops to the next sync boundary instead of
 * queueing without bound.
 */
ngx_int_t ngx_media_route_open(ngx_cycle_t *cycle, uint32_t hash,
    const ngx_str_t *application, const ngx_str_t *stream,
    const ngx_str_t *source_id, ngx_uint_t source_type, ngx_uint_t priority);
ngx_int_t ngx_media_route_close(ngx_cycle_t *cycle, uint32_t hash);

/*
 * Forwards the track contract of a routed source.  The owner needs it before
 * the program can mux or segment anything, so the transport sends it as soon
 * as the source announces its tracks.
 */
ngx_int_t ngx_media_route_tracks(ngx_cycle_t *cycle, uint32_t hash,
    const ngx_media_trackset_t *tracks);
ngx_int_t ngx_media_route_frame(ngx_cycle_t *cycle, uint32_t hash,
    const ngx_media_frame_t *frame, uint64_t sequence);

/* frames received from other workers, handed to the program owner */
typedef ngx_int_t (*ngx_media_route_frame_pt)(void *ctx, uint32_t hash,
    const ngx_media_ipc_header_t *header, ngx_media_buf_t *payload);

void ngx_media_route_set_sink(ngx_media_route_frame_pt cb, void *ctx);

#endif /* NGX_MEDIA_ROUTE_H */

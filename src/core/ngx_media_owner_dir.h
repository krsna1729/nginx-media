#ifndef NGX_MEDIA_OWNER_DIR_H
#define NGX_MEDIA_OWNER_DIR_H

#include "ngx_media_owner.h"

/*
 * Shared metadata directory (goal doc 22).
 *
 * Small, bounded bookkeeping in shared memory: liveness and progress for a
 * stream - the worker that drives it (always the deterministic owner,
 * ngx_media_route_owner), its pid, the generation it is serving and a
 * heartbeat.  The record never decides ownership: it can only repeat the
 * deterministic answer, because ngx_media_owner_dir_claim refuses a slot that
 * is not that owner's.  It deliberately holds no mutable media state -
 * sources, feeds, HLS state and recordings stay in the owner process.
 *
 * The directory is an open-addressing table in one shared memory zone, guarded
 * by a single spinlock: writes happen when a stream appears, disappears or
 * heartbeats (once per second), never on the media path.
 */

typedef struct ngx_media_owner_dir_s ngx_media_owner_dir_t;

/*
 * One bounded directory is shared by the workers.  This is the deployment-wide
 * active-stream capacity for owner metadata; a claim beyond it is refused
 * rather than leaving a stream silently without liveness/progress.
 */
#define NGX_MEDIA_OWNER_DIR_SLOTS 256


/*
 * Master process: allocates the shared mapping before workers are forked, so
 * every worker inherits the same pages.  Worker: attaches to it.
 */
ngx_int_t ngx_media_owner_dir_shm_create(ngx_cycle_t *cycle, ngx_uint_t slots,
    ngx_log_t *log);
ngx_media_owner_dir_t *ngx_media_owner_dir_attach(ngx_cycle_t *cycle,
    ngx_log_t *log);

/*
 * Publishes a stream's owner.  Only the deterministic owner may claim: a slot
 * that is not ngx_media_route_owner(hash) is refused with NULL, so a record
 * can never name a worker the hash does not.
 */
ngx_media_owner_record_t *ngx_media_owner_dir_claim(
    ngx_media_owner_dir_t *dir, uint64_t hash, ngx_uint_t slot);

/* drops a stream's record (owner shutdown or stream removal) */
void ngx_media_owner_dir_release(ngx_media_owner_dir_t *dir, uint64_t hash,
    ngx_uint_t slot);

/*
 * The next revision of the one sequence every worker mutates from.  Two
 * workers' operations on one stream are then comparable, which is what lets
 * each replica resolve a conflict the same way; a counter per worker cannot,
 * because both workers call their own next number the same thing.
 */
uint64_t ngx_media_owner_dir_revision_next(ngx_media_owner_dir_t *dir);

/* refreshes liveness and progress for a stream this worker owns */
void ngx_media_owner_dir_heartbeat(ngx_media_owner_dir_t *dir, uint64_t hash,
    ngx_uint_t slot, uint64_t generation, uint64_t frames, ngx_uint_t sources);

/*
 * Reads the progress the owner of a stream published: owner slot, generation,
 * frames and source count.  NGX_OK only while the record is live - an owner
 * that stopped heartbeating reports as missing, so a reader can tell "the
 * owner is not reporting" from "the program has carried nothing yet".
 */
ngx_int_t ngx_media_owner_dir_observe(ngx_media_owner_dir_t *dir,
    uint64_t hash, ngx_media_owner_record_t *out);

/* copies up to max records; returns how many were written */
ngx_uint_t ngx_media_owner_dir_list(ngx_media_owner_dir_t *dir,
    ngx_media_owner_record_t *out, ngx_uint_t max);

#endif /* NGX_MEDIA_OWNER_DIR_H */

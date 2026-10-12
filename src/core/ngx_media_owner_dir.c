#include "ngx_media_owner_dir.h"

#ifndef NGX_MEDIA_UNIT_TEST

#include <ngx_config.h>
#include <ngx_core.h>

#include <sys/mman.h>

/*
 * The directory is one anonymous shared mapping created by the master before
 * workers are forked, so every worker sees the same pages without involving
 * the configuration-time shared memory API.
 */

#define NGX_MEDIA_OWNER_DIR_MAGIC 0x4D4F574Eu   /* "MOWN" */

typedef struct {
    uint32_t                    magic;
    uint32_t                    slots;

    /*
     * The active cycle's worker count and epoch.  Older workers may finish
     * their existing programs, but may not claim metadata back after reload.
     */
    uint32_t                    workers;
    uint64_t                    epoch;
    ngx_atomic_t                lock;

    /*
     * The revision sequence every worker takes its mutations from, so the
     * numbers are comparable across workers and a replica resolves a conflict
     * by the higher one instead of by whichever operation it saw last.
     */
    ngx_atomic_t                revision;

    ngx_media_owner_record_t    records[1];
} ngx_media_owner_dir_shm_t;

struct ngx_media_owner_dir_s {
    ngx_media_owner_dir_shm_t  *shm;
    ngx_uint_t                  slots;
    ngx_uint_t                  workers;
    uint64_t                    epoch;
    ngx_log_t                  *log;
};

/* an owner that stops heartbeating for this long may be taken over */
#define NGX_MEDIA_OWNER_STALE_MS 10000

static ngx_media_owner_dir_shm_t  *ngx_media_owner_dir_mapped;
static uint64_t                   ngx_media_owner_dir_epoch;

static ngx_int_t
ngx_media_owner_dir_lock(ngx_media_owner_dir_t *dir)
{
    ngx_uint_t  i;

    for (i = 0; i < 100000; i++) {

        if (ngx_atomic_cmp_set(&dir->shm->lock, 0, 1)) {
            return NGX_OK;
        }

        ngx_cpu_pause();
    }

    /*
     * Never enter the critical section unless this worker acquired it.  The
     * old final compare-and-swap was intentionally ignored, which turned
     * contention into fail-open concurrent access and could clear another
     * worker's lock on unlock.
     */
    ngx_log_error(NGX_LOG_WARN, dir->log, 0,
                  "media: owner directory lock is contended");
    return NGX_AGAIN;

}
static void
ngx_media_owner_dir_unlock(ngx_media_owner_dir_t *dir)
{
    (void) ngx_atomic_cmp_set(&dir->shm->lock, 1, 0);
}

static ngx_msec_t
ngx_media_owner_dir_now(void)
{
    return (ngx_msec_t) ngx_current_msec;
}

uint64_t
ngx_media_owner_dir_revision_next(ngx_media_owner_dir_t *dir)
{
    if (dir == NULL || dir->shm == NULL) {
        return 0;
    }

    /* one sequence for every worker: the first caller gets 1 */
    return (uint64_t) ngx_atomic_fetch_add(&dir->shm->revision, 1) + 1;
}

static ngx_uint_t
ngx_media_owner_dir_index(ngx_media_owner_dir_t *dir, uint64_t hash)
{
    return (ngx_uint_t) (hash % dir->slots);
}

ngx_int_t
ngx_media_owner_dir_shm_create(ngx_cycle_t *cycle, ngx_uint_t slots,
    ngx_log_t *log)
{
    ngx_media_owner_dir_shm_t  *shm;
    ngx_media_owner_dir_t       dir;
    size_t                     size;
    ngx_uint_t                 i;

    if (ngx_media_owner_dir_mapped != NULL) {
        ngx_memzero(&dir, sizeof(dir));
        dir.shm = ngx_media_owner_dir_mapped;
        dir.log = log;
        if (ngx_media_owner_dir_lock(&dir) != NGX_OK) {
            return NGX_ERROR;
        }

        dir.shm->workers = (uint32_t) ngx_media_owner_worker_count(cycle);
        ngx_media_owner_dir_epoch = ++dir.shm->epoch;
        ngx_media_owner_dir_unlock(&dir);

        return NGX_OK;
    }

    if (slots == 0) {
        slots = NGX_MEDIA_OWNER_DIR_SLOTS;
    }


    size = sizeof(ngx_media_owner_dir_shm_t)
           + (slots - 1) * sizeof(ngx_media_owner_record_t);

    shm = mmap(NULL, size, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (shm == MAP_FAILED) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "media: could not allocate the shared owner directory");
        return NGX_ERROR;
    }

    ngx_memzero(shm, size);

    shm->magic = NGX_MEDIA_OWNER_DIR_MAGIC;
    shm->slots = (uint32_t) slots;
    shm->workers = (uint32_t) ngx_media_owner_worker_count(cycle);
    shm->epoch = 1;

    for (i = 0; i < slots; i++) {
        shm->records[i].state = NGX_MEDIA_OWNER_STATE_FREE;
    }

    ngx_media_owner_dir_mapped = shm;
    ngx_media_owner_dir_epoch = shm->epoch;

    return NGX_OK;
}

ngx_media_owner_dir_t *
ngx_media_owner_dir_attach(ngx_cycle_t *cycle, ngx_log_t *log)
{
    ngx_media_owner_dir_t      *dir;
    ngx_media_owner_dir_shm_t  *shm = ngx_media_owner_dir_mapped;

    if (shm == NULL || shm->magic != NGX_MEDIA_OWNER_DIR_MAGIC
        || shm->slots == 0)
    {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "media: the shared owner directory is missing");
        return NULL;
    }

    dir = ngx_pcalloc(cycle->pool, sizeof(ngx_media_owner_dir_t));

    if (dir == NULL) {
        return NULL;
    }

    dir->shm = shm;
    dir->slots = shm->slots;
    dir->workers = ngx_media_owner_worker_count(cycle);
    dir->epoch = ngx_media_owner_dir_epoch; /* inherited from this fork's cycle */
    dir->log = log;

    return dir;
}

static ngx_uint_t
ngx_media_owner_dir_reclaimable(ngx_media_owner_record_t *record)
{
    if (record->state != NGX_MEDIA_OWNER_STATE_OWNED) {
        return 1;
    }

    /* A stale record can be reused for another hash.  Reload takeover of the
     * same hash is handled separately by claim's epoch and PID checks. */
    if (ngx_media_owner_dir_now() - (ngx_msec_t) record->heartbeat
        > NGX_MEDIA_OWNER_STALE_MS)
    {
        return 1;   /* the owner stopped heartbeating */
    }

    return 0;
}

static ngx_media_owner_record_t *
ngx_media_owner_dir_find(ngx_media_owner_dir_t *dir, uint64_t hash)
{
    ngx_uint_t                 i, index;
    ngx_media_owner_record_t  *record;
    ngx_media_owner_record_t  *first_avail = NULL;

    for (i = 0; i < dir->slots; i++) {
        index = (ngx_media_owner_dir_index(dir, hash) + i) % dir->slots;
        record = &dir->shm->records[index];

        if (record->state == NGX_MEDIA_OWNER_STATE_OWNED) {
            if (record->hash == hash) {
                return record;
            }

            if (first_avail == NULL
                && ngx_media_owner_dir_reclaimable(record))
            {
                first_avail = record;
            }

        } else if (record->state == NGX_MEDIA_OWNER_STATE_DELETED) {
            if (first_avail == NULL) {
                first_avail = record;
            }
        } else if (record->state == NGX_MEDIA_OWNER_STATE_FREE) {
            return (first_avail != NULL) ? first_avail : record;
        }

    }

    return first_avail;
}

ngx_media_owner_record_t *
ngx_media_owner_dir_claim(ngx_media_owner_dir_t *dir, uint64_t hash,
    ngx_uint_t slot)
{
    ngx_media_owner_record_t  *record;

    if (dir == NULL || dir->shm == NULL) {
        return NULL;
    }

    /*
     * Only the deterministic owner of a stream may publish a record for it.
     *
     * The record used to be written by whoever took the request (the API
     * claim, and any worker that opened a local program), and every reader
     * then treated it as the owner - so a stream whose hash names worker 1
     * could be driven on worker 0 by a claim worker 0 never refreshed, and the
     * two answers disagreed on every worker that read them.  Refusing a
     * foreign slot here makes the record repeat ngx_media_route_owner by
     * construction: a claim either names the hash owner or writes nothing.
     */
    if (slot != ngx_media_owner_slot(hash, dir->workers)) {
        return NULL;
    }

    if (ngx_media_owner_dir_lock(dir) != NGX_OK) {
        return NULL;
    }

    if (dir->epoch != dir->shm->epoch) {
        ngx_media_owner_dir_unlock(dir);
        return NULL;
    }

    record = ngx_media_owner_dir_find(dir, hash);

    if (record == NULL) {
        ngx_log_error(NGX_LOG_WARN, dir->log, 0,
                      "media: owner directory is full (capacity=%ui); "
                      "cannot claim hash=%uL", dir->slots, hash);
        ngx_media_owner_dir_unlock(dir);
        return NULL;
    }

    if (record->hash != hash || record->slot != slot
        || record->pid != (int32_t) ngx_pid
        || ngx_media_owner_dir_reclaimable(record))
    {
        /* The current generation takes over immediately; a draining worker
         * cannot reclaim it because its attached epoch no longer matches. */
        ngx_memzero(record, sizeof(ngx_media_owner_record_t));

        record->hash = hash;
        record->state = NGX_MEDIA_OWNER_STATE_OWNED;
        record->slot = (uint32_t) slot;
        record->pid = (int32_t) ngx_pid;
        record->heartbeat = (uint64_t) ngx_media_owner_dir_now();
    }

    ngx_media_owner_dir_unlock(dir);

    return record;
}

void
ngx_media_owner_dir_release(ngx_media_owner_dir_t *dir, uint64_t hash,
    ngx_uint_t slot)
{
    ngx_media_owner_record_t  *record;

    if (dir == NULL || dir->shm == NULL) {
        return;
    }

    if (ngx_media_owner_dir_lock(dir) != NGX_OK) {
        return;
    }
    record = ngx_media_owner_dir_find(dir, hash);

    if (record != NULL && record->hash == hash && record->slot == slot
        && record->pid == (int32_t) ngx_pid
        && record->state == NGX_MEDIA_OWNER_STATE_OWNED)
    {
        ngx_memzero(record, sizeof(ngx_media_owner_record_t));
        record->state = NGX_MEDIA_OWNER_STATE_DELETED;
    }
    ngx_media_owner_dir_unlock(dir);
}

void
ngx_media_owner_dir_heartbeat(ngx_media_owner_dir_t *dir, uint64_t hash,
    ngx_uint_t slot, uint64_t generation, uint64_t frames, ngx_uint_t sources)
{
    ngx_media_owner_record_t  *record;

    if (dir == NULL || dir->shm == NULL) {
        return;
    }

    if (ngx_media_owner_dir_lock(dir) != NGX_OK) {
        return;
    }

    record = ngx_media_owner_dir_find(dir, hash);

    if (record != NULL && record->hash == hash && record->slot == slot
        && record->pid == (int32_t) ngx_pid
        && record->state == NGX_MEDIA_OWNER_STATE_OWNED)
    {
        record->generation = generation;
        record->frames = frames;
        record->sources = (uint32_t) sources;
        record->state = NGX_MEDIA_OWNER_STATE_OWNED;
        record->heartbeat = (uint64_t) ngx_media_owner_dir_now();
    }

    ngx_media_owner_dir_unlock(dir);
}

/*
 * The record a stream's owner has published, when it is still live.  A worker
 * that does not drive a program reads the owner's progress here instead of
 * reporting its own empty numbers as if they described the program: the
 * generation, the frames and the source count in this record are the owner's
 * observed state, refreshed by its heartbeat.
 *
 * A record whose owner stopped heartbeating reports as missing rather than as
 * current, so a replica says "no owner is reporting" instead of serving a
 * stale number as fact.
 */
ngx_int_t
ngx_media_owner_dir_observe(ngx_media_owner_dir_t *dir, uint64_t hash,
    ngx_media_owner_record_t *out)
{
    ngx_media_owner_record_t  *record;

    if (dir == NULL || dir->shm == NULL || out == NULL) {
        return NGX_ERROR;
    }

    if (ngx_media_owner_dir_lock(dir) != NGX_OK) {
        return NGX_ERROR;
    }

    record = ngx_media_owner_dir_find(dir, hash);

    if (record == NULL || record->hash != hash
        || record->state != NGX_MEDIA_OWNER_STATE_OWNED
        || ngx_media_owner_dir_reclaimable(record))
    {
        ngx_media_owner_dir_unlock(dir);
        return NGX_ERROR;
    }

    *out = *record;

    ngx_media_owner_dir_unlock(dir);

    return NGX_OK;
}

ngx_uint_t
ngx_media_owner_dir_list(ngx_media_owner_dir_t *dir,
    ngx_media_owner_record_t *out, ngx_uint_t max)
{
    ngx_uint_t  i, count = 0;

    if (dir == NULL || dir->shm == NULL || out == NULL) {
        return 0;
    }

    if (ngx_media_owner_dir_lock(dir) != NGX_OK) {
        return 0;
    }

    for (i = 0; i < dir->slots && count < max; i++) {

        if (dir->shm->records[i].state != NGX_MEDIA_OWNER_STATE_OWNED) {
            continue;
        }

        out[count++] = dir->shm->records[i];
    }

    ngx_media_owner_dir_unlock(dir);

    return count;
}

#endif /* !NGX_MEDIA_UNIT_TEST */

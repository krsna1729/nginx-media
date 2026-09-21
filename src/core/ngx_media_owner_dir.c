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
    ngx_atomic_t                lock;
    ngx_media_owner_record_t    records[1];
} ngx_media_owner_dir_shm_t;

struct ngx_media_owner_dir_s {
    ngx_media_owner_dir_shm_t  *shm;
    ngx_uint_t                  slots;
    ngx_log_t                  *log;
};

/* an owner that stops heartbeating for this long may be taken over */
#define NGX_MEDIA_OWNER_STALE_MS 10000

static ngx_media_owner_dir_shm_t  *ngx_media_owner_dir_mapped;

static void
ngx_media_owner_dir_lock(ngx_media_owner_dir_t *dir)
{
    ngx_uint_t  i;

    for (i = 0; i < 100000; i++) {

        if (ngx_atomic_cmp_set(&dir->shm->lock, 0, 1)) {
            return;
        }

        ngx_cpu_pause();
    }

    /* a contended lock must never stop a worker: take it and carry on */
    (void) ngx_atomic_cmp_set(&dir->shm->lock, 0, 1);
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

static ngx_uint_t
ngx_media_owner_dir_index(ngx_media_owner_dir_t *dir, uint32_t hash)
{
    return (ngx_uint_t) (hash % dir->slots);
}

ngx_int_t
ngx_media_owner_dir_shm_create(ngx_cycle_t *cycle, ngx_uint_t slots,
    ngx_log_t *log)
{
    ngx_media_owner_dir_shm_t  *shm;
    size_t                      size;
    ngx_uint_t                  i;

    (void) cycle;

    if (ngx_media_owner_dir_mapped != NULL) {
        return NGX_OK;
    }

    if (slots == 0) {
        slots = 256;
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

    for (i = 0; i < slots; i++) {
        shm->records[i].state = NGX_MEDIA_OWNER_STATE_FREE;
    }

    ngx_media_owner_dir_mapped = shm;

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
    dir->log = log;

    return dir;
}

static ngx_uint_t
ngx_media_owner_dir_reclaimable(ngx_media_owner_record_t *record)
{
    if (record->state != NGX_MEDIA_OWNER_STATE_OWNED) {
        return 1;
    }

    if (record->pid == (int32_t) ngx_pid) {
        return 1;   /* our own record from a previous cycle */
    }

    if (ngx_media_owner_dir_now() - (ngx_msec_t) record->heartbeat
        > NGX_MEDIA_OWNER_STALE_MS)
    {
        return 1;   /* the owner stopped heartbeating */
    }

    return 0;
}

static ngx_media_owner_record_t *
ngx_media_owner_dir_find(ngx_media_owner_dir_t *dir, uint32_t hash)
{
    ngx_uint_t                 i, index;
    ngx_media_owner_record_t  *record;

    for (i = 0; i < dir->slots; i++) {
        index = (ngx_media_owner_dir_index(dir, hash) + i) % dir->slots;
        record = &dir->shm->records[index];

        if (record->state == NGX_MEDIA_OWNER_STATE_FREE
            || record->hash == hash)
        {
            return record;
        }
    }

    return NULL;
}

ngx_media_owner_record_t *
ngx_media_owner_dir_get(ngx_media_owner_dir_t *dir, uint32_t hash,
    ngx_uint_t create)
{
    ngx_media_owner_record_t  *record;

    if (dir == NULL || dir->shm == NULL) {
        return NULL;
    }

    ngx_media_owner_dir_lock(dir);

    record = ngx_media_owner_dir_find(dir, hash);

    if (record != NULL && record->hash != hash) {

        if (!create) {
            record = NULL;

        } else {
            ngx_memzero(record, sizeof(ngx_media_owner_record_t));
            record->hash = hash;
            record->state = NGX_MEDIA_OWNER_STATE_FREE;
        }
    }

    ngx_media_owner_dir_unlock(dir);

    return record;
}

ngx_media_owner_record_t *
ngx_media_owner_dir_claim(ngx_media_owner_dir_t *dir, uint32_t hash,
    ngx_uint_t slot)
{
    ngx_media_owner_record_t  *record;

    if (dir == NULL || dir->shm == NULL) {
        return NULL;
    }

    ngx_media_owner_dir_lock(dir);

    record = ngx_media_owner_dir_find(dir, hash);

    if (record != NULL
        && (record->hash != hash || ngx_media_owner_dir_reclaimable(record)))
    {
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
ngx_media_owner_dir_release(ngx_media_owner_dir_t *dir, uint32_t hash,
    ngx_uint_t slot)
{
    ngx_media_owner_record_t  *record;

    if (dir == NULL || dir->shm == NULL) {
        return;
    }

    ngx_media_owner_dir_lock(dir);

    record = ngx_media_owner_dir_find(dir, hash);

    if (record != NULL && record->hash == hash && record->slot == slot
        && record->pid == (int32_t) ngx_pid)
    {
        ngx_memzero(record, sizeof(ngx_media_owner_record_t));
        record->state = NGX_MEDIA_OWNER_STATE_FREE;
    }

    ngx_media_owner_dir_unlock(dir);
}

void
ngx_media_owner_dir_heartbeat(ngx_media_owner_dir_t *dir, uint32_t hash,
    ngx_uint_t slot, uint64_t generation, uint64_t frames, ngx_uint_t sources)
{
    ngx_media_owner_record_t  *record;

    if (dir == NULL || dir->shm == NULL) {
        return;
    }

    ngx_media_owner_dir_lock(dir);

    record = ngx_media_owner_dir_find(dir, hash);

    if (record != NULL && record->hash == hash && record->slot == slot
        && record->pid == (int32_t) ngx_pid)
    {
        record->generation = generation;
        record->frames = frames;
        record->sources = (uint32_t) sources;
        record->state = NGX_MEDIA_OWNER_STATE_OWNED;
        record->heartbeat = (uint64_t) ngx_media_owner_dir_now();
    }

    ngx_media_owner_dir_unlock(dir);
}

ngx_uint_t
ngx_media_owner_dir_slot(ngx_media_owner_dir_t *dir, uint32_t hash,
    ngx_uint_t fallback)
{
    ngx_media_owner_record_t  *record;
    ngx_uint_t                 slot;

    if (dir == NULL || dir->shm == NULL) {
        return fallback;
    }

    ngx_media_owner_dir_lock(dir);

    record = ngx_media_owner_dir_find(dir, hash);

    if (record == NULL || record->hash != hash
        || ngx_media_owner_dir_reclaimable(record))
    {
        slot = fallback;

    } else {
        slot = (ngx_uint_t) record->slot;
    }

    ngx_media_owner_dir_unlock(dir);

    return slot;
}

ngx_uint_t
ngx_media_owner_dir_list(ngx_media_owner_dir_t *dir,
    ngx_media_owner_record_t *out, ngx_uint_t max)
{
    ngx_uint_t  i, count = 0;

    if (dir == NULL || dir->shm == NULL || out == NULL) {
        return 0;
    }

    ngx_media_owner_dir_lock(dir);

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

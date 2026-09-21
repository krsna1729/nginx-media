#include "ngx_media_owner.h"

#ifndef NGX_MEDIA_UNIT_TEST

#include <ngx_config.h>
#include <ngx_core.h>

#endif

/*
 * Deterministic ownership: FNV-1a over "application/stream".  The hash must be
 * identical in every worker and across restarts, so it is deliberately simple
 * and dependency free.
 */
uint32_t
ngx_media_owner_hash(const ngx_str_t *application, const ngx_str_t *stream)
{
    uint32_t  hash = 2166136261u;
    size_t    i;

    if (application != NULL) {

        for (i = 0; i < application->len; i++) {
            hash ^= (uint32_t) application->data[i];
            hash *= 16777619u;
        }
    }

    if (stream != NULL) {

        /* the separator keeps "ab" + "c" distinct from "a" + "bc" */
        hash ^= (uint32_t) '/';
        hash *= 16777619u;

        for (i = 0; i < stream->len; i++) {
            hash ^= (uint32_t) stream->data[i];
            hash *= 16777619u;
        }
    }

    return hash;
}

ngx_uint_t
ngx_media_owner_slot(uint32_t hash, ngx_uint_t workers)
{
    if (workers == 0) {
        return 0;
    }

    return (ngx_uint_t) (hash % workers);
}

#ifndef NGX_MEDIA_UNIT_TEST

ngx_uint_t
ngx_media_owner_worker_count(ngx_cycle_t *cycle)
{
    ngx_core_conf_t  *ccf;

    if (cycle == NULL) {
        return 1;
    }

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    if (ccf == NULL || ccf->worker_processes == 0) {
        return 1;
    }

    return ccf->worker_processes;
}

ngx_uint_t
ngx_media_owner_for(ngx_cycle_t *cycle, uint32_t hash)
{
    return ngx_media_owner_slot(hash, ngx_media_owner_worker_count(cycle));
}

#endif /* !NGX_MEDIA_UNIT_TEST */

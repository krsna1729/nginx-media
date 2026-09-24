#include "ngx_media_policy.h"

static ngx_media_policy_t  *ngx_media_policy_worker;

#ifndef NGX_MEDIA_UNIT_TEST
extern ngx_module_t  ngx_media_core_module;
#endif

ngx_media_policy_t *
ngx_media_policy_init(ngx_media_policy_t *policy)
{
    if (policy == NULL) {
        return NULL;
    }

    ngx_memzero(policy, sizeof(ngx_media_policy_t));

    policy->failure_timeout = NGX_MEDIA_POLICY_FAILURE_TIMEOUT_DEFAULT;
    policy->recovery_timeout = NGX_MEDIA_POLICY_RECOVERY_TIMEOUT_DEFAULT;
    policy->switchback = NGX_MEDIA_SWITCHBACK_AUTO;
    policy->srt_egress_workers = NGX_MEDIA_EGRESS_WORKERS_UNSET;
    policy->hls_push_egress_workers = NGX_MEDIA_EGRESS_WORKERS_UNSET;
    ngx_media_executor_conf_default(&policy->transform_executor);

    /*
     * ngx_conf_set_flag_slot refuses a slot that is not UNSET, so the flag
     * starts unset and the default is applied once, after configuration has
     * been parsed.  Seeding it with 1 here made the directive unusable: the
     * first (and only legal) use reported "directive is duplicate".
     */
    policy->switch_keyframe = NGX_CONF_UNSET;

    return policy;
}

ngx_media_policy_t *
ngx_media_policy_get(ngx_cycle_t *cycle)
{
    if (cycle == NULL) {
        return NULL;
    }

#ifndef NGX_MEDIA_UNIT_TEST
    if (cycle->conf_ctx != NULL
        && cycle->conf_ctx[ngx_media_core_module.index] != NULL)
    {
        return (ngx_media_policy_t *)
                   cycle->conf_ctx[ngx_media_core_module.index];
    }
#endif

    if (ngx_media_policy_worker != NULL) {
        return ngx_media_policy_worker;
    }

    ngx_media_policy_worker = ngx_pcalloc(cycle->pool,
                                          sizeof(ngx_media_policy_t));
    if (ngx_media_policy_worker == NULL) {
        return NULL;
    }

    ngx_media_policy_init(ngx_media_policy_worker);

    /*
     * Configuration has been parsed by the time anything asks for the
     * policy, so an unset flag means the operator never mentioned it.
     */
    if (ngx_media_policy_worker->switch_keyframe == NGX_CONF_UNSET) {
        ngx_media_policy_worker->switch_keyframe = 1;
    }

    return ngx_media_policy_worker;
}

#include "ngx_media_policy.h"

static ngx_media_policy_t  *ngx_media_policy_worker;

ngx_media_policy_t *
ngx_media_policy_init(ngx_media_policy_t *policy)
{
    if (policy == NULL) {
        return NULL;
    }

    ngx_memzero(policy, sizeof(ngx_media_policy_t));

    policy->failure_timeout = NGX_MEDIA_POLICY_FAILURE_TIMEOUT_DEFAULT;
    policy->recovery_timeout = NGX_MEDIA_POLICY_RECOVERY_TIMEOUT_DEFAULT;
    policy->switch_keyframe = 1;
    policy->switchback = NGX_MEDIA_SWITCHBACK_AUTO;

    return policy;
}

ngx_media_policy_t *
ngx_media_policy_get(ngx_cycle_t *cycle)
{
    if (cycle == NULL) {
        return NULL;
    }

    if (ngx_media_policy_worker != NULL) {
        return ngx_media_policy_worker;
    }

    ngx_media_policy_worker = ngx_pcalloc(cycle->pool,
                                          sizeof(ngx_media_policy_t));
    if (ngx_media_policy_worker == NULL) {
        return NULL;
    }

    ngx_media_policy_init(ngx_media_policy_worker);

    return ngx_media_policy_worker;
}

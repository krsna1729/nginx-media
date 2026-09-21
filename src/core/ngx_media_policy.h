#ifndef NGX_MEDIA_POLICY_H
#define NGX_MEDIA_POLICY_H

#include "ngx_media_platform.h"
#include "ngx_media.h"

/*
 * Selection policy (goal doc 8).
 *
 * These are platform defaults: they are set from configuration once, in the
 * master, and inherited by every worker through the cycle pool.
 *
 *   failure_timeout   a connected-but-silent or frozen source is failed after
 *                     this long without media progress
 *   recovery_timeout  a failed source must show good evidence for this long
 *                     before it becomes eligible again (hysteresis)
 *   switch_keyframe   require a decodable boundary for switches
 *   switchback        auto | manual | never
 */

typedef struct {
    ngx_msec_t   failure_timeout;
    ngx_msec_t   recovery_timeout;
    ngx_uint_t   switch_keyframe;
    ngx_uint_t   switchback;

    /* static outputs applied to every stream (per-stream configuration
     * arrives with the stream database) */
    ngx_str_t    hls_path;
    ngx_str_t    record_program_path;
    ngx_str_t    record_raw_path;
    ngx_str_t    record_iso_path;
    ngx_str_t    record_iso_source;   /* publisher identity for the ISO tap */
} ngx_media_policy_t;

#define NGX_MEDIA_POLICY_FAILURE_TIMEOUT_DEFAULT 1500
#define NGX_MEDIA_POLICY_RECOVERY_TIMEOUT_DEFAULT 10000

/* per-cycle policy; created with defaults on first use */
ngx_media_policy_t *ngx_media_policy_get(ngx_cycle_t *cycle);
ngx_media_policy_t *ngx_media_policy_init(ngx_media_policy_t *policy);

#endif /* NGX_MEDIA_POLICY_H */

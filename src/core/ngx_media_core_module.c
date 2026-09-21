/*
 * nginx-media core module registration and platform policy.
 *
 * The module owns the selection policy that every worker inherits:
 *
 *   media_failover_failure_timeout 1s;
 *   media_failover_recovery_timeout 10s;
 *   media_failover_switch_keyframe on;
 *   media_failover_switchback auto|manual|never;
 *
 * The protocol modules, the selector and the control API are added in later
 * phases; this file is the single place where the module is registered with
 * the NGINX module system.
 */

#include "ngx_media_platform.h"
#include "ngx_media.h"
#include "ngx_media_owner_dir.h"
#include "ngx_media_route.h"
#include "ngx_media_policy.h"

static void *ngx_media_core_create_conf(ngx_cycle_t *cycle);
static ngx_int_t ngx_media_core_init_module(ngx_cycle_t *cycle);
static char *ngx_media_failure_timeout_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_recovery_timeout_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_switchback_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_hls_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_record_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_record_iso_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_media_parse_msec(const ngx_str_t *value,
    ngx_msec_t *out);

/*
 * Failover timeouts are sub-second quantities, so unlike nginx's own time
 * parser a bare number means milliseconds; the usual unit suffixes are also
 * accepted ("700" and "700ms" are the same, "2s" is 2000).
 */
static ngx_int_t
ngx_media_parse_msec(const ngx_str_t *value, ngx_msec_t *out)
{
    ngx_int_t  n;
    time_t     t;

    if (value->len == 0) {
        return NGX_ERROR;
    }

    if (value->len > 2 && value->data[value->len - 2] == 'm'
        && value->data[value->len - 1] == 's')
    {
        n = ngx_atoi(value->data, value->len - 2);

        if (n == NGX_ERROR || n < 0) {
            return NGX_ERROR;
        }

        *out = (ngx_msec_t) n;

        return NGX_OK;
    }

    if (value->data[value->len - 1] < '0' || value->data[value->len - 1] > '9')
    {
        t = ngx_parse_time((ngx_str_t *) value, 0);

        if (t == NGX_ERROR || t < 0) {
            return NGX_ERROR;
        }

        *out = (ngx_msec_t) t;

        return NGX_OK;
    }

    n = ngx_atoi(value->data, value->len);

    if (n == NGX_ERROR || n < 0) {
        return NGX_ERROR;
    }

    *out = (ngx_msec_t) n;

    return NGX_OK;
}

static ngx_command_t ngx_media_core_commands[] = {

    { ngx_string("media_failover_failure_timeout"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_failure_timeout_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_failover_recovery_timeout"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_recovery_timeout_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_failover_switch_keyframe"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_media_policy_t, switch_keyframe),
      NULL },

    { ngx_string("media_failover_switchback"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_switchback_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_hls"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_hls_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_record"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_record_cmd,
      offsetof(ngx_media_policy_t, record_program_path),
      0,
      NULL },

    { ngx_string("media_record_raw"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_record_cmd,
      offsetof(ngx_media_policy_t, record_raw_path),
      0,
      NULL },

    { ngx_string("media_record_iso"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE2,
      ngx_media_record_iso_cmd,
      0,
      0,
      NULL },

      ngx_null_command
};

static ngx_core_module_t ngx_media_core_module_ctx = {
    ngx_string("media"),
    ngx_media_core_create_conf,
    NULL
};

ngx_module_t ngx_media_core_module = {
    NGX_MODULE_V1,
    &ngx_media_core_module_ctx,  /* module context */
    ngx_media_core_commands,     /* module directives */
    NGX_CORE_MODULE,             /* module type */
    NULL,                        /* init master */
    ngx_media_core_init_module,  /* init module: master, before forking */
    NULL,                        /* init process */
    NULL,                        /* init thread */
    NULL,                        /* exit thread */
    NULL,                        /* exit process */
    NULL,                        /* exit master */
    NGX_MODULE_V1_PADDING
};

static void *
ngx_media_core_create_conf(ngx_cycle_t *cycle)
{
    ngx_media_policy_t  *policy;

    policy = ngx_pcalloc(cycle->pool, sizeof(ngx_media_policy_t));
    if (policy == NULL) {
        return NULL;
    }

    return ngx_media_policy_init(policy);
}

static char *
ngx_media_failure_timeout_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_policy_t  *policy = conf;
    ngx_str_t           *value;
    ngx_msec_t           duration;

    (void) cmd;

    value = cf->args->elts;

    if (ngx_media_parse_msec(&value[1], &duration) != NGX_OK) {
        return "invalid duration";
    }

    policy->failure_timeout = duration;

    return NGX_CONF_OK;
}

static char *
ngx_media_recovery_timeout_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_policy_t  *policy = conf;
    ngx_str_t           *value;
    ngx_msec_t           duration;

    (void) cmd;

    value = cf->args->elts;

    if (ngx_media_parse_msec(&value[1], &duration) != NGX_OK) {
        return "invalid duration";
    }

    policy->recovery_timeout = duration;

    return NGX_CONF_OK;
}

static char *
ngx_media_switchback_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_policy_t  *policy = conf;
    ngx_str_t           *value;

    (void) cmd;

    value = cf->args->elts;

    if (value[1].len == sizeof("auto") - 1
        && ngx_memcmp(value[1].data, "auto", sizeof("auto") - 1) == 0)
    {
        policy->switchback = NGX_MEDIA_SWITCHBACK_AUTO;

    } else if (value[1].len == sizeof("manual") - 1
               && ngx_memcmp(value[1].data, "manual", sizeof("manual") - 1)
                  == 0)
    {
        policy->switchback = NGX_MEDIA_SWITCHBACK_MANUAL;

    } else if (value[1].len == sizeof("never") - 1
               && ngx_memcmp(value[1].data, "never", sizeof("never") - 1) == 0)
    {
        policy->switchback = NGX_MEDIA_SWITCHBACK_NEVER;

    } else {
        return "must be auto, manual or never";
    }

    return NGX_CONF_OK;
}

/*
 * Output paths point into the configuration pool, which outlives every worker:
 * the policy struct is created in that pool and inherited across the fork.
 */
static char *
ngx_media_hls_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_policy_t  *policy = conf;
    ngx_str_t           *value = cf->args->elts;

    (void) cmd;

    if (value[1].len == 0) {
        return "must not be empty";
    }

    policy->hls_path = value[1];

    return NGX_CONF_OK;
}

static char *
ngx_media_record_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t  *value = cf->args->elts;
    ngx_str_t  *slot = (ngx_str_t *) ((char *) conf + cmd->conf);

    if (value[1].len == 0) {
        return "must not be empty";
    }

    *slot = value[1];

    return NGX_CONF_OK;
}

static char *
ngx_media_record_iso_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_policy_t  *policy = conf;
    ngx_str_t           *value = cf->args->elts;

    (void) cmd;

    if (value[1].len == 0 || value[2].len == 0) {
        return "must not be empty";
    }

    policy->record_iso_source = value[1];
    policy->record_iso_path = value[2];

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_media_core_init_module(ngx_cycle_t *cycle)
{
    /*
     * The shared owner directory is allocated in the master before workers are
     * forked, so every worker inherits the same pages (goal doc 22).  It holds
     * small bookkeeping only: owner slot and pid, generation, heartbeat.
     */
    if (ngx_media_owner_dir_shm_create(cycle, 256, cycle->log) != NGX_OK) {
        return NGX_ERROR;
    }

    /* worker-to-worker routing pairs are inherited across the fork */
    if (ngx_media_route_master_init(cycle, cycle->log) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

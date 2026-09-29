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
#include "ngx_media_egress_manager.h"
#include "ngx_media_key.h"

#include <openssl/rand.h>

static void *ngx_media_core_create_conf(ngx_cycle_t *cycle);
static ngx_int_t ngx_media_core_init_module(ngx_cycle_t *cycle);

/*
 * The deployment secret every ingest key is derived under.  It lives in the
 * process, not in the graph: the graph is replicated to every worker and
 * written to the shared directory, and a secret there would be a secret in
 * every dump of it.  Loaded once before the workers fork, so they inherit it
 * and no worker reads the file itself.
 */
static u_char   ngx_media_key_master[NGX_MEDIA_KEY_MASTER_LEN];
static size_t   ngx_media_key_master_len;

const u_char *
ngx_media_ingest_secret(size_t *len)
{
    if (len != NULL) {
        *len = ngx_media_key_master_len;
    }

    return ngx_media_key_master_len == 0 ? NULL : ngx_media_key_master;
}

ngx_int_t
ngx_media_ingest_secret_load(const ngx_str_t *path, ngx_log_t *log)
{
    u_char    buf[NGX_MEDIA_KEY_MASTER_LEN * 4];
    ngx_fd_t  fd;
    ssize_t   n;
    size_t    i;

    if (path == NULL || path->len == 0) {
        return NGX_ERROR;
    }

    fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);

    if (fd == NGX_INVALID_FILE) {

        /*
         * First start: make one.  0600, and it is the one file in a
         * deployment that must not be readable by anyone else - which is a
         * simpler thing to protect than a key per source in state.
         */
        if (RAND_bytes(ngx_media_key_master, NGX_MEDIA_KEY_MASTER_LEN) != 1) {
            return NGX_ERROR;
        }

        fd = ngx_open_file(path->data,
                           NGX_FILE_WRONLY | NGX_FILE_CREATE_OR_OPEN
                               | NGX_FILE_TRUNCATE,
                           NGX_FILE_DEFAULT_ACCESS, 0600);

        if (fd == NGX_INVALID_FILE) {
            ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                          "media: could not create the ingest secret at %V",
                          path);
            return NGX_ERROR;
        }

        n = ngx_write_fd(fd, ngx_media_key_master, NGX_MEDIA_KEY_MASTER_LEN);
        ngx_close_file(fd);

        if (n != NGX_MEDIA_KEY_MASTER_LEN) {
            return NGX_ERROR;
        }

        ngx_media_key_master_len = NGX_MEDIA_KEY_MASTER_LEN;

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: generated the ingest secret at %V", path);
        return NGX_OK;
    }

    n = ngx_read_fd(fd, buf, sizeof(buf));
    ngx_close_file(fd);

    if (n <= 0) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: the ingest secret at %V is empty", path);
        return NGX_ERROR;
    }

    if ((size_t) n > sizeof(buf)) {
        n = (ssize_t) sizeof(buf);
    }

    /*
     * Whatever the file holds is the secret, stretched to the length the
     * derivation wants: an operator can supply one from a secret manager in
     * whatever form that produces.
     */
    for (i = 0; i < NGX_MEDIA_KEY_MASTER_LEN; i++) {
        ngx_media_key_master[i] = buf[i % (size_t) n];
    }

    ngx_media_key_master_len = NGX_MEDIA_KEY_MASTER_LEN;

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: loaded the ingest secret from %V", path);

    return NGX_OK;
}
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
static char *ngx_media_transform_ffmpeg_cmd(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_media_transform_profile_cmd(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_media_parse_msec(const ngx_str_t *value,
    ngx_msec_t *out);
static char *ngx_media_egress_workers_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

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

    { ngx_string("media_ingest_secret"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      0,
      offsetof(ngx_media_policy_t, ingest_secret_path),
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

    { ngx_string("media_transform_ffmpeg"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_transform_ffmpeg_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_transform_profile"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE4,
      ngx_media_transform_profile_cmd,
      0,
      0,
      NULL },


    { ngx_string("media_egress_workers"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE2,
      ngx_media_egress_workers_cmd,
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

static char *
ngx_media_transform_ffmpeg_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_media_policy_t  *policy = conf;
    ngx_str_t           *value = cf->args->elts;

    (void) cmd;

    if (value[1].len == 0
        || value[1].len >= NGX_MEDIA_EXECUTOR_MAX_EXECUTABLE)
    {
        return "must be a non-empty executable path shorter than 4096 bytes";
    }

    policy->transform_executor.executable = value[1];

    return NGX_CONF_OK;
}

static char *
ngx_media_transform_profile_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_media_policy_t  *policy = conf;
    ngx_str_t           *value = cf->args->elts;
    ngx_int_t            n[4];
    ngx_uint_t           i;

    (void) cmd;

    for (i = 0; i < 4; i++) {
        n[i] = ngx_atoi(value[i + 1].data, value[i + 1].len);

        if (n[i] <= 0) {
            return "profile values must be positive integers";
        }
    }

    if (n[0] > 8192 || n[1] > 8192
        || n[2] > 100000000 || n[3] > 10000000)
    {
        return "profile exceeds transform bounds";
    }

    policy->transform_executor.width = (ngx_uint_t) n[0];
    policy->transform_executor.height = (ngx_uint_t) n[1];
    policy->transform_executor.video_bitrate = (ngx_uint_t) n[2];
    policy->transform_executor.audio_bitrate = (ngx_uint_t) n[3];

    return NGX_CONF_OK;
}

static char *
ngx_media_egress_workers_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_policy_t  *policy = conf;
    ngx_str_t           *value = cf->args->elts;
    ngx_uint_t          *slot, maximum;
    ngx_int_t            workers;

    (void) cmd;

    if (value[1].len == sizeof("srt") - 1
        && ngx_memcmp(value[1].data, "srt", sizeof("srt") - 1) == 0)
    {
        slot = &policy->srt_egress_workers;
        maximum = 16;

    } else if (value[1].len == sizeof("hls_push") - 1
               && ngx_memcmp(value[1].data, "hls_push",
                             sizeof("hls_push") - 1) == 0)
    {
        slot = &policy->hls_push_egress_workers;
        maximum = 4;

    } else {
        return "engine must be srt or hls_push";
    }

    if (*slot != NGX_MEDIA_EGRESS_WORKERS_UNSET) {
        return "is duplicate";
    }

    if (value[2].len == sizeof("adaptive") - 1
        && ngx_memcmp(value[2].data, "adaptive",
                      sizeof("adaptive") - 1) == 0)
    {
        *slot = 0;
        return NGX_CONF_OK;
    }

    workers = ngx_atoi(value[2].data, value[2].len);
    if (workers < 1 || workers > (ngx_int_t) maximum) {
        return "worker count must be within the engine's fixed pool";
    }

    *slot = (ngx_uint_t) workers;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_media_core_init_module(ngx_cycle_t *cycle)
{
    {
        /*
         * Before the workers fork: the secret is loaded once and inherited,
         * so every worker derives the same key for the same source and no
         * worker reads the file itself.
         */
        ngx_media_policy_t  *policy;

        policy = ngx_media_policy_get(cycle);

        if (policy != NULL && policy->ingest_secret_path.len != 0
            && ngx_media_ingest_secret_load(&policy->ingest_secret_path,
                                            cycle->log)
                != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    ngx_media_policy_t  *policy;

    policy = ngx_media_policy_get(cycle);
    if (policy == NULL) {
        return NGX_ERROR;
    }

    ngx_media_egress_manager_fixed_workers_set(
        NGX_MEDIA_EGRESS_ENGINE_SRT_SHARD,
        policy->srt_egress_workers == NGX_MEDIA_EGRESS_WORKERS_UNSET
            ? 0 : policy->srt_egress_workers);
    ngx_media_egress_manager_fixed_workers_set(
        NGX_MEDIA_EGRESS_ENGINE_HLS_UPLOAD_POOL,
        policy->hls_push_egress_workers == NGX_MEDIA_EGRESS_WORKERS_UNSET
            ? 0 : policy->hls_push_egress_workers);

    /*
     * The shared owner directory is allocated in the master before workers are
     * forked, so every worker inherits the same pages (goal doc 22).  It holds
     * small bookkeeping only: owner slot and pid, generation, heartbeat.
     */
    if (ngx_media_owner_dir_shm_create(cycle, NGX_MEDIA_OWNER_DIR_SLOTS,
                                       cycle->log) != NGX_OK) {
        return NGX_ERROR;
    }

    /* worker-to-worker routing pairs are inherited across the fork */
    if (ngx_media_route_master_init(cycle, cycle->log) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

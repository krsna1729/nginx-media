/*
 * nginx-media core module registration.
 *
 * Phase 0 exposes the core object model, payload buffer ownership and the
 * bounded program feed.  Protocol modules, the selector and the control API
 * are added in later phases; this file is the single place where the module
 * is registered with the NGINX module system.
 */

#include "ngx_media_platform.h"
#include "ngx_media.h"

static ngx_core_module_t ngx_media_core_module_ctx = {
    ngx_string("media"),
    NULL,
    NULL
};

ngx_module_t ngx_media_core_module = {
    NGX_MODULE_V1,
    &ngx_media_core_module_ctx,  /* module context */
    NULL,                        /* module directives */
    NGX_CORE_MODULE,             /* module type */
    NULL,                        /* init master */
    NULL,                        /* init module */
    NULL,                        /* init process */
    NULL,                        /* init thread */
    NULL,                        /* exit thread */
    NULL,                        /* exit process */
    NULL,                        /* exit master */
    NGX_MODULE_V1_PADDING
};

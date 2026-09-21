#ifndef NGX_MEDIA_PLATFORM_H
#define NGX_MEDIA_PLATFORM_H

/*
 * Single include seam between the portable media core and its host platform.
 *
 * Production builds compile against the NGINX headers.  Unit-test builds
 * (-DNGX_MEDIA_UNIT_TEST) compile against tests/unit/shim/ngx_shim.h, which
 * provides the small subset of NGINX types and primitives the core uses.
 */

#ifdef NGX_MEDIA_UNIT_TEST

#include "ngx_shim.h"

#else

#include <ngx_config.h>
#include <ngx_core.h>

#endif

#include <stdint.h>

#endif /* NGX_MEDIA_PLATFORM_H */

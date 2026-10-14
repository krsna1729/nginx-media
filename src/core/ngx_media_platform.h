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
#include <stdbool.h>

#if defined(__has_include)
#  if __has_include(<stdckdint.h>)
#    include <stdckdint.h>
#  endif
#endif

#ifndef ckd_add
#  define ckd_add(r, a, b) __builtin_add_overflow((a), (b), (r))
#endif
#ifndef ckd_sub
#  define ckd_sub(r, a, b) __builtin_sub_overflow((a), (b), (r))
#endif
#ifndef ckd_mul
#  define ckd_mul(r, a, b) __builtin_mul_overflow((a), (b), (r))
#endif

#endif /* NGX_MEDIA_PLATFORM_H */

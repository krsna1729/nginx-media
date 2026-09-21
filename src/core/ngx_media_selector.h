#ifndef NGX_MEDIA_SELECTOR_H
#define NGX_MEDIA_SELECTOR_H

#include "ngx_media.h"
#include "ngx_media_compat.h"
#include "ngx_media_health.h"
#include "ngx_media_policy.h"

/*
 * Source selection (goal doc 8, 9).
 *
 *   eligible = health policy passes
 *   winner   = highest configured priority among eligible sources
 *
 * The selector never blends evidence into a score.  It decides when to fail
 * over (the active source lost eligibility) and when to switch back (a
 * higher-priority source is eligible again, per the switchback policy), and
 * it always completes a change through the same safe promotion path used by
 * manual switches.
 *
 * When the only eligible source left is incompatible with the program, the
 * switch is allowed as an emergency and is surfaced: the generation changes
 * (so every consumer sees the discontinuity) and the stream's counter is
 * incremented for operators.
 */

typedef struct {
    ngx_media_source_t  *best;            /* best eligible compatible standby */
    ngx_media_source_t  *emergency;       /* best eligible incompatible standby */
    ngx_uint_t           eligible;        /* eligible sources */
    ngx_uint_t           active_eligible;
} ngx_media_selector_result_t;

/* refreshes health and per-source compatibility; reports the candidates */
void ngx_media_selector_evaluate(ngx_media_stream_t *stream, ngx_msec_t now,
    ngx_media_selector_result_t *out);

/* evaluate + act: failover, switchback per policy */
ngx_int_t ngx_media_selector_run(ngx_media_stream_t *stream, ngx_msec_t now,
    ngx_media_selector_result_t *out);

/* explicit switchback request (control API) */
ngx_int_t ngx_media_selector_switchback(ngx_media_stream_t *stream,
    ngx_msec_t now);

#endif /* NGX_MEDIA_SELECTOR_H */

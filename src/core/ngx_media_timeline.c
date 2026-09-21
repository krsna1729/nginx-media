#include "ngx_media_timeline.h"

void
ngx_media_timeline_init(ngx_media_timeline_t *timeline)
{
    if (timeline == NULL) {
        return;
    }

    ngx_memzero(timeline, sizeof(ngx_media_timeline_t));
}

void
ngx_media_timeline_discontinuity(ngx_media_timeline_t *timeline)
{
    if (timeline == NULL) {
        return;
    }

    timeline->switches++;
    timeline->anchored = 0;
}

ngx_int_t
ngx_media_timeline_map(ngx_media_timeline_t *timeline, int64_t source_pts,
    int64_t source_dts, int64_t *program_pts, int64_t *program_dts)
{
    int64_t  dts, pts;

    if (timeline == NULL || program_pts == NULL || program_dts == NULL) {
        return NGX_ERROR;
    }

    if (!timeline->anchored) {
        if (timeline->switches == 0) {
            /* the first frame of the program defines t = 0 */
            timeline->offset = -source_dts;

        } else {
            timeline->offset = timeline->last_program_dts + 1 - source_dts;
        }

        timeline->anchored = 1;
    }

    dts = source_dts + timeline->offset;
    pts = source_pts + timeline->offset;

    if (timeline->has_program && dts <= timeline->last_program_dts) {
        /* source clock discontinuity or drift: re-anchor, never regress */
        timeline->offset = timeline->last_program_dts + 1 - source_dts;
        timeline->resyncs++;

        dts = source_dts + timeline->offset;
        pts = source_pts + timeline->offset;
    }

    timeline->last_program_dts = dts;
    timeline->last_program_pts = pts;
    timeline->has_program = 1;

    *program_pts = pts;
    *program_dts = dts;

    return NGX_OK;
}

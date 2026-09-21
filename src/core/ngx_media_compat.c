#include "ngx_media_compat.h"

const char *
ngx_media_compat_name(ngx_uint_t compat)
{
    switch (compat) {

    case NGX_MEDIA_COMPAT_READY:
        return "ready";

    case NGX_MEDIA_COMPAT_DEGRADED:
        return "degraded";

    case NGX_MEDIA_COMPAT_INCOMPATIBLE:
        return "incompatible";

    default:
        return "unknown";
    }
}

static ngx_uint_t
ngx_media_compat_worst(ngx_uint_t a, ngx_uint_t b)
{
    return (a > b) ? a : b;
}

static ngx_uint_t
ngx_media_compat_track(const ngx_media_track_t *program,
    const ngx_media_track_t *candidate)
{
    if (program->codec != candidate->codec) {
        return NGX_MEDIA_COMPAT_INCOMPATIBLE;
    }

    if (program->media_type != candidate->media_type) {
        return NGX_MEDIA_COMPAT_INCOMPATIBLE;
    }

    if (program->media_type == NGX_MEDIA_TYPE_AUDIO) {

        if (program->sample_rate != 0 && candidate->sample_rate != 0
            && program->sample_rate != candidate->sample_rate)
        {
            return NGX_MEDIA_COMPAT_DEGRADED;
        }

        if (program->channels != 0 && candidate->channels != 0
            && program->channels != candidate->channels)
        {
            return NGX_MEDIA_COMPAT_DEGRADED;
        }

        if (program->profile != 0 && candidate->profile != 0
            && program->profile != candidate->profile)
        {
            return NGX_MEDIA_COMPAT_DEGRADED;
        }
    }

    if (program->media_type == NGX_MEDIA_TYPE_VIDEO) {

        if (program->profile != 0 && candidate->profile != 0
            && program->profile != candidate->profile)
        {
            return NGX_MEDIA_COMPAT_DEGRADED;
        }

        if (program->level != 0 && candidate->level != 0
            && program->level != candidate->level)
        {
            return NGX_MEDIA_COMPAT_DEGRADED;
        }

        if (program->width != 0 && candidate->width != 0
            && program->width != candidate->width)
        {
            return NGX_MEDIA_COMPAT_DEGRADED;
        }

        if (program->height != 0 && candidate->height != 0
            && program->height != candidate->height)
        {
            return NGX_MEDIA_COMPAT_DEGRADED;
        }
    }

    if (program->payload_format != candidate->payload_format) {
        return NGX_MEDIA_COMPAT_DEGRADED;
    }

    return NGX_MEDIA_COMPAT_READY;
}

ngx_uint_t
ngx_media_compat_classify(const ngx_media_trackset_t *program,
    const ngx_media_trackset_t *candidate)
{
    ngx_uint_t  result, i, j, found;

    if (program == NULL || candidate == NULL || program->count == 0) {
        return NGX_MEDIA_COMPAT_READY;
    }

    result = NGX_MEDIA_COMPAT_READY;

    for (i = 0; i < program->count; i++) {
        found = 0;

        for (j = 0; j < candidate->count; j++) {

            if (candidate->tracks[j].media_type
                != program->tracks[i].media_type)
            {
                continue;
            }

            found = 1;

            result = ngx_media_compat_worst(
                result, ngx_media_compat_track(&program->tracks[i],
                                               &candidate->tracks[j]));
        }

        if (!found) {
            /* a program track without a counterpart is not interchangeable */
            return NGX_MEDIA_COMPAT_INCOMPATIBLE;
        }
    }

    if (candidate->count > program->count) {
        /* extra tracks change the program shape */
        result = ngx_media_compat_worst(result, NGX_MEDIA_COMPAT_DEGRADED);
    }

    return result;
}

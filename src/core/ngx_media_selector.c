#include "ngx_media_selector.h"
#include "ngx_media_stream.h"

void
ngx_media_selector_evaluate(ngx_media_stream_t *stream, ngx_msec_t now,
    ngx_media_selector_result_t *out)
{
    ngx_queue_t         *q;
    ngx_media_source_t  *source;
    ngx_media_trackset_t *program;

    if (out != NULL) {
        ngx_memzero(out, sizeof(ngx_media_selector_result_t));
        out->active_eligible = 1;
    }

    if (stream == NULL) {
        return;
    }

    program = (stream->active != NULL) ? stream->active->tracks : NULL;

    for (q = ngx_queue_head(&stream->sources);
         q != (ngx_queue_t *) &stream->sources;
         q = q->next)
    {
        source = ngx_queue_data(q, ngx_media_source_t, queue);

        ngx_media_health_evaluate(&source->health, now);

        source->compat = ngx_media_compat_classify(program, source->tracks);

        ngx_media_health_tracks(&source->health,
                                source->compat != NGX_MEDIA_COMPAT_INCOMPATIBLE
                                || source == stream->active);

        if (source == stream->active) {

            if (out != NULL) {
                out->active_eligible = source->health.eligible;
            }

            continue;
        }

        /*
         * Desired state first: a source an operator disabled is not a
         * candidate however healthy it looks.
         */
        if (!source->enabled) {
            continue;
        }

        if (!source->health.eligible) {
            continue;
        }

        if (out != NULL) {
            out->eligible++;
        }

        if (source->compat == NGX_MEDIA_COMPAT_INCOMPATIBLE) {

            if (out != NULL
                && (out->emergency == NULL
                    || source->priority > out->emergency->priority))
            {
                out->emergency = source;
            }

            continue;
        }

        if (out != NULL
            && (out->best == NULL || source->priority > out->best->priority))
        {
            out->best = source;
        }
    }
}

ngx_int_t
ngx_media_selector_run(ngx_media_stream_t *stream, ngx_msec_t now,
    ngx_media_selector_result_t *out)
{
    ngx_media_selector_result_t  local;
    ngx_media_selector_result_t *res = (out != NULL) ? out : &local;

    ngx_media_selector_evaluate(stream, now, res);

    if (stream == NULL) {
        return NGX_ERROR;
    }

    if (stream->active == NULL) {

        if (res->best != NULL) {
            return ngx_media_stream_promote(stream, res->best);
        }

        if (res->emergency != NULL) {
            stream->emergency_switches++;
            return ngx_media_stream_promote(stream, res->emergency);
        }

        return NGX_OK;
    }

    if (res->active_eligible) {

        /* switchback to a higher-priority eligible source */
        if (res->best != NULL
            && res->best->priority > stream->active->priority)
        {
            switch (stream->selector.switchback) {

            case NGX_MEDIA_SWITCHBACK_AUTO:
                return ngx_media_stream_promote(stream, res->best);

            case NGX_MEDIA_SWITCHBACK_MANUAL:
            case NGX_MEDIA_SWITCHBACK_NEVER:
            default:
                break;
            }
        }

        return NGX_OK;
    }

    /* the active source lost eligibility: fail over */
    if (res->best != NULL) {
        return ngx_media_stream_promote(stream, res->best);
    }

    if (res->emergency != NULL) {
        stream->emergency_switches++;
        return ngx_media_stream_promote(stream, res->emergency);
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_selector_switchback(ngx_media_stream_t *stream, ngx_msec_t now)
{
    ngx_media_selector_result_t  res;

    if (stream == NULL) {
        return NGX_ERROR;
    }

    if (stream->selector.switchback == NGX_MEDIA_SWITCHBACK_NEVER) {
        return NGX_DECLINED;
    }

    ngx_media_selector_evaluate(stream, now, &res);

    if (res.best == NULL) {
        return NGX_DECLINED;
    }

    if (stream->active != NULL
        && res.best->priority <= stream->active->priority)
    {
        return NGX_DECLINED;
    }

    return ngx_media_stream_promote(stream, res.best);
}

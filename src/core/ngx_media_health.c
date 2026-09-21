#include "ngx_media_health.h"

/*
 * Every layer is evaluated here from the timestamps of the last good evidence,
 * so a source fails exactly when it has been showing bad evidence for the
 * failure timeout, independently of how often the selector runs.
 *
 *   transport    hard evidence: a closed transport fails immediately
 *   data/media   the last media frame must be younger than failure_timeout
 *   timestamps   the last advancing DTS must be younger than failure_timeout
 *   container    assumed valid; after an error it must stay clean for
 *                recovery_timeout before it is trusted again
 */

void
ngx_media_health_init(ngx_media_health_t *health,
    const ngx_media_selector_t *selector, ngx_msec_t now)
{
    if (health == NULL) {
        return;
    }

    ngx_memzero(health, sizeof(ngx_media_health_t));

    health->required = NGX_MEDIA_HEALTH_REQUIRED;
    health->failure_timeout = selector->failure_timeout;
    health->recovery_timeout = selector->recovery_timeout;

    health->last_media = now;
    health->last_ts_progress = now;

    /* the container is assumed valid until an error is observed */
    health->evidence = NGX_MEDIA_HEALTH_CONTAINER_VALID;

    /*
     * A source is assumed usable until the evidence says otherwise: the
     * timers then decide when to fail it and when to trust it again.
     */
    health->healthy = 1;
    health->eligible = 1;
    health->healthy_since = now;
}

void
ngx_media_health_transport(ngx_media_health_t *health, ngx_uint_t up,
    ngx_msec_t now)
{
    if (health == NULL) {
        return;
    }

    if (up) {
        health->evidence |= NGX_MEDIA_HEALTH_TRANSPORT_UP;
        return;
    }

    /* hard evidence: a closed transport is failed immediately */
    health->evidence = 0;
    health->healthy_since = 0;
    health->unhealthy_since = now;

    if (health->healthy || health->eligible) {
        health->transitions++;
    }

    health->healthy = 0;
    health->eligible = 0;
}

void
ngx_media_health_media(ngx_media_health_t *health, int64_t dts,
    ngx_msec_t now)
{
    if (health == NULL) {
        return;
    }

    health->last_media = now;

    if (dts > health->last_dts) {
        health->last_dts = dts;
        health->last_ts_progress = now;
    }
}

void
ngx_media_health_container(ngx_media_health_t *health, uint64_t errors,
    ngx_msec_t now)
{
    if (health == NULL) {
        return;
    }

    if (errors > health->container_errors) {
        health->container_errors = errors;
        health->last_container_error = now;
        health->evidence &= ~(ngx_uint_t) NGX_MEDIA_HEALTH_CONTAINER_VALID;
    }
}

void
ngx_media_health_tracks(ngx_media_health_t *health, ngx_uint_t compatible)
{
    if (health == NULL) {
        return;
    }

    if (compatible) {
        health->evidence |= NGX_MEDIA_HEALTH_TRACKS_COMPATIBLE;

    } else {
        health->evidence &= ~(ngx_uint_t) NGX_MEDIA_HEALTH_TRACKS_COMPATIBLE;
    }
}

void
ngx_media_health_evaluate(ngx_media_health_t *health, ngx_msec_t now)
{
    ngx_uint_t  required_ok;

    if (health == NULL) {
        return;
    }

    /*
     * The time-based layers only exist while the transport is up: a closed
     * session cannot have flowing data, recent media or advancing timestamps.
     */
    if ((health->evidence & NGX_MEDIA_HEALTH_TRANSPORT_UP)
        && now - health->last_media <= health->failure_timeout)
    {
        health->evidence |= NGX_MEDIA_HEALTH_DATA_FLOWING
                            | NGX_MEDIA_HEALTH_MEDIA_VALID;

    } else {
        health->evidence &= ~(ngx_uint_t) (NGX_MEDIA_HEALTH_DATA_FLOWING
                                           | NGX_MEDIA_HEALTH_MEDIA_VALID);
    }

    if ((health->evidence & NGX_MEDIA_HEALTH_TRANSPORT_UP)
        && now - health->last_ts_progress <= health->failure_timeout)
    {
        health->evidence |= NGX_MEDIA_HEALTH_TIMESTAMPS_ADVANCING;

    } else {
        health->evidence &= ~(ngx_uint_t) NGX_MEDIA_HEALTH_TIMESTAMPS_ADVANCING;
    }

    if (!(health->evidence & NGX_MEDIA_HEALTH_CONTAINER_VALID)
        && now - health->last_container_error >= health->recovery_timeout)
    {
        health->evidence |= NGX_MEDIA_HEALTH_CONTAINER_VALID;
    }

    required_ok = ((health->evidence & health->required) == health->required);

    if (required_ok) {

        if (health->healthy_since == 0) {
            health->healthy_since = now;
        }

        if (!health->healthy
            && now - health->healthy_since >= health->recovery_timeout)
        {
            health->healthy = 1;
            health->transitions++;
            health->unhealthy_since = 0;
        }

    } else if (health->healthy) {

        health->healthy = 0;
        health->transitions++;
        health->healthy_since = 0;
        health->unhealthy_since = now;
    }

    health->eligible = health->healthy;
}

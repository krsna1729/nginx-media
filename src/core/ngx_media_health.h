#ifndef NGX_MEDIA_HEALTH_H
#define NGX_MEDIA_HEALTH_H

#include "ngx_media.h"

/*
 * Layered source health (goal doc 8).
 *
 * Health is evidence, not a blended score:
 *
 *   TRANSPORT_UP          the transport session is established
 *   DATA_FLOWING          bytes arrived within the failure timeout
 *   CONTAINER_VALID       the container parsed without new errors
 *   TIMESTAMPS_ADVANCING  media timestamps keep moving (frozen detection)
 *   MEDIA_VALID           decodable media frames are being produced
 *   TRACKS_COMPATIBLE     reported by the selector against the program
 *
 * Health answers whether a source is eligible; selection chooses among the
 * eligible sources.
 *
 * Timers:
 *   - a source that stops showing progress (silent or frozen) becomes
 *     unhealthy after failure_timeout
 *   - a transport that closes is unhealthy immediately: that is hard evidence
 *   - a failed source becomes healthy again only after recovery_timeout of
 *     continuous good evidence (hysteresis)
 */

#define NGX_MEDIA_HEALTH_TRANSPORT_UP          0x01
#define NGX_MEDIA_HEALTH_DATA_FLOWING          0x02
#define NGX_MEDIA_HEALTH_CONTAINER_VALID       0x04
#define NGX_MEDIA_HEALTH_TIMESTAMPS_ADVANCING  0x08
#define NGX_MEDIA_HEALTH_MEDIA_VALID           0x10
#define NGX_MEDIA_HEALTH_TRACKS_COMPATIBLE     0x20

#define NGX_MEDIA_HEALTH_REQUIRED  (NGX_MEDIA_HEALTH_TRANSPORT_UP          \
                                    | NGX_MEDIA_HEALTH_DATA_FLOWING        \
                                    | NGX_MEDIA_HEALTH_CONTAINER_VALID     \
                                    | NGX_MEDIA_HEALTH_TIMESTAMPS_ADVANCING\
                                    | NGX_MEDIA_HEALTH_MEDIA_VALID)

/*
 * A file, pull or ingest source is bursty rather than continuous: an HLS
 * segment arrives, its frames are published at once, and then there is
 * nothing until the next segment.  The selector's failure timeout is tuned
 * for a live encoder that delivers every few milliseconds, and the
 * time-based evidence layers are derived from it, so applying that timeout
 * here would mark a healthy source unhealthy between segments.  Ten seconds
 * covers a segment cadence with room to spare, and a source that has really
 * stopped is still failed within it.
 */
#define NGX_MEDIA_BURSTY_FAILURE_TIMEOUT  10000

/* the health state lives in ngx_media.h with the rest of the model */

void ngx_media_health_init(ngx_media_health_t *health,
    const ngx_media_selector_t *selector, ngx_msec_t now);

/* transport closure is hard evidence: the source fails immediately */
void ngx_media_health_transport(ngx_media_health_t *health, ngx_uint_t up,
    ngx_msec_t now);

void ngx_media_health_media(ngx_media_health_t *health, int64_t dts,
    ngx_msec_t now);

/* cumulative demuxer error counters (continuity, PSI, PES, sync, CRC) */
void ngx_media_health_container(ngx_media_health_t *health,
    uint64_t errors, ngx_msec_t now);

void ngx_media_health_tracks(ngx_media_health_t *health, ngx_uint_t compatible);

/* applies the timeouts; call periodically */
void ngx_media_health_evaluate(ngx_media_health_t *health, ngx_msec_t now);

#endif /* NGX_MEDIA_HEALTH_H */

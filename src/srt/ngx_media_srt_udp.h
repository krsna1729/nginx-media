#ifndef NGX_MEDIA_SRT_UDP_H
#define NGX_MEDIA_SRT_UDP_H

#include "ngx_media_srt_transport.h"

/*
 * Second transport backend (goal doc 11.1, phase 10): an independent
 * implementation of the same contract over plain UDP, used to qualify the
 * boundary and to give the qualification suite something to compare the
 * production backend against.  It has none of SRT's reliability features; see
 * ngx_media_srt_udp.c for the exact scope.
 */
extern ngx_media_srt_ops_t  ngx_media_srt_udp_ops;

#endif /* NGX_MEDIA_SRT_UDP_H */

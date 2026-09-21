#define _POSIX_C_SOURCE 200809L

/*
 * MPEG-TS demux fixture harness (phase 2 exit criteria).
 *
 * Reads a real MPEG-TS file, feeds it to the demuxer in small chunks (so
 * transport packets span feed calls), and validates the structural facts the
 * media core depends on: tracks discovered from PSI, Annex B video access
 * units with configuration, ADTS audio frames, monotonic DTS, and no
 * transport, continuity, PSI, CRC or PES errors.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_media_ts_demux.h"

typedef struct {
    uint64_t    video;
    uint64_t    audio;
    uint64_t    keyframes;
    uint64_t    config;
    uint64_t    other;
    uint64_t    bad_payloads;
    int64_t     last_video_dts;
    uint64_t    dts_regressions;
    uint64_t    tracks_calls;
    ngx_uint_t  ntracks;
    ngx_media_track_t tracks[8];
} fixture_t;

static void
frame_cb(void *ctx, const ngx_media_frame_t *frame)
{
    fixture_t  *f = ctx;

    if (frame->config) {
        f->config++;
        return;
    }

    if (frame->media_type == NGX_MEDIA_TYPE_VIDEO) {
        const u_char  *d;
        size_t         len;

        f->video++;

        if (frame->keyframe) {
            f->keyframes++;
        }

        if (frame->payload == NULL
            || frame->payload_format != NGX_MEDIA_PAYLOAD_ANNEXB)
        {
            f->bad_payloads++;
            return;
        }

        d = ngx_media_buf_data(frame->payload);
        len = ngx_media_buf_size(frame->payload);

        if (len < 4
            || !(d[0] == 0 && d[1] == 0 && (d[2] == 1
                                            || (d[2] == 0 && d[3] == 1))))
        {
            f->bad_payloads++;
        }

        if (f->video > 1 && frame->dts < f->last_video_dts) {
            f->dts_regressions++;
        }

        f->last_video_dts = frame->dts;

    } else if (frame->media_type == NGX_MEDIA_TYPE_AUDIO) {

        f->audio++;

        if (frame->payload == NULL
            || frame->payload_format != NGX_MEDIA_PAYLOAD_ADTS)
        {
            f->bad_payloads++;
        }

    } else {
        f->other++;
    }
}

static void
tracks_cb(void *ctx, const ngx_media_trackset_t *tracks)
{
    fixture_t  *f = ctx;
    ngx_uint_t  i;

    f->tracks_calls++;
    f->ntracks = (tracks->count < 8) ? tracks->count : 8;

    for (i = 0; i < f->ntracks; i++) {
        f->tracks[i] = tracks->tracks[i];
    }
}

int
main(int argc, char **argv)
{
    const char                 *path;
    FILE                       *fp;
    fixture_t                   f;
    ngx_media_ts_demux_t        demux;
    ngx_media_ts_demux_conf_t   conf;
    ngx_media_ts_sink_t         sink;
    ngx_media_ts_demux_stats_t  stats;
    u_char                      buf[4096];
    size_t                      n;
    ngx_uint_t                  i;
    int                         rc = 1;

    path = (argc > 1) ? argv[1] : "fixture.ts";

    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }

    ngx_memzero(&f, sizeof(f));

    conf.max_tracks = 8;
    conf.max_au_bytes = 1024 * 1024;

    sink.tracks = tracks_cb;
    sink.frame = frame_cb;

    if (ngx_media_ts_demux_init(&demux, &conf, &sink, &f, NULL) != NGX_OK) {
        fprintf(stderr, "demux init failed\n");
        fclose(fp);
        return 1;
    }

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (ngx_media_ts_demux_feed(&demux, buf, n) != NGX_OK) {
            fprintf(stderr, "feed failed\n");
            goto done;
        }
    }

    ngx_media_ts_demux_flush(&demux);
    ngx_media_ts_demux_stats(&demux, &stats);

    printf("FILE %s\n", path);
    printf("TRACKS calls=%llu count=%llu\n",
           (unsigned long long) f.tracks_calls,
           (unsigned long long) f.ntracks);

    for (i = 0; i < f.ntracks; i++) {
        printf("TRACK %llu media=%llu codec=%llu format=%llu rate=%llu "
               "channels=%llu config=%s\n",
               (unsigned long long) i,
               (unsigned long long) f.tracks[i].media_type,
               (unsigned long long) f.tracks[i].codec,
               (unsigned long long) f.tracks[i].payload_format,
               (unsigned long long) f.tracks[i].sample_rate,
               (unsigned long long) f.tracks[i].channels,
               f.tracks[i].config != NULL ? "yes" : "no");
    }

    printf("FRAMES video=%llu audio=%llu keyframes=%llu config=%llu "
           "other=%llu bad_payloads=%llu dts_regressions=%llu\n",
           (unsigned long long) f.video,
           (unsigned long long) f.audio,
           (unsigned long long) f.keyframes,
           (unsigned long long) f.config,
           (unsigned long long) f.other,
           (unsigned long long) f.bad_payloads,
           (unsigned long long) f.dts_regressions);

    printf("STATS bytes=%llu packets=%llu pcr=%llu last_pcr=%lld "
           "sync_errors=%llu transport_errors=%llu continuity_errors=%llu "
           "psi_errors=%llu crc_errors=%llu pes_errors=%llu "
           "au_overflows=%llu unsupported=%llu\n",
           (unsigned long long) stats.bytes,
           (unsigned long long) stats.packets,
           (unsigned long long) stats.has_pcr,
           (long long) stats.last_pcr,
           (unsigned long long) stats.sync_errors,
           (unsigned long long) stats.transport_errors,
           (unsigned long long) stats.continuity_errors,
           (unsigned long long) stats.psi_errors,
           (unsigned long long) stats.crc_errors,
           (unsigned long long) stats.pes_errors,
           (unsigned long long) stats.au_overflows,
           (unsigned long long) stats.unsupported_streams);

    rc = 0;

    if (f.video == 0 || f.audio == 0 || f.keyframes == 0 || f.config < 2) {
        fprintf(stderr, "missing frames: video=%llu audio=%llu keyframes=%llu "
                        "config=%llu\n",
                (unsigned long long) f.video,
                (unsigned long long) f.audio,
                (unsigned long long) f.keyframes,
                (unsigned long long) f.config);
        rc = 1;
    }

    if (f.bad_payloads != 0 || f.dts_regressions != 0) {
        fprintf(stderr, "payload problems: bad=%llu dts_regressions=%llu\n",
                (unsigned long long) f.bad_payloads,
                (unsigned long long) f.dts_regressions);
        rc = 1;
    }

    if (stats.has_pcr == 0 || stats.sync_errors != 0
        || stats.transport_errors != 0 || stats.continuity_errors != 0
        || stats.psi_errors != 0 || stats.crc_errors != 0
        || stats.pes_errors != 0 || stats.au_overflows != 0)
    {
        fprintf(stderr, "transport errors: pcr=%llu sync=%llu transport=%llu "
                        "continuity=%llu psi=%llu crc=%llu pes=%llu "
                        "overflow=%llu\n",
                (unsigned long long) stats.has_pcr,
                (unsigned long long) stats.sync_errors,
                (unsigned long long) stats.transport_errors,
                (unsigned long long) stats.continuity_errors,
                (unsigned long long) stats.psi_errors,
                (unsigned long long) stats.crc_errors,
                (unsigned long long) stats.pes_errors,
                (unsigned long long) stats.au_overflows);
        rc = 1;
    }

done:

    ngx_media_ts_demux_destroy(&demux);
    fclose(fp);

    printf("RESULT %s\n", (rc == 0) ? "ok" : "failed");

    return rc;
}

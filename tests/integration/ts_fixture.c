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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_media_ts_demux.h"
#include "ngx_media_ts_mux.h"

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

#define REMUX_MAX_FRAMES 8192

typedef struct {
    ngx_uint_t  media_type;
    ngx_uint_t  codec;
    int64_t     pts;
    int64_t     dts;
    size_t      len;
    unsigned    keyframe:1;
} expected_t;

typedef struct {
    ngx_media_ts_mux_t     mux;
    ngx_media_ts_demux_t  *demux;
    ngx_media_ts_burst_t   burst;
    expected_t             expected[REMUX_MAX_FRAMES];
    ngx_uint_t             nexpected;
    ngx_uint_t             nchecked;
    ngx_uint_t             nmatched;
    ngx_uint_t             nmismatched;
    ngx_uint_t             video_expected;
    ngx_uint_t             audio_expected;
    ngx_uint_t             video_checked;
    ngx_uint_t             audio_checked;
    ngx_uint_t             video_index;    /* per-track cursors: the demuxer's
                                              cross-track emission order is an
                                              artifact of access-unit
                                              boundaries */
    ngx_uint_t             audio_index;
    uint64_t               burst_bytes;
    uint64_t               burst_count;
    uint64_t               burst_min;
    uint64_t               burst_max;
    size_t                 burst_capacity;

} remux_t;

static void
remux_flush(remux_t *r)
{
    size_t  len;

    len = ngx_media_ts_burst_size(&r->burst);
    if (len == 0) {
        return;
    }

    (void) ngx_media_ts_mux_burst_end(&r->mux, &r->burst);

    r->burst_bytes += len;
    r->burst_count++;

    if (r->burst_min == 0 || len < r->burst_min) {
        r->burst_min = len;
    }

    if (len > r->burst_max) {
        r->burst_max = len;
    }

    /* feed the second pass in small chunks: packets span calls */
    {
        u_char  *data = ngx_media_buf_data(r->burst.backing);
        size_t   off = 0;

        while (off < len) {
            size_t  take = (len - off > 1024) ? 1024 : len - off;

            (void) ngx_media_ts_demux_feed(r->demux, data + off, take);
            off += take;
        }
    }

    ngx_media_ts_mux_burst_destroy(&r->burst);
}

/* pass 1 sink: mux every media frame and feed it to the second demuxer */
static void
remux_source_frame(void *ctx, const ngx_media_frame_t *frame)
{
    remux_t  *r = ctx;

    if (frame->config || frame->payload == NULL) {
        return;
    }

    if (r->nexpected < REMUX_MAX_FRAMES) {
        expected_t  *e = &r->expected[r->nexpected];

        e->media_type = frame->media_type;
        e->codec = frame->codec;
        e->pts = frame->pts;
        e->dts = frame->dts;
        e->len = ngx_media_buf_size(frame->payload);
        e->keyframe = frame->keyframe ? 1 : 0;

        if (frame->media_type == NGX_MEDIA_TYPE_AUDIO) {
            r->audio_expected++;

        } else {
            r->video_expected++;
        }

        r->nexpected++;
    }

    if (r->burst.backing == NULL
        && ngx_media_ts_mux_burst_init(&r->mux, &r->burst,
                                       r->burst_capacity) != NGX_OK)
    {
        return;
    }

    if (ngx_media_ts_mux_write_frame(&r->mux, &r->burst, frame, 0) == NGX_AGAIN)
    {
        remux_flush(r);

        if (ngx_media_ts_mux_burst_init(&r->mux, &r->burst,
                                        r->burst_capacity) != NGX_OK)
        {
            return;
        }

        (void) ngx_media_ts_mux_write_frame(&r->mux, &r->burst, frame, 0);
    }
}

/* pass 2 sink: compare against what was muxed */
static void
remux_check_frame(void *ctx, const ngx_media_frame_t *frame)
{
    remux_t     *r = ctx;
    expected_t  *e;
    ngx_uint_t  *index, *checked;
    ngx_uint_t   media_type = frame->media_type;

    if (frame->config || frame->payload == NULL) {
        return;
    }

    if (media_type == NGX_MEDIA_TYPE_AUDIO) {
        index = &r->audio_index;
        checked = &r->audio_checked;

    } else {
        index = &r->video_index;
        checked = &r->video_checked;
    }

    while (*index < r->nexpected
           && r->expected[*index].media_type != media_type)
    {
        (*index)++;
    }

    if (*index >= r->nexpected) {
        r->nmismatched++;
        return;
    }

    e = &r->expected[*index];
    (*index)++;
    (*checked)++;

    if (e->codec != frame->codec
        || e->pts != frame->pts
        || e->dts != frame->dts
        || e->len != ngx_media_buf_size(frame->payload)
        || e->keyframe != (frame->keyframe ? 1 : 0))
    {
        if (r->nmismatched < 4) {
            fprintf(stderr, "mismatch: type=%lu expected codec=%lu pts=%lld "
                            "len=%zu key=%u, got codec=%lu pts=%lld len=%zu "
                            "key=%u\n",
                    (unsigned long) media_type,
                    (unsigned long) e->codec, (long long) e->pts, e->len,
                    (unsigned) e->keyframe,
                    (unsigned long) frame->codec, (long long) frame->pts,
                    (size_t) ngx_media_buf_size(frame->payload),
                    (unsigned) frame->keyframe);
        }

        r->nmismatched++;

    } else {
        r->nmatched++;
    }

    r->nchecked++;
}

static void
remux_tracks_ignore(void *ctx, const ngx_media_trackset_t *tracks)
{
    (void) ctx;
    (void) tracks;
}

int
main(int argc, char **argv)
{
    const char                 *path;
    char                       *end;
    FILE                       *fp;
    fixture_t                   f;
    ngx_media_ts_demux_t        demux, demux_remux, demux_check;
    ngx_media_ts_demux_conf_t   conf;
    ngx_media_ts_sink_t         sink, remux_sink, check_sink;
    ngx_media_ts_demux_stats_t  stats, check_stats;
    remux_t                     r;
    u_char                      buf[4096];
    size_t                      n, burst_capacity = 64 * 1024;
    unsigned long long          requested;
    ngx_uint_t                  i;
    int                         rc = 1;

    path = (argc > 1) ? argv[1] : "fixture.ts";
    if (argc > 2) {
        requested = strtoull(argv[2], &end, 10);

        if (argv[2][0] == '\0' || *end != '\0' || requested == 0
            || requested > SIZE_MAX)
        {
            fprintf(stderr, "invalid burst capacity: %s\n", argv[2]);
            return 2;
        }

        burst_capacity = (size_t) requested;
    }


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

    /*
     * Second pass: demux -> mux into bursts -> demux again.  This proves the
     * muxer against real media and that burst preparation is lossless.
     */
    ngx_memzero(&r, sizeof(r));
    r.burst_capacity = burst_capacity;

    if (ngx_media_ts_mux_init(&r.mux, NULL, NULL) != NGX_OK
        || ngx_media_ts_mux_set_tracks(&r.mux, &demux.trackset) != NGX_OK)
    {
        fprintf(stderr, "mux init failed\n");
        goto done;
    }

    check_sink.tracks = remux_tracks_ignore;
    check_sink.frame = remux_check_frame;

    if (ngx_media_ts_demux_init(&demux_check, &conf, &check_sink, &r, NULL)
        != NGX_OK)
    {
        fprintf(stderr, "check demux init failed\n");
        goto done;
    }

    r.demux = &demux_check;

    remux_sink.tracks = remux_tracks_ignore;
    remux_sink.frame = remux_source_frame;

    if (ngx_media_ts_demux_init(&demux_remux, &conf, &remux_sink, &r, NULL)
        != NGX_OK)
    {
        fprintf(stderr, "remux demux init failed\n");
        goto done;
    }

    rewind(fp);

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        (void) ngx_media_ts_demux_feed(&demux_remux, buf, n);
    }

    ngx_media_ts_demux_flush(&demux_remux);
    remux_flush(&r);
    ngx_media_ts_demux_flush(&demux_check);
    ngx_media_ts_demux_stats(&demux_check, &check_stats);

    printf("REMUX capacity=%llu frames=%llu (video=%llu audio=%llu) "
           "checked=%llu matched=%llu mismatched=%llu bytes=%llu "
           "bursts=%llu min_burst=%llu max_burst=%llu frame_rollovers=%llu "
           "packets=%llu continuity_errors=%llu pes_errors=%llu\n",
           (unsigned long long) r.burst_capacity,
           (unsigned long long) r.nexpected,
           (unsigned long long) r.video_expected,
           (unsigned long long) r.audio_expected,
           (unsigned long long) r.nchecked,
           (unsigned long long) r.nmatched,
           (unsigned long long) r.nmismatched,
           (unsigned long long) r.burst_bytes,
           (unsigned long long) r.burst_count,
           (unsigned long long) r.burst_min,
           (unsigned long long) r.burst_max,
           (unsigned long long) r.mux.frame_drops,
           (unsigned long long) r.mux.packets,
           (unsigned long long) check_stats.continuity_errors,
           (unsigned long long) check_stats.pes_errors);

    rc = 0;

    if (r.nexpected == 0 || r.nchecked != r.nexpected
        || r.nmatched != r.nexpected || r.nmismatched != 0
        || r.video_checked != r.video_expected
        || r.audio_checked != r.audio_expected)
    {
        fprintf(stderr, "remux mismatch: expected=%llu checked=%llu "
                        "matched=%llu mismatched=%llu\n",
                (unsigned long long) r.nexpected,
                (unsigned long long) r.nchecked,
                (unsigned long long) r.nmatched,
                (unsigned long long) r.nmismatched);
        rc = 1;
    }

    if (check_stats.continuity_errors != 0 || check_stats.pes_errors != 0
        || check_stats.sync_errors != 0 || check_stats.crc_errors != 0
        || check_stats.psi_errors != 0)
    {
        fprintf(stderr, "remux produced container errors\n");
        rc = 1;
    }

    ngx_media_ts_demux_destroy(&demux_remux);
    ngx_media_ts_demux_destroy(&demux_check);
    ngx_media_ts_mux_destroy(&r.mux);

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

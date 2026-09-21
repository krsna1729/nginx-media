#define _POSIX_C_SOURCE 200809L

/*
 * Redundant-source switch harness (phase 3 exit criteria).
 *
 * Two real MPEG-TS fixtures are demuxed concurrently into two sources of one
 * logical stream.  Both stay hot; a manual promotion switches the program at
 * the standby's cached keyframe.  The harness proves:
 *
 *   - the program timeline is monotonic (and strictly so for video) across
 *     the switch
 *   - the switch happens exactly once and bumps the generation
 *   - the first program frame after the switch is a keyframe
 *   - the demoted source stops writing to the program but keeps its GOP cache
 *     hot, inside its hard ceilings
 *   - readers holding a pre-switch cursor see the generation change
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_media_stream.h"
#include "ngx_media_ts_demux.h"

#define PREROLL_UNITS 512
#define PREROLL_BYTES (2 * 1024 * 1024)
#define FEED_UNITS 8192
#define SWITCH_AFTER_FRAMES 40

typedef struct {
    ngx_media_stream_t  *stream;
    ngx_media_source_t  *source;
} source_ctx_t;

static void
sink_frame(void *ctx, const ngx_media_frame_t *frame)
{
    source_ctx_t  *c = ctx;

    (void) ngx_media_stream_publish(c->stream, c->source, frame,
                                    (ngx_msec_t) (frame->dts / 90));
}

static void
sink_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    source_ctx_t  *c = ctx;
    ngx_uint_t     i;

    for (i = 0; i < tracks->count; i++) {
        if (tracks->tracks[i].media_type == NGX_MEDIA_TYPE_VIDEO) {
            c->source->has_video = 1;
        }
    }
}

typedef struct {
    ngx_media_ts_demux_t  demux;
    source_ctx_t          ctx;
} endpoint_t;

static void
endpoint_init(endpoint_t *ep, ngx_media_stream_t *stream,
    ngx_media_source_t *source)
{
    ngx_media_ts_demux_conf_t  conf;
    ngx_media_ts_sink_t        sink;

    conf.max_tracks = 8;
    conf.max_au_bytes = 1024 * 1024;

    sink.tracks = sink_tracks;
    sink.frame = sink_frame;

    ep->ctx.stream = stream;
    ep->ctx.source = source;

    if (ngx_media_ts_demux_init(&ep->demux, &conf, &sink, &ep->ctx, NULL)
        != NGX_OK)
    {
        fprintf(stderr, "demux init failed\n");
        exit(1);
    }

    ngx_media_source_preroll_destroy(source);

    if (ngx_media_source_preroll_init(source, PREROLL_UNITS, PREROLL_BYTES,
                                      NULL) != NGX_OK)
    {
        fprintf(stderr, "preroll init failed\n");
        exit(1);
    }
}

int
main(int argc, char **argv)
{
    ngx_pool_t          *pool;
    ngx_media_stream_t   stream;
    ngx_media_source_t  *a, *b;
    ngx_media_feed_conf_t    feed_conf;
    ngx_media_cursor_t       pre_cursor;
    endpoint_t               ep_a, ep_b;
    char                     buf_a[4096], buf_b[4096];
    FILE                    *fa, *fb;
    size_t                   na, nb;
    ngx_uint_t               switched = 0, mismatch_seen = 0;
    uint64_t                 a_frames_at_switch = 0;
    uint64_t                 switch_seq = 0;
    int                      rc = 1;

    if (argc < 3) {
        fprintf(stderr, "usage: source_switch <a.ts> <b.ts>\n");
        return 1;
    }

    fa = fopen(argv[1], "rb");
    fb = fopen(argv[2], "rb");

    if (fa == NULL || fb == NULL) {
        fprintf(stderr, "cannot open fixtures\n");
        return 1;
    }

    pool = ngx_create_pool(8192, NULL);

    feed_conf.max_units = FEED_UNITS;
    feed_conf.max_bytes = 64 * 1024 * 1024;
    feed_conf.max_age = 0;

    if (ngx_media_stream_init(&stream, pool, NULL,
                              &(ngx_str_t) { 4, (u_char *) "live" },
                              &(ngx_str_t) { 4, (u_char *) "news" },
                              &feed_conf)
        != NGX_OK)
    {
        fprintf(stderr, "stream init failed\n");
        return 1;
    }

    a = ngx_media_stream_source_add(&stream,
                                    &(ngx_str_t) { 9, (u_char *) "encoder-a" },
                                    NGX_MEDIA_SOURCE_SRT, 100, NULL);
    b = ngx_media_stream_source_add(&stream,
                                    &(ngx_str_t) { 9, (u_char *) "encoder-b" },
                                    NGX_MEDIA_SOURCE_SRT, 90, NULL);

    if (a == NULL || b == NULL) {
        fprintf(stderr, "source registration failed\n");
        return 1;
    }

    endpoint_init(&ep_a, &stream, a);
    endpoint_init(&ep_b, &stream, b);

    /* bootstrap: the first source becomes active at its first keyframe */
    if (ngx_media_stream_promote(&stream, a) != NGX_OK) {
        fprintf(stderr, "bootstrap promotion failed\n");
        return 1;
    }

    ngx_media_feed_cursor_init(&stream.program_feed, &pre_cursor);

    for ( ;; ) {

        na = fread(buf_a, 1, sizeof(buf_a), fa);
        if (na > 0) {
            (void) ngx_media_ts_demux_feed(&ep_a.demux, (u_char *) buf_a, na);
        }

        nb = fread(buf_b, 1, sizeof(buf_b), fb);
        if (nb > 0) {
            (void) ngx_media_ts_demux_feed(&ep_b.demux, (u_char *) buf_b, nb);
        }

        if (na == 0 && nb == 0) {
            break;
        }

        if (!switched && stream.program_frames >= SWITCH_AFTER_FRAMES) {

            /* the first program frame of the new generation gets this */
            switch_seq = ngx_media_feed_head(&stream.program_feed);

            if (ngx_media_stream_promote(&stream, b) != NGX_OK) {
                fprintf(stderr, "promotion failed\n");
                return 1;
            }

            switched = 1;
            a_frames_at_switch = a->frames_out;
        }
    }

    ngx_media_ts_demux_flush(&ep_a.demux);
    ngx_media_ts_demux_flush(&ep_b.demux);

    {
        ngx_media_frame_t  out[8];
        ngx_uint_t         count;
        ngx_uint_t         status;

        status = ngx_media_feed_read(&stream.program_feed, &pre_cursor, 8, 0, 1000,
                                     out, &count);

        if (status == NGX_MEDIA_FEED_GENERATION_MISMATCH) {
            mismatch_seen = 1;
        }
    }

    printf("SOURCES a_frames_in=%llu a_frames_out=%llu b_frames_in=%llu "
           "b_frames_out=%llu\n",
           (unsigned long long) a->frames_in,
           (unsigned long long) a->frames_out,
           (unsigned long long) b->frames_in,
           (unsigned long long) b->frames_out);

    printf("PROGRAM frames=%llu switches=%llu generation=%llu "
           "switched=%llu switch_seq=%llu\n",
           (unsigned long long) stream.program_frames,
           (unsigned long long) stream.switches,
           (unsigned long long) stream.generation,
           (unsigned long long) switched,
           (unsigned long long) switch_seq);

    printf("PREROLL a_units=%llu a_bytes=%llu a_overflows=%llu "
           "b_units=%llu b_bytes=%llu b_overflows=%llu\n",
           (unsigned long long) ngx_media_source_preroll_units(a),
           (unsigned long long) ngx_media_source_preroll_bytes(a),
           (unsigned long long) a->preroll.overflows,
           (unsigned long long) ngx_media_source_preroll_units(b),
           (unsigned long long) ngx_media_source_preroll_bytes(b),
           (unsigned long long) b->preroll.overflows);

    {
        ngx_media_cursor_t  cursor;
        ngx_media_frame_t   out[64];
        ngx_uint_t          count, i, status;
        uint64_t            frames = 0, regressions = 0, video_regressions = 0;
        uint64_t            video_frames = 0, keyframe_at_switch = 0;
        int64_t             first_dts = 0, last_dts = 0, last = 0;
        int64_t             last_video = 0;

        ngx_media_feed_cursor_init(&stream.program_feed, &cursor);
        cursor.next_sequence = ngx_media_feed_tail(&stream.program_feed);

        for ( ;; ) {
            status = ngx_media_feed_read(&stream.program_feed, &cursor, 64, 0, 1000,
                                         out, &count);

            if (status != NGX_MEDIA_FEED_BATCH) {
                break;
            }

            for (i = 0; i < count; i++) {
                uint64_t  abs_seq = cursor.next_sequence - count + i;

                if (frames == 0) {
                    first_dts = out[i].dts;
                }

                if (frames > 0 && out[i].dts < last) {
                    regressions++;
                }

                if (out[i].media_type == NGX_MEDIA_TYPE_VIDEO) {
                    video_frames++;

                    if (video_frames > 1 && out[i].dts <= last_video) {
                        video_regressions++;
                    }

                    last_video = out[i].dts;
                }

                if (abs_seq == switch_seq && out[i].keyframe) {
                    keyframe_at_switch = 1;
                }

                last = out[i].dts;
                last_dts = out[i].dts;
                frames++;
            }

            ngx_media_feed_release(out, count);
        }

        printf("TIMELINE frames=%llu regressions=%llu video_frames=%llu "
               "video_regressions=%llu first_dts=%lld last_dts=%lld "
               "keyframe_at_switch=%llu\n",
               (unsigned long long) frames,
               (unsigned long long) regressions,
               (unsigned long long) video_frames,
               (unsigned long long) video_regressions,
               (long long) first_dts,
               (long long) last_dts,
               (unsigned long long) keyframe_at_switch);

        rc = 0;

        if (!switched || stream.switches != 1 || stream.generation != 2) {
            fprintf(stderr, "expected exactly one switch to generation 2\n");
            rc = 1;
        }

        if (frames == 0 || regressions != 0 || video_regressions != 0) {
            fprintf(stderr, "program timeline is not monotonic\n");
            rc = 1;
        }

        if (!keyframe_at_switch) {
            fprintf(stderr, "the switch did not start at a keyframe\n");
            rc = 1;
        }

        if (a->frames_out != a_frames_at_switch) {
            fprintf(stderr, "the demoted source kept writing to the program\n");
            rc = 1;
        }

        if (b->frames_out == 0) {
            fprintf(stderr, "the promoted source never wrote to the program\n");
            rc = 1;
        }

        if (!mismatch_seen) {
            fprintf(stderr, "readers did not see the generation change\n");
            rc = 1;
        }

        if (ngx_media_source_preroll_units(a) > PREROLL_UNITS
            || ngx_media_source_preroll_bytes(a) > PREROLL_BYTES
            || ngx_media_source_preroll_units(b) > PREROLL_UNITS
            || ngx_media_source_preroll_bytes(b) > PREROLL_BYTES)
        {
            fprintf(stderr, "a standby cache exceeded its ceiling\n");
            rc = 1;
        }

        if (ngx_media_source_preroll_units(a) == 0) {
            fprintf(stderr, "the demoted source stopped caching a hot GOP\n");
            rc = 1;
        }

        if (a->preroll.overflows != 0 || b->preroll.overflows != 0) {
            fprintf(stderr, "a standby cache overflowed during the run\n");
            rc = 1;
        }
    }

    printf("RESULT %s\n", (rc == 0) ? "ok" : "failed");

    ngx_media_ts_demux_destroy(&ep_a.demux);
    ngx_media_ts_demux_destroy(&ep_b.demux);
    ngx_media_stream_destroy(&stream);
    ngx_destroy_pool(pool);

    fclose(fa);
    fclose(fb);

    return rc;
}

/* setenv() for the allocation poison switch */
#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_hls_segmenter.h"
#include "ngx_media_ts_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/* unterminated buffers must fail instead of reading zeroed heap */
static void __attribute__((constructor)) ngx_media_hls_poison_alloc(void)
{
    setenv("NGX_MEDIA_TEST_POISON", "1", 1);
}

#define HLS_DIR ".build/hls-test"

/*
 * The segmenter writes into HLS_DIR, and the test then reads back what it
 * wrote.  The path is relative, so this has to create it: the suite is run
 * from tests/unit by `make -C tests/unit`, where .build/ does not exist, and
 * without this the test depends on being run from a directory that happens to
 * have one.  That difference is what made it pass locally and crash on CI.
 */
static void
ensure_dir(const char *path)
{
    char  tmp[512];
    char *p;

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int) sizeof(tmp)) {
        return;
    }

    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            (void) mkdir(tmp, 0755);
            *p = '/';
        }
    }

    (void) mkdir(tmp, 0755);
}

typedef struct {
    uint64_t  frames;
    uint64_t  keyframes;
    int64_t   first_dts;
    int64_t   last_dts;
    uint64_t  discontinuities;
} read_back_t;

static void
readback_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    (void) ctx;
    (void) tracks;
}

static void
readback_frame(void *ctx, const ngx_media_frame_t *frame)
{
    read_back_t  *r = ctx;

    if (frame->config) {
        return;
    }

    if (r->frames == 0) {
        r->first_dts = frame->dts;
    }

    r->frames++;

    if (frame->keyframe) {
        r->keyframes++;
    }

    r->last_dts = frame->dts;
}

/* reads a segment file back through the demuxer: the strongest check */
static ngx_int_t
read_back(const char *path, read_back_t *out, ngx_media_ts_demux_stats_t *stats)
{
    ngx_media_ts_demux_t       demux;
    ngx_media_ts_demux_conf_t  conf;
    ngx_media_ts_sink_t        sink;
    FILE                      *fp;
    u_char                     buf[4096];
    size_t                     n;

    ngx_memzero(out, sizeof(read_back_t));

    conf.max_tracks = 8;
    conf.max_au_bytes = 1024 * 1024;

    sink.tracks = readback_tracks;
    sink.frame = readback_frame;

    if (ngx_media_ts_demux_init(&demux, &conf, &sink, out, NULL) != NGX_OK) {
        return NGX_ERROR;
    }

    fp = fopen(path, "rb");

    if (fp == NULL) {
        ngx_media_ts_demux_destroy(&demux);
        return NGX_ERROR;
    }

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        (void) ngx_media_ts_demux_feed(&demux, buf, n);
    }

    ngx_media_ts_demux_flush(&demux);
    ngx_media_ts_demux_stats(&demux, stats);

    fclose(fp);
    ngx_media_ts_demux_destroy(&demux);

    return NGX_OK;
}

typedef struct {
    ngx_media_ts_mux_t       mux;
    ngx_media_trackset_t     tracks;
} fixture_t;

static void
fixture_init(fixture_t *f)
{
    ngx_media_track_t  track;

    ngx_memzero(f, sizeof(fixture_t));

    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_init(&f->mux, NULL, NULL), NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_trackset_init(&f->tracks, 4, NULL), NGX_OK);

    ngx_memzero(&track, sizeof(track));
    track.media_type = NGX_MEDIA_TYPE_VIDEO;
    track.codec = NGX_MEDIA_CODEC_H264;
    track.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    TEST_ASSERT(ngx_media_trackset_add(&f->tracks, &track) >= 0);

    ngx_memzero(&track, sizeof(track));
    track.media_type = NGX_MEDIA_TYPE_AUDIO;
    track.codec = NGX_MEDIA_CODEC_AAC;
    track.payload_format = NGX_MEDIA_PAYLOAD_ADTS;
    track.sample_rate = 48000;
    track.channels = 2;
    TEST_ASSERT(ngx_media_trackset_add(&f->tracks, &track) >= 0);

    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_set_tracks(&f->mux, &f->tracks),
                       NGX_OK);
}

static ngx_media_frame_t
video(size_t len, int64_t dts, unsigned keyframe)
{
    ngx_media_frame_t  frame;
    ngx_media_buf_t   *payload;
    u_char            *p;

    ngx_media_frame_init(&frame);

    frame.media_type = NGX_MEDIA_TYPE_VIDEO;
    frame.codec = NGX_MEDIA_CODEC_H264;
    frame.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    frame.pts = dts;
    frame.dts = dts;
    frame.keyframe = keyframe ? 1 : 0;

    payload = ngx_media_buf_alloc(len);
    TEST_ASSERT_NOT_NULL(payload);

    p = ngx_media_buf_data(payload);
    p[0] = 0x00;
    p[1] = 0x00;
    p[2] = 0x00;
    p[3] = 0x01;
    p[4] = keyframe ? 0x65 : 0x41;
    if (len > 5) {
        memset(p + 5, 0x5A, len - 5);
    }

    (void) ngx_media_buf_freeze(payload, len);
    frame.payload = payload;

    return frame;
}

/*
 * Produces a burst: `frames` video access units spaced `step` ticks apart,
 * with audio frames interleaved at the start of every video frame.
 */
static ngx_int_t
make_burst(fixture_t *f, ngx_media_ts_burst_t *burst, ngx_uint_t frames,
    int64_t first_dts, int64_t step, unsigned keyframe_every)
{
    ngx_uint_t         i;
    ngx_media_frame_t  frame;

    if (ngx_media_ts_mux_burst_init(&f->mux, burst, 128 * 1024) != NGX_OK) {
        return NGX_ERROR;
    }

    for (i = 0; i < frames; i++) {
        int64_t  dts = first_dts + (int64_t) i * step;

        frame = video(400, dts, keyframe_every && (i % keyframe_every) == 0);
        TEST_ASSERT_EQ_INT(ngx_media_ts_mux_write_frame(&f->mux, burst, &frame,
                                                        0), NGX_OK);
        ngx_media_frame_release(&frame);
    }

    return ngx_media_ts_mux_burst_end(&f->mux, burst);
}

static size_t
file_size(const char *path)
{
    struct stat  st;

    if (stat(path, &st) != 0) {
        return 0;
    }

    return (size_t) st.st_size;
}

int
main(void)
{
    fixture_t              f;
    ngx_media_hls_t        hls;
    ngx_media_hls_conf_t   conf;
    ngx_media_ts_burst_t   burst;
    read_back_t            back;
    ngx_media_ts_demux_stats_t stats;
    char                   path[256];
    uint64_t               segments;

    ensure_dir(HLS_DIR);
    ensure_dir(HLS_DIR "-small");

    fixture_init(&f);

    TEST_CASE("configuration and init");
    ngx_media_hls_conf_default(&conf);
    TEST_ASSERT_EQ_U64(conf.target_duration, 6000);

    /* a fast test profile: 1s target, 0.5s minimum, 2s maximum */
    conf.path.data = (u_char *) HLS_DIR;
    conf.path.len = sizeof(HLS_DIR) - 1;
    conf.playlist_name.data = (u_char *) "index.m3u8";
    conf.playlist_name.len = sizeof("index.m3u8") - 1;
    conf.segment_prefix.data = (u_char *) "seg-";
    conf.segment_prefix.len = sizeof("seg-") - 1;
    conf.target_duration = 1000;
    conf.min_duration = 200;
    conf.max_duration = 2000;
    conf.max_segment_bytes = 1024 * 1024;
    conf.max_segments = 3;
    conf.max_retained_bytes = 8 * 1024 * 1024;

    TEST_ASSERT_EQ_INT(ngx_media_hls_init(&hls, &conf, NULL), NGX_OK);

    TEST_CASE("a segment starts only at a video keyframe");
    /* 10 frames, 100 ms apart, no keyframe at the start */
    TEST_ASSERT_EQ_INT(make_burst(&f, &burst, 10, 900000, 9000, 0), NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_hls_add_burst(&hls, &burst), NGX_OK);
    ngx_media_ts_mux_burst_destroy(&burst);

    TEST_ASSERT_EQ_U64(hls.dropped_frames, 10);
    TEST_ASSERT_EQ_U64(ngx_media_hls_segments(&hls), 0);

    TEST_CASE("a cut happens at the next keyframe after min_duration");
    /* keyframe at index 0, every 10 frames (1 s), 25 frames = 2.4 s */
    TEST_ASSERT_EQ_INT(make_burst(&f, &burst, 25, 900000, 3600, 10), NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_hls_add_burst(&hls, &burst), NGX_OK);
    ngx_media_ts_mux_burst_destroy(&burst);

    TEST_ASSERT_EQ_U64(ngx_media_hls_segments(&hls), 2);   /* at 0 and 10 */
    TEST_ASSERT_EQ_U64(hls.segments_written, 2);
    TEST_ASSERT(hls.npieces > 0);                          /* third in flight */

    TEST_ASSERT_EQ_INT(ngx_media_hls_finish(&hls), NGX_OK);
    TEST_ASSERT_EQ_U64(ngx_media_hls_segments(&hls), 3);

    TEST_CASE("the playlist describes the segments");
    {
        FILE   *fp;
        char    line[512];
        ngx_uint_t extinf = 0, discontinuities = 0, sequences = 0;

        snprintf(path, sizeof(path), "%s/index.m3u8", HLS_DIR);

        TEST_ASSERT(file_size(path) > 0);

        fp = fopen(path, "r");
        TEST_ASSERT_NOT_NULL(fp);

        /*
         * TEST_ASSERT_NOT_NULL records a failure and continues, so without
         * this guard a missing playlist becomes a NULL dereference inside
         * fgets - the process dies and reports nothing, instead of failing
         * the check above and saying why.
         */
        while (fp != NULL && fgets(line, sizeof(line), fp) != NULL) {
            if (strncmp(line, "#EXTINF:", 8) == 0) {
                extinf++;
            }

            if (strncmp(line, "#EXT-X-DISCONTINUITY", 21) == 0) {
                discontinuities++;
            }

            if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
                sequences++;
            }

            if (strncmp(line, "#EXT-X-TARGETDURATION:", 22) == 0) {
                /* the ceiling of the 2 s maximum segment duration */
                TEST_ASSERT(strncmp(line, "#EXT-X-TARGETDURATION:2", 23) == 0);
            }
        }

        if (fp != NULL) {
            (void) fclose(fp);
        }

        TEST_ASSERT_EQ_U64(extinf, 3);
        TEST_ASSERT_EQ_U64(discontinuities, 0);
        TEST_ASSERT_EQ_U64(sequences, 1);
    }

    TEST_CASE("segment files are valid MPEG-TS and carry the frames");
    {
        snprintf(path, sizeof(path), "%s/seg-000000.ts", HLS_DIR);

        TEST_ASSERT(file_size(path) > 0);
        TEST_ASSERT_EQ_INT(read_back(path, &back, &stats), NGX_OK);

        TEST_ASSERT_EQ_U64(stats.sync_errors, 0);
        TEST_ASSERT_EQ_U64(stats.continuity_errors, 0);
        TEST_ASSERT_EQ_U64(stats.pes_errors, 0);
        TEST_ASSERT_EQ_U64(back.frames, 10);       /* frames 0..9 */
        TEST_ASSERT_EQ_U64(back.keyframes, 1);
        TEST_ASSERT_EQ_I64(back.first_dts, 900000);
        TEST_ASSERT_EQ_I64(back.last_dts, 900000 + 9 * 3600);
    }

    TEST_CASE("retention evicts segments and removes their files");
    segments = hls.segments_written;

    /* push three more 2.4 s bursts: more segments than the window */
    TEST_ASSERT_EQ_INT(make_burst(&f, &burst, 25, 950000, 3600, 10), NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_hls_add_burst(&hls, &burst), NGX_OK);
    ngx_media_ts_mux_burst_destroy(&burst);
    TEST_ASSERT_EQ_INT(ngx_media_hls_finish(&hls), NGX_OK);

    TEST_ASSERT(hls.segments_written > segments);
    TEST_ASSERT(ngx_media_hls_segments(&hls) <= conf.max_segments);
    TEST_ASSERT(hls.segments_evicted > 0);

    /* the evicted file is gone from disk */
    snprintf(path, sizeof(path), "%s/seg-000000.ts", HLS_DIR);
    TEST_ASSERT_EQ_U64(file_size(path), 0);

    TEST_CASE("a discontinuity closes the segment and is announced");
    TEST_ASSERT_EQ_INT(ngx_media_hls_discontinuity(&hls), NGX_OK);
    TEST_ASSERT_EQ_INT(make_burst(&f, &burst, 15, 1000000, 3600, 5), NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_hls_add_burst(&hls, &burst), NGX_OK);
    ngx_media_ts_mux_burst_destroy(&burst);
    TEST_ASSERT_EQ_INT(ngx_media_hls_finish(&hls), NGX_OK);

    {
        FILE  *fp;
        char   line[512];
        ngx_uint_t discontinuities = 0;
        char   tag[] = "#EXT-X-DISCONTINUITY\n";

        snprintf(path, sizeof(path), "%s/index.m3u8", HLS_DIR);

        fp = fopen(path, "r");
        TEST_ASSERT_NOT_NULL(fp);

        /*
         * TEST_ASSERT_NOT_NULL records a failure and continues, so without
         * this guard a missing playlist becomes a NULL dereference inside
         * fgets - the process dies and reports nothing, instead of failing
         * the check above and saying why.
         */
        while (fp != NULL && fgets(line, sizeof(line), fp) != NULL) {
            if (strcmp(line, tag) == 0) {
                discontinuities++;
            }
        }

        fclose(fp);

        TEST_ASSERT(discontinuities >= 1);
    }

    TEST_CASE("the byte ceiling forces a cut");
    {
        ngx_media_hls_t  small;
        uint64_t         forced;

        conf.path.data = (u_char *) HLS_DIR "-small";
        conf.path.len = sizeof(HLS_DIR "-small") - 1;
        conf.max_segment_bytes = 2000;    /* far below a 1 s segment */
        conf.max_segments = 4;
        TEST_ASSERT_EQ_INT(ngx_media_hls_init(&small, &conf, NULL), NGX_OK);

        TEST_ASSERT_EQ_INT(make_burst(&f, &burst, 25, 900000, 3600, 10), NGX_OK);
        TEST_ASSERT_EQ_INT(ngx_media_hls_add_burst(&small, &burst), NGX_OK);
        ngx_media_ts_mux_burst_destroy(&burst);

        forced = small.forced_cuts;
        TEST_ASSERT(forced > 0);
        TEST_ASSERT(small.bytes_written > 0);

        ngx_media_hls_destroy(&small);
    }

    TEST_CASE("NULL tolerance");
    TEST_ASSERT_EQ_INT(ngx_media_hls_init(NULL, NULL, NULL), NGX_ERROR);
    ngx_media_hls_destroy(NULL);
    TEST_ASSERT_EQ_U64(ngx_media_hls_segments(NULL), 0);
    TEST_ASSERT_EQ_U64(ngx_media_hls_pending_duration(NULL), 0);
    ngx_media_ts_mux_burst_destroy(NULL);
    ngx_media_ts_mux_destroy(&f.mux);
    ngx_media_trackset_destroy(&f.tracks);

    ngx_media_hls_destroy(&hls);

    TEST_LEAKS();

    TEST_MAIN_END();
}

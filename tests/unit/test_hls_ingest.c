/*
 * The HLS push source's reader thread: a threaded reader fills a bounded
 * event queue and the worker drains it (acceptance case 7).
 *
 * The queue, the mutex around it and the stopping/exited atomics are the only
 * place in the reader where two threads meet, so this suite is built for
 * ThreadSanitizer (build/tsan): it runs two readers over real MPEG-TS
 * segments while draining them the way the runtime tick does, and asserts
 * that what a reader read reaches the program feed.
 *
 * What this would catch: a queue index or slot touched without the mutex, a
 * handoff that drops the buffer reference it took (leak or use-after-free), a
 * stop the reader cannot observe, and a close that joins a thread which is
 * still inside the reader.
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_hls_ingest.h"
#include "ngx_media_stream.h"
#include "ngx_media_ts_mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define ROOT_DIR    ".build/hls-ingest"
#define DIR_A       ROOT_DIR "/a"
#define DIR_B       ROOT_DIR "/b"
#define SEGMENT_MS  3600
#define SEG_FRAMES  25
#define DRAIN_MS    8000

#define S(lit) (&(ngx_str_t) { .len = sizeof(lit) - 1, \
                               .data = (u_char *) (lit) })

/*
 * The drain hands every frame it takes off the queue to the host runtime's
 * ISO recorder.  Outputs live in ngx_media_runtime.c, which owns the NGINX
 * event loop and cannot be linked into a unit build, so that one call is
 * answered here - the same reason ngx_shim.c answers ngx_log_error.  Nothing
 * in the suite asserts through it: what a recorder would do with a frame is a
 * copy of the frame the stream already has.
 */
void
ngx_media_runtime_iso_source(ngx_media_stream_t *stream,
    ngx_media_source_t *source, const ngx_media_frame_t *frame)
{
    (void) stream;
    (void) source;
    (void) frame;
}

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

static ngx_str_t
path_str(const char *s)
{
    ngx_str_t  v;

    v.data = (u_char *) s;
    v.len = strlen(s);

    return v;
}

typedef struct {
    ngx_media_ts_mux_t    mux;
    ngx_media_trackset_t  tracks;
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
video(int64_t dts, unsigned keyframe)
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

    payload = ngx_media_buf_alloc(400);
    TEST_ASSERT_NOT_NULL(payload);

    p = ngx_media_buf_data(payload);
    p[0] = 0x00;
    p[1] = 0x00;
    p[2] = 0x00;
    p[3] = 0x01;
    p[4] = keyframe ? 0x65 : 0x41;
    memset(p + 5, 0x5A, 400 - 5);

    (void) ngx_media_buf_freeze(payload, 400);
    frame.payload = payload;

    return frame;
}

/*
 * Writes one real segment: an MPEG-TS burst of `frames` access units with a
 * keyframe every tenth, exactly what a segmenter would have produced.
 */
static void
write_segment(fixture_t *f, const char *path, int64_t first_dts)
{
    ngx_media_ts_burst_t  burst;
    ngx_media_frame_t     frame;
    FILE                 *fp;
    ngx_uint_t            i;

    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_burst_init(&f->mux, &burst, 256 * 1024),
                       NGX_OK);

    for (i = 0; i < SEG_FRAMES; i++) {
        frame = video(first_dts + (int64_t) i * SEGMENT_MS, (i % 10) == 0);
        TEST_ASSERT_EQ_INT(ngx_media_ts_mux_write_frame(&f->mux, &burst,
                                                        &frame, 0), NGX_OK);
        ngx_media_frame_release(&frame);
    }

    TEST_ASSERT_EQ_INT(ngx_media_ts_mux_burst_end(&f->mux, &burst), NGX_OK);

    fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);

    if (fp != NULL) {
        TEST_ASSERT_EQ_U64(fwrite(ngx_media_buf_data(burst.backing), 1,
                                  ngx_media_ts_burst_size(&burst), fp),
                           ngx_media_ts_burst_size(&burst));
        fclose(fp);
    }

    ngx_media_ts_mux_burst_destroy(&burst);
}

/* one pass of the worker's tick, then a millisecond off the CPU */
static void
drain_once(void)
{
    struct timespec  ts = { 0, 1000000 };

    ngx_media_hls_ingest_drain_all();
    (void) nanosleep(&ts, NULL);
}

static ngx_uint_t
drain_until_units(ngx_media_feed_t *feed, uint64_t want)
{
    unsigned  i;

    for (i = 0; i < DRAIN_MS; i++) {
        ngx_media_hls_ingest_drain_all();

        if (ngx_media_feed_units(feed) >= want) {
            return 1;
        }

        (void) nanosleep(&(struct timespec) { 0, 1000000 }, NULL);
    }

    return 0;
}

/* an empty file: ordering reads names, never contents */
static void
touch(const char *dir, const char *name)
{
    char   path[512];
    FILE  *f;

    (void) snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f != NULL) {
        (void) fputc('G', f);
        fclose(f);
    }
}

static void
write_text(const char *dir, const char *name, const char *text)
{
    char   path[512];
    FILE  *f;

    (void) snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f != NULL) {
        (void) fputs(text, f);
        fclose(f);
    }
}

static void
empty_dir(const char *dir)
{
    char   cmd[600];

    (void) snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    TEST_ASSERT(system(cmd) == 0);
}

/*
 * The order segments are read in, which is the order the demuxer assembles
 * access units across them.  Encoders number segments without padding and
 * send a media playlist after each one (RFC 8216; DASH-IF Live Media Ingest
 * Interface-2); both have to produce presentation order.
 */
static void
test_ingest_order(void)
{
    char        names[16][256];
    void       *state = NULL;
    ngx_uint_t  n;

    TEST_CASE("segment names compare by their trailing number");
    TEST_ASSERT(ngx_media_hls_ingest_name_cmp("index9.ts", "index10.ts") < 0);
    TEST_ASSERT(ngx_media_hls_ingest_name_cmp("index10.ts", "index9.ts") > 0);
    TEST_ASSERT(ngx_media_hls_ingest_name_cmp("seg-00001.ts", "seg-00002.ts") < 0);
    TEST_ASSERT(ngx_media_hls_ingest_name_cmp("seg-01.ts", "seg-1.ts") != 0);
    TEST_ASSERT(ngx_media_hls_ingest_name_cmp("a1.ts", "b0.ts") < 0);
    TEST_ASSERT(ngx_media_hls_ingest_name_cmp("x.ts", "x.ts") == 0);

    TEST_CASE("without a playlist, unpadded names are read in number order");
    empty_dir(ROOT_DIR "/order");
    touch(ROOT_DIR "/order", "index8.ts");
    touch(ROOT_DIR "/order", "index9.ts");
    touch(ROOT_DIR "/order", "index10.ts");
    touch(ROOT_DIR "/order", "index11.ts");
    n = ngx_media_hls_ingest_order(&state, ROOT_DIR "/order", names, 16);
    TEST_ASSERT_EQ_INT(n, 4);
    TEST_ASSERT(strcmp(names[0], "index8.ts") == 0);
    TEST_ASSERT(strcmp(names[1], "index9.ts") == 0);
    TEST_ASSERT(strcmp(names[2], "index10.ts") == 0);
    TEST_ASSERT(strcmp(names[3], "index11.ts") == 0);
    touch(ROOT_DIR "/order", "index12.ts");
    TEST_ASSERT_EQ_INT(ngx_media_hls_ingest_order(&state, ROOT_DIR "/order",
                                                  names, 16), 1);
    TEST_ASSERT(strcmp(names[0], "index12.ts") == 0);
    ngx_media_hls_ingest_order_free(state);
    state = NULL;

    TEST_CASE("a media playlist decides the order, by media sequence");
    empty_dir(ROOT_DIR "/playlist");
    touch(ROOT_DIR "/playlist", "zulu.ts");
    touch(ROOT_DIR "/playlist", "alpha.ts");
    touch(ROOT_DIR "/playlist", "mike.ts");
    write_text(ROOT_DIR "/playlist", "live.m3u8",
               "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
               "#EXT-X-MEDIA-SEQUENCE:40\n"
               "#EXTINF:2.0,\nzulu.ts\n#EXTINF:2.0,\nalpha.ts\n");
    n = ngx_media_hls_ingest_order(&state, ROOT_DIR "/playlist", names, 16);
    TEST_ASSERT_EQ_INT(n, 2);
    TEST_ASSERT(strcmp(names[0], "zulu.ts") == 0);
    TEST_ASSERT(strcmp(names[1], "alpha.ts") == 0);

    /* the window slides: the next playlist drops the oldest and adds one */
    write_text(ROOT_DIR "/playlist", "live.m3u8",
               "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
               "#EXT-X-MEDIA-SEQUENCE:41\n"
               "#EXTINF:2.0,\nalpha.ts\n#EXTINF:2.0,\nmike.ts\n");
    n = ngx_media_hls_ingest_order(&state, ROOT_DIR "/playlist", names, 16);
    TEST_ASSERT_EQ_INT(n, 1);
    TEST_ASSERT(strcmp(names[0], "mike.ts") == 0);

    /* a listed segment that never arrived is passed over, not waited for */
    touch(ROOT_DIR "/playlist", "papa.ts");
    write_text(ROOT_DIR "/playlist", "live.m3u8",
               "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:42\n"
               "#EXTINF:2.0,\nmike.ts\n#EXTINF:2.0,\nlost.ts\n"
               "#EXT-X-DISCONTINUITY\n#EXTINF:2.0,\npapa.ts\n");
    n = ngx_media_hls_ingest_order(&state, ROOT_DIR "/playlist", names, 16);
    TEST_ASSERT_EQ_INT(n, 1);
    TEST_ASSERT(strcmp(names[0], "papa.ts") == 0);
    ngx_media_hls_ingest_order_free(state);
    state = NULL;

    TEST_CASE("a master playlist is not a segment list");
    empty_dir(ROOT_DIR "/master");
    touch(ROOT_DIR "/master", "seg2.ts");
    touch(ROOT_DIR "/master", "seg10.ts");
    write_text(ROOT_DIR "/master", "master.m3u8",
               "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow/index.m3u8\n");
    n = ngx_media_hls_ingest_order(&state, ROOT_DIR "/master", names, 16);
    TEST_ASSERT_EQ_INT(n, 2);
    TEST_ASSERT(strcmp(names[0], "seg2.ts") == 0);
    TEST_ASSERT(strcmp(names[1], "seg10.ts") == 0);
    ngx_media_hls_ingest_order_free(state);
}

int
main(void)
{
    static const char      *dirs[2] = { DIR_A, DIR_B };

    ngx_pool_t                         *pool;
    ngx_media_stream_t                  stream[2];
    ngx_media_hls_ingest_source_t      *reader[2];
    ngx_str_t                           dir_str[2];
    ngx_media_feed_conf_t               feed_conf;
    ngx_media_source_t                 *source;
    fixture_t                           f;
    ngx_uint_t                          i, cycle;
    uint64_t                            units;

    (void) remove(DIR_A "/seg-00001.ts");
    (void) remove(DIR_A "/seg-00002.ts");
    (void) remove(DIR_B "/seg-00001.ts");

    (void) mkdir(ROOT_DIR, 0755);
    ensure_dir(DIR_A);
    ensure_dir(DIR_B);

    for (i = 0; i < 2; i++) {
        dir_str[i] = path_str(dirs[i]);
    }

    pool = ngx_create_pool(64 * 1024, NULL);
    TEST_ASSERT_NOT_NULL(pool);

    feed_conf.max_units = 4096;
    feed_conf.max_bytes = 0;
    feed_conf.max_age = 0;

    fixture_init(&f);
    write_segment(&f, DIR_A "/seg-00001.ts", 900000);
    write_segment(&f, DIR_B "/seg-00001.ts", 900000);

    TEST_CASE("two readers run over real segments");
    for (i = 0; i < 2; i++) {
        TEST_ASSERT_EQ_INT(ngx_media_stream_init(&stream[i], pool, NULL,
                                                 S("live"), S("news"),
                                                 &feed_conf), NGX_OK);

        reader[i] = ngx_media_hls_ingest_open(&stream[i], S("uploader"),
                                              &dir_str[i], NULL);
        TEST_ASSERT_NOT_NULL(reader[i]);
        TEST_ASSERT_EQ_U64(ngx_media_hls_ingest_stream_readers(&stream[i]), 1);

        /* what the tick does once the source is the one to carry */
        source = ngx_media_stream_source_find(&stream[i], S("uploader"));
        TEST_ASSERT_NOT_NULL(source);
        TEST_ASSERT_EQ_INT(ngx_media_stream_promote(&stream[i], source),
                           NGX_OK);
    }

    TEST_CASE("what the readers read reaches the program feed");
    for (i = 0; i < 2; i++) {
        TEST_ASSERT(drain_until_units(&stream[i].program_feed, 1));
    }

    TEST_CASE("a segment that appears while a reader polls is picked up");
    units = ngx_media_feed_units(&stream[0].program_feed);

    write_segment(&f, DIR_A "/seg-00002.ts", 900000 + SEG_FRAMES * SEGMENT_MS);

    TEST_ASSERT(drain_until_units(&stream[0].program_feed, units + 1));

    TEST_CASE("close stops the reader and unlinks it");
    for (i = 0; i < 2; i++) {
        ngx_media_hls_ingest_close(reader[i]);
        reader[i] = NULL;

        TEST_ASSERT_EQ_U64(ngx_media_hls_ingest_stream_readers(&stream[i]), 0);

        /* a repeated close finds nothing to stop or free */
        ngx_media_hls_ingest_close(NULL);
    }
    TEST_CASE("source removal wakes a live reader");
    reader[0] = ngx_media_hls_ingest_open(&stream[0], S("uploader"),
                                          &dir_str[0], NULL);
    TEST_ASSERT_NOT_NULL(reader[0]);
    source = ngx_media_stream_source_find(&stream[0], S("uploader"));
    TEST_ASSERT_NOT_NULL(source);
    ngx_media_stream_source_remove(&stream[0], source);

    for (i = 0; i < DRAIN_MS; i++) {
        ngx_media_hls_ingest_reap(NULL);
        if (ngx_media_hls_ingest_stream_readers(&stream[0]) == 0) {
            break;
        }
        (void) nanosleep(&(struct timespec) { 0, 1000000 }, NULL);
    }

    TEST_ASSERT_EQ_U64(ngx_media_hls_ingest_stream_readers(&stream[0]), 0);
    reader[0] = NULL;


    ngx_media_hls_ingest_drain_all();

    TEST_CASE("a stop and start cycle leaves nothing behind");
    for (cycle = 0; cycle < 3; cycle++) {
        reader[0] = ngx_media_hls_ingest_open(&stream[0], S("uploader"),
                                              &dir_str[0], NULL);
        TEST_ASSERT_NOT_NULL(reader[0]);
        TEST_ASSERT_EQ_U64(ngx_media_hls_ingest_stream_readers(&stream[0]), 1);

        drain_once();

        ngx_media_hls_ingest_close(reader[0]);
        reader[0] = NULL;

        TEST_ASSERT_EQ_U64(ngx_media_hls_ingest_stream_readers(&stream[0]), 0);
    }

    TEST_CASE("shutdown stops every live reader, and close still unlinks it");
    for (i = 0; i < 2; i++) {
        reader[i] = ngx_media_hls_ingest_open(&stream[i], S("uploader"),
                                              &dir_str[i], NULL);
        TEST_ASSERT_NOT_NULL(reader[i]);
    }

    drain_once();
    ngx_media_hls_ingest_stop_all();
    ngx_media_hls_ingest_drain_all();

    for (i = 0; i < 2; i++) {
        ngx_media_hls_ingest_close(reader[i]);
        TEST_ASSERT_EQ_U64(ngx_media_hls_ingest_stream_readers(&stream[i]), 0);
    }

    for (i = 0; i < 2; i++) {
        ngx_media_stream_destroy(&stream[i]);
    }

    ngx_media_ts_mux_destroy(&f.mux);
    ngx_media_trackset_destroy(&f.tracks);
    ngx_destroy_pool(pool);

    test_ingest_order();

    TEST_LEAKS();

    TEST_MAIN_END();
}

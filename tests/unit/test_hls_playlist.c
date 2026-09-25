/*
 * A destination's view of the stream's media playlist: the window trimmed
 * from the front with the sequence numbers kept, and a segment the
 * destination does not have marked EXT-X-GAP in place.
 *
 * What this would catch: a trim that renumbers the segments it keeps (a
 * player or an ingest would see every segment as new, or none), a removed
 * discontinuity that is not counted, a gap that removes a segment instead of
 * marking it, and a master playlist mistaken for a media playlist.
 */

#include "ngx_media_test.h"

#include "ngx_media_hls_playlist.h"

#include <stdio.h>
#include <string.h>

#define S(lit) ((ngx_str_t) { .len = sizeof(lit) - 1, .data = (u_char *) (lit) })

static const char  seven[] =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "#EXT-X-TARGETDURATION:4\n"
    "#EXT-X-MEDIA-SEQUENCE:10\n"
    "#EXT-X-DISCONTINUITY-SEQUENCE:0\n"
    "#EXTINF:2.000,\nseg-10.ts\n"
    "#EXT-X-DISCONTINUITY\n"
    "#EXTINF:2.000,\nseg-11.ts\n"
    "#EXTINF:2.000,\nseg-12.ts\n"
    "#EXTINF:2.000,\nseg-13.ts\n"
    "#EXTINF:2.000,\nseg-14.ts\n"
    "#EXTINF:2.000,\nseg-15.ts\n"
    "#EXTINF:2.000,\nseg-16.ts\n";

static ngx_int_t
rewrite(const char *in, ngx_media_hls_playlist_opts_t *opts, char *out,
    size_t cap, ngx_media_hls_playlist_info_t *info)
{
    size_t     len = 0;
    ngx_int_t  rc;

    rc = ngx_media_hls_playlist_rewrite((const u_char *) in, strlen(in), opts,
                                        (u_char *) out, cap - 1, &len, info);
    out[len] = '\0';
    return rc;
}

static void
test_whole(void)
{
    ngx_media_hls_playlist_opts_t  opts = { 0, NULL, NULL, 0 };
    static ngx_media_hls_playlist_info_t  info;
    char                           out[4096];

    TEST_CASE("a window at least as large as the playlist copies it");
    TEST_ASSERT_EQ_INT(rewrite(seven, &opts, out, sizeof(out), &info), NGX_OK);
    TEST_ASSERT(strcmp(out, seven) == 0);
    TEST_ASSERT_EQ_INT(info.segments, 7);
    TEST_ASSERT_EQ_U64(info.media_sequence, 10);

    opts.window = 7;
    TEST_ASSERT_EQ_INT(rewrite(seven, &opts, out, sizeof(out), &info), NGX_OK);
    TEST_ASSERT(strcmp(out, seven) == 0);
}

static void
test_window(void)
{
    ngx_media_hls_playlist_opts_t  opts = { 5, NULL, NULL, 0 };
    static ngx_media_hls_playlist_info_t  info;
    char                           out[4096];

    TEST_CASE("a window of five keeps the newest five and their numbers");
    TEST_ASSERT_EQ_INT(rewrite(seven, &opts, out, sizeof(out), &info), NGX_OK);
    TEST_ASSERT_EQ_INT(info.segments, 5);
    TEST_ASSERT_EQ_INT(info.removed, 2);
    TEST_ASSERT_EQ_U64(info.media_sequence, 12);
    TEST_ASSERT(strstr(out, "#EXT-X-MEDIA-SEQUENCE:12\n") != NULL);
    TEST_ASSERT(strstr(out, "#EXT-X-MEDIA-SEQUENCE:10") == NULL);
    TEST_ASSERT(strstr(out, "seg-10.ts") == NULL);
    TEST_ASSERT(strstr(out, "seg-11.ts") == NULL);
    TEST_ASSERT(strstr(out, "seg-12.ts") != NULL);
    TEST_ASSERT(strstr(out, "seg-16.ts") != NULL);
    TEST_ASSERT(info.uri[0].len == 9
                && memcmp(info.uri[0].data, "seg-12.ts", 9) == 0);
    TEST_ASSERT(info.uri[4].len == 9
                && memcmp(info.uri[4].data, "seg-16.ts", 9) == 0);

    TEST_CASE("the removed discontinuity advances the discontinuity sequence");
    TEST_ASSERT(strstr(out, "#EXT-X-DISCONTINUITY-SEQUENCE:1\n") != NULL);
    TEST_ASSERT(strstr(out, "#EXT-X-DISCONTINUITY-SEQUENCE:0") == NULL);
    TEST_ASSERT(strstr(out, "#EXT-X-DISCONTINUITY\n") == NULL);
    TEST_ASSERT(strncmp(out, "#EXTM3U\n#EXT-X-VERSION:3\n"
                             "#EXT-X-TARGETDURATION:4\n", 49) == 0);

    TEST_CASE("a window that keeps the discontinuity keeps the sequence");
    opts.window = 6;
    TEST_ASSERT_EQ_INT(rewrite(seven, &opts, out, sizeof(out), &info), NGX_OK);
    TEST_ASSERT(strstr(out, "#EXT-X-MEDIA-SEQUENCE:11\n") != NULL);
    TEST_ASSERT(strstr(out, "#EXT-X-DISCONTINUITY-SEQUENCE:0\n") != NULL);
    TEST_ASSERT(strstr(out, "#EXT-X-DISCONTINUITY\n#EXTINF:2.000,\n"
                            "seg-11.ts\n") != NULL);
}

static void
test_gap(void)
{
    ngx_str_t                      missing[2] = { S("seg-13.ts"),
                                                  S("seg-99.ts") };
    ngx_media_hls_playlist_opts_t  opts = { 5, NULL, missing, 2 };
    static ngx_media_hls_playlist_info_t  info;
    char                           out[4096];

    TEST_CASE("a segment the destination lacks is marked, not removed");
    TEST_ASSERT_EQ_INT(rewrite(seven, &opts, out, sizeof(out), &info), NGX_OK);
    TEST_ASSERT_EQ_INT(info.segments, 5);
    TEST_ASSERT_EQ_INT(info.gaps, 1);
    TEST_ASSERT(strstr(out, "#EXTINF:2.000,\n#EXT-X-GAP\nseg-13.ts\n")
                != NULL);
    TEST_ASSERT(strstr(out, "#EXT-X-GAP\nseg-14.ts") == NULL);
    TEST_ASSERT(strstr(out, "#EXT-X-MEDIA-SEQUENCE:12\n") != NULL);
}

static void
test_first(void)
{
    ngx_str_t                      first = S("seg-15.ts");
    ngx_str_t                      gone = S("seg-02.ts");
    ngx_media_hls_playlist_opts_t  opts = { 5, &first, NULL, 0 };
    static ngx_media_hls_playlist_info_t  info;
    char                           out[4096];

    TEST_CASE("a destination that joined later lists only what it had");
    TEST_ASSERT_EQ_INT(rewrite(seven, &opts, out, sizeof(out), &info), NGX_OK);
    TEST_ASSERT_EQ_INT(info.segments, 2);
    TEST_ASSERT_EQ_U64(info.media_sequence, 15);
    TEST_ASSERT(strstr(out, "#EXT-X-MEDIA-SEQUENCE:15\n") != NULL);
    TEST_ASSERT(strstr(out, "seg-14.ts") == NULL);
    TEST_ASSERT(strstr(out, "seg-15.ts\n#EXTINF:2.000,\nseg-16.ts\n")
                != NULL);
    TEST_ASSERT(strstr(out, "#EXT-X-DISCONTINUITY-SEQUENCE:1\n") != NULL);

    TEST_CASE("its first segment gone from the playlist, the window rules");
    opts.first = &gone;
    TEST_ASSERT_EQ_INT(rewrite(seven, &opts, out, sizeof(out), &info), NGX_OK);
    TEST_ASSERT_EQ_INT(info.segments, 5);
    TEST_ASSERT_EQ_U64(info.media_sequence, 12);
}

static void
test_edges(void)
{
    ngx_media_hls_playlist_opts_t  opts = { 2, NULL, NULL, 0 };
    char                           out[4096], tiny[40];
    size_t                         len;

    static const char  master[] =
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow/index.m3u8\n";
    static const char  ended[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:2\n"
        "#EXTINF:2.0,\na.ts\n#EXTINF:2.0,\nb.ts\n#EXTINF:2.0,\nc.ts\n"
        "#EXT-X-ENDLIST\n";
    static const char  crlf[] =
        "#EXTM3U\r\n#EXT-X-MEDIA-SEQUENCE:3\r\n"
        "#EXTINF:2.0,\r\na.ts\r\n#EXTINF:2.0,\r\nb.ts\r\n"
        "#EXTINF:2.0,\r\nc.ts\r\n";

    TEST_CASE("a master playlist and a non-playlist are declined");
    TEST_ASSERT_EQ_INT(rewrite(master, &opts, out, sizeof(out), NULL),
                       NGX_DECLINED);
    TEST_ASSERT_EQ_INT(rewrite("<html>", &opts, out, sizeof(out), NULL),
                       NGX_DECLINED);

    TEST_CASE("no media sequence means zero; the end list is kept");
    TEST_ASSERT_EQ_INT(rewrite(ended, &opts, out, sizeof(out), NULL), NGX_OK);
    TEST_ASSERT(strcmp(out, "#EXTM3U\n#EXT-X-TARGETDURATION:2\n"
                            "#EXT-X-MEDIA-SEQUENCE:1\n"
                            "#EXTINF:2.0,\nb.ts\n#EXTINF:2.0,\nc.ts\n"
                            "#EXT-X-ENDLIST\n") == 0);

    TEST_CASE("CRLF line endings");
    TEST_ASSERT_EQ_INT(rewrite(crlf, &opts, out, sizeof(out), NULL), NGX_OK);
    TEST_ASSERT(strstr(out, "#EXT-X-MEDIA-SEQUENCE:4\n") != NULL);
    TEST_ASSERT(strstr(out, "a.ts") == NULL);
    TEST_ASSERT(strstr(out, "c.ts") != NULL);

    TEST_CASE("an output buffer too small is an error, never a truncation");
    TEST_ASSERT_EQ_INT(ngx_media_hls_playlist_rewrite(
                           (const u_char *) seven, strlen(seven), &opts,
                           (u_char *) tiny, sizeof(tiny), &len, NULL),
                       NGX_ERROR);
    TEST_ASSERT_EQ_U64(len, 0);
}

int
main(void)
{
    printf("test_hls_playlist\n");

    test_whole();
    test_window();
    test_gap();
    test_first();
    test_edges();

    TEST_MAIN_END();
}

#include "ngx_media_test.h"
#include "ngx_media_compat.h"

#define NO_TRACK 0

static void
build(ngx_media_trackset_t *set, ngx_uint_t video_codec, ngx_uint_t width,
    ngx_uint_t audio_codec, ngx_uint_t audio_rate, ngx_uint_t audio_channels,
    ngx_uint_t payload_format)
{
    ngx_media_track_t  track;

    TEST_ASSERT_EQ_INT(ngx_media_trackset_init(set, 4, NULL), NGX_OK);

    if (video_codec != NO_TRACK) {
        ngx_memzero(&track, sizeof(track));

        track.media_type = NGX_MEDIA_TYPE_VIDEO;
        track.codec = video_codec;
        track.payload_format = payload_format;
        track.width = width;
        track.height = 1080;

        TEST_ASSERT(ngx_media_trackset_add(set, &track) >= 0);
    }

    if (audio_codec != NO_TRACK) {
        ngx_memzero(&track, sizeof(track));

        track.media_type = NGX_MEDIA_TYPE_AUDIO;
        track.codec = audio_codec;
        track.payload_format = payload_format;
        track.sample_rate = audio_rate;
        track.channels = audio_channels;
        track.profile = 2;

        TEST_ASSERT(ngx_media_trackset_add(set, &track) >= 0);
    }
}

int
main(void)
{
    ngx_media_trackset_t  program, candidate;

    TEST_CASE("identical contracts are READY");
    build(&program, NGX_MEDIA_CODEC_H264, 1920, NGX_MEDIA_CODEC_AAC, 48000, 2,
          NGX_MEDIA_PAYLOAD_ANNEXB);
    build(&candidate, NGX_MEDIA_CODEC_H264, 1920, NGX_MEDIA_CODEC_AAC, 48000,
          2, NGX_MEDIA_PAYLOAD_ANNEXB);

    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, &candidate),
                       NGX_MEDIA_COMPAT_READY);

    ngx_media_trackset_destroy(&candidate);

    TEST_CASE("a different codec is INCOMPATIBLE");
    build(&candidate, NGX_MEDIA_CODEC_H265, 1920, NGX_MEDIA_CODEC_AAC, 48000,
          2, NGX_MEDIA_PAYLOAD_ANNEXB);
    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, &candidate),
                       NGX_MEDIA_COMPAT_INCOMPATIBLE);
    ngx_media_trackset_destroy(&candidate);

    TEST_CASE("a missing program track is INCOMPATIBLE");
    build(&candidate, NGX_MEDIA_CODEC_H264, 1920, NO_TRACK, 0, 0,
          NGX_MEDIA_PAYLOAD_ANNEXB);
    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, &candidate),
                       NGX_MEDIA_COMPAT_INCOMPATIBLE);
    ngx_media_trackset_destroy(&candidate);

    TEST_CASE("resolution and frame shape changes are DEGRADED");
    build(&candidate, NGX_MEDIA_CODEC_H264, 1280, NGX_MEDIA_CODEC_AAC, 48000,
          2, NGX_MEDIA_PAYLOAD_ANNEXB);
    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, &candidate),
                       NGX_MEDIA_COMPAT_DEGRADED);
    ngx_media_trackset_destroy(&candidate);

    TEST_CASE("audio parameter changes are DEGRADED");
    build(&candidate, NGX_MEDIA_CODEC_H264, 1920, NGX_MEDIA_CODEC_AAC, 44100,
          2, NGX_MEDIA_PAYLOAD_ANNEXB);
    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, &candidate),
                       NGX_MEDIA_COMPAT_DEGRADED);
    ngx_media_trackset_destroy(&candidate);

    build(&candidate, NGX_MEDIA_CODEC_H264, 1920, NGX_MEDIA_CODEC_AAC, 48000,
          1, NGX_MEDIA_PAYLOAD_ANNEXB);
    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, &candidate),
                       NGX_MEDIA_COMPAT_DEGRADED);
    ngx_media_trackset_destroy(&candidate);

    TEST_CASE("a different payload representation is DEGRADED");
    build(&candidate, NGX_MEDIA_CODEC_H264, 1920, NGX_MEDIA_CODEC_AAC, 48000,
          2, NGX_MEDIA_PAYLOAD_AVCC);
    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, &candidate),
                       NGX_MEDIA_COMPAT_DEGRADED);
    ngx_media_trackset_destroy(&candidate);

    TEST_CASE("an extra candidate track is DEGRADED");
    build(&candidate, NGX_MEDIA_CODEC_H264, 1920, NGX_MEDIA_CODEC_AAC, 48000,
          2, NGX_MEDIA_PAYLOAD_ANNEXB);
    TEST_ASSERT(ngx_media_trackset_add(&candidate, &program.tracks[0]) >= 0);
    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, &candidate),
                       NGX_MEDIA_COMPAT_DEGRADED);
    ngx_media_trackset_destroy(&candidate);

    TEST_CASE("nothing to compare against is READY");
    build(&candidate, NGX_MEDIA_CODEC_H264, 1920, NGX_MEDIA_CODEC_AAC, 48000,
          2, NGX_MEDIA_PAYLOAD_ANNEXB);

    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(NULL, &candidate),
                       NGX_MEDIA_COMPAT_READY);
    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, NULL),
                       NGX_MEDIA_COMPAT_READY);

    program.count = 0;
    TEST_ASSERT_EQ_U64(ngx_media_compat_classify(&program, &candidate),
                       NGX_MEDIA_COMPAT_READY);
    program.count = 2;

    TEST_CASE("names");
    TEST_ASSERT(strcmp(ngx_media_compat_name(NGX_MEDIA_COMPAT_READY),
                       "ready") == 0);
    TEST_ASSERT(strcmp(ngx_media_compat_name(NGX_MEDIA_COMPAT_DEGRADED),
                       "degraded") == 0);
    TEST_ASSERT(strcmp(ngx_media_compat_name(NGX_MEDIA_COMPAT_INCOMPATIBLE),
                       "incompatible") == 0);
    TEST_ASSERT(strcmp(ngx_media_compat_name(99), "unknown") == 0);

    ngx_media_trackset_destroy(&program);
    ngx_media_trackset_destroy(&candidate);

    TEST_LEAKS();

    TEST_MAIN_END();
}

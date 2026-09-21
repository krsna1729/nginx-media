#include "ngx_media_test.h"

#include "ngx_media_aac.h"
#include "ngx_media_nal.h"

static int
nal_eq(const ngx_media_nal_t *nal, const char *expect)
{
    size_t len = strlen(expect);

    return (nal->len == len && ngx_memcmp(nal->data, expect, len) == 0) ? 1 : 0;
}

int
main(void)
{
    ngx_media_nal_iter_t  it;
    ngx_media_nal_t       nal;
    ngx_media_adts_t      adts;
    u_char                stream[64];
    u_char                asc[4];
    size_t                asc_len, n;

    TEST_CASE("NAL iteration handles 3- and 4-byte start codes");
    n = 0;
    stream[n++] = 0x00; stream[n++] = 0x00; stream[n++] = 0x00;
    stream[n++] = 0x01; stream[n++] = 0x09; stream[n++] = 0xF0;  /* AUD */
    stream[n++] = 0x00; stream[n++] = 0x00; stream[n++] = 0x01;
    stream[n++] = 0x67; stream[n++] = 0x42; stream[n++] = 0x00;  /* SPS */
    stream[n++] = 0x00; stream[n++] = 0x00; stream[n++] = 0x00;
    stream[n++] = 0x01; stream[n++] = 0x65; stream[n++] = 0x88;  /* IDR */

    ngx_media_nal_iter_init(&it, stream, n);

    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 1);
    TEST_ASSERT(nal_eq(&nal, "\x09\xf0"));
    TEST_ASSERT_EQ_U64(ngx_media_nal_type(NGX_MEDIA_CODEC_H264, nal.data,
                                          nal.len),
                       NGX_MEDIA_H264_NAL_AUD);

    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 1);
    TEST_ASSERT(nal_eq(&nal, "\x67\x42\x00"));
    TEST_ASSERT_EQ_U64(ngx_media_nal_type(NGX_MEDIA_CODEC_H264, nal.data,
                                          nal.len),
                       NGX_MEDIA_H264_NAL_SPS);
    TEST_ASSERT(ngx_media_nal_is_config(NGX_MEDIA_CODEC_H264,
                                        NGX_MEDIA_H264_NAL_SPS));
    TEST_ASSERT(!ngx_media_nal_is_vcl(NGX_MEDIA_CODEC_H264,
                                      NGX_MEDIA_H264_NAL_SPS));

    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 1);
    TEST_ASSERT(nal_eq(&nal, "\x65\x88"));
    TEST_ASSERT(ngx_media_nal_is_vcl(NGX_MEDIA_CODEC_H264,
                                     NGX_MEDIA_H264_NAL_IDR));
    TEST_ASSERT(ngx_media_nal_is_keyframe(NGX_MEDIA_CODEC_H264,
                                          NGX_MEDIA_H264_NAL_IDR));

    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 0);
    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 0);

    TEST_CASE("trailing zero bytes are not part of a NAL unit");
    n = 0;
    stream[n++] = 0x00; stream[n++] = 0x00; stream[n++] = 0x01;
    stream[n++] = 0x41; stream[n++] = 0x9A;
    stream[n++] = 0x00; stream[n++] = 0x00;   /* padding before next code */
    stream[n++] = 0x00; stream[n++] = 0x01;
    stream[n++] = 0x41; stream[n++] = 0x9B;

    ngx_media_nal_iter_init(&it, stream, n);
    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 1);
    TEST_ASSERT(nal_eq(&nal, "\x41\x9a"));
    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 1);
    TEST_ASSERT(nal_eq(&nal, "\x41\x9b"));
    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 0);

    TEST_CASE("H.265 two-byte NAL headers and IRAP classification");
    n = 0;
    stream[n++] = 0x00; stream[n++] = 0x00; stream[n++] = 0x00;
    stream[n++] = 0x01; stream[n++] = 0x40; stream[n++] = 0x01;  /* VPS */
    stream[n++] = 0x00; stream[n++] = 0x00; stream[n++] = 0x01;
    stream[n++] = 0x26; stream[n++] = 0x01;                      /* IDR_W_RADL */

    ngx_media_nal_iter_init(&it, stream, n);
    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 1);
    TEST_ASSERT_EQ_U64(ngx_media_nal_type(NGX_MEDIA_CODEC_H265, nal.data,
                                          nal.len),
                       NGX_MEDIA_H265_NAL_VPS);
    TEST_ASSERT(ngx_media_nal_is_config(NGX_MEDIA_CODEC_H265,
                                        NGX_MEDIA_H265_NAL_VPS));

    TEST_ASSERT_EQ_U64(ngx_media_nal_iter_next(&it, &nal), 1);
    TEST_ASSERT_EQ_U64(ngx_media_nal_type(NGX_MEDIA_CODEC_H265, nal.data,
                                          nal.len),
                       NGX_MEDIA_H265_NAL_IDR_W_RADL);
    TEST_ASSERT(ngx_media_nal_is_keyframe(NGX_MEDIA_CODEC_H265,
                                          NGX_MEDIA_H265_NAL_IDR_W_RADL));
    TEST_ASSERT(!ngx_media_nal_is_keyframe(NGX_MEDIA_CODEC_H265,
                                           NGX_MEDIA_H265_NAL_VPS));

    TEST_CASE("ADTS header parsing");
    n = 0;
    stream[n++] = 0xFF; stream[n++] = 0xF1;                  /* sync, no CRC */
    stream[n++] = (1 << 6) | (3 << 2) | 0;                    /* LC, 48k, ch hi */
    stream[n++] = (2 << 6) | (((7 + 8) >> 11) & 0x03);
    stream[n++] = ((7 + 8) >> 3) & 0xFF;
    stream[n++] = (((7 + 8) & 0x07) << 5) | 0x1F;
    stream[n++] = 0xFC;
    memset(stream + n, 0xAA, 8);
    n += 8;

    TEST_ASSERT_EQ_INT(ngx_media_adts_parse(stream, n, &adts), NGX_OK);
    TEST_ASSERT_EQ_U64(adts.frame_len, 15);
    TEST_ASSERT_EQ_U64(adts.header_len, 7);
    TEST_ASSERT_EQ_U64(adts.sample_rate, 48000);
    TEST_ASSERT_EQ_U64(adts.channels, 2);
    TEST_ASSERT_EQ_U64(adts.object_type, 2);

    TEST_CASE("ADTS rejects malformed headers");
    stream[0] = 0xFE;
    TEST_ASSERT_EQ_INT(ngx_media_adts_parse(stream, n, &adts), NGX_ERROR);
    stream[0] = 0xFF;
    TEST_ASSERT_EQ_INT(ngx_media_adts_parse(stream, 4, &adts), NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_adts_parse(NULL, n, &adts), NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_adts_parse(stream, n, NULL), NGX_ERROR);

    TEST_CASE("CRC-protected ADTS header is 9 bytes");
    stream[1] = 0xF0;                                        /* protection */
    TEST_ASSERT_EQ_INT(ngx_media_adts_parse(stream, n, &adts), NGX_OK);
    TEST_ASSERT_EQ_U64(adts.header_len, 9);

    TEST_CASE("AudioSpecificConfig synthesis");
    adts.object_type = 2;
    adts.sample_rate = 48000;
    adts.channels = 2;
    TEST_ASSERT_EQ_INT(ngx_media_adts_audio_specific_config(&adts, asc,
                                                            sizeof(asc),
                                                            &asc_len), NGX_OK);
    TEST_ASSERT_EQ_U64(asc_len, 2);
    TEST_ASSERT_EQ_U64(asc[0], 0x11);   /* AAC-LC, 48 kHz */
    TEST_ASSERT_EQ_U64(asc[1], 0x90);   /* stereo */
    TEST_ASSERT_EQ_INT(ngx_media_adts_audio_specific_config(NULL, asc,
                                                            sizeof(asc),
                                                            &asc_len),
                       NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_adts_audio_specific_config(&adts, asc, 1,
                                                            &asc_len),
                       NGX_ERROR);

    TEST_LEAKS();

    TEST_MAIN_END();
}

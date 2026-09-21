#include "ngx_media_aac.h"

static const ngx_uint_t ngx_media_aac_sample_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000,
    11025, 8000, 7350
};

#define NGX_MEDIA_AAC_SAMPLE_RATES \
    (sizeof(ngx_media_aac_sample_rates) / sizeof(ngx_uint_t))

ngx_int_t
ngx_media_adts_parse(const u_char *p, size_t len, ngx_media_adts_t *adts)
{
    ngx_uint_t  index, protection_absent;

    if (p == NULL || adts == NULL || len < NGX_MEDIA_ADTS_HEADER_MIN) {
        return NGX_ERROR;
    }

    /* syncword: 12 bits set */
    if (p[0] != 0xFF || (p[1] & 0xF0) != 0xF0) {
        return NGX_ERROR;
    }

    protection_absent = p[1] & 0x01;

    adts->header_len = protection_absent ? 7 : 9;

    if (len < adts->header_len) {
        return NGX_ERROR;
    }

    adts->object_type = ((p[2] >> 6) & 0x03) + 1;

    index = (p[2] >> 2) & 0x0F;

    if (index >= NGX_MEDIA_AAC_SAMPLE_RATES) {
        return NGX_ERROR;
    }

    adts->sample_rate = ngx_media_aac_sample_rates[index];

    adts->channels = (ngx_uint_t) (((p[2] & 0x01) << 2)
                                   | ((p[3] >> 6) & 0x03));

    if (adts->channels == 0) {
        return NGX_ERROR;
    }

    adts->frame_len = (ngx_uint_t) (((p[3] & 0x03) << 11)
                                    | (p[4] << 3)
                                    | ((p[5] >> 5) & 0x07));

    if (adts->frame_len < adts->header_len) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_adts_audio_specific_config(const ngx_media_adts_t *adts, u_char *buf,
    size_t cap, size_t *out_len)
{
    ngx_uint_t  index, i;

    if (adts == NULL || buf == NULL || cap < 2 || adts->sample_rate == 0) {
        return NGX_ERROR;
    }

    index = NGX_MEDIA_AAC_SAMPLE_RATES;

    for (i = 0; i < NGX_MEDIA_AAC_SAMPLE_RATES; i++) {
        if (ngx_media_aac_sample_rates[i] == adts->sample_rate) {
            index = i;
            break;
        }
    }

    if (index == NGX_MEDIA_AAC_SAMPLE_RATES) {
        return NGX_ERROR;
    }

    /*
     * audioObjectType (5) | samplingFrequencyIndex (4) | channelConfiguration
     * (4) | frameLengthFlag (1) | dependsOnCoreCoder (1) | extensionFlag (1)
     */
    buf[0] = (u_char) (((adts->object_type & 0x1F) << 3)
                       | ((index >> 1) & 0x07));

    buf[1] = (u_char) (((index & 0x01) << 7)
                       | ((adts->channels & 0x0F) << 3));

    *out_len = 2;

    return NGX_OK;
}

ngx_int_t
ngx_media_adts_write(const ngx_media_adts_t *adts, u_char *buf, size_t cap,
    size_t *out_len)
{
    ngx_uint_t  index, i;

    if (adts == NULL || buf == NULL || cap < NGX_MEDIA_ADTS_HEADER_MIN
        || adts->frame_len > 0x1FFF || adts->object_type < 1
        || adts->object_type > 4 || adts->channels < 1
        || adts->channels > 7)
    {
        return NGX_ERROR;
    }

    index = NGX_MEDIA_AAC_SAMPLE_RATES;

    for (i = 0; i < NGX_MEDIA_AAC_SAMPLE_RATES; i++) {
        if (ngx_media_aac_sample_rates[i] == adts->sample_rate) {
            index = i;
            break;
        }
    }

    if (index == NGX_MEDIA_AAC_SAMPLE_RATES) {
        return NGX_ERROR;
    }

    /* syncword, MPEG-4, layer 0, no CRC */
    buf[0] = 0xFF;
    buf[1] = 0xF1;
    buf[2] = (u_char) (((adts->object_type - 1) << 6) | (index << 2)
                       | ((adts->channels >> 2) & 0x01));
    buf[3] = (u_char) (((adts->channels & 0x03) << 6)
                       | ((adts->frame_len >> 11) & 0x03));
    buf[4] = (u_char) ((adts->frame_len >> 3) & 0xFF);
    buf[5] = (u_char) (((adts->frame_len & 0x07) << 5) | 0x1F);
    buf[6] = 0xFC;   /* buffer fullness low bits, one raw data block */

    *out_len = NGX_MEDIA_ADTS_HEADER_MIN;

    return NGX_OK;
}

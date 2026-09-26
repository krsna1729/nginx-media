#include "ngx_media_rtmp_adapter.h"

#include "ngx_media_aac.h"
#include "ngx_media_nal.h"

/* --- parameter sets ------------------------------------------------------ */

ngx_int_t
ngx_media_rtmp_avcc_parse(const u_char *data, size_t len,
    ngx_media_rtmp_avcc_t *out)
{
    size_t      pos, i;
    ngx_uint_t  count;

    if (data == NULL || out == NULL || len < 7) {
        return NGX_ERROR;
    }

    ngx_memzero(out, sizeof(ngx_media_rtmp_avcc_t));

    out->version = data[0];
    out->profile = data[1];
    out->compatibility = data[2];
    out->level = data[3];
    out->nal_length_size = (data[4] & 0x03) + 1;

    if (out->version != 1 || out->nal_length_size > 4) {
        return NGX_ERROR;
    }

    pos = 6;   /* reserved bits then the SPS count */

    count = data[5] & 0x1F;

    for (i = 0; i < count; i++) {
        size_t  slen;

        if (pos + 2 > len) {
            return NGX_ERROR;
        }

        slen = ((size_t) data[pos] << 8) | data[pos + 1];
        pos += 2;

        if (slen == 0 || pos + slen > len) {
            return NGX_ERROR;
        }

        if (out->nsps < NGX_MEDIA_RTMP_MAX_PARAM_SETS) {
            out->sps[out->nsps].data = (u_char *) data + pos;
            out->sps[out->nsps].len = slen;
            out->nsps++;
        }

        pos += slen;
    }

    if (out->nsps == 0) {
        return NGX_ERROR;
    }

    if (pos >= len) {
        return NGX_ERROR;
    }

    count = data[pos++];

    for (i = 0; i < count; i++) {
        size_t  plen;

        if (pos + 2 > len) {
            return NGX_ERROR;
        }

        plen = ((size_t) data[pos] << 8) | data[pos + 1];
        pos += 2;

        if (plen == 0 || pos + plen > len) {
            return NGX_ERROR;
        }

        if (out->npps < NGX_MEDIA_RTMP_MAX_PARAM_SETS) {
            out->pps[out->npps].data = (u_char *) data + pos;
            out->pps[out->npps].len = plen;
            out->npps++;
        }

        pos += plen;
    }

    if (out->npps == 0) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_rtmp_avcc_build(const ngx_media_rtmp_avcc_t *avcc, u_char *dst,
    size_t capacity, size_t *out_len)
{
    size_t      pos = 0;
    ngx_uint_t  i;

    if (avcc == NULL || dst == NULL || avcc->nsps == 0 || avcc->npps == 0
        || avcc->nsps > NGX_MEDIA_RTMP_MAX_PARAM_SETS
        || avcc->npps > NGX_MEDIA_RTMP_MAX_PARAM_SETS)
    {
        return NGX_ERROR;
    }

    if (capacity < 7) {
        return NGX_ERROR;
    }

    dst[pos++] = (u_char) (avcc->version ? avcc->version : 1);
    dst[pos++] = (u_char) avcc->profile;
    dst[pos++] = (u_char) avcc->compatibility;
    dst[pos++] = (u_char) avcc->level;

    {
        ngx_uint_t length_size = avcc->nal_length_size ? avcc->nal_length_size
                                                       : 4;

        if (length_size < 1 || length_size > 4) {
            return NGX_ERROR;
        }

        dst[pos++] = (u_char) (0xFC | (length_size - 1));
    }

    dst[pos++] = (u_char) (0xE0 | avcc->nsps);

    for (i = 0; i < avcc->nsps; i++) {

        if (pos + 2 + avcc->sps[i].len > capacity) {
            return NGX_ERROR;
        }

        dst[pos++] = (u_char) ((avcc->sps[i].len >> 8) & 0xFF);
        dst[pos++] = (u_char) (avcc->sps[i].len & 0xFF);
        ngx_memcpy(dst + pos, avcc->sps[i].data, avcc->sps[i].len);
        pos += avcc->sps[i].len;
    }

    if (pos + 1 > capacity) {
        return NGX_ERROR;
    }

    dst[pos++] = (u_char) avcc->npps;

    for (i = 0; i < avcc->npps; i++) {

        if (pos + 2 + avcc->pps[i].len > capacity) {
            return NGX_ERROR;
        }

        dst[pos++] = (u_char) ((avcc->pps[i].len >> 8) & 0xFF);
        dst[pos++] = (u_char) (avcc->pps[i].len & 0xFF);
        ngx_memcpy(dst + pos, avcc->pps[i].data, avcc->pps[i].len);
        pos += avcc->pps[i].len;
    }

    *out_len = pos;

    return NGX_OK;
}

size_t
ngx_media_rtmp_avcc_annexb_size(const ngx_media_rtmp_avcc_t *avcc)
{
    size_t      total = 0;
    ngx_uint_t  i;

    if (avcc == NULL) {
        return 0;
    }

    for (i = 0; i < avcc->nsps; i++) {
        total += 4 + avcc->sps[i].len;
    }

    for (i = 0; i < avcc->npps; i++) {
        total += 4 + avcc->pps[i].len;
    }

    return total;
}

ngx_int_t
ngx_media_rtmp_avcc_to_annexb(const ngx_media_rtmp_avcc_t *avcc, u_char *dst,
    size_t capacity, size_t *out_len)
{
    size_t      pos = 0;
    ngx_uint_t  i;

    if (avcc == NULL) {
        return NGX_ERROR;
    }

    if (capacity < ngx_media_rtmp_avcc_annexb_size(avcc)) {
        return NGX_ERROR;
    }

    for (i = 0; i < avcc->nsps; i++) {
        dst[pos++] = 0; dst[pos++] = 0; dst[pos++] = 0; dst[pos++] = 1;
        ngx_memcpy(dst + pos, avcc->sps[i].data, avcc->sps[i].len);
        pos += avcc->sps[i].len;
    }

    for (i = 0; i < avcc->npps; i++) {
        dst[pos++] = 0; dst[pos++] = 0; dst[pos++] = 0; dst[pos++] = 1;
        ngx_memcpy(dst + pos, avcc->pps[i].data, avcc->pps[i].len);
        pos += avcc->pps[i].len;
    }

    *out_len = pos;

    return NGX_OK;
}

/* --- audio configuration ------------------------------------------------- */

static ngx_uint_t  ngx_media_rtmp_asc_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

ngx_int_t
ngx_media_rtmp_asc_parse(const u_char *data, size_t len,
    ngx_media_rtmp_asc_t *out)
{
    ngx_uint_t  index;

    if (data == NULL || out == NULL || len < 2) {
        return NGX_ERROR;
    }

    ngx_memzero(out, sizeof(ngx_media_rtmp_asc_t));

    out->object_type = (data[0] >> 3) & 0x1F;
    index = ((ngx_uint_t) (data[0] & 0x07) << 1) | (data[1] >> 7);
    out->channels = (data[1] >> 3) & 0x0F;

    if (out->object_type == 0 || index > 12) {
        return NGX_ERROR;
    }

    out->sample_rate_index = index;
    out->sample_rate = ngx_media_rtmp_asc_rates[index];

    if (out->sample_rate == 0) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_rtmp_asc_build(const ngx_media_rtmp_asc_t *asc, u_char *dst,
    size_t capacity, size_t *out_len)
{
    if (asc == NULL || dst == NULL || capacity < 2) {
        return NGX_ERROR;
    }

    if (asc->object_type == 0 || asc->object_type > 31
        || asc->sample_rate_index > 12 || asc->channels > 15)
    {
        return NGX_ERROR;
    }

    dst[0] = (u_char) ((asc->object_type << 3)
                       | ((asc->sample_rate_index >> 1) & 0x07));
    dst[1] = (u_char) (((asc->sample_rate_index & 0x01) << 7)
                       | (asc->channels << 3));

    *out_len = 2;

    return NGX_OK;
}

/* --- payload reframing --------------------------------------------------- */

ngx_int_t
ngx_media_rtmp_avcc_payload_to_annexb(const u_char *src, size_t len,
    ngx_uint_t nal_length_size, u_char *dst, size_t capacity, size_t *out_len)
{
    size_t  pos = 0, out = 0;

    if (src == NULL || dst == NULL || nal_length_size < 1
        || nal_length_size > 4)
    {
        return NGX_ERROR;
    }

    while (pos + nal_length_size <= len) {
        size_t      nlen = 0, i;
        ngx_uint_t  zero = 1;

        for (i = 0; i < nal_length_size; i++) {
            nlen = (nlen << 8) | src[pos + i];

            if (src[pos + i] != 0) {
                zero = 0;
            }
        }

        pos += nal_length_size;

        if (nlen == 0 || pos + nlen > len) {
            return NGX_ERROR;
        }

        /* tolerate padding (a zero length NAL is not valid, but a run of
         * zero bytes between units is) */
        if (zero) {
            continue;
        }

        if (out + 4 + nlen > capacity) {
            return NGX_ERROR;
        }

        dst[out++] = 0; dst[out++] = 0; dst[out++] = 0; dst[out++] = 1;
        ngx_memcpy(dst + out, src + pos, nlen);
        out += nlen;
        pos += nlen;
    }

    if (pos != len || out == 0) {
        return NGX_ERROR;
    }

    *out_len = out;

    return NGX_OK;
}

ngx_int_t
ngx_media_rtmp_annexb_payload_to_avcc(const u_char *src, size_t len,
    ngx_uint_t nal_length_size, u_char *dst, size_t capacity, size_t *out_len)
{
    ngx_media_nal_iter_t  it;
    ngx_media_nal_t       nal;
    size_t                out = 0;
    ngx_uint_t            i;

    if (src == NULL || dst == NULL || nal_length_size < 1
        || nal_length_size > 4)
    {
        return NGX_ERROR;
    }

    ngx_media_nal_iter_init(&it, src, len);

    while (ngx_media_nal_iter_next(&it, &nal)) {

        if (out + nal_length_size + nal.len > capacity) {
            return NGX_ERROR;
        }

        for (i = 0; i < nal_length_size; i++) {
            dst[out + nal_length_size - 1 - i] = (u_char) ((nal.len >> (i * 8))
                                                           & 0xFF);
        }

        out += nal_length_size;
        ngx_memcpy(dst + out, nal.data, nal.len);
        out += nal.len;
    }

    if (out == 0) {
        return NGX_ERROR;
    }

    *out_len = out;

    return NGX_OK;
}

static int32_t ngx_media_rtmp_flv_cts(const u_char *data);

static ngx_int_t ngx_media_rtmp_emit(ngx_media_rtmp_publisher_t *pub,
    ngx_media_buf_t *buf, size_t len, ngx_uint_t media_type, ngx_uint_t codec,
    ngx_uint_t format, ngx_uint_t track, int64_t pts, int64_t dts,
    ngx_uint_t keyframe, ngx_uint_t config, ngx_media_rtmp_frame_pt frame_cb,
    void *ctx);

/* --- HEVCDecoderConfigurationRecord (hvcC) ------------------------------ */

/*
 * The tail is identical to avcC: a count, then per array a completeness byte
 * carrying the NAL unit type, a 16-bit count, and length-prefixed NAL units.
 * Only the fixed header differs, so the loop is shared.
 */
static ngx_int_t
ngx_media_rtmp_hvcc_arrays(const u_char *data, size_t len, size_t pos,
    ngx_media_rtmp_hvcc_t *out)
{
    ngx_uint_t  arrays, i;

    if (pos >= len) {
        return NGX_ERROR;
    }

    arrays = data[pos++];

    for (i = 0; i < arrays; i++) {
        ngx_uint_t  nal_type, count, n;

        if (pos + 3 > len) {
            return NGX_ERROR;
        }

        nal_type = data[pos] & 0x3F;
        count = ((ngx_uint_t) data[pos + 1] << 8) | data[pos + 2];
        pos += 3;

        for (n = 0; n < count; n++) {
            size_t     nlen;
            ngx_str_t *slot = NULL;
            ngx_uint_t *nslot = NULL;

            if (pos + 2 > len) {
                return NGX_ERROR;
            }

            nlen = ((size_t) data[pos] << 8) | data[pos + 1];
            pos += 2;

            if (nlen == 0 || pos + nlen > len) {
                return NGX_ERROR;
            }

            /* 32 VPS, 33 SPS, 34 PPS */
            switch (nal_type) {
            case 32:
                if (out->nvps < NGX_MEDIA_RTMP_MAX_PARAM_SETS) {
                    slot = &out->vps[out->nvps];
                    nslot = &out->nvps;
                }
                break;
            case 33:
                if (out->nsps < NGX_MEDIA_RTMP_MAX_PARAM_SETS) {
                    slot = &out->sps[out->nsps];
                    nslot = &out->nsps;
                }
                break;
            case 34:
                if (out->npps < NGX_MEDIA_RTMP_MAX_PARAM_SETS) {
                    slot = &out->pps[out->npps];
                    nslot = &out->npps;
                }
                break;
            default:
                break;
            }

            if (slot != NULL) {
                slot->data = (u_char *) data + pos;
                slot->len = nlen;
                (*nslot)++;
            }

            pos += nlen;
        }
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_rtmp_hvcc_parse(const u_char *data, size_t len,
    ngx_media_rtmp_hvcc_t *out)
{
    if (data == NULL || out == NULL || len < 23) {
        return NGX_ERROR;
    }

    ngx_memzero(out, sizeof(ngx_media_rtmp_hvcc_t));

    out->version = data[0];
    out->profile_space = (data[1] >> 6) & 0x03;
    out->profile_idc = data[1] & 0x1F;
    out->level = data[12];
    out->nal_length_size = (data[21] & 0x03) + 1;

    if (out->version != 1 || out->nal_length_size > 4) {
        return NGX_ERROR;
    }

    if (ngx_media_rtmp_hvcc_arrays(data, len, 22, out) != NGX_OK) {
        return NGX_ERROR;
    }

    if (out->nsps == 0) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

size_t
ngx_media_rtmp_hvcc_annexb_size(const ngx_media_rtmp_hvcc_t *hvcc)
{
    size_t      total = 0;
    ngx_uint_t  i;

    for (i = 0; i < hvcc->nvps; i++) {
        total += 4 + hvcc->vps[i].len;
    }

    for (i = 0; i < hvcc->nsps; i++) {
        total += 4 + hvcc->sps[i].len;
    }

    for (i = 0; i < hvcc->npps; i++) {
        total += 4 + hvcc->pps[i].len;
    }

    return total;
}

ngx_int_t
ngx_media_rtmp_hvcc_to_annexb(const ngx_media_rtmp_hvcc_t *hvcc, u_char *dst,
    size_t capacity, size_t *out_len)
{
    size_t      pos = 0;
    ngx_uint_t  i;
    const ngx_str_t  *sets[3];
    const ngx_uint_t *counts[3];
    ngx_uint_t  group;

    if (capacity < ngx_media_rtmp_hvcc_annexb_size(hvcc)) {
        return NGX_ERROR;
    }

    sets[0] = hvcc->vps;   counts[0] = &hvcc->nvps;
    sets[1] = hvcc->sps;   counts[1] = &hvcc->nsps;
    sets[2] = hvcc->pps;   counts[2] = &hvcc->npps;

    for (group = 0; group < 3; group++) {
        for (i = 0; i < *counts[group]; i++) {
            const ngx_str_t  *set = &sets[group][i];

            dst[pos++] = 0;
            dst[pos++] = 0;
            dst[pos++] = 0;
            dst[pos++] = 1;
            ngx_memcpy(dst + pos, set->data, set->len);
            pos += set->len;
        }
    }

    *out_len = pos;

    return NGX_OK;
}

ngx_int_t
ngx_media_rtmp_hvcc_build(const ngx_media_rtmp_hvcc_t *hvcc, u_char *dst,
    size_t capacity, size_t *out_len)
{
    size_t      pos;
    ngx_uint_t  i;
    ngx_uint_t  arrays = 0;

    if (dst == NULL || hvcc == NULL || hvcc->nsps == 0) {
        return NGX_ERROR;
    }

    if (hvcc->nvps > 0) {
        arrays++;
    }

    arrays++;   /* SPS */
    arrays++;   /* PPS */

    if (capacity < 23) {
        return NGX_ERROR;
    }

    ngx_memzero(dst, 23);

    dst[0] = 1;                                   /* configurationVersion */
    dst[1] = (u_char) ((hvcc->profile_space << 6)
                       | (hvcc->profile_idc & 0x1F));
    dst[12] = (u_char) hvcc->level;
    dst[21] = 0x03;                               /* 4-byte NAL lengths */
    dst[22] = (u_char) arrays;

    pos = 23;

    for (i = 0; i < arrays; i++) {
        const ngx_str_t  *sets;
        ngx_uint_t        count, n, nal_type;

        if (i == 0 && hvcc->nvps > 0) {
            sets = hvcc->vps;
            count = hvcc->nvps;
            nal_type = 32;

        } else if (i == (hvcc->nvps > 0 ? 1 : 0)) {
            sets = hvcc->sps;
            count = hvcc->nsps;
            nal_type = 33;

        } else {
            sets = hvcc->pps;
            count = hvcc->npps;
            nal_type = 34;
        }

        if (capacity < pos + 3) {
            return NGX_ERROR;
        }

        dst[pos++] = (u_char) (0x80 | nal_type);   /* array_completeness */
        dst[pos++] = (u_char) ((count >> 8) & 0xFF);
        dst[pos++] = (u_char) (count & 0xFF);

        for (n = 0; n < count; n++) {
            if (capacity < pos + 2 + sets[n].len) {
                return NGX_ERROR;
            }

            dst[pos++] = (u_char) ((sets[n].len >> 8) & 0xFF);
            dst[pos++] = (u_char) (sets[n].len & 0xFF);
            ngx_memcpy(dst + pos, sets[n].data, sets[n].len);
            pos += sets[n].len;
        }
    }

    *out_len = pos;

    return NGX_OK;
}

/*
 * Video sequence header, legacy or enhanced: the parameter sets live on the
 * track as one Annex B blob (the shape the MPEG-TS path carries too, so
 * compatibility classification and the muxer need no RTMP knowledge), the
 * track is added or updated, and the blob is emitted as the configuration
 * frame a player needs before any coded frame.
 */
static ngx_int_t
ngx_media_rtmp_publisher_video_config(ngx_media_rtmp_publisher_t *pub,
    ngx_media_buf_t *config, size_t config_len, ngx_uint_t codec,
    ngx_uint_t profile, ngx_uint_t level, ngx_media_rtmp_frame_pt frame_cb,
    ngx_media_rtmp_tracks_pt tracks_cb, void *ctx)
{
    ngx_media_track_t  track;

    ngx_memzero(&track, sizeof(track));

    track.media_type = NGX_MEDIA_TYPE_VIDEO;
    track.codec = codec;
    track.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    track.profile = profile;
    track.level = level;
    track.config = config;      /* add() and the update below own a ref */

    if (!pub->have_video) {
        /* add() returns the new index, or a negative error */
        if (ngx_media_trackset_add(&pub->tracks, &track) < 0) {
            ngx_media_buf_unref(config);
            return NGX_ERROR;
        }

        pub->video_track = pub->tracks.count - 1;
        pub->have_video = 1;

    } else {
        ngx_media_track_t  *slot = &pub->tracks.tracks[pub->video_track];

        ngx_media_buf_unref(slot->config);
        slot->config = ngx_media_buf_ref(config);
        slot->codec = codec;
        slot->profile = profile;
        slot->level = level;
        slot->payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
    }

    if (ngx_media_rtmp_emit(pub, config, config_len, NGX_MEDIA_TYPE_VIDEO,
                            codec, NGX_MEDIA_PAYLOAD_ANNEXB, pub->video_track,
                            pub->last_dts, pub->last_dts, 1, 1, frame_cb,
                            ctx) != NGX_OK)
    {
        pub->errors++;
        return NGX_OK;
    }

    pub->configs++;

    if (tracks_cb != NULL) {
        (void) tracks_cb(ctx, &pub->tracks);
    }

    return NGX_OK;
}

/*
 * Coded frames, legacy or extended: convert length-prefixed NAL units into the
 * Annex B the core carries, then emit them as one program frame.  The two
 * paths differ only in how they found the payload and which codec it is.
 */
/* whether a NAL unit is a parameter set: SPS/PPS, or VPS/SPS/PPS for HEVC */
static ngx_uint_t
ngx_media_rtmp_parameter_set(ngx_uint_t codec, u_char header)
{
    ngx_uint_t  type;

    if (codec == NGX_MEDIA_CODEC_H265) {
        type = (header >> 1) & 0x3f;
        return type >= 32 && type <= 34;
    }

    type = header & 0x1f;
    return type == 7 || type == 8;
}

static ngx_int_t
ngx_media_rtmp_publisher_video_payload(ngx_media_rtmp_publisher_t *pub,
    uint32_t timestamp_ms, size_t nal_length_size, const u_char *payload,
    size_t payload_len, ngx_uint_t keyframe, int32_t cts, ngx_uint_t codec,
    ngx_media_rtmp_frame_pt frame_cb, void *ctx)
{
    ngx_media_buf_t  *buf, *sets = NULL;
    size_t            capacity, i, nals = 0, pos = 0, out_len = 0;
    size_t            sets_len = 0;
    ngx_uint_t        has_sets = 0;
    u_char           *out;

    /* one start code per NAL unit replaces its length prefix */
    while (pos + nal_length_size <= payload_len) {
        size_t  nlen = 0;

        for (i = 0; i < nal_length_size; i++) {
            nlen = (nlen << 8) | payload[pos + i];
        }

        if (nlen > 0 && pos + nal_length_size < payload_len
            && ngx_media_rtmp_parameter_set(codec,
                                            payload[pos + nal_length_size]))
        {
            has_sets = 1;
        }

        pos += nal_length_size + nlen;
        nals++;

        if (pos > payload_len) {
            nals = 0;
            break;
        }
    }

    if (nals == 0 || pos != payload_len) {
        pub->errors++;
        return NGX_OK;
    }

    /*
     * RTMP carries the parameter sets once, in the sequence header, and an
     * encoder writing FLV (ffmpeg, OBS) does not repeat them in its IDR
     * frames.  MPEG-TS has no sequence header: a segment, a late joiner or a
     * player that seeks has only what is in the stream.  So a keyframe that
     * does not carry its parameter sets gets the track's current ones in
     * front of it, as Annex B, which is what every TS-based consumer expects
     * - without this every HLS segment but the first was undecodable.
     */
    if (keyframe && !has_sets && pub->have_video
        && pub->tracks.tracks[pub->video_track].config != NULL)
    {
        sets = pub->tracks.tracks[pub->video_track].config;
        sets_len = ngx_media_buf_size(sets);
    }

    capacity = sets_len + payload_len + nals * 4;

    buf = ngx_media_buf_alloc(capacity);

    if (buf == NULL) {
        return NGX_ERROR;
    }

    out = ngx_media_buf_data(buf);

    if (sets_len > 0) {
        ngx_memcpy(out, ngx_media_buf_data(sets), sets_len);
    }

    if (ngx_media_rtmp_avcc_payload_to_annexb(payload, payload_len,
                                              nal_length_size, out + sets_len,
                                              capacity - sets_len, &out_len)
        != NGX_OK)
    {
        ngx_media_buf_unref(buf);
        pub->errors++;
        return NGX_OK;
    }

    out_len += sets_len;

    pub->last_dts = (int64_t) timestamp_ms * NGX_MEDIA_RTMP_TIMESCALE;

    if (ngx_media_rtmp_emit(pub, buf, out_len, NGX_MEDIA_TYPE_VIDEO, codec,
                            NGX_MEDIA_PAYLOAD_ANNEXB, pub->video_track,
                            ((int64_t) timestamp_ms + cts)
                                * NGX_MEDIA_RTMP_TIMESCALE,
                            (int64_t) timestamp_ms * NGX_MEDIA_RTMP_TIMESCALE,
                            keyframe, 0, frame_cb, ctx) != NGX_OK)
    {
        pub->errors++;
        return NGX_OK;
    }

    pub->frames++;

    return NGX_OK;
}

/*
 * Enhanced RTMP video tags.  data[0] carries the extended flag, the frame
 * type and the packet type; data[1..4] is the fourcc; the payload follows.
 */
static ngx_int_t
ngx_media_rtmp_publisher_video_ex(ngx_media_rtmp_publisher_t *pub,
    uint32_t timestamp_ms, const u_char *data, size_t len,
    ngx_media_rtmp_frame_pt frame_cb, ngx_media_rtmp_tracks_pt tracks_cb,
    void *ctx)
{
    ngx_uint_t    frame_type, packet_type, codec;
    int32_t       cts = 0;
    const u_char *payload;
    size_t        payload_len, blob_len, out_len;
    ngx_media_buf_t  *buf;
    u_char       *out;

    if (len < 5) {
        pub->skipped++;
        return NGX_OK;
    }

    frame_type = (data[0] >> 4) & NGX_MEDIA_RTMP_FRAME_MASK;
    packet_type = data[0] & 0x0F;
    payload = data + 5;
    payload_len = len - 5;

    if (packet_type == NGX_MEDIA_RTMP_EX_SEQUENCE_END) {
        return NGX_OK;
    }

    if (packet_type == NGX_MEDIA_RTMP_EX_METADATA) {
        /* the video info block; nothing the program model needs */
        pub->skipped++;
        return NGX_OK;
    }

    if (ngx_memcmp(data + 1, "hvc1", 4) == 0) {
        codec = NGX_MEDIA_CODEC_H265;

    } else if (ngx_memcmp(data + 1, "avc1", 4) == 0) {
        codec = NGX_MEDIA_CODEC_H264;

    } else {
        /* av01, vp09 and anything else this core does not model */
        pub->skipped++;
        return NGX_OK;
    }

    if (packet_type == NGX_MEDIA_RTMP_EX_SEQUENCE_START) {

        if (codec == NGX_MEDIA_CODEC_H265) {
            size_t  blob_len;

            if (ngx_media_rtmp_hvcc_parse(payload, payload_len, &pub->hvcc)
                != NGX_OK)
            {
                pub->errors++;
                return NGX_OK;
            }

            pub->have_hvcc = 1;
            blob_len = ngx_media_rtmp_hvcc_annexb_size(&pub->hvcc);

            buf = ngx_media_buf_alloc(blob_len);

            if (buf == NULL) {
                return NGX_ERROR;
            }

            out = ngx_media_buf_data(buf);

            if (ngx_media_rtmp_hvcc_to_annexb(&pub->hvcc, out, blob_len,
                                              &out_len) != NGX_OK)
            {
                ngx_media_buf_unref(buf);
                pub->errors++;
                return NGX_OK;
            }

            return ngx_media_rtmp_publisher_video_config(pub, buf, out_len,
                       codec, pub->hvcc.profile_idc, pub->hvcc.level,
                       frame_cb, tracks_cb, ctx);
        }

        if (ngx_media_rtmp_avcc_parse(payload, payload_len, &pub->avcc)
            != NGX_OK)
        {
            pub->errors++;
            return NGX_OK;
        }

        pub->have_avcc = 1;

        blob_len = ngx_media_rtmp_avcc_annexb_size(&pub->avcc);
        buf = ngx_media_buf_alloc(blob_len);

        if (buf == NULL) {
            return NGX_ERROR;
        }

        out = ngx_media_buf_data(buf);

        if (ngx_media_rtmp_avcc_to_annexb(&pub->avcc, out, blob_len,
                                          &out_len) != NGX_OK)
        {
            ngx_media_buf_unref(buf);
            pub->errors++;
            return NGX_OK;
        }

        return ngx_media_rtmp_publisher_video_config(pub, buf, out_len, codec,
                   pub->avcc.profile, pub->avcc.level, frame_cb, tracks_cb,
                   ctx);
    }

    if (codec == NGX_MEDIA_CODEC_H265) {
        if (!pub->have_hvcc) {
            pub->skipped++;
            return NGX_OK;
        }

        if (packet_type == NGX_MEDIA_RTMP_EX_CODED_FRAMES) {
            if (payload_len < 3) {
                pub->skipped++;
                return NGX_OK;
            }

            cts = ngx_media_rtmp_flv_cts(payload);
            payload += 3;
            payload_len -= 3;
        }

        return ngx_media_rtmp_publisher_video_payload(pub, timestamp_ms,
                   pub->hvcc.nal_length_size, payload, payload_len,
                   frame_type == NGX_MEDIA_RTMP_FRAME_KEYFRAME, cts,
                   NGX_MEDIA_CODEC_H265, frame_cb, ctx);
    }

    if (!pub->have_avcc) {
        pub->skipped++;
        return NGX_OK;
    }

    if (packet_type == NGX_MEDIA_RTMP_EX_CODED_FRAMES) {
        if (payload_len < 3) {
            pub->skipped++;
            return NGX_OK;
        }

        cts = ngx_media_rtmp_flv_cts(payload);
        payload += 3;
        payload_len -= 3;
    }

    return ngx_media_rtmp_publisher_video_payload(pub, timestamp_ms,
               pub->avcc.nal_length_size, payload, payload_len,
               frame_type == NGX_MEDIA_RTMP_FRAME_KEYFRAME, cts,
               NGX_MEDIA_CODEC_H264, frame_cb, ctx);
}

/* --- FLV message bodies -------------------------------------------------- */

ngx_int_t
ngx_media_rtmp_flv_video(u_char *dst, size_t capacity, size_t *out_len,
    ngx_uint_t keyframe, int32_t composition_time, const u_char *data,
    size_t len)
{
    size_t  pos = 0;
    uint32_t cts = (uint32_t) composition_time;

    if (dst == NULL || capacity < 5 + len) {
        return NGX_ERROR;
    }

    dst[pos++] = (u_char) (((keyframe ? NGX_MEDIA_RTMP_FRAME_KEYFRAME
                                      : NGX_MEDIA_RTMP_FRAME_INTER) << 4)
                           | NGX_MEDIA_RTMP_CODEC_AVC);
    dst[pos++] = NGX_MEDIA_RTMP_AVC_NALU;
    dst[pos++] = (u_char) ((cts >> 16) & 0xFF);
    dst[pos++] = (u_char) ((cts >> 8) & 0xFF);
    dst[pos++] = (u_char) (cts & 0xFF);

    if (len > 0) {
        ngx_memcpy(dst + pos, data, len);
        pos += len;
    }

    *out_len = pos;

    return NGX_OK;
}

ngx_int_t
ngx_media_rtmp_flv_audio(u_char *dst, size_t capacity, size_t *out_len,
    ngx_uint_t aac_sequence, const u_char *data, size_t len)
{
    size_t  pos = 0;

    if (dst == NULL || capacity < 2 + len) {
        return NGX_ERROR;
    }

    dst[pos++] = (u_char) ((NGX_MEDIA_RTMP_SOUND_AAC << 4) | 0x0F);
    dst[pos++] = (u_char) (aac_sequence ? NGX_MEDIA_RTMP_AAC_SEQUENCE
                                        : NGX_MEDIA_RTMP_AAC_RAW);

    if (len > 0) {
        ngx_memcpy(dst + pos, data, len);
        pos += len;
    }

    *out_len = pos;

    return NGX_OK;
}

/* --- publisher ----------------------------------------------------------- */

ngx_int_t
ngx_media_rtmp_publisher_init(ngx_media_rtmp_publisher_t *pub, ngx_log_t *log)
{
    if (pub == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(pub, sizeof(ngx_media_rtmp_publisher_t));

    if (ngx_media_trackset_init(&pub->tracks, 4, log) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

void
ngx_media_rtmp_publisher_destroy(ngx_media_rtmp_publisher_t *pub)
{
    if (pub == NULL) {
        return;
    }

    ngx_media_trackset_destroy(&pub->tracks);
    ngx_memzero(pub, sizeof(ngx_media_rtmp_publisher_t));
}

static int32_t
ngx_media_rtmp_flv_cts(const u_char *p)
{
    int32_t  v = ((int32_t) p[0] << 16) | ((int32_t) p[1] << 8)
                 | (int32_t) p[2];

    if (v & 0x800000) {
        v |= ~0xFFFFFF;   /* sign extend the 24 bit composition time */
    }

    return v;
}

/* builds and delivers a frame that adopts the given buffer */
static ngx_int_t
ngx_media_rtmp_emit(ngx_media_rtmp_publisher_t *pub, ngx_media_buf_t *buf,
    size_t len, ngx_uint_t media_type, ngx_uint_t codec, ngx_uint_t format,
    ngx_uint_t track_index, int64_t pts, int64_t dts, ngx_uint_t keyframe,
    ngx_uint_t config, ngx_media_rtmp_frame_pt frame_cb, void *ctx)
{
    ngx_media_frame_t  frame;

    (void) ngx_media_buf_freeze(buf, len);

    ngx_media_frame_init(&frame);

    frame.media_type = media_type;
    frame.codec = codec;
    frame.payload_format = format;
    frame.track_index = track_index;
    frame.pts = pts;
    frame.dts = dts;
    frame.keyframe = keyframe ? 1 : 0;
    frame.config = config ? 1 : 0;

    ngx_media_frame_adopt(&frame, buf);

    (void) pub;

    if (frame_cb != NULL && frame_cb(ctx, &frame) != NGX_OK) {
        ngx_media_frame_release(&frame);
        return NGX_ERROR;
    }

    ngx_media_frame_release(&frame);

    return NGX_OK;
}

static ngx_int_t
ngx_media_rtmp_publisher_video(ngx_media_rtmp_publisher_t *pub,
    uint32_t timestamp_ms, const u_char *data, size_t len,
    ngx_media_rtmp_frame_pt frame_cb, ngx_media_rtmp_tracks_pt tracks_cb,
    void *ctx)
{
    ngx_uint_t    frame_type, packet_type;
    int32_t       cts;
    ngx_media_buf_t  *buf;
    size_t        out_len = 0;
    u_char       *out;

    if (len < 5) {
        pub->skipped++;
        return NGX_OK;
    }

    if ((data[0] & NGX_MEDIA_RTMP_EX_HEADER_FLAG) != 0) {
        return ngx_media_rtmp_publisher_video_ex(pub, timestamp_ms, data, len,
                                                 frame_cb, tracks_cb, ctx);
    }

    frame_type = data[0] >> 4;
    packet_type = data[1];
    cts = ngx_media_rtmp_flv_cts(data + 2);

    if (packet_type == NGX_MEDIA_RTMP_AVC_END_SEQUENCE) {
        return NGX_OK;
    }

    if (packet_type == NGX_MEDIA_RTMP_AVC_SEQUENCE) {

        if (ngx_media_rtmp_avcc_parse(data + 5, len - 5, &pub->avcc)
            != NGX_OK)
        {
            pub->errors++;      /* the module logs the counter */
            return NGX_OK;
        }

        pub->have_avcc = 1;

        /*
         * The track contract carries the parameter sets as an Annex B blob,
         * exactly like the MPEG-TS path, so compatibility classification and
         * the TS muxer need no RTMP-specific knowledge.
         */
        {
            size_t  blob_len = ngx_media_rtmp_avcc_annexb_size(&pub->avcc);

            buf = ngx_media_buf_alloc(blob_len);

            if (buf == NULL) {
                return NGX_ERROR;
            }

            out = ngx_media_buf_data(buf);

            if (ngx_media_rtmp_avcc_to_annexb(&pub->avcc, out, blob_len,
                                              &out_len) != NGX_OK)
            {
                ngx_media_buf_unref(buf);
                pub->errors++;
                return NGX_OK;
            }
        }

        {
            ngx_media_track_t  track;

            ngx_memzero(&track, sizeof(track));

            track.media_type = NGX_MEDIA_TYPE_VIDEO;
            track.codec = NGX_MEDIA_CODEC_H264;
            track.payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
            track.profile = pub->avcc.profile;
            track.level = pub->avcc.level;
            track.config = buf;      /* add() and the update below own a ref */

            if (!pub->have_video) {
                /* add() returns the new index, or a negative error */
                if (ngx_media_trackset_add(&pub->tracks, &track) < 0) {
                    ngx_media_buf_unref(buf);
                    return NGX_ERROR;
                }

                pub->video_track = pub->tracks.count - 1;
                pub->have_video = 1;

            } else {
                ngx_media_track_t  *slot = &pub->tracks.tracks[pub->video_track];

                ngx_media_buf_unref(slot->config);
                slot->config = ngx_media_buf_ref(buf);
                slot->profile = pub->avcc.profile;
                slot->level = pub->avcc.level;
                slot->payload_format = NGX_MEDIA_PAYLOAD_ANNEXB;
            }
        }

        if (ngx_media_rtmp_emit(pub, buf, out_len, NGX_MEDIA_TYPE_VIDEO,
                                NGX_MEDIA_CODEC_H264,
                                NGX_MEDIA_PAYLOAD_ANNEXB, pub->video_track,
                                pub->last_dts, pub->last_dts, 1, 1, frame_cb,
                                ctx) != NGX_OK)
        {
            pub->errors++;
        }

        pub->configs++;

        if (tracks_cb != NULL) {
            (void) tracks_cb(ctx, &pub->tracks);
        }

        return NGX_OK;
    }

    if (!pub->have_avcc) {
        pub->skipped++;
        return NGX_OK;
    }

    {
        size_t  capacity, i, nals = 0, pos = 0;
        size_t  nal_length_size = pub->avcc.nal_length_size;

        /* one start code per NAL unit replaces its length prefix */
        while (pos + nal_length_size <= len - 5) {
            size_t  nlen = 0;

            for (i = 0; i < nal_length_size; i++) {
                nlen = (nlen << 8) | data[5 + pos + i];
            }

            pos += nal_length_size + nlen;
            nals++;

            if (pos > len - 5) {
                nals = 0;
                break;
            }
        }

        if (nals == 0 || pos != len - 5) {
            pub->errors++;
            return NGX_OK;
        }

        capacity = len - 5 + nals * 4;

        buf = ngx_media_buf_alloc(capacity);

        if (buf == NULL) {
            return NGX_ERROR;
        }

        out = ngx_media_buf_data(buf);

        if (ngx_media_rtmp_avcc_payload_to_annexb(data + 5, len - 5,
                                                  nal_length_size, out,
                                                  capacity, &out_len)
            != NGX_OK)
        {
            ngx_media_buf_unref(buf);
            pub->errors++;
            return NGX_OK;
        }
    }

    pub->last_dts = (int64_t) timestamp_ms * NGX_MEDIA_RTMP_TIMESCALE;

    if (ngx_media_rtmp_emit(pub, buf, out_len, NGX_MEDIA_TYPE_VIDEO,
                            NGX_MEDIA_CODEC_H264, NGX_MEDIA_PAYLOAD_ANNEXB,
                            pub->video_track,
                            ((int64_t) timestamp_ms + cts)
                                * NGX_MEDIA_RTMP_TIMESCALE,
                            (int64_t) timestamp_ms * NGX_MEDIA_RTMP_TIMESCALE,
                            frame_type == NGX_MEDIA_RTMP_FRAME_KEYFRAME, 0,
                            frame_cb, ctx) != NGX_OK)
    {
        pub->errors++;
        return NGX_OK;
    }

    pub->frames++;

    return NGX_OK;
}

static ngx_int_t
ngx_media_rtmp_publisher_audio(ngx_media_rtmp_publisher_t *pub,
    uint32_t timestamp_ms, const u_char *data, size_t len,
    ngx_media_rtmp_frame_pt frame_cb, ngx_media_rtmp_tracks_pt tracks_cb,
    void *ctx)
{
    ngx_uint_t        packet_type;
    ngx_media_buf_t  *buf;
    size_t            out_len = 0;
    u_char           *out;

    if (len < 2) {
        pub->skipped++;
        return NGX_OK;
    }

    packet_type = data[1];

    if (packet_type == NGX_MEDIA_RTMP_AAC_SEQUENCE) {

        if (ngx_media_rtmp_asc_parse(data + 2, len - 2, &pub->asc) != NGX_OK) {
            pub->errors++;
            return NGX_OK;
        }

        pub->have_asc = 1;

        buf = ngx_media_buf_alloc(len - 2);

        if (buf == NULL) {
            return NGX_ERROR;
        }

        out = ngx_media_buf_data(buf);
        ngx_memcpy(out, data + 2, len - 2);
        out_len = len - 2;

        {
            ngx_media_track_t  track;

            ngx_memzero(&track, sizeof(track));

            track.media_type = NGX_MEDIA_TYPE_AUDIO;
            track.codec = NGX_MEDIA_CODEC_AAC;
            track.payload_format = NGX_MEDIA_PAYLOAD_ADTS;
            track.profile = pub->asc.object_type;
            track.sample_rate = pub->asc.sample_rate;
            track.channels = pub->asc.channels;
            track.config = buf;      /* add() and the update below own a ref */

            if (!pub->have_audio) {
                /* add() returns the new index, or a negative error */
                if (ngx_media_trackset_add(&pub->tracks, &track) < 0) {
                    ngx_media_buf_unref(buf);
                    return NGX_ERROR;
                }

                pub->audio_track = pub->tracks.count - 1;
                pub->have_audio = 1;

            } else {
                ngx_media_track_t  *slot = &pub->tracks.tracks[pub->audio_track];

                ngx_media_buf_unref(slot->config);
                slot->config = ngx_media_buf_ref(buf);
                slot->profile = pub->asc.object_type;
                slot->sample_rate = pub->asc.sample_rate;
                slot->channels = pub->asc.channels;
                slot->payload_format = NGX_MEDIA_PAYLOAD_ADTS;
            }
        }

        if (ngx_media_rtmp_emit(pub, buf, out_len, NGX_MEDIA_TYPE_AUDIO,
                                NGX_MEDIA_CODEC_AAC, NGX_MEDIA_PAYLOAD_RAW,
                                pub->audio_track, pub->last_dts, pub->last_dts,
                                1, 1, frame_cb, ctx) != NGX_OK)
        {
            pub->errors++;
        }

        pub->configs++;

        if (tracks_cb != NULL) {
            (void) tracks_cb(ctx, &pub->tracks);
        }

        return NGX_OK;
    }

    if (packet_type != NGX_MEDIA_RTMP_AAC_RAW || !pub->have_asc) {
        pub->skipped++;
        return NGX_OK;
    }

    {
        /* raw AAC to ADTS: the core carries ADTS like the TS path does */
        ngx_media_adts_t  adts;
        u_char            header[NGX_MEDIA_ADTS_HEADER_MAX];
        size_t            header_len = 0;

        ngx_memzero(&adts, sizeof(adts));

        adts.object_type = pub->asc.object_type;
        adts.sample_rate = pub->asc.sample_rate;
        adts.channels = pub->asc.channels;
        adts.frame_len = (ngx_uint_t) (len - 2 + NGX_MEDIA_ADTS_HEADER_MIN);
        adts.header_len = NGX_MEDIA_ADTS_HEADER_MIN;

        if (ngx_media_adts_write(&adts, header, sizeof(header), &header_len)
            != NGX_OK)
        {
            pub->errors++;
            return NGX_OK;
        }

        buf = ngx_media_buf_alloc(header_len + len - 2);

        if (buf == NULL) {
            return NGX_ERROR;
        }

        out = ngx_media_buf_data(buf);

        ngx_memcpy(out, header, header_len);
        ngx_memcpy(out + header_len, data + 2, len - 2);
        out_len = header_len + len - 2;
    }

    pub->last_dts = (int64_t) timestamp_ms * NGX_MEDIA_RTMP_TIMESCALE;

    if (ngx_media_rtmp_emit(pub, buf, out_len, NGX_MEDIA_TYPE_AUDIO,
                            NGX_MEDIA_CODEC_AAC, NGX_MEDIA_PAYLOAD_ADTS,
                            pub->audio_track,
                            (int64_t) timestamp_ms * NGX_MEDIA_RTMP_TIMESCALE,
                            (int64_t) timestamp_ms * NGX_MEDIA_RTMP_TIMESCALE,
                            1, 0, frame_cb, ctx) != NGX_OK)
    {
        pub->errors++;
        return NGX_OK;
    }

    pub->frames++;

    return NGX_OK;
}

ngx_int_t
ngx_media_rtmp_publisher_feed(ngx_media_rtmp_publisher_t *pub, ngx_uint_t type,
    uint32_t timestamp_ms, const u_char *data, size_t len,
    ngx_media_rtmp_frame_pt frame_cb, ngx_media_rtmp_tracks_pt tracks_cb,
    void *ctx)
{
    if (pub == NULL || data == NULL) {
        return NGX_ERROR;
    }

    if (type == NGX_MEDIA_RTMP_MSG_VIDEO) {
        return ngx_media_rtmp_publisher_video(pub, timestamp_ms, data, len,
                                              frame_cb, tracks_cb, ctx);
    }

    if (type == NGX_MEDIA_RTMP_MSG_AUDIO) {
        return ngx_media_rtmp_publisher_audio(pub, timestamp_ms, data, len,
                                              frame_cb, tracks_cb, ctx);
    }

    return NGX_OK;
}

/* --- fanout ring --------------------------------------------------------- */

void
ngx_media_rtmp_fanout_init(ngx_media_rtmp_fanout_t *fan, ngx_uint_t capacity,
    size_t max_bytes)
{
    if (fan == NULL) {
        return;
    }

    ngx_memzero(fan, sizeof(ngx_media_rtmp_fanout_t));

    fan->capacity = (capacity > 0 && capacity <= NGX_MEDIA_RTMP_FANOUT_UNITS)
                        ? capacity
                        : NGX_MEDIA_RTMP_FANOUT_UNITS;
    fan->max_bytes = max_bytes ? max_bytes : 16 * 1024 * 1024;
}

void
ngx_media_rtmp_fanout_destroy(ngx_media_rtmp_fanout_t *fan)
{
    ngx_uint_t  i;

    if (fan == NULL) {
        return;
    }

    for (i = 0; i < fan->capacity; i++) {

        if (fan->units[i].payload != NULL) {
            ngx_media_buf_unref(fan->units[i].payload);
            fan->units[i].payload = NULL;
        }
    }

    ngx_memzero(fan, sizeof(ngx_media_rtmp_fanout_t));
}

static void
ngx_media_rtmp_fanout_evict(ngx_media_rtmp_fanout_t *fan)
{
    ngx_uint_t  index;

    while (fan->tail < fan->head
           && (fan->head - fan->tail > fan->capacity
               || (fan->max_bytes != 0 && fan->bytes > fan->max_bytes)))
    {
        index = (ngx_uint_t) (fan->tail % fan->capacity);

        if (fan->units[index].payload != NULL) {
            fan->bytes -= ngx_media_buf_size(fan->units[index].payload);
            ngx_media_buf_unref(fan->units[index].payload);
            fan->units[index].payload = NULL;
        }

        fan->tail++;
        fan->dropped++;
    }
}

ngx_int_t
ngx_media_rtmp_fanout_push(ngx_media_rtmp_fanout_t *fan, ngx_uint_t type,
    ngx_uint_t track, uint32_t timestamp, ngx_media_buf_t *payload,
    ngx_uint_t keyframe, ngx_uint_t config)
{
    ngx_media_rtmp_media_t  *unit;
    ngx_uint_t               index;

    if (fan == NULL || payload == NULL) {
        return NGX_ERROR;
    }

    index = (ngx_uint_t) (fan->head % fan->capacity);
    unit = &fan->units[index];

    if (unit->payload != NULL) {
        fan->bytes -= ngx_media_buf_size(unit->payload);
        ngx_media_buf_unref(unit->payload);
    }

    unit->payload = ngx_media_buf_ref(payload);
    unit->type = type;
    unit->track = track;
    unit->timestamp = timestamp;
    unit->sequence = fan->head;
    unit->keyframe = keyframe ? 1 : 0;
    unit->config = config ? 1 : 0;

    fan->bytes += ngx_media_buf_size(payload);
    fan->head++;
    fan->converted++;

    ngx_media_rtmp_fanout_evict(fan);

    return NGX_OK;
}

const ngx_media_rtmp_media_t *
ngx_media_rtmp_fanout_next(const ngx_media_rtmp_fanout_t *fan, uint64_t from)
{
    uint64_t    sequence;
    ngx_uint_t  index;

    if (fan == NULL || fan->tail >= fan->head) {
        return NULL;
    }

    sequence = (from < fan->tail) ? fan->tail : from;

    if (sequence >= fan->head) {
        return NULL;
    }

    index = (ngx_uint_t) (sequence % fan->capacity);

    if (fan->units[index].payload == NULL) {
        return NULL;
    }

    return &fan->units[index];
}

uint64_t
ngx_media_rtmp_fanout_head(const ngx_media_rtmp_fanout_t *fan)
{
    return (fan != NULL) ? fan->head : 0;
}

/* --- program preparation ------------------------------------------------- */

void
ngx_media_rtmp_prepare_init(ngx_media_rtmp_prepare_t *prep, ngx_uint_t units,
    size_t max_bytes)
{
    if (prep == NULL) {
        return;
    }

    ngx_memzero(prep, sizeof(ngx_media_rtmp_prepare_t));

    prep->nal_length_size = 4;

    ngx_media_rtmp_fanout_init(&prep->fan, units, max_bytes);
}

void
ngx_media_rtmp_prepare_destroy(ngx_media_rtmp_prepare_t *prep)
{
    if (prep == NULL) {
        return;
    }

    ngx_media_rtmp_fanout_destroy(&prep->fan);
    ngx_memzero(prep, sizeof(ngx_media_rtmp_prepare_t));
}

/* an FLV sequence header built from the program's own codec configuration */
static ngx_int_t
ngx_media_rtmp_prepare_sequence(ngx_media_rtmp_prepare_t *prep,
    ngx_uint_t media_type, const ngx_media_track_t *track)
{
    u_char            body[1024];
    u_char            avcc_record[512];
    size_t            body_len = 0, record_len = 0;
    ngx_media_buf_t  *payload;
    ngx_int_t         rc = NGX_OK;

    if (track == NULL || track->config == NULL) {
        return NGX_OK;
    }

    if (media_type == NGX_MEDIA_TYPE_VIDEO) {
        ngx_media_nal_iter_t  it;
        ngx_media_nal_t       nal;
        ngx_uint_t            have_sps = 0;
        const u_char         *sps = NULL;
        size_t                sps_len = 0, pps_len = 0;
        const u_char         *pps = NULL;

        if (track->codec == NGX_MEDIA_CODEC_H265) {
            ngx_media_rtmp_hvcc_t  hvcc;
            const u_char          *vps = NULL;
            size_t                 vps_len = 0;

            ngx_memzero(&hvcc, sizeof(hvcc));

            ngx_media_nal_iter_init(&it, ngx_media_buf_data(track->config),
                                    ngx_media_buf_size(track->config));

            while (ngx_media_nal_iter_next(&it, &nal)) {
                ngx_uint_t  type;

                if (nal.len < 1) {
                    continue;
                }

                type = ngx_media_nal_type(NGX_MEDIA_CODEC_H265, nal.data,
                                          nal.len);

                if (type == 32 && vps == NULL) {
                    vps = nal.data;
                    vps_len = nal.len;

                } else if (type == 33 && !have_sps) {
                    sps = nal.data;
                    sps_len = nal.len;
                    have_sps = 1;

                } else if (type == 34 && pps == NULL) {
                    pps = nal.data;
                    pps_len = nal.len;
                }
            }

            /*
             * profile_space/profile_idc live in SPS byte 1 and level_idc in
             * byte 12, so anything shorter cannot be described.
             */
            if (vps == NULL || !have_sps || pps == NULL || sps_len < 13) {
                prep->skipped++;
                return NGX_OK;
            }

            hvcc.version = 1;
            hvcc.nal_length_size = prep->nal_length_size;
            hvcc.profile_space = (sps[1] >> 6) & 0x03;
            hvcc.profile_idc = sps[1] & 0x1F;
            hvcc.level = sps[12];
            hvcc.nvps = 1;
            hvcc.vps[0].data = (u_char *) vps;
            hvcc.vps[0].len = vps_len;
            hvcc.nsps = 1;
            hvcc.sps[0].data = (u_char *) sps;
            hvcc.sps[0].len = sps_len;
            hvcc.npps = 1;
            hvcc.pps[0].data = (u_char *) pps;
            hvcc.pps[0].len = pps_len;

            if (ngx_media_rtmp_hvcc_build(&hvcc, avcc_record,
                                          sizeof(avcc_record),
                                          &record_len) != NGX_OK)
            {
                prep->errors++;
                return NGX_OK;
            }

            /*
             * Enhanced RTMP: the extended header carries the fourcc, and the
             * hvcC record follows it.  Legacy FLV has no way to signal HEVC
             * at all, so this is the only form that can carry it.
             */
            body[0] = (u_char) (NGX_MEDIA_RTMP_EX_HEADER_FLAG
                                | (NGX_MEDIA_RTMP_FRAME_KEYFRAME << 4)
                                | NGX_MEDIA_RTMP_EX_SEQUENCE_START);
            ngx_memcpy(body + 1, "hvc1", 4);
            ngx_memcpy(body + 5, avcc_record, record_len);
            body_len = 5 + record_len;

            goto sequence_body;
        }

        ngx_media_nal_iter_init(&it, ngx_media_buf_data(track->config),
                                ngx_media_buf_size(track->config));

        while (ngx_media_nal_iter_next(&it, &nal)) {

            if (nal.len < 1) {
                continue;
            }

            if (ngx_media_nal_type(NGX_MEDIA_CODEC_H264, nal.data, nal.len)
                == 7 && !have_sps)
            {
                sps = nal.data;
                sps_len = nal.len;
                have_sps = 1;

            } else if (ngx_media_nal_type(NGX_MEDIA_CODEC_H264, nal.data,
                                          nal.len) == 8 && pps == NULL)
            {
                pps = nal.data;
                pps_len = nal.len;
            }
        }

        if (!have_sps || pps == NULL || sps_len < 4) {
            prep->skipped++;
            return NGX_OK;
        }

        {
            ngx_media_rtmp_avcc_t  avcc;

            ngx_memzero(&avcc, sizeof(avcc));

            avcc.version = 1;
            avcc.profile = sps[1];
            avcc.compatibility = sps[2];
            avcc.level = sps[3];
            avcc.nal_length_size = prep->nal_length_size;
            avcc.nsps = 1;
            avcc.sps[0].data = (u_char *) sps;
            avcc.sps[0].len = sps_len;
            avcc.npps = 1;
            avcc.pps[0].data = (u_char *) pps;
            avcc.pps[0].len = pps_len;

            if (ngx_media_rtmp_avcc_build(&avcc, avcc_record,
                                          sizeof(avcc_record),
                                          &record_len) != NGX_OK)
            {
                prep->errors++;
                return NGX_OK;
            }

            body[0] = (u_char) ((NGX_MEDIA_RTMP_FRAME_KEYFRAME << 4)
                                | NGX_MEDIA_RTMP_CODEC_AVC);
            body[1] = NGX_MEDIA_RTMP_AVC_SEQUENCE;
            body[2] = body[3] = body[4] = 0;
            ngx_memcpy(body + 5, avcc_record, record_len);
            body_len = 5 + record_len;
        }

sequence_body:

    } else {
        /* the audio configuration is the AudioSpecificConfig itself */
        body[0] = (u_char) ((NGX_MEDIA_RTMP_SOUND_AAC << 4) | 0x0F);
        body[1] = NGX_MEDIA_RTMP_AAC_SEQUENCE;
        ngx_memcpy(body + 2, ngx_media_buf_data(track->config),
                   ngx_media_buf_size(track->config));
        body_len = 2 + ngx_media_buf_size(track->config);
    }

    payload = ngx_media_buf_alloc(body_len);

    if (payload == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(ngx_media_buf_data(payload), body, body_len);
    (void) ngx_media_buf_freeze(payload, body_len);

    rc = ngx_media_rtmp_fanout_push(&prep->fan,
                                    (media_type == NGX_MEDIA_TYPE_VIDEO)
                                        ? NGX_MEDIA_RTMP_MSG_VIDEO
                                        : NGX_MEDIA_RTMP_MSG_AUDIO,
                                    track->media_type, 0, payload, 1, 1);

    ngx_media_buf_unref(payload);

    return rc;
}

/* pushes the sequence headers of a new program contract */
ngx_int_t
ngx_media_rtmp_prepare_announce(ngx_media_rtmp_prepare_t *prep,
    ngx_media_trackset_t *tracks)
{
    ngx_uint_t  i;

    for (i = 0; i < tracks->count; i++) {

        if (ngx_media_rtmp_prepare_sequence(prep, tracks->tracks[i].media_type,
                                            &tracks->tracks[i]) != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    prep->tracks = tracks;

    return NGX_OK;
}

ngx_int_t
ngx_media_rtmp_prepare_frame(ngx_media_rtmp_prepare_t *prep,
    const ngx_media_frame_t *frame)
{
    u_char           *body;
    u_char           *converted;
    size_t            body_len = 0, converted_len = 0;
    size_t            capacity;
    ngx_media_buf_t  *payload;
    ngx_uint_t        type;
    uint32_t          timestamp;
    int32_t           composition_time;
    ngx_int_t         rc;

    if (prep == NULL || frame == NULL) {
        return NGX_ERROR;
    }

    if (frame->media_type != NGX_MEDIA_TYPE_VIDEO
        && frame->media_type != NGX_MEDIA_TYPE_AUDIO)
    {
        return NGX_OK;
    }

    if (frame->payload == NULL) {
        return NGX_OK;
    }

    timestamp = (uint32_t) (frame->dts / NGX_MEDIA_RTMP_TIMESCALE);

    if (frame->media_type == NGX_MEDIA_TYPE_VIDEO) {
        size_t  len = ngx_media_buf_size(frame->payload);

        /* AVCC is shorter than Annex B: one length per start code at most */
        capacity = len + 4;

        converted = ngx_alloc(capacity, NULL);

        if (converted == NULL) {
            return NGX_ERROR;
        }

        if (ngx_media_rtmp_annexb_payload_to_avcc(
                ngx_media_buf_data(frame->payload), len,
                prep->nal_length_size, converted, capacity,
                &converted_len) != NGX_OK)
        {
            ngx_free(converted);
            prep->skipped++;
            return NGX_OK;
        }

        composition_time = (int32_t) ((frame->pts - frame->dts)
                                      / NGX_MEDIA_RTMP_TIMESCALE);

        type = NGX_MEDIA_RTMP_MSG_VIDEO;

    } else {
        /* strip the ADTS framing the core carries: headers are 7 or 9 bytes */
        ngx_media_adts_t  adts;
        size_t            len = ngx_media_buf_size(frame->payload);
        size_t            header_len = 0;

        if (ngx_media_adts_parse(ngx_media_buf_data(frame->payload), len,
                                 &adts) == NGX_OK)
        {
            header_len = adts.header_len;
        }

        if (header_len >= len) {
            prep->skipped++;
            return NGX_OK;
        }

        converted_len = len - header_len;
        converted = ngx_alloc(converted_len, NULL);

        if (converted == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(converted, ngx_media_buf_data(frame->payload) + header_len,
                   converted_len);

        composition_time = 0;
        type = NGX_MEDIA_RTMP_MSG_AUDIO;
    }

    /*
     * A video body is 5 bytes of legacy header, or 8 for the enhanced one
     * (extended header byte, fourcc, three composition-time bytes).
     */
    payload = ngx_media_buf_alloc(converted_len
                                  + ((type != NGX_MEDIA_RTMP_MSG_VIDEO) ? 2
                                     : (frame->codec == NGX_MEDIA_CODEC_H265
                                            ? 8 : 5)));

    if (payload == NULL) {
        ngx_free(converted);
        return NGX_ERROR;
    }

    body = ngx_media_buf_data(payload);

    if (type == NGX_MEDIA_RTMP_MSG_VIDEO) {
        uint32_t  cts = (uint32_t) composition_time;
        u_char    frame_type = (u_char) (frame->keyframe
                                             ? NGX_MEDIA_RTMP_FRAME_KEYFRAME
                                             : NGX_MEDIA_RTMP_FRAME_INTER);

        if (frame->codec == NGX_MEDIA_CODEC_H265) {
            /*
             * Legacy FLV cannot signal HEVC, so H.265 goes out in the
             * enhanced form: extended header, fourcc, then composition time.
             */
            body[0] = (u_char) (NGX_MEDIA_RTMP_EX_HEADER_FLAG
                                | (frame_type << 4)
                                | NGX_MEDIA_RTMP_EX_CODED_FRAMES);
            ngx_memcpy(body + 1, "hvc1", 4);
            body[5] = (u_char) ((cts >> 16) & 0xFF);
            body[6] = (u_char) ((cts >> 8) & 0xFF);
            body[7] = (u_char) (cts & 0xFF);
            body_len = 8;

        } else {
            body[0] = (u_char) ((frame_type << 4) | NGX_MEDIA_RTMP_CODEC_AVC);
            body[1] = NGX_MEDIA_RTMP_AVC_NALU;
            body[2] = (u_char) ((cts >> 16) & 0xFF);
            body[3] = (u_char) ((cts >> 8) & 0xFF);
            body[4] = (u_char) (cts & 0xFF);
            body_len = 5;
        }

    } else {
        body[0] = (u_char) ((NGX_MEDIA_RTMP_SOUND_AAC << 4) | 0x0F);
        body[1] = NGX_MEDIA_RTMP_AAC_RAW;
        body_len = 2;
    }

    ngx_memcpy(body + body_len, converted, converted_len);
    body_len += converted_len;

    ngx_free(converted);
    (void) ngx_media_buf_freeze(payload, body_len);

    rc = ngx_media_rtmp_fanout_push(&prep->fan, type, frame->track_index,
                                    timestamp, payload, frame->keyframe, 0);


    ngx_media_buf_unref(payload);

    return rc;
}

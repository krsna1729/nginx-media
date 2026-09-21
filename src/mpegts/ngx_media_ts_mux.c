#include "ngx_media_ts_mux.h"

#define NGX_MEDIA_TS_PACKET  188
#define NGX_MEDIA_TS_PAYLOAD 184

static void ngx_media_ts_put_ts(u_char *p, ngx_uint_t prefix, uint64_t v);
static size_t ngx_media_ts_payload_capacity(size_t remain, ngx_uint_t with_pcr);
static void ngx_media_ts_copy_pes(u_char *dst, size_t n, const u_char *header,
    size_t header_len, const u_char *payload, size_t payload_len, size_t off);
static ngx_int_t ngx_media_ts_mux_packet(ngx_media_ts_mux_t *mux,
    ngx_media_ts_burst_t *burst, ngx_uint_t pid, ngx_uint_t *cc,
    ngx_uint_t pusi, size_t payload_len, ngx_uint_t with_pcr,
    int64_t pcr_base, u_char **out);
static void ngx_media_ts_mux_section(ngx_media_ts_mux_t *mux,
    ngx_media_ts_burst_t *burst, ngx_uint_t pid, ngx_uint_t *cc,
    u_char *section, size_t len);
static size_t ngx_media_ts_mux_pat(ngx_media_ts_mux_t *mux, u_char *out);
static size_t ngx_media_ts_mux_pmt(ngx_media_ts_mux_t *mux, u_char *out);

void
ngx_media_ts_mux_conf_default(ngx_media_ts_mux_conf_t *conf)
{
    if (conf == NULL) {
        return;
    }

    ngx_memzero(conf, sizeof(ngx_media_ts_mux_conf_t));

    conf->pmt_pid = NGX_MEDIA_TS_MUX_PID_PMT;
    conf->pcr_pid = NGX_MEDIA_TS_MUX_PID_VIDEO;
    conf->video_pid = NGX_MEDIA_TS_MUX_PID_VIDEO;
    conf->audio_pid = NGX_MEDIA_TS_MUX_PID_AUDIO;
    conf->default_burst_bytes = 512 * 1024;
    conf->max_burst_bytes = 4 * 1024 * 1024;
}

ngx_int_t
ngx_media_ts_mux_init(ngx_media_ts_mux_t *mux,
    const ngx_media_ts_mux_conf_t *conf, ngx_log_t *log)
{
    (void) log;

    if (mux == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(mux, sizeof(ngx_media_ts_mux_t));

    if (conf != NULL) {
        mux->conf = *conf;

    } else {
        ngx_media_ts_mux_conf_default(&mux->conf);
    }

    if (mux->conf.default_burst_bytes == 0) {
        mux->conf.default_burst_bytes = 512 * 1024;
    }

    if (mux->conf.max_burst_bytes < mux->conf.default_burst_bytes) {
        mux->conf.max_burst_bytes = mux->conf.default_burst_bytes;
    }

    return NGX_OK;
}

void
ngx_media_ts_mux_destroy(ngx_media_ts_mux_t *mux)
{
    if (mux == NULL) {
        return;
    }

    ngx_memzero(mux, sizeof(ngx_media_ts_mux_t));
}

ngx_int_t
ngx_media_ts_mux_set_tracks(ngx_media_ts_mux_t *mux,
    const ngx_media_trackset_t *tracks)
{
    ngx_uint_t  i, video = 0, audio = 0;

    if (mux == NULL || tracks == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < tracks->count; i++) {

        if (tracks->tracks[i].media_type == NGX_MEDIA_TYPE_VIDEO
            && tracks->tracks[i].codec == NGX_MEDIA_CODEC_H264)
        {
            video = NGX_MEDIA_TS_STREAM_H264;

        } else if (tracks->tracks[i].media_type == NGX_MEDIA_TYPE_VIDEO
                   && tracks->tracks[i].codec == NGX_MEDIA_CODEC_H265)
        {
            video = NGX_MEDIA_TS_STREAM_H265;

        } else if (tracks->tracks[i].media_type == NGX_MEDIA_TYPE_AUDIO
                   && tracks->tracks[i].codec == NGX_MEDIA_CODEC_AAC)
        {
            audio = NGX_MEDIA_TS_STREAM_AAC_ADTS;
        }
    }

    if (video == 0 && audio == 0) {
        return NGX_ERROR;
    }

    mux->video_stream_type = video;
    mux->audio_stream_type = audio;
    mux->tracks_ready = 1;

    return NGX_OK;
}

/*
 * The payload area a packet offers for the remaining PES bytes: exactly the
 * remaining bytes when they fit (the packet is padded with an adaptation
 * field), otherwise the full area minus the adaptation bytes needed.
 */
static size_t
ngx_media_ts_payload_capacity(size_t remain, ngx_uint_t with_pcr)
{
    size_t  min_af = with_pcr ? 8 : 0;

    if (remain + min_af < NGX_MEDIA_TS_PAYLOAD) {
        return remain;
    }

    return NGX_MEDIA_TS_PAYLOAD - min_af;
}

/* copies a PES (header followed by payload) into a packet payload area */
static void
ngx_media_ts_copy_pes(u_char *dst, size_t n, const u_char *header,
    size_t header_len, const u_char *payload, size_t payload_len, size_t off)
{
    const u_char  *src;
    size_t         src_len, take;

    while (n > 0) {

        if (off < header_len) {
            src = header + off;
            src_len = header_len - off;

        } else {
            src = payload + (off - header_len);
            src_len = payload_len - (off - header_len);
        }

        take = (src_len < n) ? src_len : n;

        ngx_memcpy(dst, src, take);

        dst += take;
        off += take;
        n -= take;
    }
}

static void
ngx_media_ts_put_ts(u_char *p, ngx_uint_t prefix, uint64_t v)
{
    p[0] = (u_char) ((prefix << 4) | (((v >> 30) & 0x07) << 1) | 1);
    p[1] = (u_char) ((v >> 22) & 0xFF);
    p[2] = (u_char) ((((v >> 15) & 0x7F) << 1) | 1);
    p[3] = (u_char) ((v >> 7) & 0xFF);
    p[4] = (u_char) (((v & 0x7F) << 1) | 1);
}

static ngx_int_t
ngx_media_ts_mux_packet(ngx_media_ts_mux_t *mux, ngx_media_ts_burst_t *burst,
    ngx_uint_t pid, ngx_uint_t *cc, ngx_uint_t pusi, size_t payload_len,
    ngx_uint_t with_pcr, int64_t pcr_base, u_char **out)
{
    u_char  *p, *fill;
    size_t   af_bytes, min_af, stuffing;
    uint64_t base;

    if (burst->capacity - (size_t) (burst->cursor
                                    - ngx_media_buf_data(burst->backing))
        < NGX_MEDIA_TS_PACKET)
    {
        return NGX_AGAIN;
    }

    min_af = with_pcr ? 8 : 0;

    if (payload_len + min_af < NGX_MEDIA_TS_PAYLOAD) {
        af_bytes = NGX_MEDIA_TS_PAYLOAD - payload_len;

    } else {
        af_bytes = min_af;
    }

    p = burst->cursor;
    burst->cursor += NGX_MEDIA_TS_PACKET;

    p[0] = 0x47;
    p[1] = (u_char) ((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    p[2] = (u_char) (pid & 0xFF);
    p[3] = (u_char) (((af_bytes ? 0x03 : 0x01) << 4) | (*cc & 0x0F));

    *cc = (*cc + 1) & 0x0F;

    if (af_bytes > 0) {
        p[4] = (u_char) (af_bytes - 1);

        if (af_bytes > 1) {
            p[5] = with_pcr ? 0x10 : 0x00;

            fill = p + 6;
            stuffing = af_bytes - 2;

            if (with_pcr) {
                base = (uint64_t) pcr_base & 0x1FFFFFFFFull;

                p[6] = (u_char) (base >> 25);
                p[7] = (u_char) (base >> 17);
                p[8] = (u_char) (base >> 9);
                p[9] = (u_char) (base >> 1);
                p[10] = (u_char) (((base & 1) << 7) | 0x7E);
                p[11] = 0x00;

                fill = p + 12;
                stuffing = af_bytes - 8;
            }

            if (stuffing > 0) {
                memset(fill, 0xFF, stuffing);
            }
        }
    }

    *out = p + 4 + af_bytes;

    mux->packets++;

    if (with_pcr) {
        mux->pcr_packets++;
    }

    return NGX_OK;
}

static void
ngx_media_ts_mux_section(ngx_media_ts_mux_t *mux, ngx_media_ts_burst_t *burst,
    ngx_uint_t pid, ngx_uint_t *cc, u_char *section, size_t len)
{
    u_char  *payload;
    size_t   take;
    ngx_uint_t rc;

    rc = ngx_media_ts_mux_packet(mux, burst, pid, cc, 1, NGX_MEDIA_TS_PAYLOAD,
                                 0, 0, &payload);

    if (rc != NGX_OK) {
        return;
    }

    payload[0] = 0x00;                     /* pointer_field */

    take = (len < NGX_MEDIA_TS_PAYLOAD - 1) ? len : NGX_MEDIA_TS_PAYLOAD - 1;

    ngx_memcpy(payload + 1, section, take);

    if (take < NGX_MEDIA_TS_PAYLOAD - 1) {
        memset(payload + 1 + take, 0xFF, NGX_MEDIA_TS_PAYLOAD - 1 - take);
    }

    mux->psi_packets++;
}

static size_t
ngx_media_ts_mux_pat(ngx_media_ts_mux_t *mux, u_char *out)
{
    u_char    *p = out;
    uint32_t   crc;

    p[0] = 0x00;                            /* table_id */
    p[1] = 0xB0;                            /* section_syntax_indicator */
    p[2] = 0x0D;                            /* section_length */
    p[3] = 0x00;
    p[4] = 0x01;                            /* transport_stream_id */
    p[5] = 0xC1;                            /* reserved, version 0, current */
    p[6] = 0x00;                            /* section_number */
    p[7] = 0x00;                            /* last_section_number */
    p[8] = 0x00;
    p[9] = 0x01;                            /* program_number 1 */
    p[10] = (u_char) (0xE0 | ((mux->conf.pmt_pid >> 8) & 0x1F));
    p[11] = (u_char) (mux->conf.pmt_pid & 0xFF);

    crc = ngx_media_ts_crc32(out, 12);

    p[12] = (u_char) (crc >> 24);
    p[13] = (u_char) (crc >> 16);
    p[14] = (u_char) (crc >> 8);
    p[15] = (u_char) crc;

    return 16;
}

static size_t
ngx_media_ts_mux_pmt(ngx_media_ts_mux_t *mux, u_char *out)
{
    u_char    *p = out;
    size_t     n = (mux->video_stream_type ? 1 : 0)
                   + (mux->audio_stream_type ? 1 : 0);
    size_t     section_length = 9 + 5 * n + 4;
    uint32_t   crc;

    p[0] = 0x02;
    p[1] = (u_char) (0xB0 | ((section_length >> 8) & 0x0F));
    p[2] = (u_char) (section_length & 0xFF);
    p[3] = 0x00;
    p[4] = 0x01;                            /* program_number */
    p[5] = 0xC1;
    p[6] = 0x00;
    p[7] = 0x00;
    p[8] = (u_char) (0xE0 | ((mux->conf.pcr_pid >> 8) & 0x1F));
    p[9] = (u_char) (mux->conf.pcr_pid & 0xFF);
    p[10] = 0xF0;                           /* program_info_length 0 */
    p[11] = 0x00;

    p += 12;

    if (mux->video_stream_type) {
        p[0] = (u_char) mux->video_stream_type;
        p[1] = (u_char) (0xE0 | ((mux->conf.video_pid >> 8) & 0x1F));
        p[2] = (u_char) (mux->conf.video_pid & 0xFF);
        p[3] = 0xF0;
        p[4] = 0x00;
        p += 5;
    }

    if (mux->audio_stream_type) {
        p[0] = (u_char) mux->audio_stream_type;
        p[1] = (u_char) (0xE0 | ((mux->conf.audio_pid >> 8) & 0x1F));
        p[2] = (u_char) (mux->conf.audio_pid & 0xFF);
        p[3] = 0xF0;
        p[4] = 0x00;
        p += 5;
    }

    crc = ngx_media_ts_crc32(out, p - out);

    p[0] = (u_char) (crc >> 24);
    p[1] = (u_char) (crc >> 16);
    p[2] = (u_char) (crc >> 8);
    p[3] = (u_char) crc;

    return (p - out) + 4;
}

ngx_int_t
ngx_media_ts_mux_burst_init(ngx_media_ts_mux_t *mux, ngx_media_ts_burst_t *burst,
    size_t capacity)
{
    if (mux == NULL || burst == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(burst, sizeof(ngx_media_ts_burst_t));

    if (capacity == 0) {
        capacity = mux->conf.default_burst_bytes;
    }

    if (capacity > mux->conf.max_burst_bytes) {
        /* one burst is a bounded unit of work */
        capacity = mux->conf.max_burst_bytes;
    }

    burst->backing = ngx_media_buf_alloc(capacity);
    if (burst->backing == NULL) {
        return NGX_ERROR;
    }

    burst->cursor = ngx_media_buf_data(burst->backing);
    burst->capacity = capacity;

    mux->bursts++;

    if (mux->tracks_ready) {
        u_char  section[1024];

        ngx_media_ts_mux_section(mux, burst, NGX_MEDIA_TS_MUX_PID_PAT,
                                 &mux->cc_pat, section,
                                 ngx_media_ts_mux_pat(mux, section));

        ngx_media_ts_mux_section(mux, burst, mux->conf.pmt_pid, &mux->cc_pmt,
                                 section, ngx_media_ts_mux_pmt(mux, section));
    }

    return NGX_OK;
}

void
ngx_media_ts_mux_burst_destroy(ngx_media_ts_burst_t *burst)
{
    if (burst == NULL) {
        return;
    }

    ngx_media_buf_unref(burst->backing);

    ngx_memzero(burst, sizeof(ngx_media_ts_burst_t));
}

size_t
ngx_media_ts_burst_size(const ngx_media_ts_burst_t *burst)
{
    if (burst == NULL || burst->backing == NULL) {
        return 0;
    }

    return (size_t) (burst->cursor - ngx_media_buf_data(burst->backing));
}

ngx_int_t
ngx_media_ts_mux_write_frame(ngx_media_ts_mux_t *mux, ngx_media_ts_burst_t *burst,
    const ngx_media_frame_t *frame, unsigned pcr)
{
    u_char                 header[24];
    const u_char          *payload;
    size_t                 payload_len, header_len, total, off, take;
    ngx_uint_t             pid, stream_id, bounded, cc, first;
    ngx_int_t              rc;
    uint64_t               packets_before, pcr_before;
    ngx_media_ts_slice_t  *slice;

    if (mux == NULL || burst == NULL || frame == NULL
        || burst->backing == NULL)
    {
        return NGX_ERROR;
    }

    if (frame->payload == NULL || !mux->tracks_ready) {
        return NGX_OK;
    }

    if (burst->nslices >= NGX_MEDIA_TS_MUX_MAX_SLICES) {
        return NGX_AGAIN;
    }

    if (frame->media_type == NGX_MEDIA_TYPE_AUDIO) {
        pid = mux->conf.audio_pid;
        stream_id = 0xC0;
        bounded = 1;
        cc = mux->cc_audio;

    } else {
        pid = mux->conf.video_pid;
        stream_id = 0xE0;
        bounded = 0;
        cc = mux->cc_video;
    }

    payload = ngx_media_buf_data(frame->payload);
    payload_len = ngx_media_buf_size(frame->payload);

    if (frame->pts == frame->dts) {
        header_len = 14;

    } else {
        header_len = 19;
    }

    header[0] = 0x00;
    header[1] = 0x00;
    header[2] = 0x01;
    header[3] = (u_char) stream_id;

    if (bounded) {
        size_t  total_len = 3 + (header_len - 9) + payload_len;

        header[4] = (u_char) ((total_len >> 8) & 0xFF);
        header[5] = (u_char) (total_len & 0xFF);

    } else {
        header[4] = 0x00;
        header[5] = 0x00;
    }

    header[6] = 0x80;
    header[7] = (frame->pts == frame->dts) ? 0x80 : 0xC0;
    header[8] = (u_char) (header_len - 9);

    ngx_media_ts_put_ts(header + 9, (frame->pts == frame->dts) ? 0x2 : 0x3,
                        (uint64_t) frame->pts);

    if (header_len == 19) {
        ngx_media_ts_put_ts(header + 14, 0x1, (uint64_t) frame->dts);
    }

    slice = &burst->slices[burst->nslices];
    slice->offset = (size_t) (burst->cursor
                              - ngx_media_buf_data(burst->backing));

    packets_before = mux->packets;
    pcr_before = mux->pcr_packets;

    total = header_len + payload_len;
    off = 0;
    first = 1;

    while (off < total) {

        size_t   remain = total - off;
        u_char  *out;

        take = ngx_media_ts_payload_capacity(remain, (first && pcr) ? 1 : 0);

        rc = ngx_media_ts_mux_packet(mux, burst, pid, &cc, first, take,
                                     (first && pcr) ? 1 : 0,
                                     (int64_t) frame->dts, &out);

        if (rc != NGX_OK) {
            /*
             * The burst is full.  Roll the burst back to the previous frame
             * boundary: no partial packets, no advanced continuity counters,
             * so the caller can start a fresh burst and retry the frame.
             */
            burst->cursor = ngx_media_buf_data(burst->backing) + slice->offset;
            mux->packets = packets_before;
            mux->pcr_packets = pcr_before;
            mux->frame_drops++;

            return NGX_AGAIN;
        }

        ngx_media_ts_copy_pes(out, take, header, header_len, payload,
                              payload_len, off);

        off += take;
        first = 0;
    }

    slice->len = (size_t) (burst->cursor
                           - ngx_media_buf_data(burst->backing))
                 - slice->offset;
    slice->pts = frame->pts;
    slice->dts = frame->dts;
    slice->media_type = frame->media_type;
    slice->codec = frame->codec;
    slice->keyframe = frame->keyframe ? 1 : 0;

    burst->nslices++;

    mux->frame_bytes += payload_len;

    if (frame->media_type == NGX_MEDIA_TYPE_AUDIO) {
        mux->cc_audio = cc;

    } else {
        mux->cc_video = cc;
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_ts_mux_burst_end(ngx_media_ts_mux_t *mux, ngx_media_ts_burst_t *burst)
{
    size_t  len;

    if (mux == NULL || burst == NULL || burst->backing == NULL) {
        return NGX_ERROR;
    }

    len = ngx_media_ts_burst_size(burst);

    if (ngx_media_buf_freeze(burst->backing, len) != NGX_OK) {
        return NGX_ERROR;
    }

    mux->bytes += len;

    return NGX_OK;
}

#include "ngx_media_ts_demux.h"

#define NGX_MEDIA_TS_PID_MASK 0x1FFF

static uint32_t ngx_media_ts_crc32(const u_char *p, size_t len);

static void ngx_media_ts_process_packet(ngx_media_ts_demux_t *demux,
    const u_char *p);
static void ngx_media_ts_psi_feed(ngx_media_ts_demux_t *demux,
    ngx_media_ts_psi_t *psi, ngx_uint_t pusi, const u_char *payload,
    size_t len);
static void ngx_media_ts_parse_section(ngx_media_ts_demux_t *demux,
    const u_char *section, size_t len);
static void ngx_media_ts_parse_pat(ngx_media_ts_demux_t *demux,
    const u_char *section, size_t len);
static void ngx_media_ts_parse_pmt(ngx_media_ts_demux_t *demux,
    const u_char *section, size_t len);
static ngx_uint_t ngx_media_ts_codec(ngx_uint_t stream_type);
static ngx_media_ts_track_t *ngx_media_ts_track_by_pid(
    ngx_media_ts_demux_t *demux, ngx_uint_t pid);
static void ngx_media_ts_track_clear(ngx_media_ts_track_t *track);
static void ngx_media_ts_tracks_build(ngx_media_ts_demux_t *demux);
static void ngx_media_ts_pes_start(ngx_media_ts_demux_t *demux,
    ngx_media_ts_track_t *track, const u_char *payload, size_t len);
static void ngx_media_ts_au_append(ngx_media_ts_demux_t *demux,
    ngx_media_ts_track_t *track, const u_char *data, size_t len);
static void ngx_media_ts_au_flush(ngx_media_ts_demux_t *demux,
    ngx_media_ts_track_t *track);
static void ngx_media_ts_audio_flush(ngx_media_ts_demux_t *demux,
    ngx_media_ts_track_t *track);
static ngx_int_t ngx_media_ts_emit(ngx_media_ts_demux_t *demux,
    ngx_media_ts_track_t *track, const u_char *data, size_t len, int64_t pts,
    int64_t dts, ngx_uint_t keyframe, ngx_uint_t config,
    ngx_uint_t payload_format);
static ngx_int_t ngx_media_ts_emit_config(ngx_media_ts_demux_t *demux,
    ngx_media_ts_track_t *track, const u_char *data, size_t len,
    ngx_uint_t payload_format);
static int64_t ngx_media_ts_pes_ts(const u_char *p);
static ngx_int_t ngx_media_ts_cc_check(ngx_uint_t *cc, ngx_uint_t *valid,
    ngx_uint_t new_cc, ngx_uint_t has_payload);

/*
 * MPEG-2 systems CRC-32: poly 0x04C11DB7, init all ones, no final xor, MSB
 * first.  A section that appends its own CRC over this function yields zero.
 */
static uint32_t
ngx_media_ts_crc32(const u_char *p, size_t len)
{
    uint32_t  crc = 0xFFFFFFFFu;
    size_t    i;
    int       b;

    for (i = 0; i < len; i++) {
        crc ^= (uint32_t) p[i] << 24;

        for (b = 0; b < 8; b++) {
            if (crc & 0x80000000u) {
                crc = (crc << 1) ^ 0x04C11DB7u;

            } else {
                crc = crc << 1;
            }
        }
    }

    return crc;
}

static ngx_uint_t
ngx_media_ts_codec(ngx_uint_t stream_type)
{
    switch (stream_type) {

    case NGX_MEDIA_TS_STREAM_H264:
        return NGX_MEDIA_CODEC_H264;

    case NGX_MEDIA_TS_STREAM_H265:
        return NGX_MEDIA_CODEC_H265;

    case NGX_MEDIA_TS_STREAM_AAC_ADTS:
        return NGX_MEDIA_CODEC_AAC;

    default:
        return NGX_MEDIA_CODEC_NONE;
    }
}

static ngx_int_t
ngx_media_ts_cc_check(ngx_uint_t *cc, ngx_uint_t *valid, ngx_uint_t new_cc,
    ngx_uint_t has_payload)
{
    if (!*valid) {
        *valid = 1;
        *cc = new_cc;
        return NGX_OK;
    }

    if (!has_payload) {
        /* adaptation-only packets may repeat the counter */
        *cc = new_cc;
        return NGX_OK;
    }

    if (new_cc != ((*cc + 1) & 0x0F)) {
        *cc = new_cc;
        return NGX_DECLINED;
    }

    *cc = new_cc;

    return NGX_OK;
}

static int64_t
ngx_media_ts_pes_ts(const u_char *p)
{
    return ((int64_t) (p[0] & 0x0E) << 29)
           | ((int64_t) p[1] << 22)
           | ((int64_t) (p[2] & 0xFE) << 14)
           | ((int64_t) p[3] << 7)
           | ((int64_t) p[4] >> 1);
}

ngx_int_t
ngx_media_ts_demux_init(ngx_media_ts_demux_t *demux,
    const ngx_media_ts_demux_conf_t *conf, const ngx_media_ts_sink_t *sink,
    void *sink_ctx, ngx_log_t *log)
{
    if (demux == NULL || sink == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(demux, sizeof(ngx_media_ts_demux_t));

    demux->max_tracks = (conf != NULL && conf->max_tracks != 0)
                        ? conf->max_tracks : NGX_MEDIA_TS_DEFAULT_TRACKS;

    demux->max_au_bytes = (conf != NULL && conf->max_au_bytes != 0)
                          ? conf->max_au_bytes : NGX_MEDIA_TS_DEFAULT_AU_BYTES;

    if (demux->max_tracks > (size_t) -1 / sizeof(ngx_media_ts_track_t)) {
        return NGX_ERROR;
    }

    demux->tracks = ngx_alloc(demux->max_tracks * sizeof(ngx_media_ts_track_t),
                              log);
    if (demux->tracks == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(demux->tracks,
                demux->max_tracks * sizeof(ngx_media_ts_track_t));

    demux->sink = *sink;
    demux->sink_ctx = sink_ctx;

    demux->pat.pid = NGX_MEDIA_TS_PID_PAT;
    demux->pmt.pid = NGX_MEDIA_TS_PID_NULL;

    demux->stats.last_pcr = 0;

    return NGX_OK;
}

void
ngx_media_ts_demux_destroy(ngx_media_ts_demux_t *demux)
{
    ngx_uint_t  i;

    if (demux == NULL || demux->tracks == NULL) {
        return;
    }

    for (i = 0; i < demux->ntracks; i++) {
        ngx_media_ts_track_clear(&demux->tracks[i]);
    }

    ngx_media_trackset_destroy(&demux->trackset);

    ngx_free(demux->tracks);

    ngx_memzero(demux, sizeof(ngx_media_ts_demux_t));
}

static void
ngx_media_ts_track_clear(ngx_media_ts_track_t *track)
{
    ngx_media_buf_unref(track->config);

    ngx_free(track->au);

    track->au = NULL;
    track->au_len = 0;
    track->au_cap = 0;
    track->au_overflow = 0;
    track->config = NULL;
    track->config_len = 0;
}

void
ngx_media_ts_demux_flush(ngx_media_ts_demux_t *demux)
{
    ngx_uint_t  i;

    if (demux == NULL || demux->tracks == NULL) {
        return;
    }

    for (i = 0; i < demux->ntracks; i++) {
        ngx_media_ts_au_flush(demux, &demux->tracks[i]);
    }
}

void
ngx_media_ts_demux_stats(const ngx_media_ts_demux_t *demux,
    ngx_media_ts_demux_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    if (demux == NULL) {
        ngx_memzero(out, sizeof(ngx_media_ts_demux_stats_t));
        return;
    }

    *out = demux->stats;
}

ngx_int_t
ngx_media_ts_demux_feed(ngx_media_ts_demux_t *demux, const u_char *data,
    size_t len)
{
    size_t  off, take, need, q;

    if (demux == NULL || demux->tracks == NULL || data == NULL) {
        return NGX_ERROR;
    }

    demux->stats.bytes += len;

    off = 0;

    if (demux->tail_len > 0) {
        need = NGX_MEDIA_TS_PACKET_SIZE - demux->tail_len;
        take = (len < need) ? len : need;

        ngx_memcpy(demux->tail + demux->tail_len, data, take);

        demux->tail_len += take;
        off += take;

        if (demux->tail_len < NGX_MEDIA_TS_PACKET_SIZE) {
            return NGX_OK;
        }

        if (demux->tail[0] == NGX_MEDIA_TS_SYNC_BYTE) {
            ngx_media_ts_process_packet(demux, demux->tail);

        } else {
            demux->stats.sync_errors++;
        }

        demux->tail_len = 0;
    }

    while (off + NGX_MEDIA_TS_PACKET_SIZE <= len) {

        if (data[off] != NGX_MEDIA_TS_SYNC_BYTE) {
            /* resync on a sync byte that is 188 bytes away from the next one */
            q = off + 1;

            while (q < len) {
                if (data[q] == NGX_MEDIA_TS_SYNC_BYTE
                    && (q + NGX_MEDIA_TS_PACKET_SIZE >= len
                        || data[q + NGX_MEDIA_TS_PACKET_SIZE]
                           == NGX_MEDIA_TS_SYNC_BYTE))
                {
                    break;
                }

                q++;
            }

            demux->stats.sync_errors++;
            demux->stats.skipped_bytes += q - off;
            off = q;

            continue;
        }

        ngx_media_ts_process_packet(demux, data + off);
        off += NGX_MEDIA_TS_PACKET_SIZE;
    }

    demux->tail_len = len - off;

    if (demux->tail_len > 0) {
        ngx_memcpy(demux->tail, data + off, demux->tail_len);
    }

    return NGX_OK;
}

static void
ngx_media_ts_process_packet(ngx_media_ts_demux_t *demux, const u_char *p)
{
    const u_char         *payload;
    size_t                payload_len;
    ngx_uint_t            pid, pusi, afc, cc, discontinuity, has_payload;
    ngx_media_ts_track_t *track;

    demux->stats.packets++;

    if (p[1] & 0x80) {
        demux->stats.transport_errors++;
        return;
    }

    pusi = (p[1] & 0x40) ? 1 : 0;
    pid = ((ngx_uint_t) (p[1] & 0x1F) << 8) | p[2];

    if (pid == NGX_MEDIA_TS_PID_NULL) {
        return;
    }

    if ((p[3] >> 6) & 0x03) {
        /* scrambled: the media core never handles scrambled payloads */
        demux->stats.transport_errors++;
        return;
    }

    afc = (p[3] >> 4) & 0x03;
    cc = p[3] & 0x0F;

    if (afc == 0) {
        demux->stats.transport_errors++;
        return;
    }

    p += 4;
    payload_len = 184;
    discontinuity = 0;

    if (afc & 0x02) {
        size_t  af_len = p[0];

        if (af_len > 183) {
            demux->stats.transport_errors++;
            return;
        }

        if (af_len > 0) {
            if (p[1] & 0x10) {
                /* PCR: 33-bit base at 90 kHz plus a 9-bit extension */
                if (af_len >= 7) {
                    demux->stats.last_pcr =
                        ((int64_t) p[2] << 25)
                        | ((int64_t) p[3] << 17)
                        | ((int64_t) p[4] << 9)
                        | ((int64_t) p[5] << 1)
                        | ((int64_t) p[6] >> 7);
                    demux->stats.has_pcr = 1;
                }
            }

            if (p[1] & 0x80) {
                discontinuity = 1;
            }
        }

        p += 1 + af_len;
        payload_len = 184 - 1 - af_len;
    }

    has_payload = (afc & 0x01) ? 1 : 0;

    payload = has_payload ? p : NULL;

    if (!has_payload) {
        payload_len = 0;
    }

    if (pid == NGX_MEDIA_TS_PID_PAT) {

        if (ngx_media_ts_cc_check(&demux->pat.cc, &demux->pat.cc_valid, cc,
                                  has_payload) == NGX_DECLINED)
        {
            demux->stats.continuity_errors++;
        }

        if (has_payload) {
            ngx_media_ts_psi_feed(demux, &demux->pat, pusi, payload,
                                  payload_len);
        }

        return;
    }

    if (demux->pat_valid && pid == demux->pmt_pid) {

        if (ngx_media_ts_cc_check(&demux->pmt.cc, &demux->pmt.cc_valid, cc,
                                  has_payload) == NGX_DECLINED)
        {
            demux->stats.continuity_errors++;
        }

        if (has_payload) {
            ngx_media_ts_psi_feed(demux, &demux->pmt, pusi, payload,
                                  payload_len);
        }

        return;
    }

    track = ngx_media_ts_track_by_pid(demux, pid);

    if (track == NULL) {
        return;
    }

    if (ngx_media_ts_cc_check(&track->cc, &track->cc_valid, cc, has_payload)
        == NGX_DECLINED && !discontinuity)
    {
        demux->stats.continuity_errors++;
    }

    if (!has_payload) {
        return;
    }

    if (pusi) {
        ngx_media_ts_au_flush(demux, track);
        ngx_media_ts_pes_start(demux, track, payload, payload_len);

    } else if (track->au_len > 0 || track->au_overflow
               || track->pes_remaining > 0)
    {
        size_t  append = payload_len;

        if (track->pes_remaining > 0 && append > track->pes_remaining) {
            append = track->pes_remaining;
        }

        ngx_media_ts_au_append(demux, track, payload, append);

        if (track->pes_remaining > 0) {
            track->pes_remaining -= append;

            if (track->pes_remaining == 0) {
                ngx_media_ts_au_flush(demux, track);
            }
        }
    }
}

static ngx_media_ts_track_t *
ngx_media_ts_track_by_pid(ngx_media_ts_demux_t *demux, ngx_uint_t pid)
{
    ngx_uint_t  i;

    for (i = 0; i < demux->ntracks; i++) {
        if (demux->tracks[i].pid == pid) {
            return &demux->tracks[i];
        }
    }

    return NULL;
}

static void
ngx_media_ts_psi_feed(ngx_media_ts_demux_t *demux, ngx_media_ts_psi_t *psi,
    ngx_uint_t pusi, const u_char *payload, size_t len)
{
    const u_char  *p;
    size_t         avail, need, take, total;
    ngx_uint_t     section_length;

    if (!pusi && !psi->assembling) {
        return;
    }

    p = payload;
    avail = len;

    if (pusi) {
        ngx_uint_t  pointer;

        if (avail == 0) {
            demux->stats.psi_errors++;
            return;
        }

        pointer = p[0];
        p++;
        avail--;

        if (pointer > avail) {
            demux->stats.psi_errors++;
            psi->assembling = 0;
            psi->len = 0;
            return;
        }

        /* pointer_field skips the tail of a previous section */
        p += pointer;
        avail -= pointer;

        psi->assembling = 0;
        psi->len = 0;
        psi->expected = 0;
    }

    while (avail > 0) {

        if (psi->len == 0) {

            if (avail < 3) {
                ngx_memcpy(psi->buf, p, avail);
                psi->len = avail;
                psi->assembling = 1;
                return;
            }

            if (p[0] == 0xFF) {
                /* stuffing bytes fill the rest of the packet */
                return;
            }

            section_length = ((ngx_uint_t) (p[1] & 0x0F) << 8) | p[2];
            total = 3 + section_length;

            if (total > NGX_MEDIA_TS_SECTION_MAX || section_length < 4) {
                demux->stats.psi_errors++;
                psi->assembling = 0;
                psi->len = 0;
                return;
            }

            psi->expected = total;
        }

        need = psi->expected - psi->len;
        take = (avail < need) ? avail : need;

        ngx_memcpy(psi->buf + psi->len, p, take);

        psi->len += take;
        p += take;
        avail -= take;

        if (psi->len == psi->expected) {
            ngx_media_ts_parse_section(demux, psi->buf, psi->len);
            psi->len = 0;
            psi->expected = 0;
            psi->assembling = 0;

        } else {
            psi->assembling = 1;
        }
    }
}

static void
ngx_media_ts_parse_section(ngx_media_ts_demux_t *demux, const u_char *section,
    size_t len)
{
    if (ngx_media_ts_crc32(section, len) != 0) {
        demux->stats.crc_errors++;
        return;
    }

    switch (section[0]) {

    case 0x00:
        ngx_media_ts_parse_pat(demux, section, len);
        break;

    case 0x02:
        ngx_media_ts_parse_pmt(demux, section, len);
        break;

    default:
        /* other tables are not needed by the media core */
        break;
    }
}

static void
ngx_media_ts_parse_pat(ngx_media_ts_demux_t *demux, const u_char *s, size_t len)
{
    size_t      end, p;
    ngx_uint_t  program, pid;

    if (len < 12) {
        demux->stats.psi_errors++;
        return;
    }

    end = len - 4;   /* CRC */

    for (p = 8; p + 4 <= end; p += 4) {

        program = ((ngx_uint_t) s[p] << 8) | s[p + 1];
        pid = ((ngx_uint_t) (s[p + 2] & 0x1F) << 8) | s[p + 3];

        if (program == 0) {
            /* network information PID */
            continue;
        }

        if (!demux->pat_valid || demux->pmt_pid != pid) {
            demux->pmt_pid = pid;
            demux->pmt.pid = pid;
            demux->pmt_valid = 0;
            demux->pmt.assembling = 0;
            demux->pmt.len = 0;
            demux->pmt.expected = 0;
        }

        demux->pat_valid = 1;

        return;
    }

    demux->stats.psi_errors++;
}

static void
ngx_media_ts_parse_pmt(ngx_media_ts_demux_t *demux, const u_char *s, size_t len)
{
    ngx_uint_t  pids[32], types[32], n = 0, i, changed = 0, codec;
    size_t      end, p, es_info_len, info_len;
    ngx_media_ts_track_t  *track;

    if (len < 16) {
        demux->stats.psi_errors++;
        return;
    }

    end = len - 4;   /* CRC */

    demux->pcr_pid = ((ngx_uint_t) (s[8] & 0x1F) << 8) | s[9];
    info_len = ((size_t) (s[10] & 0x0F) << 8) | s[11];

    p = 12 + info_len;

    if (p > end) {
        demux->stats.psi_errors++;
        return;
    }

    while (p + 5 <= end && n < 32) {

        codec = ngx_media_ts_codec(s[p]);

        if (codec == NGX_MEDIA_CODEC_NONE) {
            demux->stats.unsupported_streams++;

        } else if (n < demux->max_tracks) {
            pids[n] = ((ngx_uint_t) (s[p + 1] & 0x1F) << 8) | s[p + 2];
            types[n] = s[p];
            n++;
        }

        es_info_len = ((size_t) (s[p + 3] & 0x0F) << 8) | s[p + 4];

        p += 5 + es_info_len;
    }

    if (n != demux->ntracks) {
        changed = 1;

    } else {
        for (i = 0; i < n; i++) {
            if (demux->tracks[i].pid != pids[i]
                || demux->tracks[i].stream_type != types[i])
            {
                changed = 1;
                break;
            }
        }
    }

    if (!changed) {
        demux->pmt_valid = 1;
        return;
    }

    for (i = 0; i < demux->ntracks; i++) {
        ngx_media_ts_track_clear(&demux->tracks[i]);
        ngx_memzero(&demux->tracks[i], sizeof(ngx_media_ts_track_t));
    }

    ngx_media_trackset_destroy(&demux->trackset);
    demux->tracks_ready = 0;

    demux->ntracks = n;

    for (i = 0; i < n; i++) {
        track = &demux->tracks[i];

        track->pid = pids[i];
        track->stream_type = types[i];
        track->codec = ngx_media_ts_codec(types[i]);
        track->track_index = i;

        track->track.media_type = (track->codec == NGX_MEDIA_CODEC_AAC)
                                  ? NGX_MEDIA_TYPE_AUDIO
                                  : NGX_MEDIA_TYPE_VIDEO;
        track->track.codec = track->codec;
        track->track.payload_format = (track->codec == NGX_MEDIA_CODEC_AAC)
                                      ? NGX_MEDIA_PAYLOAD_ADTS
                                      : NGX_MEDIA_PAYLOAD_ANNEXB;
    }

    demux->pmt_valid = 1;

    ngx_media_ts_tracks_build(demux);
}

static void
ngx_media_ts_tracks_build(ngx_media_ts_demux_t *demux)
{
    ngx_uint_t  i;

    ngx_media_trackset_destroy(&demux->trackset);

    if (ngx_media_trackset_init(&demux->trackset, demux->max_tracks, NULL)
        != NGX_OK)
    {
        return;
    }

    for (i = 0; i < demux->ntracks; i++) {
        (void) ngx_media_trackset_add(&demux->trackset,
                                      &demux->tracks[i].track);
    }

    demux->tracks_ready = 1;

    if (demux->sink.tracks != NULL) {
        demux->sink.tracks(demux->sink_ctx, &demux->trackset);
    }
}

static void
ngx_media_ts_pes_start(ngx_media_ts_demux_t *demux, ngx_media_ts_track_t *track,
    const u_char *payload, size_t len)
{
    size_t      off, append;
    ngx_uint_t  flags, pes_len;

    track->has_pts = 0;
    track->has_dts = 0;
    track->pes_remaining = 0;

    if (len < 9 || payload[0] != 0x00 || payload[1] != 0x00
        || payload[2] != 0x01 || (payload[6] & 0xC0) != 0x80)
    {
        demux->stats.pes_errors++;
        return;
    }

    flags = payload[7];
    off = 9 + payload[8];

    if (off > len) {
        demux->stats.pes_errors++;
        return;
    }

    if (flags & 0x80) {
        if (len < 14) {
            demux->stats.pes_errors++;
            return;
        }

        track->pts = ngx_media_ts_pes_ts(payload + 9);
        track->has_pts = 1;
    }

    if ((flags & 0xC0) == 0xC0) {
        if (len < 19) {
            demux->stats.pes_errors++;
            return;
        }

        track->dts = ngx_media_ts_pes_ts(payload + 14);
        track->has_dts = 1;
    }

    /*
     * PES_packet_length bounds the PES payload.  Everything after it inside a
     * transport packet is stuffing and must never reach an access unit.  A
     * length of zero means the PES is unbounded and ends at the next payload
     * unit start (typical for video).
     */
    pes_len = ((ngx_uint_t) payload[4] << 8) | payload[5];

    append = len - off;

    if (pes_len > 0) {
        if (pes_len < 3 + (ngx_uint_t) payload[8]) {
            demux->stats.pes_errors++;
            return;
        }

        track->pes_remaining = pes_len - 3 - payload[8];

        if (append > track->pes_remaining) {
            append = track->pes_remaining;
        }

        track->pes_remaining -= append;
    }

    ngx_media_ts_au_append(demux, track, payload + off, append);

    if (pes_len > 0 && track->pes_remaining == 0) {
        /* the PES is complete: for audio that is a whole set of frames */
        ngx_media_ts_au_flush(demux, track);
    }
}

static void
ngx_media_ts_au_append(ngx_media_ts_demux_t *demux, ngx_media_ts_track_t *track,
    const u_char *data, size_t len)
{
    size_t  need, cap;
    u_char *p;

    if (len == 0) {
        return;
    }

    need = track->au_len + len;

    if (need > demux->max_au_bytes) {
        /* drop the partial unit: a truncated access unit must not be emitted */
        demux->stats.au_overflows++;
        track->au_len = 0;
        track->au_overflow = 1;
        return;
    }

    if (need > track->au_cap) {
        cap = track->au_cap ? track->au_cap : 8192;

        while (cap < need) {
            cap <<= 1;
        }

        if (cap > demux->max_au_bytes) {
            cap = demux->max_au_bytes;
        }

        p = ngx_alloc(cap, NULL);
        if (p == NULL) {
            demux->stats.au_overflows++;
            track->au_len = 0;
            track->au_overflow = 1;
            return;
        }

        if (track->au_len > 0) {
            ngx_memcpy(p, track->au, track->au_len);
        }

        ngx_free(track->au);

        track->au = p;
        track->au_cap = cap;
    }

    ngx_memcpy(track->au + track->au_len, data, len);
    track->au_len = need;
}

static ngx_int_t
ngx_media_ts_emit(ngx_media_ts_demux_t *demux, ngx_media_ts_track_t *track,
    const u_char *data, size_t len, int64_t pts, int64_t dts,
    ngx_uint_t keyframe, ngx_uint_t config, ngx_uint_t payload_format)
{
    ngx_media_buf_t    *buf;
    ngx_media_frame_t   frame;

    if (len == 0) {
        return NGX_OK;
    }

    buf = ngx_media_buf_alloc(len);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(ngx_media_buf_data(buf), data, len);
    (void) ngx_media_buf_freeze(buf, len);

    ngx_media_frame_init(&frame);

    frame.media_type = track->track.media_type;
    frame.codec = track->codec;
    frame.payload_format = payload_format;
    frame.track_index = track->track_index;
    frame.pts = pts;
    frame.dts = dts;
    frame.keyframe = keyframe ? 1 : 0;
    frame.config = config ? 1 : 0;

    ngx_media_frame_adopt(&frame, buf);

    if (demux->sink.frame != NULL) {
        (void) demux->sink.frame(demux->sink_ctx, &frame);
    }

    ngx_media_frame_release(&frame);

    if (config) {
        demux->stats.config_frames_out++;

    } else {
        demux->stats.frames_out++;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_media_ts_emit_config(ngx_media_ts_demux_t *demux,
    ngx_media_ts_track_t *track, const u_char *data, size_t len,
    ngx_uint_t payload_format)
{
    ngx_media_buf_t  *buf;

    if (len == 0) {
        return NGX_OK;
    }

    if (track->config != NULL && track->config_len == len
        && ngx_memcmp(ngx_media_buf_data(track->config), data, len) == 0)
    {
        return NGX_OK;
    }

    buf = ngx_media_buf_alloc(len);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(ngx_media_buf_data(buf), data, len);
    (void) ngx_media_buf_freeze(buf, len);

    ngx_media_buf_unref(track->config);

    track->config = buf;              /* the demuxer owns one reference */
    track->config_len = len;
    track->track.config = buf;        /* alias; the trackset holds its own */

    if (track->track_index < demux->trackset.count) {
        ngx_media_buf_unref(demux->trackset.tracks[track->track_index].config);
        demux->trackset.tracks[track->track_index].config =
            ngx_media_buf_ref(buf);
    }

    (void) ngx_media_ts_emit(demux, track, data, len, track->pts, track->dts,
                             1, 1, payload_format);

    if (demux->sink.tracks != NULL && demux->tracks_ready) {
        demux->sink.tracks(demux->sink_ctx, &demux->trackset);
    }

    return NGX_OK;
}

static void
ngx_media_ts_au_flush(ngx_media_ts_demux_t *demux, ngx_media_ts_track_t *track)
{
    ngx_media_nal_iter_t  it;
    ngx_media_nal_t       nal;
    ngx_uint_t            type, keyframe = 0, vcl = 0, have_config = 0;
    u_char               *cfg = NULL;
    size_t                cfg_len = 0, cfg_cap = 0, need;
    int64_t               pts, dts;

    if (track->au_len == 0) {
        track->au_overflow = 0;
        track->has_pts = 0;
        track->has_dts = 0;
        return;
    }

    if (track->au_overflow) {
        /* the access unit was not captured completely: never emit it */
        track->au_len = 0;
        track->au_overflow = 0;
        track->has_pts = 0;
        track->has_dts = 0;
        return;
    }

    if (track->codec == NGX_MEDIA_CODEC_AAC) {
        ngx_media_ts_audio_flush(demux, track);
        return;
    }

    ngx_media_nal_iter_init(&it, track->au, track->au_len);

    while (ngx_media_nal_iter_next(&it, &nal)) {

        type = ngx_media_nal_type(track->codec, nal.data, nal.len);

        if (ngx_media_nal_is_config(track->codec, type)) {
            need = cfg_len + 4 + nal.len;

            if (need > cfg_cap) {
                size_t  cap = cfg_cap ? cfg_cap : 256;
                u_char *np;

                while (cap < need) {
                    cap <<= 1;
                }

                np = ngx_alloc(cap, NULL);
                if (np == NULL) {
                    break;
                }

                if (cfg_len > 0) {
                    ngx_memcpy(np, cfg, cfg_len);
                }

                ngx_free(cfg);

                cfg = np;
                cfg_cap = cap;
            }

            cfg[cfg_len++] = 0x00;
            cfg[cfg_len++] = 0x00;
            cfg[cfg_len++] = 0x00;
            cfg[cfg_len++] = 0x01;

            ngx_memcpy(cfg + cfg_len, nal.data, nal.len);
            cfg_len += nal.len;

            have_config = 1;
        }

        if (ngx_media_nal_is_vcl(track->codec, type)) {
            vcl = 1;
        }

        if (ngx_media_nal_is_keyframe(track->codec, type)) {
            keyframe = 1;
        }
    }

    if (have_config) {
        (void) ngx_media_ts_emit_config(demux, track, cfg, cfg_len,
                                        NGX_MEDIA_PAYLOAD_ANNEXB);
    }

    ngx_free(cfg);

    if (vcl) {
        pts = track->has_pts ? track->pts : track->last_pts;
        dts = track->has_dts ? track->dts : pts;

        (void) ngx_media_ts_emit(demux, track, track->au, track->au_len, pts,
                                 dts, keyframe, 0, NGX_MEDIA_PAYLOAD_ANNEXB);

        track->last_pts = pts;
    }

    track->au_len = 0;
    track->has_pts = 0;
    track->has_dts = 0;
}

static void
ngx_media_ts_audio_flush(ngx_media_ts_demux_t *demux, ngx_media_ts_track_t *track)
{
    const u_char    *p;
    size_t           left;
    ngx_media_adts_t adts;
    u_char           asc[2];
    size_t           asc_len;
    int64_t          pts;
    ngx_uint_t       meta_changed = 0;

    p = track->au;
    left = track->au_len;

    if (track->has_pts) {
        track->last_pts = track->pts;
    }

    while (left >= NGX_MEDIA_ADTS_HEADER_MIN) {

        if (ngx_media_adts_parse(p, left, &adts) != NGX_OK) {
            demux->stats.pes_errors++;
            break;
        }

        if (adts.frame_len > left) {
            /* a partial ADTS frame: the PES was truncated */
            demux->stats.pes_errors++;
            break;
        }

        if (ngx_media_adts_audio_specific_config(&adts, asc, sizeof(asc),
                                                 &asc_len) == NGX_OK)
        {
            (void) ngx_media_ts_emit_config(demux, track, asc, asc_len,
                                            NGX_MEDIA_PAYLOAD_RAW);
        }

        if (track->track.sample_rate != adts.sample_rate
            || track->track.channels != adts.channels
            || track->track.profile != adts.object_type)
        {
            track->track.sample_rate = adts.sample_rate;
            track->track.channels = adts.channels;
            track->track.profile = adts.object_type;
            meta_changed = 1;
        }

        pts = track->last_pts;

        (void) ngx_media_ts_emit(demux, track, p, adts.frame_len, pts, pts, 1,
                                 0, NGX_MEDIA_PAYLOAD_ADTS);

        track->last_pts = pts
                          + (int64_t) (1024 * 90000 / adts.sample_rate);

        p += adts.frame_len;
        left -= adts.frame_len;
    }

    if (meta_changed && demux->tracks_ready && demux->sink.tracks != NULL) {
        ngx_uint_t  i;

        for (i = 0; i < demux->ntracks; i++) {
            if (demux->tracks[i].track_index < demux->trackset.count) {
                demux->trackset.tracks[demux->tracks[i].track_index] =
                    demux->tracks[i].track;
            }
        }

        demux->sink.tracks(demux->sink_ctx, &demux->trackset);
    }

    track->au_len = 0;
    track->has_pts = 0;
    track->has_dts = 0;
}

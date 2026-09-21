#ifndef NGX_MEDIA_AAC_H
#define NGX_MEDIA_AAC_H

#include "ngx_media_platform.h"

/*
 * AAC framing for MPEG-TS (ADTS, stream type 0x0F).
 *
 * ADTS frames are self-delimiting: the header carries the frame length, the
 * sampling frequency index, the channel configuration and the profile, which
 * is everything the media core needs to slice a PES payload into audio frames
 * and to synthesize the AudioSpecificConfig that the track contract carries.
 */

#define NGX_MEDIA_ADTS_HEADER_MIN 7
#define NGX_MEDIA_ADTS_HEADER_MAX 9

typedef struct {
    ngx_uint_t  frame_len;    /* whole ADTS frame, header included */
    ngx_uint_t  header_len;   /* 7, or 9 with a CRC */
    ngx_uint_t  object_type;  /* audio object type (profile + 1) */
    ngx_uint_t  sample_rate;  /* Hz */
    ngx_uint_t  channels;
} ngx_media_adts_t;

/* returns NGX_OK when a complete ADTS header was parsed */
ngx_int_t ngx_media_adts_parse(const u_char *p, size_t len,
    ngx_media_adts_t *adts);

/* writes the 2-byte AudioSpecificConfig for the parsed header */
ngx_int_t ngx_media_adts_audio_specific_config(const ngx_media_adts_t *adts,
    u_char *buf, size_t cap, size_t *out_len);

/*
 * Writes a 7-byte ADTS header in front of a raw AAC frame: the RTMP adapter
 * turns FLV raw AAC payloads into the ADTS framing the core carries, which is
 * the inverse of what the TS demuxer does.
 */
ngx_int_t ngx_media_adts_write(const ngx_media_adts_t *adts, u_char *buf,
    size_t cap, size_t *out_len);

#endif /* NGX_MEDIA_AAC_H */

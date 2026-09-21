#ifndef NGX_MEDIA_RTMP_ADAPTER_H
#define NGX_MEDIA_RTMP_ADAPTER_H

#include "ngx_media.h"
#include "ngx_media_rtmp_wire.h"

/*
 * RTMP adapter (goal doc 12): converts between the RTMP/FLV framing and the
 * protocol-neutral encoded media the core carries.
 *
 * The core representation is the MPEG-TS one used by the SRT ingest path:
 * H.264/H.265 payloads are Annex B and AAC payloads carry ADTS headers, with
 * the codec configuration (avcC / AudioSpecificConfig) delivered once as a
 * config frame.  Publishers therefore convert on the way in and players
 * convert on the way out, and every consumer downstream (TS muxer, HLS,
 * recording, SRT output) stays framing-agnostic.
 */

#define NGX_MEDIA_RTMP_TIMESCALE          90   /* ms to 90 kHz */

/* FLV video frame types */
#define NGX_MEDIA_RTMP_FRAME_KEYFRAME     1
#define NGX_MEDIA_RTMP_FRAME_INTER        2

/* FLV video codec ids */
#define NGX_MEDIA_RTMP_CODEC_AVC          7

/* FLV audio format (AAC) */
#define NGX_MEDIA_RTMP_SOUND_AAC          10

/* AVC packet types */
#define NGX_MEDIA_RTMP_AVC_SEQUENCE       0
#define NGX_MEDIA_RTMP_AVC_NALU           1
#define NGX_MEDIA_RTMP_AVC_END_SEQUENCE   2

/* AAC packet types */
#define NGX_MEDIA_RTMP_AAC_SEQUENCE       0
#define NGX_MEDIA_RTMP_AAC_RAW            1

#define NGX_MEDIA_RTMP_MAX_PARAM_SETS     4

/* parsed AVCDecoderConfigurationRecord; the strings alias the input */
typedef struct {
    ngx_uint_t  version;
    ngx_uint_t  profile;
    ngx_uint_t  compatibility;
    ngx_uint_t  level;
    ngx_uint_t  nal_length_size;      /* bytes, 1..4 */

    ngx_str_t   sps[NGX_MEDIA_RTMP_MAX_PARAM_SETS];
    ngx_uint_t  nsps;
    ngx_str_t   pps[NGX_MEDIA_RTMP_MAX_PARAM_SETS];
    ngx_uint_t  npps;
} ngx_media_rtmp_avcc_t;

/* parsed AudioSpecificConfig fields */
typedef struct {
    ngx_uint_t  object_type;
    ngx_uint_t  sample_rate_index;
    ngx_uint_t  sample_rate;
    ngx_uint_t  channels;
} ngx_media_rtmp_asc_t;

ngx_int_t ngx_media_rtmp_avcc_parse(const u_char *data, size_t len,
    ngx_media_rtmp_avcc_t *out);
ngx_int_t ngx_media_rtmp_avcc_build(const ngx_media_rtmp_avcc_t *avcc,
    u_char *dst, size_t capacity, size_t *out_len);

/* parameter sets as one Annex B blob (start codes), as the TS path carries */
size_t ngx_media_rtmp_avcc_annexb_size(const ngx_media_rtmp_avcc_t *avcc);
ngx_int_t ngx_media_rtmp_avcc_to_annexb(const ngx_media_rtmp_avcc_t *avcc,
    u_char *dst, size_t capacity, size_t *out_len);

ngx_int_t ngx_media_rtmp_asc_parse(const u_char *data, size_t len,
    ngx_media_rtmp_asc_t *out);
ngx_int_t ngx_media_rtmp_asc_build(const ngx_media_rtmp_asc_t *asc,
    u_char *dst, size_t capacity, size_t *out_len);

/*
 * AVCC (length prefixed NAL units) to Annex B and back.  The caller sizes the
 * destination with ngx_media_rtmp_avcc_to_annexb_size() style helpers:
 * Annex B adds one start code per NAL unit, AVCC removes one.
 */
ngx_int_t ngx_media_rtmp_avcc_payload_to_annexb(const u_char *src, size_t len,
    ngx_uint_t nal_length_size, u_char *dst, size_t capacity, size_t *out_len);
ngx_int_t ngx_media_rtmp_annexb_payload_to_avcc(const u_char *src, size_t len,
    ngx_uint_t nal_length_size, u_char *dst, size_t capacity, size_t *out_len);

/* FLV message bodies */
ngx_int_t ngx_media_rtmp_flv_video(u_char *dst, size_t capacity,
    size_t *out_len, ngx_uint_t keyframe, int32_t composition_time,
    const u_char *data, size_t len);
ngx_int_t ngx_media_rtmp_flv_audio(u_char *dst, size_t capacity,
    size_t *out_len, ngx_uint_t aac_sequence, const u_char *data, size_t len);

/* publisher (ingest) state */
typedef struct {
    ngx_media_trackset_t  tracks;
    ngx_uint_t            video_track;      /* index in tracks */
    ngx_uint_t            audio_track;
    unsigned              have_video:1;
    unsigned              have_audio:1;

    ngx_media_rtmp_avcc_t avcc;
    ngx_uint_t            have_avcc;

    ngx_media_rtmp_asc_t  asc;
    ngx_uint_t            have_asc;

    int64_t               last_dts;      /* for config frames */

    ngx_uint_t            frames;
    ngx_uint_t            configs;
    ngx_uint_t            errors;
    ngx_uint_t            skipped;
} ngx_media_rtmp_publisher_t;

typedef ngx_int_t (*ngx_media_rtmp_frame_pt)(void *ctx,
    const ngx_media_frame_t *frame);
typedef ngx_int_t (*ngx_media_rtmp_tracks_pt)(void *ctx,
    const ngx_media_trackset_t *tracks);

ngx_int_t ngx_media_rtmp_publisher_init(ngx_media_rtmp_publisher_t *pub,
    ngx_log_t *log);
void ngx_media_rtmp_publisher_destroy(ngx_media_rtmp_publisher_t *pub);

/*
 * Feeds one RTMP audio or video message.  Config messages produce a config
 * frame and a track notification; media messages produce one frame.  A
 * message that cannot be converted (unsupported codec, missing config) is
 * counted and skipped, never fatal.
 */
ngx_int_t ngx_media_rtmp_publisher_feed(ngx_media_rtmp_publisher_t *pub,
    ngx_uint_t type, uint32_t timestamp_ms, const u_char *data, size_t len,
    ngx_media_rtmp_frame_pt frame_cb, ngx_media_rtmp_tracks_pt tracks_cb,
    void *ctx);

/* --- player (output) side ------------------------------------------------ */

/*
 * One converted program message.  The payload is the FLV body (flags,
 * composition time and AVCC framed media) built once per program and shared
 * by every RTMP player; only chunk headers are per connection (goal 12.1).
 */
typedef struct {
    ngx_media_buf_t  *payload;
    ngx_uint_t        type;          /* NGX_MEDIA_RTMP_MSG_AUDIO / VIDEO */
    ngx_uint_t        track;
    uint32_t          timestamp;     /* milliseconds */
    uint64_t          sequence;
    unsigned          keyframe:1;
    unsigned          config:1;
} ngx_media_rtmp_media_t;

#define NGX_MEDIA_RTMP_FANOUT_UNITS  1024

typedef struct {
    ngx_media_rtmp_media_t  units[NGX_MEDIA_RTMP_FANOUT_UNITS];
    ngx_uint_t              capacity;
    uint64_t                head;        /* next sequence to write */
    uint64_t                tail;        /* oldest retained sequence */
    uint64_t                dropped;
    uint64_t                converted;
    size_t                  bytes;
    size_t                  max_bytes;
} ngx_media_rtmp_fanout_t;

void ngx_media_rtmp_fanout_init(ngx_media_rtmp_fanout_t *fan,
    ngx_uint_t capacity, size_t max_bytes);
void ngx_media_rtmp_fanout_destroy(ngx_media_rtmp_fanout_t *fan);

/* takes a reference on the payload */
ngx_int_t ngx_media_rtmp_fanout_push(ngx_media_rtmp_fanout_t *fan,
    ngx_uint_t type, ngx_uint_t track, uint32_t timestamp,
    ngx_media_buf_t *payload, ngx_uint_t keyframe, ngx_uint_t config);

/* the oldest retained unit whose sequence is >= from */
const ngx_media_rtmp_media_t *ngx_media_rtmp_fanout_next(
    const ngx_media_rtmp_fanout_t *fan, uint64_t from);

uint64_t ngx_media_rtmp_fanout_head(const ngx_media_rtmp_fanout_t *fan);

#endif /* NGX_MEDIA_RTMP_ADAPTER_H */

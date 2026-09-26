#ifndef NGX_MEDIA_RTMP_WIRE_H
#define NGX_MEDIA_RTMP_WIRE_H

#include "ngx_media.h"
#include "ngx_media_buffer.h"

/*
 * RTMP wire layer (goal doc 12): handshake, chunk streams and AMF0.
 *
 * Deliberately free of nginx event/connection types so that the protocol can
 * be unit tested against synthetic byte streams.  The layer owns no sockets:
 * the module feeds it bytes and sends the packets it builds.
 */

#define NGX_MEDIA_RTMP_VERSION          3
#define NGX_MEDIA_RTMP_HANDSHAKE_SIZE   1536

#define NGX_MEDIA_RTMP_DEFAULT_CHUNK    128
#define NGX_MEDIA_RTMP_MAX_CHUNK        65536
#define NGX_MEDIA_RTMP_MAX_MESSAGE      (16 * 1024 * 1024)
#define NGX_MEDIA_RTMP_MAX_CSID         64

/* message types */
#define NGX_MEDIA_RTMP_MSG_CHUNK_SIZE    1
#define NGX_MEDIA_RTMP_MSG_ABORT         2
#define NGX_MEDIA_RTMP_MSG_ACK           3
#define NGX_MEDIA_RTMP_MSG_USER_CONTROL  4
#define NGX_MEDIA_RTMP_MSG_WINDOW_ACK    5
#define NGX_MEDIA_RTMP_MSG_PEER_BANDWIDTH 6
#define NGX_MEDIA_RTMP_MSG_AUDIO         8
#define NGX_MEDIA_RTMP_MSG_VIDEO         9
#define NGX_MEDIA_RTMP_MSG_DATA_AMF0     18
#define NGX_MEDIA_RTMP_MSG_COMMAND_AMF0  20

/* user control events */
#define NGX_MEDIA_RTMP_EVENT_STREAM_BEGIN   0
#define NGX_MEDIA_RTMP_EVENT_STREAM_EOF     1
#define NGX_MEDIA_RTMP_EVENT_STREAM_DRY     2
#define NGX_MEDIA_RTMP_EVENT_BUFFER_LENGTH  3
#define NGX_MEDIA_RTMP_EVENT_STREAM_IS_RECORDED 4
#define NGX_MEDIA_RTMP_EVENT_PING_REQUEST   6
#define NGX_MEDIA_RTMP_EVENT_PING_RESPONSE  7

/* --- sha256 / hmac ------------------------------------------------------- */

typedef struct {
    uint32_t  state[8];
    uint64_t  bits;
    size_t    have;
    u_char    block[64];
} ngx_media_sha256_t;

void ngx_media_sha256_init(ngx_media_sha256_t *ctx);
void ngx_media_sha256_update(ngx_media_sha256_t *ctx, const void *data,
    size_t len);
void ngx_media_sha256_final(ngx_media_sha256_t *ctx, u_char out[32]);
void ngx_media_hmac_sha256(const u_char *key, size_t key_len,
    const u_char *data, size_t len, u_char out[32]);

/* --- handshake ----------------------------------------------------------- */

typedef struct {
    ngx_uint_t  state;      /* 0 = C0C1, 1 = C2, 2 = done */
    ngx_uint_t  complex;    /* C1 asked for the digest handshake */
    size_t      have;
    u_char      c1[NGX_MEDIA_RTMP_HANDSHAKE_SIZE];
    u_char      c2[NGX_MEDIA_RTMP_HANDSHAKE_SIZE];
    u_char      out[1 + 2 * NGX_MEDIA_RTMP_HANDSHAKE_SIZE];
    size_t      out_len;
} ngx_media_rtmp_handshake_t;

void ngx_media_rtmp_handshake_init(ngx_media_rtmp_handshake_t *hs);
ngx_int_t ngx_media_rtmp_handshake_feed(ngx_media_rtmp_handshake_t *hs,
    const u_char *data, size_t len, size_t *consumed);

/* --- chunk reader -------------------------------------------------------- */

typedef struct {
    ngx_uint_t        csid;
    ngx_uint_t        type;
    ngx_uint_t        stream_id;
    uint32_t          timestamp;      /* absolute, milliseconds */
    uint32_t          delta;
    u_char            fmt;
    size_t            length;
    size_t            received;
    ngx_media_buf_t  *payload;
    unsigned          used:1;
    unsigned          extended:1;
} ngx_media_rtmp_cs_t;

typedef ngx_int_t (*ngx_media_rtmp_message_pt)(void *ctx, ngx_uint_t type,
    ngx_uint_t stream_id, uint32_t timestamp, ngx_media_buf_t *payload);

typedef struct {
    ngx_uint_t           chunk_size;      /* chunk size the peer uses */
    ngx_uint_t           max_message;
    uint64_t             bytes_in;
    ngx_media_rtmp_cs_t  chunks[NGX_MEDIA_RTMP_MAX_CSID];

    /*
     * A chunk may be split across reads.  Only one chunk can be partial at a
     * time (chunks interleave, payloads do not), so the reader remembers the
     * csid whose chunk still owes payload bytes.
     */
    ngx_uint_t           pending_csid;
    size_t               pending_remaining;

    ngx_uint_t           messages;
    ngx_uint_t           errors;
} ngx_media_rtmp_reader_t;

/* init() is for fresh memory; reset() also releases a partially received
 * message and is the safe way to start over on a reader that was used */
void ngx_media_rtmp_reader_init(ngx_media_rtmp_reader_t *r);
void ngx_media_rtmp_reader_reset(ngx_media_rtmp_reader_t *r);

/*
 * Consumes as much of `data` as forms complete messages.  NGX_ERROR on a
 * malformed stream (the caller drops the connection); *consumed reports the
 * bytes taken.  Every complete message is handed to the callback, which may
 * take its own reference on the payload.
 */
ngx_int_t ngx_media_rtmp_reader_feed(ngx_media_rtmp_reader_t *r,
    const u_char *data, size_t len, size_t *consumed,
    ngx_media_rtmp_message_pt cb, void *ctx);

/* --- chunk writer -------------------------------------------------------- */

/* headers and payload slices alternate: two entries per chunk */
#define NGX_MEDIA_RTMP_MAX_OUT_PARTS    512
#define NGX_MEDIA_RTMP_MAX_OUT_CSID     8

/*
 * One outgoing message as a scatter/gather list: chunk headers are owned by
 * the packet, media bytes are references to the shared payload buffer, so no
 * receiver copies payload bytes (goal doc 12.1).
 */
typedef struct {
    const u_char  *data;
    size_t         len;
} ngx_media_rtmp_part_t;

typedef struct {
    ngx_media_rtmp_part_t  parts[NGX_MEDIA_RTMP_MAX_OUT_PARTS];
    ngx_uint_t             nparts;
    u_char                *head;
    size_t                 head_len;
    ngx_media_buf_t       *payload;
} ngx_media_rtmp_packet_t;

typedef struct {
    ngx_uint_t  chunk_size;
    ngx_uint_t  max_message;
    uint32_t    timestamp[NGX_MEDIA_RTMP_MAX_OUT_CSID];
    ngx_uint_t  stream_id[NGX_MEDIA_RTMP_MAX_OUT_CSID];
    unsigned    used[NGX_MEDIA_RTMP_MAX_OUT_CSID];
} ngx_media_rtmp_writer_t;

void ngx_media_rtmp_writer_init(ngx_media_rtmp_writer_t *w,
    ngx_uint_t chunk_size, ngx_uint_t max_message);
void ngx_media_rtmp_packet_init(ngx_media_rtmp_packet_t *pkt);
void ngx_media_rtmp_packet_destroy(ngx_media_rtmp_packet_t *pkt);

/* takes a reference on the payload (offset/len select the shared range) */
ngx_int_t ngx_media_rtmp_writer_message(ngx_media_rtmp_writer_t *w,
    ngx_media_rtmp_packet_t *pkt, ngx_uint_t csid, ngx_uint_t type,
    ngx_uint_t stream_id, uint32_t timestamp, ngx_media_buf_t *payload,
    size_t offset, size_t len);

/*
 * What one message costs a queue that has to reserve room for it before it is
 * built: a chain slot per part (two per chunk) and a payload reference per
 * chunk.  A queue that reserves one of each per *message* undercounts a
 * multi-chunk message, which is how a >256 KiB keyframe used to run past the
 * fixed in_flight ring.  Both the player and the destination reserve from
 * this, and it is derived from the writer they will hand the message to, so
 * the reservation cannot drift from what the fill loop actually appends.
 */
typedef struct {
    ngx_uint_t  parts;   /* chain slots the message will occupy */
    ngx_uint_t  refs;    /* payload references it will hold */
} ngx_media_rtmp_footprint_t;

/* NGX_AGAIN when the message is past what this writer can emit at all */
ngx_int_t ngx_media_rtmp_message_footprint(const ngx_media_rtmp_writer_t *w,
    size_t len, ngx_media_rtmp_footprint_t *out);

/* --- AMF0 ---------------------------------------------------------------- */

#define NGX_MEDIA_AMF_NUMBER      0x00
#define NGX_MEDIA_AMF_BOOLEAN     0x01
#define NGX_MEDIA_AMF_STRING      0x02
#define NGX_MEDIA_AMF_OBJECT      0x03
#define NGX_MEDIA_AMF_NULL        0x05
#define NGX_MEDIA_AMF_UNDEFINED   0x06
#define NGX_MEDIA_AMF_ECMA_ARRAY  0x08
#define NGX_MEDIA_AMF_OBJECT_END  0x09
#define NGX_MEDIA_AMF_STRICT_ARRAY 0x0A

#define NGX_MEDIA_AMF_MAX_MEMBERS 24

/*
 * Nested aggregates are read by recursing one frame per nesting level and
 * every frame carries a full ngx_media_amf_value_t, so the peer's input
 * decides how much stack the parse costs.  Real AMF0 commands nest a handful
 * of levels; input deeper than this is rejected instead of being allowed to
 * consume the worker's stack.  The fuzz generators build inputs on both sides
 * of this boundary.
 */
#define NGX_MEDIA_AMF_MAX_DEPTH 1024

/*
 * A bounded AMF0 value.  String member data points into the message payload
 * that is alive for the duration of the callback; type NGX_MEDIA_AMF_OBJECT
 * members are recorded as OBJECT with an empty string.
 */
typedef struct {
    ngx_str_t   name;
    ngx_uint_t  type;
    double      number;
    ngx_uint_t  boolean;
    ngx_str_t   string;
} ngx_media_amf_member_t;

typedef struct {
    ngx_uint_t             type;
    double                 number;
    ngx_uint_t             boolean;
    ngx_str_t              string;
    ngx_uint_t             count;
    ngx_media_amf_member_t members[NGX_MEDIA_AMF_MAX_MEMBERS];
} ngx_media_amf_value_t;

/* NGX_OK / NGX_ERROR (malformed) / NGX_AGAIN (truncated input) */
ngx_int_t ngx_media_amf_read(const u_char *data, size_t len,
    ngx_media_amf_value_t *out, size_t *consumed);

const ngx_str_t *ngx_media_amf_member(const ngx_media_amf_value_t *value,
    const char *name);
ngx_int_t ngx_media_amf_member_number(const ngx_media_amf_value_t *value,
    const char *name, double *out);

typedef struct {
    u_char  *data;
    size_t   capacity;
    size_t   len;
    ngx_uint_t objects;      /* open object/array nesting */
} ngx_media_amf_writer_t;

void ngx_media_amf_writer_init(ngx_media_amf_writer_t *w, u_char *data,
    size_t capacity);
ngx_int_t ngx_media_amf_put_number(ngx_media_amf_writer_t *w, double v);
ngx_int_t ngx_media_amf_put_boolean(ngx_media_amf_writer_t *w, ngx_uint_t v);
ngx_int_t ngx_media_amf_put_string(ngx_media_amf_writer_t *w,
    const u_char *s, size_t len);
ngx_int_t ngx_media_amf_put_null(ngx_media_amf_writer_t *w);
ngx_int_t ngx_media_amf_begin_object(ngx_media_amf_writer_t *w,
    ngx_uint_t ecma, ngx_uint_t hint);
ngx_int_t ngx_media_amf_end_object(ngx_media_amf_writer_t *w);
ngx_int_t ngx_media_amf_put_member_string(ngx_media_amf_writer_t *w,
    const char *name, const u_char *s, size_t len);
ngx_int_t ngx_media_amf_put_member_number(ngx_media_amf_writer_t *w,
    const char *name, double v);
ngx_int_t ngx_media_amf_put_member_boolean(ngx_media_amf_writer_t *w,
    const char *name, ngx_uint_t v);

#endif /* NGX_MEDIA_RTMP_WIRE_H */

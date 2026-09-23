/*
 * nginx-media RTMP destination backend (goal doc 12, 16).
 *
 * A destination is an RTMP client: it connects out to the remote server,
 * performs the RTMP handshake, runs the publish command sequence and then
 * carries the program's media to it.
 *
 * Media is taken from the same shared FLV preparation the RTMP players read
 * (ngx_media_runtime_prepare): the program is converted to FLV once per
 * stream, and every receiver -- player or destination -- adds only its own
 * chunk headers.  A destination therefore costs one reference per message,
 * never a copy (goal doc 12.1).
 *
 * Bounded by construction.  The socket is non-blocking and the outbound
 * queue is a fixed number of messages; once it is full the destination stops
 * taking units from the fanout and lets it evict ahead, exactly the way the
 * SRT destination drops under backpressure.  The reader has a fixed buffer,
 * the connection pool is reset whenever the queue drains, and every stage of
 * the protocol has a deadline after which the connection is dropped and
 * retried, so a stalled remote server stalls nothing but its own destination.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include "ngx_media_destination.h"
#include "ngx_media_runtime.h"
#include "ngx_media_rtmp_adapter.h"
#include "ngx_media_rtmp_destination.h"
#include "ngx_media_rtmp_wire.h"

#define NGX_MEDIA_RTMP_DEST_MAX          8
#define NGX_MEDIA_RTMP_DEST_CHUNK        4096
#define NGX_MEDIA_RTMP_DEST_READ_BUFFER  16384
#define NGX_MEDIA_RTMP_DEST_MAX_QUEUE    128
#define NGX_MEDIA_RTMP_DEST_MAX_BYTES    (512 * 1024)
#define NGX_MEDIA_RTMP_DEST_MAX_BATCH    32
#define NGX_MEDIA_RTMP_DEST_TICK         40
#define NGX_MEDIA_RTMP_DEST_RETRY        1000
#define NGX_MEDIA_RTMP_DEST_STAGE        5000
#define NGX_MEDIA_RTMP_DEST_POOL_SIZE    (32 * 1024)
#define NGX_MEDIA_RTMP_DEST_AMF_BUFFER   2048

/* one outbound message stream, as createStream() returned it */
#define NGX_MEDIA_RTMP_DEST_STREAM_ID    1

/* chunk stream ids: commands, audio, video and data never share one */
#define NGX_MEDIA_RTMP_DEST_CSID_CONTROL 2
#define NGX_MEDIA_RTMP_DEST_CSID_COMMAND 3
#define NGX_MEDIA_RTMP_DEST_CSID_AUDIO   4
#define NGX_MEDIA_RTMP_DEST_CSID_VIDEO   5
#define NGX_MEDIA_RTMP_DEST_CSID_DATA    6

/* the phases of the publish command sequence */
#define NGX_MEDIA_RTMP_DEST_PHASE_CONNECT  0
#define NGX_MEDIA_RTMP_DEST_PHASE_CREATE   1
#define NGX_MEDIA_RTMP_DEST_PHASE_PUBLISH  2

/* destination states */
#define NGX_MEDIA_RTMP_DEST_IDLE         0
#define NGX_MEDIA_RTMP_DEST_CONNECTING   1
#define NGX_MEDIA_RTMP_DEST_HANDSHAKE    2
#define NGX_MEDIA_RTMP_DEST_COMMANDS     3
#define NGX_MEDIA_RTMP_DEST_PUBLISHING   4

typedef struct ngx_media_rtmp_dest_s  ngx_media_rtmp_dest_t;

struct ngx_media_rtmp_dest_s {
    ngx_uint_t                   used;
    ngx_uint_t                   state;

    ngx_str_t                    id;
    ngx_str_t                    application;
    ngx_str_t                    stream_name;
    ngx_str_t                    host;
    ngx_uint_t                   port;

    ngx_media_stream_t          *stream;
    ngx_media_rtmp_prepare_t    *prepare;
    uint64_t                     cursor;
    unsigned                     announced:1;
    unsigned                     metadata_sent:1;

    ngx_connection_t            *connection;
    ngx_pool_t                  *pool;

    ngx_media_rtmp_handshake_t   handshake;
    ngx_media_rtmp_reader_t      reader;
    ngx_media_rtmp_writer_t      writer;
    unsigned                     reply_sent:1;

    u_char                       read_buffer[NGX_MEDIA_RTMP_DEST_READ_BUFFER];
    size_t                       pending_len;

    ngx_uint_t                   phase;
    ngx_uint_t                   stream_id;
    ngx_msec_t                   deadline;
    uint64_t                     reconnects;

    ngx_chain_t                 *out;
    ngx_chain_t                 *out_last;
    ngx_uint_t                   out_queue;
    size_t                        out_bytes;

    ngx_media_buf_t             *in_flight[NGX_MEDIA_RTMP_DEST_MAX_QUEUE];
    ngx_uint_t                   in_flight_head;
    ngx_uint_t                   in_flight_count;

    ngx_event_t                  timer;
    ngx_log_t                   *log;
};

static ngx_media_rtmp_dest_t  ngx_media_rtmp_destinations[
    NGX_MEDIA_RTMP_DEST_MAX];

static ngx_media_rtmp_dest_t *ngx_media_rtmp_dest_alloc(void);
static ngx_int_t ngx_media_rtmp_dest_connect(ngx_media_rtmp_dest_t *d);
static void ngx_media_rtmp_dest_connected(ngx_media_rtmp_dest_t *d);
static void ngx_media_rtmp_dest_release(ngx_media_rtmp_dest_t *d);
static void ngx_media_rtmp_dest_flush(ngx_media_rtmp_dest_t *d);
static void ngx_media_rtmp_dest_pump(ngx_media_rtmp_dest_t *d);
static void ngx_media_rtmp_dest_timer(ngx_event_t *ev);
static void ngx_media_rtmp_dest_read_handler(ngx_event_t *ev);
static void ngx_media_rtmp_dest_write_handler(ngx_event_t *ev);
static ngx_int_t ngx_media_rtmp_dest_on_message(void *ctx, ngx_uint_t type,
    ngx_uint_t stream_id, uint32_t timestamp, ngx_media_buf_t *payload);
static ngx_chain_t *ngx_media_rtmp_dest_chain(ngx_media_rtmp_dest_t *d,
    ngx_buf_t *b);
static ngx_int_t ngx_media_rtmp_dest_queue_raw(ngx_media_rtmp_dest_t *d,
    const u_char *data, size_t len);
static ngx_int_t ngx_media_rtmp_dest_queue_message(ngx_media_rtmp_dest_t *d,
    ngx_uint_t csid, ngx_uint_t type, ngx_uint_t stream_id, uint32_t timestamp,
    ngx_media_buf_t *payload, size_t len);
static void ngx_media_rtmp_dest_send_control(ngx_media_rtmp_dest_t *d,
    ngx_uint_t type, const u_char *body, size_t len);
static void ngx_media_rtmp_dest_send_command(ngx_media_rtmp_dest_t *d,
    ngx_uint_t stream_id, const u_char *body, size_t len);
static void ngx_media_rtmp_dest_send_connect(ngx_media_rtmp_dest_t *d);
static void ngx_media_rtmp_dest_send_publish_sequence(ngx_media_rtmp_dest_t *d);
static void ngx_media_rtmp_dest_send_publish(ngx_media_rtmp_dest_t *d);
static void ngx_media_rtmp_dest_send_metadata(ngx_media_rtmp_dest_t *d);
static void ngx_media_rtmp_dest_on_command(ngx_media_rtmp_dest_t *d,
    ngx_media_buf_t *payload);

/* --- slot bookkeeping ---------------------------------------------------- */

static ngx_media_rtmp_dest_t *
ngx_media_rtmp_dest_alloc(void)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_RTMP_DEST_MAX; i++) {

        if (!ngx_media_rtmp_destinations[i].used) {
            ngx_memzero(&ngx_media_rtmp_destinations[i],
                        sizeof(ngx_media_rtmp_dest_t));

            ngx_media_rtmp_destinations[i].used = 1;

            return &ngx_media_rtmp_destinations[i];
        }
    }

    return NULL;
}

/* --- output queue -------------------------------------------------------- */

/* builds one chain node holding a buffer the pool owns */
static ngx_chain_t *
ngx_media_rtmp_dest_chain(ngx_media_rtmp_dest_t *d, ngx_buf_t *b)
{
    ngx_chain_t  *cl;

    cl = ngx_alloc_chain_link(d->pool);

    if (cl == NULL) {
        return NULL;
    }

    cl->buf = b;
    cl->next = NULL;

    if (d->out_last != NULL) {
        d->out_last->next = cl;

    } else {
        d->out = cl;
    }

    d->out_last = cl;
    d->out_queue++;
    d->out_bytes += (size_t) (b->last - b->pos);

    return cl;
}

/* queues raw bytes: the handshake is not a chunked message */
static ngx_int_t
ngx_media_rtmp_dest_queue_raw(ngx_media_rtmp_dest_t *d, const u_char *data,
    size_t len)
{
    ngx_buf_t  *b;
    if (d->out_queue + 1 > NGX_MEDIA_RTMP_DEST_MAX_QUEUE
        || len > NGX_MEDIA_RTMP_DEST_MAX_BYTES
        || d->out_bytes > NGX_MEDIA_RTMP_DEST_MAX_BYTES - len)
    {
        return NGX_AGAIN;
    }

    b = ngx_create_temp_buf(d->pool, len);

    if (b == NULL) {
        return NGX_ERROR;
    }

    b->last = ngx_cpymem(b->last, data, len);

    if (ngx_media_rtmp_dest_chain(d, b) == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

/*
 * Queues one RTMP message.  The chunk headers are copied once into a pool
 * buffer and the media bytes are referenced in place, in the exact
 * interleaving the writer produced, so the shared payload stays alive until
 * the last byte of the message has been sent (goal doc 12.1).  Admission
 * counts every chain slot and payload reference the writer is about to
 * produce, from the writer itself, so the queue and the ring can never be
 * overrun by a multi chunk message.
 */
static ngx_int_t
ngx_media_rtmp_dest_queue_message(ngx_media_rtmp_dest_t *d, ngx_uint_t csid,
    ngx_uint_t type, ngx_uint_t stream_id, uint32_t timestamp,
    ngx_media_buf_t *payload, size_t len)
{
    ngx_media_rtmp_packet_t      packet;
    ngx_media_rtmp_writer_t      writer;
    ngx_media_rtmp_footprint_t   foot;
    ngx_buf_t                   *head;
    ngx_chain_t                 *pending, *pending_last, *cl;
    ngx_uint_t                   i, pending_queue;
    size_t                       wire_bytes;

    /*
     * Reserve the whole message before a byte of it is built: one chain slot
     * and one in-flight reference per chunk the writer is about to emit.  A
     * message that does not fit is refused here and nowhere else, so a refusal
     * leaves the queue, the ring and the byte count exactly as they were.
     */
    if (ngx_media_rtmp_message_footprint(&d->writer, len, &foot) != NGX_OK) {
        return NGX_AGAIN;
    }

    if (d->out_queue + foot.parts > NGX_MEDIA_RTMP_DEST_MAX_QUEUE
        || d->in_flight_count + foot.refs > NGX_MEDIA_RTMP_DEST_MAX_QUEUE)
    {
        return NGX_AGAIN;
    }

    ngx_media_rtmp_packet_init(&packet);
    writer = d->writer;

    if (ngx_media_rtmp_writer_message(&writer, &packet, csid, type,
                                      stream_id, timestamp, payload, 0, len)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    wire_bytes = 0;

    for (i = 0; i < packet.nparts; i++) {
        if (packet.parts[i].len > NGX_MEDIA_RTMP_DEST_MAX_BYTES
            || wire_bytes > NGX_MEDIA_RTMP_DEST_MAX_BYTES
                              - packet.parts[i].len)
        {
            ngx_media_rtmp_packet_destroy(&packet);
            return NGX_AGAIN;
        }

        wire_bytes += packet.parts[i].len;
    }

    if (d->out_bytes > NGX_MEDIA_RTMP_DEST_MAX_BYTES - wire_bytes) {
        ngx_media_rtmp_packet_destroy(&packet);
        return NGX_AGAIN;
    }

    /* one buffer holds every chunk header of this message */
    head = ngx_create_temp_buf(d->pool, packet.head_len);

    if (head == NULL) {
        ngx_media_rtmp_packet_destroy(&packet);
        return NGX_ERROR;
    }

    head->last = ngx_cpymem(head->last, packet.head, packet.head_len);
    pending = NULL;
    pending_last = NULL;
    pending_queue = 0;

    /*
     * Build the message on a private chain.  Nothing becomes visible in the
     * destination queue, and no payload reference enters the ring, until
     * every part has an allocated buffer and chain link.
     */
    for (i = 0; i < packet.nparts; i++) {
        ngx_buf_t  *b;

        b = ngx_calloc_buf(d->pool);

        if (b == NULL) {
            ngx_media_rtmp_packet_destroy(&packet);
            return NGX_ERROR;
        }

        if (packet.parts[i].data >= packet.head
            && packet.parts[i].data < packet.head + packet.head_len)
        {
            /* a chunk header: a window into the copied header buffer */
            b->temporary = 1;
            b->start = head->start + (packet.parts[i].data - packet.head);
            b->pos = b->start;
            b->end = b->start + packet.parts[i].len;
            b->last = b->end;

        } else {
            /* a payload slice: a reference into the shared buffer */
            b->memory = 1;
            b->start = (u_char *) packet.parts[i].data;
            b->pos = b->start;
            b->end = b->start + packet.parts[i].len;
            b->last = b->end;
        }

        cl = ngx_alloc_chain_link(d->pool);

        if (cl == NULL) {
            ngx_media_rtmp_packet_destroy(&packet);
            return NGX_ERROR;
        }

        cl->buf = b;
        cl->next = NULL;

        if (pending_last != NULL) {
            pending_last->next = cl;

        } else {
            pending = cl;
        }

        pending_last = cl;
        pending_queue++;
    }

    if (d->out_last != NULL) {
        d->out_last->next = pending;

    } else {
        d->out = pending;
    }

    d->out_last = pending_last;
    d->out_queue += pending_queue;
    d->out_bytes += wire_bytes;

    for (i = 0; i < packet.nparts; i++) {
        if (packet.parts[i].data < packet.head
            || packet.parts[i].data >= packet.head + packet.head_len)
        {
            d->in_flight[(d->in_flight_head + d->in_flight_count)
                         % NGX_MEDIA_RTMP_DEST_MAX_QUEUE] =
                ngx_media_buf_ref(packet.payload);
            d->in_flight_count++;
        }
    }

    d->writer = writer;
    d->out_last->buf->flush = 1;

    ngx_media_rtmp_packet_destroy(&packet);

    return NGX_OK;
}

static void
ngx_media_rtmp_dest_flush(ngx_media_rtmp_dest_t *d)
{
    ngx_connection_t  *c = d->connection;
    ngx_chain_t       *sent_tail, *cl, *next;
    size_t             remaining_bytes;

    if (c == NULL || d->out == NULL) {
        return;
    }

    /* send_chain() returns the part of the chain that is still unsent */
    sent_tail = c->send_chain(c, d->out, 0);

    if (sent_tail == NGX_CHAIN_ERROR) {
        ngx_media_rtmp_dest_release(d);
        return;
    }

    for (cl = d->out; cl != NULL && cl != sent_tail; cl = next) {
        next = cl->next;

        if (cl->buf->memory && d->in_flight_count > 0) {
            ngx_media_buf_unref(d->in_flight[d->in_flight_head]);
            d->in_flight_head = (d->in_flight_head + 1)
                                % NGX_MEDIA_RTMP_DEST_MAX_QUEUE;
            d->in_flight_count--;
        }

        d->out_queue--;
    }
    remaining_bytes = 0;

    for (cl = sent_tail; cl != NULL; cl = cl->next) {
        if (cl->buf->pos < cl->buf->last) {
            remaining_bytes += (size_t) (cl->buf->last - cl->buf->pos);
        }
    }

    d->out_bytes = remaining_bytes;

    d->out = sent_tail;
    d->out_last = sent_tail;

    if (d->out_last != NULL) {
        while (d->out_last->next != NULL) {
            d->out_last = d->out_last->next;
        }

        if (!c->write->active) {
            (void) ngx_handle_write_event(c->write, 0);
        }

        return;
    }

    /*
     * Nothing is outstanding any more, so every buffer the last flush
     * allocated from the connection pool can go back.  Without this a
     * destination that runs for days accumulates one message worth of pool
     * per message ever sent.
     */
    if (d->in_flight_count == 0 && d->state == NGX_MEDIA_RTMP_DEST_PUBLISHING
        && d->pending_len == 0)
    {
        ngx_reset_pool(d->pool);
    }
}

/* --- commands ------------------------------------------------------------ */

static void
ngx_media_rtmp_dest_send_control(ngx_media_rtmp_dest_t *d, ngx_uint_t type,
    const u_char *body, size_t len)
{
    ngx_media_buf_t  *payload;

    payload = ngx_media_buf_alloc(len ? len : 1);

    if (payload == NULL) {
        return;
    }

    if (len > 0 && body != NULL) {
        ngx_memcpy(ngx_media_buf_data(payload), body, len);
    }

    (void) ngx_media_buf_freeze(payload, len);

    (void) ngx_media_rtmp_dest_queue_message(d,
                                             NGX_MEDIA_RTMP_DEST_CSID_CONTROL,
                                             type, 0, 0, payload, len);

    ngx_media_buf_unref(payload);
}

static void
ngx_media_rtmp_dest_send_command(ngx_media_rtmp_dest_t *d,
    ngx_uint_t stream_id, const u_char *body, size_t len)
{
    ngx_media_buf_t  *payload;

    payload = ngx_media_buf_alloc(len);

    if (payload == NULL) {
        return;
    }

    ngx_memcpy(ngx_media_buf_data(payload), body, len);
    (void) ngx_media_buf_freeze(payload, len);

    if (ngx_media_rtmp_dest_queue_message(
            d, NGX_MEDIA_RTMP_DEST_CSID_COMMAND,
            NGX_MEDIA_RTMP_MSG_COMMAND_AMF0, stream_id, 0, payload, len)
        != NGX_OK)
    {
        ngx_log_error(NGX_LOG_WARN, d->log, 0,
                      "media: rtmp destination %V could not queue a command",
                      &d->id);
    }

    ngx_media_buf_unref(payload);
}

/* "connect": the application name and the endpoint, as every server expects */
static void
ngx_media_rtmp_dest_send_connect(ngx_media_rtmp_dest_t *d)
{
    u_char                 buf[NGX_MEDIA_RTMP_DEST_AMF_BUFFER];
    u_char                 tcurl[512];
    size_t                 tcurl_len;
    ngx_media_amf_writer_t w;

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    tcurl_len = ngx_snprintf(tcurl, sizeof(tcurl), "rtmp://%V:%ui/%V",
                             &d->host, d->port, &d->application) - tcurl;

    if (ngx_media_amf_put_string(&w, (const u_char *) "connect", 7) != NGX_OK
        || ngx_media_amf_put_number(&w, 1) != NGX_OK
        || ngx_media_amf_begin_object(&w, 0, 0) != NGX_OK
        || ngx_media_amf_put_member_string(&w, "app", d->application.data,
                                           d->application.len) != NGX_OK
        || ngx_media_amf_put_member_string(&w, "type",
                                           (const u_char *) "nonprivate",
                                           11) != NGX_OK
        || ngx_media_amf_put_member_string(
               &w, "flashVer",
               (const u_char *) "FMLE/3.0 (compatible; nginx-media)",
               34) != NGX_OK
        || ngx_media_amf_put_member_string(&w, "tcUrl", tcurl,
                                           tcurl_len) != NGX_OK
        || ngx_media_amf_put_member_boolean(&w, "fpad", 0) != NGX_OK
        || ngx_media_amf_put_member_number(&w, "capabilities", 15) != NGX_OK
        || ngx_media_amf_put_member_number(&w, "audioCodecs", 4071) != NGX_OK
        || ngx_media_amf_put_member_number(&w, "videoCodecs", 252) != NGX_OK
        || ngx_media_amf_put_member_number(&w, "videoFunction", 1) != NGX_OK
        || ngx_media_amf_put_member_number(&w, "objectEncoding", 0) != NGX_OK
        || ngx_media_amf_end_object(&w) != NGX_OK)
    {
        return;
    }

    ngx_media_rtmp_dest_send_command(d, 0, buf, w.len);

    d->phase = NGX_MEDIA_RTMP_DEST_PHASE_CONNECT;
}

/*
 * The rest of the publish handshake.  A server may ignore releaseStream and
 * FCPublish, but FMLE and every server that follows it expect them, and a
 * sequence that omits them is rejected by some.
 */
static void
ngx_media_rtmp_dest_send_publish_sequence(ngx_media_rtmp_dest_t *d)
{
    u_char                 buf[NGX_MEDIA_RTMP_DEST_AMF_BUFFER];
    ngx_media_amf_writer_t w;

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    if (ngx_media_amf_put_string(&w, (const u_char *) "releaseStream", 13)
        == NGX_OK
        && ngx_media_amf_put_number(&w, 2) == NGX_OK
        && ngx_media_amf_put_null(&w) == NGX_OK
        && ngx_media_amf_put_string(&w, d->stream_name.data,
                                    d->stream_name.len) == NGX_OK)
    {
        ngx_media_rtmp_dest_send_command(d, 0, buf, w.len);
    }

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    if (ngx_media_amf_put_string(&w, (const u_char *) "FCPublish", 9) == NGX_OK
        && ngx_media_amf_put_number(&w, 3) == NGX_OK
        && ngx_media_amf_put_null(&w) == NGX_OK
        && ngx_media_amf_put_string(&w, d->stream_name.data,
                                    d->stream_name.len) == NGX_OK)
    {
        ngx_media_rtmp_dest_send_command(d, 0, buf, w.len);
    }

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    if (ngx_media_amf_put_string(&w, (const u_char *) "createStream", 12)
        == NGX_OK
        && ngx_media_amf_put_number(&w, 4) == NGX_OK
        && ngx_media_amf_put_null(&w) == NGX_OK)
    {
        ngx_media_rtmp_dest_send_command(d, 0, buf, w.len);
    }

    d->phase = NGX_MEDIA_RTMP_DEST_PHASE_CREATE;
}

static void
ngx_media_rtmp_dest_send_publish(ngx_media_rtmp_dest_t *d)
{
    u_char                 buf[NGX_MEDIA_RTMP_DEST_AMF_BUFFER];
    ngx_media_amf_writer_t w;

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    if (ngx_media_amf_put_string(&w, (const u_char *) "publish", 7) != NGX_OK
        || ngx_media_amf_put_number(&w, 5) != NGX_OK
        || ngx_media_amf_put_null(&w) != NGX_OK
        || ngx_media_amf_put_string(&w, d->stream_name.data,
                                    d->stream_name.len) != NGX_OK
        || ngx_media_amf_put_string(&w, (const u_char *) "live", 4) != NGX_OK)
    {
        return;
    }

    ngx_media_rtmp_dest_send_command(d, d->stream_id, buf, w.len);

    d->phase = NGX_MEDIA_RTMP_DEST_PHASE_PUBLISH;
}

/*
 * @setDataFrame/onMetaData, from the program's own track contract, so the
 * receiving side knows the codec before the first frame arrives.  H.265 is
 * signalled the way enhanced RTMP does it: the fourcc as a string rather
 * than a legacy FLV codec id, which cannot express HEVC at all.
 */
static void
ngx_media_rtmp_dest_send_metadata(ngx_media_rtmp_dest_t *d)
{
    u_char                  buf[NGX_MEDIA_RTMP_DEST_AMF_BUFFER];
    ngx_media_amf_writer_t  w;
    ngx_media_trackset_t   *tracks;
    ngx_media_buf_t        *payload;
    ngx_uint_t              i, members;

    if (d->stream == NULL || d->stream->active == NULL) {
        return;
    }

    tracks = d->stream->active->tracks;

    if (tracks == NULL || tracks->count == 0) {
        return;
    }

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    /*
     * An ECMA array carries a member count, and a wrong one is what a strict
     * parser chokes on, so it is counted rather than guessed: duration,
     * a codec description per track, and the encoder name.
     */
    members = 2;

    for (i = 0; i < tracks->count; i++) {

        if (tracks->tracks[i].media_type == NGX_MEDIA_TYPE_VIDEO
            || tracks->tracks[i].media_type == NGX_MEDIA_TYPE_AUDIO)
        {
            members += 4;
        }
    }

    if (ngx_media_amf_put_string(&w, (const u_char *) "@setDataFrame", 13)
        != NGX_OK
        || ngx_media_amf_put_string(&w, (const u_char *) "onMetaData", 10)
           != NGX_OK
        || ngx_media_amf_begin_object(&w, 1, members) != NGX_OK)
    {
        return;
    }

    (void) ngx_media_amf_put_member_number(&w, "duration", 0);

    for (i = 0; i < tracks->count; i++) {
        const ngx_media_track_t  *track = &tracks->tracks[i];

        if (track->media_type == NGX_MEDIA_TYPE_VIDEO) {
            double  rate = 0;

            if (track->frame_rate_den != 0) {
                rate = (double) track->frame_rate_num
                       / (double) track->frame_rate_den;
            }

            (void) ngx_media_amf_put_member_number(&w, "width",
                                                   (double) track->width);
            (void) ngx_media_amf_put_member_number(&w, "height",
                                                   (double) track->height);
            (void) ngx_media_amf_put_member_number(&w, "framerate", rate);

            if (track->codec == NGX_MEDIA_CODEC_H265) {
                (void) ngx_media_amf_put_member_string(
                           &w, "videocodecid", (const u_char *) "hvc1", 4);

            } else {
                (void) ngx_media_amf_put_member_number(
                           &w, "videocodecid",
                           (double) NGX_MEDIA_RTMP_CODEC_AVC);
            }

        } else if (track->media_type == NGX_MEDIA_TYPE_AUDIO) {
            (void) ngx_media_amf_put_member_number(&w, "audiosamplerate",
                                                   (double) track->sample_rate);
            (void) ngx_media_amf_put_member_number(&w, "audiosamplesize", 16);
            (void) ngx_media_amf_put_member_boolean(&w, "stereo",
                                                    track->channels > 1);
            (void) ngx_media_amf_put_member_number(
                       &w, "audiocodecid",
                       (double) NGX_MEDIA_RTMP_SOUND_AAC);
        }
    }

    (void) ngx_media_amf_put_member_string(&w, "encoder",
                                           (const u_char *) "nginx-media", 11);

    if (ngx_media_amf_end_object(&w) != NGX_OK) {
        return;
    }

    payload = ngx_media_buf_alloc(w.len);

    if (payload == NULL) {
        return;
    }

    ngx_memcpy(ngx_media_buf_data(payload), buf, w.len);
    (void) ngx_media_buf_freeze(payload, w.len);

    (void) ngx_media_rtmp_dest_queue_message(
        d, NGX_MEDIA_RTMP_DEST_CSID_DATA, NGX_MEDIA_RTMP_MSG_DATA_AMF0,
        d->stream_id, 0, payload, w.len);

    ngx_media_buf_unref(payload);
}

/* --- command replies ----------------------------------------------------- */

static void
ngx_media_rtmp_dest_on_command(ngx_media_rtmp_dest_t *d, ngx_media_buf_t *payload)
{
    const u_char          *data = ngx_media_buf_data(payload);
    size_t                 len = ngx_media_buf_size(payload);
    size_t                 used = 0;
    ngx_media_amf_value_t  command, value;
    ngx_str_t             *name;

    if (ngx_media_amf_read(data, len, &command, &used) != NGX_OK
        || command.type != NGX_MEDIA_AMF_STRING)
    {
        return;
    }

    data += used;
    len -= used;

    if (ngx_media_amf_read(data, len, &value, &used) != NGX_OK) {
        return;
    }

    data += used;
    len -= used;

    name = &command.string;

    if (name->len == sizeof("_result") - 1
        && ngx_memcmp(name->data, "_result", sizeof("_result") - 1) == 0)
    {
        if (d->phase == NGX_MEDIA_RTMP_DEST_PHASE_CONNECT) {
            /* connect answered: the rest of the publish sequence */
            ngx_media_rtmp_dest_send_publish_sequence(d);
            ngx_media_rtmp_dest_flush(d);
            return;
        }

        if (d->phase == NGX_MEDIA_RTMP_DEST_PHASE_CREATE) {
            /*
             * createStream answered with the message stream id; the reply is
             * [null] [stream id].
             */
            if (ngx_media_amf_read(data, len, &value, &used) == NGX_OK
                && value.type == NGX_MEDIA_AMF_NULL)
            {
                data += used;
                len -= used;
            }

            if (ngx_media_amf_read(data, len, &value, &used) == NGX_OK
                && value.type == NGX_MEDIA_AMF_NUMBER
                && value.number >= 1)
            {
                d->stream_id = (ngx_uint_t) value.number;
            }

            if (d->stream_id == 0) {
                d->stream_id = NGX_MEDIA_RTMP_DEST_STREAM_ID;
            }

            ngx_media_rtmp_dest_send_publish(d);
            ngx_media_rtmp_dest_flush(d);
            return;
        }

        return;
    }

    if (name->len == sizeof("onStatus") - 1
        && ngx_memcmp(name->data, "onStatus", sizeof("onStatus") - 1) == 0)
    {
        ngx_media_amf_value_t  status;
        const ngx_str_t       *code;

        if (d->phase != NGX_MEDIA_RTMP_DEST_PHASE_PUBLISH) {
            return;
        }

        /* [null] [status object] */
        if (ngx_media_amf_read(data, len, &value, &used) == NGX_OK
            && value.type == NGX_MEDIA_AMF_NULL)
        {
            data += used;
            len -= used;
        }

        ngx_memzero(&status, sizeof(status));

        if (ngx_media_amf_read(data, len, &status, &used) != NGX_OK
            || status.type != NGX_MEDIA_AMF_OBJECT)
        {
            return;
        }

        code = ngx_media_amf_member(&status, "code");

        if (code == NULL) {
            return;
        }

        if (code->len == sizeof("NetStream.Publish.Start") - 1
            && ngx_memcmp(code->data, "NetStream.Publish.Start",
                          sizeof("NetStream.Publish.Start") - 1) == 0)
        {
            d->state = NGX_MEDIA_RTMP_DEST_PUBLISHING;
            d->deadline = 0;
            d->cursor = 0;

            ngx_media_rtmp_dest_pump(d);
            ngx_media_rtmp_dest_flush(d);

            ngx_log_error(NGX_LOG_NOTICE, d->log, 0,
                          "media: rtmp destination %V published %V/%V to "
                          "%V:%ui (attempts=%uL)",
                          &d->id, &d->application, &d->stream_name, &d->host,
                          d->port, d->reconnects);

            return;
        }

        ngx_log_error(NGX_LOG_WARN, d->log, 0,
                      "media: rtmp destination %V publish rejected: %V",
                      &d->id, code);

        ngx_media_rtmp_dest_release(d);
    }
}

static ngx_int_t
ngx_media_rtmp_dest_on_message(void *ctx, ngx_uint_t type, ngx_uint_t stream_id,
    uint32_t timestamp, ngx_media_buf_t *payload)
{
    ngx_media_rtmp_dest_t  *d = ctx;

    (void) stream_id;
    (void) timestamp;

    if (type == NGX_MEDIA_RTMP_MSG_COMMAND_AMF0) {
        ngx_media_rtmp_dest_on_command(d, payload);
        return NGX_OK;
    }

    if (type == NGX_MEDIA_RTMP_MSG_USER_CONTROL) {
        const u_char  *body = ngx_media_buf_data(payload);

        if (ngx_media_buf_size(payload) >= 6 && body[0] == 0
            && body[1] == NGX_MEDIA_RTMP_EVENT_PING_REQUEST)
        {
            u_char  reply[6];

            ngx_memcpy(reply, body, 6);
            reply[1] = NGX_MEDIA_RTMP_EVENT_PING_RESPONSE;

            ngx_media_rtmp_dest_send_control(
                d, NGX_MEDIA_RTMP_MSG_USER_CONTROL, reply, sizeof(reply));
        }
    }

    /* window ack, peer bandwidth and acknowledgements need no reply here */
    return NGX_OK;
}

/* --- media pump ---------------------------------------------------------- */

static void
ngx_media_rtmp_dest_pump(ngx_media_rtmp_dest_t *d)
{
    const ngx_media_rtmp_media_t  *unit;
    ngx_uint_t                     sent = 0;
    ngx_int_t                      rc;

    if (d->prepare == NULL || d->stream == NULL
        || d->state != NGX_MEDIA_RTMP_DEST_PUBLISHING)
    {
        return;
    }

    if (!d->metadata_sent) {

        if (d->stream->active == NULL || d->stream->active->tracks == NULL) {
            return;
        }

        if (!d->announced) {
            d->announced = 1;

            /*
             * A destination added to a program that is already running starts
             * past the sequence headers the fanout has long evicted, so ask
             * the shared preparation for them once and read from there.
             */
            if (d->cursor == 0 && d->prepare->fan.tail > 0) {
                uint64_t  head = ngx_media_rtmp_fanout_head(&d->prepare->fan);

                if (ngx_media_rtmp_prepare_announce(
                        d->prepare, d->stream->active->tracks) == NGX_OK)
                {
                    d->cursor = head;
                }
            }
        }

        ngx_media_rtmp_dest_send_metadata(d);
        d->metadata_sent = 1;
    }

    while (sent < NGX_MEDIA_RTMP_DEST_MAX_BATCH
           && d->out_queue < NGX_MEDIA_RTMP_DEST_MAX_QUEUE)
    {
        unit = ngx_media_rtmp_fanout_next(&d->prepare->fan, d->cursor);

        if (unit == NULL) {
            break;
        }

        rc = ngx_media_rtmp_dest_queue_message(
            d,
            (unit->type == NGX_MEDIA_RTMP_MSG_VIDEO)
                ? NGX_MEDIA_RTMP_DEST_CSID_VIDEO
                : NGX_MEDIA_RTMP_DEST_CSID_AUDIO,
            unit->type, d->stream_id, unit->timestamp, unit->payload,
            ngx_media_buf_size(unit->payload));

        if (rc == NGX_AGAIN) {
            break;
        }

        if (rc != NGX_OK) {
            ngx_media_rtmp_dest_release(d);
            return;
        }

        d->cursor = unit->sequence + 1;
        sent++;
    }

    if (sent > 0) {
        ngx_media_rtmp_dest_flush(d);
    }
}

/* --- connection ---------------------------------------------------------- */

static void
ngx_media_rtmp_dest_release(ngx_media_rtmp_dest_t *d)
{
    ngx_connection_t  *c = d->connection;

    d->connection = NULL;

    if (c != NULL) {
        ngx_close_connection(c);
    }

    /* stop holding the shared payloads the queue was keeping alive */
    while (d->in_flight_count > 0) {
        ngx_media_buf_unref(d->in_flight[d->in_flight_head]);
        d->in_flight_head = (d->in_flight_head + 1)
                            % NGX_MEDIA_RTMP_DEST_MAX_QUEUE;
        d->in_flight_count--;
    }

    d->out = NULL;
    d->out_last = NULL;
    d->out_queue = 0;
    d->out_bytes = 0;

    if (d->pool != NULL) {
        ngx_reset_pool(d->pool);
    }

    ngx_media_rtmp_reader_reset(&d->reader);
    ngx_media_rtmp_writer_init(&d->writer, NGX_MEDIA_RTMP_DEST_CHUNK,
                               NGX_MEDIA_RTMP_MAX_MESSAGE);
    ngx_media_rtmp_handshake_init(&d->handshake);

    d->pending_len = 0;
    d->reply_sent = 0;
    d->phase = 0;
    d->stream_id = 0;
    d->cursor = 0;
    d->announced = 0;
    d->metadata_sent = 0;
    d->deadline = 0;
    d->state = NGX_MEDIA_RTMP_DEST_IDLE;
}

/* C0C1, the client half of the handshake: version, then a simple C1 */
static ngx_int_t
ngx_media_rtmp_dest_handshake_start(ngx_media_rtmp_dest_t *d)
{
    u_char  packet[1 + NGX_MEDIA_RTMP_HANDSHAKE_SIZE];
    size_t  i;

    packet[0] = NGX_MEDIA_RTMP_VERSION;

    /* the simple handshake: a zero time/version field, then 1528 random bytes */
    ngx_memzero(packet + 1, 8);

    for (i = 9; i < sizeof(packet); i++) {
        packet[i] = (u_char) ngx_random();
    }

    return ngx_media_rtmp_dest_queue_raw(d, packet, sizeof(packet));
}

/* the handshake's S0S1S2 has arrived: answer with C2 and connect */
static void
ngx_media_rtmp_dest_handshake_done(ngx_media_rtmp_dest_t *d)
{
    u_char  chunk[4];

    if (ngx_media_rtmp_dest_queue_raw(d, d->handshake.c1,
                                      NGX_MEDIA_RTMP_HANDSHAKE_SIZE) != NGX_OK)
    {
        ngx_media_rtmp_dest_release(d);
        return;
    }

    /* tell the peer the outbound chunk size before any multi chunk message */
    chunk[0] = (u_char) ((NGX_MEDIA_RTMP_DEST_CHUNK >> 24) & 0xFF);
    chunk[1] = (u_char) ((NGX_MEDIA_RTMP_DEST_CHUNK >> 16) & 0xFF);
    chunk[2] = (u_char) ((NGX_MEDIA_RTMP_DEST_CHUNK >> 8) & 0xFF);
    chunk[3] = (u_char) (NGX_MEDIA_RTMP_DEST_CHUNK & 0xFF);

    ngx_media_rtmp_dest_send_control(d, NGX_MEDIA_RTMP_MSG_CHUNK_SIZE, chunk,
                                     sizeof(chunk));

    ngx_media_rtmp_dest_send_connect(d);

    ngx_media_rtmp_dest_flush(d);
}

/* the socket is connected: start the RTMP handshake */
static void
ngx_media_rtmp_dest_connected(ngx_media_rtmp_dest_t *d)
{
    d->state = NGX_MEDIA_RTMP_DEST_HANDSHAKE;
    d->deadline = ngx_current_msec;

    if (ngx_media_rtmp_dest_handshake_start(d) != NGX_OK) {
        ngx_media_rtmp_dest_release(d);
        return;
    }

    ngx_media_rtmp_dest_flush(d);

    if (d->state != NGX_MEDIA_RTMP_DEST_HANDSHAKE) {
        return;
    }

    ngx_log_error(NGX_LOG_INFO, d->log, 0,
                  "media: rtmp destination %V connected to %V:%ui",
                  &d->id, &d->host, d->port);
}

/*
 * Non-blocking connect.  NGX_DECLINED is a malformed address, which is a
 * configuration error and never becomes connectable; NGX_ERROR is a transient
 * failure the retry timer will pick up.
 */
static ngx_int_t
ngx_media_rtmp_dest_connect(ngx_media_rtmp_dest_t *d)
{
    struct sockaddr_in  sin;
    ngx_connection_t   *c;
    ngx_int_t           fd, rc;
    ngx_err_t           err;

    ngx_memzero(&sin, sizeof(sin));

    sin.sin_family = AF_INET;
    sin.sin_port = htons((in_port_t) d->port);

    /* a char* cast, as inet_pton takes no length */
    {
        char  host[NGX_INET_ADDRSTRLEN];

        if (d->host.len == 0 || d->host.len >= sizeof(host)) {
            return NGX_DECLINED;
        }

        ngx_memcpy(host, d->host.data, d->host.len);
        host[d->host.len] = '\0';

        if (inet_pton(AF_INET, host, &sin.sin_addr) != 1) {
            return NGX_DECLINED;
        }
    }

    fd = ngx_socket(AF_INET, SOCK_STREAM, 0);

    if (fd == -1) {
        ngx_log_error(NGX_LOG_WARN, d->log, ngx_socket_errno,
                      "media: rtmp destination %V could not create a socket",
                      &d->id);
        return NGX_ERROR;
    }

    if (ngx_nonblocking(fd) == -1) {
        ngx_log_error(NGX_LOG_WARN, d->log, ngx_socket_errno,
                      "media: rtmp destination %V could not set the socket "
                      "non-blocking", &d->id);
        (void) ngx_close_socket(fd);
        return NGX_ERROR;
    }

    c = ngx_get_connection(fd, d->log);

    if (c == NULL) {
        (void) ngx_close_socket(fd);
        return NGX_ERROR;
    }

    c->log = d->log;
    c->pool = d->pool;
    c->data = d;
    c->recv = ngx_recv;
    c->send = ngx_send;
    c->recv_chain = ngx_recv_chain;
    c->send_chain = ngx_send_chain;
    c->read->handler = ngx_media_rtmp_dest_read_handler;
    c->read->log = d->log;
    c->write->handler = ngx_media_rtmp_dest_write_handler;
    c->write->log = d->log;

    d->connection = c;

    if (ngx_add_conn(c) != NGX_OK) {
        ngx_media_rtmp_dest_release(d);
        return NGX_ERROR;
    }

    rc = connect(fd, (struct sockaddr *) &sin, sizeof(sin));

    if (rc == -1) {
        err = ngx_socket_errno;

        if (err != NGX_EINPROGRESS) {
            ngx_log_error(NGX_LOG_WARN, d->log, err,
                          "media: rtmp destination %V could not connect to "
                          "%V:%ui", &d->id, &d->host, d->port);

            ngx_media_rtmp_dest_release(d);
            return NGX_ERROR;
        }

        /* the write event reports the result */
        d->state = NGX_MEDIA_RTMP_DEST_CONNECTING;
        d->deadline = ngx_current_msec;

        return NGX_OK;
    }

    ngx_media_rtmp_dest_connected(d);

    return (d->state == NGX_MEDIA_RTMP_DEST_HANDSHAKE)
               ? NGX_OK
               : NGX_ERROR;
}

/* --- events -------------------------------------------------------------- */

static void
ngx_media_rtmp_dest_write_handler(ngx_event_t *ev)
{
    ngx_connection_t       *c = ev->data;
    ngx_media_rtmp_dest_t  *d = c->data;
    socklen_t               len = sizeof(int);
    int                     err = 0;

    if (d == NULL || c != d->connection) {
        return;
    }

    if (d->state == NGX_MEDIA_RTMP_DEST_CONNECTING) {

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len) == -1
            || err != 0)
        {
            ngx_log_error(NGX_LOG_WARN, d->log, err != 0 ? err : 0,
                          "media: rtmp destination %V could not connect to "
                          "%V:%ui", &d->id, &d->host, d->port);

            ngx_media_rtmp_dest_release(d);
            return;
        }

        ngx_media_rtmp_dest_connected(d);
        return;
    }

    ngx_media_rtmp_dest_flush(d);
}

static void
ngx_media_rtmp_dest_read_handler(ngx_event_t *ev)
{
    ngx_connection_t       *c = ev->data;
    ngx_media_rtmp_dest_t  *d = c->data;
    ssize_t                 n;
    size_t                  consumed, take;

    if (d == NULL || c != d->connection) {
        return;
    }

    for ( ;; ) {
        take = sizeof(d->read_buffer) - d->pending_len;

        if (take == 0) {
            ngx_log_error(NGX_LOG_WARN, d->log, 0,
                          "media: rtmp destination %V: message header exceeds "
                          "the read buffer", &d->id);
            ngx_media_rtmp_dest_release(d);
            return;
        }

        n = c->recv(c, d->read_buffer + d->pending_len, take);

        if (n == NGX_AGAIN) {
            break;
        }

        if (n == 0 || n == NGX_ERROR) {
            ngx_media_rtmp_dest_release(d);
            return;
        }

        d->pending_len += (size_t) n;

        /* handshake first, then the chunk stream */
        if (d->state == NGX_MEDIA_RTMP_DEST_HANDSHAKE) {
            ngx_int_t  hrc;

            hrc = ngx_media_rtmp_handshake_feed(&d->handshake, d->read_buffer,
                                                d->pending_len, &consumed);

            if (consumed > 0) {
                ngx_memmove(d->read_buffer, d->read_buffer + consumed,
                            d->pending_len - consumed);
                d->pending_len -= consumed;
            }

            if (hrc == NGX_ERROR) {
                ngx_log_error(NGX_LOG_WARN, d->log, 0,
                              "media: rtmp destination %V: handshake failed",
                              &d->id);
                ngx_media_rtmp_dest_release(d);
                return;
            }

            if (!d->reply_sent && d->handshake.state >= 2) {
                /*
                 * S0S1 has arrived: the C2 that answers it and the command
                 * sequence can go out before S2 finishes streaming in, which
                 * is what keeps the handshake from costing a round trip.  The
                 * state stays HANDSHAKE until S2 has been consumed, so the
                 * bytes still owed to the handshake are never mistaken for
                 * chunk data.
                 */
                d->reply_sent = 1;
                ngx_media_rtmp_dest_handshake_done(d);

                if (d->state == NGX_MEDIA_RTMP_DEST_IDLE) {
                    return;
                }
            }

            if (hrc == NGX_OK) {
                d->state = NGX_MEDIA_RTMP_DEST_COMMANDS;
                d->deadline = ngx_current_msec;
            }
        }

        if (d->state != NGX_MEDIA_RTMP_DEST_HANDSHAKE && d->pending_len > 0) {

            if (ngx_media_rtmp_reader_feed(&d->reader, d->read_buffer,
                                           d->pending_len, &consumed,
                                           ngx_media_rtmp_dest_on_message, d)
                != NGX_OK)
            {
                ngx_media_rtmp_dest_release(d);
                return;
            }

            if (consumed > 0) {
                ngx_memmove(d->read_buffer, d->read_buffer + consumed,
                            d->pending_len - consumed);
                d->pending_len -= consumed;
            }

            if (d->state == NGX_MEDIA_RTMP_DEST_IDLE) {
                return;
            }
        }

        ngx_media_rtmp_dest_flush(d);

        if (d->connection == NULL) {
            return;
        }
    }

    if (d->out != NULL) {
        ngx_media_rtmp_dest_flush(d);
    }
}

/* --- timer --------------------------------------------------------------- */

static void
ngx_media_rtmp_dest_timer(ngx_event_t *ev)
{
    ngx_media_rtmp_dest_t  *d = ev->data;
    ngx_msec_t              interval = NGX_MEDIA_RTMP_DEST_TICK;

    if (!d->used) {
        return;
    }

    /* every stage of the protocol is bounded: a silent peer is dropped */
    if (d->deadline != 0
        && ngx_current_msec - d->deadline > NGX_MEDIA_RTMP_DEST_STAGE)
    {
        ngx_log_error(NGX_LOG_WARN, d->log, 0,
                      "media: rtmp destination %V timed out in state %ui",
                      &d->id, d->state);

        ngx_media_rtmp_dest_release(d);
    }

    if (d->state == NGX_MEDIA_RTMP_DEST_IDLE) {

        if (ngx_media_rtmp_dest_connect(d) != NGX_OK) {
            d->reconnects++;
            interval = NGX_MEDIA_RTMP_DEST_RETRY;
        }

    } else if (d->state == NGX_MEDIA_RTMP_DEST_PUBLISHING) {
        ngx_media_rtmp_dest_pump(d);
    }

    if (d->used) {
        ngx_add_timer(&d->timer, interval);
    }
}

/* --- backend entry points ------------------------------------------------ */

ngx_int_t
ngx_media_rtmp_destination_add(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination, ngx_log_t *log)
{
    ngx_media_rtmp_dest_t  *d;
    ngx_int_t               rc;

    if (stream == NULL || destination == NULL
        || destination->host.data == NULL || destination->port == 0)
    {
        return NGX_ERROR;
    }

    d = ngx_media_rtmp_dest_alloc();

    if (d == NULL) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: rtmp destination %V: no free slot",
                      &destination->id);
        return NGX_ERROR;
    }

    d->log = ((ngx_cycle_t *) ngx_cycle)->log;
    d->pool = ngx_create_pool(NGX_MEDIA_RTMP_DEST_POOL_SIZE, d->log);

    if (d->pool == NULL) {
        ngx_memzero(d, sizeof(ngx_media_rtmp_dest_t));
        return NGX_ERROR;
    }

    d->id = destination->id;
    d->application = stream->application;
    d->host = destination->host;
    d->port = destination->port;
    d->stream = stream;
    d->stream_name = destination->streamid.len ? destination->streamid
                                               : stream->name;

    /*
     * The same shared FLV preparation the players read: the program is
     * converted once and this destination adds only its chunk headers.
     */
    d->prepare = ngx_media_runtime_prepare(stream, d->log);

    ngx_media_rtmp_handshake_init(&d->handshake);
    ngx_media_rtmp_reader_init(&d->reader);
    ngx_media_rtmp_writer_init(&d->writer, NGX_MEDIA_RTMP_DEST_CHUNK,
                               NGX_MEDIA_RTMP_MAX_MESSAGE);

    ngx_memzero(&d->timer, sizeof(ngx_event_t));

    d->timer.handler = ngx_media_rtmp_dest_timer;
    d->timer.log = d->log;
    d->timer.data = d;

    rc = ngx_media_rtmp_dest_connect(d);

    if (rc == NGX_DECLINED) {
        ngx_destroy_pool(d->pool);
        ngx_memzero(d, sizeof(ngx_media_rtmp_dest_t));

        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: rtmp destination %V host \"%V\" is not an IPv4 "
                      "address", &destination->id, &destination->host);

        return NGX_ERROR;
    }

    /* a transient connect failure is retried; the destination still starts */
    destination->impl = d;

    ngx_add_timer(&d->timer, NGX_MEDIA_RTMP_DEST_TICK);

    ngx_log_error(NGX_LOG_NOTICE, d->log, 0,
                  "media: rtmp destination %V started for %V/%V -> %V:%ui",
                  &d->id, &d->application, &d->stream_name, &d->host,
                  d->port);

    return NGX_OK;
}

void
ngx_media_rtmp_destination_remove(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination)
{
    ngx_media_rtmp_dest_t  *d = destination->impl;

    (void) stream;

    if (d == NULL || !d->used) {
        return;
    }

    if (d->timer.timer_set) {
        ngx_del_timer(&d->timer);
    }

    ngx_media_rtmp_dest_release(d);

    ngx_log_error(NGX_LOG_NOTICE, d->log, 0,
                  "media: rtmp destination %V stopped", &d->id);

    if (d->pool != NULL) {
        ngx_destroy_pool(d->pool);
    }

    ngx_memzero(d, sizeof(ngx_media_rtmp_dest_t));
}

void
ngx_media_rtmp_destination_stop_all(void)
{
    ngx_uint_t              i;
    ngx_media_rtmp_dest_t  *d;

    for (i = 0; i < NGX_MEDIA_RTMP_DEST_MAX; i++) {
        d = &ngx_media_rtmp_destinations[i];

        if (!d->used) {
            continue;
        }

        if (d->timer.timer_set) {
            ngx_del_timer(&d->timer);
        }

        ngx_media_rtmp_dest_release(d);

        if (d->pool != NULL) {
            ngx_destroy_pool(d->pool);
        }

        ngx_memzero(d, sizeof(ngx_media_rtmp_dest_t));
    }
}

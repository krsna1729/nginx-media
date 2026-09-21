#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include "ngx_media_rtmp_adapter.h"
#include "ngx_media_rtmp_wire.h"
#include "ngx_media_registry.h"
#include "ngx_media_route.h"
#include "ngx_media_runtime.h"
#include "ngx_media_selector.h"

/*
 * RTMP transport module (goal doc 12).
 *
 * Publishers are registered as sources of the same logical streams the SRT
 * path uses, so a stream may be fed by SRT and RTMP encoders at the same time
 * and the selector stays protocol neutral.  Players consume the program feed:
 * the runtime converts the program to FLV once and every player shares those
 * payloads, adding only its own chunk headers (goal doc 12.1).
 */

#define NGX_MEDIA_RTMP_MAX_SESSIONS      32
#define NGX_MEDIA_RTMP_DEFAULT_PRIORITY  50
#define NGX_MEDIA_RTMP_OUT_CHUNK         4096
#define NGX_MEDIA_RTMP_MAX_OUT_QUEUE     64
#define NGX_MEDIA_RTMP_READ_BUFFER       16384
#define NGX_MEDIA_RTMP_PLAY_INTERVAL     40
#define NGX_MEDIA_RTMP_MAX_PLAY_BATCH    32
#define NGX_MEDIA_RTMP_AMF_BUFFER        1024

/* session states */
#define NGX_MEDIA_RTMP_STATE_HANDSHAKE   1
#define NGX_MEDIA_RTMP_STATE_COMMAND     2
#define NGX_MEDIA_RTMP_STATE_PUBLISHING  3
#define NGX_MEDIA_RTMP_STATE_PLAYING     4
#define NGX_MEDIA_RTMP_STATE_CLOSED      5

typedef struct {
    ngx_str_t   name;
    ngx_uint_t  priority;
} ngx_media_rtmp_priority_t;

typedef struct ngx_media_rtmp_session_s  ngx_media_rtmp_session_t;

struct ngx_media_rtmp_session_s {
    ngx_uint_t                    used;
    ngx_uint_t                    state;
    ngx_connection_t             *connection;

    ngx_ssl_connection_t         *ssl;         /* set when the peer is RTMPS */
    unsigned                       ssl_ready:1;

    ngx_media_rtmp_handshake_t    handshake;
    unsigned                      reply_sent:1;
    ngx_media_rtmp_reader_t       reader;
    ngx_media_rtmp_writer_t       writer;
    ngx_media_rtmp_publisher_t    publisher;

    u_char                        read_buffer[NGX_MEDIA_RTMP_READ_BUFFER];
    size_t                        pending_len;

    /* publishing */
    ngx_str_t                     app;
    ngx_str_t                     stream_name;
    ngx_media_stream_t           *stream;
    ngx_media_source_t           *source;
    ngx_uint_t                    stream_id;

    /* set when this worker does not own the stream and routes to the owner */
    unsigned                      routed:1;
    uint32_t                      routed_hash;
    uint64_t                      routed_sequence;

    /* playing */
    ngx_media_rtmp_prepare_t     *prepare;
    uint64_t                      cursor;
    ngx_uint_t                    out_queue;
    ngx_chain_t                  *out;
    ngx_chain_t                  *out_last;

    /*
     * Payload references held for messages that are still on the wire: the
     * chain nodes only carry chunk headers, media bytes stay in the shared
     * payload until the last byte of that message was sent (goal 12.1).
     */
    ngx_media_buf_t              *in_flight[NGX_MEDIA_RTMP_MAX_OUT_QUEUE];
    ngx_uint_t                    in_flight_head;
    ngx_uint_t                    in_flight_count;
    ngx_event_t                   play_timer;
    unsigned                      play_armed:1;

    ngx_log_t                    *log;
};

typedef struct {
    ngx_str_t                  listen;
    ngx_uint_t                 listen_set;
    ngx_array_t               *priorities;   /* ngx_media_rtmp_priority_t */

    /*
     * RTMPS: the same RTMP protocol inside a TLS session.  The listener is
     * our own socket rather than an http one, so the handshake is driven
     * here, through the same ngx_ssl_* API the http server uses.
     */
    ngx_ssl_t                  ssl;
    ngx_flag_t                 ssl_enabled;
    ngx_str_t                  ssl_certificate;
    ngx_str_t                  ssl_certificate_key;
} ngx_media_rtmp_main_conf_t;

static void *ngx_media_rtmp_create_conf(ngx_cycle_t *cycle);
static char *ngx_media_rtmp_listen_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_rtmp_ssl_certificate_cmd(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_uint_t ngx_media_rtmp_ssl_enabled(void);
static ngx_int_t ngx_media_rtmp_ssl_start(ngx_media_rtmp_session_t *session);
static void ngx_media_rtmp_ssl_ready(ngx_connection_t *c);
static char *ngx_media_rtmp_priority_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void ngx_media_rtmp_play_timer(ngx_event_t *ev);
static void ngx_media_rtmp_close_session(ngx_media_rtmp_session_t *session);
static ngx_chain_t *ngx_media_rtmp_chain_buf(ngx_media_rtmp_session_t *session,
    ngx_buf_t *b);
static ngx_int_t ngx_media_rtmp_init_process(ngx_cycle_t *cycle);
static void ngx_media_rtmp_exit_process(ngx_cycle_t *cycle);

static ngx_media_rtmp_session_t  ngx_media_rtmp_sessions[
    NGX_MEDIA_RTMP_MAX_SESSIONS];

static ngx_connection_t  *ngx_media_rtmp_listener;
static ngx_uint_t         ngx_media_rtmp_started;

static ngx_command_t ngx_media_rtmp_commands[] = {

    { ngx_string("media_rtmp_listen"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_rtmp_listen_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_rtmp_ssl"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_media_rtmp_main_conf_t, ssl_enabled),
      NULL },

    { ngx_string("media_rtmp_ssl_certificate"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_rtmp_ssl_certificate_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_rtmp_ssl_certificate_key"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_rtmp_ssl_certificate_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_rtmp_source_priority"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE2,
      ngx_media_rtmp_priority_cmd,
      0,
      0,
      NULL },

      ngx_null_command
};

static ngx_core_module_t ngx_media_rtmp_module_ctx = {
    ngx_string("media_rtmp"),
    ngx_media_rtmp_create_conf,
    NULL
};

ngx_module_t ngx_media_rtmp_module = {
    NGX_MODULE_V1,
    &ngx_media_rtmp_module_ctx,    /* module context */
    ngx_media_rtmp_commands,       /* module directives */
    NGX_CORE_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    ngx_media_rtmp_init_process,   /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    ngx_media_rtmp_exit_process,   /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

/* --- configuration ------------------------------------------------------- */

static void *
ngx_media_rtmp_create_conf(ngx_cycle_t *cycle)
{
    ngx_media_rtmp_main_conf_t  *mcf;

    mcf = ngx_pcalloc(cycle->pool, sizeof(ngx_media_rtmp_main_conf_t));

    if (mcf == NULL) {
        return NULL;
    }

    mcf->priorities = ngx_array_create(cycle->pool, 4,
                                       sizeof(ngx_media_rtmp_priority_t));

    if (mcf->priorities == NULL) {
        return NULL;
    }

    /*
     * ngx_conf_set_flag_slot refuses a slot that is not UNSET, and pcalloc
     * leaves it zero.  Seeding it with the default instead would make the
     * directive report "duplicate" on its first and only legal use, which is
     * exactly what it did.
     */
    mcf->ssl_enabled = NGX_CONF_UNSET;

    return mcf;
}

/*
 * media_rtmp_ssl_certificate <file>;
 * media_rtmp_ssl_certificate_key <file>;
 *
 * The TLS context is created on the second of the pair, when both paths are
 * known: ngx_ssl_certificate() needs them together, and creating it twice
 * would leak a context.
 */
static char *
ngx_media_rtmp_ssl_certificate_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_media_rtmp_main_conf_t  *mcf = conf;
    ngx_str_t                   *value = cf->args->elts;

    (void) cmd;

    if (ngx_strcmp(value[0].data, "media_rtmp_ssl_certificate_key") == 0) {
        if (mcf->ssl_certificate_key.len != 0) {
            return "duplicate media_rtmp_ssl_certificate_key";
        }

        mcf->ssl_certificate_key = value[1];

    } else {
        if (mcf->ssl_certificate.len != 0) {
            return "duplicate media_rtmp_ssl_certificate";
        }

        mcf->ssl_certificate = value[1];
    }

    if (mcf->ssl_certificate.len == 0 || mcf->ssl_certificate_key.len == 0) {
        return NGX_CONF_OK;
    }

    if (ngx_ssl_create(&mcf->ssl, NGX_SSL_TLSv1_2, NULL) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (ngx_ssl_certificate(cf, &mcf->ssl, &mcf->ssl_certificate,
                            &mcf->ssl_certificate_key, NULL) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    mcf->ssl_enabled = 1;

    return NGX_CONF_OK;
}

static char *
ngx_media_rtmp_listen_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_rtmp_main_conf_t  *mcf = conf;
    ngx_str_t                   *value = cf->args->elts;

    (void) cmd;

    if (value[1].len == 0) {
        return "must not be empty";
    }

    mcf->listen = value[1];
    mcf->listen_set = 1;

    return NGX_CONF_OK;
}

static char *
ngx_media_rtmp_priority_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_rtmp_main_conf_t  *mcf = conf;
    ngx_str_t                   *value = cf->args->elts;
    ngx_media_rtmp_priority_t   *entry;
    ngx_int_t                    priority;

    (void) cmd;

    if (value[1].len == 0) {
        return "source identity must not be empty";
    }

    priority = ngx_atoi(value[2].data, value[2].len);

    if (priority == NGX_ERROR || priority < 0 || priority > 1000) {
        return "priority must be a number between 0 and 1000";
    }

    entry = ngx_array_push(mcf->priorities);

    if (entry == NULL) {
        return NGX_CONF_ERROR;
    }

    entry->name = value[1];
    entry->priority = (ngx_uint_t) priority;

    return NGX_CONF_OK;
}

static ngx_uint_t
ngx_media_rtmp_priority(ngx_str_t *name)
{
    ngx_media_rtmp_main_conf_t  *mcf;
    ngx_media_rtmp_priority_t   *entries;
    ngx_uint_t                   i;

    mcf = (ngx_media_rtmp_main_conf_t *)
              ((ngx_cycle_t *) ngx_cycle)
                  ->conf_ctx[ngx_media_rtmp_module.index];

    if (mcf == NULL || mcf->priorities == NULL) {
        return NGX_MEDIA_RTMP_DEFAULT_PRIORITY;
    }

    entries = mcf->priorities->elts;

    for (i = 0; i < mcf->priorities->nelts; i++) {

        if (entries[i].name.len == name->len
            && ngx_memcmp(entries[i].name.data, name->data, name->len) == 0)
        {
            return entries[i].priority;
        }
    }

    return NGX_MEDIA_RTMP_DEFAULT_PRIORITY;
}

/* --- output queue -------------------------------------------------------- */

static ngx_media_rtmp_session_t *
ngx_media_rtmp_session_alloc(ngx_connection_t *c)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_RTMP_MAX_SESSIONS; i++) {

        if (!ngx_media_rtmp_sessions[i].used) {
            ngx_media_rtmp_session_t  *session = &ngx_media_rtmp_sessions[i];

            ngx_memzero(session, sizeof(ngx_media_rtmp_session_t));

            session->used = 1;
            session->state = NGX_MEDIA_RTMP_STATE_HANDSHAKE;
            session->connection = c;
            session->log = c->log;

            ngx_media_rtmp_handshake_init(&session->handshake);
            ngx_media_rtmp_reader_init(&session->reader);
            ngx_media_rtmp_writer_init(&session->writer,
                                       NGX_MEDIA_RTMP_OUT_CHUNK,
                                       NGX_MEDIA_RTMP_MAX_MESSAGE);
            (void) ngx_media_rtmp_publisher_init(&session->publisher, c->log);

            return session;
        }
    }

    return NULL;
}

static void
ngx_media_rtmp_session_close(ngx_media_rtmp_session_t *session)
{
    ngx_media_stream_t  *stream = session->stream;
    uint64_t             publisher_frames, publisher_configs;
    uint64_t             publisher_errors, publisher_skipped;

    if (session->play_timer.timer_set) {
        ngx_del_timer(&session->play_timer);
    }

    if (session->out != NULL) {
        ngx_free_chain(session->connection->pool, session->out);
        session->out = NULL;
        session->out_last = NULL;
    }

    while (session->in_flight_count > 0) {
        ngx_media_buf_unref(session->in_flight[session->in_flight_head]);
        session->in_flight_head = (session->in_flight_head + 1)
                                  % NGX_MEDIA_RTMP_MAX_OUT_QUEUE;
        session->in_flight_count--;
    }

    ngx_media_rtmp_reader_reset(&session->reader);
    /*
     * Read the counters before destroying the publisher: destroy zeroes the
     * struct, which made every close log read frames=0 configs=0 whatever
     * the session had actually carried.
     */
    publisher_frames = session->publisher.frames;
    publisher_configs = session->publisher.configs;
    publisher_errors = session->publisher.errors;
    publisher_skipped = session->publisher.skipped;

    ngx_media_rtmp_publisher_destroy(&session->publisher);

    if (session->routed) {
        (void) ngx_media_route_close((ngx_cycle_t *) ngx_cycle,
                                     session->routed_hash);

        ngx_log_error(NGX_LOG_NOTICE, session->log, 0,
                      "media: routed rtmp publisher closed hash=%uL",
                      session->routed_hash);
    }

    if (stream != NULL && session->source != NULL) {
        ngx_media_health_transport(&session->source->health, 0,
                                   ngx_current_msec);
        ngx_media_stream_source_remove(stream, session->source);

        ngx_log_error(NGX_LOG_NOTICE, session->log, 0,
                      "media: rtmp publisher closed stream=%V/%V frames=%uL "
                      "configs=%uL errors=%uL skipped=%uL generation=%ui",
                      &stream->application, &stream->name,
                      publisher_frames, publisher_configs, publisher_errors,
                      publisher_skipped, stream->generation);
    }

    if (session->stream_name.data != NULL) {
        ngx_log_error(NGX_LOG_NOTICE, session->log, 0,
                      "media: rtmp session closed stream=%V",
                      &session->stream_name);
    }

    if (session->connection != NULL) {
        ngx_media_rtmp_session_t  *slot = session;

        ngx_memzero(slot, sizeof(ngx_media_rtmp_session_t));
    }
}

/* queues raw bytes (the handshake reply is not a chunked message) */
static ngx_int_t
ngx_media_rtmp_queue_raw(ngx_media_rtmp_session_t *session, const u_char *data,
    size_t len)
{
    ngx_buf_t  *b;

    if (session->out_queue >= NGX_MEDIA_RTMP_MAX_OUT_QUEUE) {
        return NGX_AGAIN;
    }

    b = ngx_create_temp_buf(session->connection->pool, len);

    if (b == NULL) {
        return NGX_ERROR;
    }

    b->last = ngx_cpymem(b->last, data, len);

    if (ngx_media_rtmp_chain_buf(session, b) == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

/* builds one chain node holding an owned buffer */
static ngx_chain_t *
ngx_media_rtmp_chain_buf(ngx_media_rtmp_session_t *session, ngx_buf_t *b)
{
    ngx_chain_t  *cl;

    cl = ngx_alloc_chain_link(session->connection->pool);

    if (cl == NULL) {
        return NULL;
    }

    cl->buf = b;
    cl->next = NULL;

    if (session->out_last != NULL) {
        session->out_last->next = cl;

    } else {
        session->out = cl;
    }

    session->out_last = cl;
    session->out_queue++;

    return cl;
}

/*
 * Queues one RTMP message.  The chunk headers are copied once into a
 * connection pool buffer and the media bytes are referenced in place, in the
 * exact interleaving the writer produced: a multi chunk message alternates a
 * chunk header with its payload slice.  A receiver therefore pays for headers
 * only, and one reference per payload slice keeps the shared media alive until
 * that slice has been sent (goal doc 12.1).
 */
static ngx_int_t
ngx_media_rtmp_queue_message(ngx_media_rtmp_session_t *session,
    ngx_uint_t csid, ngx_uint_t type, ngx_uint_t stream_id, uint32_t timestamp,
    ngx_media_buf_t *payload, size_t offset, size_t len)
{
    ngx_media_rtmp_packet_t  packet;
    ngx_buf_t               *head;
    ngx_uint_t               i, slices;

    slices = (len > 0) ? 1 : 0;

    if (session->out_queue + slices * 2 + 1
        > NGX_MEDIA_RTMP_MAX_OUT_QUEUE
        || session->in_flight_count + slices > NGX_MEDIA_RTMP_MAX_OUT_QUEUE)
    {
        return NGX_AGAIN;
    }

    ngx_media_rtmp_packet_init(&packet);

    if (ngx_media_rtmp_writer_message(&session->writer, &packet, csid, type,
                                      stream_id, timestamp, payload, offset,
                                      len) != NGX_OK)
    {
        return NGX_ERROR;
    }

    /* one buffer holds every chunk header of this message */
    head = ngx_create_temp_buf(session->connection->pool, packet.head_len);

    if (head == NULL) {
        ngx_media_rtmp_packet_destroy(&packet);
        return NGX_ERROR;
    }

    head->last = ngx_cpymem(head->last, packet.head, packet.head_len);

    for (i = 0; i < packet.nparts; i++) {
        ngx_buf_t    *b;
        ngx_chain_t  *cl;

        if (packet.parts[i].data >= packet.head
            && packet.parts[i].data < packet.head + packet.head_len)
        {
            /* a chunk header: a window into the copied header buffer */
            b = ngx_calloc_buf(session->connection->pool);

            if (b == NULL) {
                ngx_media_rtmp_packet_destroy(&packet);
                return NGX_ERROR;
            }

            b->temporary = 1;
            b->start = head->start
                       + (packet.parts[i].data - packet.head);
            b->pos = b->start;
            b->end = b->start + packet.parts[i].len;
            b->last = b->end;

        } else {
            /* a payload slice: a reference into the shared buffer */
            b = ngx_calloc_buf(session->connection->pool);

            if (b == NULL) {
                ngx_media_rtmp_packet_destroy(&packet);
                return NGX_ERROR;
            }

            b->memory = 1;
            b->start = (u_char *) packet.parts[i].data;
            b->pos = b->start;
            b->end = b->start + packet.parts[i].len;
            b->last = b->end;

            session->in_flight[(session->in_flight_head
                                + session->in_flight_count)
                               % NGX_MEDIA_RTMP_MAX_OUT_QUEUE] =
                ngx_media_buf_ref(packet.payload);
            session->in_flight_count++;
        }

        cl = ngx_media_rtmp_chain_buf(session, b);

        if (cl == NULL) {
            ngx_media_rtmp_packet_destroy(&packet);
            return NGX_ERROR;
        }
    }

    if (session->out_last != NULL) {
        session->out_last->buf->flush = 1;
    }

    ngx_media_rtmp_packet_destroy(&packet);

    return NGX_OK;
}

static void
ngx_media_rtmp_flush(ngx_media_rtmp_session_t *session)
{
    ngx_connection_t  *c = session->connection;
    ngx_chain_t       *sent_tail, *cl, *next;

    if (session->out == NULL) {
        return;
    }

    /* send_chain() returns the part of the chain that is still unsent */
    sent_tail = c->send_chain(c, session->out, 0);


    if (sent_tail == NGX_CHAIN_ERROR) {
        ngx_media_rtmp_close_session(session);
        return;
    }

    /* every node before the tail went out completely */
    for (cl = session->out; cl != NULL && cl != sent_tail; cl = next) {
        next = cl->next;

        if (cl->buf->memory && session->in_flight_count > 0) {
            ngx_media_buf_unref(session->in_flight[session->in_flight_head]);
            session->in_flight_head = (session->in_flight_head + 1)
                                      % NGX_MEDIA_RTMP_MAX_OUT_QUEUE;
            session->in_flight_count--;
        }

        session->out_queue--;
    }

    session->out = sent_tail;

    /* the tail must follow the chain that is still pending */
    session->out_last = sent_tail;

    if (session->out_last != NULL) {
        while (session->out_last->next != NULL) {
            session->out_last = session->out_last->next;
        }

        if (!c->write->active) {
            (void) ngx_handle_write_event(c->write, 0);
        }
    }
}

/* --- commands ------------------------------------------------------------ */

static void
ngx_media_rtmp_send_control(ngx_media_rtmp_session_t *session,
    ngx_uint_t type, const void *body, size_t len)
{
    ngx_media_buf_t  *payload;
    u_char           *p;

    payload = ngx_media_buf_alloc(len ? len : 1);

    if (payload == NULL) {
        return;
    }

    p = ngx_media_buf_data(payload);

    if (len > 0 && body != NULL) {
        ngx_memcpy(p, body, len);
    }

    (void) ngx_media_buf_freeze(payload, len);

    (void) ngx_media_rtmp_queue_message(session, 2, type, 0, 0, payload, 0,
                                        len);

    ngx_media_buf_unref(payload);
}

static void
ngx_media_rtmp_send_amf(ngx_media_rtmp_session_t *session, ngx_uint_t stream_id,
    u_char *body, size_t len)
{
    ngx_media_buf_t  *payload;

    payload = ngx_media_buf_alloc(len);

    if (payload == NULL) {
        return;
    }

    ngx_memcpy(ngx_media_buf_data(payload), body, len);
    (void) ngx_media_buf_freeze(payload, len);

    (void) ngx_media_rtmp_queue_message(session, 3,
                                        NGX_MEDIA_RTMP_MSG_COMMAND_AMF0,
                                        stream_id, 0, payload, 0, len);

    ngx_media_buf_unref(payload);
}

/* _result with a status object, as every client expects after connect */
static void
ngx_media_rtmp_reply_result(ngx_media_rtmp_session_t *session,
    double transaction, const char *code, const char *level,
    const char *description, ngx_uint_t object_encoding)
{
    u_char                 buf[NGX_MEDIA_RTMP_AMF_BUFFER];
    ngx_media_amf_writer_t w;

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    if (ngx_media_amf_put_string(&w, (const u_char *) "_result", 7) != NGX_OK
        || ngx_media_amf_put_number(&w, transaction) != NGX_OK
        || ngx_media_amf_begin_object(&w, 0, 0) != NGX_OK
        || ngx_media_amf_put_member_string(&w, "level",
                                           (const u_char *) level,
                                           ngx_strlen(level)) != NGX_OK
        || ngx_media_amf_put_member_string(&w, "code",
                                           (const u_char *) code,
                                           ngx_strlen(code)) != NGX_OK
        || ngx_media_amf_put_member_string(&w, "description",
                                           (const u_char *) description,
                                           ngx_strlen(description)) != NGX_OK)
    {
        return;
    }

    if (object_encoding != (ngx_uint_t) -1
        && ngx_media_amf_put_member_number(&w, "objectEncoding",
                                           (double) object_encoding) != NGX_OK)
    {
        return;
    }

    if (ngx_media_amf_end_object(&w) != NGX_OK) {
        return;
    }

    ngx_media_rtmp_send_amf(session, 0, buf, w.len);
}

static void
ngx_media_rtmp_reply_create_stream(ngx_media_rtmp_session_t *session,
    double transaction)
{
    u_char                 buf[NGX_MEDIA_RTMP_AMF_BUFFER];
    ngx_media_amf_writer_t w;

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    if (ngx_media_amf_put_string(&w, (const u_char *) "_result", 7) != NGX_OK
        || ngx_media_amf_put_number(&w, transaction) != NGX_OK
        || ngx_media_amf_put_null(&w) != NGX_OK
        || ngx_media_amf_put_number(&w, 1) != NGX_OK)
    {
        return;
    }

    ngx_media_rtmp_send_amf(session, 0, buf, w.len);
}

/* onStatus for publish and play */
static void
ngx_media_rtmp_send_status(ngx_media_rtmp_session_t *session, double transaction,
    const char *level, const char *code, const char *description)
{
    u_char                 buf[NGX_MEDIA_RTMP_AMF_BUFFER];
    ngx_media_amf_writer_t w;

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    if (ngx_media_amf_put_string(&w, (const u_char *) "onStatus", 8) != NGX_OK
        || ngx_media_amf_put_number(&w, transaction) != NGX_OK
        || ngx_media_amf_put_null(&w) != NGX_OK
        || ngx_media_amf_begin_object(&w, 0, 0) != NGX_OK
        || ngx_media_amf_put_member_string(&w, "level",
                                           (const u_char *) level,
                                           ngx_strlen(level)) != NGX_OK
        || ngx_media_amf_put_member_string(&w, "code",
                                           (const u_char *) code,
                                           ngx_strlen(code)) != NGX_OK
        || ngx_media_amf_put_member_string(&w, "description",
                                           (const u_char *) description,
                                           ngx_strlen(description)) != NGX_OK
        || ngx_media_amf_end_object(&w) != NGX_OK)
    {
        return;
    }

    ngx_media_rtmp_send_amf(session, 1, buf, w.len);
}

/* --- publishing ---------------------------------------------------------- */

static ngx_int_t
ngx_media_rtmp_frame_cb(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_rtmp_session_t  *session = ctx;

    if (session->routed) {
        (void) ngx_media_route_frame((ngx_cycle_t *) ngx_cycle,
                                     session->routed_hash, frame,
                                     session->routed_sequence++);
        return NGX_OK;
    }

    if (session->stream == NULL || session->source == NULL) {
        return NGX_OK;
    }

    ngx_media_health_media(&session->source->health, frame->dts,
                           ngx_current_msec);

    ngx_media_runtime_iso_source(session->stream, session->source, frame);

    (void) ngx_media_stream_publish(session->stream, session->source, frame,
                                    ngx_current_msec);

    return NGX_OK;
}

static ngx_int_t
ngx_media_rtmp_tracks_cb(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_media_rtmp_session_t  *session = ctx;
    ngx_uint_t                 i;

    if (session->routed) {
        (void) ngx_media_route_tracks((ngx_cycle_t *) ngx_cycle,
                                      session->routed_hash, tracks);
        return NGX_OK;
    }

    if (session->source == NULL) {
        return NGX_OK;
    }

    (void) ngx_media_source_tracks_set(session->source, tracks, session->log);

    for (i = 0; i < tracks->count; i++) {

        if (tracks->tracks[i].media_type == NGX_MEDIA_TYPE_VIDEO) {
            session->source->has_video = 1;
        }
    }

    return NGX_OK;
}

/* splits "app/stream" (and query suffixes) into the routing keys */
static void
ngx_media_rtmp_split_name(ngx_str_t *app, ngx_str_t *name)
{
    u_char  *slash, *end;

    slash = ngx_strlchr(name->data, name->data + name->len, '/');

    if (slash == NULL) {
        app->data = name->data;
        app->len = 0;

        end = ngx_strlchr(name->data, name->data + name->len, '?');

        if (end != NULL) {
            name->len = (size_t) (end - name->data);
        }

        return;
    }

    app->data = name->data;
    app->len = (size_t) (slash - name->data);

    {
        u_char  *rest = slash + 1;
        size_t   rest_len = name->len - app->len - 1;

        end = ngx_strlchr(rest, rest + rest_len, '?');

        if (end != NULL) {
            rest_len = (size_t) (end - rest);
        }

        name->data = rest;
        name->len = rest_len;
    }
}

/* copies a name out of a message payload into the connection pool */
static ngx_int_t
ngx_media_rtmp_keep(ngx_media_rtmp_session_t *session, ngx_str_t *name)
{
    u_char  *p;

    p = ngx_pnalloc(session->connection->pool, name->len ? name->len : 1);

    if (p == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(p, name->data, name->len);

    name->data = p;

    return NGX_OK;
}

static ngx_int_t
ngx_media_rtmp_start_publish(ngx_media_rtmp_session_t *session,
    ngx_str_t *app, ngx_str_t *name, ngx_str_t *type)
{
    ngx_media_registry_t  *registry;
    ngx_media_stream_t    *stream;
    ngx_media_source_t    *source;
    ngx_media_feed_conf_t  feed_conf;

    if (name->len == 0) {
        ngx_media_rtmp_send_status(session, 0, "error",
                                   "NetStream.Publish.BadName",
                                   "no stream name");
        return NGX_DECLINED;
    }

    /*
     * The program lives on exactly one worker; a publisher landing elsewhere
     * is routed there over the bounded inter-worker transport (goal doc 22).
     */
    session->routed_hash = ngx_media_owner_hash(app, name);

    if (!ngx_media_route_is_owner((ngx_cycle_t *) ngx_cycle,
                                  session->routed_hash))
    {
        if (ngx_media_rtmp_keep(session, app) != NGX_OK
            || ngx_media_rtmp_keep(session, name) != NGX_OK
            || ngx_media_rtmp_keep(session, type) != NGX_OK)
        {
            return NGX_ERROR;
        }

        if (ngx_media_route_open((ngx_cycle_t *) ngx_cycle,
                                 session->routed_hash, app, name, name,
                                 NGX_MEDIA_SOURCE_RTMP,
                                 ngx_media_rtmp_priority(name)) != NGX_OK)
        {
            ngx_log_error(NGX_LOG_WARN, session->log, 0,
                          "media: could not route %V/%V to its owner worker",
                          app, name);
            return NGX_ERROR;
        }

        session->routed = 1;
        session->state = NGX_MEDIA_RTMP_STATE_PUBLISHING;

        ngx_log_error(NGX_LOG_NOTICE, session->log, 0,
                      "media: rtmp publisher routed to the owner stream=%V/%V",
                      app, name);

        ngx_media_rtmp_send_status(session, 0, "status",
                                   "NetStream.Publish.Start", "publishing");

        return NGX_OK;
    }

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        return NGX_ERROR;
    }

    if (ngx_media_rtmp_keep(session, app) != NGX_OK
        || ngx_media_rtmp_keep(session, name) != NGX_OK
        || ngx_media_rtmp_keep(session, type) != NGX_OK)
    {
        return NGX_ERROR;
    }

    feed_conf.max_units = 2048;
    feed_conf.max_bytes = 32 * 1024 * 1024;
    feed_conf.max_age = 10000;

    stream = ngx_media_registry_stream_create(registry, app, name, &feed_conf,
                                              session->log);

    if (stream == NULL) {
        return NGX_ERROR;
    }

    /* a reconnect of the same identity replaces the previous incarnation */
    source = ngx_media_stream_source_find(stream, name);

    if (source != NULL) {
        ngx_media_stream_source_remove(stream, source);
    }

    /* priority comes from trusted configuration, never from the client */
    source = ngx_media_stream_source_add(stream, name, NGX_MEDIA_SOURCE_RTMP,
                                         ngx_media_rtmp_priority(name),
                                         session->log);

    if (source == NULL) {
        return NGX_ERROR;
    }

    {
        ngx_media_policy_t  *policy;

        policy = ngx_media_policy_get((ngx_cycle_t *) ngx_cycle);

        if (policy != NULL) {
            ngx_media_stream_set_policy(stream, policy);
        }
    }

    ngx_media_health_init(&source->health, &stream->selector,
                          ngx_current_msec);
    ngx_media_health_transport(&source->health, 1, ngx_current_msec);

    if (stream->active == NULL) {
        (void) ngx_media_stream_promote(stream, source);
    }

    {
        ngx_media_owner_dir_t  *dir = ngx_media_runtime_owner_dir();

        if (dir != NULL) {
            (void) ngx_media_owner_dir_claim(dir,
                                             ngx_media_owner_hash(app, name),
                                             (ngx_uint_t) ngx_process_slot);
        }
    }

    session->stream = stream;
    session->source = source;
    session->state = NGX_MEDIA_RTMP_STATE_PUBLISHING;

    ngx_log_error(NGX_LOG_NOTICE, session->log, 0,
                  "media: rtmp publisher stream=%V/%V type=%V priority=%ui "
                  "sources=%ui",
                  app, name, type, ngx_media_rtmp_priority(name),
                  ngx_media_stream_source_count(stream));

    ngx_media_rtmp_send_status(session, 0, "status",
                               "NetStream.Publish.Start", "publishing");

    return NGX_OK;
}

static ngx_int_t
ngx_media_rtmp_start_play(ngx_media_rtmp_session_t *session, ngx_str_t *app,
    ngx_str_t *name)
{
    ngx_media_registry_t  *registry;
    ngx_media_stream_t    *stream;

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        return NGX_ERROR;
    }

    if (ngx_media_rtmp_keep(session, app) != NGX_OK
        || ngx_media_rtmp_keep(session, name) != NGX_OK)
    {
        return NGX_ERROR;
    }

    stream = ngx_media_registry_stream(registry, app, name);

    if (stream == NULL) {
        ngx_media_rtmp_send_status(session, 0, "error",
                                   "NetStream.Play.StreamNotFound",
                                   "no such stream");
        return NGX_DECLINED;
    }

    session->stream = stream;
    session->prepare = ngx_media_runtime_prepare(stream, session->log);
    session->state = NGX_MEDIA_RTMP_STATE_PLAYING;

    /* stream begin, then the status every client waits for */
    {
        u_char  begin[6];

        begin[0] = 0; begin[1] = 0;
        begin[2] = 0; begin[3] = 0;
        begin[4] = 0; begin[5] = 1;

        ngx_media_rtmp_send_control(session, NGX_MEDIA_RTMP_MSG_USER_CONTROL,
                                    begin, sizeof(begin));
    }

    ngx_media_rtmp_send_status(session, 0, "status", "NetStream.Play.Reset",
                               "playing");
    ngx_media_rtmp_send_status(session, 0, "status", "NetStream.Play.Start",
                               "playing");

    /* start the pump that drains the shared program fanout */
    ngx_memzero(&session->play_timer, sizeof(ngx_event_t));

    session->play_timer.handler = ngx_media_rtmp_play_timer;
    session->play_timer.log = session->log;
    session->play_timer.data = session;
    session->play_armed = 1;

    ngx_add_timer(&session->play_timer, NGX_MEDIA_RTMP_PLAY_INTERVAL);

    ngx_log_error(NGX_LOG_NOTICE, session->log, 0,
                  "media: rtmp player stream=%V/%V", app, name);

    return NGX_OK;
}

/* --- message handling ---------------------------------------------------- */

static ngx_int_t
ngx_media_rtmp_handle_command(ngx_media_rtmp_session_t *session,
    ngx_media_buf_t *payload)
{
    const u_char          *data = ngx_media_buf_data(payload);
    size_t                 len = ngx_media_buf_size(payload);
    size_t                 used = 0;
    ngx_media_amf_value_t  command, txn;
    ngx_int_t              rc;
    double                 transaction = 0;
    ngx_str_t             *name;

    rc = ngx_media_amf_read(data, len, &command, &used);

    if (rc != NGX_OK || command.type != NGX_MEDIA_AMF_STRING) {
        return NGX_OK;
    }

    data += used;
    len -= used;

    if (ngx_media_amf_read(data, len, &txn, &used) == NGX_OK) {
        data += used;
        len -= used;

        if (txn.type == NGX_MEDIA_AMF_NUMBER) {
            transaction = txn.number;
        }
    }

    name = &command.string;

#define NGX_MEDIA_RTMP_CMD(literal)                                           \
    (name->len == sizeof(literal) - 1                                          \
     && ngx_memcmp(name->data, literal, sizeof(literal) - 1) == 0)

    if (NGX_MEDIA_RTMP_CMD("connect")) {
        u_char                 body[NGX_MEDIA_RTMP_AMF_BUFFER];
        ngx_media_amf_writer_t w;
        ngx_media_amf_value_t  props;

        /*
         * Window acknowledgement size (4 bytes), set peer bandwidth
         * (4 byte window plus a one byte limit type) and the outbound chunk
         * size.  The body lengths are exact: a short body desynchronises
         * every client that follows the specification.
         */
        {
            u_char  window[4], bandwidth[5], chunk[4];

            window[0] = 0x00; window[1] = 0x26;
            window[2] = 0x25; window[3] = 0xA0;

            ngx_media_rtmp_send_control(session,
                                        NGX_MEDIA_RTMP_MSG_WINDOW_ACK, window,
                                        sizeof(window));

            ngx_memcpy(bandwidth, window, 4);
            bandwidth[4] = 2;    /* dynamic */

            ngx_media_rtmp_send_control(session,
                                        NGX_MEDIA_RTMP_MSG_PEER_BANDWIDTH,
                                        bandwidth, sizeof(bandwidth));

            chunk[0] = (u_char) ((NGX_MEDIA_RTMP_OUT_CHUNK >> 24) & 0xFF);
            chunk[1] = (u_char) ((NGX_MEDIA_RTMP_OUT_CHUNK >> 16) & 0xFF);
            chunk[2] = (u_char) ((NGX_MEDIA_RTMP_OUT_CHUNK >> 8) & 0xFF);
            chunk[3] = (u_char) (NGX_MEDIA_RTMP_OUT_CHUNK & 0xFF);

            ngx_media_rtmp_send_control(session,
                                        NGX_MEDIA_RTMP_MSG_CHUNK_SIZE, chunk,
                                        sizeof(chunk));
        }

        /* remember the application name for later publish/play commands */
        if (ngx_media_amf_read(data, len, &props, &used) == NGX_OK) {

            if (props.type == NGX_MEDIA_AMF_OBJECT) {
                const ngx_str_t  *app = ngx_media_amf_member(&props, "app");

                if (app != NULL) {
                    session->app.len = app->len;
                    session->app.data = ngx_pnalloc(session->connection->pool,
                                                    app->len);
                    if (session->app.data != NULL) {
                        ngx_memcpy(session->app.data, app->data, app->len);
                    } else {
                        session->app.len = 0;
                    }
                }
            }
        }

        ngx_media_amf_writer_init(&w, body, sizeof(body));

        if (ngx_media_amf_put_string(&w, (const u_char *) "_result", 7)
            == NGX_OK
            && ngx_media_amf_put_number(&w, transaction) == NGX_OK
            && ngx_media_amf_begin_object(&w, 0, 0) == NGX_OK
            && ngx_media_amf_put_member_string(&w, "fmsVer",
                                               (const u_char *) "FMS/3,5,7,7009",
                                               14) == NGX_OK
            && ngx_media_amf_put_member_string(&w, "level",
                                               (const u_char *) "status", 6)
               == NGX_OK
            && ngx_media_amf_put_member_string(
                   &w, "code", (const u_char *) "NetConnection.Connect.Success",
                   29) == NGX_OK
            && ngx_media_amf_put_member_number(&w, "objectEncoding", 0)
               == NGX_OK
            && ngx_media_amf_end_object(&w) == NGX_OK)
        {
            ngx_media_rtmp_send_amf(session, 0, body, w.len);
        }

        session->state = NGX_MEDIA_RTMP_STATE_COMMAND;

        return NGX_OK;
    }

    if (NGX_MEDIA_RTMP_CMD("releaseStream")
        || NGX_MEDIA_RTMP_CMD("FCPublish"))
    {
        ngx_media_rtmp_reply_result(session, transaction,
                                    "NetStream.Publish.Start", "status",
                                    "ok", (ngx_uint_t) -1);
        return NGX_OK;
    }

    if (NGX_MEDIA_RTMP_CMD("createStream")) {
        ngx_media_rtmp_reply_create_stream(session, transaction);
        return NGX_OK;
    }

    if (NGX_MEDIA_RTMP_CMD("publish")) {
        ngx_media_amf_value_t  name_value, type_value;
        ngx_str_t              publish_name;
        ngx_str_t              publish_type;
        ngx_uint_t             have_name = 0;

        publish_type.data = (u_char *) "live";
        publish_type.len = 4;
        publish_name.data = NULL;
        publish_name.len = 0;

        ngx_memzero(&name_value, sizeof(name_value));
        ngx_memzero(&type_value, sizeof(type_value));

        /* [null] [stream name] [publish type] */
        if (ngx_media_amf_read(data, len, &name_value, &used) == NGX_OK
            && name_value.type == NGX_MEDIA_AMF_NULL)
        {
            data += used;
            len -= used;
        }

        if (ngx_media_amf_read(data, len, &name_value, &used) == NGX_OK) {
            data += used;
            len -= used;

            if (name_value.type == NGX_MEDIA_AMF_STRING) {
                publish_name = name_value.string;
                have_name = 1;
            }
        }

        if (ngx_media_amf_read(data, len, &type_value, &used) == NGX_OK
            && type_value.type == NGX_MEDIA_AMF_STRING)
        {
            publish_type = type_value.string;
        }

        if (!have_name) {
            ngx_media_rtmp_send_status(session, 0, "error",
                                       "NetStream.Publish.BadName",
                                       "no stream name");
            return NGX_OK;
        }

        {
            ngx_str_t  app = session->app;
            ngx_str_t  stream_name = publish_name;

            if (app.len == 0) {
                /* clients may put "app/stream" in the publish name */
                ngx_media_rtmp_split_name(&app, &stream_name);
            }

            if (ngx_media_rtmp_start_publish(session, &app, &stream_name,
                                             &publish_type) == NGX_OK)
            {
                session->stream_name = stream_name;
            }
        }

        return NGX_OK;
    }

    if (NGX_MEDIA_RTMP_CMD("play")) {
        ngx_media_amf_value_t  v;

        ngx_memzero(&v, sizeof(v));

        if (ngx_media_amf_read(data, len, &v, &used) == NGX_OK
            && v.type == NGX_MEDIA_AMF_NULL)
        {
            data += used;
            len -= used;
            (void) ngx_media_amf_read(data, len, &v, &used);
        }

        if (v.type == NGX_MEDIA_AMF_STRING) {
            ngx_str_t  app = session->app;
            ngx_str_t  play_name = v.string;

            if (app.len == 0) {
                ngx_media_rtmp_split_name(&app, &play_name);
            }

            (void) ngx_media_rtmp_start_play(session, &app, &play_name);
        }

        return NGX_OK;
    }

    if (NGX_MEDIA_RTMP_CMD("deleteStream")
        || NGX_MEDIA_RTMP_CMD("closeStream")
        || NGX_MEDIA_RTMP_CMD("FCUnpublish"))
    {
        session->state = NGX_MEDIA_RTMP_STATE_COMMAND;
        return NGX_OK;
    }

    if (NGX_MEDIA_RTMP_CMD("_checkbw") || NGX_MEDIA_RTMP_CMD("FCSubscribe")) {
        ngx_media_rtmp_reply_result(session, transaction, "NetStream.Play.Start",
                                    "status", "ok", (ngx_uint_t) -1);
        return NGX_OK;
    }

#undef NGX_MEDIA_RTMP_CMD

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, session->log, 0,
                   "media: rtmp ignoring command %V", name);

    return NGX_OK;
}

static ngx_int_t
ngx_media_rtmp_on_message(void *ctx, ngx_uint_t type, ngx_uint_t stream_id,
    uint32_t timestamp, ngx_media_buf_t *payload)
{
    ngx_media_rtmp_session_t  *session = ctx;

    switch (type) {

    case NGX_MEDIA_RTMP_MSG_COMMAND_AMF0:
        return ngx_media_rtmp_handle_command(session, payload);

    case NGX_MEDIA_RTMP_MSG_AUDIO:
    case NGX_MEDIA_RTMP_MSG_VIDEO:

        if (session->state != NGX_MEDIA_RTMP_STATE_PUBLISHING) {
            return NGX_OK;
        }

        return ngx_media_rtmp_publisher_feed(&session->publisher, type,
                                             timestamp,
                                             ngx_media_buf_data(payload),
                                             ngx_media_buf_size(payload),
                                             ngx_media_rtmp_frame_cb,
                                             ngx_media_rtmp_tracks_cb, session);

    case NGX_MEDIA_RTMP_MSG_USER_CONTROL:

        if (ngx_media_buf_size(payload) >= 6
            && ngx_media_buf_data(payload)[0] == 0
            && ngx_media_buf_data(payload)[1] == 6)
        {
            /* ping request: answer with a ping response */
            u_char  body[6];

            ngx_memcpy(body, ngx_media_buf_data(payload), 6);
            body[1] = NGX_MEDIA_RTMP_EVENT_PING_RESPONSE;

            ngx_media_rtmp_send_control(session,
                                        NGX_MEDIA_RTMP_MSG_USER_CONTROL, body,
                                        6);
        }

        return NGX_OK;

    default:
        return NGX_OK;
    }

    (void) stream_id;
}

/* --- player pump --------------------------------------------------------- */

static void
ngx_media_rtmp_play_timer(ngx_event_t *ev)
{
    ngx_media_rtmp_session_t      *session = ev->data;
    const ngx_media_rtmp_media_t  *unit;
    ngx_uint_t                     sent = 0;

    if (session->state != NGX_MEDIA_RTMP_STATE_PLAYING
        || session->prepare == NULL)
    {
        return;
    }

    /* one visit is bounded (goal doc 34 item 13) */
    while (sent < NGX_MEDIA_RTMP_MAX_PLAY_BATCH
           && session->out_queue < NGX_MEDIA_RTMP_MAX_OUT_QUEUE)
    {
        unit = ngx_media_rtmp_fanout_next(&session->prepare->fan,
                                          session->cursor);

        if (unit == NULL) {
            break;
        }

        {
            ngx_int_t  qrc;

            qrc = ngx_media_rtmp_queue_message(session, 4, unit->type, 1,
                                               unit->timestamp, unit->payload, 0,
                                               ngx_media_buf_size(unit->payload));

            if (qrc != NGX_OK) {
                break;
            }
        }

        session->cursor = unit->sequence + 1;
        sent++;
    }

    ngx_media_rtmp_flush(session);

    if (session->used && session->state == NGX_MEDIA_RTMP_STATE_PLAYING) {
        ngx_add_timer(ev, NGX_MEDIA_RTMP_PLAY_INTERVAL);
    }
}

/* --- connection handling ------------------------------------------------- */

static void
ngx_media_rtmp_close_session(ngx_media_rtmp_session_t *session)
{
    ngx_connection_t  *c = session->connection;

    ngx_media_rtmp_session_close(session);

    if (c != NULL) {
        c->data = NULL;
        ngx_close_connection(c);
    }
}

static void
ngx_media_rtmp_read_handler(ngx_event_t *ev)
{
    ngx_connection_t          *c = ev->data;
    ngx_media_rtmp_session_t  *session = c->data;
    ssize_t                    n;
    size_t                     consumed, take;

    if (session == NULL) {
        return;
    }

    if (session->ssl != NULL && !session->ssl_ready) {
        /* ngx_ssl_handshake_handler() owns the connection until it finishes */
        return;
    }

    for ( ;; ) {
        take = sizeof(session->read_buffer) - session->pending_len;

        if (take == 0) {
            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "media: rtmp: message header exceeds the read "
                          "buffer");
            ngx_media_rtmp_close_session(session);
            return;
        }

        n = c->recv(c, session->read_buffer + session->pending_len, take);


        if (n == NGX_AGAIN) {
            break;
        }

        if (n == 0) {
            ngx_media_rtmp_close_session(session);
            return;
        }

        if (n == NGX_ERROR) {
            ngx_media_rtmp_close_session(session);
            return;
        }

        session->pending_len += (size_t) n;

        /* handshake first, then the chunk stream */
        if (session->state == NGX_MEDIA_RTMP_STATE_HANDSHAKE) {
            ngx_int_t  hrc;

            hrc = ngx_media_rtmp_handshake_feed(&session->handshake,
                                                session->read_buffer,
                                                session->pending_len,
                                                &consumed);

            if (consumed > 0) {
                ngx_memmove(session->read_buffer,
                            session->read_buffer + consumed,
                            session->pending_len - consumed);
                session->pending_len -= consumed;
            }

            if (hrc == NGX_ERROR) {
                ngx_log_error(NGX_LOG_WARN, c->log, 0,
                              "media: rtmp: handshake failed");
                ngx_media_rtmp_close_session(session);
                return;
            }

            /*
             * S0S1S2 is ready as soon as C1 arrived: the client only sends C2
             * after reading the reply, so waiting for C2 would deadlock.
             */
            if (session->handshake.out_len > 0 && !session->reply_sent) {
                u_char  ack[4];

                session->reply_sent = 1;

                (void) ngx_media_rtmp_queue_raw(session, session->handshake.out,
                                                session->handshake.out_len);

                ack[0] = 0x00; ack[1] = 0x26; ack[2] = 0x25; ack[3] = 0xA0;

                ngx_media_rtmp_send_control(session,
                                            NGX_MEDIA_RTMP_MSG_WINDOW_ACK, ack,
                                            sizeof(ack));

                ngx_media_rtmp_flush(session);

                if (!session->used) {
                    return;
                }
            }

            if (hrc == NGX_OK) {
                session->state = NGX_MEDIA_RTMP_STATE_COMMAND;
            }
        }

        if (session->state != NGX_MEDIA_RTMP_STATE_HANDSHAKE
            && session->pending_len > 0)
        {
            if (ngx_media_rtmp_reader_feed(&session->reader,
                                           session->read_buffer,
                                           session->pending_len, &consumed,
                                           ngx_media_rtmp_on_message,
                                           session) != NGX_OK)
            {
                ngx_media_rtmp_close_session(session);
                return;
            }

            if (consumed > 0) {
                ngx_memmove(session->read_buffer,
                            session->read_buffer + consumed,
                            session->pending_len - consumed);
                session->pending_len -= consumed;
            }
        }

        ngx_media_rtmp_flush(session);

        if (!session->used) {
            return;
        }
    }

    if (session->out != NULL) {
        ngx_media_rtmp_flush(session);
    }
}

static void
ngx_media_rtmp_write_handler(ngx_event_t *ev)
{
    ngx_connection_t          *c = ev->data;
    ngx_media_rtmp_session_t  *session = c->data;

    if (session == NULL) {
        return;
    }

    ngx_media_rtmp_flush(session);
}

static void
ngx_media_rtmp_accept_handler(ngx_event_t *ev)
{
    ngx_connection_t          *c = ev->data;

    ngx_media_rtmp_session_t  *session;
    ngx_connection_t          *nc;
    ngx_int_t                  fd;
    struct sockaddr            sa;
    socklen_t                  len = sizeof(struct sockaddr);

    for ( ;; ) {
        fd = accept(c->fd, &sa, &len);

        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            if (errno == EINTR) {
                continue;
            }

            ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                          "media: rtmp accept failed");
            return;
        }

        if (ngx_nonblocking(fd) == -1) {
            ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                          "media: rtmp cannot set the accepted socket "
                          "non-blocking");
            (void) close(fd);
            continue;
        }

        nc = ngx_get_connection(fd, c->log);

        if (nc == NULL) {
            (void) close(fd);
            continue;
        }

        nc->log = c->log;

        /* ngx_get_connection() leaves these to the accept path we bypass */
        nc->recv = ngx_recv;
        nc->send = ngx_send;
        nc->recv_chain = ngx_recv_chain;
        nc->send_chain = ngx_send_chain;

        nc->pool = ngx_create_pool(4096, c->log);

        if (nc->pool == NULL) {
            ngx_close_connection(nc);
            continue;
        }

        session = ngx_media_rtmp_session_alloc(nc);

        if (session == NULL) {
            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "media: rtmp: no free session slot");
            ngx_close_connection(nc);
            continue;
        }

        nc->data = session;
        nc->read->handler = ngx_media_rtmp_read_handler;
        nc->read->log = c->log;
        nc->write->handler = ngx_media_rtmp_write_handler;
        nc->write->log = c->log;

        if (ngx_add_conn(nc) != NGX_OK) {
            ngx_media_rtmp_close_session(session);
            continue;
        }

        if (ngx_media_rtmp_ssl_enabled()) {
            /*
             * RTMPS: the peer speaks RTMP inside TLS.  The handshake is
             * non-blocking, so it is driven from the connection handlers and
             * nothing else runs until it completes.
             */
            if (ngx_media_rtmp_ssl_start(session) != NGX_OK) {
                ngx_log_error(NGX_LOG_WARN, nc->log, 0,
                              "media: rtmps session could not be started");
                ngx_media_rtmp_close_session(session);
                continue;
            }

            ngx_log_error(NGX_LOG_INFO, nc->log, 0,
                          "media: rtmps handshake started");
        }

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "media: rtmp connection accepted");
    }
}

/* --- rtmps --------------------------------------------------------------- */

static ngx_media_rtmp_main_conf_t *
ngx_media_rtmp_conf(void)
{
    return (ngx_media_rtmp_main_conf_t *)
               ((ngx_cycle_t *) ngx_cycle)
                   ->conf_ctx[ngx_media_rtmp_module.index];
}

static ngx_uint_t
ngx_media_rtmp_ssl_enabled(void)
{
    ngx_media_rtmp_main_conf_t  *mcf = ngx_media_rtmp_conf();

    if (mcf == NULL) {
        return 0;
    }

    /*
     * The slot starts UNSET so ngx_conf_set_flag_slot can own it, and
     * configuration has been parsed by the time anyone asks.  Returning the
     * raw value would make the default -1, which is truthy: every plain RTMP
     * listener would then try to speak TLS.
     */
    if (mcf->ssl_enabled == NGX_CONF_UNSET) {
        mcf->ssl_enabled = 0;
    }

    return (ngx_uint_t) mcf->ssl_enabled;
}

/*
 * Called by nginx when the TLS handshake finishes.  The RTMP handshake is
 * already waiting in the SSL buffer, so the read path has to be restarted:
 * nothing else will wake it.
 */
static void
ngx_media_rtmp_ssl_ready(ngx_connection_t *c)
{
    ngx_media_rtmp_session_t  *session = c->data;

    if (session == NULL) {
        ngx_close_connection(c);
        return;
    }

    if (session->ssl_ready) {
        /* the first pass and the event callback can both get here */
        return;
    }

    session->ssl_ready = 1;

    ngx_log_error(NGX_LOG_INFO, c->log, 0, "media: rtmps handshake complete");

    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_media_rtmp_close_session(session);
        return;
    }

    /*
     * ngx_ssl_create_connection() installed ngx_ssl_handshake_handler() as the
     * connection handlers, so they have to be put back: otherwise every later
     * event dispatches into the handshake handler again, which calls this
     * callback again and reads nothing.
     */
    c->read->handler = ngx_media_rtmp_read_handler;
    c->write->handler = ngx_media_rtmp_write_handler;

    /*
     * The peer's RTMP handshake may already be in the SSL buffer, in which
     * case the socket will not become readable again.  Mark the event ready
     * and run the read path directly, as nginx's own SSL handler does.
     */
    c->read->ready = 1;
    ngx_media_rtmp_read_handler(c->read);
}

static ngx_int_t
ngx_media_rtmp_ssl_start(ngx_media_rtmp_session_t *session)
{
    ngx_media_rtmp_main_conf_t  *mcf = ngx_media_rtmp_conf();

    if (mcf == NULL) {
        return NGX_ERROR;
    }

    if (ngx_ssl_create_connection(&mcf->ssl, session->connection,
                                  NGX_SSL_BUFFER) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_WARN, session->connection->log, 0,
                      "media: rtmps: ngx_ssl_create_connection failed "
                      "(ssl ctx=%p)", mcf->ssl.ctx);
        return NGX_ERROR;
    }

    /*
     * ngx_ssl_create_connection() installs ngx_ssl_handshake_handler() as the
     * connection handlers and leaves this callback to the caller.  Leaving it
     * unset is a call through a NULL pointer the moment the handshake
     * completes, which is exactly how this crashed.
     */
    session->connection->ssl->handler = ngx_media_rtmp_ssl_ready;
    session->ssl = session->connection->ssl;

    /*
     * ngx_ssl_create_connection() installs ngx_ssl_handshake_handler() so the
     * handshake can be *continued* on events, but something has to start it.
     * nginx's http path calls ngx_ssl_handshake() at this point for exactly
     * that reason; without it the handler never runs and both peers wait.
     */
    switch (ngx_ssl_handshake(session->connection)) {

    case NGX_OK:
        /* finished in one pass: the handler is not called back */
        ngx_media_rtmp_ssl_ready(session->connection);
        return NGX_OK;

    case NGX_AGAIN:
        return NGX_OK;

    default:
        ngx_log_error(NGX_LOG_WARN, session->connection->log, 0,
                      "media: rtmps handshake failed to start");
        return NGX_ERROR;
    }
}

/* --- module hooks -------------------------------------------------------- */

static ngx_int_t
ngx_media_rtmp_init_process(ngx_cycle_t *cycle)
{
    ngx_media_rtmp_main_conf_t  *mcf;
    ngx_connection_t            *c;
    ngx_sockaddr_t               sa;
    socklen_t                    socklen;
    ngx_int_t                    fd;
    ngx_int_t                    rc;
    ngx_str_t                    host, port_text;
    ngx_int_t                    port;
    u_char                      *colon;

    mcf = (ngx_media_rtmp_main_conf_t *)
              cycle->conf_ctx[ngx_media_rtmp_module.index];

    /* every worker adopts the owner directory, routing and program runtime */
    if (ngx_media_runtime_init(cycle, cycle->log) != NGX_OK) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: could not initialise the program runtime");
        return NGX_ERROR;
    }

    (void) ngx_media_runtime_arm(cycle, cycle->log);

    if (mcf == NULL || !mcf->listen_set) {
        return NGX_OK;
    }

    /* transport sockets stay with worker 0 (goal doc 22) */
    if (ngx_process_slot != 0) {
        return NGX_OK;
    }

    colon = ngx_strlchr(mcf->listen.data, mcf->listen.data + mcf->listen.len,
                        ':');

    if (colon == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: invalid media_rtmp_listen \"%V\"",
                      &mcf->listen);
        return NGX_ERROR;
    }

    host.data = mcf->listen.data;
    host.len = (size_t) (colon - mcf->listen.data);
    port_text.data = colon + 1;
    port_text.len = mcf->listen.len - host.len - 1;

    port = ngx_atoi(port_text.data, port_text.len);

    if (port < 1 || port > 65535) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: invalid media_rtmp_listen \"%V\"", &mcf->listen);
        return NGX_ERROR;
    }

    ngx_memzero(&sa, sizeof(sa));

    sa.sockaddr_in.sin_family = AF_INET;
    sa.sockaddr_in.sin_port = htons((in_port_t) port);
    sa.sockaddr_in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (host.len > 0 && !(host.len == 9
                          && ngx_strncmp(host.data, "127.0.0.1", 9) == 0))
    {
        if (ngx_inet_addr(host.data, host.len) == INADDR_NONE) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "media: media_rtmp_listen host must be an IPv4 "
                          "address");
            return NGX_ERROR;
        }

        sa.sockaddr_in.sin_addr.s_addr = ngx_inet_addr(host.data, host.len);
    }

    socklen = sizeof(struct sockaddr_in);

    fd = ngx_socket(AF_INET, SOCK_STREAM, 0);

    if (fd == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                      "media: rtmp socket() failed");
        return NGX_ERROR;
    }

    if (ngx_nonblocking(fd) == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                      "media: rtmp cannot set the listener non-blocking");
        (void) ngx_close_socket(fd);
        return NGX_ERROR;
    }

    {
        int  on = 1;

        (void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *) &on,
                          sizeof(int));
    }

    rc = bind(fd, &sa.sockaddr, socklen);

    if (rc == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                      "media: rtmp bind(%V) failed", &mcf->listen);
        (void) ngx_close_socket(fd);
        return NGX_ERROR;
    }

    if (listen(fd, 128) == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                      "media: rtmp listen() failed");
        (void) ngx_close_socket(fd);
        return NGX_ERROR;
    }

    c = ngx_get_connection(fd, cycle->log);

    if (c == NULL) {
        (void) ngx_close_socket(fd);
        return NGX_ERROR;
    }

    c->log = cycle->log;
    c->read->handler = ngx_media_rtmp_accept_handler;
    c->read->log = cycle->log;
    c->data = c;

    if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
        ngx_free_connection(c);
        (void) ngx_close_socket(fd);
        return NGX_ERROR;
    }

    ngx_media_rtmp_listener = c;
    ngx_media_rtmp_started = 1;


    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  "media: rtmp listener ready on %V", &mcf->listen);

    return NGX_OK;
}

static void
ngx_media_rtmp_exit_process(ngx_cycle_t *cycle)
{
    ngx_uint_t  i;

    if (!ngx_media_rtmp_started) {
        return;
    }

    ngx_media_rtmp_started = 0;

    for (i = 0; i < NGX_MEDIA_RTMP_MAX_SESSIONS; i++) {

        if (ngx_media_rtmp_sessions[i].used) {
            ngx_media_rtmp_session_close(&ngx_media_rtmp_sessions[i]);
        }
    }

    if (ngx_media_rtmp_listener != NULL) {
        (void) ngx_del_event(ngx_media_rtmp_listener->read, NGX_READ_EVENT, 0);
        (void) ngx_close_socket(ngx_media_rtmp_listener->fd);
        ngx_media_rtmp_listener->fd = (ngx_socket_t) -1;
        ngx_free_connection(ngx_media_rtmp_listener);
        ngx_media_rtmp_listener = NULL;
    }

    ngx_media_runtime_shutdown(cycle->log);
}

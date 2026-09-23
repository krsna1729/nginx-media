/*
 * nginx-media SRT ingest module (goal doc 11.2, 11.3).
 *
 * Static configuration for now is a listening capability:
 *
 *     media_srt_listen 127.0.0.1:9000;
 *
 * and, when that listener must also accept group (bonded) callers, the second
 * local address the other leg arrives on (goal doc 11.4):
 *
 *     media_srt_listen_bond 127.0.0.2;
 *
 * One listening endpoint per worker (goal doc 22).  The directive may be
 * given once per worker and worker i binds the i-th entry, so a publisher
 * that connects to entry i is accepted by worker i and only routed to the
 * program's owner when that is a different worker.  A single entry keeps the
 * original shape: worker 0 owns the listener, and routing is the only path
 * for a publisher whose program lives elsewhere.  libsrt has no reuseport of
 * its own, so one endpoint per worker is what replaces it - a single shared
 * endpoint would funnel every publisher through the worker that bound it.
 *
 * Or the endpoint is shared, when the configuration asks for that:
 *
 *     media_srt_listen_shared 127.0.0.1:9000;
 *
 * Every worker then binds that one address on a UDP socket it creates itself
 * with SO_REUSEPORT set and hands to libsrt with srt_bind_acquire(), and the
 * kernel spreads incoming publishers across those sockets by 4-tuple - so a
 * publisher arrives on whichever worker the kernel picked and is routed to
 * the program's owner from there.  It is a separate directive, not a flag on
 * media_srt_listen, because the mode belongs to the whole listener set rather
 * than to one entry, and it is refused beside media_srt_listen and
 * media_srt_listen_bond.  A bonded caller cannot be served this way at all:
 * its legs have different source addresses by construction, so they hash to
 * different workers, and the library's mirror-group registry is process
 * state, so those workers cannot join the legs into one bond.  And the set of
 * processes bound to the port must not change while publishers are connected:
 * a reload changes it twice - the new generation binds before the old one
 * exits - and every flow that re-hashes lands on a worker with no state for
 * it.  docs/operations.md has the measurements; the mode logs a warning at
 * startup and does not change what either of the other two directives means.
 *
 * Each worker's transport helper thread runs one shared poll over its
 * listener and every session, and hands compact events plus raw MPEG-TS
 * chunks (tagged with their session) to the worker through an eventfd.  The
 * worker parses and validates the Stream ID, registers the publisher as a
 * source of a logical stream, demuxes its MPEG-TS into encoded frames and
 * publishes them through the activation gate into the program feed.
 */

#include "ngx_media_platform.h"
#include "ngx_media_registry.h"
#include "ngx_media_route.h"
#include "ngx_media_runtime.h"
#include "ngx_media_destination.h"
#include "ngx_media_srt_output.h"

/*
 * The backends media_srt_backend can select.  Both are weak: a build may link
 * either, both, or neither, and the directive reports which ones it carries.
 */
extern ngx_media_srt_ops_t  ngx_media_srt_haivision_ops
    __attribute__((weak));
extern ngx_media_srt_ops_t  ngx_media_srt_udp_ops
    __attribute__((weak));
#include "ngx_media_selector.h"
#include "ngx_media_srt_ingest.h"
#include "ngx_media_ts_demux.h"

#include <ngx_event.h>

#define NGX_MEDIA_SRT_MAX_PRIORITIES 16

typedef struct {
    ngx_str_t   id;        /* publisher identity from the stream id */
    ngx_uint_t  priority;  /* trusted operator configuration */
} ngx_media_srt_priority_t;

/*
 * SRT encryption (goal doc 11).  The passphrase is stored here and the
 * params struct points at it, so the pointer handed to the transport stays
 * valid for the life of the configuration.
 *
 * Scope: one listener accepts many publishers, and the passphrase is a
 * connection parameter -- the stream id only becomes readable *after* the
 * handshake.  A per-source passphrase is therefore impossible on the ingest
 * side: the listener carries one passphrase (the global setting).  Stream and
 * destination scope apply where they can be honoured, on the output side.
 */
typedef struct {
    ngx_str_t                passphrase;
    ngx_uint_t               mode;       /* NGX_MEDIA_SRT_CRYPTO_CTR|GCM */
    ngx_uint_t               pbkeylen;   /* 0: library default */
    ngx_uint_t               enforced;
    unsigned                 set:1;
} ngx_media_srt_crypto_conf_t;

#define NGX_MEDIA_SRT_MAX_CRYPTO_STREAMS  16

typedef struct {
    ngx_str_t                      name;     /* application/stream */
    ngx_media_srt_crypto_conf_t    crypto;
} ngx_media_srt_stream_crypto_t;

/*
 * One listening endpoint.  media_srt_listen may be repeated, once per worker:
 * worker i binds the i-th entry, and this worker's endpoint is the only one
 * it ever binds.
 */
typedef struct {
    ngx_str_t   endpoint;   /* host:port as written in the configuration */
} ngx_media_srt_listen_t;

typedef struct {
    ngx_array_t              *listens;   /* ngx_media_srt_listen_t */

    /*
     * The one endpoint every worker binds (media_srt_listen_shared).  It is a
     * directive of its own rather than a flag on media_srt_listen because the
     * mode belongs to the whole listener set, not to one entry: with a flag an
     * operator could write one shared entry beside per-worker entries, and
     * "worker i binds entry i" would no longer say which entry that is.  Held
     * here because exactly one is legal, and refused beside the two directives
     * whose meaning it would otherwise contradict.
     *
     * Binding it needs SO_REUSEPORT on a socket this module creates, which is
     * a libsrt backend capability: on a backend without one the worker
     * reports that the listener could not be bound and keeps retrying.
     */
    ngx_str_t                shared;
    unsigned                 shared_set:1;

    /*
     * The second local address of a bonded listener (goal doc 11.4).  Bonding
     * is transport-internal: this address is bound with group acceptance
     * enabled so a group caller's legs become one session, and nothing above
     * the transport ever sees a bond member.
     */
    ngx_str_t                bond;
    unsigned                 bond_set:1;
    ngx_media_srt_priority_t priorities[NGX_MEDIA_SRT_MAX_PRIORITIES];
    ngx_uint_t               npriorities;

    /* SRT destinations fed from the shared program preparation */
    ngx_media_srt_output_conf_t  outputs[NGX_MEDIA_SRT_MAX_OUTPUTS];
    ngx_uint_t                   noutputs;

    /* SRT encryption: the listener default, and per-stream overrides */
    ngx_media_srt_crypto_conf_t      crypto;
    ngx_media_srt_params_t           crypto_params;   /* listener */
    ngx_media_srt_params_t           output_params[NGX_MEDIA_SRT_MAX_OUTPUTS];
    ngx_media_srt_stream_crypto_t    crypto_streams[NGX_MEDIA_SRT_MAX_CRYPTO_STREAMS];
    ngx_uint_t                       ncrypto_streams;
} ngx_media_srt_main_conf_t;

typedef struct {
    uint64_t               id;
    ngx_uint_t             used;
    ngx_media_stream_t    *stream;
    ngx_media_source_t    *source;

    /* set when this worker does not own the stream and routes to the owner */
    unsigned               routed:1;
    uint64_t               hash;
    uint64_t               routed_sequence;
    ngx_media_ts_demux_t   demux;
    ngx_uint_t             demux_ready;
    uint64_t               frames_video;
    uint64_t               frames_audio;
    uint64_t               keyframes;
    uint64_t               config_frames;
} ngx_media_srt_slot_t;

static void *ngx_media_srt_create_conf(ngx_cycle_t *cycle);
static char *ngx_media_srt_init_main_conf(ngx_cycle_t *cycle, void *conf);
static char *ngx_media_srt_listen_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_srt_listen_bond_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_srt_listen_shared_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_srt_crypto_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_media_srt_init_process(ngx_cycle_t *cycle);
static void ngx_media_srt_exit_process(ngx_cycle_t *cycle);
static void ngx_media_srt_shutdown_handler(ngx_event_t *ev);
static void ngx_media_srt_handler(ngx_event_t *ev);
static ngx_int_t ngx_media_srt_parse_endpoint(ngx_pool_t *pool,
    const ngx_str_t *endpoint, ngx_str_t *host, ngx_uint_t *port);

static char *ngx_media_srt_priority_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_srt_output_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_media_srt_backend_cmd(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void ngx_media_srt_output_sink(void *ctx, ngx_media_stream_t *stream,
    ngx_media_buf_t *burst, size_t len, ngx_uint_t keyframe);

/* runtime destinations (normative revision): the core model calls these */
static ngx_int_t ngx_media_srt_destination_add(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination, ngx_log_t *log);
static void ngx_media_srt_destination_remove(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination);
static void ngx_media_srt_output_handler(ngx_event_t *ev);
static ngx_uint_t ngx_media_srt_priority(const ngx_str_t *id);
static void ngx_media_srt_stream_policy(ngx_media_stream_t *stream);
static uint64_t ngx_media_srt_demux_errors(
    const ngx_media_ts_demux_stats_t *stats);
static void ngx_media_srt_sink_frame(void *ctx,
    const ngx_media_frame_t *frame);
static void ngx_media_srt_sink_tracks(void *ctx,
    const ngx_media_trackset_t *tracks);
static void ngx_media_srt_slot_open(ngx_log_t *log, uint64_t session_id,
    ngx_media_srt_streamid_t *id);
static void ngx_media_srt_slot_close(ngx_log_t *log, uint64_t session_id);
static ngx_media_srt_slot_t *ngx_media_srt_slot_find(uint64_t id);
static ngx_media_srt_slot_t *ngx_media_srt_slot_alloc(void);

static ngx_media_srt_ingest_t   ngx_media_srt_ingest;
static ngx_connection_t        *ngx_media_srt_connection;
static ngx_uint_t               ngx_media_srt_started;

/*
 * How often the worker checks whether a graceful shutdown has begun, and the
 * watcher that does it.  See ngx_media_srt_shutdown_handler().
 */
#define NGX_MEDIA_SRT_SHUTDOWN_INTERVAL  100

static ngx_event_t              ngx_media_srt_shutdown_ev;

/*
 * The program runtime is armed in every worker, before the worker-0 checks,
 * and the destination block runs without the listener: teardown is gated on
 * what was actually started, not on the listener, or an instance with SRT
 * destinations and no listener never stops them.
 */
static ngx_uint_t               ngx_media_srt_runtime_started;
static uint64_t                 ngx_media_srt_drained_bytes;
static uint64_t                 ngx_media_srt_drained_chunks;
static ngx_msec_t               ngx_media_srt_last_summary;

static ngx_media_srt_slot_t  ngx_media_srt_slots[
    NGX_MEDIA_SRT_MAX_SESSIONS];

/* SRT destinations fed from the shared program preparation */
static ngx_media_srt_outputs_t  *ngx_media_srt_outputs;
static ngx_connection_t         *ngx_media_srt_output_connection;

static ngx_media_feed_conf_t    ngx_media_srt_feed_conf = {
    2048, 32 * 1024 * 1024, 10000
};

static ngx_command_t ngx_media_srt_commands[] = {

    { ngx_string("media_srt_listen"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_srt_listen_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_listen_bond"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_srt_listen_bond_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_listen_shared"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_srt_listen_shared_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_source_priority"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE2,
      ngx_media_srt_priority_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_crypto"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1|NGX_CONF_TAKE2|NGX_CONF_TAKE3
          |NGX_CONF_TAKE4,
      ngx_media_srt_crypto_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_crypto_stream"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE2|NGX_CONF_TAKE3|NGX_CONF_TAKE4
          |NGX_CONF_TAKE5,
      ngx_media_srt_crypto_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_backend"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_media_srt_backend_cmd,
      0,
      0,
      NULL },

    { ngx_string("media_srt_output"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE2|NGX_CONF_TAKE3,
      ngx_media_srt_output_cmd,
      0,
      0,
      NULL },

      ngx_null_command
};

static ngx_core_module_t ngx_media_srt_module_ctx = {
    ngx_string("media_srt"),
    ngx_media_srt_create_conf,
    ngx_media_srt_init_main_conf
};

ngx_module_t ngx_media_srt_module = {
    NGX_MODULE_V1,
    &ngx_media_srt_module_ctx,     /* module context */
    ngx_media_srt_commands,        /* module directives */
    NGX_CORE_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    ngx_media_srt_init_process,    /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    ngx_media_srt_exit_process,    /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_media_srt_slot_t *
ngx_media_srt_slot_find(uint64_t id)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
        if (ngx_media_srt_slots[i].used
            && ngx_media_srt_slots[i].id == id)
        {
            return &ngx_media_srt_slots[i];
        }
    }

    return NULL;
}

static ngx_media_srt_slot_t *
ngx_media_srt_slot_alloc(void)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
        if (!ngx_media_srt_slots[i].used) {
            ngx_memzero(&ngx_media_srt_slots[i],
                        sizeof(ngx_media_srt_slot_t));
            ngx_media_srt_slots[i].used = 1;

            return &ngx_media_srt_slots[i];
        }
    }

    return NULL;
}

static void
ngx_media_srt_sink_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ngx_media_srt_slot_t  *session = ctx;

    if (session == NULL) {
        return;
    }

    if (session->routed) {
        /* the program lives on another worker: hand the frame over once */
        (void) ngx_media_route_frame((ngx_cycle_t *) ngx_cycle, session->hash,
                                     frame, session->routed_sequence);
    }

    if (session->stream != NULL && session->source != NULL) {
        ngx_media_health_media(&session->source->health, frame->dts,
                               ngx_current_msec);

        /* ISO tap: this source before the selector */
        ngx_media_runtime_iso_source(session->stream, session->source, frame);

        (void) ngx_media_stream_publish(session->stream, session->source,
                                        frame, ngx_current_msec);
    }

    if (session->routed) {
        session->routed_sequence++;
    }

    if (frame->config) {
        session->config_frames++;
        return;
    }

    if (frame->media_type == NGX_MEDIA_TYPE_VIDEO) {
        session->frames_video++;

        if (frame->keyframe) {
            session->keyframes++;
        }

    } else if (frame->media_type == NGX_MEDIA_TYPE_AUDIO) {
        session->frames_audio++;
    }
}

static void
ngx_media_srt_sink_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ngx_media_srt_slot_t  *session = ctx;
    ngx_uint_t                i;

    if (session != NULL && session->routed) {
        (void) ngx_media_route_tracks((ngx_cycle_t *) ngx_cycle, session->hash,
                                      tracks);
    }

    if (session != NULL && session->source != NULL) {
        (void) ngx_media_source_tracks_set(session->source, tracks,
                                           ngx_cycle->log);
    }

    for (i = 0; i < tracks->count; i++) {

        if (tracks->tracks[i].media_type == NGX_MEDIA_TYPE_VIDEO
            && session != NULL && session->source != NULL)
        {
            /* the standby cache opens at a video keyframe only */
            session->source->has_video = 1;
        }

        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                      "media: srt track %ui media=%ui codec=%ui format=%ui "
                      "rate=%ui channels=%ui config=%s",
                      i,
                      tracks->tracks[i].media_type,
                      tracks->tracks[i].codec,
                      tracks->tracks[i].payload_format,
                      tracks->tracks[i].sample_rate,
                      tracks->tracks[i].channels,
                      tracks->tracks[i].config != NULL ? "yes" : "no");
    }
}

static void
ngx_media_srt_demux_start(ngx_media_srt_slot_t *session, ngx_log_t *log)
{
    ngx_media_ts_demux_conf_t  conf;
    ngx_media_ts_sink_t        sink;

    if (session->demux_ready) {
        ngx_media_ts_demux_destroy(&session->demux);
        session->demux_ready = 0;
    }

    conf.max_tracks = 8;
    conf.max_au_bytes = 1024 * 1024;

    sink.tracks = ngx_media_srt_sink_tracks;
    sink.frame = ngx_media_srt_sink_frame;

    if (ngx_media_ts_demux_init(&session->demux, &conf, &sink, session, log)
        != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: could not start the TS demuxer");
        return;
    }

    session->demux_ready = 1;
}

static void
ngx_media_srt_slot_open(ngx_log_t *log, uint64_t session_id,
    ngx_media_srt_streamid_t *id)
{
    ngx_media_registry_t     *registry;
    ngx_media_source_t       *source;
    ngx_media_stream_t       *stream;
    ngx_media_srt_slot_t  *session;

    session = ngx_media_srt_slot_alloc();

    if (session == NULL) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "media: no free ingest session slot for session=%uL",
                      session_id);
        return;
    }

    session->id = session_id;

    /*
     * The program lives on exactly one worker.  When the publisher lands
     * elsewhere, the frames are forwarded over the bounded inter-worker
     * transport instead of being registered here (goal doc 22, 23).
     */
    session->hash = ngx_media_owner_hash(&id->application, &id->stream);

    if (!ngx_media_route_is_owner((ngx_cycle_t *) ngx_cycle, session->hash)) {
        session->routed = 1;

        if (ngx_media_route_open((ngx_cycle_t *) ngx_cycle, session->hash,
                                 &id->application, &id->stream, &id->source,
                                 NGX_MEDIA_SOURCE_SRT,
                                 ngx_media_srt_priority(&id->source))
            != NGX_OK)
        {
            ngx_log_error(NGX_LOG_WARN, log, 0,
                          "media: could not route %V/%V to its owner worker",
                          &id->application, &id->stream);
            session->routed = 0;
            ngx_memzero(session, sizeof(ngx_media_srt_slot_t));
            return;
        }

        ngx_media_srt_demux_start(session, log);

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: srt publisher routed to the owner stream=%V/%V "
                      "source=%V", &id->application, &id->stream, &id->source);

        return;
    }

    registry = ngx_media_registry_get((ngx_cycle_t *) ngx_cycle);

    if (registry == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: no stream registry in this worker");
        return;
    }

    /*
     * The stream outlives the publisher's connection, so the log it keeps has
     * to be the worker's: a connection log dies with the connection, and a
     * reader or a pool logging through it afterwards reads freed memory.
     */
    stream = ngx_media_registry_stream_create(registry, &id->application,
                                              &id->stream,
                                              &ngx_media_srt_feed_conf,
                                              ((ngx_cycle_t *) ngx_cycle)->log);

    if (stream == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: could not create stream %V/%V",
                      &id->application, &id->stream);
        return;
    }

    /* a reconnect of the same identity replaces the previous incarnation */
    source = ngx_media_stream_source_find(stream, &id->source);

    if (source != NULL) {
        ngx_media_stream_source_remove(stream, source);
        source = NULL;
    }

    /* priority comes from trusted configuration, never from the encoder */
    source = ngx_media_stream_source_add(stream, &id->source,
                                         NGX_MEDIA_SOURCE_SRT,
                                         ngx_media_srt_priority(&id->source),
                                         log);

    if (source == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "media: could not register source %V for %V/%V",
                      &id->source, &id->application, &id->stream);
        return;
    }

    ngx_media_srt_stream_policy(stream);

    /* publish the program's record: liveness and progress for the replicas */
    {
        ngx_media_owner_dir_t  *dir = ngx_media_runtime_owner_dir();

        if (dir != NULL) {
            (void) ngx_media_owner_dir_claim(dir, session->hash,
                                             (ngx_uint_t) ngx_worker);
        }
    }

    session->stream = stream;
    session->source = source;

    ngx_media_health_init(&source->health, &stream->selector,
                          ngx_current_msec);
    ngx_media_health_transport(&source->health, 1, ngx_current_msec);

    /*
     * Bootstrap: the first source of a stream is promoted through the normal
     * safe path and becomes active at its first keyframe.
     */
    if (stream->active == NULL) {
        (void) ngx_media_stream_promote(stream, source);
    }

    ngx_media_srt_demux_start(session, log);

    if (stream->active != NULL) {
        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: srt program stream=%V/%V sources=%ui active=%V",
                      &stream->application, &stream->name,
                      ngx_media_stream_source_count(stream),
                      &stream->active->id);

    } else {
        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: srt program stream=%V/%V sources=%ui "
                      "active=none",
                      &stream->application, &stream->name,
                      ngx_media_stream_source_count(stream));
    }
}

static void
ngx_media_srt_slot_close(ngx_log_t *log, uint64_t session_id)
{
    ngx_media_ts_demux_stats_t  stats;
    ngx_media_srt_slot_t    *session;
    ngx_media_stream_t         *stream;

    session = ngx_media_srt_slot_find(session_id);

    if (session == NULL) {
        return;
    }

    if (session->demux_ready) {
        ngx_media_ts_demux_flush(&session->demux);
        ngx_media_ts_demux_stats(&session->demux, &stats);

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: srt demux video=%uL audio=%uL keyframes=%uL "
                      "config=%uL pcr=%uL last_pcr=%L sync_errors=%uL "
                      "continuity_errors=%uL psi_errors=%uL crc_errors=%uL "
                      "pes_errors=%uL au_overflows=%uL unsupported=%uL",
                      session->frames_video,
                      session->frames_audio,
                      session->keyframes,
                      session->config_frames,
                      (uint64_t) stats.has_pcr,
                      (int64_t) stats.last_pcr,
                      stats.sync_errors,
                      stats.continuity_errors,
                      stats.psi_errors,
                      stats.crc_errors,
                      stats.pes_errors,
                      stats.au_overflows,
                      stats.unsupported_streams);

        ngx_media_ts_demux_destroy(&session->demux);
        session->demux_ready = 0;
    }

    if (session->routed) {
        (void) ngx_media_route_close((ngx_cycle_t *) ngx_cycle, session->hash);

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: routed srt publisher closed hash=%uL frames=%uL",
                      session->hash, session->routed_sequence);

        ngx_memzero(session, sizeof(ngx_media_srt_slot_t));
        return;
    }

    stream = session->stream;

    if (session->source != NULL) {
        ngx_media_health_transport(&session->source->health, 0,
                                   ngx_current_msec);
    }

    if (stream != NULL && session->source != NULL) {
        ngx_media_stream_source_remove(stream, session->source);
        session->source = NULL;
    }

    if (stream != NULL) {

        if (stream->active != NULL) {
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "media: srt program stream=%V/%V frames=%uL "
                          "generation=%ui switches=%uL sources=%ui active=%V",
                          &stream->application, &stream->name,
                          stream->program_frames, stream->generation,
                          stream->switches,
                          ngx_media_stream_source_count(stream),
                          &stream->active->id);

        } else {
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "media: srt program stream=%V/%V frames=%uL "
                          "generation=%ui switches=%uL sources=%ui "
                          "active=none",
                          &stream->application, &stream->name,
                          stream->program_frames, stream->generation,
                          stream->switches,
                          ngx_media_stream_source_count(stream));
        }
    }

    ngx_memzero(session, sizeof(ngx_media_srt_slot_t));
}

extern ngx_module_t  ngx_media_core_module;

/*
 * The transport parameters for a stream: the per-stream override when one is
 * configured for it, otherwise the global setting.  Returns NULL when no
 * encryption applies.
 */
static const ngx_media_srt_params_t *
ngx_media_srt_crypto_params(ngx_media_srt_main_conf_t *mcf,
    const ngx_str_t *stream, ngx_media_srt_params_t *store)
{
    ngx_media_srt_crypto_conf_t  *crypto = &mcf->crypto;
    ngx_uint_t                    i;

    if (stream != NULL && stream->len != 0) {
        for (i = 0; i < mcf->ncrypto_streams; i++) {
            if (mcf->crypto_streams[i].name.len == stream->len
                && ngx_strncmp(mcf->crypto_streams[i].name.data,
                               stream->data, stream->len) == 0)
            {
                crypto = &mcf->crypto_streams[i].crypto;
                break;
            }
        }
    }

    if (!crypto->set) {
        return NULL;
    }

    ngx_memzero(store, sizeof(*store));

    store->passphrase = crypto->passphrase.data;
    store->passphrase_len = crypto->passphrase.len;
    store->pbkeylen = crypto->pbkeylen;
    store->cryptomode = crypto->mode;
    store->enforced = crypto->enforced;

    return store;
}

/*
 * media_srt_crypto <passphrase> [ctr|gcm] [0|16|24|32] [on|off];
 * media_srt_crypto_stream <application/stream> <passphrase> [...];
 *
 * The optional arguments are the cipher mode, the AES key length and whether
 * a peer whose secret does not match is rejected.  SRT requires 10..79
 * characters of passphrase; anything else is a configuration error rather
 * than a handshake failure later.
 */
static char *
ngx_media_srt_crypto_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_srt_main_conf_t  *mcf = conf;
    ngx_str_t                  *value = cf->args->elts;
    ngx_media_srt_crypto_conf_t  *crypto;
    ngx_uint_t                  at = 1;

    if (ngx_strcmp(value[0].data, "media_srt_crypto_stream") == 0) {
        ngx_media_srt_stream_crypto_t  *entry;
        ngx_uint_t                      i;

        if (mcf->ncrypto_streams >= NGX_MEDIA_SRT_MAX_CRYPTO_STREAMS) {
            return "too many media_srt_crypto_stream directives";
        }

        entry = NULL;

        for (i = 0; i < mcf->ncrypto_streams; i++) {
            if (mcf->crypto_streams[i].name.len == value[1].len
                && ngx_strncmp(mcf->crypto_streams[i].name.data,
                               value[1].data, value[1].len) == 0)
            {
                entry = &mcf->crypto_streams[i];
                break;
            }
        }

        if (entry == NULL) {
            entry = &mcf->crypto_streams[mcf->ncrypto_streams++];
            entry->name = value[1];
        }

        crypto = &entry->crypto;
        at = 2;

    } else {
        crypto = &mcf->crypto;
    }

    if (crypto->set) {
        return "duplicate SRT crypto directive";
    }

    if (value[at].len < 10 || value[at].len > 79) {
        return "SRT passphrase must be 10..79 characters";
    }

    crypto->passphrase = value[at];
    crypto->mode = NGX_MEDIA_SRT_CRYPTO_CTR;
    crypto->pbkeylen = 0;
    crypto->enforced = 1;
    crypto->set = 1;

    at++;

    if (cf->args->nelts > at) {
        if (ngx_strcmp(value[at].data, "gcm") == 0) {
            crypto->mode = NGX_MEDIA_SRT_CRYPTO_GCM;

        } else if (ngx_strcmp(value[at].data, "ctr") != 0) {
            return "cipher mode must be ctr or gcm";
        }

        at++;
    }

    if (cf->args->nelts > at) {
        ngx_int_t  len = ngx_atoi(value[at].data, value[at].len);

        if (len != 0 && len != 16 && len != 24 && len != 32) {
            return "key length must be 0 (library default), 16, 24 or 32";
        }

        crypto->pbkeylen = (ngx_uint_t) len;
        at++;
    }

    if (cf->args->nelts > at) {
        if (ngx_strcmp(value[at].data, "on") == 0) {
            crypto->enforced = 1;

        } else if (ngx_strcmp(value[at].data, "off") == 0) {
            crypto->enforced = 0;

        } else {
            return "enforcement must be on or off";
        }
    }

    (void) cmd;

    return NGX_CONF_OK;
}

/*
 * media_srt_backend haivision|udp
 *
 * The transport backend is a build-time-pluggable implementation of the same
 * contract (goal doc 11.1).  "srt" is the SRT library backend: Haivision/srt
 * is the reference library, and robotweax/srt is a clean-slate implementation
 * of the same C API that the build can link instead without any code change.
 * "udp" is the test double used to qualify that boundary; it is not an SRT
 * implementation and is not built by default.
 */
static char *
ngx_media_srt_backend_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t  *value = cf->args->elts;

    (void) cmd;
    (void) conf;

    if (value[1].len == sizeof("srt") - 1
        && ngx_strncmp(value[1].data, "srt", sizeof("srt") - 1) == 0)
    {
        if (&ngx_media_srt_haivision_ops == NULL) {
            return "this build has no SRT library backend"
                   " (build with MEDIA_SRT_BACKEND=srt or both)";
        }

        ngx_media_srt_set_backend(&ngx_media_srt_haivision_ops);
        return NGX_CONF_OK;
    }

    if (value[1].len == sizeof("udp") - 1
        && ngx_strncmp(value[1].data, "udp", sizeof("udp") - 1) == 0)
    {
        if (&ngx_media_srt_udp_ops == NULL) {
            return "this build has no UDP test double"
                   " (build with MEDIA_SRT_BACKEND=udp or both)";
        }

        ngx_media_srt_set_backend(&ngx_media_srt_udp_ops);
        return NGX_CONF_OK;
    }

    return "backend must be srt (the SRT library) or udp (test double)";
}

/*
 * media_srt_output <application/stream> <host:port> [streamid]
 *
 * The destination consumes the transport bursts the program runtime already
 * prepares for HLS and recording, so SRT output adds no second preparation
 * and no per-receiver copy (goal doc 16, 34 item 11).
 */
static char *
ngx_media_srt_output_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_srt_main_conf_t    *mcf = conf;
    ngx_media_srt_output_conf_t  *out;
    ngx_str_t                    *value = cf->args->elts;
    u_char                       *colon;
    ngx_str_t                     resource;
    ngx_int_t                     port;

    (void) cmd;

    if (mcf->noutputs >= NGX_MEDIA_SRT_MAX_OUTPUTS) {
        return "too many media_srt_output destinations";
    }

    resource = value[1];

    if (resource.len == 0) {
        return "destination stream must not be empty";
    }

    out = &mcf->outputs[mcf->noutputs];

    ngx_memzero(out, sizeof(ngx_media_srt_output_conf_t));

    colon = ngx_strlchr(resource.data, resource.data + resource.len, '/');

    if (colon == NULL) {
        return "destination stream must be application/stream";
    }

    out->application.data = resource.data;
    out->application.len = (size_t) (colon - resource.data);
    out->stream.data = colon + 1;
    out->stream.len = resource.len - out->application.len - 1;

    if (out->application.len == 0 || out->stream.len == 0) {
        return "destination stream must be application/stream";
    }

    colon = ngx_strlchr(value[2].data, value[2].data + value[2].len, ':');

    if (colon == NULL) {
        return "destination address must be host:port";
    }

    out->host.len = (size_t) (colon - value[2].data);

    port = ngx_atoi(colon + 1, value[2].len - out->host.len - 1);

    if (out->host.len == 0 || port < 1 || port > 65535) {
        return "destination address must be host:port";
    }

    /*
     * The transport layer takes a C string, and the configuration value is a
     * slice of "host:port": keep a terminated copy of the host.
     */
    out->host.data = ngx_pnalloc(cf->pool, out->host.len + 1);

    if (out->host.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(out->host.data, value[2].data, out->host.len);
    out->host.data[out->host.len] = '\0';

    out->port = (ngx_uint_t) port;

    if (cf->args->nelts == 4) {

        if (value[3].len == 0
            || value[3].len > NGX_MEDIA_SRT_STREAMID_MAX)
        {
            return "stream id is too long";
        }

        out->streamid = value[3];
    }

    out->max_units = 256;
    out->max_bytes = 8 * 1024 * 1024;
    /* short timeouts keep shutdown and reconnect latency bounded */
    out->connect_timeout = 2000;
    out->send_timeout = 1000;

    mcf->noutputs++;

    return NGX_CONF_OK;
}

/*
 * Runtime destinations (normative revision).  The core owns the destination
 * model; this is the backend that starts and stops the actual SRT caller.
 * The slot index lives in destination->impl so removal need not search.
 */
static ngx_int_t
ngx_media_srt_destination_add(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination, ngx_log_t *log)
{
    ngx_media_srt_output_conf_t  conf;
    ngx_uint_t                   index = 0;

    if (ngx_media_srt_outputs == NULL || destination->host.data == NULL
        || destination->port == 0)
    {
        return NGX_ERROR;
    }

    ngx_memzero(&conf, sizeof(conf));

    conf.application = stream->application;
    conf.stream = stream->name;
    conf.host = destination->host;
    conf.port = destination->port;
    conf.streamid = destination->streamid;
    conf.max_units = 256;
    conf.max_bytes = 8 * 1024 * 1024;
    conf.connect_timeout = 2000;
    conf.send_timeout = 2000;

    if (ngx_media_srt_outputs_add(ngx_media_srt_outputs, &conf, &index, log)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    destination->impl = (void *) (uintptr_t) (index + 1);

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "media: srt destination %V started for %V/%V -> %V:%ui",
                  &destination->id, &stream->application, &stream->name,
                  &destination->host, destination->port);

    return NGX_OK;
}

static void
ngx_media_srt_destination_remove(ngx_media_stream_t *stream,
    ngx_media_destination_t *destination)
{
    uintptr_t  slot = (uintptr_t) destination->impl;

    (void) stream;

    if (ngx_media_srt_outputs == NULL || slot == 0) {
        return;
    }

    ngx_media_srt_outputs_remove(ngx_media_srt_outputs,
                                 (ngx_uint_t) slot - 1);
}

static void
ngx_media_srt_output_sink(void *ctx, ngx_media_stream_t *stream,
    ngx_media_buf_t *burst, size_t len, ngx_uint_t keyframe)
{
    ngx_media_srt_outputs_t  *outs = ctx;

    if (outs == NULL || stream == NULL) {
        return;
    }

    (void) ngx_media_srt_outputs_push(outs, &stream->application,
                                      &stream->name, burst, len, keyframe);
}

/* destination status changes arrive on their own eventfd */
static void
ngx_media_srt_output_handler(ngx_event_t *ev)
{
    ngx_connection_t           *c = ev->data;
    ngx_media_srt_outputs_t    *outs = c->data;
    ngx_media_srt_out_event_t   events[8];
    ngx_uint_t                  n, i;
    u_char                      evbuf[8];
    ssize_t                     r;

    r = read(outs != NULL ? ngx_media_srt_outputs_notify_fd(outs) : -1, evbuf,
             sizeof(evbuf));
    (void) r;

    if (outs == NULL) {
        return;
    }

    for ( ;; ) {
        n = ngx_media_srt_outputs_event_read(outs, events, 8);

        if (n == 0) {
            break;
        }

        for (i = 0; i < n; i++) {

            if (events[i].type == NGX_MEDIA_SRT_OUT_EVENT_CONNECTED) {
                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt output %ui connected (bursts=%uL "
                              "bytes=%uL dropped=%uL)",
                              events[i].index, events[i].sent_bursts,
                              events[i].sent_bytes, events[i].dropped);

            } else {
                ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                              "media: srt output %ui not connected "
                              "(attempts=%uL dropped=%uL)",
                              events[i].index, events[i].reconnects,
                              events[i].dropped);
            }
        }
    }
}

static void
ngx_media_srt_stream_policy(ngx_media_stream_t *stream)
{
    ngx_media_policy_t  *policy;

    policy = (ngx_media_policy_t *)
                 ((ngx_cycle_t *) ngx_cycle)
                     ->conf_ctx[ngx_media_core_module.index];

    if (policy == NULL) {
        policy = ngx_media_policy_get((ngx_cycle_t *) ngx_cycle);
    }

    if (policy != NULL) {
        ngx_media_stream_set_policy(stream, policy);
    }
}

static uint64_t
ngx_media_srt_demux_errors(const ngx_media_ts_demux_stats_t *stats)
{
    return stats->sync_errors + stats->transport_errors
           + stats->continuity_errors + stats->psi_errors
           + stats->crc_errors + stats->pes_errors;
}

static ngx_uint_t
ngx_media_srt_priority(const ngx_str_t *id)
{
    ngx_media_srt_main_conf_t  *mcf;
    ngx_uint_t                  i;

    mcf = (ngx_media_srt_main_conf_t *)
              ((ngx_cycle_t *) ngx_cycle)
                  ->conf_ctx[ngx_media_srt_module.index];

    if (mcf == NULL) {
        return 0;
    }

    for (i = 0; i < mcf->npriorities; i++) {
        if (mcf->priorities[i].id.len == id->len
            && ngx_memcmp(mcf->priorities[i].id.data, id->data, id->len) == 0)
        {
            return mcf->priorities[i].priority;
        }
    }

    return 0;
}

static char *
ngx_media_srt_priority_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_srt_main_conf_t  *mcf = conf;
    ngx_str_t                  *value;
    ngx_int_t                   priority;

    (void) cmd;

    value = cf->args->elts;

    if (mcf->npriorities >= NGX_MEDIA_SRT_MAX_PRIORITIES) {
        return "too many media_srt_source_priority directives";
    }

    priority = ngx_atoi(value[2].data, value[2].len);
    if (priority == NGX_ERROR || priority < 0 || priority > 65535) {
        return "invalid priority";
    }

    mcf->priorities[mcf->npriorities].id = value[1];
    mcf->priorities[mcf->npriorities].priority = (ngx_uint_t) priority;
    mcf->npriorities++;

    return NGX_CONF_OK;
}

static void *
ngx_media_srt_create_conf(ngx_cycle_t *cycle)
{
    ngx_media_srt_main_conf_t  *mcf;

    mcf = ngx_pcalloc(cycle->pool, sizeof(ngx_media_srt_main_conf_t));
    if (mcf == NULL) {
        return NULL;
    }

    /*
     * One entry per media_srt_listen, so the directive can be given once per
     * worker.  Created here rather than at first use because the directive
     * handler only pushes.
     */
    mcf->listens = ngx_array_create(cycle->pool, 4,
                                    sizeof(ngx_media_srt_listen_t));

    if (mcf->listens == NULL) {
        return NULL;
    }

    return mcf;
}

static char *
ngx_media_srt_listen_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_srt_main_conf_t  *mcf = conf;
    ngx_media_srt_listen_t     *listen;
    ngx_str_t                  *value;

    (void) cmd;

    value = cf->args->elts;

    if (value[1].len == 0) {
        return NGX_CONF_ERROR;
    }

    listen = ngx_array_push(mcf->listens);
    if (listen == NULL) {
        return NGX_CONF_ERROR;
    }

    listen->endpoint = value[1];

    return NGX_CONF_OK;
}

static char *
ngx_media_srt_listen_bond_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_srt_main_conf_t  *mcf = conf;
    ngx_str_t                  *value;

    (void) cmd;

    if (mcf->bond_set) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (value[1].len == 0) {
        return NGX_CONF_ERROR;
    }

    mcf->bond = value[1];
    mcf->bond_set = 1;

    return NGX_CONF_OK;
}

static char *
ngx_media_srt_listen_shared_cmd(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_media_srt_main_conf_t  *mcf = conf;
    ngx_str_t                  *value;

    (void) cmd;

    if (mcf->shared_set) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (value[1].len == 0) {
        return NGX_CONF_ERROR;
    }

    mcf->shared = value[1];
    mcf->shared_set = 1;

    return NGX_CONF_OK;
}

/*
 * The checks that have to happen before a worker binds anything.
 *
 * A bonded listener is two addresses of one port, so media_srt_listen_bond
 * only makes sense beside a specific media_srt_listen: a wildcard listen
 * already owns every address, the same address cannot be bound twice, and the
 * second address has to be an address the transport can parse.  Each endpoint
 * gets the same treatment, because a bonded listener per worker is one bond
 * address published on every worker's port rather than a different rule.
 *
 * Two further rules follow from one endpoint per worker:
 *
 * - an endpoint given twice would have two workers bind one address, which
 *   the second would report only as a startup failure about an address in
 *   use;
 * - more endpoints than workers is refused rather than trimmed.  A worker
 *   binds one endpoint, so the extra entries would never accept anything, and
 *   an operator who wrote them would be wrong about how many publishers their
 *   configuration can take.  Fewer endpoints than workers stays legal: the
 *   remaining workers own programs without accepting publishers, which is the
 *   shape a deployment adding ingest ports one at a time has.
 *
 * Checking here is what turns each of those into a configuration error naming
 * the directive instead of a startup failure about an address in use.
 *
 * media_srt_listen_shared is the other way round: one endpoint for the whole
 * instance, which every worker binds.  It is refused beside either of the
 * others, because neither combination has a meaning worth inventing:
 *
 * - beside media_srt_listen, the entries would no longer line up with the
 *   workers ("worker i binds entry i" says nothing about a worker that must
 *   also bind the shared port), and a worker accepts on one listener;
 * - beside media_srt_listen_bond, a bond cannot work at all.  The legs of a
 *   group caller have different source addresses by construction, which is
 *   the point of bonding, and SO_REUSEPORT hashes on the outer address and
 *   port - so the legs land on different workers.  libsrt joins the legs of
 *   one caller into one group through uglobal()'s registry, which is process
 *   state (CUDT::uglobal() is a function-local static, findPeerGroup_LOCKED()
 *   walks its m_Groups), so those workers do not find each other's mirror
 *   group and each serves one leg of the caller's bond as a session of its
 *   own.  A bond that silently becomes two independent publishers is a
 *   correctness failure, not a degradation, so the combination is refused and
 *   the port-per-worker mode is where group callers belong.
 */
static char *
ngx_media_srt_init_main_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_media_srt_main_conf_t  *mcf = conf;
    ngx_media_srt_listen_t     *listens;
    ngx_uint_t                  i, j, n, workers;
    u_char                     *colon;
    size_t                      host_len;
    const char                 *problem;

    listens = (mcf->listens != NULL) ? mcf->listens->elts : NULL;
    n = (mcf->listens != NULL) ? mcf->listens->nelts : 0;

    if (mcf->shared_set) {
        problem = NULL;

        if (n > 0) {
            problem = "cannot be combined with media_srt_listen: one endpoint "
                      "per worker and one endpoint for the instance are two "
                      "modes, and a worker accepts on one listener";

        } else if (mcf->bond_set) {
            problem = "cannot be combined with media_srt_listen_bond: the "
                      "legs of a bonded caller have different source "
                      "addresses, so they hash to different workers, which "
                      "cannot join them into one bond - use media_srt_listen "
                      "(one endpoint per worker) for group callers";
        }

        if (problem != NULL) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "media: media_srt_listen_shared %V %s", &mcf->shared,
                          problem);

            return NGX_CONF_ERROR;
        }

        /*
         * The one line an operator who enabled the mode is sure to read.
         * Said here, by the master, because it is a property of the mode and
         * not of a worker: printed once, before any worker starts, and again
         * on a reload - which is the operation it is about.
         *
         * What it says is measured: see "One shared SRT port" in
         * docs/operations.md.  A reload starts the new generation before the
         * old one exits, so the set of processes bound to the port changes
         * twice, and SO_REUSEPORT re-hashes the flows in flight each time.
         * The sessions that move are served by a worker with no state for
         * them, so they end - and a session that moves gets no
         * acknowledgement, so its caller hangs until SRT gives up rather than
         * failing at once.
         */
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "media: media_srt_listen_shared %V is bound by every "
                      "worker: publishers spread across them, and a reload "
                      "ends every live publisher's session, because the new "
                      "generation binds the port before the old one exits and "
                      "the kernel re-hashes the flows still running.  A "
                      "bonded (group) caller cannot use it at all.  See \"One "
                      "shared SRT port\" in docs/operations.md before "
                      "relying on it",
                      &mcf->shared);
    }

    if (mcf->bond_set) {
        problem = NULL;

        if (n == 0) {
            problem = "needs media_srt_listen: both leg addresses are "
                      "published on its port";

        } else if (ngx_inet_addr(mcf->bond.data, mcf->bond.len) == INADDR_NONE)
        {
            problem = "needs an IPv4 address";
        }

        if (problem != NULL) {
            /*
             * init_conf can only report an error, so the reason goes through
             * the log: without it the operator sees a failed configuration
             * and no statement of which rule was broken.
             */
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "media: media_srt_listen_bond %V %s", &mcf->bond,
                          problem);

            return NGX_CONF_ERROR;
        }
    }

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (listens[j].endpoint.len == listens[i].endpoint.len
                && ngx_memcmp(listens[j].endpoint.data,
                              listens[i].endpoint.data,
                              listens[i].endpoint.len) == 0)
            {
                ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                              "media: media_srt_listen %V is duplicate: one "
                              "endpoint per worker, and two workers cannot "
                              "bind one address",
                              &listens[i].endpoint);

                return NGX_CONF_ERROR;
            }
        }
    }

    for (i = 0; i < n && mcf->bond_set; i++) {
        /*
         * Both remaining rules are about the address, not the port, so the
         * host is the part before the colon.
         */
        colon = ngx_strlchr(listens[i].endpoint.data,
                            listens[i].endpoint.data + listens[i].endpoint.len,
                            ':');
        host_len = (colon != NULL)
                       ? (size_t) (colon - listens[i].endpoint.data)
                       : listens[i].endpoint.len;

        problem = NULL;

        if (host_len == sizeof("0.0.0.0") - 1
            && ngx_strncmp(listens[i].endpoint.data, "0.0.0.0", host_len) == 0)
        {
            problem = "needs a specific address: a wildcard listen already "
                      "covers every address, the bonded leg included";

        } else if (host_len == mcf->bond.len
                   && ngx_strncmp(listens[i].endpoint.data, mcf->bond.data,
                                  host_len) == 0)
        {
            problem = "must differ from the media_srt_listen_bond address";
        }

        if (problem == NULL) {
            continue;
        }

        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: media_srt_listen %V %s", &listens[i].endpoint,
                      problem);

        return NGX_CONF_ERROR;
    }

    workers = ngx_media_owner_worker_count(cycle);

    /*
     * More endpoints than workers is a warning, not a configuration error,
     * because the worker count is not always a property of the configuration:
     * the container takes it from the environment, so a deployment that
     * scales down would stop starting at all if this refused.  The endpoints
     * past the worker count are unused, and saying so once at startup is
     * enough.
     */
    if (n > workers) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                      "media: media_srt_listen has %ui endpoint(s) and there "
                      "are %ui worker(s): a worker binds one endpoint, so the "
                      "last %ui will not be used",
                      n, workers, n - workers);
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_media_srt_parse_endpoint(ngx_pool_t *pool, const ngx_str_t *endpoint,
    ngx_str_t *host, ngx_uint_t *port)
{
    u_char     *colon, *last;
    ngx_int_t   value;

    last = endpoint->data + endpoint->len;

    colon = ngx_strlchr(endpoint->data, last, ':');

    if (colon == NULL || colon == endpoint->data || colon + 1 == last) {
        return NGX_ERROR;
    }

    value = ngx_atoi(colon + 1, last - colon - 1);
    if (value < 1 || value > 65535) {
        return NGX_ERROR;
    }

    /*
     * The transport hands the host to inet_pton(), which needs a NUL: the
     * endpoint is a slice of the configuration, so the copy must be
     * terminated rather than aliased.
     */
    host->len = colon - endpoint->data;
    host->data = ngx_pnalloc(pool, host->len + 1);
    if (host->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(host->data, endpoint->data, host->len);
    host->data[host->len] = '\0';

    *port = (ngx_uint_t) value;

    return NGX_OK;
}

static ngx_int_t
ngx_media_srt_init_process(ngx_cycle_t *cycle)
{
    ngx_media_srt_main_conf_t   *mcf;
    ngx_media_srt_ingest_conf_t  conf;
    ngx_str_t                   *endpoint;
    ngx_connection_t            *c;
    ngx_str_t                    host;
    ngx_uint_t                   port, shared;

    mcf = (ngx_media_srt_main_conf_t *)
              cycle->conf_ctx[ngx_media_srt_module.index];

    /*
     * Which transport implementation is in use, and which library provides
     * it.  Haivision/srt and robotweax/srt are indistinguishable in code, so
     * this line is how a deployment knows what it linked.
     */
    if (ngx_media_srt_backend() != NULL) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "media: srt transport backend=%s library=%s",
                      ngx_media_srt_backend()->name,
                      ngx_media_srt_backend()->library_version != NULL
                          ? ngx_media_srt_backend()->library_version()
                          : "unknown");
    }

    /*
     * Every worker adopts the shared owner directory and its routing
     * endpoints, and arms the program runtime: a worker that does not accept
     * publishers still owns the programs its hash selects.
     */
    if (ngx_media_runtime_init(cycle, cycle->log) != NGX_OK) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: could not initialise the program runtime");
        return NGX_ERROR;
    }

    (void) ngx_media_runtime_arm(cycle, cycle->log);

    ngx_media_srt_runtime_started = 1;

    if (mcf == NULL) {
        return NGX_OK;
    }

    /*
     * The SRT destination subsystem is started even with nothing declared:
     * destinations can be added through the control API at runtime, and they
     * need the sender pool to exist first.
     */
    {
        ngx_media_srt_outputs_t  *outs;
        ngx_uint_t                i;

        for (i = 0; i < mcf->noutputs; i++) {
            ngx_str_t  stream;

            stream.len = mcf->outputs[i].application.len + 1
                         + mcf->outputs[i].stream.len;
            stream.data = ngx_pnalloc(cycle->pool, stream.len);

            if (stream.data == NULL) {
                return NGX_ERROR;
            }

            ngx_snprintf(stream.data, stream.len, "%V/%V",
                         &mcf->outputs[i].application,
                         &mcf->outputs[i].stream);

            mcf->outputs[i].params = ngx_media_srt_crypto_params(
                                         mcf, &stream,
                                         &mcf->output_params[i]);

            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                          "media: srt destination %V:%ui crypto=%s",
                          &mcf->outputs[i].host, mcf->outputs[i].port,
                          mcf->outputs[i].params == NULL ? "none"
                              : (mcf->outputs[i].params
                                     == &mcf->crypto_params
                                 ? "global" : "stream"));
        }

        {
            static ngx_media_destination_ops_t  srt_destination_ops = {
                NGX_MEDIA_DEST_SRT,
                ngx_media_srt_destination_add,
                ngx_media_srt_destination_remove
            };

            (void) ngx_media_destination_register(&srt_destination_ops);
        }

        if (ngx_media_srt_outputs_start(&outs, mcf->outputs, mcf->noutputs,
                                        NGX_MEDIA_SRT_OUT_MAX_EVENTS,
                                        cycle->log) != NGX_OK)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "media: could not start the SRT outputs");
            return NGX_ERROR;
        }

        ngx_media_srt_outputs = outs;

        c = ngx_get_connection(ngx_media_srt_outputs_notify_fd(outs),
                               cycle->log);

        if (c != NULL) {
            c->data = outs;
            c->read->handler = ngx_media_srt_output_handler;
            c->read->log = cycle->log;

            if (ngx_add_event(c->read, NGX_READ_EVENT, 0) == NGX_OK) {
                ngx_media_srt_output_connection = c;

            } else {
                ngx_free_connection(c);
            }
        }

        ngx_media_runtime_set_sink(ngx_media_srt_output_sink, outs);

        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "media: %ui SRT destination(s) started", mcf->noutputs);
    }

    /*
     * This worker's listener.  media_srt_listen may be given once per worker
     * and worker i binds the i-th entry, so a publisher that connects to
     * entry i is accepted by worker i: the receive path of a publisher is the
     * worker that carries it whenever the publisher was placed there, and a
     * session is routed to the program's owner only when that is a different
     * worker.  With one entry this is exactly the single-listener shape -
     * worker 0 binds it, every other worker returns here.
     *
     * media_srt_listen_shared is the other mode, and every worker takes this
     * path: there is one endpoint for the instance and no entry to index, so
     * no worker returns early.  Which worker accepts a publisher is the
     * kernel's decision there, and routing to the program's owner is what
     * gets the media to where it belongs - see the constraint the mode
     * carries in docs/operations.md before choosing it.
     *
     * The index is ngx_worker, not ngx_process_slot: the slot is the position
     * in the process table, which a reload changes - the respawned worker
     * took a different slot, so with the slot the wrong endpoint was bound
     * after a reload.  A worker with no endpoint of its own owns programs
     * without accepting publishers, which is what fewer entries than workers
     * means: it still owns the programs its hash selects.
     */
    endpoint = NULL;
    shared = 0;

    if (mcf->shared_set) {
        endpoint = &mcf->shared;
        shared = 1;

    } else if (mcf->listens != NULL
               && (ngx_uint_t) ngx_worker < mcf->listens->nelts)
    {
        endpoint = &((ngx_media_srt_listen_t *)
                         mcf->listens->elts)[ngx_worker].endpoint;
    }

    if (endpoint == NULL) {
        return NGX_OK;
    }

    /*
     * The listener is optional.  An instance whose destinations all point
     * outward publishes without ever accepting a publisher, and refusing to
     * start its destinations because it has no listener would be exactly the
     * bug this ordering fixes.
     */
    if (ngx_media_srt_parse_endpoint(cycle->pool, endpoint, &host, &port)
        != NGX_OK)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: invalid %s \"%V\"",
                      shared ? "media_srt_listen_shared" : "media_srt_listen",
                      endpoint);
        return NGX_ERROR;
    }

    ngx_memzero(&conf, sizeof(conf));

    conf.host = host;
    conf.port = port;
    conf.max_chunks = 256;
    conf.max_bytes = 8 * 1024 * 1024;
    conf.max_events = 64;
    conf.max_sessions = NGX_MEDIA_SRT_MAX_SESSIONS;
    conf.shared = shared;
    conf.params = ngx_media_srt_crypto_params(mcf, NULL, &mcf->crypto_params);

    if (mcf->bond_set) {
        /*
         * The transport hands this to inet_pton(), which needs a NUL, and the
         * configured value is only a slice of the configuration.
         */
        conf.bond_host.len = mcf->bond.len;
        conf.bond_host.data = ngx_pnalloc(cycle->pool, mcf->bond.len + 1);

        if (conf.bond_host.data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(conf.bond_host.data, mcf->bond.data, mcf->bond.len);
        conf.bond_host.data[mcf->bond.len] = '\0';
    }

    if (ngx_media_srt_ingest_start(&ngx_media_srt_ingest, &conf, cycle->log)
        != NGX_OK)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "media: could not start the SRT ingest runtime");
        return NGX_ERROR;
    }

    c = ngx_get_connection(ngx_media_srt_ingest.notify_fd, cycle->log);
    if (c == NULL) {
        ngx_media_srt_ingest_stop(&ngx_media_srt_ingest);
        return NGX_ERROR;
    }

    c->data = &ngx_media_srt_ingest;
    c->read->handler = ngx_media_srt_handler;
    c->read->log = cycle->log;

    if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
        ngx_free_connection(c);
        ngx_media_srt_ingest_stop(&ngx_media_srt_ingest);
        return NGX_ERROR;
    }

    ngx_media_srt_connection = c;
    ngx_media_srt_started = 1;

    /*
     * From here on, a graceful shutdown has to give this port up promptly:
     * see ngx_media_srt_shutdown_handler().
     */
    ngx_memzero(&ngx_media_srt_shutdown_ev, sizeof(ngx_event_t));

    ngx_media_srt_shutdown_ev.handler = ngx_media_srt_shutdown_handler;
    ngx_media_srt_shutdown_ev.log = cycle->log;

    ngx_add_timer(&ngx_media_srt_shutdown_ev,
                  NGX_MEDIA_SRT_SHUTDOWN_INTERVAL);

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  shared ? "media: SRT ingest runtime started for %V, shared "
                           "with every worker"
                         : "media: SRT ingest runtime started for %V",
                  endpoint);

    return NGX_OK;
}

/*
 * A worker that is shutting down stops accepting publishers at once.
 *
 * On a reload nginx starts the replacement worker before the worker it
 * replaces has exited, and an SRT listener owns the UDP socket the port is
 * bound to, so the replacement's first bind fails.  Two things have to happen
 * for the port to move over: this worker must stop accepting publishers it is
 * about to drop, which this watcher does as soon as the shutdown starts, and
 * the replacement must keep retrying the bind until this worker - and with it
 * the socket a live publisher is holding - is gone.  The sessions themselves
 * are never touched here: they keep running, and keep that socket alive,
 * until this worker exits.
 *
 * That is the ordering nginx itself uses: a worker closes its listening
 * sockets when it begins to shut down and still drains what it accepted.
 *
 * Nothing calls a module back when that moment arrives - nginx sets ngx_exiting
 * in its own worker loop - so this watcher polls for it.  It re-arms only
 * while the worker is alive, so it never holds the exit up, and it stops
 * polling once it has released the listener.
 */
static void
ngx_media_srt_shutdown_handler(ngx_event_t *ev)
{
    if (!ngx_exiting && !ngx_terminate && !ngx_quit) {
        ngx_add_timer(ev, NGX_MEDIA_SRT_SHUTDOWN_INTERVAL);
        return;
    }

    if (ngx_media_srt_started) {
        ngx_log_error(NGX_LOG_INFO, ev->log, 0,
                      "media: releasing the srt listener for the worker a "
                      "reload starts");

        ngx_media_srt_ingest_stop_accepting(&ngx_media_srt_ingest);
    }
}

static void
ngx_media_srt_handler(ngx_event_t *ev)
{
    ngx_connection_t            *c = ev->data;
    ngx_media_srt_ingest_t      *ingest = c->data;
    ngx_media_srt_event_t        events[16];
    ngx_media_ts_ingest_chunk_t  chunks[16];
    ngx_media_ts_ingest_stats_t  stats;
    ngx_media_ts_demux_stats_t   demux_stats;
    ngx_media_srt_streamid_t     id;
    ngx_media_srt_slot_t     *session;
    ngx_uint_t                   n, i, count;
    ngx_uint_t                   force_summary;
    uint64_t                     bytes;
    u_char                       evbuf[8];
    ssize_t                      r;

    r = read(ingest->notify_fd, evbuf, sizeof(evbuf));
    (void) r;

    /*
     * The byte is taken, so a wake-up is no longer outstanding.  This happens
     * before the drains below, not after: an event pushed while this handler
     * runs must write a byte of its own, because the drain may already have
     * passed the point where it was queued.  See ngx_media_srt_notify().
     */
    ingest->notified = 0;

    force_summary = 0;

    for ( ;; ) {
        n = ngx_media_srt_ingest_event_read(ingest, events, 16);

        if (n == 0) {
            break;
        }

        for (i = 0; i < n; i++) {

            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                           "media: srt event type=%ui session=%uL bytes=%uL",
                           events[i].type, events[i].session_id,
                           events[i].bytes);

            switch (events[i].type) {

            case NGX_MEDIA_SRT_EVENT_READY:
                if (ingest->conf.bond_host.len > 0) {
                    ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                                  "media: srt listener ready on %V:%ui bonded "
                                  "with %V",
                                  &ingest->conf.host, ingest->conf.port,
                                  &ingest->conf.bond_host);
                    break;
                }

                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt listener ready on %V:%ui",
                              &ingest->conf.host, ingest->conf.port);
                break;

            case NGX_MEDIA_SRT_EVENT_FAILED:
                if (ingest->conf.bond_host.len > 0) {
                    ngx_log_error(NGX_LOG_EMERG, ev->log, 0,
                                  "media: srt listener failed on %V:%ui bonded "
                                  "with %V (%s)",
                                  &ingest->conf.host, ingest->conf.port,
                                  &ingest->conf.bond_host,
                                  ngx_media_srt_last_error());
                    break;
                }

                ngx_log_error(NGX_LOG_EMERG, ev->log, 0,
                              "media: srt listener failed on %V:%ui (%s)",
                              &ingest->conf.host, ingest->conf.port,
                              ngx_media_srt_last_error());
                break;

            case NGX_MEDIA_SRT_EVENT_OPEN:

                if (ngx_media_srt_streamid_parse(events[i].streamid,
                                                 events[i].streamid_len, &id)
                    != NGX_OK)
                {
                    ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                                  "media: srt publisher rejected session=%uL: "
                                  "malformed stream id", events[i].session_id);
                    break;
                }

                if (id.mode_kind != NGX_MEDIA_SRT_MODE_PUBLISH) {
                    ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                                  "media: srt publisher rejected session=%uL: "
                                  "mode is not publish", events[i].session_id);
                    break;
                }

                if (id.source.len == 0) {
                    ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                                  "media: srt publisher rejected session=%uL: "
                                  "no source identity in the stream id",
                                  events[i].session_id);
                    break;
                }

                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt source open app=%V stream=%V "
                              "source=%V session=%uL",
                              &id.application, &id.stream, &id.source,
                              events[i].session_id);

                ngx_media_srt_slot_open(ev->log, events[i].session_id, &id);
                break;

            case NGX_MEDIA_SRT_EVENT_CLOSE:
                ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                              "media: srt source close session=%uL bytes=%uL "
                              "chunks=%uL",
                              events[i].session_id, events[i].bytes,
                              events[i].chunks);

                ngx_media_srt_slot_close(ev->log, events[i].session_id);

                force_summary = 1;
                break;

            default:
                break;
            }
        }
    }

    bytes = 0;

    for ( ;; ) {
        count = ngx_media_ts_ingest_read(&ingest->payload, chunks, 16);

        if (count == 0) {
            break;
        }

        for (i = 0; i < count; i++) {
            bytes += chunks[i].len;

            if (ngx_media_runtime_raw_ready()) {
                /* RAW tap: transport bytes before normalization */
                (void) ngx_media_runtime_raw_bytes(chunks[i].data,
                                                   chunks[i].len);
            }

            session = ngx_media_srt_slot_find(chunks[i].session_id);

            if (session != NULL && session->source != NULL
                && (session->source->stream == NULL
                    || ngx_atomic_fetch_add(
                           &session->source->pending_remove, 0)))
            {
                /*
                 * The source was removed through the control API.  Ordered
                 * teardown means its transport goes too: without this the
                 * publisher is still attached and re-creates the source on
                 * its next event, so a delete looks like it did nothing.
                 */
                ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                              "media: srt source %V removed, closing its "
                              "session", &session->source->id);

                (void) ngx_media_srt_ingest_close_session(&ngx_media_srt_ingest,
                                                          session->id);
                continue;
            }

            if (session != NULL && session->demux_ready) {
                (void) ngx_media_ts_demux_feed(&session->demux, chunks[i].data,
                                               chunks[i].len);
            }
        }

        ngx_media_ts_ingest_release(chunks, count);

        ngx_media_srt_drained_chunks += count;
    }

    if (bytes > 0) {
        ngx_media_srt_drained_bytes += bytes;

        for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
            session = &ngx_media_srt_slots[i];

            if (!session->used || !session->demux_ready
                || session->source == NULL)
            {
                continue;
            }

            ngx_media_ts_demux_stats(&session->demux, &demux_stats);

            ngx_media_health_container(&session->source->health,
                                       ngx_media_srt_demux_errors(&demux_stats),
                                       ngx_current_msec);
        }
    }

    if (bytes > 0 || force_summary) {

        if (force_summary
            || ngx_current_msec - ngx_media_srt_last_summary >= 1000)
        {
            ngx_media_ts_ingest_stats(&ingest->payload, &stats);

            ngx_log_error(NGX_LOG_INFO, ev->log, 0,
                          "media: srt ingest drained bytes=%uL chunks=%uL "
                          "dropped_chunks=%uL sessions=%uL",
                          ngx_media_srt_drained_bytes,
                          ngx_media_srt_drained_chunks,
                          stats.chunks_dropped,
                          (uint64_t) ingest->sessions_accepted);

            ngx_media_srt_last_summary = ngx_current_msec;
        }
    }
}

static void
ngx_media_srt_exit_process(ngx_cycle_t *cycle)
{
    ngx_uint_t  i;
    ngx_uint_t  ingest;

    /*
     * Ordered teardown, gated on what this process actually started.  The
     * runtime is armed in every worker, and the listener is optional: an
     * instance whose destinations all point outward starts the sender pool,
     * the notify fd and the runtime sink without ever creating one.  Gating
     * all of this on the listener flag left those sender threads running
     * while the worker destroyed the pool they use.
     */
    if (!ngx_media_srt_runtime_started) {
        return;
    }

    ngx_media_srt_runtime_started = 0;

    if (ngx_media_srt_shutdown_ev.timer_set) {
        ngx_del_timer(&ngx_media_srt_shutdown_ev);
    }

    /* the ingest side, up only when a listener was configured */
    ingest = ngx_media_srt_started;
    ngx_media_srt_started = 0;

    if (ingest) {
        for (i = 0; i < NGX_MEDIA_SRT_MAX_SESSIONS; i++) {
            if (ngx_media_srt_slots[i].demux_ready) {
                ngx_media_ts_demux_destroy(&ngx_media_srt_slots[i].demux);
                ngx_media_srt_slots[i].demux_ready = 0;
            }
        }
    }

    if (ngx_media_srt_output_connection != NULL) {
        (void) ngx_del_event(ngx_media_srt_output_connection->read,
                             NGX_READ_EVENT, 0);
        ngx_media_srt_output_connection->fd = (ngx_socket_t) -1;
        ngx_free_connection(ngx_media_srt_output_connection);
        ngx_media_srt_output_connection = NULL;
    }

    if (ngx_media_srt_outputs != NULL) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "media: stopping SRT destinations");
        ngx_media_runtime_set_sink(NULL, NULL);
        ngx_media_srt_outputs_stop(ngx_media_srt_outputs);
        ngx_media_srt_outputs = NULL;
    }

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "media: stopping program runtime");
    ngx_media_runtime_shutdown(cycle->log);
    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "media: program runtime stopped");

    if (ngx_media_srt_connection != NULL) {
        (void) ngx_del_event(ngx_media_srt_connection->read, NGX_READ_EVENT, 0);
        ngx_media_srt_connection->fd = (ngx_socket_t) -1;
        ngx_free_connection(ngx_media_srt_connection);
        ngx_media_srt_connection = NULL;
    }

    if (ingest) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "media: stopping SRT ingest");
        ngx_media_srt_ingest_stop(&ngx_media_srt_ingest);
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "media: SRT ingest stopped");
    }

    ngx_media_srt_shutdown();
}

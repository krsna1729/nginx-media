/*
 * Security / safety / hardening regressions.
 *
 * Covers three hardening rounds:
 *
 *   previous round (566358e, srt/egress):
 *     - SRT outputs_remove() deferred close while dest->sending (UAF)
 *     - idle sender threads sleep on idle_cond instead of spinning
 *
 *   last committed round (5f971cd, core/rtmp/mpegts/http/owner):
 *     1. RTMP chunk reader: fmt0/1 length change frees stale cs->payload
 *     2. AMF0 skip path: member name length bounds check
 *     3. MPEG-TS PSI: 3-byte section header split across packets
 *     4. HTTP TLS: SSL_set1_host() hostname verification
 *     5. HTTP connect: getaddrinfo() DNS + IPv4/IPv6 (was inet_pton IPv4 only)
 *     6. HTTP header: snprintf() truncation guard before socket write
 *     7. HTTP put_file: no ngx_cycle->pool use from background threads
 *     8. Owner dir: DELETED tombstone preserves probe chains
 *
 *   current working-tree round (uncommitted safety fixes):
 *     9.  HLS ingest API: segment names must be *.ts, no dotfiles, strict
 *         charset (path traversal / temp-file pickup guard)
 *     10. File/HLS-ingest/HLS-pull open: close the half-opened source on
 *         reader-start failure (fd/handle leak)
 *     11. HLS ingest seen-set: bounded ring (seen_next), not drop-after-full
 *     12. Route master init: reload with a new worker count closes stale
 *         socket pairs (fd leak)
 *     13. HLS segmenter: every new segment starts with PAT/PMT (npieces==0)
 *         and bytes are counted once (no double-count)
 *
 * Runtime-testable parsers (RTMP, AMF, TS, HLS segmenter) are exercised
 * through the real production code and fail on the pre-fix sources (ASan
 * aborts or wrong results).  Properties that need sockets/TLS/threads or
 * nginx request types are verified by a functional demonstration plus a
 * source-property check that fails on the pre-fix tree and passes on the
 * fixed tree.
 */

#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include "ngx_media_test.h"

#include "ngx_media_rtmp_wire.h"
#include "ngx_media_ts_demux.h"
#include "ngx_media_owner.h"
#include "ngx_media_hls_segmenter.h"
#include "ngx_media_nal.h"
#include "ngx_media_record.h"
#include "ngx_media_ipc.h"
#include "ngx_media_srt_transport.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Never write a core file: a regression test that proves a pre-fix crash
 * must report FAIL, not dump core.  Called first in main(); children
 * inherit the limit, so an expected crash in a forked probe also leaves
 * no core behind. */
static void
disable_coredumps(void)
{
    struct rlimit  rl;

    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    (void) setrlimit(RLIMIT_CORE, &rl);
}

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if (!(cond)) {                                                        \
            ngx_media_test_failures++;                                        \
            printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,               \
                   ##__VA_ARGS__);                                            \
        }                                                                     \
    } while (0)

/* Reads a whole file; returns malloc'd buffer (caller frees) or NULL. */
static char *
read_file(const char *path, size_t *len_out)
{
    FILE  *f;
    char  *buf;
    size_t cap = 65536, n;
    size_t off = 0;

    f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }

    buf = malloc(cap + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    while ((n = fread(buf + off, 1, cap - off, f)) > 0) {
        off += n;
        if (off == cap) {
            cap *= 2;
            buf = realloc(buf, cap + 1);
            if (buf == NULL) {
                fclose(f);
                return NULL;
            }
        }
    }

    fclose(f);
    buf[off] = '\0';
    if (len_out != NULL) {
        *len_out = off;
    }
    return buf;
}

/*
 * Looks for `needle` in one of the candidate paths (unit binary runs from
 * tests/unit/build/, CI may run from elsewhere).  Returns 1 when found.
 */
static int
file_contains(const char *paths[], ngx_uint_t npaths, const char *needle)
{
    ngx_uint_t  i;

    for (i = 0; i < npaths; i++) {
        char   *text = read_file(paths[i], NULL);
        int     found = 0;

        if (text == NULL) {
            continue;
        }

        found = (strstr(text, needle) != NULL);
        free(text);

        if (found) {
            return 1;
        }
    }

    return 0;
}

/* Counts occurrences of `needle` in the first readable candidate path. */
static int
file_count(const char *paths[], ngx_uint_t npaths, const char *needle)
{
    ngx_uint_t  i;

    for (i = 0; i < npaths; i++) {
        char  *text = read_file(paths[i], NULL);
        int    count = 0;
        char  *p;

        if (text == NULL) {
            continue;
        }

        for (p = text; (p = strstr(p, needle)) != NULL; p += 1) {
            count++;
        }

        free(text);
        return count;
    }

    return 0;
}

static const char *http_paths[] = {
    "../../src/core/ngx_media_http.c",
    "tests/unit/../../src/core/ngx_media_http.c",
    "src/core/ngx_media_http.c",
};

static const char *owner_dir_paths[] = {
    "../../src/core/ngx_media_owner_dir.c",
    "src/core/ngx_media_owner_dir.c",
};

static const char *owner_h_paths[] = {
    "../../src/core/ngx_media_owner.h",
    "src/core/ngx_media_owner.h",
};

static const char *srt_out_paths[] = {
    "../../src/srt/ngx_media_srt_output.c",
    "src/srt/ngx_media_srt_output.c",
};

static const char *rtmp_paths[] = {
    "../../src/rtmp/ngx_media_rtmp_wire.c",
    "src/rtmp/ngx_media_rtmp_wire.c",
};

static const char *ts_paths[] = {
    "../../src/mpegts/ngx_media_ts_demux.c",
    "src/mpegts/ngx_media_ts_demux.c",
};

static const char *api_paths[] = {
    "../../src/api/ngx_media_api_module.c",
    "src/api/ngx_media_api_module.c",
};

static const char *file_paths[] = {
    "../../src/core/ngx_media_file.c",
    "src/core/ngx_media_file.c",
};

static const char *hls_ingest_paths[] = {
    "../../src/core/ngx_media_hls_ingest.c",
    "src/core/ngx_media_hls_ingest.c",
};

static const char *hls_pull_paths[] = {
    "../../src/core/ngx_media_hls_pull.c",
    "src/core/ngx_media_hls_pull.c",
};

static const char *route_paths[] = {
    "../../src/core/ngx_media_route.c",
    "src/core/ngx_media_route.c",
};

static const char *hls_seg_paths[] = {
    "../../src/hls/ngx_media_hls_segmenter.c",
    "src/hls/ngx_media_hls_segmenter.c",
};

static const char *rtmp_mod_paths[] = {
    "../../src/rtmp/ngx_media_rtmp_module.c",
    "src/rtmp/ngx_media_rtmp_module.c",
};

static const char *srt_tr_paths[] = {
    "../../src/srt/ngx_media_srt_transport.c",
    "src/srt/ngx_media_srt_transport.c",
};

static const char *hls_push_paths[] = {
    "../../src/core/ngx_media_hls_push.c",
    "src/core/ngx_media_hls_push.c",
};

static const char *nal_paths[] = {
    "../../src/codec/ngx_media_nal.c",
    "src/codec/ngx_media_nal.c",
};

static const char *record_paths[] = {
    "../../src/record/ngx_media_record.c",
    "src/record/ngx_media_record.c",
};

/* ------------------------------------------------------------------ */
/* 1. RTMP: fmt0 length change must drop the stale payload.            */
/*                                                                     */
/* Pre-fix: cs->payload from the previous (smaller) message is reused, */
/* so a later larger message writes past the old buffer (heap overflow */
/* under ASan, heap corruption otherwise).  This test never feeds the  */
/* overflowing payload: after feeding only the second header it checks */
/* that the reader dropped the stale buffer (new capacity, new pointer)*/
/* so it fails on pre-fix code without crashing or dumping core.       */
/* ------------------------------------------------------------------ */

typedef struct {
    ngx_uint_t  count;
    size_t      len[4];
    u_char      data[4][256];
} rtmp_sink_t;

static ngx_int_t
rtmp_collect(void *ctx, ngx_uint_t type, ngx_uint_t stream_id,
    uint32_t timestamp, ngx_media_buf_t *payload)
{
    rtmp_sink_t  *s = ctx;
    size_t        n;

    (void) type;
    (void) stream_id;
    (void) timestamp;

    if (s->count < 4) {
        n = ngx_media_buf_size(payload);
        s->len[s->count] = n;
        if (n <= sizeof(s->data[0])) {
            memcpy(s->data[s->count], ngx_media_buf_data(payload), n);
        }
        s->count++;
    }

    return NGX_OK;
}

static void
build_fmt0(u_char *out, ngx_uint_t csid, uint32_t ts, size_t len,
    ngx_uint_t type, ngx_uint_t stream_id)
{
    out[0] = (u_char) (csid & 0x3F);
    out[1] = (u_char) ((ts >> 16) & 0xFF);
    out[2] = (u_char) ((ts >> 8) & 0xFF);
    out[3] = (u_char) (ts & 0xFF);
    out[4] = (u_char) ((len >> 16) & 0xFF);
    out[5] = (u_char) ((len >> 8) & 0xFF);
    out[6] = (u_char) (len & 0xFF);
    out[7] = (u_char) type;
    out[8] = (u_char) ((stream_id >> 24) & 0xFF);
    out[9] = (u_char) ((stream_id >> 16) & 0xFF);
    out[10] = (u_char) ((stream_id >> 8) & 0xFF);
    out[11] = (u_char) (stream_id & 0xFF);
}

static void
test_rtmp_length_change(void)
{
    ngx_media_rtmp_reader_t  r;
    rtmp_sink_t              sink;
    u_char                   wire1[12 + 128];
    u_char                   hdr2[12];
    size_t                   consumed;
    ngx_uint_t               i;
    ngx_media_rtmp_cs_t     *cs;
    ngx_media_buf_t         *old_payload;
    size_t                   old_cap;

    TEST_CASE("rtmp: fmt0 length change drops the stale payload "
              "(no crash)");

    memset(&sink, 0, sizeof(sink));
    ngx_media_rtmp_reader_init(&r);
    /* default chunking: one full 128-byte chunk leaves a 200-byte message
     * incomplete with pending_remaining == 0, so the next header is parsed
     * as a header -- the path the fix guards. */
    r.chunk_size = 128;
    r.max_message = NGX_MEDIA_RTMP_MAX_MESSAGE;

    /* first message: 200 bytes declared, one full 128-byte chunk delivered */
    build_fmt0(wire1, 4, 0, 200, NGX_MEDIA_RTMP_MSG_AUDIO, 1);
    memset(wire1 + 12, 0xAA, 128);

    CHECK(ngx_media_rtmp_reader_feed(&r, wire1, sizeof(wire1), &consumed,
                                     rtmp_collect, &sink) == NGX_OK,
          "partial first message accepted");
    CHECK(sink.count == 0, "no complete message yet: %lu", sink.count);
    CHECK(consumed == sizeof(wire1), "all partial bytes consumed");

    /* locate the chunk stream and remember its payload buffer */
    cs = NULL;
    for (i = 0; i < NGX_MEDIA_RTMP_MAX_CSID; i++) {
        if (r.chunks[i].used && r.chunks[i].csid == 4) {
            cs = &r.chunks[i];
            break;
        }
    }
    CHECK(cs != NULL, "chunk stream 4 exists");
    CHECK(cs != NULL && cs->payload != NULL, "stale payload present");
    if (cs == NULL || cs->payload == NULL) {
        ngx_media_rtmp_reader_reset(&r);
        return;
    }
    old_payload = cs->payload;
    old_cap = ngx_media_buf_capacity(old_payload);
    CHECK(old_cap == 200, "stale capacity is 200: %lu", old_cap);

    /*
     * Feed only the second header (256 bytes declared, no payload bytes).
     * Fixed code unrefs the 200-byte buffer and allocates 256; pre-fix code
     * keeps the 200-byte buffer.  No payload is copied, so neither version
     * can overflow, abort, or dump core here.
     */
    build_fmt0(hdr2, 4, 0, 256, NGX_MEDIA_RTMP_MSG_AUDIO, 1);

    CHECK(ngx_media_rtmp_reader_feed(&r, hdr2, sizeof(hdr2), &consumed,
                                     rtmp_collect, &sink) == NGX_OK,
          "second header accepted");
    CHECK(consumed == sizeof(hdr2), "header consumed");
    CHECK(sink.count == 0, "still no complete message: %lu", sink.count);

    cs = NULL;
    for (i = 0; i < NGX_MEDIA_RTMP_MAX_CSID; i++) {
        if (r.chunks[i].used && r.chunks[i].csid == 4) {
            cs = &r.chunks[i];
            break;
        }
    }
    CHECK(cs != NULL && cs->payload != NULL, "new payload allocated");
    if (cs != NULL && cs->payload != NULL) {
        CHECK(cs->payload != old_payload,
              "stale buffer was dropped (pointer changed)");
        CHECK(ngx_media_buf_capacity(cs->payload) == 256,
              "new capacity is 256: %lu",
              ngx_media_buf_capacity(cs->payload));
        CHECK(cs->length == 256, "new length recorded: %lu", cs->length);
    }

    /*
     * Health check on a fresh reader: single-chunk delivery with a large
     * chunk size is safe on every version and proves the reader still
     * works after the length change above.  The pre-fix overflow is never
     * fed, so this test cannot crash or dump core on any version; the
     * capacity assertions above are what fail pre-fix.
     */
    {
        u_char  health[12 + 256];

        ngx_media_rtmp_reader_reset(&r);
        memset(&sink, 0, sizeof(sink));
        ngx_media_rtmp_reader_init(&r);
        r.chunk_size = 256;
        r.max_message = NGX_MEDIA_RTMP_MAX_MESSAGE;
        build_fmt0(health, 5, 0, 256, NGX_MEDIA_RTMP_MSG_AUDIO, 1);
        for (i = 0; i < 256; i++) {
            health[12 + i] = (u_char) (0xBB + (i & 0x0F));
        }
        CHECK(ngx_media_rtmp_reader_feed(&r, health, sizeof(health),
                                         &consumed, rtmp_collect, &sink)
              == NGX_OK,
              "fresh message accepted after length change");
        CHECK(sink.count == 1, "one message delivered: %lu", sink.count);
        if (sink.count == 1) {
            CHECK(sink.len[0] == 256,
                  "delivered length is the new length: %lu", sink.len[0]);
        }
    }

    /* source-property companion: the fix unrefs cs->payload on fmt0/1 */
    CHECK(file_contains(rtmp_paths,
                        sizeof(rtmp_paths) / sizeof(rtmp_paths[0]),
                        "ngx_media_buf_unref(cs->payload)"),
          "rtmp wire frees stale cs->payload on length change");

    ngx_media_rtmp_reader_reset(&r);
}

/* ------------------------------------------------------------------ */
/* 2. AMF0: skip-path member name length must be bounds checked.       */
/*                                                                     */
/* Pre-fix: `pos += 2 + nlen` without verifying `pos + 2 + nlen <= len`*/
/* makes `len - pos` wrap to a huge size_t and the recursive read runs */
/* out of bounds (crash/OOB).  Fixed code returns NGX_AGAIN.  The evil  */
/* input below runs in a forked child with core dumps disabled, so a   */
/* pre-fix crash becomes a FAIL, never a core file; the source check   */
/* pins the fix even when the child crashes.                           */
/* ------------------------------------------------------------------ */

/* exit codes for the forked evil-input probe */
#define SEC_AMF_PROBE_OK     0   /* returned NGX_AGAIN, consumed in bounds */
#define SEC_AMF_PROBE_WRONG  1   /* returned something else (pre-fix logic) */

static void
test_amf_skip_oob(void)
{
    ngx_media_amf_value_t  value;
    size_t                 consumed = 0;
    ngx_int_t              rc;

    /*
     * Top-level object { "a": <nested object> }, where the nested object
     * claims a 255-byte member name but only 5 bytes remain.
     *
     *   03 | 00 01 'a' | 03 | 00 FF 41 41 41 41 41
     */
    u_char  evil[] = {
        0x03,
        0x00, 0x01, 'a',
        0x03,
        0x00, 0xFF, 'A', 'A', 'A', 'A', 'A',
    };

    TEST_CASE("amf0: nested name length past the buffer asks for more "
              "(OOB read)");

    /*
     * Forked probe: the child feeds the evil input and exits
     * SEC_AMF_PROBE_OK only when the fixed behaviour (NGX_AGAIN, consumed
     * in bounds) is observed.  Any crash (signal death) or wrong result
     * makes the parent report FAIL.  Core dumps are already disabled
     * process-wide, and the child inherits that, so no core file appears
     * on any path.
     */
    {
        pid_t  pid;
        int    status = 0;

        fflush(stdout);
        pid = fork();

        if (pid == -1) {
            CHECK(0, "fork for amf probe failed");
        } else if (pid == 0) {
            /* child: no test counters here, just probe and exit */
            ngx_media_amf_value_t  cval;
            size_t                 cused = 0xDEAD;
            ngx_int_t              crc;

            crc = ngx_media_amf_read(evil, sizeof(evil), &cval, &cused);

            if (crc == NGX_AGAIN && cused <= sizeof(evil)) {
                _exit(SEC_AMF_PROBE_OK);
            }
            _exit(SEC_AMF_PROBE_WRONG);
        } else {
            while (waitpid(pid, &status, 0) < 0) {
                /* retry on EINTR */
            }

            if (WIFEXITED(status) && WEXITSTATUS(status) == SEC_AMF_PROBE_OK) {
                CHECK(1, "forked probe: truncated nested name asks for more");
            } else if (WIFSIGNALED(status)) {
                CHECK(0, "truncated nested name crashed child (signal %d, "
                      "no core): pre-fix OOB", WTERMSIG(status));
            } else {
                CHECK(0, "truncated nested name returns AGAIN, got child "
                      "exit %d", WIFEXITED(status) ? WEXITSTATUS(status)
                                                   : -1);
            }
        }
    }

    /* source-property pin: the skip path must bounds-check the name */
    CHECK(file_contains(rtmp_paths,
                        sizeof(rtmp_paths) / sizeof(rtmp_paths[0]),
                        "if (pos + 2 + nlen > len)"),
          "amf skip path bounds-checks the member name");

    consumed = 0xDEAD;
    rc = 0;

    /* a well-formed nested object still decodes */
    {
        /* { "a": { "b": 1.0 } } with correct 9-byte AMF0 number */
        u_char  good[] = {
            0x03,
            0x00, 0x01, 'a',
            0x03,
            0x00, 0x01, 'b',
            0x00, 0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x09,
            0x00, 0x00, 0x09,
        };

        rc = ngx_media_amf_read(good, sizeof(good), &value, &consumed);
        CHECK(rc == NGX_OK, "well-formed nested object still decodes: %ld",
              (long) rc);
    }
}

/* ------------------------------------------------------------------ */
/* 8. MPEG-TS: PSI section header split across packet boundaries.      */
/*                                                                     */
/* Pre-fix: when only 1-2 bytes of the 3-byte section header arrived at*/
/* the end of a packet, psi->expected stayed 0, so `expected - len`    */
/* underflowed and the section never completed.  Fixed code accumulates*/
/* the 3-byte header explicitly.                                       */
/* ------------------------------------------------------------------ */

static uint32_t
ts_crc32(const u_char *p, size_t len)
{
    uint32_t  crc = 0xFFFFFFFFu;
    size_t    i;
    int       b;

    for (i = 0; i < len; i++) {
        crc ^= (uint32_t) p[i] << 24;
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u
                                      : (crc << 1);
        }
    }
    return crc;
}

typedef struct {
    ngx_uint_t  track_notes;
    ngx_uint_t  frames;
} ts_sink_ctx_t;

static void
ts_sink_tracks(void *ctx, const ngx_media_trackset_t *tracks)
{
    ts_sink_ctx_t  *s = ctx;

    (void) tracks;
    s->track_notes++;
}

static void
ts_sink_frame(void *ctx, const ngx_media_frame_t *frame)
{
    ts_sink_ctx_t  *s = ctx;

    (void) frame;
    s->frames++;
}

static void
ts_write_packet(u_char *out, ngx_uint_t pid, ngx_uint_t pusi, ngx_uint_t cc,
    const u_char *payload, size_t len)
{
    u_char  *p = out;
    size_t   af = 0;

    memset(p, 0xFF, 188);
    p[0] = 0x47;
    p[1] = (u_char) ((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    p[2] = (u_char) (pid & 0xFF);

    if (len < 184) {
        size_t  stuff = 184 - len - 1;

        af = 1 + stuff;
        p[3] = (u_char) ((0x03 << 4) | (cc & 0x0F));
        p[4] = (u_char) stuff;
        if (stuff > 0) {
            p[5] = 0x00;
        }
    } else {
        p[3] = (u_char) ((0x01 << 4) | (cc & 0x0F));
    }

    if (len > 184 - af) {
        len = 184 - af;
    }
    if (len > 0) {
        memcpy(p + 4 + af, payload, len);
    }
}

static void
test_ts_psi_split(void)
{
    ngx_media_ts_demux_t       demux;
    ngx_media_ts_demux_conf_t  conf;
    ngx_media_ts_sink_t        sink;
    ts_sink_ctx_t              ctx;
    u_char                     section[16];
    u_char                     pkt1[188], pkt2[188];
    u_char                     pay1[2], pay2[15];
    uint32_t                   crc;

    TEST_CASE("mpegts: PSI section header split 1+15 across packets");

    /* minimal PAT: table 0x00, section_length 13, program 1 -> PID 0x1000 */
    section[0] = 0x00;
    section[1] = 0xB0;
    section[2] = 0x0D;
    section[3] = 0x00;
    section[4] = 0x01;
    section[5] = 0xC1;
    section[6] = 0x00;
    section[7] = 0x00;
    section[8] = 0x00;
    section[9] = 0x01;
    section[10] = 0xF0;   /* 0xE0 | ((0x1000 >> 8) & 0x1F) */
    section[11] = 0x00;
    crc = ts_crc32(section, 12);
    section[12] = (u_char) (crc >> 24);
    section[13] = (u_char) (crc >> 16);
    section[14] = (u_char) (crc >> 8);
    section[15] = (u_char) crc;

    memset(&ctx, 0, sizeof(ctx));
    conf.max_tracks = 8;
    conf.max_au_bytes = 0;
    sink.tracks = ts_sink_tracks;
    sink.frame = ts_sink_frame;

    CHECK(ngx_media_ts_demux_init(&demux, &conf, &sink, &ctx, NULL) == NGX_OK,
          "demux init");

    /*
     * Packet 1 (PUSI): pointer 0x00 + first 1 byte of the section.
     * Packet 2 (!PUSI): remaining 15 bytes.  The 3-byte section header is
     * therefore split 1/2 across the packet boundary.
     */
    pay1[0] = 0x00;
    pay1[1] = section[0];
    ts_write_packet(pkt1, 0x0000, 1, 0, pay1, sizeof(pay1));

    memcpy(pay2, section + 1, 15);
    ts_write_packet(pkt2, 0x0000, 0, 1, pay2, sizeof(pay2));

    CHECK(ngx_media_ts_demux_feed(&demux, pkt1, sizeof(pkt1)) == NGX_OK,
          "first fragment accepted");
    CHECK(ngx_media_ts_demux_feed(&demux, pkt2, sizeof(pkt2)) == NGX_OK,
          "second fragment accepted");

    CHECK(demux.pmt_pid == 0x1000, "split PAT reassembled, pmt_pid=%lu",
          (unsigned long) demux.pmt_pid);
    {
        ngx_media_ts_demux_stats_t  stats;

        ngx_media_ts_demux_stats(&demux, &stats);
        CHECK(stats.psi_errors == 0, "no psi error on split header: %llu",
              (unsigned long long) stats.psi_errors);
    }

    /* 2-byte split as well: 2 bytes then 14 bytes */
    {
        ngx_media_ts_demux_t  d2;
        ts_sink_ctx_t         c2;
        u_char                q1[188], q2[188];
        u_char                r1[3], r2[14];

        memset(&c2, 0, sizeof(c2));
        CHECK(ngx_media_ts_demux_init(&d2, &conf, &sink, &c2, NULL) == NGX_OK,
              "demux re-init for 2-byte split");

        r1[0] = 0x00;
        r1[1] = section[0];
        r1[2] = section[1];
        ts_write_packet(q1, 0x0000, 1, 0, r1, sizeof(r1));

        memcpy(r2, section + 2, 14);
        ts_write_packet(q2, 0x0000, 0, 1, r2, sizeof(r2));

        CHECK(ngx_media_ts_demux_feed(&d2, q1, sizeof(q1)) == NGX_OK,
              "2-byte first fragment accepted");
        CHECK(ngx_media_ts_demux_feed(&d2, q2, sizeof(q2)) == NGX_OK,
              "2-byte second fragment accepted");
        CHECK(d2.pmt_pid == 0x1000, "2-byte split reassembled: %lu",
              (unsigned long) d2.pmt_pid);

        ngx_media_ts_demux_destroy(&d2);
    }

    ngx_media_ts_demux_destroy(&demux);
}

/* ------------------------------------------------------------------ */
/* 9. Owner directory: tombstone preserves colliding probe chains.     */
/*                                                                     */
/* Pre-fix: release() reset the slot to FREE, severing the linear-     */
/* probing chain so a colliding stream hashed behind it became         */
/* unreachable.  Fixed code leaves a DELETED tombstone that find()     */
/* skips and claim() reclaims.                                         */
/*                                                                     */
/* The production table lives in shared memory behind nginx types, so  */
/* this test demonstrates the exact probing invariant on a faithful    */
/* replica and pins the production fix by source property (DELETED     */
/* state, first_avail, tombstone reclaim).  On the pre-fix tree the    */
/* source checks fail.                                                 */
/* ------------------------------------------------------------------ */

#define SEC_FREE     0
#define SEC_OWNED    1
#define SEC_DELETED  2

typedef struct {
    uint32_t  hash;
    uint32_t  state;
} sec_slot_t;

static ngx_uint_t
sec_index(uint32_t hash, ngx_uint_t slots)
{
    return (ngx_uint_t) (hash % slots);
}

/* fixed find(): skips tombstones, remembers first reclaimable slot */
static sec_slot_t *
sec_find_fixed(sec_slot_t *tab, ngx_uint_t slots, uint32_t hash)
{
    ngx_uint_t  i, index;
    sec_slot_t *first_avail = NULL;

    for (i = 0; i < slots; i++) {
        index = (sec_index(hash, slots) + i) % slots;

        if (tab[index].state == SEC_OWNED) {
            if (tab[index].hash == hash) {
                return &tab[index];
            }
        } else if (tab[index].state == SEC_DELETED) {
            if (first_avail == NULL) {
                first_avail = &tab[index];
            }
        } else {
            return (first_avail != NULL) ? first_avail : &tab[index];
        }
    }

    return first_avail;
}

/* buggy find(): stops at the first non-matching slot */
static sec_slot_t *
sec_find_buggy(sec_slot_t *tab, ngx_uint_t slots, uint32_t hash)
{
    ngx_uint_t  i, index;

    for (i = 0; i < slots; i++) {
        index = (sec_index(hash, slots) + i) % slots;

        if (tab[index].state == SEC_FREE || tab[index].hash == hash) {
            return &tab[index];
        }
    }

    return NULL;
}

static void
test_owner_tombstone(void)
{
    sec_slot_t  fixed[8];
    sec_slot_t  buggy[8];
    uint32_t    hash_a = 8;    /* 8 % 8 == 0 */
    uint32_t    hash_b = 16;   /* 16 % 8 == 0, collides with hash_a */
    sec_slot_t *slot;

    TEST_CASE("owner dir: release leaves a tombstone, colliding streams "
              "stay reachable");

    memset(fixed, 0, sizeof(fixed));
    memset(buggy, 0, sizeof(buggy));

    /* claim A then B (B probes past A) under both algorithms */
    slot = sec_find_fixed(fixed, 8, hash_a);
    slot->hash = hash_a;
    slot->state = SEC_OWNED;

    slot = sec_find_fixed(fixed, 8, hash_b);
    slot->hash = hash_b;
    slot->state = SEC_OWNED;

    slot = sec_find_buggy(buggy, 8, hash_a);
    slot->hash = hash_a;
    slot->state = SEC_OWNED;

    slot = sec_find_buggy(buggy, 8, hash_b);
    slot->hash = hash_b;
    slot->state = SEC_OWNED;

    /* release A: fixed leaves a tombstone, buggy leaves FREE */
    slot = sec_find_fixed(fixed, 8, hash_a);
    CHECK(slot != NULL && slot->hash == hash_a, "fixed finds A to release");
    memset(slot, 0, sizeof(*slot));
    slot->state = SEC_DELETED;

    slot = sec_find_buggy(buggy, 8, hash_a);
    memset(slot, 0, sizeof(*slot));
    slot->state = SEC_FREE;

    /* B must still be reachable afterwards */
    slot = sec_find_fixed(fixed, 8, hash_b);
    CHECK(slot != NULL && slot->state == SEC_OWNED && slot->hash == hash_b,
          "fixed: colliding stream survives the release");

    slot = sec_find_buggy(buggy, 8, hash_b);
    CHECK(!(slot != NULL && slot->state == SEC_OWNED && slot->hash == hash_b),
          "buggy algorithm demonstrably loses the colliding stream");

    /* reclaim: a new colliding hash reuses the tombstone */
    {
        uint32_t    hash_c = 24;   /* 24 % 8 == 0 */

        slot = sec_find_fixed(fixed, 8, hash_c);
        CHECK(slot != NULL && slot->state == SEC_DELETED,
              "fixed: new colliding claim reclaims the tombstone");
    }

    /* production pins: the header defines DELETED and the .c uses it */
#ifdef NGX_MEDIA_OWNER_STATE_DELETED
    CHECK(NGX_MEDIA_OWNER_STATE_DELETED == 2,
          "owner.h defines the DELETED tombstone");
#else
    CHECK(0, "owner.h defines the DELETED tombstone (missing pre-fix)");
#endif
    CHECK(file_contains(owner_h_paths,
                        sizeof(owner_h_paths) / sizeof(owner_h_paths[0]),
                        "NGX_MEDIA_OWNER_STATE_DELETED"),
          "owner.h carries the tombstone state");
    CHECK(file_contains(owner_dir_paths,
                        sizeof(owner_dir_paths) / sizeof(owner_dir_paths[0]),
                        "NGX_MEDIA_OWNER_STATE_DELETED"),
          "owner_dir.c uses the tombstone state");
    CHECK(file_contains(owner_dir_paths,
                        sizeof(owner_dir_paths) / sizeof(owner_dir_paths[0]),
                        "first_avail"),
          "owner_dir.c find() remembers the first tombstone");
}

/* ------------------------------------------------------------------ */
/* HTTP hardening (vulns 4-7).                                         */
/* ------------------------------------------------------------------ */

static void
test_http_tls_hostname(void)
{
    TEST_CASE("http: TLS verifies the hostname (SSL_set1_host)");

    /*
     * Verifying the chain without binding it to the requested hostname
     * accepts any valid certificate for another host (MITM).  The fix calls
     * SSL_set1_host() before SSL_connect().
     */
    CHECK(file_contains(http_paths,
                        sizeof(http_paths) / sizeof(http_paths[0]),
                        "SSL_set1_host"),
          "http.c binds the peer certificate to the hostname");
}

static void
test_http_dns(void)
{
    struct sockaddr_in  v4;
    struct addrinfo     hints, *res = NULL;
    int                 gai_ok, v4_ok;

    TEST_CASE("http: outbound connections resolve DNS/IPv6 "
              "(getaddrinfo, not inet_pton-only)");

    /* functional demo: "localhost" is not an IPv4 literal */
    v4_ok = inet_pton(AF_INET, "localhost", &v4.sin_addr);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    gai_ok = getaddrinfo("localhost", "80", &hints, &res);

    CHECK(v4_ok != 1, "inet_pton rejects a DNS name (the old limitation)");
    CHECK(gai_ok == 0 && res != NULL,
          "getaddrinfo resolves DNS names (the fix)");
    if (res != NULL) {
        freeaddrinfo(res);
    }

    /* production pins */
    CHECK(file_contains(http_paths,
                        sizeof(http_paths) / sizeof(http_paths[0]),
                        "getaddrinfo"),
          "http.c resolves via getaddrinfo()");
    CHECK(!file_contains(http_paths,
                         sizeof(http_paths) / sizeof(http_paths[0]),
                         "inet_pton(AF_INET, host_z"),
          "http.c no longer restricts to IPv4 literals");
}

static void
test_http_header_truncation(void)
{
    char    header[1024];
    int     n;
    char    big[2048];

    TEST_CASE("http: snprintf() truncation is rejected before the write");

    /* functional demo: an over-long target does not fit the 1 KiB header */
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    n = snprintf(header, sizeof(header),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                 big, "example.com");

    CHECK(n < 0 || (size_t) n >= sizeof(header),
          "over-long URL is detected as truncation (%d)", n);
    CHECK(file_contains(http_paths,
                        sizeof(http_paths) / sizeof(http_paths[0]),
                        "(size_t) header_len >= sizeof(header)"),
          "http.c clamps snprintf() length before socket write");
}

static void
test_http_thread_pool(void)
{
    TEST_CASE("http: background threads never allocate from ngx_cycle->pool");

    /*
     * ngx_cycle->pool is single-threaded.  put_file() runs on background
     * push threads, so it must use bounded stack buffers.  The pre-fix code
     * called ngx_pnalloc(ngx_cycle->pool, ...).
     */
    CHECK(!file_contains(http_paths,
                         sizeof(http_paths) / sizeof(http_paths[0]),
                         "ngx_cycle->pool"),
          "http.c uses no cycle pool (thread-safe buffers)");
}

/* ------------------------------------------------------------------ */
/* Previous round (566358e): SRT UAF defer + idle sleep.               */
/* ------------------------------------------------------------------ */

static void
test_srt_defer_and_idle(void)
{
    TEST_CASE("srt: remove() during an in-flight send defers the close "
              "(UAF)");

    /*
     * outputs_remove() ran close(session) while the sender thread was
     * outside the lock inside session_send(session) on the same pointer.
     * The fix checks dest->sending under the lock and lets the sender that
     * observes the epoch mismatch close the session after re-locking.
     */
    CHECK(file_contains(srt_out_paths,
                        sizeof(srt_out_paths) / sizeof(srt_out_paths[0]),
                        "if (!dest->sending)"),
          "outputs_remove() defers close while sending");
    CHECK(file_contains(srt_out_paths,
                        sizeof(srt_out_paths) / sizeof(srt_out_paths[0]),
                        "if (!dest->used && dest->session != NULL)"),
          "sender thread reclaims a session removed mid-send");

    TEST_CASE("srt: idle senders sleep on idle_cond instead of spinning");

    /*
     * Sender threads previously looped over empty slots at ~95% CPU.  They
     * now wait on a pool-wide condition when no outputs are used and are
     * woken by add/remove/stop.
     */
    CHECK(file_contains(srt_out_paths,
                        sizeof(srt_out_paths) / sizeof(srt_out_paths[0]),
                        "idle_cond"),
          "sender pool has an idle condition");
    CHECK(file_contains(srt_out_paths,
                        sizeof(srt_out_paths) / sizeof(srt_out_paths[0]),
                        "pthread_cond_wait"),
          "idle senders sleep instead of spinning");
    CHECK(file_contains(srt_out_paths,
                        sizeof(srt_out_paths) / sizeof(srt_out_paths[0]),
                        "pthread_cond_broadcast(&outs->idle_cond"),
          "add/remove/stop wake idle senders");
    CHECK(file_contains(ts_paths,
                        sizeof(ts_paths) / sizeof(ts_paths[0]),
                        "hdr_need"),
          "ts demux accumulates split section headers");
}

/* ------------------------------------------------------------------ */
/* Current working-tree round: HLS ingest name validation, close-on-   */
/* failure, seen ring, route reload fds, segmenter PSI/bytes.          */
/* ------------------------------------------------------------------ */

/* mirrors src/api/ngx_media_api_module.c ingest validation */
static int
sec_valid_segment_name(const u_char *name, size_t len)
{
    size_t  i;

    if (len < 4 || memcmp(name + len - 3, ".ts", 3) != 0
        || name[0] == '.')
    {
        return 0;
    }

    for (i = 0; i < len; i++) {
        u_char  c = name[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
        {
            return 0;
        }
    }

    return 1;
}

static void
test_api_segment_names(void)
{
    TEST_CASE("api: HLS ingest segment names are strict *.ts, no dotfiles, "
              "safe charset");

    CHECK(sec_valid_segment_name((const u_char *) "seg-1.ts", 8),
          "plain segment accepted");
    CHECK(sec_valid_segment_name((const u_char *) "A_9-.ts", 7),
          "charset boundary accepted");
    CHECK(!sec_valid_segment_name((const u_char *) "../evil.ts", 10),
          "path traversal rejected");
    CHECK(!sec_valid_segment_name((const u_char *) ".hidden.ts", 10),
          "dotfile rejected");
    CHECK(!sec_valid_segment_name((const u_char *) "seg-1.m3u8", 10),
          "non-ts suffix rejected");
    CHECK(!sec_valid_segment_name((const u_char *) "seg-1", 5),
          "missing suffix rejected");
    CHECK(!sec_valid_segment_name((const u_char *) "a/b.ts", 6),
          "slash rejected");
    CHECK(!sec_valid_segment_name((const u_char *) "a;b.ts", 6),
          "semicolon rejected");
    CHECK(!sec_valid_segment_name((const u_char *) "a b.ts", 6),
          "space rejected");
    CHECK(!sec_valid_segment_name((const u_char *) "", 0),
          "empty rejected");

    CHECK(file_contains(api_paths,
                        sizeof(api_paths) / sizeof(api_paths[0]),
                        "name.data[0] == '.'"),
          "api rejects dotfiles");
    CHECK(file_contains(api_paths,
                        sizeof(api_paths) / sizeof(api_paths[0]),
                        "\".ts\""),
          "api requires the .ts suffix");
}

static void
test_open_close_on_failure(void)
{
    TEST_CASE("open: half-opened sources are closed on reader-start "
              "failure (fd leak)");

    /*
     * The fix adds one more close call in each open-failure path.  Counting
     * occurrences pins the fix: HEAD has 3/1/1, fixed has 4/2/2.  A plain
     * existence check would pass on HEAD because closes exist elsewhere.
     */
    CHECK(file_count(file_paths,
                     sizeof(file_paths) / sizeof(file_paths[0]),
                     "ngx_media_file_close(source)") >= 4,
          "file source closes on open failure (count>=4)");
    CHECK(file_count(hls_ingest_paths,
                     sizeof(hls_ingest_paths) / sizeof(hls_ingest_paths[0]),
                     "ngx_media_hls_ingest_close(ingest)") >= 2,
          "hls ingest closes on reader-start failure (count>=2)");
    CHECK(file_count(hls_pull_paths,
                     sizeof(hls_pull_paths) / sizeof(hls_pull_paths[0]),
                     "ngx_media_hls_pull_close(pull)") >= 2,
          "hls pull closes on reader-start failure (count>=2)");
}

#define SEC_SEEN 8

static void
test_ingest_seen_ring(void)
{
    u_char      seen[SEC_SEEN][32];
    ngx_uint_t  nseen = 0, seen_next = 0;
    ngx_uint_t  i;

    TEST_CASE("hls ingest: seen-set is a bounded ring, not drop-after-full");

    /* functional demo of the fixed ring: 3x capacity still records */
    for (i = 0; i < 3 * SEC_SEEN; i++) {
        char   name[32];
        size_t len = (size_t) snprintf(name, sizeof(name), "seg-%lu.ts",
                                       (unsigned long) i);

        ngx_uint_t  slot;

        if (nseen < SEC_SEEN) {
            slot = nseen++;
        } else {
            slot = seen_next;
            seen_next = (seen_next + 1) % SEC_SEEN;
        }

        memcpy(seen[slot], name, len + 1);
    }

    CHECK(nseen == SEC_SEEN, "bounded at capacity: %lu", nseen);
    CHECK(seen_next == (2 * SEC_SEEN) % SEC_SEEN,
          "ring advances instead of stalling");

    CHECK(file_contains(hls_ingest_paths,
                        sizeof(hls_ingest_paths)
                        / sizeof(hls_ingest_paths[0]),
                        "seen_next"),
          "hls ingest uses a ring for the seen-set");
}

static void
test_route_reload_fds(void)
{
    TEST_CASE("route: reload with a new worker count closes stale socket "
              "pairs (fd leak)");

    CHECK(file_contains(route_paths,
                        sizeof(route_paths) / sizeof(route_paths[0]),
                        "close(ngx_media_route_fds"),
          "route closes stale fds on worker-count change");
    CHECK(file_contains(route_paths,
                        sizeof(route_paths) / sizeof(route_paths[0]),
                        "ngx_media_route_workers == workers"),
          "route re-inits when the worker count changes");
}

static void
test_hls_segmenter_psi_bytes(void)
{
    ngx_media_hls_t       hls;
    ngx_media_hls_conf_t  conf;
    ngx_media_buf_t      *backing;
    ngx_media_ts_burst_t  burst;

    TEST_CASE("hls segmenter: new segments start with PAT/PMT, bytes "
              "counted once");

    ngx_media_hls_conf_default(&conf);
    conf.path.data = (u_char *) ".build/sec-seg";
    conf.path.len = sizeof(".build/sec-seg") - 1;
    conf.target_duration = 1000;
    conf.min_duration = 1000;
    conf.max_duration = 1000;
    conf.max_segment_bytes = 1 << 20;
    conf.max_segments = 4;
    conf.max_retained_bytes = 1 << 20;

    CHECK(ngx_media_hls_init(&hls, &conf, NULL) == NGX_OK, "segmenter init");

    backing = ngx_media_buf_alloc(1024);
    CHECK(backing != NULL, "backing allocated");
    memset(ngx_media_buf_data(backing), 0x47, 1024);
    (void) ngx_media_buf_freeze(backing, 1024);

    memset(&burst, 0, sizeof(burst));
    burst.backing = backing;
    burst.psi_len = 100;
    burst.nslices = 1;
    burst.slices[0].offset = 100;
    burst.slices[0].len = 400;
    burst.slices[0].media_type = NGX_MEDIA_TYPE_VIDEO;
    burst.slices[0].codec = NGX_MEDIA_CODEC_H264;
    burst.slices[0].keyframe = 1;
    burst.slices[0].dts = 90000;
    burst.slices[0].pts = 90000;

    CHECK(ngx_media_hls_add_burst(&hls, &burst) == NGX_OK, "burst added");

    /*
     * Fixed: pieces = [PSI(100), slice(400)], bytes = 500 counted once by
     * hls_piece().  Buggy: bytes = 100 + 400 + 400 = 900 (explicit
     * `hls->bytes += slice->len` double-counts) and PSI handling differs.
     */
    CHECK(hls.npieces == 2, "psi + slice pieces: %lu", hls.npieces);
    if (hls.npieces == 2) {
        CHECK(hls.pieces[0].len == 100 && hls.pieces[0].offset == 0,
              "first piece is the PAT/PMT prefix");
        CHECK(hls.pieces[1].len == 400 && hls.pieces[1].offset == 100,
              "second piece is the media slice");
    }
    CHECK(hls.bytes == 500, "bytes counted once: %lu", (unsigned long) hls.bytes);

    ngx_media_buf_unref(backing);
    ngx_media_hls_destroy(&hls);

    CHECK(file_contains(hls_seg_paths,
                        sizeof(hls_seg_paths) / sizeof(hls_seg_paths[0]),
                        "if (hls->npieces == 0 && burst->psi_len > 0)"),
          "segmenter prepends PAT/PMT to every new segment");
}

/* ------------------------------------------------------------------ */
/* Round 4: player-queue accounting, null backends, push scan, NAL     */
/* loop, record suffix, route bounds, URL validation, API truncation.  */
/* ------------------------------------------------------------------ */

/* mirrors the reservation in ngx_media_rtmp_queue_message */
#define SEC_RTMP_OUT_CHUNK  4096
#define SEC_RTMP_MAX_QUEUE  64

static void
test_rtmp_player_reserve(void)
{
    /* a 300 KiB keyframe: the writer emits ceil(len/4096) payload slices */
    size_t  len = 300 * 1024;
    size_t  chunks = (len + SEC_RTMP_OUT_CHUNK - 1) / SEC_RTMP_OUT_CHUNK;
    size_t  old_reserve_chain = 1 * 2 + 1;
    size_t  old_reserve_refs = 1;
    size_t  need_chain = chunks * 2 + 1;
    size_t  need_refs = chunks;

    TEST_CASE("rtmp: player queue reserves per-chunk, not per-message");

    CHECK(chunks == 75, "300 KiB is 75 chunks: %lu", chunks);
    CHECK(old_reserve_chain < need_chain && old_reserve_refs < need_refs,
          "the old per-message reservation demonstrably undercounts "
          "(chain %lu<%lu, refs %lu<%lu)",
          old_reserve_chain, need_chain, old_reserve_refs, need_refs);

    /*
     * Admission outcome from an empty queue: the old check passes and the
     * message proceeds into the 64-slot ring (overflow past 64 refs); the
     * fixed check refuses with AGAIN.  A 100 KiB message (25 chunks) still
     * fits under the fixed accounting.
     */
    CHECK(old_reserve_chain <= SEC_RTMP_MAX_QUEUE
          && old_reserve_refs <= SEC_RTMP_MAX_QUEUE,
          "old check admits the 75-chunk message (then overflows)");
    CHECK(need_chain > SEC_RTMP_MAX_QUEUE || need_refs > SEC_RTMP_MAX_QUEUE,
          "fixed check refuses it instead (chain %lu, refs %lu)",
          need_chain, need_refs);
    {
        size_t  small = (100 * 1024 + SEC_RTMP_OUT_CHUNK - 1)
                        / SEC_RTMP_OUT_CHUNK;

        CHECK(small * 2 + 1 <= SEC_RTMP_MAX_QUEUE
              && small <= SEC_RTMP_MAX_QUEUE,
              "a 100 KiB message (%lu chunks) is still admitted", small);
    }
    CHECK(file_contains(rtmp_mod_paths,
                        sizeof(rtmp_mod_paths) / sizeof(rtmp_mod_paths[0]),
                        "(len + NGX_MEDIA_RTMP_OUT_CHUNK - 1)"),
          "player queue sizes the reservation by chunk count");
    CHECK(file_contains(rtmp_mod_paths,
                        sizeof(rtmp_mod_paths) / sizeof(rtmp_mod_paths[0]),
                        "in_flight_count >= NGX_MEDIA_RTMP_MAX_OUT_QUEUE"),
          "player queue guards the ring inside the slice loop");
}

static void
test_srt_null_backend(void)
{
    const char  *err;

    TEST_CASE("srt: transport wrappers survive a missing backend");

    /* with a backend linked these are live values, never NULL */
    err = ngx_media_srt_last_error();
    CHECK(err != NULL, "last_error always returns a string");

    CHECK(file_contains(srt_tr_paths,
                        sizeof(srt_tr_paths) / sizeof(srt_tr_paths[0]),
                        "|| ngx_media_srt_backend()->connect == NULL"),
          "connect() guards the backend pointer");
    CHECK(file_contains(srt_tr_paths,
                        sizeof(srt_tr_paths) / sizeof(srt_tr_paths[0]),
                        "|| ngx_media_srt_backend()->send == NULL"),
          "send() guards the backend pointer");
    CHECK(file_contains(srt_tr_paths,
                        sizeof(srt_tr_paths) / sizeof(srt_tr_paths[0]),
                        "|| ngx_media_srt_backend()->last_error == NULL"),
          "last_error() guards the backend pointer");
}

/* mirrors the fixed scanner condition in ngx_media_hls_push.c */
static int
sec_push_wanted(const char *name)
{
    size_t  len = strlen(name);

    if (len < 4) {
        return 0;
    }

    if (strcmp(name + len - 3, ".ts") == 0) {
        return 1;
    }

    if (len >= 5 && strcmp(name + len - 5, ".m3u8") == 0) {
        return 1;
    }

    return 0;
}

static void
test_push_scan_names(void)
{
    const char  *evil = "abcd";   /* len 4, not a segment */

    TEST_CASE("hls push: 4-char names never read before the buffer");

    /* the old expression evaluates name+4-5 == name-1: demonstrate the
     * underflow without dereferencing it, so this is core-free */
    CHECK(evil + strlen(evil) - 5 < evil,
          "old arithmetic points before the name (the OOB)");

    CHECK(!sec_push_wanted("abcd"), "4-char non-segment skipped");
    CHECK(!sec_push_wanted("abc"), "short name skipped");
    CHECK(sec_push_wanted("seg-1.ts"), ".ts accepted");
    CHECK(sec_push_wanted("index.m3u8"), ".m3u8 accepted");
    CHECK(!sec_push_wanted("index.m3u"), "truncated suffix rejected");

    CHECK(file_contains(hls_push_paths,
                        sizeof(hls_push_paths) / sizeof(hls_push_paths[0]),
                        "name_len >= 5"),
          "scanner guards the .m3u8 suffix length");
}

#define SEC_NAL_EMPTIES  200000

static void
test_nal_no_recursion(void)
{
    u_char                *buf;
    size_t                 i;

    TEST_CASE("nal: a run of empty units terminates without recursion");

    buf = malloc((size_t) SEC_NAL_EMPTIES * 3);
    CHECK(buf != NULL, "fuzz buffer allocated");

    if (buf == NULL) {
        return;
    }

    for (i = 0; i < SEC_NAL_EMPTIES; i++) {
        buf[i * 3] = 0x00;
        buf[i * 3 + 1] = 0x00;
        buf[i * 3 + 2] = 0x01;
    }

    /*
     * Forked probe like the AMF one: 200k nested frames would smash the
     * 8 MB stack on pre-fix code (each level keeps a frame).  The child
     * inherits the no-core limit, so a pre-fix crash is a FAIL, not a
     * core file.
     */
    {
        pid_t  pid;
        int    status = 0;

        fflush(stdout);
        pid = fork();

        if (pid == -1) {
            CHECK(0, "fork for nal probe failed");
        } else if (pid == 0) {
            ngx_media_nal_iter_t  cit;
            ngx_media_nal_t        cnal;
            size_t                 cn = 0;

            ngx_media_nal_iter_init(&cit, buf, (size_t) SEC_NAL_EMPTIES * 3);

            while (ngx_media_nal_iter_next(&cit, &cnal)) {
                cn++;
            }

            _exit((cn == 0) ? 0 : 1);
        } else {
            while (waitpid(pid, &status, 0) < 0) {
                /* retry on EINTR */
            }

            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                CHECK(1, "forked probe: empty units terminate with no NALs");
            } else if (WIFSIGNALED(status)) {
                CHECK(0, "empty units crashed child (signal %d, no core): "
                      "pre-fix recursion", WTERMSIG(status));
            } else {
                CHECK(0, "empty units yielded NALs (child exit %d)",
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            }
        }
    }

    /* the same input in-process on a shallow run: safe at any depth the
     * fixed loop handles, and shallow enough that even pre-fix recursion
     * cannot overflow the stack, so the main process never crashes */
    {
        u_char                small[64 * 3];
        ngx_media_nal_iter_t  sit;
        ngx_media_nal_t        snal;
        size_t                 sn = 0, si;

        for (si = 0; si < 64; si++) {
            small[si * 3] = 0x00;
            small[si * 3 + 1] = 0x00;
            small[si * 3 + 2] = 0x01;
        }

        ngx_media_nal_iter_init(&sit, small, sizeof(small));

        while (ngx_media_nal_iter_next(&sit, &snal)) {
            sn++;
        }

        CHECK(sn == 0, "no NALs from empty units: %lu", sn);
    }

    free(buf);

    CHECK(!file_contains(nal_paths,
                         sizeof(nal_paths) / sizeof(nal_paths[0]),
                         "return ngx_media_nal_iter_next(it, nal);"),
          "iterator loops instead of recursing");
}

static void
test_record_suffix(void)
{
    ngx_media_record_t       rec;
    ngx_media_record_conf_t  conf;
    ngx_media_buf_t         *buf;
    u_char                  *raw;
    char                     expect[256];
    struct stat              st;
    ngx_uint_t               i;

    TEST_CASE("record: part suffix never reads past path.len");

    (void) mkdir(".build", 0755);
    (void) mkdir(".build/sec-record", 0755);

    /*
     * A path slice with no NUL at path.len: pre-fix %s keeps reading into
     * the 'Q' fill, so part 2 lands in a garbage filename and the expected
     * file is missing (FAIL, no crash).  Fixed code uses %.*s.
     */
    raw = malloc(64);
    CHECK(raw != NULL, "path buffer allocated");

    if (raw == NULL) {
        return;
    }

    memcpy(raw, ".build/sec-record/r.ts", 23);
    memset(raw + 23, 'Q', 64 - 23 - 1);
    raw[63] = '\0';

    snprintf(expect, sizeof(expect), ".build/sec-record/r-0002.ts");
    (void) unlink(expect);

    ngx_media_record_conf_default(&conf);
    conf.path.data = raw;
    conf.path.len = 23;
    conf.tap = NGX_MEDIA_RECORD_PROGRAM;
    conf.max_part_bytes = 512;
    conf.max_pending_bytes = 64 * 1024;
    conf.max_jobs = 256;

    CHECK(ngx_media_record_init(&rec, &conf, NULL) == NGX_OK, "record init");

    for (i = 0; i < 16; i++) {
        buf = ngx_media_buf_alloc(256);

        if (buf == NULL) {
            break;
        }

        memset(ngx_media_buf_data(buf), (int) i, 256);
        (void) ngx_media_buf_freeze(buf, 256);
        (void) ngx_media_record_append(&rec, buf, 0, 256);
        ngx_media_buf_unref(buf);
    }

    ngx_media_record_stop(&rec);
    free(raw);

    CHECK(stat(expect, &st) == 0, "part 2 has the exact bounded name");
    (void) unlink(expect);

    CHECK(file_contains(record_paths,
                        sizeof(record_paths) / sizeof(record_paths[0]),
                        "%.*s-%04llu%.*s"),
          "part suffix is bounded by the slice length");
}

static void
test_route_ipc_bounds(void)
{
    /* a track set with a 70 KiB codec config blob */
    size_t  len = sizeof(uint32_t) + 10 * sizeof(uint32_t) + 70 * 1024;

    TEST_CASE("route: open/tracks refuse payloads past one datagram");

    CHECK(len > NGX_MEDIA_IPC_MAX_PAYLOAD,
          "70 KiB config exceeds the %lu-byte datagram payload",
          (unsigned long) NGX_MEDIA_IPC_MAX_PAYLOAD);
    CHECK(file_contains(route_paths,
                        sizeof(route_paths) / sizeof(route_paths[0]),
                        "len > NGX_MEDIA_IPC_MAX_PAYLOAD"),
          "route_open/tracks guard the datagram bound");
}

/* mirrors the fixed ngx_media_http_split validation */
static int
sec_url_ok(const char *url)
{
    const char  *host, *slash, *colon, *end, *target, *p;
    size_t       host_len, target_len;

    if (strncmp(url, "https://", 8) == 0) {
        host = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
    } else {
        return 0;
    }

    end = url + strlen(url);
    slash = strchr(host, '/');

    if (slash == NULL) {
        target = "/";
        target_len = 1;
        host_len = (size_t) (end - host);
    } else {
        target = slash;
        target_len = (size_t) (end - slash);
        host_len = (size_t) (slash - host);
    }

    for (p = target; p < target + target_len; p++) {
        if ((unsigned char) *p <= 0x20 || *p == 0x7F) {
            return 0;
        }
    }

    for (p = host; p < host + host_len; p++) {
        if ((unsigned char) *p <= 0x20 || *p == 0x7F || *p == '#') {
            return 0;
        }
    }

    if (target[0] != '/') {
        return 0;
    }

    colon = memchr(host, ':', host_len);

    if (colon != NULL) {
        long  port = atol(colon + 1);

        if (port <= 0 || port > 65535) {
            return 0;
        }

        host_len = (size_t) (colon - host);
    }

    return host_len > 0 && target_len > 0;
}

static void
test_http_split_validation(void)
{
    TEST_CASE("http: endpoint URLs reject controls and bad ports");

    CHECK(sec_url_ok("http://example.com/live/a.ts"), "plain URL accepted");
    CHECK(sec_url_ok("https://example.com:8443/a.ts"), "explicit port ok");
    CHECK(!sec_url_ok("http://example.com/a\r\nX-Evil: 1"),
          "CRLF injection rejected");
    CHECK(!sec_url_ok("http://example.com/a b.ts"), "space rejected");
    CHECK(!sec_url_ok("http://example.com:99999/a.ts"), "port range checked");
    CHECK(!sec_url_ok("http://example.com:0/a.ts"), "port zero rejected");
    CHECK(!sec_url_ok("http:///a.ts"), "empty host rejected");
    CHECK(!sec_url_ok("ftp://example.com/a.ts"), "scheme restricted");

    CHECK(file_contains(http_paths,
                        sizeof(http_paths) / sizeof(http_paths[0]),
                        "*p <= 0x20"),
          "http_split rejects control bytes");
    CHECK(file_contains(http_paths,
                        sizeof(http_paths) / sizeof(http_paths[0]),
                        "> 65535"),
          "http_split validates the port range");
}

static void
test_api_truncation(void)
{
    TEST_CASE("api: truncated JSON reports 500, never a cut 200");

    /*
     * destination_json used to return OK unconditionally and desired_get
     * closed with OK: three pre-existing uses of the status idiom, five
     * once both report truncation.
     */
    CHECK(file_count(api_paths,
                     sizeof(api_paths) / sizeof(api_paths[0]),
                     "return (*last < end - 1) ? NGX_OK : NGX_ERROR;") >= 4,
          "destination_json/desired tail report truncation");
    CHECK(file_count(api_paths,
                     sizeof(api_paths) / sizeof(api_paths[0]),
                     "if (ngx_media_api_destination_json") >= 4,
          "truncation propagates to a 500 status");
}

int
main(void)
{
    disable_coredumps();
    printf("== security hardening regressions\n");

    /* last committed round */
    test_rtmp_length_change();
    test_amf_skip_oob();
    test_ts_psi_split();
    test_owner_tombstone();
    test_http_tls_hostname();
    test_http_dns();
    test_http_header_truncation();
    test_http_thread_pool();

    /* previous round */
    test_srt_defer_and_idle();

    /* current working-tree round */
    test_api_segment_names();
    test_open_close_on_failure();
    test_ingest_seen_ring();
    test_route_reload_fds();
    test_hls_segmenter_psi_bytes();

    /* round 4 */
    test_rtmp_player_reserve();
    test_srt_null_backend();
    test_push_scan_names();
    test_nal_no_recursion();
    test_record_suffix();
    test_route_ipc_bounds();
    test_http_split_validation();
    test_api_truncation();

    TEST_LEAKS();
    TEST_MAIN_END();
}

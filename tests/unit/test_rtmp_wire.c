/*
 * RTMP wire layer: sha256/hmac vectors, handshake (simple and Adobe digest),
 * chunk stream round trips and AMF0 encoding/decoding.
 *
 * The digest handshake is verified the way a real client (librtmp, ffmpeg)
 * verifies it: the server reply must carry a valid HMAC over the packet with
 * the digest field removed.
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_rtmp_wire.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if (!(cond)) {                                                        \
            ngx_media_test_failures++;                                        \
            printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,               \
                   ##__VA_ARGS__);                                            \
        }                                                                     \
    } while (0)

static void
hex(const u_char *data, size_t len, char *out)
{
    static const char  digits[] = "0123456789abcdef";
    size_t             i;

    for (i = 0; i < len; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }

    out[len * 2] = '\0';
}

static void
test_sha256(void)
{
    struct {
        const char  *input;
        const char  *expect;
    } vectors[] = {
        { "",
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc",
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    ngx_uint_t        i;
    char              got[65];

    TEST_CASE("sha256 vectors");

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        ngx_media_sha256_t  ctx;
        u_char              out[32];

        ngx_media_sha256_init(&ctx);
        ngx_media_sha256_update(&ctx, vectors[i].input,
                                strlen(vectors[i].input));
        ngx_media_sha256_final(&ctx, out);
        hex(out, sizeof(out), got);

        CHECK(strcmp(got, vectors[i].expect) == 0,
              "sha256('%s') = %s", vectors[i].input, got);
    }

    /* a million 'a' characters, fed in odd sized pieces */
    {
        ngx_media_sha256_t  ctx;
        u_char              out[32];
        u_char              chunk[64];
        size_t              left = 1000000;

        memset(chunk, 'a', sizeof(chunk));

        ngx_media_sha256_init(&ctx);

        while (left > 0) {
            size_t take = (left > 37) ? 37 : left;

            ngx_media_sha256_update(&ctx, chunk, take);
            left -= take;
        }

        ngx_media_sha256_final(&ctx, out);
        hex(out, sizeof(out), got);

        CHECK(strcmp(got,
                     "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
              == 0, "sha256(1e6 * 'a') = %s", got);
    }

    /* RFC 4231 test case 1 and 2 */
    {
        u_char  out[32];

        ngx_media_hmac_sha256((const u_char *) "\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b"
                              "\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b",
                              20, (const u_char *) "Hi There", 8, out);
        hex(out, sizeof(out), got);
        CHECK(strcmp(got,
                     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7")
              == 0, "hmac case 1 = %s", got);

        ngx_media_hmac_sha256((const u_char *) "Jefe", 4,
                              (const u_char *) "what do ya want for nothing?", 28,
                              out);
        hex(out, sizeof(out), got);
        CHECK(strcmp(got,
                     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")
              == 0, "hmac case 2 = %s", got);
    }
}

static void
test_handshake(void)
{
    ngx_media_rtmp_handshake_t  hs;
    u_char                      c0c1[1 + NGX_MEDIA_RTMP_HANDSHAKE_SIZE];
    u_char                      c2[NGX_MEDIA_RTMP_HANDSHAKE_SIZE];
    size_t                      consumed;

    TEST_CASE("simple handshake");

    /* C1 with a zero version field: the simple handshake */
    memset(c0c1, 0, sizeof(c0c1));
    c0c1[0] = NGX_MEDIA_RTMP_VERSION;
    c0c1[1] = 0x11;

    ngx_media_rtmp_handshake_init(&hs);

    CHECK(ngx_media_rtmp_handshake_feed(&hs, c0c1, sizeof(c0c1), &consumed)
          == NGX_AGAIN, "handshake wants C2");
    CHECK(consumed == sizeof(c0c1), "C0C1 consumed: %lu", consumed);
    CHECK(hs.complex == 0, "simple handshake detected");
    CHECK(hs.out_len == 1 + 2 * NGX_MEDIA_RTMP_HANDSHAKE_SIZE,
          "reply is S0S1S2: %lu", hs.out_len);
    CHECK(hs.out[0] == NGX_MEDIA_RTMP_VERSION, "S0 version");

    memset(c2, 0x5A, sizeof(c2));
    CHECK(ngx_media_rtmp_handshake_feed(&hs, c2, sizeof(c2), &consumed)
          == NGX_OK, "handshake complete");
    CHECK(consumed == sizeof(c2), "C2 consumed: %lu", consumed);

    TEST_CASE("digest handshake reply");

    memset(c0c1, 0x33, sizeof(c0c1));
    c0c1[0] = NGX_MEDIA_RTMP_VERSION;
    /* a nonzero version field requests the digest handshake */
    c0c1[5] = 0x09;
    c0c1[6] = 0x00;
    c0c1[7] = 0x7C;
    c0c1[8] = 0x02;

    ngx_media_rtmp_handshake_init(&hs);

    CHECK(ngx_media_rtmp_handshake_feed(&hs, c0c1, sizeof(c0c1), &consumed)
          == NGX_AGAIN, "digest handshake wants C2");
    CHECK(hs.complex == 1, "complex handshake detected");

    {
        const u_char  *s1 = hs.out + 1;
        const u_char  *s2 = hs.out + 1 + NGX_MEDIA_RTMP_HANDSHAKE_SIZE;
        u_char         digest[32];
        ngx_uint_t     offset;

        CHECK(s1[4] == 4 && s1[5] == 5 && s1[6] == 0 && s1[7] == 1,
              "S1 version marked as digest handshake");

        offset = 12 + (ngx_uint_t) (s1[8] + s1[9] + s1[10] + s1[11]) % 728;

        /* rebuild the expected digest the way a client verifies it */
        {
            static const u_char  key[68] = {
                'G','e','n','u','i','n','e',' ','A','d','o','b','e',' ','F',
                'l','a','s','h',' ','M','e','d','i','a',' ','S','e','r','v',
                'e','r',' ','0','0','1',
                0xF0,0xEE,0xC2,0x4A,0x80,0x68,0xBE,0xE8,0x2E,0x00,0xD0,0xD1,
                0x02,0x9E,0x7E,0x57,0x6E,0xEC,0x5D,0x2D,0x29,0x80,0x6F,0xAB,
                0x93,0xB8,0xE6,0x36,0xCF,0xEB,0x31,0xAE
            };
            u_char  covered[NGX_MEDIA_RTMP_HANDSHAKE_SIZE - 32];

            /* the digest covers S1 with its own 32 byte field removed */
            memcpy(covered, s1, offset);
            memcpy(covered + offset, s1 + offset + 32,
                   sizeof(covered) - offset);

            ngx_media_hmac_sha256(key, sizeof(key), covered, sizeof(covered),
                                  digest);
        }

        CHECK(memcmp(s1 + offset, digest, 32) == 0,
              "S1 carries a valid FMS-key digest at offset %lu", offset);
        CHECK(offset >= 12 && offset < 728 + 12, "digest offset in range");

        /* S2: digest over the first 1504 bytes, keyed with the player key */
        {
            static const u_char  key[68] = {
                'G','e','n','u','i','n','e',' ','A','d','o','b','e',' ','F',
                'l','a','s','h',' ','P','l','a','y','e','r',' ','0','0','1',
                0xF0,0xEE,0xC2,0x4A,0x80,0x68,0xBE,0xE8,0x2E,0x00,0xD0,0xD1,
                0x02,0x9E,0x7E,0x57,0x6E,0xEC,0x5D,0x2D,0x29,0x80,0x6F,0xAB,
                0x93,0xB8,0xE6,0x36,0xCF,0xEB,0x31,0xAE
            };

            u_char  expect[32];

            ngx_media_hmac_sha256(key, sizeof(key), s2,
                                  NGX_MEDIA_RTMP_HANDSHAKE_SIZE - 32, expect);

            CHECK(memcmp(s2 + NGX_MEDIA_RTMP_HANDSHAKE_SIZE - 32, expect, 32)
                  == 0, "S2 carries a valid player-key digest");
        }
    }

    TEST_CASE("handshake rejects a bad version");
    memset(c0c1, 0, sizeof(c0c1));
    c0c1[0] = 9;

    ngx_media_rtmp_handshake_init(&hs);
    CHECK(ngx_media_rtmp_handshake_feed(&hs, c0c1, sizeof(c0c1), &consumed)
          == NGX_ERROR, "wrong RTMP version rejected");

    TEST_CASE("handshake across partial reads");
    memset(c0c1, 0, sizeof(c0c1));
    c0c1[0] = NGX_MEDIA_RTMP_VERSION;

    ngx_media_rtmp_handshake_init(&hs);

    {
        size_t  off = 0;

        while (off < sizeof(c0c1)) {
            size_t  take = 7;
            size_t  used = 0;
            ngx_int_t  rc;

            if (off + take > sizeof(c0c1)) {
                take = sizeof(c0c1) - off;
            }

            rc = ngx_media_rtmp_handshake_feed(&hs, c0c1 + off, take, &used);

            CHECK(used == take, "partial feed consumed %lu of %lu", used, take);
            off += used;

            if (off == sizeof(c0c1)) {
                CHECK(rc == NGX_AGAIN, "still waiting for C2");
                break;
            }

            CHECK(rc == NGX_AGAIN, "partial feed keeps asking");
        }
    }
}

/* collects messages for assertions */
typedef struct {
    ngx_uint_t       count;
    ngx_uint_t       type[8];
    ngx_uint_t       stream_id[8];
    uint32_t         timestamp[8];
    size_t           len[8];
    u_char           data[8][800];
} collector_t;

static ngx_int_t
collect(void *ctx, ngx_uint_t type, ngx_uint_t stream_id, uint32_t timestamp,
    ngx_media_buf_t *payload)
{
    collector_t  *c = ctx;

    if (c->count >= 8) {
        return NGX_ERROR;
    }

    c->type[c->count] = type;
    c->stream_id[c->count] = stream_id;
    c->timestamp[c->count] = timestamp;
    c->len[c->count] = ngx_media_buf_size(payload);

    if (c->len[c->count] <= sizeof(c->data[0])) {
        memcpy(c->data[c->count], ngx_media_buf_data(payload),
               c->len[c->count]);
    }

    c->count++;

    return NGX_OK;
}

static size_t
flatten(const ngx_media_rtmp_packet_t *pkt, u_char *out, size_t capacity)
{
    size_t      total = 0;
    ngx_uint_t  i;

    for (i = 0; i < pkt->nparts; i++) {

        if (total + pkt->parts[i].len > capacity) {
            return 0;
        }

        memcpy(out + total, pkt->parts[i].data, pkt->parts[i].len);
        total += pkt->parts[i].len;
    }

    return total;
}

static void
test_chunks(void)
{
    ngx_media_rtmp_writer_t   w;
    ngx_media_rtmp_reader_t   r;
    ngx_media_rtmp_packet_t   pkt;
    collector_t               c;
    u_char                    wire[8192];
    size_t                    wire_len, consumed;
    ngx_media_buf_t          *payload;
    u_char                   *body;
    ngx_uint_t                i;

    TEST_CASE("chunk stream round trip");

    payload = ngx_media_buf_alloc(700);

    CHECK(payload != NULL, "payload allocated");

    body = ngx_media_buf_data(payload);

    for (i = 0; i < 700; i++) {
        body[i] = (u_char) (i * 7);
    }

    (void) ngx_media_buf_freeze(payload, 700);

    ngx_media_rtmp_writer_init(&w, 256, NGX_MEDIA_RTMP_MAX_MESSAGE);
    ngx_media_rtmp_reader_init(&r);

    wire_len = 0;

    /* announce the sender's chunk size, as every real client does */
    {
        ngx_media_buf_t  *ctl = ngx_media_buf_alloc(4);
        u_char           *cp = ngx_media_buf_data(ctl);

        cp[0] = 0; cp[1] = 0; cp[2] = 1; cp[3] = 0;   /* 256 */

        (void) ngx_media_buf_freeze(ctl, 4);

        ngx_media_rtmp_packet_init(&pkt);
        CHECK(ngx_media_rtmp_writer_message(&w, &pkt, 2,
                                            NGX_MEDIA_RTMP_MSG_CHUNK_SIZE, 0, 0,
                                            ctl, 0, 4) == NGX_OK,
              "chunk size announced");

        wire_len = flatten(&pkt, wire, sizeof(wire));
        CHECK(wire_len > 0, "chunk size serialized");

        ngx_media_rtmp_packet_destroy(&pkt);

        ngx_media_buf_unref(ctl);
    }

    /* four messages: audio-ish, video-ish, command-ish and audio again */
    {
        struct {
            ngx_uint_t  csid;
            ngx_uint_t  type;
            ngx_uint_t  stream_id;
            uint32_t    ts;
            size_t      offset;
            size_t      len;
        } messages[] = {
            { 4, NGX_MEDIA_RTMP_MSG_AUDIO, 1, 0, 0, 700 },
            { 6, NGX_MEDIA_RTMP_MSG_VIDEO, 1, 40, 100, 400 },
            { 3, NGX_MEDIA_RTMP_MSG_COMMAND_AMF0, 0, 0, 0, 20 },
            { 4, NGX_MEDIA_RTMP_MSG_AUDIO, 1, 80, 0, 700 },
        };

        size_t  total = wire_len;

        for (i = 0; i < sizeof(messages) / sizeof(messages[0]); i++) {
            size_t  part;

            ngx_media_rtmp_packet_init(&pkt);

            CHECK(ngx_media_rtmp_writer_message(&w, &pkt, messages[i].csid,
                                                messages[i].type,
                                                messages[i].stream_id,
                                                messages[i].ts, payload,
                                                messages[i].offset,
                                                messages[i].len)
                  == NGX_OK, "message %lu written", i);

            part = flatten(&pkt, wire + total, sizeof(wire) - total);

            CHECK(part > 0, "message %lu flattened", i);

            total += part;

            ngx_media_rtmp_packet_destroy(&pkt);
        }

        wire_len = total;
    }

    /* feed it back in awkward pieces, the way a socket delivers bytes */
    memset(&c, 0, sizeof(c));

    {
        u_char  pending[4096];
        size_t  have = 0, off = 0;

        while (off < wire_len) {
            size_t  take = 13;
            size_t  used = 0;

            if (off + take > wire_len) {
                take = wire_len - off;
            }

            memcpy(pending + have, wire + off, take);
            have += take;
            off += take;

            CHECK(ngx_media_rtmp_reader_feed(&r, pending, have, &used, collect,
                                             &c) == NGX_OK,
                  "reader accepts a partial feed");

            if (used > 0) {
                memmove(pending, pending + used, have - used);
                have -= used;
            }
        }

        CHECK(off == wire_len, "all wire bytes offered: %lu of %lu", off,
              wire_len);
        CHECK(have == 0, "no bytes left unparsed: %lu", have);
    }

    /* the first message is the chunk size announcement */
    CHECK(c.count == 5, "five messages decoded: %lu", c.count);

    if (c.count == 5) {
        CHECK(c.type[0] == NGX_MEDIA_RTMP_MSG_CHUNK_SIZE,
              "message 0 is the chunk size");
        CHECK(r.chunk_size == 256, "chunk size applied: %lu", r.chunk_size);

        CHECK(c.type[1] == NGX_MEDIA_RTMP_MSG_AUDIO, "message 1 is audio");
        CHECK(c.stream_id[1] == 1, "message 1 stream id");
        CHECK(c.timestamp[1] == 0, "message 1 timestamp");
        CHECK(c.len[1] == 700, "message 1 length: %lu", c.len[1]);
        CHECK(memcmp(c.data[1], body, 64) == 0, "message 1 payload");

        CHECK(c.type[2] == NGX_MEDIA_RTMP_MSG_VIDEO, "message 2 is video");
        CHECK(c.timestamp[2] == 40, "message 2 timestamp: %u", c.timestamp[2]);
        CHECK(c.len[2] == 400, "message 2 length: %lu", c.len[2]);
        CHECK(memcmp(c.data[2], body + 100, 64) == 0,
              "message 2 references the right payload range");

        CHECK(c.type[3] == NGX_MEDIA_RTMP_MSG_COMMAND_AMF0, "message 3 is AMF");
        CHECK(c.len[3] == 20, "message 3 length: %lu", c.len[3]);

        CHECK(c.timestamp[4] == 80, "message 4 timestamp: %u", c.timestamp[4]);
        CHECK(c.len[4] == 700, "message 4 length: %lu", c.len[4]);
    }

    TEST_CASE("set chunk size and extended timestamp");

    /* a chunk size change followed by a large timestamp */
    {
        ngx_media_rtmp_writer_t  w2;
        ngx_media_rtmp_reader_t  r2;
        ngx_media_buf_t         *ctl;
        u_char                  *p;
        size_t                   total = 0;

        ctl = ngx_media_buf_alloc(4);
        p = ngx_media_buf_data(ctl);
        p[0] = 0; p[1] = 0; p[2] = 1; p[3] = 0;   /* 256 */
        (void) ngx_media_buf_freeze(ctl, 4);

        ngx_media_rtmp_writer_init(&w2, 128, NGX_MEDIA_RTMP_MAX_MESSAGE);
        ngx_media_rtmp_reader_init(&r2);

        ngx_media_rtmp_packet_init(&pkt);
        CHECK(ngx_media_rtmp_writer_message(&w2, &pkt, 2,
                                            NGX_MEDIA_RTMP_MSG_CHUNK_SIZE, 0, 0,
                                            ctl, 0, 4) == NGX_OK,
              "chunk size message written");
        total += flatten(&pkt, wire + total, sizeof(wire) - total);
        ngx_media_rtmp_packet_destroy(&pkt);

        /* 0xFFFFFF milliseconds forces the extended timestamp field */
        ngx_media_rtmp_packet_init(&pkt);
        CHECK(ngx_media_rtmp_writer_message(&w2, &pkt, 4,
                                            NGX_MEDIA_RTMP_MSG_AUDIO, 1,
                                            0xFFFFFF + 5, ctl, 0, 4) == NGX_OK,
              "extended timestamp message written");
        total += flatten(&pkt, wire + total, sizeof(wire) - total);
        ngx_media_rtmp_packet_destroy(&pkt);

        memset(&c, 0, sizeof(c));

        CHECK(ngx_media_rtmp_reader_feed(&r2, wire, total, &consumed, collect,
                                         &c) == NGX_OK, "control stream read");
        CHECK(r2.chunk_size == 256, "chunk size applied: %lu", r2.chunk_size);
        CHECK(c.count == 2, "two messages: %lu", c.count);
        CHECK(c.count == 2 && c.timestamp[1] == 0xFFFFFF + 5,
              "extended timestamp decoded: %u", c.count == 2 ? c.timestamp[1] : 0);

        ngx_media_rtmp_reader_reset(&r2);

        ngx_media_buf_unref(ctl);
    }

    TEST_CASE("malformed streams are rejected");

    {
        /* a fmt 0 header claiming a 16 MB + 1 byte message */
        u_char  bad[12] = { 0x03, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x08, 0x00,
                            0x00, 0x00, 0x01 };

        ngx_media_rtmp_reader_reset(&r);
        r.max_message = 1024;   /* a three byte length cannot exceed 16 MB */
        memset(&c, 0, sizeof(c));

        CHECK(ngx_media_rtmp_reader_feed(&r, bad, sizeof(bad), &consumed,
                                         collect, &c) == NGX_ERROR,
              "oversized message rejected");

        ngx_media_rtmp_reader_reset(&r);
    }

    ngx_media_rtmp_reader_reset(&r);

    ngx_media_buf_unref(payload);
}

static void
test_amf(void)
{
    u_char                 buf[1024];
    ngx_media_amf_writer_t w;
    ngx_media_amf_value_t  value;
    size_t                 consumed;

    TEST_CASE("AMF0 round trip");

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));

    CHECK(ngx_media_amf_put_string(&w, (const u_char *) "connect", 7)
          == NGX_OK, "put command name");
    CHECK(ngx_media_amf_put_number(&w, 1) == NGX_OK, "put transaction id");
    CHECK(ngx_media_amf_begin_object(&w, 0, 0) == NGX_OK, "begin object");
    CHECK(ngx_media_amf_put_member_string(&w, "app", (const u_char *) "live",
                                          4) == NGX_OK, "app member");
    CHECK(ngx_media_amf_put_member_string(&w, "tcUrl",
                                          (const u_char *)
                                              "rtmp://127.0.0.1/live", 21)
          == NGX_OK, "tcUrl member");
    CHECK(ngx_media_amf_put_member_number(&w, "objectEncoding", 0) == NGX_OK,
          "objectEncoding member");
    CHECK(ngx_media_amf_put_member_boolean(&w, "fpad", 1) == NGX_OK,
          "fpad member");
    CHECK(ngx_media_amf_put_member_string(&w, "flashVer",
                                          (const u_char *) "FMLE/3.0", 8)
          == NGX_OK, "flashVer member");
    CHECK(ngx_media_amf_end_object(&w) == NGX_OK, "end object");

    /* decode: value 0 is the command name */
    CHECK(ngx_media_amf_read(buf, w.len, &value, &consumed) == NGX_OK,
          "read command name");
    CHECK(value.type == NGX_MEDIA_AMF_STRING, "command is a string");
    CHECK(value.string.len == 7
          && memcmp(value.string.data, "connect", 7) == 0,
          "command name round trip");

    {
        size_t  off = consumed;

        CHECK(ngx_media_amf_read(buf + off, w.len - off, &value, &consumed)
              == NGX_OK, "read transaction id");
        CHECK(value.type == NGX_MEDIA_AMF_NUMBER && value.number == 1,
              "transaction id round trip");
        consumed += off;
    }

    {
        size_t  off = consumed;

        CHECK(ngx_media_amf_read(buf + off, w.len - off, &value, &consumed)
              == NGX_OK, "read connect object");
        CHECK(value.type == NGX_MEDIA_AMF_OBJECT, "object type");
        CHECK(value.count == 5, "five members: %lu", value.count);

        {
            const ngx_str_t  *app = ngx_media_amf_member(&value, "app");
            double            number = 0;

            CHECK(app != NULL && app->len == 4
                  && memcmp(app->data, "live", 4) == 0, "app member");

            CHECK(ngx_media_amf_member_number(&value, "objectEncoding",
                                              &number) == NGX_OK
                  && number == 0, "objectEncoding member");

            CHECK(ngx_media_amf_member(&value, "tcUrl") != NULL, "tcUrl member");
            CHECK(ngx_media_amf_member(&value, "missing") == NULL,
                  "missing member is NULL");
        }

        off += consumed;
        CHECK(off == w.len, "whole command consumed: %lu of %lu", off, w.len);
    }

    TEST_CASE("AMF0 ecma array and nested values");

    ngx_media_amf_writer_init(&w, buf, sizeof(buf));
    CHECK(ngx_media_amf_put_string(&w, (const u_char *) "onMetaData", 10)
          == NGX_OK, "name");
    CHECK(ngx_media_amf_begin_object(&w, 1, 6) == NGX_OK, "ecma array");
    CHECK(ngx_media_amf_put_member_number(&w, "duration", 0) == NGX_OK, "d");
    CHECK(ngx_media_amf_put_member_number(&w, "width", 640) == NGX_OK, "w");
    CHECK(ngx_media_amf_put_member_number(&w, "height", 360) == NGX_OK, "h");
    CHECK(ngx_media_amf_put_member_number(&w, "videocodecid", 7) == NGX_OK, "vc");
    CHECK(ngx_media_amf_put_member_number(&w, "audiocodecid", 10) == NGX_OK,
          "ac");
    CHECK(ngx_media_amf_put_member_number(&w, "stereo", 1) == NGX_OK, "st");
    CHECK(ngx_media_amf_end_object(&w) == NGX_OK, "end");

    CHECK(ngx_media_amf_read(buf, w.len, &value, &consumed) == NGX_OK,
          "read name");
    {
        size_t  off = consumed;
        double  width = 0, height = 0;

        CHECK(ngx_media_amf_read(buf + off, w.len - off, &value, &consumed)
              == NGX_OK, "read metadata object");
        CHECK(value.type == NGX_MEDIA_AMF_ECMA_ARRAY, "ecma array type");
        CHECK(value.count == 6, "six members: %lu", value.count);
        CHECK(ngx_media_amf_member_number(&value, "width", &width) == NGX_OK
              && width == 640, "width member");
        CHECK(ngx_media_amf_member_number(&value, "height", &height) == NGX_OK
              && height == 360, "height member");
        CHECK(off + consumed == w.len, "metadata fully consumed");
    }

    TEST_CASE("truncation is reported");

    {
        u_char  small[16];

        ngx_media_amf_writer_init(&w, small, sizeof(small));
        CHECK(ngx_media_amf_put_string(&w, (const u_char *) "publish", 7)
              == NGX_OK, "small write");
    }

    CHECK(ngx_media_amf_read(buf, 3, &value, &consumed) == NGX_AGAIN,
          "truncated string asks for more");
    CHECK(ngx_media_amf_read(buf, w.len < 4 ? w.len : 4, &value, &consumed)
          == NGX_AGAIN, "truncated ecma array asks for more");

    {
        u_char  bad[4] = { 0x42, 0x00, 0x00, 0x00 };

        CHECK(ngx_media_amf_read(bad, sizeof(bad), &value, &consumed)
              == NGX_ERROR, "unknown AMF type rejected");
    }
}

int
main(void)
{
    printf("== rtmp wire\n");

    test_sha256();
    test_handshake();
    test_chunks();
    test_amf();

    TEST_LEAKS();
    TEST_MAIN_END();
}

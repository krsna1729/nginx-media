#include "ngx_media_rtmp_wire.h"

#include <string.h>
#include <time.h>

/* --- sha256 -------------------------------------------------------------- */

static const uint32_t ngx_media_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define NGX_MEDIA_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
ngx_media_sha256_block(ngx_media_sha256_t *ctx, const u_char *p)
{
    uint32_t   w[64], a, b, c, d, e, f, g, h, t1, t2;
    ngx_uint_t i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t) p[i * 4] << 24) | ((uint32_t) p[i * 4 + 1] << 16)
               | ((uint32_t) p[i * 4 + 2] << 8) | (uint32_t) p[i * 4 + 3];
    }

    for (i = 16; i < 64; i++) {
        uint32_t s0 = NGX_MEDIA_ROTR(w[i - 15], 7)
                      ^ NGX_MEDIA_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = NGX_MEDIA_ROTR(w[i - 2], 17)
                      ^ NGX_MEDIA_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2];
    d = ctx->state[3]; e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t s1 = NGX_MEDIA_ROTR(e, 6) ^ NGX_MEDIA_ROTR(e, 11)
                      ^ NGX_MEDIA_ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t s0 = NGX_MEDIA_ROTR(a, 2) ^ NGX_MEDIA_ROTR(a, 13)
                      ^ NGX_MEDIA_ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);

        t1 = h + s1 + ch + ngx_media_sha256_k[i] + w[i];
        t2 = s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

void
ngx_media_sha256_init(ngx_media_sha256_t *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bits = 0;
    ctx->have = 0;
}

static void
ngx_media_sha256_consume(ngx_media_sha256_t *ctx, const u_char *p, size_t len)
{
    if (ctx->have > 0) {
        size_t take = 64 - ctx->have;

        if (take > len) {
            take = len;
        }

        memcpy(ctx->block + ctx->have, p, take);

        ctx->have += take;
        p += take;
        len -= take;

        if (ctx->have == 64) {
            ngx_media_sha256_block(ctx, ctx->block);
            ctx->have = 0;
        }
    }

    while (len >= 64) {
        ngx_media_sha256_block(ctx, p);
        p += 64;
        len -= 64;
    }

    if (len > 0) {
        memcpy(ctx->block, p, len);
        ctx->have = len;
    }
}

void
ngx_media_sha256_update(ngx_media_sha256_t *ctx, const void *data, size_t len)
{
    ctx->bits += (uint64_t) len * 8;

    ngx_media_sha256_consume(ctx, data, len);
}

void
ngx_media_sha256_final(ngx_media_sha256_t *ctx, u_char out[32])
{
    u_char      pad[72];
    uint64_t    bits = ctx->bits;
    size_t      pad_len;
    ngx_uint_t  i;

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;

    /* 0x80, zeros, then the message length in the last eight bytes */
    pad_len = (ctx->have < 56) ? (56 - ctx->have) : (120 - ctx->have);

    for (i = 0; i < 8; i++) {
        pad[pad_len + i] = (u_char) ((bits >> ((7 - i) * 8)) & 0xFF);
    }

    ngx_media_sha256_consume(ctx, pad, pad_len + 8);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (u_char) (ctx->state[i] >> 24);
        out[i * 4 + 1] = (u_char) (ctx->state[i] >> 16);
        out[i * 4 + 2] = (u_char) (ctx->state[i] >> 8);
        out[i * 4 + 3] = (u_char) ctx->state[i];
    }
}

void
ngx_media_hmac_sha256(const u_char *key, size_t key_len, const u_char *data,
    size_t len, u_char out[32])
{
    u_char               k[64], inner[32];
    ngx_media_sha256_t   ctx;
    ngx_uint_t           i;

    memset(k, 0, sizeof(k));

    if (key_len > sizeof(k)) {
        ngx_media_sha256_init(&ctx);
        ngx_media_sha256_update(&ctx, key, key_len);
        ngx_media_sha256_final(&ctx, k);

    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }

    ngx_media_sha256_init(&ctx);

    for (i = 0; i < sizeof(k); i++) {
        k[i] ^= 0x36;
    }

    ngx_media_sha256_update(&ctx, k, sizeof(k));
    ngx_media_sha256_update(&ctx, data, len);
    ngx_media_sha256_final(&ctx, inner);

    for (i = 0; i < sizeof(k); i++) {
        k[i] ^= 0x36 ^ 0x5C;
    }

    ngx_media_sha256_init(&ctx);
    ngx_media_sha256_update(&ctx, k, sizeof(k));
    ngx_media_sha256_update(&ctx, inner, sizeof(inner));
    ngx_media_sha256_final(&ctx, out);
}

/* --- handshake ----------------------------------------------------------- */

/*
 * Adobe key material.  Only the printable part of each key is used, which is
 * what libavformat's validator and the FMS specification use: the server's S1
 * digest is keyed with the media server key (36 bytes) at the offset derived
 * from S1 itself.
 */
static const u_char ngx_media_rtmp_fms_key[68] = {
    'G','e','n','u','i','n','e',' ','A','d','o','b','e',' ','F','l','a','s',
    'h',' ','M','e','d','i','a',' ','S','e','r','v','e','r',' ','0','0','1',
    0xF0,0xEE,0xC2,0x4A,0x80,0x68,0xBE,0xE8,0x2E,0x00,0xD0,0xD1,0x02,0x9E,
    0x7E,0x57,0x6E,0xEC,0x5D,0x2D,0x29,0x80,0x6F,0xAB,0x93,0xB8,0xE6,0x36,
    0xCF,0xEB,0x31,0xAE
};

/* only the printable prefix of the key signs S1; S2 uses the full key */
#define NGX_MEDIA_RTMP_SERVER_KEY_LEN 36
#define NGX_MEDIA_RTMP_PLAYER_KEY_LEN 30

static const u_char ngx_media_rtmp_player_key[68] = {
    'G','e','n','u','i','n','e',' ','A','d','o','b','e',' ','F','l','a','s',
    'h',' ','P','l','a','y','e','r',' ','0','0','1',
    0xF0,0xEE,0xC2,0x4A,0x80,0x68,0xBE,0xE8,0x2E,0x00,0xD0,0xD1,0x02,0x9E,
    0x7E,0x57,0x6E,0xEC,0x5D,0x2D,0x29,0x80,0x6F,0xAB,0x93,0xB8,0xE6,0x36,
    0xCF,0xEB,0x31,0xAE
};


static void
ngx_media_rtmp_random(u_char *dst, size_t len, uint64_t seed)
{
    uint64_t  x = seed ? seed : 0x9E3779B97F4A7C15ULL;
    size_t    i;

    for (i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;

        dst[i] = (u_char) (x >> 24);
    }
}

/* HMAC over a 1536 byte packet with its 32 byte digest field removed */
static void
ngx_media_rtmp_handshake_digest(const u_char *packet, size_t offset,
    const u_char *key, size_t key_len, u_char out[32])
{
    ngx_media_sha256_t  ctx;
    u_char              k[64], inner[32];
    size_t              i;

    memset(k, 0, sizeof(k));

    if (key_len > sizeof(k)) {
        ngx_media_sha256_init(&ctx);
        ngx_media_sha256_update(&ctx, key, key_len);
        ngx_media_sha256_final(&ctx, k);

    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }

    ngx_media_sha256_init(&ctx);

    for (i = 0; i < sizeof(k); i++) {
        k[i] ^= 0x36;
    }

    ngx_media_sha256_update(&ctx, k, sizeof(k));
    ngx_media_sha256_update(&ctx, packet, offset);
    ngx_media_sha256_update(&ctx, packet + offset + 32,
                            NGX_MEDIA_RTMP_HANDSHAKE_SIZE - offset - 32);
    ngx_media_sha256_final(&ctx, inner);

    for (i = 0; i < sizeof(k); i++) {
        k[i] ^= 0x36 ^ 0x5C;
    }

    ngx_media_sha256_init(&ctx);
    ngx_media_sha256_update(&ctx, k, sizeof(k));
    ngx_media_sha256_update(&ctx, inner, sizeof(inner));
    ngx_media_sha256_final(&ctx, out);
}

void
ngx_media_rtmp_handshake_init(ngx_media_rtmp_handshake_t *hs)
{
    memset(hs, 0, sizeof(ngx_media_rtmp_handshake_t));
}

/*
 * The client's digest field sits at one of the two offsets the specification
 * defines; the one that verifies against the flash player key tells us the
 * client used the digest handshake and where its digest lives.
 */
static ngx_uint_t
ngx_media_rtmp_handshake_client_digest(const u_char *c1)
{
    static const ngx_uint_t  bases[2] = { 8, 772 };
    u_char                   digest[32];
    ngx_uint_t               i, offset;

    for (i = 0; i < 2; i++) {
        offset = bases[i] + 4
                 + (ngx_uint_t) (c1[bases[i]] + c1[bases[i] + 1]
                                 + c1[bases[i] + 2] + c1[bases[i] + 3]) % 728;

        ngx_media_rtmp_handshake_digest(c1, offset, ngx_media_rtmp_player_key,
                                        NGX_MEDIA_RTMP_PLAYER_KEY_LEN, digest);

        if (memcmp(digest, c1 + offset, 32) == 0) {
            return offset;
        }
    }

    return 0;
}

static void
ngx_media_rtmp_handshake_reply(ngx_media_rtmp_handshake_t *hs)
{
    u_char     *s1 = hs->out + 1;
    u_char     *s2 = hs->out + 1 + NGX_MEDIA_RTMP_HANDSHAKE_SIZE;
    u_char      digest[32], key[32];
    uint64_t    seed;
    ngx_uint_t  i, client_digest;

    seed = (uint64_t) time(NULL) * 1000003ULL;

    for (i = 0; i < 4; i++) {
        seed = seed * 251ULL + hs->c1[i];
        seed = seed * 251ULL + hs->c1[NGX_MEDIA_RTMP_HANDSHAKE_SIZE - 1 - i];
    }

    hs->out[0] = NGX_MEDIA_RTMP_VERSION;

    s1[0] = s1[1] = s1[2] = s1[3] = 0;

    if (hs->complex) {
        s1[4] = 4; s1[5] = 5; s1[6] = 0; s1[7] = 1;

    } else {
        s1[4] = s1[5] = s1[6] = s1[7] = 0;
    }

    ngx_media_rtmp_random(s1 + 8, NGX_MEDIA_RTMP_HANDSHAKE_SIZE - 8, seed);

    client_digest = ngx_media_rtmp_handshake_client_digest(hs->c1);

    if (hs->complex && client_digest > 0) {
        ngx_uint_t  offset = 12
                             + (ngx_uint_t) (s1[8] + s1[9] + s1[10] + s1[11])
                                   % 728;

        /* S1 is signed with the printable part of the media server key */
        ngx_media_rtmp_handshake_digest(s1, offset, ngx_media_rtmp_fms_key,
                                        NGX_MEDIA_RTMP_SERVER_KEY_LEN, digest);
        memcpy(s1 + offset, digest, 32);

        /*
         * S2 repeats the client's random body and is signed with a key
         * derived from the client's own digest, which is what a validating
         * client recomputes.
         */
        memcpy(s2, hs->c1, NGX_MEDIA_RTMP_HANDSHAKE_SIZE - 32);

        ngx_media_hmac_sha256(ngx_media_rtmp_fms_key,
                              sizeof(ngx_media_rtmp_fms_key),
                              hs->c1 + client_digest, 32, key);

        ngx_media_hmac_sha256(key, 32, s2,
                              NGX_MEDIA_RTMP_HANDSHAKE_SIZE - 32, digest);
        memcpy(s2 + NGX_MEDIA_RTMP_HANDSHAKE_SIZE - 32, digest, 32);

    } else {
        /* the simple handshake echoes the client's packet */
        memcpy(s2, hs->c1, NGX_MEDIA_RTMP_HANDSHAKE_SIZE);
    }

    hs->out_len = 1 + 2 * NGX_MEDIA_RTMP_HANDSHAKE_SIZE;
}

ngx_int_t
ngx_media_rtmp_handshake_feed(ngx_media_rtmp_handshake_t *hs,
    const u_char *data, size_t len, size_t *consumed)
{
    size_t  need, take;

    *consumed = 0;

    /* C0: the client's version byte */
    if (hs->state == 0) {

        if (len < 1) {
            return NGX_AGAIN;
        }

        if (data[0] != NGX_MEDIA_RTMP_VERSION) {
            return NGX_ERROR;
        }

        hs->state = 1;
        hs->have = 0;
        data++;
        len--;
        *consumed += 1;
    }

    /* C1: the client's handshake packet (decides simple vs digest) */
    if (hs->state == 1) {

        take = NGX_MEDIA_RTMP_HANDSHAKE_SIZE - hs->have;

        if (take > len) {
            take = len;
        }

        memcpy(hs->c1 + hs->have, data, take);

        hs->have += take;
        data += take;
        len -= take;
        *consumed += take;

        if (hs->have < NGX_MEDIA_RTMP_HANDSHAKE_SIZE) {
            return NGX_AGAIN;
        }

        hs->complex = (hs->c1[4] || hs->c1[5] || hs->c1[6] || hs->c1[7]);

        ngx_media_rtmp_handshake_reply(hs);

        hs->state = 2;
        hs->have = 0;
    }

    /* C2: echoed handshake, not verified (librtmp echoes S1 rather than
     * digesting it, and the spec does not require a check) */
    if (hs->state == 2) {

        need = NGX_MEDIA_RTMP_HANDSHAKE_SIZE - hs->have;
        take = (need < len) ? need : len;

        if (take > 0) {
            memcpy(hs->c2 + hs->have, data, take);
        }

        hs->have += take;
        *consumed += take;

        if (hs->have < NGX_MEDIA_RTMP_HANDSHAKE_SIZE) {
            return NGX_AGAIN;
        }

        hs->state = 3;
    }

    return NGX_OK;
}

/* --- chunk reader -------------------------------------------------------- */

void
ngx_media_rtmp_reader_init(ngx_media_rtmp_reader_t *r)
{
    memset(r, 0, sizeof(ngx_media_rtmp_reader_t));

    r->chunk_size = NGX_MEDIA_RTMP_DEFAULT_CHUNK;
    r->max_message = NGX_MEDIA_RTMP_MAX_MESSAGE;
}

void
ngx_media_rtmp_reader_reset(ngx_media_rtmp_reader_t *r)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_RTMP_MAX_CSID; i++) {
        if (r->chunks[i].payload != NULL) {
            ngx_media_buf_unref(r->chunks[i].payload);
        }
    }

    ngx_media_rtmp_reader_init(r);
}

static ngx_media_rtmp_cs_t *
ngx_media_rtmp_cs_get(ngx_media_rtmp_reader_t *r, ngx_uint_t csid,
    ngx_uint_t create)
{
    ngx_uint_t            i;
    ngx_media_rtmp_cs_t  *slot = NULL;

    for (i = 0; i < NGX_MEDIA_RTMP_MAX_CSID; i++) {

        if (r->chunks[i].used && r->chunks[i].csid == csid) {
            return &r->chunks[i];
        }

        if (!r->chunks[i].used && slot == NULL) {
            slot = &r->chunks[i];
        }
    }

    if (!create || slot == NULL) {
        return NULL;
    }

    memset(slot, 0, sizeof(ngx_media_rtmp_cs_t));

    slot->used = 1;
    slot->csid = csid;

    return slot;
}

/* control messages that steer the reader itself are handled here */
static ngx_int_t
ngx_media_rtmp_reader_control(ngx_media_rtmp_reader_t *r,
    ngx_media_rtmp_cs_t *cs, ngx_media_buf_t *payload)
{
    const u_char  *p;

    if (cs->type != NGX_MEDIA_RTMP_MSG_CHUNK_SIZE
        && cs->type != NGX_MEDIA_RTMP_MSG_ABORT)
    {
        return NGX_OK;
    }

    if (payload == NULL || ngx_media_buf_size(payload) != 4) {
        r->errors++;
        return NGX_ERROR;
    }

    p = ngx_media_buf_data(payload);

    if (cs->type == NGX_MEDIA_RTMP_MSG_CHUNK_SIZE) {
        ngx_uint_t  size;

        if (cs->length != 4) {
            r->errors++;
            return NGX_ERROR;
        }

        size = ((ngx_uint_t) p[0] << 24) | ((ngx_uint_t) p[1] << 16)
               | ((ngx_uint_t) p[2] << 8) | (ngx_uint_t) p[3];

        if (size == 0 || size > NGX_MEDIA_RTMP_MAX_CHUNK) {
            r->errors++;
            return NGX_ERROR;
        }

        r->chunk_size = size;

    } else if (cs->type == NGX_MEDIA_RTMP_MSG_ABORT) {
        ngx_media_rtmp_cs_t  *target;

        if (cs->length != 4) {
            r->errors++;
            return NGX_ERROR;
        }

        target = ngx_media_rtmp_cs_get(r, ((ngx_uint_t) p[0] << 24)
                                       | ((ngx_uint_t) p[1] << 16)
                                       | ((ngx_uint_t) p[2] << 8)
                                       | (ngx_uint_t) p[3], 0);

        if (target != NULL && target->payload != NULL) {
            ngx_media_buf_unref(target->payload);
            target->payload = NULL;
            target->received = 0;
            target->length = 0;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_media_rtmp_reader_complete(ngx_media_rtmp_reader_t *r,
    ngx_media_rtmp_cs_t *cs, ngx_media_rtmp_message_pt cb, void *ctx)
{
    ngx_media_buf_t  *payload = cs->payload;
    ngx_int_t         rc;

    cs->payload = NULL;

    if (payload == NULL) {
        return NGX_OK;
    }

    (void) ngx_media_buf_freeze(payload, cs->length);

    rc = ngx_media_rtmp_reader_control(r, cs, payload);

    if (rc == NGX_OK && cb != NULL) {
        rc = cb(ctx, cs->type, cs->stream_id, cs->timestamp, payload);
    }

    ngx_media_buf_unref(payload);

    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    r->messages++;

    return NGX_OK;
}

ngx_int_t
ngx_media_rtmp_reader_feed(ngx_media_rtmp_reader_t *r, const u_char *data,
    size_t len, size_t *consumed, ngx_media_rtmp_message_pt cb, void *ctx)
{
    const u_char          *p = data;
    size_t                 left = len;
    ngx_media_rtmp_cs_t   *cs;
    ngx_uint_t             csid, fmt, header_len;
    uint32_t               ts_field;
    size_t                 take, want;
    ngx_int_t              rc;

    *consumed = 0;

    while (left > 0) {
        u_char  *header;

        if (r->pending_remaining > 0) {
            /* finishing a chunk whose header was parsed on an earlier read */
            cs = ngx_media_rtmp_cs_get(r, r->pending_csid, 0);

            if (cs == NULL) {
                r->errors++;
                return NGX_ERROR;
            }

            goto payload;
        }

        header = (u_char *) p;

        fmt = (ngx_uint_t) (p[0] >> 6);
        csid = (ngx_uint_t) (p[0] & 0x3F);
        header_len = 1;

        if (csid == 0) {
            if (left < 2) {
                break;
            }

            csid = 64 + (ngx_uint_t) p[1];
            header_len = 2;

        } else if (csid == 1) {
            if (left < 3) {
                break;
            }

            csid = 64 + (ngx_uint_t) p[1] + (ngx_uint_t) p[2] * 256;
            header_len = 3;
        }

        cs = ngx_media_rtmp_cs_get(r, csid, fmt != 3);

        if (cs == NULL) {
            r->errors++;
            return NGX_ERROR;
        }

        if (fmt == 0 || fmt == 1 || fmt == 2) {
            size_t  mh = (fmt == 0) ? 11 : ((fmt == 1) ? 7 : 3);

            if (left < header_len + mh) {
                break;
            }

            ts_field = ((uint32_t) header[header_len] << 16)
                       | ((uint32_t) header[header_len + 1] << 8)
                       | (uint32_t) header[header_len + 2];

            header_len += 3;

            if (fmt != 2) {
                /* fmt 1 also carries the message type; only fmt 2 omits it */
                cs->length = ((size_t) header[header_len] << 16)
                             | ((size_t) header[header_len + 1] << 8)
                             | (size_t) header[header_len + 2];
                cs->type = header[header_len + 3];

                header_len += 4;

                if (fmt == 0) {
                    cs->stream_id = ((ngx_uint_t) header[header_len] << 24)
                                    | ((ngx_uint_t) header[header_len + 1] << 16)
                                    | ((ngx_uint_t) header[header_len + 2] << 8)
                                    | (ngx_uint_t) header[header_len + 3];
                    header_len += 4;
                }
            }
            if (fmt != 2 && cs->payload != NULL) {
                /*
                 * A new message length on this csid replaces any payload that
                 * was partially received or left by a previous message.
                 */
                ngx_media_buf_unref(cs->payload);
                cs->payload = NULL;
            }

            cs->extended = (ts_field == 0xFFFFFF);
            cs->received = 0;
            if (cs->extended) {
                if (left < header_len + 4) {
                    break;
                }

                ts_field = ((uint32_t) header[header_len] << 24)
                           | ((uint32_t) header[header_len + 1] << 16)
                           | ((uint32_t) header[header_len + 2] << 8)
                           | (uint32_t) header[header_len + 3];
                header_len += 4;

                if (fmt == 0) {
                    cs->timestamp = ts_field;

                } else {
                    cs->timestamp += ts_field;
                }

                cs->delta = ts_field;

            } else if (fmt == 0) {
                cs->timestamp = ts_field;
                cs->delta = ts_field;

            } else {
                cs->timestamp += ts_field;
                cs->delta = ts_field;
            }

        } else {
            /* fmt 3: continue a partial message, or repeat the last header */
            if (cs->received >= cs->length || cs->payload == NULL) {
                cs->received = 0;
                cs->timestamp += cs->delta;
            }

            if (cs->extended) {
                if (left < header_len + 4) {
                    break;
                }

                header_len += 4;
            }
        }

        if (cs->length > r->max_message) {
            r->errors++;
            return NGX_ERROR;
        }

        if (cs->payload == NULL && cs->length > 0) {
            cs->payload = ngx_media_buf_alloc(cs->length);

            if (cs->payload == NULL) {
                r->errors++;
                return NGX_ERROR;
            }
        }

        want = cs->length - cs->received;

        r->pending_csid = csid;
        r->pending_remaining = (want < r->chunk_size) ? want : r->chunk_size;

        /* the header was parsed completely: account for it now */
        p += header_len;
        left -= header_len;
        *consumed += header_len;

payload:

        take = cs->length - cs->received;

        if (take > r->pending_remaining) {
            take = r->pending_remaining;
        }

        if (take > left) {
            take = left;
        }

        if (take > 0 && cs->payload != NULL) {
            memcpy(ngx_media_buf_data(cs->payload) + cs->received, p, take);
        }

        cs->received += take;
        r->pending_remaining -= take;
        p += take;
        left -= take;
        *consumed += take;

        if (cs->received >= cs->length) {
            r->pending_remaining = 0;
            r->pending_csid = 0;

            rc = ngx_media_rtmp_reader_complete(r, cs, cb, ctx);

            if (rc != NGX_OK) {
                return rc;
            }
        }
    }

    r->bytes_in += (uint64_t) *consumed;

    return NGX_OK;
}

/* --- chunk writer -------------------------------------------------------- */

void
ngx_media_rtmp_writer_init(ngx_media_rtmp_writer_t *w, ngx_uint_t chunk_size,
    ngx_uint_t max_message)
{
    memset(w, 0, sizeof(ngx_media_rtmp_writer_t));

    w->chunk_size = (chunk_size > 0 && chunk_size <= NGX_MEDIA_RTMP_MAX_CHUNK)
                        ? chunk_size
                        : NGX_MEDIA_RTMP_DEFAULT_CHUNK;
    w->max_message = max_message ? max_message : NGX_MEDIA_RTMP_MAX_MESSAGE;
}

void
ngx_media_rtmp_packet_init(ngx_media_rtmp_packet_t *pkt)
{
    memset(pkt, 0, sizeof(ngx_media_rtmp_packet_t));
}

void
ngx_media_rtmp_packet_destroy(ngx_media_rtmp_packet_t *pkt)
{
    if (pkt->payload != NULL) {
        ngx_media_buf_unref(pkt->payload);
    }

    if (pkt->head != NULL) {
        ngx_free(pkt->head);
    }

    memset(pkt, 0, sizeof(ngx_media_rtmp_packet_t));
}

/*
 * One message as a scatter/gather list: the chunk headers are built into one
 * owned allocation and interleaved with references to the shared payload, so
 * a receiver never copies media bytes (goal doc 12.1).
 */
ngx_int_t
ngx_media_rtmp_writer_message(ngx_media_rtmp_writer_t *w,
    ngx_media_rtmp_packet_t *pkt, ngx_uint_t csid, ngx_uint_t type,
    ngx_uint_t stream_id, uint32_t timestamp, ngx_media_buf_t *payload,
    size_t offset, size_t len)
{
    u_char     *p;
    size_t      chunks, head_size, i, pos, remain;
    ngx_uint_t  fmt, ts_field;
    unsigned    extended = 0;

    if (csid >= NGX_MEDIA_RTMP_MAX_OUT_CSID || len > w->max_message) {
        return NGX_ERROR;
    }

    if (len > 0 && payload == NULL) {
        return NGX_ERROR;
    }

    if (len > 0 && ngx_media_buf_size(payload) < offset + len) {
        return NGX_ERROR;
    }

    if (!w->used[csid]) {
        fmt = 0;

    } else if (timestamp >= w->timestamp[csid]
               && (uint64_t) (timestamp - w->timestamp[csid]) < 0xFFFFFF
               && w->stream_id[csid] == stream_id)
    {
        fmt = 1;

    } else {
        fmt = 0;
    }

    ts_field = (fmt == 0) ? timestamp : (timestamp - w->timestamp[csid]);

    if (ts_field >= 0xFFFFFF) {
        extended = 1;
    }

    chunks = (len + w->chunk_size - 1) / w->chunk_size;

    if (chunks == 0) {
        chunks = 1;
    }

    if (chunks * 2 > NGX_MEDIA_RTMP_MAX_OUT_PARTS) {
        return NGX_ERROR;
    }

    head_size = chunks * (1 + (extended ? 4 : 0));

    if (fmt == 0) {
        head_size += 11;

    } else if (fmt == 1) {
        head_size += 7;
    }

    pkt->head = ngx_alloc(head_size, NULL);

    if (pkt->head == NULL) {
        return NGX_ERROR;
    }

    pkt->head_len = head_size;
    pkt->nparts = 0;

    p = pkt->head;
    pos = 0;
    remain = len;

    for (i = 0; i < chunks; i++) {
        u_char     *header = p;
        size_t      take;
        ngx_uint_t  chunk_fmt = (i == 0) ? fmt : 3;

        *p++ = (u_char) ((chunk_fmt << 6) | (csid & 0x3F));

        if (chunk_fmt != 3) {
            /* a field of 0xFFFFFF means "the real value is in the
             * extended timestamp field that follows" */
            ngx_uint_t  field = extended ? 0xFFFFFF : ts_field;

            *p++ = (u_char) ((field >> 16) & 0xFF);
            *p++ = (u_char) ((field >> 8) & 0xFF);
            *p++ = (u_char) (field & 0xFF);

            if (chunk_fmt == 0) {
                *p++ = (u_char) ((len >> 16) & 0xFF);
                *p++ = (u_char) ((len >> 8) & 0xFF);
                *p++ = (u_char) (len & 0xFF);
                *p++ = (u_char) type;
                *p++ = (u_char) ((stream_id >> 24) & 0xFF);
                *p++ = (u_char) ((stream_id >> 16) & 0xFF);
                *p++ = (u_char) ((stream_id >> 8) & 0xFF);
                *p++ = (u_char) (stream_id & 0xFF);

            } else {
                *p++ = (u_char) ((len >> 16) & 0xFF);
                *p++ = (u_char) ((len >> 8) & 0xFF);
                *p++ = (u_char) (len & 0xFF);
                *p++ = (u_char) type;
            }
        }

        if (extended) {
            *p++ = (u_char) ((ts_field >> 24) & 0xFF);
            *p++ = (u_char) ((ts_field >> 16) & 0xFF);
            *p++ = (u_char) ((ts_field >> 8) & 0xFF);
            *p++ = (u_char) (ts_field & 0xFF);
        }

        pkt->parts[pkt->nparts].data = header;
        pkt->parts[pkt->nparts].len = (size_t) (p - header);
        pkt->nparts++;

        take = (remain < w->chunk_size) ? remain : w->chunk_size;

        if (take > 0) {
            pkt->parts[pkt->nparts].data = ngx_media_buf_data(payload) + offset
                                           + pos;
            pkt->parts[pkt->nparts].len = take;
            pkt->nparts++;
        }

        pos += take;
        remain -= take;
    }

    if (payload != NULL) {
        pkt->payload = ngx_media_buf_ref(payload);
    }

    w->used[csid] = 1;
    w->stream_id[csid] = stream_id;
    w->timestamp[csid] = timestamp;

    return NGX_OK;
}

/*
 * The reservation a queue makes before handing this message to the writer.
 * See the declaration in the header for why it is counted per chunk: a queue
 * that reserved one slot and one reference per message wrote past its ring as
 * soon as the writer split a large keyframe.
 *
 * The length is bounded by the writer's own message bound before the chunk
 * count is taken, so the addition below cannot wrap.
 */
ngx_int_t
ngx_media_rtmp_message_footprint(const ngx_media_rtmp_writer_t *w, size_t len,
    ngx_media_rtmp_footprint_t *out)
{
    size_t  chunks;

    if (w == NULL || out == NULL || w->chunk_size == 0
        || len > w->max_message)
    {
        /* past what writer_message() can emit: nothing to reserve either */
        return NGX_AGAIN;
    }

    out->parts = 1;
    out->refs = 0;

    if (len == 0) {
        /* one header-only chunk, and no payload slice to reference */
        return NGX_OK;
    }

    chunks = (len + w->chunk_size - 1) / w->chunk_size;

    if (chunks * 2 > NGX_MEDIA_RTMP_MAX_OUT_PARTS) {
        return NGX_AGAIN;
    }

    out->parts = (ngx_uint_t) chunks * 2;
    out->refs = (ngx_uint_t) chunks;

    return NGX_OK;
}

/* --- AMF0 ---------------------------------------------------------------- */

static void
ngx_media_amf_put_u16(u_char *p, size_t v)
{
    p[0] = (u_char) ((v >> 8) & 0xFF);
    p[1] = (u_char) (v & 0xFF);
}

static void
ngx_media_amf_put_double(u_char *p, double v)
{
    union { double d; uint64_t u; } bits;
    ngx_uint_t i;

    bits.d = v;

    for (i = 0; i < 8; i++) {
        p[i] = (u_char) ((bits.u >> ((7 - i) * 8)) & 0xFF);
    }
}

static double
ngx_media_amf_get_double(const u_char *p)
{
    union { double d; uint64_t u; } bits;
    ngx_uint_t i;

    bits.u = 0;

    for (i = 0; i < 8; i++) {
        bits.u = (bits.u << 8) | (uint64_t) p[i];
    }

    return bits.d;
}

/*
 * Reads one value.  Strings alias the input buffer; nested objects and arrays
 * inside a member are skipped (recorded as OBJECT with no data) because the
 * commands this layer must understand only carry scalars.
 */
static ngx_int_t
ngx_media_amf_read_value(const u_char *data, size_t len,
    ngx_media_amf_value_t *out, size_t *consumed, ngx_uint_t record)
{
    ngx_uint_t  type;

    if (len < 1) {
        return NGX_AGAIN;
    }

    type = data[0];
    *consumed = 1;

    switch (type) {

    case NGX_MEDIA_AMF_NUMBER:
        if (len < 9) {
            return NGX_AGAIN;
        }

        out->type = type;
        out->number = ngx_media_amf_get_double(data + 1);
        *consumed = 9;

        return NGX_OK;

    case NGX_MEDIA_AMF_BOOLEAN:
        if (len < 2) {
            return NGX_AGAIN;
        }

        out->type = type;
        out->boolean = data[1] ? 1 : 0;
        *consumed = 2;

        return NGX_OK;

    case NGX_MEDIA_AMF_STRING:
        if (len < 3) {
            return NGX_AGAIN;
        }

        {
            size_t  slen = ((size_t) data[1] << 8) | data[2];

            if (len < 3 + slen) {
                return NGX_AGAIN;
            }

            out->type = type;
            out->string.data = (u_char *) data + 3;
            out->string.len = slen;
            *consumed = 3 + slen;
        }

        return NGX_OK;

    case NGX_MEDIA_AMF_NULL:
    case NGX_MEDIA_AMF_UNDEFINED:
        out->type = type;

        return NGX_OK;

    case NGX_MEDIA_AMF_OBJECT:
    case NGX_MEDIA_AMF_ECMA_ARRAY:
    case NGX_MEDIA_AMF_STRICT_ARRAY:
        break;

    default:
        return NGX_ERROR;
    }

    out->type = (record || type != NGX_MEDIA_AMF_ECMA_ARRAY)
                    ? type
                    : NGX_MEDIA_AMF_OBJECT;

    {
        size_t  pos = 1;

        if (type == NGX_MEDIA_AMF_ECMA_ARRAY
            || type == NGX_MEDIA_AMF_STRICT_ARRAY)
        {
            if (len < 5) {
                return NGX_AGAIN;
            }

            pos = 5;
        }

        if (!record) {
            /* skip the whole aggregate without recording its members */
            ngx_uint_t  depth = 0;

            while (pos < len) {

                /* the terminator is an empty name followed by 0x09 */
                if (pos + 3 <= len && data[pos] == 0 && data[pos + 1] == 0
                    && data[pos + 2] == NGX_MEDIA_AMF_OBJECT_END)
                {
                    pos += 3;
                    break;
                }

                if (pos + 2 > len) {
                    return NGX_AGAIN;
                }
                {
                    size_t     nlen = ((size_t) data[pos] << 8) | data[pos + 1];
                    size_t     used = 0;
                    ngx_int_t  rc;

                    if (pos + 2 + nlen > len) {
                        return NGX_AGAIN;
                    }

                    pos += 2 + nlen;

                    rc = ngx_media_amf_read_value(data + pos, len - pos, out,
                                                  &used, 0);

                    if (rc != NGX_OK) {
                        return rc;
                    }

                    pos += used;
                }

                depth++;

                if (depth > 1024) {
                    return NGX_ERROR;
                }
            }

            *consumed = pos;

            return NGX_OK;
        }

        for ( ;; ) {
            size_t      nlen, used = 0;
            ngx_media_amf_value_t  member;
            ngx_int_t   rc;

            if (pos >= len) {
                return NGX_AGAIN;
            }

            if (pos + 3 <= len && data[pos] == 0 && data[pos + 1] == 0
                && data[pos + 2] == NGX_MEDIA_AMF_OBJECT_END)
            {
                pos += 3;
                break;
            }

            if (pos + 2 > len) {
                return NGX_AGAIN;
            }

            nlen = ((size_t) data[pos] << 8) | data[pos + 1];

            if (pos + 2 + nlen >= len) {
                return NGX_AGAIN;
            }

            memset(&member, 0, sizeof(member));

            rc = ngx_media_amf_read_value(data + pos + 2 + nlen,
                                          len - pos - 2 - nlen, &member, &used,
                                          0);

            if (rc != NGX_OK) {
                return rc;
            }

            if (out->count < NGX_MEDIA_AMF_MAX_MEMBERS) {
                out->members[out->count].name.data = (u_char *) data + pos + 2;
                out->members[out->count].name.len = nlen;
                out->members[out->count].type = member.type;
                out->members[out->count].number = member.number;
                out->members[out->count].boolean = member.boolean;
                out->members[out->count].string = member.string;
                out->count++;
            }

            pos += 2 + nlen + used;
        }

        *consumed = pos;
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_amf_read(const u_char *data, size_t len, ngx_media_amf_value_t *out,
    size_t *consumed)
{
    memset(out, 0, sizeof(ngx_media_amf_value_t));

    return ngx_media_amf_read_value(data, len, out, consumed, 1);
}

const ngx_str_t *
ngx_media_amf_member(const ngx_media_amf_value_t *value, const char *name)
{
    size_t      nlen = strlen(name);
    ngx_uint_t  i;

    if (value == NULL
        || (value->type != NGX_MEDIA_AMF_OBJECT
            && value->type != NGX_MEDIA_AMF_ECMA_ARRAY))
    {
        return NULL;
    }

    for (i = 0; i < value->count; i++) {

        if (value->members[i].name.len == nlen
            && memcmp(value->members[i].name.data, name, nlen) == 0)
        {
            return &value->members[i].string;
        }
    }

    return NULL;
}

ngx_int_t
ngx_media_amf_member_number(const ngx_media_amf_value_t *value,
    const char *name, double *out)
{
    size_t      nlen = strlen(name);
    ngx_uint_t  i;

    if (value == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < value->count; i++) {

        if (value->members[i].name.len == nlen
            && memcmp(value->members[i].name.data, name, nlen) == 0)
        {
            if (value->members[i].type != NGX_MEDIA_AMF_NUMBER) {
                return NGX_ERROR;
            }

            *out = value->members[i].number;

            return NGX_OK;
        }
    }

    return NGX_ERROR;
}

void
ngx_media_amf_writer_init(ngx_media_amf_writer_t *w, u_char *data,
    size_t capacity)
{
    w->data = data;
    w->capacity = capacity;
    w->len = 0;
    w->objects = 0;
}

static ngx_int_t
ngx_media_amf_room(ngx_media_amf_writer_t *w, size_t need)
{
    return (w->len + need <= w->capacity) ? NGX_OK : NGX_ERROR;
}

ngx_int_t
ngx_media_amf_put_number(ngx_media_amf_writer_t *w, double v)
{
    if (ngx_media_amf_room(w, 9) != NGX_OK) {
        return NGX_ERROR;
    }

    w->data[w->len++] = NGX_MEDIA_AMF_NUMBER;
    ngx_media_amf_put_double(w->data + w->len, v);
    w->len += 8;

    return NGX_OK;
}

ngx_int_t
ngx_media_amf_put_boolean(ngx_media_amf_writer_t *w, ngx_uint_t v)
{
    if (ngx_media_amf_room(w, 2) != NGX_OK) {
        return NGX_ERROR;
    }

    w->data[w->len++] = NGX_MEDIA_AMF_BOOLEAN;
    w->data[w->len++] = v ? 1 : 0;

    return NGX_OK;
}

ngx_int_t
ngx_media_amf_put_string(ngx_media_amf_writer_t *w, const u_char *s, size_t len)
{
    if (len > 0xFFFF || ngx_media_amf_room(w, 3 + len) != NGX_OK) {
        return NGX_ERROR;
    }

    w->data[w->len++] = NGX_MEDIA_AMF_STRING;
    ngx_media_amf_put_u16(w->data + w->len, len);
    w->len += 2;

    if (len > 0) {
        memcpy(w->data + w->len, s, len);
        w->len += len;
    }

    return NGX_OK;
}

ngx_int_t
ngx_media_amf_put_null(ngx_media_amf_writer_t *w)
{
    if (ngx_media_amf_room(w, 1) != NGX_OK) {
        return NGX_ERROR;
    }

    w->data[w->len++] = NGX_MEDIA_AMF_NULL;

    return NGX_OK;
}

ngx_int_t
ngx_media_amf_begin_object(ngx_media_amf_writer_t *w, ngx_uint_t ecma,
    ngx_uint_t hint)
{
    if (ngx_media_amf_room(w, ecma ? 5 : 1) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ecma) {
        w->data[w->len++] = NGX_MEDIA_AMF_ECMA_ARRAY;
        ngx_media_amf_put_u16(w->data + w->len, (hint >> 16) & 0xFFFF);
        ngx_media_amf_put_u16(w->data + w->len + 2, hint & 0xFFFF);
        w->len += 4;

    } else {
        w->data[w->len++] = NGX_MEDIA_AMF_OBJECT;
    }

    w->objects++;

    return NGX_OK;
}

ngx_int_t
ngx_media_amf_end_object(ngx_media_amf_writer_t *w)
{
    if (w->objects == 0 || ngx_media_amf_room(w, 3) != NGX_OK) {
        return NGX_ERROR;
    }

    w->data[w->len++] = 0;
    w->data[w->len++] = 0;
    w->data[w->len++] = NGX_MEDIA_AMF_OBJECT_END;
    w->objects--;

    return NGX_OK;
}

ngx_int_t
ngx_media_amf_put_member_string(ngx_media_amf_writer_t *w, const char *name,
    const u_char *s, size_t len)
{
    size_t  nlen = strlen(name);

    if (nlen > 0xFFFF || ngx_media_amf_room(w, 2 + nlen) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_media_amf_put_u16(w->data + w->len, nlen);
    memcpy(w->data + w->len + 2, name, nlen);
    w->len += 2 + nlen;

    return ngx_media_amf_put_string(w, s, len);
}

ngx_int_t
ngx_media_amf_put_member_number(ngx_media_amf_writer_t *w, const char *name,
    double v)
{
    size_t  nlen = strlen(name);

    if (nlen > 0xFFFF || ngx_media_amf_room(w, 2 + nlen) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_media_amf_put_u16(w->data + w->len, nlen);
    memcpy(w->data + w->len + 2, name, nlen);
    w->len += 2 + nlen;

    return ngx_media_amf_put_number(w, v);
}

ngx_int_t
ngx_media_amf_put_member_boolean(ngx_media_amf_writer_t *w, const char *name,
    ngx_uint_t v)
{
    size_t  nlen = strlen(name);

    if (nlen > 0xFFFF || ngx_media_amf_room(w, 2 + nlen) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_media_amf_put_u16(w->data + w->len, nlen);
    memcpy(w->data + w->len + 2, name, nlen);
    w->len += 2 + nlen;

    return ngx_media_amf_put_boolean(w, v);
}

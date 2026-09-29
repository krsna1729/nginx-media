#include "ngx_media_key.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

/*
 * Crockford base32: no I, L, O or U, so a key read off a screen and typed
 * into an encoder cannot become a different key.  Decoding is case- and
 * separator-insensitive on the device side; we only ever generate.
 */
static const u_char ngx_media_key_alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

void
ngx_media_key_hash(const u_char *key, size_t len, u_char *hash)
{
    if (key == NULL || hash == NULL) {
        return;
    }

    (void) SHA256(key, len, hash);
}

void
ngx_media_key_print(const u_char *hash, u_char *print)
{
    static const u_char hex[] = "0123456789abcdef";
    ngx_uint_t        i;

    if (hash == NULL || print == NULL) {
        return;
    }

    for (i = 0; i < NGX_MEDIA_KEY_PRINT_LEN / 2; i++) {
        print[i * 2] = hex[(hash[i] >> 4) & 0x0f];
        print[i * 2 + 1] = hex[hash[i] & 0x0f];
    }
}

ngx_int_t
ngx_media_key_nonce(u_char *nonce)
{
    if (nonce == NULL) {
        return NGX_ERROR;
    }

    return RAND_bytes(nonce, NGX_MEDIA_KEY_NONCE_LEN) == 1 ? NGX_OK
                                                          : NGX_ERROR;
}

ngx_int_t
ngx_media_key_derive(const u_char *master, size_t master_len,
    const ngx_str_t *id, const u_char *nonce, u_char *out, size_t cap,
    size_t *out_len, u_char *hash, u_char *print)
{
    u_char      mac[EVP_MAX_MD_SIZE];
    unsigned    mac_len = 0;
    u_char      material[4 + 256 + NGX_MEDIA_KEY_NONCE_LEN];
    size_t      material_len;
    u_char     *p;
    size_t      i;

    if (master == NULL || master_len == 0 || id == NULL || nonce == NULL
        || out == NULL || out_len == NULL || hash == NULL || print == NULL
        || cap < NGX_MEDIA_KEY_SECRET_LEN)
    {
        return NGX_ERROR;
    }

    if (id->len > 256) {
        return NGX_ERROR;
    }

    /*
     * The message is the source's id and its nonce, in that order and with
     * the id's length in front of it: two sources whose ids differ but whose
     * nonces collide still derive different keys, and an id that contains the
     * nonce's bytes cannot be confused with a different id.  On the stack:
     * the material is bounded and small, and a hash of a credential is no
     * place for an allocation that can fail.
     */
    material_len = 4 + id->len + NGX_MEDIA_KEY_NONCE_LEN;

    p = material;
    material[0] = (u_char) (id->len & 0xff);
    material[1] = (u_char) ((id->len >> 8) & 0xff);
    material[2] = (u_char) ((id->len >> 16) & 0xff);
    material[3] = (u_char) ((id->len >> 24) & 0xff);
    p = material + 4;
    ngx_memcpy(p, id->data, id->len);
    p += id->len;
    ngx_memcpy(p, nonce, NGX_MEDIA_KEY_NONCE_LEN);

    if (HMAC(EVP_sha256(), master, (int) master_len, material, material_len,
             mac, &mac_len) == NULL)
    {
        return NGX_ERROR;
    }

    p = out;

    for (i = 0; i < NGX_MEDIA_KEY_SECRET_LEN; i++) {
        /* the alphabet, indexed by the mac's bytes, wrapping: 26 characters
         * of 32 values is 130 bits, and the mac has 256 to draw on */
        *p++ = ngx_media_key_alphabet[mac[i % mac_len] & 0x1f];
    }

    *out_len = (size_t) (p - out);

    ngx_media_key_hash(out, *out_len, hash);
    ngx_media_key_print(hash, print);

    return NGX_OK;
}

#include "ngx_media_key.h"

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
ngx_media_key_issue(const ngx_str_t *application, const ngx_str_t *name,
    const ngx_str_t *id, u_char *out, size_t cap, size_t *out_len,
    u_char *hash, u_char *print)
{
    u_char      secret[NGX_MEDIA_KEY_SECRET_LEN];
    u_char     *p;
    size_t      need, i;

    if (application == NULL || name == NULL || id == NULL || out == NULL
        || out_len == NULL || hash == NULL || print == NULL
        || application->len == 0 || name->len == 0 || id->len == 0)
    {
        return NGX_ERROR;
    }

    need = application->len + 1 + name->len + 1 + id->len + 1
           + NGX_MEDIA_KEY_SECRET_LEN;

    if (need > cap) {
        return NGX_ERROR;
    }

    /*
     * The secret comes from the system's CSPRNG.  A key is a credential, and
     * a guessable one is worse than none: it would be an admission decision
     * an attacker can make.
     */
    if (RAND_bytes(secret, (int) sizeof(secret)) != 1) {
        return NGX_ERROR;
    }

    p = out;

    ngx_memcpy(p, application->data, application->len);
    p += application->len;
    *p++ = '/';

    ngx_memcpy(p, name->data, name->len);
    p += name->len;
    *p++ = '/';

    ngx_memcpy(p, id->data, id->len);
    p += id->len;
    *p++ = '-';

    for (i = 0; i < NGX_MEDIA_KEY_SECRET_LEN; i++) {
        /* 256 is not a multiple of 32, so take the low five bits */
        *p++ = ngx_media_key_alphabet[secret[i] & 0x1f];
    }

    *out_len = (size_t) (p - out);

    ngx_media_key_hash(out, *out_len, hash);
    ngx_media_key_print(hash, print);

    return NGX_OK;
}

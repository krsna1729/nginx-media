#include "ngx_media_test.h"
#include "ngx_media_key.h"

/*
 * The ingest key is a bearer credential: whoever holds it can publish as that
 * source.  What is tested here is the part that has to be right for that to
 * mean anything - the entropy source is real, the readable half names the
 * source, the hash is what is compared, and a comparison does not leak how
 * far it matched.
 */

static int
print_is_hex(const u_char *print)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_MEDIA_KEY_PRINT_LEN; i++) {
        if (!((print[i] >= '0' && print[i] <= '9')
              || (print[i] >= 'a' && print[i] <= 'f')))
        {
            return 0;
        }
    }

    return 1;
}

int
main(void)
{
    ngx_str_t   id = { sizeof("enc1") - 1, (u_char *) "enc1" };
    ngx_str_t   other_id = { sizeof("enc2") - 1, (u_char *) "enc2" };
    u_char      master[32], nonce[NGX_MEDIA_KEY_NONCE_LEN];
    u_char      nonce2[NGX_MEDIA_KEY_NONCE_LEN];
    u_char      key[NGX_MEDIA_KEY_MAX], again[NGX_MEDIA_KEY_MAX];
    u_char      hash[NGX_MEDIA_KEY_HASH_LEN], hash2[NGX_MEDIA_KEY_HASH_LEN];
    u_char      print[NGX_MEDIA_KEY_PRINT_LEN], print2[NGX_MEDIA_KEY_PRINT_LEN];
    size_t      len = 0, len2 = 0;
    ngx_uint_t  i;

    memset(master, 0x5a, sizeof(master));

    TEST_CASE("nonce: two are different, and none is all zero");
    TEST_ASSERT_EQ_INT(ngx_media_key_nonce(nonce), NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_key_nonce(nonce2), NGX_OK);
    TEST_ASSERT(memcmp(nonce, nonce2, sizeof(nonce)) != 0);

    TEST_CASE("derive: the same source and nonce derive the same key");
    TEST_ASSERT_EQ_INT(ngx_media_key_derive(master, sizeof(master), &id, nonce,
                                            key, sizeof(key), &len, hash,
                                            print),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(len, NGX_MEDIA_KEY_SECRET_LEN);
    TEST_ASSERT_EQ_INT(ngx_media_key_derive(master, sizeof(master), &id, nonce,
                                            again, sizeof(again), &len2,
                                            hash2, print2),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(len, len2);
    TEST_ASSERT(memcmp(key, again, len) == 0);
    TEST_ASSERT(memcmp(hash, hash2, sizeof(hash)) == 0);

    TEST_CASE("derive: a new nonce, another source or another secret differ");
    TEST_ASSERT_EQ_INT(ngx_media_key_derive(master, sizeof(master), &id,
                                            nonce2, again, sizeof(again),
                                            &len2, hash2, print2),
                       NGX_OK);
    TEST_ASSERT(memcmp(key, again, len) != 0);

    TEST_ASSERT_EQ_INT(ngx_media_key_derive(master, sizeof(master), &other_id,
                                            nonce, again, sizeof(again),
                                            &len2, hash2, print2),
                       NGX_OK);
    TEST_ASSERT(memcmp(key, again, len) != 0);

    master[0] ^= 0xff;
    TEST_ASSERT_EQ_INT(ngx_media_key_derive(master, sizeof(master), &id, nonce,
                                            again, sizeof(again), &len2,
                                            hash2, print2),
                       NGX_OK);
    TEST_ASSERT(memcmp(key, again, len) != 0);
    master[0] ^= 0xff;

    TEST_CASE("the key is path- and name-safe, and has no lookalikes");
    TEST_ASSERT(memchr(key, '/', len) == NULL);
    TEST_ASSERT(memchr(key, '-', len) == NULL);

    for (i = 0; i < NGX_MEDIA_KEY_SECRET_LEN; i++) {
        u_char c = key[i];

        if (c >= '0' && c <= '9') {
            continue;
        }
        TEST_ASSERT(c >= 'A' && c <= 'Z');
        TEST_ASSERT(c != 'I' && c != 'L' && c != 'O' && c != 'U');
    }

    TEST_CASE("print: the fingerprint is the hash, not the key");
    ngx_media_key_print(hash, print);
    ngx_media_key_print(hash2, print2);
    TEST_ASSERT(print_is_hex(print) && print_is_hex(print2));
    TEST_ASSERT(memcmp(print, print2, NGX_MEDIA_KEY_PRINT_LEN) != 0);

    TEST_CASE("derive: a key that does not fit is refused, not truncated");
    TEST_ASSERT_EQ_INT(ngx_media_key_derive(master, sizeof(master), &id, nonce,
                                            key, 8, &len, hash, print),
                       NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_key_derive(NULL, 0, &id, nonce, key,
                                            sizeof(key), &len, hash, print),
                       NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_key_derive(master, sizeof(master), NULL, nonce,
                                            key, sizeof(key), &len, hash,
                                            print),
                       NGX_ERROR);

    return 0;
}


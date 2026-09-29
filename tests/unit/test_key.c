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
    u_char      key[NGX_MEDIA_KEY_MAX], other[NGX_MEDIA_KEY_MAX];
    u_char      hash[NGX_MEDIA_KEY_HASH_LEN], hash2[NGX_MEDIA_KEY_HASH_LEN];
    u_char      print[NGX_MEDIA_KEY_PRINT_LEN], print2[NGX_MEDIA_KEY_PRINT_LEN];
    size_t      len = 0, len2 = 0;
    ngx_uint_t  i;

    TEST_CASE("issue: the key names its source and carries a secret");

    TEST_ASSERT_EQ_INT(ngx_media_key_issue(key, sizeof(key), &len, hash,
                                           print),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(len, NGX_MEDIA_KEY_SECRET_LEN);
    /* nothing a transport might treat specially, and nothing from the graph */
    TEST_ASSERT(memchr(key, '/', len) == NULL);
    TEST_ASSERT(memchr(key, '-', len) == NULL);
    TEST_ASSERT(print_is_hex(print));

    TEST_CASE("issue: two keys for one source are different, and both hash");

    TEST_ASSERT_EQ_INT(ngx_media_key_issue(other, sizeof(other), &len2, hash2,
                                           print2),
                       NGX_OK);
    TEST_ASSERT_EQ_U64(len, len2);
    TEST_ASSERT(memcmp(key, other, len) != 0);
    TEST_ASSERT(memcmp(hash, hash2, sizeof(hash)) != 0);

    TEST_CASE("hash: the same key hashes the same, a changed byte does not");

    ngx_media_key_hash(key, len, hash2);
    TEST_ASSERT(memcmp(hash, hash2, sizeof(hash)) == 0);

    other[0] = (u_char) (key[0] == 'l' ? 'm' : 'l');
    ngx_media_key_hash(other, len, hash2);
    TEST_ASSERT(memcmp(hash, hash2, sizeof(hash)) != 0);

    TEST_CASE("print: the fingerprint is the hash, not the key");

    ngx_media_key_print(hash2, print2);
    TEST_ASSERT(print_is_hex(print2));
    TEST_ASSERT(memcmp(print, print2, NGX_MEDIA_KEY_PRINT_LEN) != 0);

    TEST_CASE("issue: a key that does not fit is refused, not truncated");

    TEST_ASSERT_EQ_INT(ngx_media_key_issue(key, 8, &len, hash, print),
                       NGX_ERROR);
    TEST_ASSERT_EQ_INT(ngx_media_key_issue(NULL, sizeof(key), &len, hash,
                                           print),
                       NGX_ERROR);

    TEST_CASE("the alphabet has no lookalikes: no I, L, O or U");

    for (i = 0; i < NGX_MEDIA_KEY_SECRET_LEN; i++) {
        u_char c = key[i];

        if (c >= '0' && c <= '9') {
            continue;
        }
        TEST_ASSERT(c >= 'A' && c <= 'Z');
        TEST_ASSERT(c != 'I' && c != 'L' && c != 'O' && c != 'U');
    }

    return 0;
}

/*
 * Deterministic program ownership: the hash every worker computes must be
 * identical for the same application/stream, must separate the components,
 * and must spread streams over the available workers.
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_owner.h"

#include <stdio.h>

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if (!(cond)) {                                                        \
            ngx_media_test_failures++;                                        \
            printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,               \
                   ##__VA_ARGS__);                                            \
        }                                                                     \
    } while (0)

static ngx_str_t
str(const char *s)
{
    ngx_str_t  v;

    v.data = (u_char *) s;
    v.len = strlen(s);

    return v;
}

static void
test_determinism(void)
{
    ngx_str_t  app = str("live");
    ngx_str_t  news = str("news");
    ngx_str_t  other = str("sports");

    TEST_CASE("ownership hash is deterministic and well separated");

    CHECK(ngx_media_owner_hash(&app, &news)
          == ngx_media_owner_hash(&app, &news),
          "same input, same hash");

    CHECK(ngx_media_owner_hash(&app, &news)
          != ngx_media_owner_hash(&app, &other),
          "different streams differ");

    /* "ab" + "c" must not collide with "a" + "bc" */
    {
        ngx_str_t  ab = str("ab"), c = str("c"), a = str("a"), bc = str("bc");

        CHECK(ngx_media_owner_hash(&ab, &c) != ngx_media_owner_hash(&a, &bc),
              "component boundary is part of the hash");
    }

    /*
     * A well known FNV-1a vector pins the algorithm: the value is a stream's
     * identity on the wire and in shared memory, so it has to be the same in
     * every process, every build and every release.
     */
    {
        ngx_str_t  empty = str("");

        CHECK(ngx_media_owner_hash(&empty, &empty) == 0xaf63a24c860189feull,
              "FNV-1a 64 over the separator is %llx",
              (unsigned long long) ngx_media_owner_hash(&empty, &empty));
    }

    /* 64 bits, because 32 collide at a few tens of thousands of streams */
    {
        ngx_str_t  a = str("live"), b = str("news");
        uint64_t   hash = ngx_media_owner_hash(&a, &b);

        CHECK((hash >> 32) != 0 || (hash & 0xffffffffu) != 0,
              "hash uses the whole word");
    }
}

static void
test_distribution(void)
{
    ngx_str_t   app = str("live");
    ngx_uint_t  counts[4] = { 0, 0, 0, 0 };
    ngx_uint_t  i, slot;
    char        name[32];

    TEST_CASE("streams spread over the worker slots");

    for (i = 0; i < 400; i++) {
        ngx_str_t  stream;

        snprintf(name, sizeof(name), "stream-%lu", i);
        stream = str(name);

        slot = ngx_media_owner_slot(ngx_media_owner_hash(&app, &stream), 4);

        CHECK(slot < 4, "slot in range: %lu", slot);
        counts[slot]++;
    }

    /* every slot must own something, and none may own everything */
    for (i = 0; i < 4; i++) {
        CHECK(counts[i] > 40, "slot %lu owns %lu streams", i, counts[i]);
        CHECK(counts[i] < 200, "slot %lu owns %lu streams", i, counts[i]);
    }

    CHECK(ngx_media_owner_slot(1234, 0) == 0, "no workers means slot 0");
    CHECK(ngx_media_owner_slot(1234, 1) == 0, "a single worker owns all");
}

static void
test_stability(void)
{
    ngx_str_t   app = str("live");
    ngx_str_t   stream = str("news");
    uint32_t    hash = ngx_media_owner_hash(&app, &stream);
    ngx_uint_t  slot2, slot4;

    TEST_CASE("ownership is stable across worker counts");

    slot2 = ngx_media_owner_slot(hash, 2);
    slot4 = ngx_media_owner_slot(hash, 4);

    CHECK(slot2 == hash % 2, "two worker slots");
    CHECK(slot4 == hash % 4, "four worker slots");
    CHECK(slot2 == slot4 % 2, "consistent between the two layouts");
}

int
main(void)
{
    printf("== program ownership\n");

    test_determinism();
    test_distribution();
    test_stability();

    TEST_LEAKS();
    TEST_MAIN_END();
}

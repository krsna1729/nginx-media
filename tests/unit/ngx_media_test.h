#ifndef NGX_MEDIA_TEST_H
#define NGX_MEDIA_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_shim.h"

static int ngx_media_test_failures;
static int ngx_media_test_checks;

#define TEST_CASE(name)                                                       \
    do {                                                                      \
        printf("  case: %s\n", (name));                                       \
    } while (0)

#define TEST_FAIL(fmt, ...)                                                   \
    do {                                                                      \
        ngx_media_test_failures++;                                            \
        printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);     \
    } while (0)

#define TEST_ASSERT(cond)                                                     \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if (!(cond)) {                                                        \
            TEST_FAIL("assertion failed: %s", #cond);                         \
        }                                                                     \
    } while (0)

#define TEST_ASSERT_EQ_U64(actual, expected)                                  \
    do {                                                                      \
        unsigned long long a_ = (unsigned long long) (actual);                \
        unsigned long long e_ = (unsigned long long) (expected);              \
        ngx_media_test_checks++;                                              \
        if (a_ != e_) {                                                       \
            TEST_FAIL("%s: expected %llu, got %llu", #actual, e_, a_);        \
        }                                                                     \
    } while (0)

#define TEST_ASSERT_EQ_I64(actual, expected)                                  \
    do {                                                                      \
        long long a_ = (long long) (actual);                                  \
        long long e_ = (long long) (expected);                                \
        ngx_media_test_checks++;                                              \
        if (a_ != e_) {                                                       \
            TEST_FAIL("%s: expected %lld, got %lld", #actual, e_, a_);        \
        }                                                                     \
    } while (0)

#define TEST_ASSERT_EQ_INT(actual, expected)                                  \
    do {                                                                      \
        long long a_ = (long long) (actual);                                  \
        long long e_ = (long long) (expected);                                \
        ngx_media_test_checks++;                                              \
        if (a_ != e_) {                                                       \
            TEST_FAIL("%s: expected %lld, got %lld", #actual, e_, a_);        \
        }                                                                     \
    } while (0)

#define TEST_ASSERT_NOT_NULL(ptr)                                             \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if ((ptr) == NULL) {                                                  \
            TEST_FAIL("%s: expected non-NULL", #ptr);                         \
        }                                                                     \
    } while (0)

#define TEST_ASSERT_NULL(ptr)                                                 \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if ((ptr) != NULL) {                                                  \
            TEST_FAIL("%s: expected NULL", #ptr);                             \
        }                                                                     \
    } while (0)

/* every shim allocation must have been returned */
#define TEST_LEAKS()                                                          \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if (ngx_media_test_allocs != ngx_media_test_frees) {                  \
            TEST_FAIL("leak: %zu allocations, %zu frees",                     \
                      ngx_media_test_allocs, ngx_media_test_frees);           \
        }                                                                     \
    } while (0)

#define TEST_MAIN_END()                                                       \
    do {                                                                      \
        printf("%s: %d checks, %d failures\n", __FILE__,                      \
               ngx_media_test_checks, ngx_media_test_failures);               \
        return (ngx_media_test_failures == 0) ? 0 : 1;                        \
    } while (0)

#endif /* NGX_MEDIA_TEST_H */

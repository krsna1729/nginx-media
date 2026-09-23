#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_destination.h"

#include <arpa/inet.h>

static void
test_destination_strdup_terminates_host(void)
{
    static const u_char  address[] = {
        '1', '2', '7', '.', '0', '.', '0', '.', '1'
    };

    ngx_pool_t  *pool;
    ngx_str_t    source;
    ngx_str_t   *copy;
    struct in_addr  parsed;

    TEST_CASE("destination string copy is NUL-terminated for IPv4 parsers");

    pool = ngx_create_pool(4096, NULL);
    TEST_ASSERT_NOT_NULL(pool);
    if (pool == NULL) {
        return;
    }

    source.data = (u_char *) address;
    source.len = sizeof(address);
    copy = ngx_media_destination_strdup(pool, &source);

    TEST_ASSERT_NOT_NULL(copy);
    if (copy != NULL) {
        TEST_ASSERT_EQ_U64(copy->len, sizeof(address));
        TEST_ASSERT(copy->data[copy->len] == '\0');
        TEST_ASSERT_EQ_INT(inet_pton(AF_INET, (const char *) copy->data,
                                     &parsed), 1);
    }

    ngx_destroy_pool(pool);
}

int
main(void)
{
    test_destination_strdup_terminates_host();
    TEST_LEAKS();
    TEST_MAIN_END();
}

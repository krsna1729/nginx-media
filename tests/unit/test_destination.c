#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_destination.h"
#include "ngx_media_stream.h"

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

static void
test_destination_without_backend(void)
{
    ngx_pool_t               *pool;
    ngx_media_stream_t        stream;
    ngx_media_destination_t  *destination;
    ngx_media_feed_conf_t     feed_conf = { .max_units = 4 };
    ngx_str_t                 application = {
        .len = 4, .data = (u_char *) "live"
    };
    ngx_str_t                 name = { .len = 4, .data = (u_char *) "news" };
    ngx_str_t                 id = { .len = 4, .data = (u_char *) "edge" };
    ngx_int_t                 rc;

    TEST_CASE("a fresh destination cannot start without its transport backend");

    pool = ngx_create_pool(4096, NULL);
    TEST_ASSERT_NOT_NULL(pool);
    if (pool == NULL) {
        return;
    }

    rc = ngx_media_stream_init(&stream, pool, NULL, &application, &name,
                               &feed_conf);
    TEST_ASSERT_EQ_INT(rc, NGX_OK);
    if (rc != NGX_OK) {
        ngx_destroy_pool(pool);
        return;
    }

    destination = ngx_media_destination_add(&stream, &id, NGX_MEDIA_DEST_SRT,
                                            NULL);
    TEST_ASSERT_NOT_NULL(destination);
    if (destination != NULL) {
        TEST_ASSERT_EQ_INT(ngx_media_destination_start(&stream, destination,
                                                       NULL), NGX_ERROR);
        ngx_media_destination_remove(&stream, destination);
        TEST_ASSERT_NULL(ngx_media_destination_find(&stream, &id));
    }

    ngx_media_stream_destroy(&stream);
    ngx_destroy_pool(pool);
}

int
main(void)
{
    test_destination_strdup_terminates_host();
    test_destination_without_backend();
    TEST_LEAKS();
    TEST_MAIN_END();
}

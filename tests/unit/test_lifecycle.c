/*
 * Lifecycle resilience: a concurrent component must start and stop cleanly
 * every time, release everything it owns, and tolerate a stop with work still
 * queued or a repeated stop.  Run under ASan/UBSan this is the cheap version
 * of the audit that found the shutdown hangs and double frees.
 */

#define _DEFAULT_SOURCE 1

#include "ngx_media_test.h"

#include "ngx_media_record.h"

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        ngx_media_test_checks++;                                              \
        if (!(cond)) {                                                        \
            ngx_media_test_failures++;                                        \
            printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,               \
                   ##__VA_ARGS__);                                            \
        }                                                                     \
    } while (0)

static ngx_media_buf_t *
payload(size_t len)
{
    ngx_media_buf_t  *buf = ngx_media_buf_alloc(len);

    if (buf == NULL) {
        return NULL;
    }

    memset(ngx_media_buf_data(buf), 0x47, len);
    (void) ngx_media_buf_freeze(buf, len);

    return buf;
}

static void
test_repeated_cycles(void)
{
    ngx_media_record_conf_t  conf;
    ngx_media_record_t       rec;
    ngx_media_buf_t         *buf;
    ngx_uint_t               cycle, i;

    TEST_CASE("a writer thread survives repeated start and stop cycles");

    ngx_media_record_conf_default(&conf);

    conf.path.data = (u_char *) ".build/lifecycle/part.ts";
    conf.path.len = sizeof(".build/lifecycle/part.ts") - 1;
    conf.tap = NGX_MEDIA_RECORD_PROGRAM;

    for (cycle = 0; cycle < 10; cycle++) {

        CHECK(ngx_media_record_init(&rec, &conf, NULL) == NGX_OK,
              "cycle %lu: writer started", cycle);

        /* queue work and stop immediately: the writer must drain and exit */
        for (i = 0; i < 32; i++) {
            buf = payload(1024);

            CHECK(buf != NULL, "payload allocated");
            (void) ngx_media_record_append(&rec, buf, 0, 1024);
            ngx_media_buf_unref(buf);
        }

        ngx_media_record_stop(&rec);
        CHECK(rec.fd == -1, "cycle %lu: file closed", cycle);
    }
}

static void
test_stop_is_idempotent(void)
{
    ngx_media_record_conf_t  conf;
    ngx_media_record_t       rec;
    ngx_media_buf_t         *buf;

    TEST_CASE("stop is safe to repeat and safe with an empty queue");

    ngx_media_record_conf_default(&conf);

    conf.path.data = (u_char *) ".build/lifecycle/idem.ts";
    conf.path.len = sizeof(".build/lifecycle/idem.ts") - 1;
    conf.tap = NGX_MEDIA_RECORD_RAW;

    CHECK(ngx_media_record_init(&rec, &conf, NULL) == NGX_OK,
          "writer started");

    /* nothing was ever queued */
    ngx_media_record_stop(&rec);
    ngx_media_record_stop(&rec);

    CHECK(rec.fd == -1, "still closed after a repeated stop");

    /* a stop with work in flight, then a repeated stop */
    CHECK(ngx_media_record_init(&rec, &conf, NULL) == NGX_OK,
          "writer restarted");

    buf = payload(4096);
    CHECK(buf != NULL, "payload allocated");
    (void) ngx_media_record_append(&rec, buf, 0, 4096);
    ngx_media_buf_unref(buf);

    ngx_media_record_stop(&rec);
    ngx_media_record_stop(&rec);

    CHECK(rec.fd == -1, "closed after a stop with work in flight");
}

static void
test_append_after_stop(void)
{
    ngx_media_record_conf_t  conf;
    ngx_media_record_t       rec;
    ngx_media_buf_t         *buf;
    ngx_int_t                rc;

    TEST_CASE("a stopped writer refuses work instead of writing into a "
              "closed file");

    ngx_media_record_conf_default(&conf);

    conf.path.data = (u_char *) ".build/lifecycle/after.ts";
    conf.path.len = sizeof(".build/lifecycle/after.ts") - 1;
    conf.tap = NGX_MEDIA_RECORD_PROGRAM;

    CHECK(ngx_media_record_init(&rec, &conf, NULL) == NGX_OK,
          "writer started");
    ngx_media_record_stop(&rec);

    buf = payload(256);
    CHECK(buf != NULL, "payload allocated");

    rc = ngx_media_record_append(&rec, buf, 0, 256);

    CHECK(rc != NGX_OK, "append after stop is refused: %ld", (long) rc);

    ngx_media_buf_unref(buf);

    /* and the component can still be reused afterwards */
    CHECK(ngx_media_record_init(&rec, &conf, NULL) == NGX_OK,
          "writer restarted after a refused append");
    ngx_media_record_stop(&rec);
}

int
main(void)
{
    printf("== lifecycle resilience\n");

    (void) mkdir(".build/lifecycle", 0755);

    test_repeated_cycles();
    test_stop_is_idempotent();
    test_append_after_stop();

    TEST_LEAKS();
    TEST_MAIN_END();
}

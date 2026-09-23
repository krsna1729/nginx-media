#define _POSIX_C_SOURCE 200809L
#include "ngx_media_test.h"
#include <signal.h>
#include "ngx_media_executor.h"

#include <time.h>

static ngx_str_t
executor_path(const char *text)
{
    ngx_str_t value;

    value.data = (u_char *) text;
    value.len = strlen(text);

    return value;
}

static void
executor_wait_briefly(void)
{
    struct timespec delay;

    delay.tv_sec = 0;
    delay.tv_nsec = 2 * 1000 * 1000;
    (void) nanosleep(&delay, NULL);
}

typedef struct {
    ngx_uint_t started;
    ngx_uint_t failed;
    ngx_uint_t stopped;
    ngx_uint_t epochs;
    size_t     output_bytes;
} executor_test_state_t;

static void
executor_output(void *ctx, const u_char *data, size_t len, uint64_t epoch)
{
    executor_test_state_t *state = ctx;

    (void) data;
    (void) epoch;
    state->output_bytes += len;
}

static void
executor_event(void *ctx, const ngx_media_executor_event_t *event)
{
    executor_test_state_t *state = ctx;

    switch (event->type) {
    case NGX_MEDIA_EXECUTOR_EVENT_STARTED:
    case NGX_MEDIA_EXECUTOR_EVENT_RESTARTED:
        state->started++;
        break;
    case NGX_MEDIA_EXECUTOR_EVENT_FAILED:
        state->failed++;
        break;
    case NGX_MEDIA_EXECUTOR_EVENT_STOPPED:
        state->stopped++;
        break;
    case NGX_MEDIA_EXECUTOR_EVENT_EPOCH:
        state->epochs++;
        break;
    default:
        break;
    }
}

int
main(void)
{
    ngx_media_executor_conf_t conf;
    (void) signal(SIGPIPE, SIG_IGN);
    ngx_media_executor_t       executor;
    ngx_media_executor_event_t event;
    ngx_media_buf_t           *payload;
    executor_test_state_t      state;
    ngx_msec_t                 now;
    ngx_uint_t                 i;

    now = 1;
    ngx_memzero(&state, sizeof(state));
    ngx_media_executor_conf_default(&conf);
    conf.executable = executor_path("/bin/false");
    conf.restart_min = 1;
    conf.restart_max = 2;

    TEST_CASE("executor rejects queueing before launch");
    TEST_ASSERT_EQ_INT(ngx_media_executor_init(&executor, &conf,
                                               executor_output, executor_event,
                                               &state, NULL), NGX_OK);
    payload = ngx_media_buf_alloc(8);
    TEST_ASSERT_NOT_NULL(payload);
    (void) ngx_media_buf_freeze(payload, 8);
    TEST_ASSERT_EQ_INT(ngx_media_executor_feed(&executor, payload, 0, 8),
                       NGX_AGAIN);

    TEST_CASE("child exit is reaped and restart is bounded");
    TEST_ASSERT_EQ_INT(ngx_media_executor_start(&executor, 0, NULL), NGX_OK);
    TEST_ASSERT_EQ_INT(ngx_media_executor_feed(&executor, payload, 0, 8),
                       NGX_OK);

    for (i = 0; i < 20 && state.failed == 0; i++) {
        executor_wait_briefly();
        ngx_media_executor_tick(&executor, now++, NULL);
    }

    TEST_ASSERT(state.failed > 0);
    TEST_ASSERT_EQ_INT(ngx_media_executor_state(&executor),
                       NGX_MEDIA_EXECUTOR_RESTARTING);
    TEST_ASSERT_EQ_U64(ngx_media_executor_pending(&executor), 0);
    TEST_ASSERT(state.epochs > 0);

    TEST_CASE("stop closes descriptors and emits lifecycle event");
    ngx_media_executor_stop(&executor, NULL);
    TEST_ASSERT_EQ_INT(ngx_media_executor_state(&executor),
                       NGX_MEDIA_EXECUTOR_STOPPED);
    TEST_ASSERT(state.stopped > 0);
    TEST_ASSERT_EQ_U64(ngx_media_executor_pending_bytes(&executor), 0);

    ngx_media_buf_unref(payload);
    ngx_memzero(&event, sizeof(event));
    TEST_LEAKS();
    TEST_MAIN_END();
}

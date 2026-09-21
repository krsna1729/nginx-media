#include "ngx_media_test.h"
#include "ngx_media_health.h"

static void
set_selector(ngx_media_selector_t *selector, ngx_msec_t failure,
    ngx_msec_t recovery)
{
    ngx_memzero(selector, sizeof(ngx_media_selector_t));

    selector->failure_timeout = failure;
    selector->recovery_timeout = recovery;
}

int
main(void)
{
    ngx_media_health_t    health;
    ngx_media_selector_t  selector;

    TEST_CASE("a fresh source is assumed usable");
    set_selector(&selector, 1000, 500);
    ngx_media_health_init(&health, &selector, 0);
    TEST_ASSERT_EQ_U64(health.healthy, 1);
    TEST_ASSERT_EQ_U64(health.eligible, 1);
    /* the container is assumed valid until an error is observed */
    TEST_ASSERT_EQ_U64(health.evidence, NGX_MEDIA_HEALTH_CONTAINER_VALID);

    TEST_CASE("a silent source fails after failure_timeout");
    ngx_media_health_transport(&health, 1, 100);
    ngx_media_health_media(&health, 90000, 150);
    ngx_media_health_container(&health, 0, 150);
    ngx_media_health_evaluate(&health, 150);
    TEST_ASSERT_EQ_U64(health.healthy, 1);

    ngx_media_health_evaluate(&health, 1100);
    TEST_ASSERT_EQ_U64(health.healthy, 1);      /* 950 ms of silence: not yet */

    ngx_media_health_evaluate(&health, 1200);
    TEST_ASSERT_EQ_U64(health.healthy, 0);      /* 1050 ms of silence: failed */
    TEST_ASSERT_EQ_U64(health.eligible, 0);
    TEST_ASSERT_EQ_U64(health.transitions, 1);

    TEST_CASE("recovery needs recovery_timeout of good evidence");
    ngx_media_health_media(&health, 180000, 1400);
    ngx_media_health_container(&health, 0, 1400);
    ngx_media_health_evaluate(&health, 1400);
    TEST_ASSERT_EQ_U64(health.healthy, 0);      /* hysteresis */

    ngx_media_health_media(&health, 270000, 1800);
    ngx_media_health_evaluate(&health, 1950);
    TEST_ASSERT_EQ_U64(health.healthy, 1);
    TEST_ASSERT_EQ_U64(health.transitions, 2);

    TEST_CASE("frozen timestamps fail a connected source");
    set_selector(&selector, 1000, 500);
    ngx_media_health_init(&health, &selector, 0);
    ngx_media_health_transport(&health, 1, 0);
    ngx_media_health_media(&health, 90000, 0);
    ngx_media_health_container(&health, 0, 0);
    ngx_media_health_evaluate(&health, 100);
    TEST_ASSERT_EQ_U64(health.healthy, 1);

    /* media keeps arriving, but the timestamps never advance */
    ngx_media_health_media(&health, 90000, 200);
    ngx_media_health_media(&health, 90000, 700);
    ngx_media_health_evaluate(&health, 800);
    TEST_ASSERT_EQ_U64(health.healthy, 1);

    ngx_media_health_media(&health, 90000, 1600);
    ngx_media_health_evaluate(&health, 1700);
    TEST_ASSERT_EQ_U64(health.healthy, 0);

    TEST_CASE("a closed transport fails immediately");
    set_selector(&selector, 5000, 500);
    ngx_media_health_init(&health, &selector, 0);
    ngx_media_health_transport(&health, 1, 0);
    ngx_media_health_media(&health, 1, 0);
    ngx_media_health_evaluate(&health, 10);
    TEST_ASSERT_EQ_U64(health.healthy, 1);

    ngx_media_health_transport(&health, 0, 20);
    TEST_ASSERT_EQ_U64(health.healthy, 0);
    TEST_ASSERT_EQ_U64(health.eligible, 0);
    TEST_ASSERT_EQ_U64(health.evidence, 0);

    TEST_CASE("container errors clear validation until they stop");
    set_selector(&selector, 1000, 400);
    ngx_media_health_init(&health, &selector, 0);
    ngx_media_health_transport(&health, 1, 0);
    ngx_media_health_media(&health, 100, 0);
    ngx_media_health_container(&health, 0, 0);
    ngx_media_health_evaluate(&health, 500);
    TEST_ASSERT(health.evidence & NGX_MEDIA_HEALTH_CONTAINER_VALID);

    ngx_media_health_container(&health, 3, 600);
    TEST_ASSERT(!(health.evidence & NGX_MEDIA_HEALTH_CONTAINER_VALID));

    ngx_media_health_media(&health, 200, 600);
    ngx_media_health_evaluate(&health, 700);
    TEST_ASSERT(!(health.evidence & NGX_MEDIA_HEALTH_CONTAINER_VALID));

    ngx_media_health_evaluate(&health, 1000);
    TEST_ASSERT(health.evidence & NGX_MEDIA_HEALTH_CONTAINER_VALID);

    TEST_CASE("the compatibility bit is reported, not required");
    set_selector(&selector, 1000, 500);
    ngx_media_health_init(&health, &selector, 0);
    ngx_media_health_transport(&health, 1, 0);
    ngx_media_health_media(&health, 10, 0);
    ngx_media_health_container(&health, 0, 0);
    ngx_media_health_tracks(&health, 0);
    ngx_media_health_evaluate(&health, 10);
    TEST_ASSERT_EQ_U64(health.healthy, 1);
    TEST_ASSERT(!(health.evidence & NGX_MEDIA_HEALTH_TRACKS_COMPATIBLE));

    ngx_media_health_tracks(&health, 1);
    TEST_ASSERT(health.evidence & NGX_MEDIA_HEALTH_TRACKS_COMPATIBLE);

    TEST_CASE("NULL tolerance");
    ngx_media_health_init(NULL, &selector, 0);
    ngx_media_health_transport(NULL, 1, 0);
    ngx_media_health_media(NULL, 0, 0);
    ngx_media_health_container(NULL, 0, 0);
    ngx_media_health_tracks(NULL, 1);
    ngx_media_health_evaluate(NULL, 0);

    TEST_LEAKS();

    TEST_MAIN_END();
}

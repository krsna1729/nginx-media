/*
 * Deleted identities: what the replication guard remembers, and for how long.
 *
 * The ring is what stops a graph operation that was in flight when a stream
 * was deleted from creating it again on a replica.  These cases pin the two
 * halves that decide it - an operation the deletion has moved past is refused,
 * a newer one is applied - and the bound, because the ring is fixed and a
 * deletion that falls out of it is a deletion the worker no longer remembers.
 */

#include "ngx_media_test.h"
#include "ngx_media_tombstone.h"

#define TOMBSTONE_HASH_A  0x1111u
#define TOMBSTONE_HASH_B  0x2222u

static void
test_deletion_refuses_what_it_moved_past(void)
{
    ngx_media_tombstone_ring_t  ring;

    TEST_CASE("a deletion refuses the operations it has moved past");

    ngx_media_tombstone_init(&ring);

    /* nothing recorded: every operation is new */
    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_A, 1), 0);

    ngx_media_tombstone_add(&ring, TOMBSTONE_HASH_A, 10);

    /* older, and the same revision: the deletion already moved past both */
    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_A, 9), 1);
    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_A, 10), 1);

    /* a create that follows the delete is applied */
    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_A, 11), 0);

    /* another identity is untouched by it */
    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_B, 1), 0);
}

static void
test_deleted_twice(void)
{
    ngx_media_tombstone_ring_t  ring;

    TEST_CASE("an identity deleted twice is refused up to the newer deletion");

    ngx_media_tombstone_init(&ring);

    ngx_media_tombstone_add(&ring, TOMBSTONE_HASH_A, 10);
    ngx_media_tombstone_add(&ring, TOMBSTONE_HASH_A, 40);

    /*
     * An operation between the two deletions is refused: it is newer than the
     * first but not than the second, and the object it would recreate was
     * removed after it was issued.
     */
    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_A, 20), 1);
    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_A, 41), 0);
}

static void
test_the_window_is_bounded(void)
{
    ngx_media_tombstone_ring_t  ring;
    ngx_uint_t                  i;

    TEST_CASE("the window is bounded, and the oldest deletion falls out");

    ngx_media_tombstone_init(&ring);

    /* the oldest slot holds the deletion under test; the rest are filler */
    ngx_media_tombstone_add(&ring, TOMBSTONE_HASH_A, 100);

    for (i = 1; i < NGX_MEDIA_TOMBSTONES; i++) {
        ngx_media_tombstone_add(&ring, TOMBSTONE_HASH_B, 200 + i);
    }

    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_A, 100),
                       1);

    /* the next deletion reuses the oldest slot: that one is forgotten */
    ngx_media_tombstone_add(&ring, TOMBSTONE_HASH_B, 500);

    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_A, 100),
                       0);
    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(&ring, TOMBSTONE_HASH_B, 499),
                       1);

    /* and the ring never grows past its slots */
    TEST_ASSERT_EQ_U64(ring.count, NGX_MEDIA_TOMBSTONES);
}

static void
test_null_tolerance(void)
{
    TEST_CASE("NULL tolerance");

    ngx_media_tombstone_init(NULL);
    ngx_media_tombstone_add(NULL, TOMBSTONE_HASH_A, 1);
    TEST_ASSERT_EQ_U64(ngx_media_tombstone_has(NULL, TOMBSTONE_HASH_A, 1), 0);
}

int
main(void)
{
    test_deletion_refuses_what_it_moved_past();
    test_deleted_twice();
    test_the_window_is_bounded();
    test_null_tolerance();

    TEST_LEAKS();

    TEST_MAIN_END();
}

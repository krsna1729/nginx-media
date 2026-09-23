#include "ngx_media_registry.h"
#include "ngx_media_file.h"
#include "ngx_media_hls_ingest.h"
#include "ngx_media_hls_pull.h"
#include "ngx_media_runtime.h"

static ngx_media_registry_t  *ngx_media_registry_worker;

/*
 * How long a deleted stream may stay on the draining list before the worker
 * says so.  A reader stops as soon as it notices its source is gone, which is
 * a refresh interval for a pull and a directory pass for an ingest; a reader
 * still there after this has stopped making progress and its pool is being
 * held by a thread that will not leave.
 */
#define NGX_MEDIA_REGISTRY_DRAIN_WARN_MS  5000

static ngx_int_t
ngx_media_stream_name_eq(const ngx_str_t *a, const ngx_str_t *b)
{
    if (a == NULL || b == NULL || a->len != b->len) {
        return 0;
    }

    return (a->len == 0 || ngx_memcmp(a->data, b->data, a->len) == 0) ? 1 : 0;
}

ngx_int_t
ngx_media_registry_init(ngx_media_registry_t *registry, ngx_pool_t *pool,
    ngx_log_t *log)
{
    if (registry == NULL || pool == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(registry, sizeof(ngx_media_registry_t));

    ngx_queue_init(&registry->entries);
    ngx_queue_init(&registry->draining);
    ngx_media_tombstone_init(&registry->tombstones);
    registry->pool = pool;
    registry->log = log;
    registry->count = 0;

    return NGX_OK;
}

/*
 * Releases one entry: the stream's ordered teardown, then the pool that
 * carries it.  Reached either straight from a delete, when nothing else
 * references the stream, or from the tick once the last reader that
 * referenced it has been closed.
 */
static void
ngx_media_registry_entry_release(ngx_media_registry_entry_t *entry)
{
    ngx_media_stream_destroy(&entry->stream);

    if (entry->pool != NULL) {
        ngx_destroy_pool(entry->pool);
    }

    ngx_free(entry);
}

static void
ngx_media_registry_release_queue(ngx_queue_t *queue)
{
    ngx_queue_t                 *q;
    ngx_media_registry_entry_t  *entry;

    while (!ngx_queue_empty(queue)) {
        q = ngx_queue_head(queue);
        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);

        ngx_queue_remove(&entry->link);

        ngx_media_registry_entry_release(entry);
    }
}

void
ngx_media_registry_destroy(ngx_media_registry_t *registry)
{
    if (registry == NULL || registry->pool == NULL) {
        return;
    }

    /*
     * Both queues go, draining included.  A draining entry waits for a reader
     * that holds a thread, and the shutdown that precedes this has already
     * joined those: exit_process stops the pull, ingest and push threads
     * before the cycle pool is destroyed, so nothing can still be reading a
     * stream whose pool is released here.
     */
    ngx_media_registry_release_queue(&registry->entries);
    ngx_media_registry_release_queue(&registry->draining);

    ngx_queue_init(&registry->entries);
    ngx_queue_init(&registry->draining);
    registry->count = 0;
    registry->draining_count = 0;
}

static void
ngx_media_registry_cleanup(void *data)
{
    ngx_media_registry_t  *registry = data;

    ngx_media_registry_destroy(registry);
}

ngx_media_registry_t *
ngx_media_registry_get(ngx_cycle_t *cycle)
{
    ngx_pool_cleanup_t  *cln;

    if (cycle == NULL) {
        return NULL;
    }

    if (ngx_media_registry_worker != NULL) {
        return ngx_media_registry_worker;
    }

    ngx_media_registry_worker = ngx_pcalloc(cycle->pool,
                                            sizeof(ngx_media_registry_t));
    if (ngx_media_registry_worker == NULL) {
        return NULL;
    }

    if (ngx_media_registry_init(ngx_media_registry_worker, cycle->pool,
                                cycle->log) != NGX_OK)
    {
        ngx_media_registry_worker = NULL;
        return NULL;
    }

    cln = ngx_pool_cleanup_add(cycle->pool, 0);
    if (cln == NULL) {
        ngx_media_registry_worker = NULL;
        return NULL;
    }

    cln->handler = ngx_media_registry_cleanup;
    cln->data = ngx_media_registry_worker;

    return ngx_media_registry_worker;
}

ngx_media_stream_t *
ngx_media_registry_stream(ngx_media_registry_t *registry,
    const ngx_str_t *application, const ngx_str_t *name)
{
    ngx_queue_t                 *q;
    ngx_media_registry_entry_t  *entry;
    ngx_media_stream_t          *stream;

    if (registry == NULL || name == NULL || name->len == 0) {
        return NULL;
    }

    for (q = ngx_queue_head(&registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = q->next)
    {
        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);
        stream = &entry->stream;

        if (ngx_media_stream_name_eq(&stream->name, name)
            && ngx_media_stream_name_eq(&stream->application, application))
        {
            return stream;
        }
    }

    return NULL;
}

ngx_uint_t
ngx_media_registry_stream_is_live(const ngx_media_registry_t *registry,
    const ngx_media_stream_t *stream)
{
    ngx_queue_t                 *q;
    ngx_media_registry_entry_t  *entry;

    if (registry == NULL || stream == NULL) {
        return 0;
    }

    for (q = ngx_queue_head((ngx_queue_t *) &registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = q->next)
    {
        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);

        if (&entry->stream == stream) {
            return 1;
        }
    }

    return 0;
}

ngx_media_stream_t *
ngx_media_registry_stream_create(ngx_media_registry_t *registry,
    const ngx_str_t *application, const ngx_str_t *name,
    const ngx_media_feed_conf_t *feed_conf, ngx_log_t *log)
{
    ngx_media_stream_t          *existing;
    ngx_media_registry_entry_t  *entry;

    if (registry == NULL || name == NULL || name->len == 0) {
        return NULL;
    }

    existing = ngx_media_registry_stream(registry, application, name);
    if (existing != NULL) {
        return existing;
    }

    entry = ngx_alloc(sizeof(ngx_media_registry_entry_t), log);
    if (entry == NULL) {
        return NULL;
    }

    ngx_memzero(entry, sizeof(ngx_media_registry_entry_t));

    entry->pool = ngx_create_pool(4096, log);
    if (entry->pool == NULL) {
        ngx_free(entry);
        return NULL;
    }

    if (ngx_media_stream_init(&entry->stream, entry->pool, log,
                              application, name, feed_conf) != NGX_OK)
    {
        ngx_destroy_pool(entry->pool);
        ngx_free(entry);
        return NULL;
    }

    ngx_queue_insert_tail(&registry->entries, &entry->link);
    registry->count++;

    return &entry->stream;
}

/*
 * Ordered teardown (normative revision): stop the program, then take the
 * sources down, then release the object and its desired-state record.  The
 * order matters because a source that is still running would otherwise write
 * into a feed that no longer exists.
 *
 * The object leaves the registry here and nothing can reach it again, but its
 * memory may not: a reader that holds a thread is closed by the tick, and
 * freeing the pool it points at before then would hand it a pointer into
 * freed memory.  So a stream with such a reader goes on the draining list, and
 * its pool is released by ngx_media_registry_drain() once the reader is gone.
 */
ngx_int_t
ngx_media_registry_stream_destroy(ngx_media_registry_t *registry,
    ngx_media_stream_t *stream)
{
    ngx_media_registry_entry_t  *entry;
    ngx_media_source_t          *source;
    ngx_queue_t                 *q, *next;
    ngx_uint_t                   pull, ingest;

    if (registry == NULL || stream == NULL) {
        return NGX_ERROR;
    }

    for (q = ngx_queue_head(&registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = q->next)
    {
        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);

        if (&entry->stream != stream) {
            continue;
        }

        /*
         * Ordered teardown.  Everything the runtime keys by this stream goes
         * first: the outputs flush from the program feed, the player
         * preparation holds a conversion of it and the routed slots publish
         * into it, so all three have to be released while it is still there.
         */
        ngx_media_runtime_stream_release(&entry->stream);

        /*
         * A file reader is paced by the tick on this thread, so closing it is
         * a close and not a wait: the reader is unlinked, its descriptor
         * released and its source removed.
         */
        ngx_media_file_close_stream(&entry->stream);

        /*
         * The remaining sources are detached rather than destroyed, because
         * the readers that hold a thread - an origin being pulled, a directory
         * being watched - see the removal in their own loop and leave, and a
         * transport that was attached to one of these finds its next publish
         * refused instead of feeding a stream the operator has deleted.
         */
        for (q = ngx_queue_head(&entry->stream.sources);
             q != (ngx_queue_t *) &entry->stream.sources;
             q = next)
        {
            next = q->next;
            source = ngx_queue_data(q, ngx_media_source_t, queue);

            ngx_media_stream_source_remove(&entry->stream, source);
        }

        ngx_queue_remove(&entry->link);
        registry->count--;

        /*
         * The deletion is remembered before the object goes, so a graph
         * operation that was already in flight cannot create the stream
         * again: the revision the deletion moved past is the object's own,
         * which the delete path has already carried up to the operation's
         * when the operation was newer than this replica's copy.
         */
        ngx_media_registry_tombstone(
            registry,
            ngx_media_owner_hash(&entry->stream.application,
                                 &entry->stream.name),
            entry->stream.revision);

        pull = ngx_media_hls_pull_stream_readers(&entry->stream);
        ingest = ngx_media_hls_ingest_stream_readers(&entry->stream);

        if (pull == 0 && ingest == 0) {
            ngx_media_registry_entry_release(entry);
            return NGX_OK;
        }

        entry->draining_since = ngx_current_msec;
        entry->draining_warned = 0;

        ngx_queue_insert_tail(&registry->draining, &entry->link);
        registry->draining_count++;

        ngx_log_error(NGX_LOG_NOTICE, registry->log, 0,
                      "media: %V/%V deleted, its %ui pull and %ui ingest "
                      "readers are still stopping",
                      &entry->stream.application, &entry->stream.name,
                      pull, ingest);

        return NGX_OK;
    }

    return NGX_ERROR;
}

void
ngx_media_registry_drain(ngx_media_registry_t *registry, ngx_log_t *log)
{
    ngx_media_registry_entry_t  *entry;
    ngx_queue_t                 *q, *next;
    ngx_uint_t                   pull, ingest;

    if (registry == NULL) {
        return;
    }

    for (q = ngx_queue_head(&registry->draining);
         q != (ngx_queue_t *) &registry->draining;
         q = next)
    {
        next = q->next;

        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);

        pull = ngx_media_hls_pull_stream_readers(&entry->stream);
        ingest = ngx_media_hls_ingest_stream_readers(&entry->stream);

        if (pull > 0 || ingest > 0) {

            /*
             * The reader has a thread and has been told its source is gone;
             * it leaves in its own time, and the pool it reads stays alive
             * until it does.  An operator who wonders where the memory went
             * gets one line naming what is holding it, not a silent leak.
             */
            if (!entry->draining_warned
                && ngx_current_msec - entry->draining_since
                   > NGX_MEDIA_REGISTRY_DRAIN_WARN_MS)
            {
                entry->draining_warned = 1;

                ngx_log_error(NGX_LOG_WARN, log, 0,
                              "media: %V/%V is still held by %ui pull and %ui "
                              "ingest readers %Mms after its delete",
                              &entry->stream.application, &entry->stream.name,
                              pull, ingest,
                              ngx_current_msec - entry->draining_since);
            }

            continue;
        }

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "media: %V/%V released, its readers have stopped",
                      &entry->stream.application, &entry->stream.name);

        ngx_queue_remove(&entry->link);
        registry->draining_count--;

        ngx_media_registry_entry_release(entry);
    }
}

ngx_uint_t
ngx_media_registry_draining_count(const ngx_media_registry_t *registry)
{
    return (registry != NULL) ? registry->draining_count : 0;
}

void
ngx_media_registry_tombstone(ngx_media_registry_t *registry, uint64_t hash,
    uint64_t revision)
{
    if (registry == NULL) {
        return;
    }

    ngx_media_tombstone_add(&registry->tombstones, hash, revision);
}

ngx_uint_t
ngx_media_registry_is_tombstoned(const ngx_media_registry_t *registry,
    uint64_t hash, uint64_t revision)
{
    if (registry == NULL) {
        return 0;
    }

    return ngx_media_tombstone_has(&registry->tombstones, hash, revision);
}

ngx_uint_t
ngx_media_registry_count(const ngx_media_registry_t *registry)
{
    return (registry != NULL) ? registry->count : 0;
}

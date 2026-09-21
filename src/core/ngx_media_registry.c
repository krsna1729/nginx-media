#include "ngx_media_registry.h"

static ngx_media_registry_t  *ngx_media_registry_worker;

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
    (void) log;

    if (registry == NULL || pool == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(registry, sizeof(ngx_media_registry_t));

    ngx_queue_init(&registry->entries);
    registry->pool = pool;
    registry->count = 0;

    return NGX_OK;
}

void
ngx_media_registry_destroy(ngx_media_registry_t *registry)
{
    ngx_queue_t                    *q, *next;
    ngx_media_registry_entry_t     *entry;

    if (registry == NULL || registry->pool == NULL) {
        return;
    }

    for (q = ngx_queue_head(&registry->entries);
         q != (ngx_queue_t *) &registry->entries;
         q = next)
    {
        next = q->next;
        entry = ngx_queue_data(q, ngx_media_registry_entry_t, link);

        ngx_media_stream_destroy(&entry->stream);
    }

    ngx_queue_init(&registry->entries);
    registry->count = 0;
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

    entry = ngx_pcalloc(registry->pool, sizeof(ngx_media_registry_entry_t));
    if (entry == NULL) {
        return NULL;
    }

    if (ngx_media_stream_init(&entry->stream, registry->pool, log,
                              application, name, feed_conf) != NGX_OK)
    {
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
 */
ngx_int_t
ngx_media_registry_stream_destroy(ngx_media_registry_t *registry,
    ngx_media_stream_t *stream)
{
    ngx_media_registry_entry_t  *entry;
    ngx_queue_t                 *q;

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

        ngx_media_stream_destroy(&entry->stream);

        ngx_queue_remove(&entry->link);
        registry->count--;

        return NGX_OK;
    }

    return NGX_ERROR;
}

ngx_uint_t
ngx_media_registry_count(const ngx_media_registry_t *registry)
{
    return (registry != NULL) ? registry->count : 0;
}

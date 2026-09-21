#include "ngx_media_ts_ingest.h"

static ngx_uint_t ngx_media_ts_ingest_pow2(ngx_uint_t n);

static void
ngx_media_ts_ingest_lock(ngx_media_ts_ingest_t *ingest)
{
    while (!ngx_atomic_cmp_set(&ingest->lock, 0, 1)) {
        /* spin: critical sections are short (no allocation, no I/O) */
    }
}

static void
ngx_media_ts_ingest_unlock(ngx_media_ts_ingest_t *ingest)
{
    (void) ngx_atomic_cmp_set(&ingest->lock, 1, 0);
}

static ngx_uint_t
ngx_media_ts_ingest_pow2(ngx_uint_t n)
{
    ngx_uint_t  p;

    for (p = 1; p < n; p <<= 1) {
        if (p > ((ngx_uint_t) -1 >> 1)) {
            return 0;
        }
    }

    return p;
}

ngx_int_t
ngx_media_ts_ingest_init(ngx_media_ts_ingest_t *ingest,
    const ngx_media_ts_ingest_conf_t *conf, ngx_log_t *log)
{
    if (ingest == NULL || conf == NULL || conf->max_chunks == 0
        || conf->max_bytes == 0)
    {
        return NGX_ERROR;
    }

    ngx_memzero(ingest, sizeof(ngx_media_ts_ingest_t));

    ingest->capacity = ngx_media_ts_ingest_pow2(conf->max_chunks);
    if (ingest->capacity == 0) {
        return NGX_ERROR;
    }

    if (ingest->capacity > (size_t) -1 / sizeof(ngx_media_ts_ingest_chunk_t)) {
        return NGX_ERROR;
    }

    ingest->chunks = ngx_alloc(
        ingest->capacity * sizeof(ngx_media_ts_ingest_chunk_t), log);
    if (ingest->chunks == NULL) {
        ingest->capacity = 0;
        return NGX_ERROR;
    }

    ngx_memzero(ingest->chunks,
                ingest->capacity * sizeof(ngx_media_ts_ingest_chunk_t));

    ingest->max_chunks = conf->max_chunks;
    ingest->max_bytes = conf->max_bytes;

    return NGX_OK;
}

void
ngx_media_ts_ingest_destroy(ngx_media_ts_ingest_t *ingest)
{
    uint64_t  seq;

    if (ingest == NULL || ingest->chunks == NULL) {
        return;
    }

    ngx_media_ts_ingest_lock(ingest);

    for (seq = ingest->tail; seq < ingest->head; seq++) {
        ngx_free(ingest->chunks[seq & (ingest->capacity - 1)].data);
    }

    ngx_free(ingest->chunks);

    ngx_memzero(ingest, sizeof(ngx_media_ts_ingest_t));
}

ngx_int_t
ngx_media_ts_ingest_write(ngx_media_ts_ingest_t *ingest, uint64_t session_id,
    const u_char *data, size_t len, ngx_msec_t now)
{
    ngx_media_ts_ingest_chunk_t  *chunk;
    u_char                       *copy;
    ngx_int_t                     rc;

    if (ingest == NULL || ingest->chunks == NULL || data == NULL || len == 0
        || len > NGX_MEDIA_TS_INGEST_CHUNK_MAX)
    {
        return NGX_ERROR;
    }

    ngx_media_ts_ingest_lock(ingest);

    ingest->stats.bytes_in += len;
    ingest->stats.chunks_in++;
    ingest->stats.last_write = now;

    if (ingest->head - ingest->tail >= ingest->max_chunks
        || ingest->bytes + len > ingest->max_bytes)
    {
        ingest->stats.chunks_dropped++;
        ingest->stats.bytes_dropped += len;

        ngx_media_ts_ingest_unlock(ingest);
        return NGX_AGAIN;
    }

    copy = ngx_alloc(len, NULL);
    if (copy == NULL) {
        ingest->stats.chunks_dropped++;
        ingest->stats.bytes_dropped += len;

        ngx_media_ts_ingest_unlock(ingest);
        return NGX_ERROR;
    }

    ngx_memcpy(copy, data, len);

    chunk = &ingest->chunks[ingest->head & (ingest->capacity - 1)];
    chunk->data = copy;
    chunk->len = len;
    chunk->time = now;
    chunk->session_id = session_id;

    ingest->bytes += len;
    ingest->head++;

    rc = NGX_OK;

    ngx_media_ts_ingest_unlock(ingest);

    return rc;
}

ngx_uint_t
ngx_media_ts_ingest_read(ngx_media_ts_ingest_t *ingest,
    ngx_media_ts_ingest_chunk_t *out, ngx_uint_t max_chunks)
{
    ngx_media_ts_ingest_chunk_t  *chunk;
    ngx_uint_t                    count;

    if (ingest == NULL || ingest->chunks == NULL || out == NULL
        || max_chunks == 0)
    {
        return 0;
    }

    ngx_media_ts_ingest_lock(ingest);

    count = 0;

    while (count < max_chunks && ingest->tail < ingest->head) {
        chunk = &ingest->chunks[ingest->tail & (ingest->capacity - 1)];

        out[count] = *chunk;

        ingest->bytes -= chunk->len;
        ingest->tail++;
        count++;
    }

    ngx_media_ts_ingest_unlock(ingest);

    return count;
}

void
ngx_media_ts_ingest_release(ngx_media_ts_ingest_chunk_t *chunks,
    ngx_uint_t count)
{
    ngx_uint_t  i;

    if (chunks == NULL) {
        return;
    }

    for (i = 0; i < count; i++) {
        ngx_free(chunks[i].data);
        chunks[i].data = NULL;
        chunks[i].len = 0;
    }
}

void
ngx_media_ts_ingest_stats(ngx_media_ts_ingest_t *ingest,
    ngx_media_ts_ingest_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    ngx_memzero(out, sizeof(ngx_media_ts_ingest_stats_t));

    if (ingest == NULL || ingest->chunks == NULL) {
        return;
    }

    ngx_media_ts_ingest_lock(ingest);
    *out = ingest->stats;
    ngx_media_ts_ingest_unlock(ingest);
}

ngx_uint_t
ngx_media_ts_ingest_pending(ngx_media_ts_ingest_t *ingest)
{
    ngx_uint_t  pending;

    if (ingest == NULL || ingest->chunks == NULL) {
        return 0;
    }

    ngx_media_ts_ingest_lock(ingest);
    pending = (ngx_uint_t) (ingest->head - ingest->tail);
    ngx_media_ts_ingest_unlock(ingest);

    return pending;
}

size_t
ngx_media_ts_ingest_bytes(ngx_media_ts_ingest_t *ingest)
{
    size_t  bytes;

    if (ingest == NULL || ingest->chunks == NULL) {
        return 0;
    }

    ngx_media_ts_ingest_lock(ingest);
    bytes = ingest->bytes;
    ngx_media_ts_ingest_unlock(ingest);

    return bytes;
}

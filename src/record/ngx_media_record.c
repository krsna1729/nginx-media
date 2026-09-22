#include "ngx_media_record.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

void
ngx_media_record_conf_default(ngx_media_record_conf_t *conf)
{
    if (conf == NULL) {
        return;
    }

    ngx_memzero(conf, sizeof(ngx_media_record_conf_t));

    conf->tap = NGX_MEDIA_RECORD_PROGRAM;
    conf->max_part_bytes = 512 * 1024 * 1024;
    conf->max_pending_bytes = 32 * 1024 * 1024;
    conf->max_jobs = 1024;
}

/* part 1 is the configured path; later parts insert a sequence number */
static ngx_int_t
ngx_media_record_open_part(ngx_media_record_t *rec)
{
    u_char  *dot, *p, *name;
    size_t   len;
    int      fd;

    rec->stats.parts++;

    if (rec->stats.parts == 1) {
        len = rec->conf.path.len + 1;
        name = ngx_alloc(len, NULL);

        if (name == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(name, rec->conf.path.data, rec->conf.path.len);
        name[rec->conf.path.len] = '\0';

    } else {
        /* path.ts -> path-0002.ts */
        dot = NULL;

        for (p = rec->conf.path.data + rec->conf.path.len;
             p > rec->conf.path.data; p--)
        {
            if (*(p - 1) == '.') {
                dot = p - 1;
                break;
            }

            if (*(p - 1) == '/') {
                break;
            }
        }

        if (dot != NULL) {
            size_t  prefix = (size_t) (dot - rec->conf.path.data);
            size_t  suffix = rec->conf.path.len - prefix;

            /*
             * The suffix is a slice, not a string: dot points into an
             * ngx_str_t with no NUL guarantee, so %s would read past
             * path.len.  Size for the longest uint64 part number.
             */
            len = prefix + 1 + 20 + suffix + 1;

        } else {
            len = rec->conf.path.len + 1 + 20 + 1;
        }

        name = ngx_alloc(len, NULL);

        if (name == NULL) {
            return NGX_ERROR;
        }

        if (dot != NULL) {
            size_t  prefix = (size_t) (dot - rec->conf.path.data);
            size_t  suffix = rec->conf.path.len - prefix;
            int     n;

            n = snprintf((char *) name, len, "%.*s-%04llu%.*s", (int) prefix,
                         (char *) rec->conf.path.data,
                         (unsigned long long) rec->stats.parts,
                         (int) suffix, (char *) dot);

            if (n < 0 || (size_t) n >= len) {
                ngx_free(name);
                return NGX_ERROR;
            }

        } else {
            snprintf((char *) name, len, "%.*s-%04llu",
                     (int) rec->conf.path.len, (char *) rec->conf.path.data,
                     (unsigned long long) rec->stats.parts);
        }
    }

    fd = open((const char *) name, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    ngx_free(name);

    if (fd == -1) {
        rec->stats.write_errors++;
        return NGX_ERROR;
    }

    rec->fd = fd;
    rec->part_bytes = 0;

    return NGX_OK;
}

static void
ngx_media_record_close_part(ngx_media_record_t *rec)
{
    if (rec->fd != -1) {
        (void) close(rec->fd);
        rec->fd = -1;
    }
}

static void
ngx_media_record_write_job(ngx_media_record_t *rec,
    ngx_media_record_job_t *job)
{
    const u_char  *p = ngx_media_buf_data(job->buf) + job->offset;
    size_t         left = job->len;

    if (rec->fd == -1 && ngx_media_record_open_part(rec) != NGX_OK) {
        rec->stats.bytes_dropped += job->len;
        return;
    }

    while (left > 0) {
        ssize_t  n = write(rec->fd, p, left);

        if (n <= 0) {
            if (n == -1 && errno == EINTR) {
                continue;
            }

            rec->stats.write_errors++;
            rec->stats.bytes_dropped += left;
            return;
        }

        p += n;
        left -= (size_t) n;
        rec->part_bytes += (size_t) n;
        rec->stats.bytes_written += (uint64_t) n;
    }

    if (rec->part_bytes >= rec->conf.max_part_bytes) {
        /* roll to a new part rather than growing one file forever */
        ngx_media_record_close_part(rec);
    }
}

static void *
ngx_media_record_thread(void *data)
{
    ngx_media_record_t     *rec = data;
    ngx_media_record_job_t  job;
    ngx_uint_t              have;

    for ( ;; ) {
        (void) pthread_mutex_lock(&rec->mutex);

        have = (rec->jobs_head != rec->jobs_tail);

        while (!have && !rec->stop) {
            (void) pthread_cond_wait(&rec->cond, &rec->mutex);
            have = (rec->jobs_head != rec->jobs_tail);
        }

        if (!have && rec->stop) {
            (void) pthread_mutex_unlock(&rec->mutex);
            break;
        }

        job = rec->jobs[rec->jobs_head];
        rec->jobs_head = (rec->jobs_head + 1) % rec->jobs_capacity;

        rec->stats.pending_jobs--;
        rec->stats.pending_bytes -= job.len;

        (void) pthread_mutex_unlock(&rec->mutex);

        ngx_media_record_write_job(rec, &job);

        ngx_media_buf_unref(job.buf);
    }

    return NULL;
}

ngx_int_t
ngx_media_record_init(ngx_media_record_t *rec,
    const ngx_media_record_conf_t *conf, ngx_log_t *log)
{
    (void) log;

    if (rec == NULL || conf == NULL || conf->path.len == 0) {
        return NGX_ERROR;
    }

    ngx_memzero(rec, sizeof(ngx_media_record_t));

    rec->conf = *conf;

    if (rec->conf.max_jobs == 0) {
        rec->conf.max_jobs = 1024;
    }

    if (rec->conf.max_pending_bytes == 0) {
        rec->conf.max_pending_bytes = 32 * 1024 * 1024;
    }

    if (rec->conf.max_part_bytes == 0) {
        rec->conf.max_part_bytes = 512 * 1024 * 1024;
    }

    rec->jobs = ngx_alloc(rec->conf.max_jobs * sizeof(ngx_media_record_job_t),
                          log);

    if (rec->jobs == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(rec->jobs,
                rec->conf.max_jobs * sizeof(ngx_media_record_job_t));

    rec->jobs_capacity = rec->conf.max_jobs;
    rec->fd = -1;

    if (pthread_mutex_init(&rec->mutex, NULL) != 0) {
        ngx_free(rec->jobs);
        rec->jobs = NULL;
        return NGX_ERROR;
    }

    if (pthread_cond_init(&rec->cond, NULL) != 0) {
        (void) pthread_mutex_destroy(&rec->mutex);
        ngx_free(rec->jobs);
        rec->jobs = NULL;
        return NGX_ERROR;
    }

    if (pthread_create(&rec->thread, NULL, ngx_media_record_thread, rec)
        != 0)
    {
        (void) pthread_cond_destroy(&rec->cond);
        (void) pthread_mutex_destroy(&rec->mutex);
        ngx_free(rec->jobs);
        rec->jobs = NULL;
        return NGX_ERROR;
    }

    rec->thread_started = 1;

    return NGX_OK;
}

void
ngx_media_record_stop(ngx_media_record_t *rec)
{
    ngx_uint_t  i;

    if (rec == NULL || rec->jobs == NULL) {
        return;
    }

    (void) pthread_mutex_lock(&rec->mutex);
    rec->stop = 1;
    (void) pthread_cond_signal(&rec->cond);
    (void) pthread_mutex_unlock(&rec->mutex);

    if (rec->thread_started) {
        (void) pthread_join(rec->thread, NULL);
        rec->thread_started = 0;
    }

    /* any job that never reached the writer is released here */
    for (i = rec->jobs_head; i != rec->jobs_tail;
         i = (i + 1) % rec->jobs_capacity)
    {
        ngx_media_buf_unref(rec->jobs[i].buf);
    }

    ngx_media_record_close_part(rec);

    (void) pthread_cond_destroy(&rec->cond);
    (void) pthread_mutex_destroy(&rec->mutex);

    ngx_free(rec->jobs);
    rec->jobs = NULL;
    rec->jobs_capacity = 0;
    rec->stopped = 1;
}

ngx_int_t
ngx_media_record_append(ngx_media_record_t *rec, ngx_media_buf_t *buf,
    size_t offset, size_t len)
{
    ngx_uint_t  next;

    if (rec == NULL || rec->jobs == NULL || rec->stopped || buf == NULL
        || len == 0)
    {
        return NGX_ERROR;
    }

    if (offset + len > ngx_media_buf_size(buf)) {
        return NGX_ERROR;
    }

    (void) pthread_mutex_lock(&rec->mutex);

    next = (rec->jobs_tail + 1) % rec->jobs_capacity;

    if (next == rec->jobs_head
        || rec->stats.pending_bytes + len > rec->conf.max_pending_bytes)
    {
        /* bounded: drop and count instead of blocking the media path */
        rec->stats.jobs_dropped++;
        rec->stats.bytes_dropped += len;

        (void) pthread_mutex_unlock(&rec->mutex);

        return NGX_AGAIN;
    }

    rec->jobs[rec->jobs_tail].buf = ngx_media_buf_ref(buf);
    rec->jobs[rec->jobs_tail].offset = offset;
    rec->jobs[rec->jobs_tail].len = len;
    rec->jobs_tail = next;

    rec->stats.jobs++;
    rec->stats.pending_jobs++;
    rec->stats.pending_bytes += len;

    (void) pthread_cond_signal(&rec->cond);
    (void) pthread_mutex_unlock(&rec->mutex);

    return NGX_OK;
}

void
ngx_media_record_stats(ngx_media_record_t *rec, ngx_media_record_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    if (rec == NULL) {
        ngx_memzero(out, sizeof(ngx_media_record_stats_t));
        return;
    }

    if (rec->stopped) {
        /* the writer is gone: the counters are stable, no lock needed */
        *out = rec->stats;
        return;
    }

    (void) pthread_mutex_lock(&rec->mutex);
    *out = rec->stats;
    (void) pthread_mutex_unlock(&rec->mutex);
}

ngx_int_t
ngx_media_record_append_bytes(ngx_media_record_t *rec, const u_char *data,
    size_t len)
{
    ngx_media_buf_t  *buf;
    ngx_int_t         rc;

    if (rec == NULL || data == NULL || len == 0) {
        return NGX_ERROR;
    }

    buf = ngx_media_buf_alloc(len);

    if (buf == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(ngx_media_buf_data(buf), data, len);
    (void) ngx_media_buf_freeze(buf, len);

    rc = ngx_media_record_append(rec, buf, 0, len);

    ngx_media_buf_unref(buf);

    return rc;
}

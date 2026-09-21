#ifndef NGX_MEDIA_RECORD_H
#define NGX_MEDIA_RECORD_H

#include "ngx_media.h"

#include <pthread.h>

/*
 * Recording taps (goal doc 20).
 *
 *   RAW      transport bytes before normalization
 *   ISO      one named source before the selector
 *   PROGRAM  post-selection, timeline-normalized program
 *
 * Writing never blocks the event loop: the producer only hands buffer
 * references to a bounded queue and a writer thread performs the blocking
 * file I/O.  When the queue is full the recording drops and counts rather
 * than pushing back into the media path.
 *
 * Parts roll at a hard byte ceiling; the file is closed cleanly when the
 * recording stops, so a reload never needs to transfer an open descriptor.
 */

#define NGX_MEDIA_RECORD_RAW      1
#define NGX_MEDIA_RECORD_ISO      2
#define NGX_MEDIA_RECORD_PROGRAM  3

typedef struct {
    ngx_str_t   path;                /* part 1; later parts get a suffix */
    ngx_uint_t  tap;
    size_t      max_part_bytes;
    size_t      max_pending_bytes;
    ngx_uint_t  max_jobs;
} ngx_media_record_conf_t;

typedef struct {
    uint64_t    bytes_written;
    uint64_t    parts;
    uint64_t    jobs;
    uint64_t    jobs_dropped;
    uint64_t    bytes_dropped;
    uint64_t    write_errors;
    ngx_uint_t  pending_jobs;
    size_t      pending_bytes;
} ngx_media_record_stats_t;

typedef struct {
    ngx_media_buf_t  *buf;
    size_t            offset;
    size_t            len;
} ngx_media_record_job_t;

typedef struct {
    ngx_media_record_conf_t  conf;

    pthread_mutex_t          mutex;
    pthread_cond_t           cond;
    pthread_t                thread;
    ngx_uint_t               thread_started;
    unsigned                 stop:1;
    unsigned                 stopped:1;

    ngx_media_record_job_t  *jobs;
    ngx_uint_t               jobs_capacity;
    ngx_uint_t               jobs_head;    /* next job to write */
    ngx_uint_t               jobs_tail;    /* next free slot */

    ngx_media_record_stats_t stats;

    int                      fd;
    size_t                   part_bytes;
} ngx_media_record_t;

void ngx_media_record_conf_default(ngx_media_record_conf_t *conf);

ngx_int_t ngx_media_record_init(ngx_media_record_t *rec,
    const ngx_media_record_conf_t *conf, ngx_log_t *log);
void ngx_media_record_stop(ngx_media_record_t *rec);

/* takes a reference to the buffer; the job is dropped when the queue is full */
ngx_int_t ngx_media_record_append(ngx_media_record_t *rec,
    ngx_media_buf_t *buf, size_t offset, size_t len);

void ngx_media_record_stats(ngx_media_record_t *rec,
    ngx_media_record_stats_t *out);

#endif /* NGX_MEDIA_RECORD_H */

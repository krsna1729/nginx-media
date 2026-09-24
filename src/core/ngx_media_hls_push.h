#ifndef NGX_MEDIA_HLS_PUSH_H
#define NGX_MEDIA_HLS_PUSH_H

#include "ngx_media.h"

#include <sys/types.h>

/*
 * HLS push publishes files after the segmenter atomically renames each
 * completed segment or playlist. The notifier opens the sealed inode once and
 * shares references across the bounded worker-local queues. A file remains
 * readable after HLS retention unlinks its path; queue overflow drops and
 * accounts the oldest queued item.
 */

typedef struct ngx_media_hls_push_t ngx_media_hls_push_t;

/* registers the backend and starts the upload pool */
ngx_int_t ngx_media_hls_push_register(ngx_log_t *log);

/* stops the pool and joins its threads after output finalization */
void ngx_media_hls_push_stop(void);

/* adjusts active uploaders using shared CPU and queue-pressure signals */
void ngx_media_hls_push_adapt(void);

/* Opens a sealed inode once, then shares it across matching push queues. */
void ngx_media_hls_push_sealed(const ngx_str_t *directory,
    const ngx_str_t *path, ngx_log_t *log);

/* totals across destinations, for the control API */
ngx_uint_t ngx_media_hls_push_count(void);
uint64_t ngx_media_hls_push_uploaded_total(void);
uint64_t ngx_media_hls_push_dropped_total(void);
uint64_t ngx_media_hls_push_failed_total(void);

#endif /* NGX_MEDIA_HLS_PUSH_H */

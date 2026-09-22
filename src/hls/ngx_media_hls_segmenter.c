#include "ngx_media_hls_segmenter.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define NGX_MEDIA_HLS_MS(dts)  ((ngx_msec_t) ((dts) / 90))

static ngx_int_t ngx_media_hls_write_file(ngx_media_hls_t *hls,
    const ngx_str_t *name);
static ngx_int_t ngx_media_hls_write_playlist(ngx_media_hls_t *hls);
static ngx_int_t ngx_media_hls_cut(ngx_media_hls_t *hls);
static ngx_int_t ngx_media_hls_piece(ngx_media_hls_t *hls,
    ngx_media_buf_t *buf, size_t offset, size_t len);
static void ngx_media_hls_evict(ngx_media_hls_t *hls);
static ngx_int_t ngx_media_hls_path(ngx_media_hls_t *hls,
    const ngx_str_t *name, ngx_str_t *out, ngx_pool_t *pool);
static u_char *ngx_media_hls_printf(u_char *p, u_char *end,
    const char *fmt, ...);

static u_char *
ngx_media_hls_printf(u_char *p, u_char *end, const char *fmt, ...)
{
    va_list  ap;
    size_t   avail;
    int      n;

    avail = (size_t) (end - p);

    if (avail == 0) {
        return p;
    }

    va_start(ap, fmt);
    n = vsnprintf((char *) p, avail, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return p;
    }

    if ((size_t) n >= avail) {
        return end;
    }

    return p + n;
}

void
ngx_media_hls_conf_default(ngx_media_hls_conf_t *conf)
{
    if (conf == NULL) {
        return;
    }

    ngx_memzero(conf, sizeof(ngx_media_hls_conf_t));

    conf->target_duration = 6000;
    conf->min_duration = 3000;
    conf->max_duration = 12000;
    conf->max_segment_bytes = 8 * 1024 * 1024;
    conf->max_segments = 6;
    conf->max_retained_bytes = 64 * 1024 * 1024;
}

/*
 * Creates the directory the playlist and the segments are written to, and every
 * directory above it.
 *
 * One mkdir() is not enough once a program's output is a directory of its own
 * under the configured root: the root may not exist yet either, and a failure
 * to create the program's directory would show up as a playlist that never
 * appears rather than as an error.
 */
static void
ngx_media_hls_mkdir_p(const ngx_str_t *path, ngx_log_t *log)
{
    u_char  *dir;
    size_t   i;

    if (path->len == 0) {
        return;
    }

    /*
     * The configured directory is an ngx_str_t, not a C string: build a
     * terminated copy before handing components to mkdir().
     */
    dir = ngx_alloc(path->len + 1, log);

    if (dir == NULL) {
        return;
    }

    ngx_memcpy(dir, path->data, path->len);
    dir[path->len] = '\0';

    for (i = 1; i <= path->len; i++) {

        if (i < path->len && dir[i] != '/') {
            continue;
        }

        dir[i] = '\0';

        if (dir[0] != '\0') {
            (void) mkdir((const char *) dir, 0755);
        }

        if (i < path->len) {
            dir[i] = '/';
        }
    }

    ngx_free(dir);
}

ngx_int_t
ngx_media_hls_init(ngx_media_hls_t *hls, const ngx_media_hls_conf_t *conf,
    ngx_log_t *log)
{
    ngx_uint_t  capacity;

    if (hls == NULL || conf == NULL || conf->path.len == 0) {
        return NGX_ERROR;
    }

    ngx_memzero(hls, sizeof(ngx_media_hls_t));

    /* after the memzero: setting it first is how it gets wiped */
    hls->log = log;

    hls->conf = *conf;

    if (hls->conf.playlist_name.len == 0) {
        hls->conf.playlist_name.data = (u_char *) "index.m3u8";
        hls->conf.playlist_name.len = sizeof("index.m3u8") - 1;
    }

    if (hls->conf.segment_prefix.len == 0) {
        hls->conf.segment_prefix.data = (u_char *) "seg-";
        hls->conf.segment_prefix.len = sizeof("seg-") - 1;
    }

    if (hls->conf.max_segments == 0) {
        hls->conf.max_segments = 6;
    }

    if (hls->conf.target_duration == 0) {
        hls->conf.target_duration = 6000;
    }

    if (hls->conf.min_duration > hls->conf.target_duration) {
        hls->conf.min_duration = hls->conf.target_duration;
    }

    if (hls->conf.max_duration < hls->conf.target_duration) {
        hls->conf.max_duration = hls->conf.target_duration * 2;
    }

    capacity = hls->conf.max_segments + 1;

    hls->segments = ngx_alloc(capacity * sizeof(ngx_media_hls_segment_t), log);
    if (hls->segments == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(hls->segments, capacity * sizeof(ngx_media_hls_segment_t));

    hls->segments_capacity = capacity;

    ngx_media_hls_mkdir_p(&hls->conf.path, log);

    return NGX_OK;
}

void
ngx_media_hls_destroy(ngx_media_hls_t *hls)
{
    ngx_uint_t  i;

    if (hls == NULL) {
        return;
    }

    for (i = 0; i < hls->nsegments; i++) {
        ngx_free(hls->segments[i].name.data);
    }

    for (i = 0; i < hls->npieces; i++) {
        ngx_media_buf_unref(hls->pieces[i].buf);
    }

    ngx_free(hls->segments);

    ngx_memzero(hls, sizeof(ngx_media_hls_t));
}

static ngx_int_t
ngx_media_hls_path(ngx_media_hls_t *hls, const ngx_str_t *name, ngx_str_t *out,
    ngx_pool_t *pool)
{
    u_char  *p;

    out->len = hls->conf.path.len + 1 + name->len;

    if (pool != NULL) {
        out->data = ngx_pnalloc(pool, out->len + 1);

    } else {
        out->data = ngx_alloc(out->len + 1, NULL);
    }

    if (out->data == NULL) {
        return NGX_ERROR;
    }

    p = out->data;
    ngx_memcpy(p, hls->conf.path.data, hls->conf.path.len);
    p += hls->conf.path.len;
    *p++ = '/';
    ngx_memcpy(p, name->data, name->len);
    p += name->len;
    *p = '\0';   /* open(), unlink() and rename() take C strings */

    return NGX_OK;
}

ngx_msec_t
ngx_media_hls_pending_duration(const ngx_media_hls_t *hls)
{
    if (hls == NULL || hls->npieces == 0) {
        return 0;
    }

    return NGX_MEDIA_HLS_MS(hls->last_dts - hls->first_dts);
}

ngx_uint_t
ngx_media_hls_segments(const ngx_media_hls_t *hls)
{
    return (hls != NULL) ? hls->nsegments : 0;
}

ngx_int_t
ngx_media_hls_add_burst(ngx_media_hls_t *hls,
    const ngx_media_ts_burst_t *burst)
{
    ngx_uint_t  i;

    if (hls == NULL || burst == NULL || burst->backing == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < burst->nslices; i++) {
        const ngx_media_ts_slice_t  *slice = &burst->slices[i];
        ngx_msec_t                   duration;

        /*
         * A segment may only start at a video sync boundary: everything else
         * is dropped rather than written into an undecodable segment.
         */
        if (!hls->started) {

            if (slice->media_type == NGX_MEDIA_TYPE_VIDEO
                && slice->keyframe)
            {
                hls->started = 1;
                hls->first_dts = slice->dts;
                hls->last_dts = slice->dts;

            } else {
                hls->dropped_frames++;
                continue;
            }
        }

        duration = NGX_MEDIA_HLS_MS(hls->last_dts - hls->first_dts);

        if (hls->npieces > 0
            && slice->media_type == NGX_MEDIA_TYPE_VIDEO
            && slice->keyframe
            && duration >= hls->conf.min_duration)
        {
            (void) ngx_media_hls_cut(hls);

            hls->started = 1;
            hls->first_dts = slice->dts;
            hls->last_dts = slice->dts;

        } else if (hls->npieces > 0
                   && duration >= hls->conf.max_duration)
        {
            (void) ngx_media_hls_cut(hls);
            hls->forced_cuts++;

            hls->started = 1;
            hls->first_dts = slice->dts;
            hls->last_dts = slice->dts;
        }

        if (hls->npieces >= NGX_MEDIA_HLS_MAX_PIECES) {
            /* hard ceiling: the piece list never grows without bound */
            (void) ngx_media_hls_cut(hls);
            hls->forced_cuts++;

            hls->started = 0;
            hls->dropped_frames++;
            continue;
        }
        if (hls->npieces == 0 && burst->psi_len > 0) {
            if (ngx_media_hls_piece(hls, burst->backing, 0,
                                   burst->psi_len) != NGX_OK)
            {
                hls->errors++;
            }
        }

        if (ngx_media_hls_piece(hls, burst->backing, slice->offset,
                                slice->len) != NGX_OK)
        {
            hls->errors++;
            continue;
        }

        hls->last_dts = slice->dts;

        if (hls->bytes > hls->conf.max_segment_bytes) {
            (void) ngx_media_hls_cut(hls);
            hls->forced_cuts++;

            hls->started = 0;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_media_hls_piece(ngx_media_hls_t *hls, ngx_media_buf_t *buf, size_t offset,
    size_t len)
{
    if (hls->npieces >= NGX_MEDIA_HLS_MAX_PIECES) {
        return NGX_ERROR;
    }

    hls->pieces[hls->npieces].buf = ngx_media_buf_ref(buf);
    hls->pieces[hls->npieces].offset = offset;
    hls->pieces[hls->npieces].len = len;
    hls->npieces++;

    hls->bytes += len;

    return NGX_OK;
}

ngx_int_t
ngx_media_hls_discontinuity(ngx_media_hls_t *hls)
{
    if (hls == NULL) {
        return NGX_ERROR;
    }

    if (hls->npieces > 0) {
        (void) ngx_media_hls_cut(hls);
    }

    hls->discontinuity_next = 1;
    hls->started = 0;

    return NGX_OK;
}

ngx_int_t
ngx_media_hls_finish(ngx_media_hls_t *hls)
{
    if (hls == NULL) {
        return NGX_ERROR;
    }

    if (hls->npieces == 0) {
        return NGX_OK;
    }

    return ngx_media_hls_cut(hls);
}

static ngx_int_t
ngx_media_hls_cut(ngx_media_hls_t *hls)
{
    ngx_media_hls_segment_t  *segment;
    ngx_uint_t                i;

    if (hls->npieces == 0 || hls->nsegments >= hls->segments_capacity) {
        return NGX_OK;
    }

    segment = &hls->segments[hls->nsegments];

    ngx_memzero(segment, sizeof(ngx_media_hls_segment_t));

    segment->sequence = hls->next_sequence++;
    segment->duration = NGX_MEDIA_HLS_MS(hls->last_dts - hls->first_dts);
    segment->bytes = hls->bytes;
    segment->discontinuity = hls->discontinuity_next ? 1 : 0;

    segment->name.len = hls->conf.segment_prefix.len + 6 + 3;
    segment->name.data = ngx_alloc(segment->name.len + 1, hls->log);

    if (segment->name.data == NULL) {
        hls->errors++;
        ngx_log_error(NGX_LOG_ERR, hls->log, 0,
                      "media: hls could not allocate a segment name; "
                      "%uL segment(s) dropped so far", hls->errors);
        goto reset;
    }

    (void) snprintf((char *) segment->name.data, segment->name.len + 1,
                    "%.*s%06lu.ts",
                    (int) hls->conf.segment_prefix.len,
                    (char *) hls->conf.segment_prefix.data,
                    (unsigned long) segment->sequence);

    if (ngx_media_hls_write_file(hls, &segment->name) != NGX_OK) {
        hls->errors++;
        ngx_log_error(NGX_LOG_ERR, hls->log, ngx_errno,
                      "media: hls could not write %V into %V; %uL segment(s) "
                      "dropped so far", &segment->name, &hls->conf.path,
                      hls->errors);
        ngx_free(segment->name.data);
        segment->name.data = NULL;
        goto reset;
    }

    hls->nsegments++;
    hls->retained_bytes += segment->bytes;
    hls->segments_written++;
    hls->bytes_written += segment->bytes;
    hls->discontinuity_next = 0;

    ngx_media_hls_evict(hls);

    (void) ngx_media_hls_write_playlist(hls);

reset:

    for (i = 0; i < hls->npieces; i++) {
        ngx_media_buf_unref(hls->pieces[i].buf);
        hls->pieces[i].buf = NULL;
    }

    hls->npieces = 0;
    hls->bytes = 0;
    hls->first_dts = 0;
    hls->last_dts = 0;

    return NGX_OK;
}

static void
ngx_media_hls_evict(ngx_media_hls_t *hls)
{
    ngx_media_hls_segment_t  *segments = hls->segments;
    ngx_str_t                 path;
    ngx_uint_t                i;

    while (hls->nsegments > hls->conf.max_segments
           || (hls->conf.max_retained_bytes != 0
               && hls->retained_bytes > hls->conf.max_retained_bytes))
    {
        if (segments[0].discontinuity) {
            /* the tag leaves the playlist: the sequence must account for it */
            hls->discontinuity_sequence++;
        }

        if (segments[0].name.data != NULL
            && ngx_media_hls_path(hls, &segments[0].name, &path, NULL)
               == NGX_OK)
        {
            (void) unlink((const char *) path.data);
            ngx_free(path.data);
        }

        hls->retained_bytes -= segments[0].bytes;
        hls->segments_evicted++;

        ngx_free(segments[0].name.data);
        segments[0].name.data = NULL;

        for (i = 1; i < hls->nsegments; i++) {
            segments[i - 1] = segments[i];
        }

        hls->nsegments--;

        ngx_memzero(&segments[hls->nsegments],
                    sizeof(ngx_media_hls_segment_t));
    }
}

static ngx_int_t
ngx_media_hls_write_file(ngx_media_hls_t *hls, const ngx_str_t *name)
{
    ngx_str_t   path, tmp;
    ngx_int_t   rc = NGX_ERROR;
    int         fd;
    ngx_uint_t  i;

    if (ngx_media_hls_path(hls, name, &path, NULL) != NGX_OK) {
        return NGX_ERROR;
    }

    /*
     * The file is written under a temporary name and renamed into place, the
     * way the playlist is.  A segment's final name is what a watcher reacts to
     * - an HLS push destination uploads the file it sees, and a viewer fetches
     * the name the playlist lists - and a reader that arrives while the file
     * is being written would otherwise get a truncated segment: the upload
     * would carry half a segment to the remote, and a player would fail on it.
     * rename() is atomic within a directory, so the name appears complete or
     * not at all.
     */
    tmp.len = path.len + sizeof(".tmp") - 1;
    tmp.data = ngx_alloc(tmp.len + 1, NULL);

    if (tmp.data == NULL) {
        ngx_free(path.data);
        return NGX_ERROR;
    }

    ngx_memcpy(tmp.data, path.data, path.len);
    ngx_memcpy(tmp.data + path.len, ".tmp", sizeof(".tmp") - 1);
    tmp.data[tmp.len] = '\0';

    fd = open((const char *) tmp.data, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd == -1) {
        ngx_free(tmp.data);
        ngx_free(path.data);
        return NGX_ERROR;
    }

    for (i = 0; i < hls->npieces; i++) {
        const u_char  *p = ngx_media_buf_data(hls->pieces[i].buf)
                           + hls->pieces[i].offset;
        size_t         left = hls->pieces[i].len;

        while (left > 0) {
            ssize_t  n = write(fd, p, left);

            if (n <= 0) {
                if (n == -1 && errno == EINTR) {
                    continue;
                }

                goto done;
            }

            p += n;
            left -= (size_t) n;
        }
    }

    if (close(fd) != 0) {
        fd = -1;
        goto done;
    }

    fd = -1;

    if (rename((const char *) tmp.data, (const char *) path.data) != 0) {
        goto done;
    }

    rc = NGX_OK;

done:

    if (rc != NGX_OK) {
        /* a failed segment leaves nothing behind, not even its temporary */
        (void) unlink((const char *) tmp.data);
    }

    if (fd != -1) {
        (void) close(fd);
    }

    ngx_free(tmp.data);
    ngx_free(path.data);

    return rc;
}

static ngx_int_t
ngx_media_hls_write_playlist(ngx_media_hls_t *hls)
{
    u_char       *buf, *p, *end;
    ngx_str_t     path, tmp;
    uint64_t      target;
    ngx_uint_t    i;
    int           fd;
    size_t        len, total;
    ssize_t       n;
    ngx_int_t     rc = NGX_ERROR;

    len = 256 + (size_t) hls->nsegments * 128;

    buf = ngx_alloc(len, NULL);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    p = buf;
    end = buf + len;

    /* EXT-X-TARGETDURATION is the maximum segment duration in seconds */
    target = (hls->conf.max_duration + 999) / 1000;

    if (target < (hls->conf.target_duration + 999) / 1000) {
        target = (hls->conf.target_duration + 999) / 1000;
    }

    p = ngx_media_hls_printf(p, end,
                             "#EXTM3U\n"
                             "#EXT-X-VERSION:3\n"
                             "#EXT-X-TARGETDURATION:%llu\n"
                             "#EXT-X-MEDIA-SEQUENCE:%llu\n"
                             "#EXT-X-DISCONTINUITY-SEQUENCE:%llu\n",
                             (unsigned long long) target,
                             (unsigned long long)
                                 (hls->nsegments ? hls->segments[0].sequence
                                                 : 0),
                             (unsigned long long) hls->discontinuity_sequence);

    for (i = 0; i < hls->nsegments; i++) {

        if (hls->segments[i].discontinuity) {
            p = ngx_media_hls_printf(p, end, "#EXT-X-DISCONTINUITY\n");
        }

        p = ngx_media_hls_printf(p, end, "#EXTINF:%llu.%03llu,\n%.*s\n",
                                 (unsigned long long)
                                     (hls->segments[i].duration / 1000),
                                 (unsigned long long)
                                     (hls->segments[i].duration % 1000),
                                 (int) hls->segments[i].name.len,
                                 (char *) hls->segments[i].name.data);
    }

    if (p >= end) {
        hls->errors++;
        ngx_free(buf);
        return NGX_ERROR;
    }

    total = (size_t) (p - buf);

    if (ngx_media_hls_path(hls, &hls->conf.playlist_name, &path, NULL)
        != NGX_OK)
    {
        ngx_free(buf);
        return NGX_ERROR;
    }

    tmp.len = path.len + sizeof(".tmp") - 1;
    tmp.data = ngx_alloc(tmp.len + 1, NULL);

    if (tmp.data == NULL) {
        ngx_free(path.data);
        ngx_free(buf);
        return NGX_ERROR;
    }

    p = tmp.data;
    ngx_memcpy(p, path.data, path.len);
    ngx_memcpy(p + path.len, ".tmp", sizeof(".tmp") - 1);
    p[tmp.len] = '\0';

    fd = open((const char *) tmp.data, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd == -1) {
        goto done;
    }

    n = write(fd, buf, total);

    if (n != (ssize_t) total) {
        (void) close(fd);
        goto done;
    }

    (void) close(fd);

    if (rename((const char *) tmp.data, (const char *) path.data) != 0) {
        goto done;
    }

    rc = NGX_OK;

done:

    ngx_free(tmp.data);
    ngx_free(path.data);
    ngx_free(buf);

    return rc;
}

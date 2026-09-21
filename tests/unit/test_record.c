#include "ngx_media_test.h"
#include "ngx_media_record.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define REC_DIR  ".build/record-test"
#define JOBS     2000
#define JOB_LEN  1024

static size_t
file_size(const char *path)
{
    struct stat  st;

    if (stat(path, &st) != 0) {
        return 0;
    }

    return (size_t) st.st_size;
}

static ngx_uint_t
read_file(const char *path, u_char *out, size_t cap)
{
    FILE      *fp;
    size_t     n;

    fp = fopen(path, "rb");

    if (fp == NULL) {
        return 0;
    }

    n = fread(out, 1, cap, fp);
    fclose(fp);

    return (ngx_uint_t) n;
}

static ngx_media_buf_t *
payload(ngx_uint_t index)
{
    ngx_media_buf_t  *buf;
    u_char           *p;
    ngx_uint_t        i;

    buf = ngx_media_buf_alloc(JOB_LEN);
    if (buf == NULL) {
        return NULL;
    }

    p = ngx_media_buf_data(buf);

    /* the first four bytes carry the job index so the written stream can be
     * verified block by block without assuming which jobs were accepted */
    p[0] = (u_char) (index & 0xFF);
    p[1] = (u_char) ((index >> 8) & 0xFF);
    p[2] = (u_char) ((index >> 16) & 0xFF);
    p[3] = (u_char) ((index >> 24) & 0xFF);

    for (i = 4; i < JOB_LEN; i++) {
        p[i] = (u_char) ((index + i) & 0xFF);
    }

    (void) ngx_media_buf_freeze(buf, JOB_LEN);

    return buf;
}

int
main(void)
{
    ngx_media_record_t         rec;
    ngx_media_record_conf_t    conf;
    ngx_media_record_stats_t   stats;
    ngx_media_buf_t           *buf;
    u_char                    *expected, *actual;
    ngx_uint_t                 i, part, off;
    char                       path[256];
    size_t                     size;
    uint64_t                   accepted, dropped;

    (void) mkdir(".build", 0755);
    (void) mkdir(REC_DIR, 0755);

    expected = malloc((size_t) JOBS * JOB_LEN);
    actual = malloc((size_t) JOBS * JOB_LEN);
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_NOT_NULL(actual);

    snprintf(path, sizeof(path), "%s/program.ts", REC_DIR);
    (void) unlink(path);
    for (i = 2; i < 512; i++) {
        char  part_path[256];
        snprintf(part_path, sizeof(part_path), "%s/program-%04lu.ts", REC_DIR,
                 (unsigned long) i);
        (void) unlink(part_path);
    }

    TEST_CASE("configuration defaults and validation");
    ngx_media_record_conf_default(&conf);
    TEST_ASSERT_EQ_U64(conf.tap, NGX_MEDIA_RECORD_PROGRAM);
    TEST_ASSERT(conf.max_part_bytes > 0);
    TEST_ASSERT_EQ_INT(ngx_media_record_init(NULL, NULL, NULL), NGX_ERROR);

    conf.path.data = (u_char *) path;
    conf.path.len = strlen(path);
    conf.tap = NGX_MEDIA_RECORD_PROGRAM;
    conf.max_part_bytes = 8 * JOB_LEN;         /* roll every 8 jobs */
    conf.max_pending_bytes = 256 * JOB_LEN;    /* bounded queue */
    conf.max_jobs = 256;

    TEST_CASE("init and append");
    TEST_ASSERT_EQ_INT(ngx_media_record_init(&rec, &conf, NULL), NGX_OK);

    for (i = 0; i < JOBS; i++) {
        int  rc;

        buf = payload(i);
        TEST_ASSERT_NOT_NULL(buf);

        rc = ngx_media_record_append(&rec, buf, 0, JOB_LEN);

        /* the queue is bounded: full means drop-and-count, never blocking */
        TEST_ASSERT(rc == NGX_OK || rc == NGX_AGAIN);

        /* a range that does not fit the buffer is always rejected */
        TEST_ASSERT_EQ_INT(ngx_media_record_append(&rec, buf, JOB_LEN, 1),
                           NGX_ERROR);

        ngx_media_buf_unref(buf);
    }

    TEST_CASE("stop drains the writer");
    ngx_media_record_stop(&rec);

    ngx_media_record_stats(&rec, &stats);

    accepted = stats.jobs;
    dropped = stats.jobs_dropped;

    printf("  record: jobs=%llu dropped=%llu bytes=%llu parts=%llu\n",
           (unsigned long long) stats.jobs,
           (unsigned long long) stats.jobs_dropped,
           (unsigned long long) stats.bytes_written,
           (unsigned long long) stats.parts);

    TEST_CASE("accounting is conserved and bounded");
    TEST_ASSERT_EQ_U64(accepted + dropped, JOBS);
    TEST_ASSERT_EQ_U64(stats.bytes_written, accepted * JOB_LEN);
    TEST_ASSERT_EQ_U64(stats.bytes_dropped, dropped * JOB_LEN);
    TEST_ASSERT_EQ_U64(stats.write_errors, 0);
    TEST_ASSERT_EQ_U64(stats.pending_jobs, 0);
    TEST_ASSERT(accepted > 0);
    TEST_ASSERT(stats.parts >= 2);             /* the ceiling rolled parts */

    TEST_CASE("the written stream is the accepted jobs in order");
    {
        ngx_uint_t  nparts = 0;
        uint32_t    previous = 0;
        int64_t     last_index = -1;

        off = 0;

        for (part = 1; part <= stats.parts; part++) {

            if (part == 1) {
                snprintf(path, sizeof(path), "%s/program.ts", REC_DIR);

            } else {
                snprintf(path, sizeof(path), "%s/program-%04lu.ts", REC_DIR,
                         (unsigned long) part);
            }

            size = file_size(path);

            if (size == 0) {
                continue;                      /* never written to */
            }

            TEST_ASSERT(size <= conf.max_part_bytes);
            TEST_ASSERT_EQ_U64(size % JOB_LEN, 0);
            TEST_ASSERT_EQ_U64(read_file(path, actual + off, size), size);

            off += size;
            nparts++;
        }

        TEST_ASSERT_EQ_U64(off, stats.bytes_written);
        TEST_ASSERT(nparts >= 2);

        /* every 1 KiB block is a complete job and the indices increase */
        for (i = 0; i < off / JOB_LEN; i++) {
            const u_char  *block = actual + (size_t) i * JOB_LEN;
            uint32_t       index;
            ngx_uint_t     j;

            index = (uint32_t) block[0]
                    | ((uint32_t) block[1] << 8)
                    | ((uint32_t) block[2] << 16)
                    | ((uint32_t) block[3] << 24);

            TEST_ASSERT((int64_t) index > last_index);
            last_index = (int64_t) index;

            for (j = 4; j < JOB_LEN; j++) {
                if (block[j] != (u_char) ((index + j) & 0xFF)) {
                    TEST_FAIL("job %u corrupted at byte %lu",
                              (unsigned) index, (unsigned long) j);
                    break;
                }
            }

            previous = index;
            (void) previous;
        }
    }

    TEST_CASE("validation and NULL tolerance");
    ngx_media_record_stats(NULL, &stats);
    TEST_ASSERT_EQ_U64(stats.bytes_written, 0);
    ngx_media_record_stats(&rec, NULL);
    ngx_media_record_stop(NULL);
    ngx_media_record_conf_default(NULL);

    /* stopping twice is harmless: no descriptor or buffer leaks */
    ngx_media_record_stop(&rec);

    free(expected);
    free(actual);

    TEST_LEAKS();

    TEST_MAIN_END();
}

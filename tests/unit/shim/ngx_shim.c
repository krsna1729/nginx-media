#include "ngx_shim.h"

#include <stdlib.h>

size_t ngx_media_test_allocs;
size_t ngx_media_test_frees;

void *
ngx_media_test_alloc(size_t size, ngx_log_t *log)
{
    void  *p;

    (void) log;

    p = malloc(size);

    if (p != NULL) {
        ngx_media_test_allocs++;
    }

    return p;
}

void
ngx_media_test_free(void *ptr)
{
    if (ptr != NULL) {
        ngx_media_test_frees++;
    }

    free(ptr);
}

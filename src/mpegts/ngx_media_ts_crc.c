#include "ngx_media_ts_crc.h"

uint32_t
ngx_media_ts_crc32(const u_char *p, size_t len)
{
    uint32_t  crc = 0xFFFFFFFFu;
    size_t    i;
    int       b;

    for (i = 0; i < len; i++) {
        crc ^= (uint32_t) p[i] << 24;

        for (b = 0; b < 8; b++) {
            if (crc & 0x80000000u) {
                crc = (crc << 1) ^ 0x04C11DB7u;

            } else {
                crc = crc << 1;
            }
        }
    }

    return crc;
}

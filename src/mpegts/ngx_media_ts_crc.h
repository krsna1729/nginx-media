#ifndef NGX_MEDIA_TS_CRC_H
#define NGX_MEDIA_TS_CRC_H

#include "ngx_media_platform.h"

/*
 * MPEG-2 systems CRC-32: polynomial 0x04C11DB7, initial value all ones, no
 * final xor, MSB first.  A section that appends its own CRC over this
 * function yields zero, which is how the demuxer validates PSI sections.
 */
uint32_t ngx_media_ts_crc32(const u_char *p, size_t len);

#endif /* NGX_MEDIA_TS_CRC_H */

#!/usr/bin/env bash
# Run after the production nginx tree is configured: bash tests/integration/route_lifecycle.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX_SRC="${NGINX_SRC:-$ROOT/.build/nginx-1.30.5}"
OUT="${ROUTE_LIFECYCLE_BIN:-$ROOT/.build/route-lifecycle}"
if [ ! -f "$NGINX_SRC/objs/ngx_auto_config.h" ]; then
    echo "configured nginx headers missing: $NGINX_SRC; run make nginx first" >&2
    exit 1
fi
mkdir -p "$(dirname "$OUT")"
"${CC:-cc}" -O1 -g -std=gnu2x -Wall -Wextra -Wno-unused-parameter -Werror \
    -ffunction-sections -fdata-sections \
    ${CFLAGS:-} \
    -I"$NGINX_SRC/src/core" -I"$NGINX_SRC/src/event" \
    -I"$NGINX_SRC/src/event/modules" -I"$NGINX_SRC/src/os/unix" \
    -I"$NGINX_SRC/objs" -I"$ROOT/src/core" -I"$ROOT/src/codec" \
    -I"$ROOT/src/mpegts" -I"$ROOT/src/srt" -I"$ROOT/src/hls" \
    -I"$ROOT/src/record" -I"$ROOT/src/rtmp" \
    "$ROOT/tests/integration/route_lifecycle.c" \
    "$ROOT/src/core/ngx_media_owner.c" \
    "$NGINX_SRC/src/core/ngx_palloc.c" "$NGINX_SRC/src/os/unix/ngx_alloc.c" \
    "$NGINX_SRC/src/core/ngx_log.c" "$NGINX_SRC/src/core/ngx_string.c" \
    "$NGINX_SRC/src/core/ngx_times.c" "$NGINX_SRC/src/os/unix/ngx_time.c" \
    "$NGINX_SRC/src/os/unix/ngx_errno.c" "$NGINX_SRC/src/os/unix/ngx_thread_id.c" \
    -Wl,--gc-sections ${LDFLAGS:-} -lpthread -o "$OUT"
"$OUT"

#!/usr/bin/env bash
#
# How HLS segments should be served and where they should live.
#
# The question this answers: does a memory filesystem help, and does sendfile
# (including the kTLS zero-copy path) help.  Measured rather than assumed.
#
# Variants:
#   disk            the current setup: segments on disk, sendfile off
#   disk+sendfile   same files, sendfile on with tcp_nopush
#   tmpfs+sendfile  segments on a tmpfs mount, sendfile on
#   tmpfs+tls       segments on tmpfs, HTTPS with kTLS and sendfile on
#
# Each variant is measured with the same number of sequential requests for the
# same segment, so the numbers are comparable; the report gives MiB/s and the
# per-request cost.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/bench-hls"
SEG_MB="${SEG_MB:-8}"
REQUESTS="${REQUESTS:-400}"
PORT=18460
TLS_PORT=18461
TMPFS="$RUN/tmpfs"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/disk" "$TMPFS"

cleanup() {
    pkill -KILL -f "$RUN/conf/nginx.conf" 2>/dev/null
    sudo -n umount "$TMPFS" 2>/dev/null || umount "$TMPFS" 2>/dev/null
    return 0
}
trap cleanup EXIT

echo "== building a ${SEG_MB} MiB segment"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=1280x720:rate=25" -t "$(( SEG_MB / 2 ))" \
    -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p \
    -f mpegts "$RUN/disk/seg.ts" 2>/dev/null \
    || { echo "could not build the fixture" >&2; exit 1; }

cp "$RUN/disk/seg.ts" "$TMPFS/seg.ts"

SIZE=$(stat -c%s "$RUN/disk/seg.ts")
echo "   segment is $(( SIZE / 1024 )) KiB"

# a tmpfs for the segments; sudo -n keeps this non-interactive, and the mount
# is torn down in cleanup()
if sudo -n mount -t tmpfs -o size=64m tmpfs "$TMPFS" 2>/dev/null; then
    TMPFS_OK=1
elif mount -t tmpfs -o size=64m tmpfs "$TMPFS" 2>/dev/null; then
    TMPFS_OK=1
else
    echo "   NOTE: could not mount tmpfs; tmpfs variants skipped"
    TMPFS_OK=0
fi

cp "$RUN/disk/seg.ts" "$TMPFS/seg.ts"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log warn;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

http {
    access_log off;

    server {
        listen 127.0.0.1:$PORT;

        location /disk/ {
            alias $RUN/disk/;
        }

        location /tmpfs/ {
            alias $TMPFS/;
        }
    }

    server {
        listen 127.0.0.1:$TLS_PORT ssl;
        ssl_certificate $RUN/cert.pem;
        ssl_certificate_key $RUN/key.pem;
        ssl_conf_command Options KTLS;

        location /tmpfs/ {
            alias $TMPFS/;
        }
    }
}
EOF

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$RUN/key.pem" -out "$RUN/cert.pem" \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" \
    >/dev/null 2>&1

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 \
    || { echo "configuration rejected" >&2; exit 1; }

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

measure() {
    # $1: label, $2: url, $3: extra curl args
    local label="$1" url="$2" extra="${3:-}"
    local start end elapsed

    # warm the cache and the connection path
    curl -fsS $extra -o /dev/null "$url" || return 1

    start=$(date +%s%N)

    for _ in $(seq 1 "$REQUESTS"); do
        curl -fsS $extra -o /dev/null "$url" || return 1
    done

    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))

    awk -v label="$label" -v ms="$elapsed" -v req="$REQUESTS" \
        -v bytes="$SIZE" 'BEGIN {
            total = req * bytes
            secs = ms / 1000
            printf "   %-16s %6d ms total  %7.1f MiB/s  %6.2f ms/request\n",
                   label, ms, (total / 1048576) / secs, ms / req
        }'
}

echo "== serving $REQUESTS requests per variant (segment: $(( SIZE / 1024 )) KiB)"

measure "disk"          "http://127.0.0.1:$PORT/disk/seg.ts"

# sendfile is a per-location directive; rewrite and reload for each variant
with_sendfile() {
    python3 - "$RUN/conf/nginx.conf" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
if "sendfile on" not in s:
    s = s.replace("    access_log off;",
                  "    access_log off;\n    sendfile on;\n    tcp_nopush on;")
    open(p, "w").write(s)
PY
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s reload >/dev/null 2>&1
    sleep 0.5
}

with_sendfile

measure "disk+sendfile" "http://127.0.0.1:$PORT/disk/seg.ts"

if [ "$TMPFS_OK" = "1" ]; then
    measure "tmpfs+sendfile" "http://127.0.0.1:$PORT/tmpfs/seg.ts"

    measure "tmpfs+tls+ktls" "https://127.0.0.1:$TLS_PORT/tmpfs/seg.ts" \
        "--cacert $RUN/cert.pem"
else
    echo "   tmpfs variants skipped (no mount privilege)"
fi

# Cold cache: this is where sendfile is supposed to earn its place, because
# it hands page-cache pages straight to the socket instead of copying them
# through user space.  Dropping caches needs root.
drop_caches() {
    sync
    if sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null; then
        return 0
    fi
    return 1
}

if [ "$TMPFS_OK" = "1" ]; then
    echo "== cold cache (page cache dropped before each variant)"
    if drop_caches; then
        measure "disk cold" "http://127.0.0.1:$PORT/disk/seg.ts"
    else
        echo "   NOTE: could not drop caches; cold variants skipped"
    fi
fi

echo "== page cache state"
free -m | head -2

trap - EXIT
cleanup

echo "== hls serve bench done"

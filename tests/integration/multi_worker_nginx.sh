#!/usr/bin/env bash
#
# Multi-worker ownership and routing (phase 8 exit criteria).
#
# Two worker processes.  The transports accept on worker 0 (transport socket
# ownership may differ from program ownership, goal doc 22), while each program
# is owned by the worker its application/stream hash selects.  A publisher
# whose stream belongs to worker 1 must therefore be routed across the bounded
# inter-worker transport and still produce a complete program: the worker log
# shows the routing, and the HLS output proves the media arrived.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/multi-worker"
BASE=$(( 19600 + ($$ % 100) * 4 ))
SRT_PORT="${MW_SRT_PORT:-$BASE}"
HTTP_PORT="${MW_HTTP_PORT:-$(( BASE + 1 ))}"
PUB=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf" "$RUN/hls"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 2;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/hls;

media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /media/api/ {
            media_api;
        }
    }
}
EOF

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx with two workers"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

grep -q 'start worker process' "$RUN/logs/error.log" \
    || { echo "no workers started" >&2; exit 1; }

WORKERS="$(grep -c 'start worker process' "$RUN/logs/error.log" || true)"
[ "$WORKERS" -ge 2 ] || { echo "expected two workers, saw $WORKERS" >&2; exit 1; }

grep -q 'routing socket pairs created for 2 workers' "$RUN/logs/error.log" \
    || { echo "routing pairs were not created" >&2; exit 1; }

grep -q 'worker 1 routing ready' "$RUN/logs/error.log" \
    || { echo "worker 1 did not adopt its routing endpoints" >&2; exit 1; }

# find a stream name whose owner is worker 1
OWNER1=""
for i in $(seq 1 40); do
    NAME="news$i"
    HASH="$(python3 - "$NAME" <<'PY'
import sys
h = 2166136261
for b in b"live/" + sys.argv[1].encode():
    h ^= b
    h = (h * 16777619) & 0xffffffff
print(h % 2)
PY
)"
    if [ "$HASH" = "1" ]; then
        OWNER1="$NAME"
        break
    fi
done

[ -n "$OWNER1" ] || { echo "no stream maps to worker 1" >&2; exit 1; }

echo "== publishing live/$OWNER1, owned by worker 1"
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 96k \
    -t 10 -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/$OWNER1,m=publish,s=encoder-a" \
    >"$RUN/pub.log" 2>&1 &
PUB=$!

for _ in $(seq 1 200); do
    grep -q "srt publisher routed to the owner stream=live/$OWNER1" \
        "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.1
done

grep -q "srt publisher routed to the owner stream=live/$OWNER1" \
    "$RUN/logs/error.log" \
    || { echo "the publisher was not routed to its owner" >&2; exit 1; }

grep -q "routed source opened stream=live/$OWNER1" "$RUN/logs/error.log" \
    || { echo "the owner did not open the routed source" >&2; exit 1; }

for _ in $(seq 1 300); do
    [ -f "$RUN/hls/index.m3u8" ] \
        && [ "$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" || true)" -ge 1 ] \
        && break
    sleep 0.1
done

grep -q '^#EXTM3U' "$RUN/hls/index.m3u8" \
    || { echo "the routed program produced no hls output" >&2; exit 1; }

SEGMENT="$(grep '\.ts$' "$RUN/hls/index.m3u8" | head -1)"
[ -n "$SEGMENT" ] || { echo "no segment was written" >&2; exit 1; }

PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
    stream=codec_name,codec_type -of csv "$RUN/hls/$SEGMENT" 2>&1)"

printf '%s\n' "$PROBE"

printf '%s' "$PROBE" | grep -q ',h264,video' \
    || { echo "the routed segment has no H.264" >&2; exit 1; }
printf '%s' "$PROBE" | grep -q ',aac,audio' \
    || { echo "the routed segment has no AAC" >&2; exit 1; }

# the program is owned by worker 1, so its outputs were driven there
grep -q 'worker 1 routing ready' "$RUN/logs/error.log" \
    || { echo "worker 1 was not serving" >&2; exit 1; }

echo "== stop"
kill -KILL "$PUB" 2>/dev/null || true
PUB=0

sleep 1

PID="$(cat "$RUN/logs/nginx.pid")"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit
trap - EXIT

for _ in $(seq 1 400); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.05
done

if kill -0 "$PID" 2>/dev/null; then
    echo "nginx did not shut down" >&2
    kill -9 "$PID" || true
    exit 1
fi

EXITED="$(grep -c 'worker process .* exited with code 0' "$RUN/logs/error.log" || true)"
[ "$EXITED" -ge 2 ] || { echo "workers did not exit cleanly: $EXITED" >&2; exit 1; }

echo "== multi worker ok"

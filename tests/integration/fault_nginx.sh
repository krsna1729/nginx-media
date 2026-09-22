#!/usr/bin/env bash
#
# Fault injection: the engine must not crash when inputs misbehave mid-stream.
#
# Two publishers feed the same program.  Source A is stalled (SIGSTOP) and then
# resumed, source A is replaced by a publisher whose transport bytes are
# corrupted, and source B is killed outright.  Through all of it the worker must
# stay up, keep failing over through the normal path, keep producing HLS and
# still shut down cleanly.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/fault"
BASE=$(( 19800 + ($$ % 80) * 4 ))
SRT_PORT="${FAULT_SRT_PORT:-$BASE}"
HTTP_PORT="${FAULT_HTTP_PORT:-$(( BASE + 1 ))}"
PUB_A=0
PUB_B=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

cleanup() {
    [ "$PUB_A" != "0" ] && kill -KILL "$PUB_A" 2>/dev/null || true
    [ "$PUB_B" != "0" ] && kill -KILL "$PUB_B" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf" "$RUN/hls"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_failover_failure_timeout 800;
media_failover_recovery_timeout 300;

media_hls $RUN/hls;

media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;
media_srt_source_priority encoder-b 90;

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

publish() {
    local source="$1" seconds="$2"
    local pid

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -f lavfi -i "sine=frequency=$3:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$seconds" -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/fault,m=publish,s=$source" \
        >>"$RUN/pub-$source.log" 2>&1 &

    pid=$!
    echo "$pid"
}

# a publisher whose transport bytes are corrupted on the way out
publish_corrupt() {
    local seconds="$1"

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -t "$seconds" -f mpegts - 2>>"$RUN/pub-corrupt.log" \
    | python3 -c '
import sys, random
random.seed(7)
try:
    while True:
        chunk = sys.stdin.buffer.read(4096)
        if not chunk:
            break
        data = bytearray(chunk)
        for _ in range(24):
            data[random.randrange(len(data))] ^= 0xFF
        sys.stdout.buffer.write(bytes(data))
except (BrokenPipeError, ValueError):
    pass
' \
    | ffmpeg -hide_banner -loglevel error -f mpegts -i pipe:0 -c copy \
        -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/fault,m=publish,s=encoder-a" \
        >>"$RUN/pub-corrupt.log" 2>&1 &
}

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

echo "== two publishers on live/fault"
PUB_A="$(publish encoder-a 20 440)"
sleep 1
PUB_B="$(publish encoder-b 18 660)"

for _ in $(seq 1 200); do
    grep -q 'media: srt program stream=live/fault' "$RUN/logs/error.log" \
        2>/dev/null && break
    sleep 0.05
done

echo "== stall source a"
kill -STOP "$PUB_A"
sleep 2

ACTIVE="$(curl -fsS "http://127.0.0.1:$HTTP_PORT/media/api/v1/streams" 2>/dev/null \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["streams"][0]["active"])' 2>/dev/null || echo "?")"
echo "   active after the stall: $ACTIVE"

[ "$ACTIVE" = "encoder-b" ] \
    || { echo "the program did not fail over from the stalled source" >&2; exit 1; }

echo "== resume source a"
kill -CONT "$PUB_A"
sleep 3

echo "== corrupt source a"
kill -KILL "$PUB_A" 2>/dev/null || true
PUB_A=0
publish_corrupt 8

sleep 4

# the corrupt publisher must not disturb the program: b is still serving
ACTIVE="$(curl -fsS "http://127.0.0.1:$HTTP_PORT/media/api/v1/streams" 2>/dev/null \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["streams"][0]["active"])' 2>/dev/null || echo "?")"
echo "   active with corrupt a: $ACTIVE"

[ "$ACTIVE" = "encoder-b" ] \
    || { echo "the program was disturbed by a corrupt source" >&2; exit 1; }

echo "== kill source b"
kill -KILL "$PUB_B" 2>/dev/null || true
PUB_B=0

sleep 3

PID="$(pgrep -f 'nginx: worker process' 2>/dev/null | head -1 || true)"
[ -n "$PID" ] || { echo "the worker died during the fault injection" >&2; exit 1; }

if grep -aqE 'signal [0-9]+ \(core dumped\)|exited on signal' "$RUN/logs/error.log"; then
    echo "the worker crashed" >&2
    grep -aE 'signal|alert' "$RUN/logs/error.log" | tail -5 >&2
    exit 1
fi

SEGMENTS="$(grep -c '^#EXTINF' "$RUN/hls/live/fault/index.m3u8" 2>/dev/null || echo 0)"
echo "   hls segments after the faults: $SEGMENTS"
[ "$SEGMENTS" -ge 2 ] || { echo "hls stopped producing during the faults" >&2; exit 1; }

grep -aq 'continuity_errors' "$RUN/logs/error.log" \
    || { echo "no demux statistics were reported" >&2; exit 1; }

echo "== stop"
[ "$PUB_A" != "0" ] && kill -KILL "$PUB_A" 2>/dev/null || true
[ "$PUB_B" != "0" ] && kill -KILL "$PUB_B" 2>/dev/null || true
PUB_A=0
PUB_B=0
pkill -KILL -f "mode=caller&streamid=#!::r=live/fault" 2>/dev/null || true

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

grep -aq 'exited with code 0' "$RUN/logs/error.log" \
    || { echo "worker did not exit cleanly" >&2; exit 1; }

echo "== fault injection ok"

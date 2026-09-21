#!/usr/bin/env bash
#
# HLS and recording through nginx (phase 5 exit criteria).
#
# A publisher feeds live/news over SRT; the worker muxes the program into
# bursts and produces an HLS playlist with segments, a PROGRAM recording, a RAW
# transport recording and an ISO recording of one named source.  The publisher
# is killed and restarted mid-run so the playlist must announce the
# discontinuity.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/hls-nginx"
SRT_PORT="${HLS_SRT_PORT:-19046}"
HTTP_PORT="${HLS_HTTP_PORT:-18446}"
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
STREAMID='%23!::r%3Dlive%2Fnews%2Cm%3Dpublish%2Cs%3D'
PUB_A=0
PUB_PID=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf" "$RUN/hls" "$RUN/rec"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_failover_failure_timeout 700;
media_failover_recovery_timeout 300;
media_failover_switchback auto;

media_hls $RUN/hls;
media_record $RUN/rec/program.ts;
media_record_raw $RUN/rec/raw.ts;
media_record_iso encoder-a $RUN/rec/iso-a.ts;

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

        location /hls/ {
            alias $RUN/hls/;
        }

        location /rec/ {
            alias $RUN/rec/;
        }
    }
}
EOF

cleanup() {
    [ "$PUB_A" != "0" ] && kill -KILL "$PUB_A" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

start_publisher() {
    local seconds="$1"

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$seconds" -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=$STREAMID""encoder-a" \
        >"$RUN/pub.log" 2>&1 &

    PUB_PID=$!
}

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

grep -q 'srt listener ready' "$RUN/logs/error.log" \
    || { echo "listener not ready" >&2; exit 1; }

echo "== publishing for 16s (with a restart at 6s)"
start_publisher 16
PUB_A="$PUB_PID"

sleep 6

kill -KILL "$PUB_A"
PUB_A=0
sleep 1

start_publisher 10
PUB_A="$PUB_PID"

# the switch that follows the restart must be announced in the playlist
for _ in $(seq 1 200); do
    grep -q '^#EXT-X-DISCONTINUITY' "$RUN/hls/index.m3u8" 2>/dev/null && break
    sleep 0.1
done

# and at least two segments must be closed by now
for _ in $(seq 1 100); do
    [ "$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" 2>/dev/null || true)" -ge 2 ] && break
    sleep 0.1
done

echo "== playlist"
cat "$RUN/hls/index.m3u8"

grep -q '^#EXTM3U' "$RUN/hls/index.m3u8" || { echo "no playlist header" >&2; exit 1; }

SEGMENTS="$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" || true)"
[ "$SEGMENTS" -ge 2 ] || { echo "too few segments: $SEGMENTS" >&2; exit 1; }

grep -q '^#EXT-X-DISCONTINUITY' "$RUN/hls/index.m3u8" \
    || { echo "no discontinuity after the restart" >&2; exit 1; }

SEGMENT_NAME="$(grep '\.ts$' "$RUN/hls/index.m3u8" | tail -1)"

echo "== serving $SEGMENT_NAME over HTTP"
curl -fsS "http://127.0.0.1:$HTTP_PORT/hls/$SEGMENT_NAME" -o "$RUN/fetched.ts"

SHA_DISK="$(sha256sum "$RUN/hls/$SEGMENT_NAME" | cut -d' ' -f1)"
SHA_HTTP="$(sha256sum "$RUN/fetched.ts" | cut -d' ' -f1)"
[ "$SHA_DISK" = "$SHA_HTTP" ] \
    || { echo "served segment differs from the file on disk" >&2; exit 1; }

echo "== probing the served segment"
PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
    stream=codec_name,codec_type -show_entries format=duration -of csv \
    "$RUN/fetched.ts" 2>&1)"

printf '%s\n' "$PROBE"

printf '%s' "$PROBE" | grep -q ',h264,video' || { echo "segment has no H.264" >&2; exit 1; }
printf '%s' "$PROBE" | grep -q ',aac,audio' || { echo "segment has no AAC" >&2; exit 1; }

echo "== recordings"
for f in "$RUN/rec/program.ts" "$RUN/rec/iso-a.ts"; do
    [ -s "$f" ] || { echo "$f is missing" >&2; exit 1; }

    REC_PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
        stream=codec_name,codec_type -of csv "$f" 2>&1)"

    printf '%s\n' "$REC_PROBE"

    printf '%s' "$REC_PROBE" | grep -q ',h264,video' \
        || { echo "$f has no H.264" >&2; exit 1; }

    printf '%s' "$REC_PROBE" | grep -q ',aac,audio' \
        || { echo "$f has no AAC" >&2; exit 1; }
done

[ -s "$RUN/rec/raw.ts" ] || { echo "raw recording is missing" >&2; exit 1; }

RAW_SIZE="$(stat -c %s "$RUN/rec/raw.ts")"
[ "$RAW_SIZE" -gt 100000 ] || { echo "raw recording too small: $RAW_SIZE" >&2; exit 1; }

PROGRAM_SIZE="$(stat -c %s "$RUN/rec/program.ts")"
echo "   program=$PROGRAM_SIZE bytes raw=$RAW_SIZE bytes segments=$SEGMENTS"

echo "== stop"
kill -KILL "$PUB_A" 2>/dev/null || true
PUB_A=0

sleep 1

PID="$(cat "$RUN/logs/nginx.pid")"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit
trap - EXIT

for _ in $(seq 1 100); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.05
done

if kill -0 "$PID" 2>/dev/null; then
    echo "nginx did not shut down" >&2
    kill -9 "$PID" || true
    exit 1
fi

grep -q 'exited with code 0' "$RUN/logs/error.log" \
    || { echo "worker did not exit cleanly" >&2; exit 1; }

if grep -q 'open socket' "$RUN/logs/error.log"; then
    echo "worker left a socket registered at shutdown" >&2
    exit 1
fi

echo "== hls and recording ok"

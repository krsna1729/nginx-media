#!/usr/bin/env bash
#
# RTMP through nginx (phase 6 exit criteria).
#
# An ffmpeg publisher feeds live/news over RTMP; the worker registers it as a
# source of the same logical stream the SRT path uses, so the program reaches
# HLS and the control API.  A second ffmpeg then *plays* the program back over
# RTMP and must decode video and audio.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/rtmp-nginx"
RTMP_PORT="${RTMP_PORT:-19350}"
HTTP_PORT="${RTMP_HTTP_PORT:-18350}"
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
PUB_PID=0
PLAY_PID=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

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

media_hls $RUN/hls;

media_rtmp_listen 127.0.0.1:$RTMP_PORT;
media_rtmp_source_priority news 100;

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
    }
}
EOF

cleanup() {
    [ "$PUB_PID" != "0" ] && kill -KILL "$PUB_PID" 2>/dev/null || true
    [ "$PLAY_PID" != "0" ] && kill -KILL "$PLAY_PID" 2>/dev/null || true
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
        -t "$seconds" -f flv "rtmp://127.0.0.1:$RTMP_PORT/live/news" \
        >"$RUN/pub.log" 2>&1 &

    PUB_PID=$!
}

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'rtmp listener ready' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

grep -q 'rtmp listener ready' "$RUN/logs/error.log" \
    || { echo "rtmp listener not ready" >&2; exit 1; }

echo "== publishing over rtmp for 14s"
start_publisher 14

# the publisher must appear as a source of live/news
for _ in $(seq 1 200); do
    grep -q 'media: rtmp publisher stream=live/news' "$RUN/logs/error.log" \
        2>/dev/null && break
    sleep 0.05
done

grep -q 'media: rtmp publisher stream=live/news' "$RUN/logs/error.log" \
    || { echo "publisher was not registered" >&2; exit 1; }

for _ in $(seq 1 200); do
    curl -fsS "$API/streams" 2>/dev/null | grep -q '"type":2' && break
    sleep 0.05
done

# the program must actually carry frames before the outputs are asserted
for _ in $(seq 1 200); do
    curl -fsS "$API/streams" 2>/dev/null \
        | grep -qE '"program_frames":[1-9]' && break
    sleep 0.1
done

echo "== control api"
STREAMS="$(curl -fsS "$API/streams")"
printf '%s\n' "$STREAMS"

printf '%s' "$STREAMS" | grep -q '"application":"live"' \
    || { echo "stream not listed" >&2; exit 1; }
printf '%s' "$STREAMS" | grep -q '"name":"news"' \
    || { echo "stream name wrong" >&2; exit 1; }
printf '%s' "$STREAMS" | grep -q '"type":2' \
    || { echo "rtmp source not listed" >&2; exit 1; }
printf '%s' "$STREAMS" | grep -q '"priority":100' \
    || { echo "configured priority not applied" >&2; exit 1; }
printf '%s' "$STREAMS" | grep -qE '"program_frames":[1-9]' \
    || { echo "no program frames from the rtmp source" >&2; exit 1; }

# the program must reach the outputs: an HLS playlist appears
for _ in $(seq 1 200); do
    [ -f "$RUN/hls/index.m3u8" ] \
        && [ "$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" || true)" -ge 1 ] \
        && break
    sleep 0.1
done

grep -q '^#EXTM3U' "$RUN/hls/index.m3u8" \
    || { echo "no hls output from the rtmp source" >&2; exit 1; }

echo "== playing the program back over rtmp"
ffmpeg -hide_banner -loglevel error -y \
    -i "rtmp://127.0.0.1:$RTMP_PORT/live/news" \
    -t 4 -c copy "$RUN/played.flv" >"$RUN/play.log" 2>&1 &

PLAY_PID=$!
wait "$PLAY_PID" || { cat "$RUN/play.log"; echo "playback failed" >&2; exit 1; }
PLAY_PID=0

PLAY_PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
    stream=codec_name,codec_type -of csv "$RUN/played.flv" 2>&1)"

printf '%s\n' "$PLAY_PROBE"

printf '%s' "$PLAY_PROBE" | grep -q ',h264,video' \
    || { echo "played stream has no H.264" >&2; exit 1; }
printf '%s' "$PLAY_PROBE" | grep -q ',aac,audio' \
    || { echo "played stream has no AAC" >&2; exit 1; }

FRAMES="$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries packet=pts_time -of csv=p=0 "$RUN/played.flv" 2>/dev/null \
    | wc -l)"

echo "   played $FRAMES video packets from the rtmp program"
[ "$FRAMES" -ge 20 ] || { echo "too few video packets: $FRAMES" >&2; exit 1; }

# the worker must announce both the publish and the play session
grep -q 'media: rtmp player stream=live/news' "$RUN/logs/error.log" \
    || { echo "play session was not served" >&2; exit 1; }

echo "== stop"
kill -KILL "$PUB_PID" 2>/dev/null || true
PUB_PID=0

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

echo "== rtmp ok"

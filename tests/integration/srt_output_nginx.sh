#!/usr/bin/env bash
#
# SRT output and fanout (phase 7 exit criteria).
#
# Two nginx instances: A ingests an SRT publisher and prepares the program
# transport once, then pushes it to two SRT destinations.  B is the receiver
# of one of them and must produce a decodable program of its own, which proves
# the output path end to end (including the stream id the caller announces)
# while HLS on A proves the preparation is shared, not repeated per
# destination.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/srt-output"
# unique ports per run: back-to-back runs cannot collide on SRT sockets
BASE=$(( 19200 + ($$ % 200) * 4 ))
IN_PORT="${SRT_OUT_IN_PORT:-$BASE}"
OUT_A="${SRT_OUT_A_PORT:-$(( BASE + 1 ))}"
OUT_B="${SRT_OUT_B_PORT:-$(( BASE + 2 ))}"
PUB=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null || true
    "$NGINX" -p "$RUN/a" -c conf/nginx.conf -s quit 2>/dev/null || true
    "$NGINX" -p "$RUN/b" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$RUN"
mkdir -p "$RUN/a/logs" "$RUN/a/conf" "$RUN/a/hls" \
         "$RUN/b/logs" "$RUN/b/conf" "$RUN/b/hls"

# A: SRT ingest, the program preparation, two SRT destinations and HLS
cat > "$RUN/a/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/a/hls;

media_srt_listen 127.0.0.1:$IN_PORT;
media_srt_source_priority encoder-a 100;
media_srt_output live/news 127.0.0.1:$OUT_A "#!::r=live/news,m=publish,s=srt-out-a";
media_srt_output live/news 127.0.0.1:$OUT_B "#!::r=live/news,m=publish,s=srt-out-b";
EOF

# B: receives one destination and serves its own program as HLS
cat > "$RUN/b/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/b/hls;

media_srt_listen 127.0.0.1:$OUT_A;
media_srt_source_priority srt-out-a 100;
EOF

echo "== config test"
"$NGINX" -p "$RUN/a" -c conf/nginx.conf -t
"$NGINX" -p "$RUN/b" -c conf/nginx.conf -t

echo "== starting nginx (sink first)"
"$NGINX" -p "$RUN/b" -c conf/nginx.conf
"$NGINX" -p "$RUN/a" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$RUN/a/logs/error.log" 2>/dev/null \
        && grep -q 'srt listener ready' "$RUN/b/logs/error.log" 2>/dev/null \
        && break
    sleep 0.05
done

grep -q '2 SRT destination(s) started' "$RUN/a/logs/error.log" \
    || { echo "destinations were not started" >&2; exit 1; }

# The senders are one pool over the destination table, so once the ingest
# session and both destinations are up the worker's thread count is fixed:
# media flowing for ten seconds must not add a thread.  This is the
# integration half of goal doc 34 item 16 - a sender per burst or per push
# shows up here as a count that keeps climbing.  (The SRT backend creates one
# thread per socket of its own, so the baseline is taken after the sockets
# exist rather than before.)
MASTER_A="$(cat "$RUN/a/logs/nginx.pid")"
WORKER_A="$(pgrep -P "$MASTER_A" | head -1)"

[ -n "$WORKER_A" ] || { echo "no worker under the master" >&2; exit 1; }

echo "== publishing over srt for 10s"
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 96k \
    -t 10 -f mpegts \
    "srt://127.0.0.1:$IN_PORT?mode=caller&streamid=%23!::r%3Dlive%2Fnews%2Cm%3Dpublish%2Cs%3Dencoder-a" \
    >"$RUN/pub.log" 2>&1 &
PUB=$!

# the receiver of the first destination must register the source the caller
# announced
for _ in $(seq 1 200); do
    grep -q 'media: srt source open app=live stream=news source=srt-out-a' \
        "$RUN/b/logs/error.log" 2>/dev/null && break
    sleep 0.1
done

grep -q 'media: srt source open app=live stream=news source=srt-out-a' \
    "$RUN/b/logs/error.log" \
    || { echo "the srt output never reached its destination" >&2; exit 1; }

# and it must carry media: a closed segment appears at the first keyframe
# boundary after the segmenter's minimum duration
for _ in $(seq 1 300); do
    [ -f "$RUN/b/hls/index.m3u8" ] \
        && [ "$(grep -c '^#EXTINF' "$RUN/b/hls/index.m3u8" || true)" -ge 1 ] \
        && break
    sleep 0.1
done

grep -q '^#EXTM3U' "$RUN/b/hls/index.m3u8" \
    || { echo "the destination produced no hls playlist" >&2; exit 1; }

SEGMENT="$(grep '\.ts$' "$RUN/b/hls/index.m3u8" | head -1)"
[ -n "$SEGMENT" ] || { echo "the destination produced no segment" >&2; exit 1; }

PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
    stream=codec_name,codec_type -of csv "$RUN/b/hls/$SEGMENT" 2>&1)"

printf '%s\n' "$PROBE"

printf '%s' "$PROBE" | grep -q ',h264,video' \
    || { echo "the destination segment has no H.264" >&2; exit 1; }
printf '%s' "$PROBE" | grep -q ',aac,audio' \
    || { echo "the destination segment has no AAC" >&2; exit 1; }

# the same preparation also feeds HLS on the sender
grep -q '^#EXTM3U' "$RUN/a/hls/index.m3u8" \
    || { echo "no hls output alongside the srt destinations" >&2; exit 1; }

grep -q 'media: srt output 0 connected' "$RUN/a/logs/error.log" \
    || { echo "destination 0 never connected" >&2; exit 1; }

# the sockets exist by now, so this is the pool's own baseline
THREADS_START="$(ls "/proc/$WORKER_A/task" | wc -l)"
echo "   worker A runs $THREADS_START threads with both destinations connected"

THREADS_MEDIA="$(ls "/proc/$WORKER_A/task" | wc -l)"

[ "$THREADS_MEDIA" = "$THREADS_START" ] \
    || { echo "the sender pool grew while carrying media: $THREADS_START -> $THREADS_MEDIA threads" >&2
         exit 1; }

echo "   still $THREADS_MEDIA threads after carrying media to both destinations"

echo "== stop"
kill -KILL "$PUB" 2>/dev/null || true
PUB=0

sleep 1

for inst in a b; do
    PID="$(cat "$RUN/$inst/logs/nginx.pid")"
    "$NGINX" -p "$RUN/$inst" -c conf/nginx.conf -s quit

    # a destination thread may be inside a bounded connect or send
    for _ in $(seq 1 400); do
        kill -0 "$PID" 2>/dev/null || break
        sleep 0.05
    done

    if kill -0 "$PID" 2>/dev/null; then
        echo "nginx ($inst) did not shut down" >&2
        kill -9 "$PID" || true
        exit 1
    fi

    grep -q 'exited with code 0' "$RUN/$inst/logs/error.log" \
        || { echo "worker ($inst) did not exit cleanly" >&2; exit 1; }
done

trap - EXIT

echo "== srt output ok"

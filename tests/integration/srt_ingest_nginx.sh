#!/usr/bin/env bash
#
# SRT ingest through nginx (phase 1 exit criteria).
#
# Starts nginx built with the nginx-media module, pushes MPEG-TS over SRT with
# ffmpeg, and asserts what the worker observed: exactly one listener owner,
# the parsed source identity, per-session byte counts and the drained payload
# totals with no drops.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/srt-nginx"
PORT="${SRT_INGEST_NGINX_PORT:-19042}"
LOG="$RUN/logs/error.log"

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf"

# two workers: the listener must be owned by worker 0 only
cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_srt_listen 127.0.0.1:$PORT;
EOF

cleanup() {
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    if grep -q 'srt listener ready' "$LOG" 2>/dev/null; then
        break
    fi
    sleep 0.05
done

if ! grep -q 'srt listener ready' "$LOG"; then
    echo "listener never became ready:" >&2
    cat "$LOG" >&2
    exit 1
fi

OWNERS="$(grep -c 'srt listener ready' "$LOG")"
if [ "$OWNERS" -ne 1 ]; then
    echo "expected exactly one listener owner, found $OWNERS" >&2
    cat "$LOG" >&2
    exit 1
fi

# #!::r=live/news,m=publish,s=encoder-a, URL-encoded
STREAMID='%23!::r%3Dlive%2Fnews%2Cm%3Dpublish%2Cs%3Dencoder-a'

echo "== pushing 3s of MPEG-TS (H.264 + AAC) over SRT"
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i testsrc=size=320x240:rate=25 \
    -f lavfi -i sine=frequency=440:sample_rate=48000 -ac 2 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 96k \
    -t 3 -f mpegts "srt://127.0.0.1:$PORT?mode=caller&streamid=$STREAMID"

for _ in $(seq 1 200); do
    if grep -q 'srt source close' "$LOG" 2>/dev/null; then
        break
    fi
    sleep 0.05
done

echo "== worker view"
grep -E 'srt listener ready|srt source open|srt source close|srt ingest drained' "$LOG" || true

grep -q 'srt source open app=live stream=news source=encoder-a' "$LOG" \
    || { echo "source was not registered with the expected identity" >&2; exit 1; }

CLOSE_LINE="$(grep 'srt source close' "$LOG" | tail -1 || true)"
SESSION_BYTES="$(printf '%s' "$CLOSE_LINE" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')"

if [ -z "$SESSION_BYTES" ] || [ "$SESSION_BYTES" -le 0 ]; then
    echo "no positive session byte count: '$CLOSE_LINE'" >&2
    exit 1
fi

DRAIN_LINE="$(grep 'srt ingest drained' "$LOG" | tail -1 || true)"
DRAINED_BYTES="$(printf '%s' "$DRAIN_LINE" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')"
DROPPED_CHUNKS="$(printf '%s' "$DRAIN_LINE" | sed -n 's/.*dropped_chunks=\([0-9]*\).*/\1/p')"

if [ -z "$DRAINED_BYTES" ] || [ "$DRAINED_BYTES" -le 0 ]; then
    echo "no positive drained byte count: '$DRAIN_LINE'" >&2
    exit 1
fi

if [ "$DROPPED_CHUNKS" != "0" ]; then
    echo "unexpected ingest drops: '$DRAIN_LINE'" >&2
    exit 1
fi

if [ "$DRAINED_BYTES" != "$SESSION_BYTES" ]; then
    echo "drained $DRAINED_BYTES bytes but the session carried $SESSION_BYTES" >&2
    exit 1
fi

# phase 2: the worker demuxes the ingested MPEG-TS into encoded frames
DEMUX_LINE="$(grep 'srt demux video=' "$LOG" | tail -1 || true)"

if [ -z "$DEMUX_LINE" ]; then
    echo "no demux summary in the worker log" >&2
    exit 1
fi

VIDEO_FRAMES="$(printf '%s' "$DEMUX_LINE" | sed -n 's/.*video=\([0-9]*\).*/\1/p')"
AUDIO_FRAMES="$(printf '%s' "$DEMUX_LINE" | sed -n 's/.*audio=\([0-9]*\).*/\1/p')"
KEYFRAMES="$(printf '%s' "$DEMUX_LINE" | sed -n 's/.*keyframes=\([0-9]*\).*/\1/p')"

[ "$VIDEO_FRAMES" -ge 20 ] \
    || { echo "too few video frames: '$DEMUX_LINE'" >&2; exit 1; }
[ "$AUDIO_FRAMES" -ge 20 ] \
    || { echo "too few audio frames: '$DEMUX_LINE'" >&2; exit 1; }
[ "$KEYFRAMES" -ge 1 ] \
    || { echo "no keyframes: '$DEMUX_LINE'" >&2; exit 1; }

for counter in sync_errors continuity_errors psi_errors crc_errors \
               pes_errors au_overflows; do
    printf '%s' "$DEMUX_LINE" | grep -q "$counter=0" \
        || { echo "$counter is not zero: '$DEMUX_LINE'" >&2; exit 1; }
done

grep -q 'srt track 0 media=1 codec=1' "$LOG" \
    || { echo "H.264 video track not registered" >&2; exit 1; }

grep -q 'srt track 1 media=2 codec=3 format=3 rate=48000 channels=2 config=yes' "$LOG" \
    || { echo "AAC audio track not registered correctly" >&2; exit 1; }

echo "== demuxed video=$VIDEO_FRAMES audio=$AUDIO_FRAMES keyframes=$KEYFRAMES"

echo "== stopping nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit
trap - EXIT

PID="$(cat "$RUN/logs/nginx.pid" 2>/dev/null || true)"

for _ in $(seq 1 100); do
    if [ -z "$PID" ] || ! kill -0 "$PID" 2>/dev/null; then
        break
    fi
    sleep 0.05
done

if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
    echo "nginx did not shut down" >&2
    kill -9 "$PID" || true
    exit 1
fi

grep -q 'exited with code 0' "$LOG" \
    || { echo "worker did not exit cleanly" >&2; exit 1; }

if grep -q 'open socket' "$LOG"; then
    echo "worker left a socket registered at shutdown" >&2
    exit 1
fi

echo "== nginx srt ingest ok"

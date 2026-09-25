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
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/srt-nginx"
PORT="${SRT_INGEST_NGINX_PORT:-19042}"
API_PORT="${SRT_INGEST_NGINX_API_PORT:-19048}"
API="http://127.0.0.1:$API_PORT/media/api/v1"
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

http {
    access_log off;

    server {
        listen 127.0.0.1:$API_PORT;

        location /media/api/ {
            media_api;
        }
    }
}
EOF

cleanup() {
    # only this script's instance, addressed through its own prefix's pid
    # file.  A graceful signal is tried first so the master reaps its workers;
    # a worker stuck in transport teardown must not keep the instance (and the
    # ports) alive, so what is left is killed outright.  The prefix is unique
    # to this target, so another agent's nginx is never touched.
    [ -f "$RUN/logs/nginx.pid" ] || return 0

    pid="$(cat "$RUN/logs/nginx.pid" 2>/dev/null)"

    [ -n "$pid" ] || return 0

    kill -QUIT "$pid" 2>/dev/null

    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done

    for child in $(pgrep -P "$pid" 2>/dev/null); do
        kill -KILL "$child" 2>/dev/null
    done

    kill -KILL "$pid" 2>/dev/null

    return 0
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
STREAMID='#!::r=live/news,m=publish,s=encoder-a'

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

# A session the worker closes must give its ingest slot back clean, because
# the slot is reused: alloc takes the first one with no session.  Anything the
# closed session leaves behind is therefore inherited by the publisher that
# lands on that slot next, and the failure that inherited state produces is
# silent - the session is refused at its first poll event, before a byte is
# read, so the publisher has a connected transport, reads bytes=0 and the
# worker logs nothing about why.  This is the shape the assertion below
# catches: remove a source under a live publisher (which is what makes the
# worker ask for the session's close), then publish again and require the new
# session to carry media.
echo "== the slot a worker-closed session frees is reused clean"
CHURN_SECS=12

timeout "$(( CHURN_SECS + 20 ))" ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i testsrc=size=320x240:rate=25 \
    -f lavfi -i sine=frequency=440:sample_rate=48000 -ac 2 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 96k \
    -t "$CHURN_SECS" -f mpegts \
    "srt://127.0.0.1:$PORT?mode=caller&streamid=#!::r=live/news,m=publish,s=encoder-churn" \
    >"$RUN/churn.log" 2>&1 &
CHURN_PID=$!

for _ in $(seq 1 200); do
    if grep -q 'srt source open app=live stream=news source=encoder-churn' \
            "$LOG" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

grep -q 'srt source open app=live stream=news source=encoder-churn' "$LOG" \
    || { echo "the churn publisher was not accepted" >&2
         cat "$RUN/churn.log" >&2; exit 1; }

CLOSES_BEFORE="$(grep -c 'srt source close' "$LOG" || true)"

# ordered teardown: deleting the source closes its transport, so the worker
# asks the ingest thread to close the session
curl -fsS -X DELETE "$API/streams/live/news/sources/encoder-churn" >/dev/null

for _ in $(seq 1 200); do
    if grep -q 'source encoder-churn removed, closing its session' "$LOG"; then
        break
    fi
    sleep 0.1
done

grep -q 'source encoder-churn removed, closing its session' "$LOG" \
    || { echo "deleting the source did not close its session" >&2; exit 1; }

# the property only shows on the slot the closed session frees, so the next
# publisher has to start after that session is gone rather than racing it
for _ in $(seq 1 200); do
    if [ "$(grep -c 'srt source close' "$LOG" || true)" \
         -gt "$CLOSES_BEFORE" ]; then
        break
    fi
    sleep 0.1
done

[ "$(grep -c 'srt source close' "$LOG" || true)" -gt "$CLOSES_BEFORE" ] \
    || { echo "the session whose source was removed never closed" >&2; exit 1; }

kill -KILL "$CHURN_PID" 2>/dev/null
wait "$CHURN_PID" 2>/dev/null || true

# the next publisher is the one that proves the property
FRESH_SECS=6
CLOSES_AT_FRESH="$(grep -c 'srt source close' "$LOG" || true)"

timeout "$(( FRESH_SECS + 20 ))" ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i testsrc=size=320x240:rate=25 \
    -f lavfi -i sine=frequency=440:sample_rate=48000 -ac 2 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 96k \
    -t "$FRESH_SECS" -f mpegts \
    "srt://127.0.0.1:$PORT?mode=caller&streamid=#!::r=live/news,m=publish,s=encoder-fresh" \
    >"$RUN/fresh.log" 2>&1 &
FRESH_PID=$!

# Its own frames have to reach the program.  The stream's program_frames is
# cumulative and the publisher whose source was removed already contributed to
# it, so this counts the frames of *this* source: a session that was dropped
# at its first poll event never feeds the demux, so the source's frames_out
# stays 0.
for _ in $(seq 1 250); do
    if curl -fsS "$API/streams/live/news/sources" 2>/dev/null \
            | grep -qE '"id":"encoder-fresh"[^}]*"frames_out":[1-9]'; then
        break
    fi
    sleep 0.1
done

curl -fsS "$API/streams/live/news/sources" \
    | grep -qE '"id":"encoder-fresh"[^}]*"frames_out":[1-9]' \
    || { echo "the publisher after the removed source carried no media" >&2
         curl -fsS "$API/streams/live/news/sources" >&2
         grep 'source=encoder-fresh' "$LOG" >&2
         cat "$RUN/fresh.log" >&2; exit 1; }

wait "$FRESH_PID" 2>/dev/null || true

# and its session carried bytes rather than being dropped at bytes=0.  Its
# close is the one this waits for: reading the last close line while the
# session is still open would read the *previous* session's.
for _ in $(seq 1 100); do
    if [ "$(grep -c 'srt source close' "$LOG" || true)" \
         -gt "$CLOSES_AT_FRESH" ]; then
        break
    fi
    sleep 0.1
done

FRESH_CLOSE="$(grep 'srt source close' "$LOG" | tail -1 || true)"
FRESH_BYTES="$(printf '%s' "$FRESH_CLOSE" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')"

[ -n "$FRESH_BYTES" ] && [ "$FRESH_BYTES" -gt 0 ] \
    || { echo "the session after the removed source was dropped: '$FRESH_CLOSE'" >&2
         exit 1; }

echo "   the next publisher's session carried bytes=$FRESH_BYTES"

# And every session that opened was closed.  The worker learns about a
# session from the ingest thread's ring, and a wake-up that is skipped while
# no byte is pending strands the event: the worker never logs the CLOSE, never
# frees the slot, and keeps the dead publisher's source registered - one of
# sixteen slots per stranded event, with nothing in the log to say so.
OPENS="$(grep -c 'srt source open' "$LOG" || true)"
CLOSES="$(grep -c 'srt source close' "$LOG" || true)"

[ "$OPENS" = "$CLOSES" ] \
    || { echo "$OPENS sessions were accepted but $CLOSES closed:" >&2
         grep 'srt source' "$LOG" >&2; exit 1; }

echo "   every accepted session was closed ($OPENS/$CLOSES)"

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

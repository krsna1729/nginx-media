#!/usr/bin/env bash
#
# Deterministic SRT ingest test (phase 1 exit criteria).
#
# An SRT publisher (ffmpeg) sends MPEG-TS to the listener implemented by the
# transport adapter.  The harness parses the Stream ID, feeds the received
# bytes through the bounded TS ingest queue and drains it again.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT="${SRT_INGEST_PORT:-19041}"
IDLE_MS=1500
BUILD="$ROOT/.build"
BIN="$BUILD/srt_ingest"
LOG="$BUILD/srt_ingest.log"

mkdir -p "$BUILD"

CFLAGS_SRT="$(pkg-config --cflags srt)"
LIBS_SRT="$(pkg-config --libs srt)"

echo "== building harness"
cc -O1 -g -Wall -Wextra -Werror -std=c11 \
    -DNGX_MEDIA_UNIT_TEST \
    -I"$ROOT/src/core" -I"$ROOT/src/srt" -I"$ROOT/src/mpegts" \
    -I"$ROOT/tests/unit/shim" \
    $CFLAGS_SRT \
    -o "$BIN" \
    "$ROOT/tests/integration/srt_ingest.c" \
    "$ROOT/src/core/ngx_media_buffer.c" \
    "$ROOT/src/core/ngx_media_frame.c" \
    "$ROOT/src/core/ngx_media_feed.c" \
    "$ROOT/src/core/ngx_media_track.c" \
    "$ROOT/src/mpegts/ngx_media_ts_ingest.c" \
    "$ROOT/src/srt/ngx_media_srt_streamid.c" \
    "$ROOT/src/srt/ngx_media_srt_transport.c" \
    "$ROOT/src/srt/ngx_media_srt_haivision.c" \
    "$ROOT/tests/unit/shim/ngx_shim.c" \
    $LIBS_SRT -lpthread

echo "== starting listener on 127.0.0.1:$PORT"
rm -f "$LOG"
"$BIN" "$PORT" "$IDLE_MS" >"$LOG" 2>&1 &
HARNESS=$!

cleanup() {
    if kill -0 "$HARNESS" 2>/dev/null; then
        kill "$HARNESS" 2>/dev/null || true
    fi
}
trap cleanup EXIT

for _ in $(seq 1 200); do
    if grep -q '^LISTENING' "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$HARNESS" 2>/dev/null; then
        echo "harness died before listening:" >&2
        cat "$LOG" >&2
        exit 1
    fi
    sleep 0.05
done

if ! grep -q '^LISTENING' "$LOG"; then
    echo "listener never became ready:" >&2
    cat "$LOG" >&2
    exit 1
fi

# #!::r=live/news,m=publish,s=encoder-a, URL-encoded
STREAMID='%23!::r%3Dlive%2Fnews%2Cm%3Dpublish%2Cs%3Dencoder-a'

echo "== pushing 3s of MPEG-TS over SRT"
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i testsrc=size=320x240:rate=25 \
    -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -c:v mpeg2video -q:v 6 -c:a mp2 -b:a 128k \
    -t 3 -f mpegts "srt://127.0.0.1:$PORT?mode=caller&streamid=$STREAMID"

set +e
wait "$HARNESS"
RC=$?
set -e
trap - EXIT

echo "== harness output"
cat "$LOG"

if [ "$RC" -ne 0 ]; then
    echo "harness failed (rc=$RC)" >&2
    exit 1
fi

grep -q 'SOURCE app=live stream=news source=encoder-a mode=publish' "$LOG" \
    || { echo "unexpected stream id" >&2; exit 1; }

grep -q 'dropped_chunks=0' "$LOG" \
    || { echo "unexpected ingest drops" >&2; exit 1; }

echo "== srt ingest ok"

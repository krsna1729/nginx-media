#!/usr/bin/env bash
#
# Redundant-source switch test (phase 3 exit criteria).
#
# Two independent ffmpeg-generated MPEG-TS fixtures are demuxed concurrently
# into two sources of one logical stream; a manual promotion switches the
# program at the standby's cached keyframe.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/.build"
FIX="$BUILD/fixtures"
BIN="$BUILD/source_switch"
LOG="$BUILD/source_switch.log"

mkdir -p "$FIX"

echo "== building harness"
cc -O1 -g -Wall -Wextra -Werror -std=c11 \
    -DNGX_MEDIA_UNIT_TEST \
    -I"$ROOT/src/core" -I"$ROOT/src/codec" -I"$ROOT/src/mpegts" \
    -I"$ROOT/src/srt" -I"$ROOT/tests/unit/shim" \
    -o "$BIN" \
    "$ROOT/tests/integration/source_switch.c" \
    "$ROOT/src/core/ngx_media_buffer.c" \
    "$ROOT/src/core/ngx_media_frame.c" \
    "$ROOT/src/core/ngx_media_feed.c" \
    "$ROOT/src/core/ngx_media_track.c" \
    "$ROOT/src/core/ngx_media_timeline.c" \
    "$ROOT/src/core/ngx_media_source.c" \
    "$ROOT/src/core/ngx_media_stream.c" \
    "$ROOT/src/core/ngx_media_policy.c" \
    "$ROOT/src/codec/ngx_media_nal.c" \
    "$ROOT/src/codec/ngx_media_aac.c" \
    "$ROOT/src/mpegts/ngx_media_ts_demux.c" \
    "$ROOT/tests/unit/shim/ngx_shim.c"

generate() {
    local out="$1" pattern="$2" freq="$3"

    if [ -f "$out" ]; then
        return
    fi

    echo "== generating $out"
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i "$pattern" \
        -f lavfi -i "sine=frequency=$freq:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t 6 -f mpegts "$out"
}

generate "$FIX/encoder_a.ts" "testsrc=size=320x240:rate=25" 440
generate "$FIX/encoder_b.ts" "smptehdbars=size=320x240:rate=25" 880

echo "== switching between two live sources"
"$BIN" "$FIX/encoder_a.ts" "$FIX/encoder_b.ts" | tee "$LOG"

grep -q 'RESULT ok' "$LOG" || { echo "source switch failed" >&2; exit 1; }

echo "== source switch ok"

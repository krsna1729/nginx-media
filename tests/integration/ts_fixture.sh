#!/usr/bin/env bash
#
# MPEG-TS demux fixture test (phase 2 exit criteria).
#
# Generates deterministic local media with ffmpeg (H.264+AAC and H.265+AAC
# MPEG-TS) and demuxes it through the in-process demuxer, validating tracks,
# access units, ADTS frames, timestamps and error counters.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/.build"
FIX="$BUILD/fixtures"
BIN="$BUILD/ts_fixture"

mkdir -p "$FIX"

echo "== building harness"
cc -O1 -g -Wall -Wextra -Werror -std=c11 \
    -DNGX_MEDIA_UNIT_TEST \
    -I"$ROOT/src/core" -I"$ROOT/src/codec" -I"$ROOT/src/mpegts" \
    -I"$ROOT/src/srt" -I"$ROOT/tests/unit/shim" \
    -o "$BIN" \
    "$ROOT/tests/integration/ts_fixture.c" \
    "$ROOT/src/core/ngx_media_buffer.c" \
    "$ROOT/src/core/ngx_media_frame.c" \
    "$ROOT/src/core/ngx_media_feed.c" \
    "$ROOT/src/core/ngx_media_track.c" \
    "$ROOT/src/codec/ngx_media_nal.c" \
    "$ROOT/src/codec/ngx_media_aac.c" \
    "$ROOT/src/mpegts/ngx_media_ts_crc.c" \
    "$ROOT/src/mpegts/ngx_media_ts_demux.c" \
    "$ROOT/src/mpegts/ngx_media_ts_mux.c" \
    "$ROOT/tests/unit/shim/ngx_shim.c"

generate_h264() {
    echo "== generating H.264 + AAC fixture"
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i testsrc=size=320x240:rate=25 \
        -f lavfi -i sine=frequency=440:sample_rate=48000 -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t 4 -f mpegts "$FIX/h264_aac.ts"
}

generate_h265() {
    echo "== generating H.265 + AAC fixture"
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i testsrc=size=320x240:rate=25 \
        -f lavfi -i sine=frequency=440:sample_rate=48000 -ac 2 \
        -c:v libx265 -preset ultrafast -x265-params log-level=error -g 25 \
        -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t 2 -f mpegts "$FIX/h265_aac.ts"
}

generate_h264
generate_h265

echo "== demuxing H.264 fixture"
"$BIN" "$FIX/h264_aac.ts"

echo "== demuxing H.265 fixture"
"$BIN" "$FIX/h265_aac.ts"

# structural expectations on the H.264 fixture run
"$BIN" "$FIX/h264_aac.ts" > "$BUILD/ts_fixture_h264.log"

grep -q 'RESULT ok' "$BUILD/ts_fixture_h264.log" \
    || { echo "h264 fixture failed" >&2; exit 1; }

grep -q 'TRACK 0 media=1 codec=1' "$BUILD/ts_fixture_h264.log" \
    || { echo "h264: video track not discovered" >&2; exit 1; }

grep -q 'TRACK 1 media=2 codec=3' "$BUILD/ts_fixture_h264.log" \
    || { echo "h264: audio track not discovered" >&2; exit 1; }

grep -q 'rate=48000 channels=2' "$BUILD/ts_fixture_h264.log" \
    || { echo "h264: audio parameters wrong" >&2; exit 1; }

VIDEO_FRAMES="$(sed -n 's/.*FRAMES video=\([0-9]*\).*/\1/p' "$BUILD/ts_fixture_h264.log")"
AUDIO_FRAMES="$(sed -n 's/.*FRAMES video=[0-9]* audio=\([0-9]*\).*/\1/p' "$BUILD/ts_fixture_h264.log")"
KEYFRAMES="$(sed -n 's/.*keyframes=\([0-9]*\).*/\1/p' "$BUILD/ts_fixture_h264.log")"

# 4s at 25 fps with a 1s GOP: expect roughly 100 video frames, 4 keyframes
[ "$VIDEO_FRAMES" -ge 50 ] || { echo "too few video frames: $VIDEO_FRAMES" >&2; exit 1; }
[ "$AUDIO_FRAMES" -ge 100 ] || { echo "too few audio frames: $AUDIO_FRAMES" >&2; exit 1; }
[ "$KEYFRAMES" -ge 2 ] || { echo "too few keyframes: $KEYFRAMES" >&2; exit 1; }

echo "== ts fixture ok (video=$VIDEO_FRAMES audio=$AUDIO_FRAMES keyframes=$KEYFRAMES)"

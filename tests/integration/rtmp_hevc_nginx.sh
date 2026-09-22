#!/usr/bin/env bash
#
# Enhanced RTMP, end to end.  Legacy FLV cannot signal HEVC at all: an HEVC
# publisher only works if the extended header is understood, and an HEVC
# program can only be served if it is written in the extended form.  This test
# drives both directions with a real encoder and a real player.
#
#   1. ffmpeg publishes H.265 over RTMP  -> nginx must register an H.265 source
#   2. the program reaches HLS           -> the segments must decode as HEVC
#   3. ffmpeg plays the program back     -> the played stream must be HEVC
#
# ffmpeg writes the enhanced form automatically for HEVC (extended header
# byte, "hvc1" fourcc, HEVCDecoderConfigurationRecord on sequence start).
#
# The sequence-header builder needs at least 13 bytes of SPS: profile_space
# and profile_idc live in byte 1 and level_idc in byte 12.  Accepting a
# shorter SPS produced an hvcC with a garbage level, which is what a player
# rejects when it opens the stream.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/rtmp-hevc"
RTMP_PORT=1953
HTTP_PORT=18445

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

PUB=0
cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null
    pkill -KILL -f 'nginx: ' 2>/dev/null
    return 0
}
trap cleanup EXIT

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
media_rtmp_source_priority encoder-hevc 100;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /hls/ {
            alias $RUN/hls/;
        }

        location /media/api/ {
            media_api;
        }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || {
    echo "configuration rejected" >&2
    exit 1
}

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

echo "== publishing H.265 over enhanced rtmp"
timeout 60 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
    -c:v libx265 -preset ultrafast -x265-params log-level=none \
    -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 96k \
    -t 10 -f flv "rtmp://127.0.0.1:$RTMP_PORT/live/hevc" \
    >"$RUN/pub.log" 2>&1 &
PUB=$!

for _ in $(seq 1 150); do
    grep -q 'rtmp publisher stream=live/hevc' "$RUN/logs/error.log" 2>/dev/null \
        && break
    sleep 0.1
done

grep -q 'rtmp publisher stream=live/hevc' "$RUN/logs/error.log" \
    || { cat "$RUN/pub.log"; echo "the HEVC publisher was not registered" >&2; exit 1; }

echo "   publisher registered"

# The HLS segment is the proof that the extended header was parsed: the
# segmenter only writes HEVC if the source was modelled as HEVC rather than
# read as a legacy codec id, which is what the old parser would have done.
for _ in $(seq 1 200); do
    [ -f "$RUN/hls/live/hevc/index.m3u8" ] \
        && [ "$(grep -c '^#EXTINF' "$RUN/hls/live/hevc/index.m3u8" || true)" -ge 1 ] \
        && break
    sleep 0.1
done

# the player has to attach while the publisher is still running: the program
# ends with its only source
echo "== playing the program back over enhanced rtmp"
timeout 60 ffmpeg -hide_banner -loglevel error -y \
    -i "rtmp://127.0.0.1:$RTMP_PORT/live/hevc" \
    -t 4 -c copy "$RUN/played.flv" >"$RUN/play.log" 2>&1 &
PLAY=$!

SEG=""
for candidate in "$RUN"/hls/live/hevc/*.ts; do
    [ -e "$candidate" ] || continue

    if ffprobe -hide_banner -loglevel error -select_streams v:0 \
            -show_entries stream=codec_name -of csv=p=0 "$candidate" \
            2>/dev/null | grep -q '^hevc$'; then
        SEG="$candidate"
        break
    fi
done

[ -n "$SEG" ] || { echo "no HLS segment carried HEVC" >&2; exit 1; }

echo "   hls carries HEVC ($(basename "$SEG"), $(stat -c%s "$SEG") bytes)"

wait "$PLAY" 2>/dev/null \
    || { cat "$RUN/play.log"; echo "playback failed" >&2; exit 1; }

wait "$PUB" 2>/dev/null
PUB=0

PLAY_PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
    stream=codec_name,codec_type -of csv "$RUN/played.flv" 2>&1)"

printf '%s\n' "$PLAY_PROBE"

printf '%s' "$PLAY_PROBE" | grep -q ',hevc,video' \
    || { echo "the played stream is not HEVC" >&2; exit 1; }
printf '%s' "$PLAY_PROBE" | grep -q ',aac,audio' \
    || { echo "the played stream has no AAC" >&2; exit 1; }

FRAMES="$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries packet=pts_time -of csv=p=0 "$RUN/played.flv" 2>/dev/null \
    | wc -l)"

echo "   played $FRAMES HEVC video packets"
[ "$FRAMES" -ge 20 ] || { echo "too few video packets: $FRAMES" >&2; exit 1; }

# and the player must have decoded them, not merely copied them
timeout 60 ffmpeg -hide_banner -loglevel error -i "$RUN/played.flv" \
    -f null - 2>"$RUN/decode.log" \
    || { cat "$RUN/decode.log"; echo "the played stream does not decode" >&2; exit 1; }

[ -s "$RUN/decode.log" ] && { cat "$RUN/decode.log"; exit 1; }

echo "   the played stream decodes"

trap - EXIT
cleanup

echo "== rtmp hevc ok"

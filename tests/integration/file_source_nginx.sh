#!/usr/bin/env bash
#
# File input (goal doc 21): a first-class source.
#
# A TS file is registered as a source through the control API, and the runtime
# tick reads it, demuxes it and publishes the frames through the normal source
# gate.  Nothing here is special-cased: the source appears in the control API,
# carries health, and its frames reach the program and HLS like any publisher.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/file-source"
HTTP_PORT=18500

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

cleanup() {
    pkill -KILL -f 'nginx: ' 2>/dev/null
    return 0
}
trap cleanup EXIT

echo "== building a TS fixture"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 10 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -f mpegts "$RUN/slate.ts" 2>/dev/null \
    || { echo "could not build the fixture" >&2; exit 1; }

echo "   fixture is $(stat -c%s "$RUN/slate.ts") bytes"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/hls;

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

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || {
    echo "configuration rejected" >&2
    exit 1
}

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

echo "== a file source is created through the api"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"slate"}' "$API/streams" >/dev/null

STATUS="$(curl -sS -o "$RUN/src.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"slate-file\",\"type\":\"file\",\"path\":\"$RUN/slate.ts\"}" \
    "$API/streams/live/slate/sources")"

cat "$RUN/src.json"; echo

[ "$STATUS" = "201" ] || { echo "expected 201, got $STATUS" >&2; exit 1; }

curl -fsS "$API/streams/live/slate/sources" | grep -q '"id":"slate-file"' \
    || { echo "the file source is not listed" >&2; exit 1; }

echo "== the runtime tick reads it and the program carries frames"
for _ in $(seq 1 300); do
    FRAMES="$(curl -fsS "$API/streams/live/slate" \
        | grep -o '"program_frames":[0-9]*' | cut -d: -f2)"
    [ "${FRAMES:-0}" -gt 0 ] && break
    sleep 0.1
done

echo "   program frames: ${FRAMES:-0}"

[ "${FRAMES:-0}" -gt 0 ] \
    || { echo "the file source produced no frames" >&2
         tail -5 "$RUN/logs/error.log" >&2; exit 1; }

for _ in $(seq 1 200); do
    [ -f "$RUN/hls/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/hls/index.m3u8" ] \
    || { echo "the file source produced no hls output" >&2; exit 1; }

SEG="$(ls "$RUN"/hls/*.ts 2>/dev/null | head -1)"

[ -n "$SEG" ] || { echo "no segments" >&2; exit 1; }

PROBE="$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries stream=codec_name,width,height -of csv=p=0 "$SEG" 2>&1)"

echo "   hls segment: $PROBE"

printf '%s' "$PROBE" | grep -q '^h264' \
    || { echo "the segment does not carry H.264" >&2; exit 1; }

grep -q 'file source .* opened' "$RUN/logs/error.log" \
    || { echo "the file source was not opened" >&2; exit 1; }

echo "== the file ends and its source finishes"
for _ in $(seq 1 300); do
    grep -q 'file source .* finished' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.1
done

grep -q 'file source .* finished' "$RUN/logs/error.log" \
    || { echo "the file source never reported finishing" >&2; exit 1; }

echo "   the source finished at end of file"''

trap - EXIT
cleanup

echo "== file source ok"

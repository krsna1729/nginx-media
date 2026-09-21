#!/usr/bin/env bash
#
# HLS push source (normative revision, acceptance case 7): someone else PUTs
# segments to us and they become a source.
#
#   7. upload HLS into an HLS PUT source and make it eligible through the
#      normal health model
#
# The endpoint stores what it receives in a directory; a source of type
# hls_push watches that directory and demuxes what arrives.  Neither half
# knows about the other beyond the directory, so an upload arriving by any
# other means would work the same way.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/hls-ingest"
HTTP_PORT=18550
UPLOAD_PORT=18551

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/incoming"

cleanup() {
    pkill -KILL -f 'nginx: ' 2>/dev/null
    return 0
}
trap cleanup EXIT

echo "== building segments to upload"
# real segments, cut at keyframes by ffmpeg's own segmenter: splitting a TS at
# arbitrary byte offsets produces fragments no demuxer can follow
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 12 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -f segment -segment_time 2 -segment_format mpegts \
    "$RUN/incoming/seg-%05d.ts" 2>/dev/null \
    || { echo "could not build the fixture" >&2; exit 1; }

ls -la "$RUN/incoming" | tail -4

[ "$(ls "$RUN/incoming"/*.ts 2>/dev/null | wc -l)" -ge 2 ] \
    || { echo "the segmenter produced too few segments" >&2; exit 1; }

ls -la "$RUN/incoming" | tail -4

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/hls;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;
        location /media/api/ { media_api; }
    }

    server {
        listen 127.0.0.1:$UPLOAD_PORT;

        # the body is buffered to a file and renamed into the ingest
        # directory, so the reader never sees a partial segment
        client_body_temp_path $RUN/body;

        location /ingest/ {
            media_hls_ingest $RUN/uploaded;
        }
    }
}
EOF

mkdir -p "$RUN/uploaded" "$RUN/body"

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || {
    echo "configuration rejected" >&2
    exit 1
}

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

echo "== a stream and an hls push source"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"upload"}' "$API/streams" >/dev/null

STATUS="$(curl -sS -o "$RUN/src.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"uploader\",\"type\":\"hls_push\",\"path\":\"$RUN/uploaded\"}" \
    "$API/streams/live/upload/sources")"

cat "$RUN/src.json"; echo

[ "$STATUS" = "201" ] || { echo "expected 201, got $STATUS" >&2; exit 1; }

curl -fsS "$API/streams/live/upload/sources" | grep -q '"id":"uploader"' \
    || { echo "the source is not registered" >&2; exit 1; }

echo "== segments are uploaded"
UPLOADED=0

# Uploaded at roughly the rate they were produced, which is what a live
# uploader does.  Dumping twelve seconds of media in one burst is not a
# realistic test: the program feed is a bounded window, and a source that
# outruns every consumer simply overruns it.
for seg in "$RUN"/incoming/*.ts; do
    base="$(basename "$seg")"
    CODE="$(curl -sS -o /dev/null -w '%{http_code}' -T "$seg" \
        "http://127.0.0.1:$UPLOAD_PORT/ingest/$base")"
    echo "   PUT $base -> $CODE"
    [ "$CODE" = "201" ] || { echo "upload of $base failed" >&2; exit 1; }
    UPLOADED=$(( UPLOADED + 1 ))
    sleep 2
done

echo "   stored: $(ls "$RUN/uploaded" | wc -l) of $UPLOADED segments"

[ "$(ls "$RUN/uploaded" | wc -l)" -eq "$UPLOADED" ] \
    || { echo "not every upload landed" >&2; exit 1; }

echo "== the source reads them through the normal health model"
for _ in $(seq 1 300); do
    curl -fsS "$API/streams/live/upload" \
        | grep -qE '"program_frames":[1-9]' && break
    sleep 0.1
done

FRAMES="$(curl -fsS "$API/streams/live/upload" \
    | grep -o '"program_frames":[0-9]*' | cut -d: -f2)"

echo "   program frames: ${FRAMES:-0}"

[ "${FRAMES:-0}" -gt 0 ] \
    || { echo "the uploaded segments produced no frames" >&2
         tail -5 "$RUN/logs/error.log" >&2; exit 1; }

SOURCES="$(curl -fsS "$API/streams/live/upload/sources")"
printf '%s\n' "$SOURCES"

printf '%s' "$SOURCES" | grep -q '"id":"uploader".*"healthy":true' \
    || { echo "the upload source is not healthy" >&2; exit 1; }

printf '%s' "$SOURCES" | grep -q '"id":"uploader".*"eligible":true' \
    || { echo "the upload source is not eligible" >&2; exit 1; }

echo "   the upload source is healthy and eligible"

for _ in $(seq 1 200); do
    [ -f "$RUN/hls/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/hls/index.m3u8" ] \
    || { echo "the uploaded program produced no hls" >&2; exit 1; }

SEG="$(ls "$RUN"/hls/*.ts 2>/dev/null | head -1)"
PROBE="$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries stream=codec_name,width,height -of csv=p=0 "$SEG" 2>&1)"

echo "   hls segment: $PROBE"

printf '%s' "$PROBE" | grep -q '^h264' \
    || { echo "the segment does not carry H.264" >&2; exit 1; }

trap - EXIT
cleanup

echo "== hls ingest ok"

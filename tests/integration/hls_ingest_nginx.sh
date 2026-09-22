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

        # a second location, so a container with a stream type this build
        # cannot carry reaches a reader of its own instead of the one above
        location /ingest-odd/ {
            media_hls_ingest $RUN/uploaded-odd;
        }
    }
}
EOF

mkdir -p "$RUN/uploaded" "$RUN/uploaded-odd" "$RUN/body"

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
    [ -f "$RUN/hls/live/upload/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/hls/live/upload/index.m3u8" ] \
    || { echo "the uploaded program produced no hls" >&2; exit 1; }

SEG="$(ls "$RUN"/hls/live/upload/*.ts 2>/dev/null | head -1)"
PROBE="$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries stream=codec_name,width,height -of csv=p=0 "$SEG" 2>&1)"

echo "   hls segment: $PROBE"

printf '%s' "$PROBE" | grep -q '^h264' \
    || { echo "the segment does not carry H.264" >&2; exit 1; }

# ---------------------------------------------------------------------------
# what an hls input may carry, and what happens when it carries something else
# ---------------------------------------------------------------------------

echo "== a segment in a stream type this build cannot carry is reported"

# MPEG-2 video in MPEG-TS: a real HLS segment, in a container this build
# demuxes, carrying a stream type it does not track
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 4 \
    -c:v mpeg2video -g 25 -f mpegts "$RUN/incoming/mpeg2.ts" 2>/dev/null \
    || { echo "could not build the unsupported fixture" >&2; exit 1; }

mkdir -p "$RUN/uploaded-odd"

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"odd"}' "$API/streams")"

[ "$STATUS" = "201" ] || [ "$STATUS" = "200" ] \
    || { echo "could not create the odd stream: $STATUS" >&2; exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"odd-uploader\",\"type\":\"hls_push\",\"path\":\"$RUN/uploaded-odd\"}" \
    "$API/streams/live/odd/sources")"

[ "$STATUS" = "201" ] \
    || { echo "could not create the odd source: $STATUS" >&2; exit 1; }

CODE="$(curl -sS -o /dev/null -w '%{http_code}' -T "$RUN/incoming/mpeg2.ts" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest-odd/mpeg2.ts")"

echo "   PUT mpeg2.ts -> $CODE"

[ "$CODE" = "201" ] || { echo "upload of the unsupported segment failed" >&2
                         exit 1; }

for _ in $(seq 1 200); do
    grep -q 'carries no stream type this build can carry' \
        "$RUN/logs/error.log" && break
    sleep 0.1
done

grep -q 'carries no stream type this build can carry' "$RUN/logs/error.log" \
    || { echo "an unsupported input was never reported" >&2
         tail -10 "$RUN/logs/error.log" >&2; exit 1; }

echo "   reported:"
grep -a 'carries no stream type this build can carry' \
    "$RUN/logs/error.log" | tail -1 | sed 's/^/   /'

# and it never carries media: the demux has nothing it can publish, so the
# source has no frames and never goes on air.  It reports as a source whose
# transport is up and which is not carrying media yet - the same state a
# publisher that connected and has not sent a keyframe is in - rather than as
# one that failed, because nothing about it failed: it is carrying something
# this build cannot read, and the line above is where that is said.
sleep 3

ODD="$(curl -fsS "$API/streams/live/odd/sources")"

printf '%s' "$ODD" | grep -q '"id":"odd-uploader".*"frames_in":0' \
    || { echo "an unsupported input published frames" >&2
         printf '%s\n' "$ODD" >&2; exit 1; }

printf '%s' "$ODD" | grep -q '"id":"odd-uploader".*"active":false' \
    || { echo "an unsupported input went on air" >&2
         printf '%s\n' "$ODD" >&2; exit 1; }

echo "   it published no frames and never went on air"

trap - EXIT
cleanup

echo "== hls ingest ok"

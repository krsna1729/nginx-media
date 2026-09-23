#!/usr/bin/env bash
#
# External transform execution: FFmpeg is a supervised child, not linked into
# the nginx worker.  A file source enters the worker, FFmpeg changes the
# profile, and the packaged HLS segment exposes the transformed geometry.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/external-transform"
HTTP_PORT=18504
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

stop_instance() {
    local pid child

    [ -f "$RUN/logs/nginx.pid" ] || return 0
    pid="$(cat "$RUN/logs/nginx.pid")"

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

cleanup() {
    stop_instance
    return 0
}
trap cleanup EXIT

echo "== building a source fixture"
ffmpeg -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=640x360:rate=25" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -t 8 -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 96k -f mpegts "$RUN/source.ts" \
    || { echo "could not build the fixture" >&2; exit 1; }

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/hls;
media_transform_ffmpeg /usr/bin/ffmpeg;
media_transform_profile 320 180 400000 64000;

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

echo "== profile intent is accepted and exposed"
STATUS="$(curl -sS -o "$RUN/create.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"transformed","media":"profile"}' \
    "$API/streams")"

cat "$RUN/create.json"; echo
[ "$STATUS" = "201" ] \
    || { echo "profile stream create failed: $STATUS" >&2; exit 1; }
grep -q '"media":"profile"' "$RUN/create.json" \
    || { echo "profile intent was not returned" >&2; exit 1; }

STATUS="$(curl -sS -o "$RUN/invalid.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"invalid","media":"other"}' \
    "$API/streams")"
[ "$STATUS" = "400" ] \
    || { echo "invalid media intent was accepted: $STATUS" >&2; exit 1; }
grep -q '"error":"unknown_media_mode"' "$RUN/invalid.json" \
    || { echo "invalid media error was not returned" >&2; exit 1; }

curl -fsS "$API/streams" > "$RUN/streams.json"
if grep -q '"name":"invalid"' "$RUN/streams.json"; then
    echo "invalid media request created a stream" >&2
    exit 1
fi

curl -fsS "$API/desired" > "$RUN/desired.json"
grep -q '"media":"profile"' "$RUN/desired.json" \
    || { echo "desired state omitted profile intent" >&2; exit 1; }

echo "== file source enters the external executor"
STATUS="$(curl -sS -o "$RUN/source.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"fixture\",\"type\":\"file\",\"path\":\"$RUN/source.ts\"}" \
    "$API/streams/live/transformed/sources")"

cat "$RUN/source.json"; echo
[ "$STATUS" = "201" ] \
    || { echo "file source create failed: $STATUS" >&2; exit 1; }

for _ in $(seq 1 300); do
    [ -f "$RUN/hls/live/transformed/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/hls/live/transformed/index.m3u8" ] \
    || { echo "the transformed stream produced no playlist" >&2
         tail -20 "$RUN/logs/error.log" >&2
         exit 1; }

SEG=""
for _ in $(seq 1 100); do
    for candidate in "$RUN"/hls/live/transformed/*.ts; do
        if [ -f "$candidate" ]; then
            SEG="$candidate"
            break
        fi
    done
    [ -n "$SEG" ] && break
    sleep 0.1
done

[ -n "$SEG" ] \
    || { echo "the transformed stream produced no segment" >&2; exit 1; }

PROBE="$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries stream=codec_name,width,height -of csv=p=0 "$SEG" 2>&1)"
echo "   transformed segment: $PROBE"
printf '%s' "$PROBE" | grep -q '^h264,320,180$' \
    || { echo "external executor did not produce 320x180 H.264" >&2; exit 1; }

grep -q 'transform executor started' "$RUN/logs/error.log" \
    || { echo "the external executor did not start" >&2; exit 1; }

grep -q 'transform executor event wiring ready' "$RUN/logs/error.log" \
    || { echo "the external executor was not wired into nginx events" >&2; exit 1; }

grep -q 'transform executor' "$RUN/logs/error.log" \
    || { echo "the executor emitted no lifecycle log" >&2; exit 1; }

trap - EXIT
cleanup

echo "== external transform ok"

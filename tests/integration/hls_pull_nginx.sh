#!/usr/bin/env bash
#
# HLS pull source (normative revision, acceptance case 6).
#
# An origin instance publishes HLS.  A second instance pulls that playlist as
# a source, and the pulled source behaves like any other: it appears in the
# control API, carries health, and can be promoted through the normal
# selector.
#
#   6. add an HLS pull source at runtime, establish media health, promote it

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/hls-pull"
ORIGIN_SRT=24700
ORIGIN_HTTP=18520
PULL_SRT=24701
PULL_HTTP=18521

rm -rf "$RUN"
mkdir -p "$RUN/origin/conf" "$RUN/origin/logs" "$RUN/origin/hls" \
         "$RUN/puller/conf" "$RUN/puller/logs" "$RUN/puller/hls"

PUB=0
cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null
    pkill -KILL -f 'nginx: ' 2>/dev/null
    return 0
}
trap cleanup EXIT

cat > "$RUN/origin/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/origin/hls;
media_srt_listen 127.0.0.1:$ORIGIN_SRT;
media_srt_source_priority origin-encoder 100;

http {
    access_log off;

    # the origin serves TLS, so the pull source has to speak https and trust
    # a private CA: this is the capability the module must not block on
    server {
        listen 127.0.0.1:$ORIGIN_HTTP ssl;
        ssl_certificate $RUN/cert.pem;
        ssl_certificate_key $RUN/key.pem;
        ssl_conf_command Options KTLS;
        location /hls/ { alias $RUN/origin/hls/; }
    }
}
EOF

echo "== generating a certificate for the origin"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$RUN/key.pem" -out "$RUN/cert.pem" \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" \
    >"$RUN/openssl.log" 2>&1 \
    || { echo "certificate generation failed" >&2; exit 1; }

cat > "$RUN/puller/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/puller/hls;
media_srt_listen 127.0.0.1:$PULL_SRT;
media_srt_source_priority live-encoder 200;

http {
    access_log off;
    server {
        listen 127.0.0.1:$PULL_HTTP;
        location /media/api/ { media_api; }
    }
}
EOF

"$NGINX" -p "$RUN/origin" -c conf/nginx.conf -t >/dev/null || exit 1
"$NGINX" -p "$RUN/puller" -c conf/nginx.conf -t >/dev/null || exit 1

"$NGINX" -p "$RUN/origin" -c conf/nginx.conf
"$NGINX" -p "$RUN/puller" -c conf/nginx.conf
sleep 0.5

API="http://127.0.0.1:$PULL_HTTP/media/api/v1"

echo "== the puller starts with a live source of its own"
CREATE="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"relay"}' "$API/streams")"

echo "   stream create: $CREATE"

[ "$CREATE" = "201" ] || { echo "could not create the stream" >&2
                           tail -3 "$RUN/puller/logs/error.log" >&2; exit 1; }

SRC="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"id":"live-encoder","type":"srt","priority":200}' \
    "$API/streams/live/relay/sources")"

echo "   source create: $SRC"

[ "$SRC" = "201" ] || { echo "could not create the source" >&2; exit 1; }

timeout 60 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -t 30 -f mpegts \
    "srt://127.0.0.1:$PULL_SRT?mode=caller&streamid=%23!::r%3Dlive%2Frelay%2Cm%3Dpublish%2Cs%3Dlive-encoder" \
    >"$RUN/pub-live.log" 2>&1 &
PUB=$!

echo "== the origin publishes hls"
# The origin has to outlast the test, not just the assertion.  A pull source
# takes its media from the origin's playlist, so once the origin stops there
# is nothing new to fetch and nothing for the program to switch to - and this
# test spends most of its time waiting for a source to become healthy, for a
# switch to land on a keyframe, and for the relay to segment.
timeout 180 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x360:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -t 120 -f mpegts \
    "srt://127.0.0.1:$ORIGIN_SRT?mode=caller&streamid=%23!::r%3Dlive%2Forigin%2Cm%3Dpublish%2Cs%3Dorigin-encoder" \
    >"$RUN/pub-origin.log" 2>&1 &
ORIGIN_PUB=$!

for _ in $(seq 1 300); do
    [ -f "$RUN/origin/hls/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/origin/hls/index.m3u8" ] \
    || { echo "the origin produced no hls" >&2; exit 1; }

echo "   origin playlist is up"

echo "== an hls pull source is added at runtime"
STATUS="$(curl -sS -o "$RUN/pull.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"origin-hls\",\"type\":\"hls_pull\",\"path\":\"https://127.0.0.1:$ORIGIN_HTTP/hls/index.m3u8\",\"ca_file\":\"$RUN/cert.pem\"}" \
    "$API/streams/live/relay/sources")"

cat "$RUN/pull.json"; echo

[ "$STATUS" = "201" ] || { echo "expected 201, got $STATUS" >&2; exit 1; }

for _ in $(seq 1 400); do
    curl -fsS "$API/streams/live/relay/sources" \
        | grep -q '"id":"origin-hls".*"healthy":true' && break
    sleep 0.1
done

SOURCES="$(curl -fsS "$API/streams/live/relay/sources")"
printf '%s\n' "$SOURCES"

printf '%s' "$SOURCES" | grep -q '"id":"origin-hls"' \
    || { echo "the pull source is not registered" >&2; exit 1; }

printf '%s' "$SOURCES" | grep -q '"id":"origin-hls".*"healthy":true' \
    || { echo "the pull source never became healthy" >&2
         grep -a 'hls pull' "$RUN/puller/logs/error.log" | tail -3 >&2; exit 1; }

echo "== it is promoted through the normal selector"
STATUS="$(curl -sS -o "$RUN/switch.json" -w '%{http_code}' \
    -X POST "$API/streams/live/relay/switch?source=origin-hls")"

cat "$RUN/switch.json"; echo

[ "$STATUS" = "200" ] || { echo "promotion failed: $STATUS" >&2; exit 1; }

# A switch is not instantaneous and is not meant to be: the selector takes it
# at the incoming source's next keyframe, so the request is acknowledged
# before the program is actually carrying the new source.  Wait for the
# program to say it has switched rather than reading the acknowledgement.
for _ in $(seq 1 400); do
    curl -fsS "$API/streams/live/relay" \
        | grep -q '"active":"origin-hls"' && break
    sleep 0.1
done

ACTIVE="$(curl -fsS "$API/streams/live/relay" \
    | grep -o '"active":"[a-z-]*"' | head -1)"

echo "   promoted: $ACTIVE"

[ "$ACTIVE" = '"active":"origin-hls"' ] \
    || { echo "the pull source was not promoted" >&2
         curl -fsS "$API/streams/live/relay" >&2; exit 1; }

for _ in $(seq 1 300); do
    [ -f "$RUN/puller/hls/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/puller/hls/index.m3u8" ] \
    || { echo "the pulled program produced no hls" >&2; exit 1; }

SEG="$(ls "$RUN"/puller/hls/*.ts 2>/dev/null | head -1)"

[ -n "$SEG" ] || { echo "no segments at the puller" >&2; exit 1; }

PROBE="$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries stream=codec_name,width,height -of csv=p=0 "$SEG" 2>&1)"

echo "   pulled hls segment: $PROBE"

printf '%s' "$PROBE" | grep -q '^h264' \
    || { echo "the pulled segment does not carry H.264" >&2; exit 1; }

grep -q 'hls pull source origin-hls opened' "$RUN/puller/logs/error.log" \
    || { echo "the pull source was not opened" >&2; exit 1; }

kill -KILL "$ORIGIN_PUB" 2>/dev/null
kill -KILL "$PUB" 2>/dev/null
wait 2>/dev/null

trap - EXIT
cleanup

echo "== hls pull ok"

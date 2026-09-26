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
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/hls-pull"
ORIGIN_SRT=24700
ORIGIN_HTTP=18520
PULL_SRT=24701
PULL_HTTP=18521

rm -rf "$RUN"
mkdir -p "$RUN/origin/conf" "$RUN/origin/logs" "$RUN/origin/hls" \
         "$RUN/puller/conf" "$RUN/puller/logs" "$RUN/puller/hls"

PUB=0

# The master is stopped and waited for, and its children are then killed by
# parent: nginx rewrites a worker's argv to "nginx: worker process", so a
# pattern that matches the configuration path matches the master only, and a
# worker whose master was killed outright is reparented to init and keeps the
# ports the next run needs.
stop_instance() {
    local prefix="$1" pid child

    [ -f "$prefix/logs/nginx.pid" ] || return 0

    pid="$(cat "$prefix/logs/nginx.pid")"

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
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null
    stop_instance "$RUN/origin"
    stop_instance "$RUN/puller"
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
    "srt://127.0.0.1:$PULL_SRT?mode=caller&streamid=#!::r=live/relay,m=publish,s=live-encoder" \
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
    "srt://127.0.0.1:$ORIGIN_SRT?mode=caller&streamid=#!::r=live/origin,m=publish,s=origin-encoder" \
    >"$RUN/pub-origin.log" 2>&1 &
ORIGIN_PUB=$!

for _ in $(seq 1 300); do
    [ -f "$RUN/origin/hls/live/origin/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/origin/hls/live/origin/index.m3u8" ] \
    || { echo "the origin produced no hls" >&2; exit 1; }

echo "   origin playlist is up"

echo "== an hls pull source is added at runtime"
STATUS="$(curl -sS -o "$RUN/pull.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"origin-hls\",\"type\":\"hls_pull\",\"path\":\"https://127.0.0.1:$ORIGIN_HTTP/hls/live/origin/index.m3u8\",\"ca_file\":\"$RUN/cert.pem\"}" \
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
    [ -f "$RUN/puller/hls/live/relay/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/puller/hls/live/relay/index.m3u8" ] \
    || { echo "the pulled program produced no hls" >&2; exit 1; }

SEG="$(ls "$RUN"/puller/hls/live/relay/*.ts 2>/dev/null | head -1)"

[ -n "$SEG" ] || { echo "no segments at the puller" >&2; exit 1; }

PROBE="$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries stream=codec_name,width,height -of csv=p=0 "$SEG" 2>&1)"

echo "   pulled hls segment: $PROBE"

printf '%s' "$PROBE" | grep -q '^h264' \
    || { echo "the pulled segment does not carry H.264" >&2; exit 1; }

grep -q 'hls pull source origin-hls opened' "$RUN/puller/logs/error.log" \
    || { echo "the pull source was not opened" >&2; exit 1; }

# ---------------------------------------------------------------------------
# a source whose origin accepts and never answers
# ---------------------------------------------------------------------------

echo "== a pull source whose origin never answers gives up on its own deadline"

STALL_PORT=18522

python3 - "$STALL_PORT" <<'STALL' >"$RUN/stall.log" 2>&1 &
import socket
import sys

listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", int(sys.argv[1])))
listener.listen(8)

print("listening", flush=True)

held = []

while True:
    # accepted, and never answered: the playlist fetch waits for a reply that
    # is not coming
    conn, _ = listener.accept()
    print("accepted", flush=True)
    held.append(conn)
STALL
STALL=$!

# the source must be pointed at a listener that is already accepting, or the
# connect is refused and the case proves nothing about waiting
for _ in $(seq 1 100); do
    grep -q listening "$RUN/stall.log" && break
    sleep 0.1
done

grep -q listening "$RUN/stall.log" \
    || { echo "the stall server never listened" >&2
         kill -KILL "$STALL" 2>/dev/null; exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"stalled\",\"type\":\"hls_pull\",\"path\":\"http://127.0.0.1:$STALL_PORT/index.m3u8\"}" \
    "$API/streams/live/relay/sources")"

[ "$STATUS" = "201" ] \
    || { echo "could not create the stalled source: $STATUS" >&2
         kill -KILL "$STALL" 2>/dev/null; exit 1; }

# the fetch reached a listener that accepted it and then said nothing, so the
# only thing that can end it is the deadline on the socket
for _ in $(seq 1 100); do
    grep -q accepted "$RUN/stall.log" && break
    sleep 0.1
done

grep -q accepted "$RUN/stall.log" \
    || { echo "the fetch never reached the stall server" >&2
         kill -KILL "$STALL" 2>/dev/null; exit 1; }

# the I/O deadline is 10 s and the reader retries between fetches, so the first
# failure is expected well inside this window; without a deadline the fetch
# waits for the kernel's own timeout and this window is never enough
for _ in $(seq 1 300); do
    grep -q 'hls pull stalled could not fetch the playlist' \
        "$RUN/puller/logs/error.log" && break
    sleep 0.1
done

grep -q 'hls pull stalled could not fetch the playlist' \
    "$RUN/puller/logs/error.log" \
    || { echo "a source whose origin never answers did not give up" >&2
         kill -KILL "$STALL" 2>/dev/null
         tail -5 "$RUN/puller/logs/error.log" >&2
         exit 1; }

echo "   the fetch failed on its deadline"

kill -KILL "$STALL" 2>/dev/null
wait "$STALL" 2>/dev/null

kill -KILL "$ORIGIN_PUB" 2>/dev/null
kill -KILL "$PUB" 2>/dev/null
wait 2>/dev/null

trap - EXIT
cleanup

echo "== hls pull ok"

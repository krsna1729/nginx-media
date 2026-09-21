#!/usr/bin/env bash
#
# RTMPS: the same RTMP protocol inside TLS, on our own listener socket.
#
#   1. a publisher using rtmps:// is accepted, and the program reaches HLS
#   2. a publisher using plain rtmp:// against an RTMPS listener is refused
#   3. the same listener serves playback over rtmps://
#
# The certificate is self-signed and generated here, so the test needs no
# credentials and no external service.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/rtmps"
RTMP_PORT=1936
HTTP_PORT=18446

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

PUB=0
PLAY=0
cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null
    [ "$PLAY" != "0" ] && kill -KILL "$PLAY" 2>/dev/null
    pkill -KILL -f 'nginx: ' 2>/dev/null
    return 0
}
trap cleanup EXIT

echo "== generating a self-signed certificate"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$RUN/key.pem" -out "$RUN/cert.pem" \
    -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    >"$RUN/openssl.log" 2>&1 \
    || { cat "$RUN/openssl.log"; echo "certificate generation failed" >&2; exit 1; }

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
media_rtmp_source_priority encoder-tls 100;
media_rtmp_ssl on;
media_rtmp_ssl_certificate $RUN/cert.pem;
media_rtmp_ssl_certificate_key $RUN/key.pem;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /hls/ {
            alias $RUN/hls/;
        }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || {
    echo "the RTMPS configuration was rejected" >&2
    exit 1
}

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

echo "== publishing over rtmps"
timeout 60 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 96k \
    -tls_verify 1 -ca_file "$RUN/cert.pem" \
    -t 8 -f flv "rtmps://127.0.0.1:$RTMP_PORT/live/tls" \
    >"$RUN/pub.log" 2>&1 &
PUB=$!

for _ in $(seq 1 150); do
    grep -q 'rtmp publisher stream=live/tls' "$RUN/logs/error.log" 2>/dev/null \
        && break
    sleep 0.1
done

grep -q 'rtmp publisher stream=live/tls' "$RUN/logs/error.log" \
    || { cat "$RUN/pub.log"; echo "the rtmps publisher was not registered" >&2; exit 1; }

echo "   rtmps publisher registered"

for _ in $(seq 1 200); do
    [ -f "$RUN/hls/index.m3u8" ] \
        && [ "$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" || true)" -ge 1 ] \
        && break
    sleep 0.1
done

[ -f "$RUN/hls/index.m3u8" ] \
    || { echo "no hls output from the rtmps source" >&2; exit 1; }

echo "   the program reached hls"

# playback, while the publisher is still running
# -rw_timeout bounds the socket: without it a stalled RTMPS session makes
# ffmpeg wait forever and the test hangs instead of failing
timeout 45 ffmpeg -hide_banner -loglevel error -y \
    -tls_verify 1 -ca_file "$RUN/cert.pem" \
    -rw_timeout 10000000 \
    -i "rtmps://127.0.0.1:$RTMP_PORT/live/tls" \
    -t 3 -c copy "$RUN/played.flv" >"$RUN/play.log" 2>&1 \
    || { cat "$RUN/play.log"; echo "rtmps playback failed" >&2; exit 1; }

PLAY_PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
    stream=codec_name,codec_type -of csv "$RUN/played.flv" 2>&1)"

printf '%s\n' "$PLAY_PROBE"

printf '%s' "$PLAY_PROBE" | grep -q ',h264,video' \
    || { echo "the played stream has no H.264" >&2; exit 1; }

echo "   rtmps playback carried the program"

# the publisher may be waiting on a session the server has not closed yet;
# it has served its purpose, so do not let it hold the test open
kill -KILL "$PUB" 2>/dev/null
wait "$PUB" 2>/dev/null
PUB=0

echo "== plain rtmp against the rtmps listener must be refused"
timeout 30 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -t 3 -f flv "rtmp://127.0.0.1:$RTMP_PORT/live/plain" \
    >"$RUN/plain.log" 2>&1

if grep -q 'rtmp publisher stream=live/plain' "$RUN/logs/error.log"; then
    echo "a plain RTMP publisher was accepted by an RTMPS listener" >&2
    exit 1
fi

echo "   refused (no publisher registered for the plain connection)"

trap - EXIT
cleanup

echo "== rtmps ok"

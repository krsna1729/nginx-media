#!/usr/bin/env bash
#
# Control API and manual switch through nginx (phase 3 exit criteria).
#
# Two publishers feed the same logical stream (live/news) as encoder-a and
# encoder-b.  The API lists the stream, its sources and switches the program
# between them while both stay connected.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/api-nginx"
SRT_PORT="${API_SRT_PORT:-19043}"
HTTP_PORT="${API_HTTP_PORT:-18443}"
LOG="$RUN/logs/error.log"
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf" "$RUN/html"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_srt_listen 127.0.0.1:$SRT_PORT;

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

PUB_A=0
PUB_B=0

cleanup() {
    [ "$PUB_A" != "0" ] && kill "$PUB_A" 2>/dev/null || true
    [ "$PUB_B" != "0" ] && kill "$PUB_B" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$LOG" 2>/dev/null && break
    sleep 0.05
done

grep -q 'srt listener ready' "$LOG" || { echo "listener not ready" >&2; cat "$LOG" >&2; exit 1; }

publish() {
    local pattern="$1" freq="$2" source="$3" out="$4"

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "$pattern" \
        -f lavfi -i "sine=frequency=$freq:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t 10 -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=%23!::r%3Dlive%2Fnews%2Cm%3Dpublish%2Cs%3D$source" \
        >"$RUN/$out.log" 2>&1
}

echo "== starting two publishers on live/news"
publish "testsrc=size=320x240:rate=25" 440 "encoder-a" pub_a &
PUB_A=$!
sleep 1
publish "smptehdbars=size=320x240:rate=25" 880 "encoder-b" pub_b &
PUB_B=$!

for _ in $(seq 1 200); do
    if grep -q 'srt source open.*source=encoder-b' "$LOG" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

sleep 2

echo "== listing streams"
LIST="$(curl -fsS "$API/streams")"
printf '%s\n' "$LIST"

printf '%s' "$LIST" | grep -q '"application":"live","name":"news"' \
    || { echo "stream not listed" >&2; exit 1; }
printf '%s' "$LIST" | grep -q '"active":"encoder-a"' \
    || { echo "encoder-a is not the active source" >&2; exit 1; }
printf '%s' "$LIST" | grep -q '"id":"encoder-b"' \
    || { echo "encoder-b not registered" >&2; exit 1; }

echo "== sources endpoint"
SOURCES="$(curl -fsS "$API/streams/live/news/sources")"
printf '%s\n' "$SOURCES"
printf '%s' "$SOURCES" | grep -q '"id":"encoder-a".*"state":"active"' \
    || { echo "encoder-a state wrong" >&2; exit 1; }
printf '%s' "$SOURCES" | grep -q '"id":"encoder-b".*"state":"standby"' \
    || { echo "encoder-b is not a hot standby" >&2; exit 1; }

STANDBY_PREROLL="$(printf '%s' "$SOURCES" | sed -n 's/.*"id":"encoder-b".*"preroll_units":\([0-9]*\).*/\1/p')"
[ -n "$STANDBY_PREROLL" ] && [ "$STANDBY_PREROLL" -gt 0 ] \
    || { echo "the standby has no cached GOP" >&2; exit 1; }

echo "== switching to encoder-b"
SWITCH="$(curl -fsS -X POST "$API/streams/live/news/switch?source=encoder-b")"
printf '%s\n' "$SWITCH"

printf '%s' "$SWITCH" | grep -q '"active":"encoder-b"' \
    || { echo "switch did not take effect" >&2; exit 1; }
printf '%s' "$SWITCH" | grep -q '"generation":2' \
    || { echo "generation was not bumped" >&2; exit 1; }
printf '%s' "$SWITCH" | grep -q '"switches":1' \
    || { echo "switch was not counted" >&2; exit 1; }

DETAIL="$(curl -fsS "$API/streams/live/news")"
printf '%s\n' "$DETAIL"
printf '%s' "$DETAIL" | grep -q '"active":"encoder-b"' \
    || { echo "detail does not show the new active source" >&2; exit 1; }
printf '%s' "$DETAIL" | grep -q '"id":"encoder-a".*"state":"standby"' \
    || { echo "the demoted source is not a standby" >&2; exit 1; }

echo "== error paths"
STATUS="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$API/streams/live/news/switch?source=nobody")"
[ "$STATUS" = "404" ] || { echo "unknown source should be 404, got $STATUS" >&2; exit 1; }

STATUS="$(curl -s -o /dev/null -w '%{http_code}' "$API/streams/live/news/switch?source=encoder-a")"
[ "$STATUS" = "405" ] || { echo "GET on switch should be 405, got $STATUS" >&2; exit 1; }

STATUS="$(curl -s -o /dev/null -w '%{http_code}' "$API/streams/live/missing")"
[ "$STATUS" = "404" ] || { echo "unknown stream should be 404, got $STATUS" >&2; exit 1; }

wait "$PUB_A" 2>/dev/null || true
PUB_A=0
wait "$PUB_B" 2>/dev/null || true
PUB_B=0

echo "== worker view"
grep -E 'srt program|srt source open|api switch' "$LOG" || true

PID="$(cat "$RUN/logs/nginx.pid")"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit
trap - EXIT

for _ in $(seq 1 100); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.05
done

if kill -0 "$PID" 2>/dev/null; then
    echo "nginx did not shut down" >&2
    kill -9 "$PID" || true
    exit 1
fi

grep -q 'exited with code 0' "$LOG" \
    || { echo "worker did not exit cleanly" >&2; exit 1; }

echo "== api switch ok"

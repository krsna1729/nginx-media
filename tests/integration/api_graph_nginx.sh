#!/usr/bin/env bash
#
# Runtime graph: stream CRUD through the control API (normative revision).
#
# The deployment starts with zero declared streams, so everything below is
# created at runtime.  The last case is the one that matters most: a stream
# created through the API is the same runtime object a publisher lands on, so
# there is no separate static and dynamic implementation.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/api-graph"
SRT_PORT=24640
HTTP_PORT=18448
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

PUB=0
cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null
    pkill -KILL -f 'nginx: ' 2>/dev/null
    return 0
}
trap cleanup EXIT

# note: no media_srt_output, and no streams declared anywhere
cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/hls;
media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;

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

echo "== a deployment with no declared streams"
LIST="$(curl -fsS "$API/streams")"
printf '%s\n' "$LIST"

printf '%s' "$LIST" | grep -q '"streams":\[\]' \
    || { echo "expected an empty graph" >&2; exit 1; }

echo "== create a stream through the api"
STATUS="$(curl -sS -o "$RUN/create.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"news"}' "$API/streams")"

cat "$RUN/create.json"; echo

[ "$STATUS" = "201" ] || { echo "expected 201, got $STATUS" >&2; exit 1; }

grep -q '"created":true' "$RUN/create.json" \
    || { echo "the stream was not reported as created" >&2; exit 1; }

REVISION="$(grep -o '"revision":[0-9]*' "$RUN/create.json" | cut -d: -f2)"
echo "   created at revision $REVISION"

echo "== the same request again is idempotent"
STATUS="$(curl -sS -o "$RUN/create2.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"news"}' "$API/streams")"

cat "$RUN/create2.json"; echo

[ "$STATUS" = "200" ] || { echo "replay should be 200, got $STATUS" >&2; exit 1; }

grep -q '"created":false' "$RUN/create2.json" \
    || { echo "the replay created a second object" >&2; exit 1; }

COUNT="$(curl -fsS "$API/streams" \
    | grep -o '"application":"live","name":"news"' | wc -l)"

[ "$COUNT" = "1" ] || { echo "expected one stream, found $COUNT" >&2; exit 1; }

echo "   replay produced no duplicate"

echo "== a mutation carries the revision"
STATUS="$(curl -sS -o "$RUN/patch.json" -w '%{http_code}' \
    -X PATCH -H 'Content-Type: application/json' \
    -d "{\"revision\":$REVISION,\"failure_timeout_ms\":2500}" \
    "$API/streams/live/news")"

cat "$RUN/patch.json"; echo

[ "$STATUS" = "200" ] || { echo "expected 200, got $STATUS" >&2; exit 1; }

NEW_REVISION="$(grep -o '"revision":[0-9]*' "$RUN/patch.json" | cut -d: -f2)"

[ "$NEW_REVISION" -gt "$REVISION" ] \
    || { echo "the revision did not advance" >&2; exit 1; }

grep -q '"failure_timeout_ms":2500' <(curl -fsS "$API/streams/live/news") \
    || { echo "the patch did not take effect" >&2; exit 1; }

echo "   revision $REVISION -> $NEW_REVISION, policy updated"

echo "== a stale mutation is refused"
STATUS="$(curl -sS -o "$RUN/stale.json" -w '%{http_code}' \
    -X PATCH -H 'Content-Type: application/json' \
    -d "{\"revision\":$REVISION,\"failure_timeout_ms\":9999}" \
    "$API/streams/live/news")"

cat "$RUN/stale.json"; echo

[ "$STATUS" = "409" ] || { echo "stale write should be 409, got $STATUS" >&2; exit 1; }

grep -q '"failure_timeout_ms":2500' <(curl -fsS "$API/streams/live/news") \
    || { echo "the stale write was applied anyway" >&2; exit 1; }

echo "   refused with 409, newer state intact"

echo "== a publisher lands on the api-created stream"
timeout 40 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -t 5 -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=%23!::r%3Dlive%2Fnews%2Cm%3Dpublish%2Cs%3Dencoder-a" \
    >"$RUN/pub.log" 2>&1 &
PUB=$!

for _ in $(seq 1 100); do
    grep -q 'srt source open app=live stream=news' "$RUN/logs/error.log" \
        2>/dev/null && break
    sleep 0.1
done

wait "$PUB" 2>/dev/null
PUB=0

grep -q 'srt source open app=live stream=news' "$RUN/logs/error.log" \
    || { cat "$RUN/pub.log"; echo "the publisher did not attach" >&2; exit 1; }

for _ in $(seq 1 100); do
    [ -f "$RUN/hls/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/hls/index.m3u8" ] \
    || { echo "no hls output for the api-created stream" >&2; exit 1; }

echo "   the publisher attached and the program reached hls"

echo "== delete is ordered and idempotent"
STATUS="$(curl -sS -o "$RUN/delete.json" -w '%{http_code}' \
    -X DELETE "$API/streams/live/news")"

cat "$RUN/delete.json"; echo

[ "$STATUS" = "200" ] || { echo "expected 200, got $STATUS" >&2; exit 1; }

grep -q '"deleted":true' "$RUN/delete.json" \
    || { echo "the stream was not deleted" >&2; exit 1; }

STATUS="$(curl -sS -o "$RUN/delete2.json" -w '%{http_code}' \
    -X DELETE "$API/streams/live/news")"

cat "$RUN/delete2.json"; echo

[ "$STATUS" = "200" ] || { echo "deleting again should succeed, got $STATUS" >&2; exit 1; }

grep -q '"deleted":false' "$RUN/delete2.json" \
    || { echo "the second delete should report absence" >&2; exit 1; }

curl -fsS "$API/streams" | grep -q '"streams":\[\]' \
    || { echo "the graph is not empty after deletion" >&2; exit 1; }

echo "   deleted, and deleting again is a no-op"

trap - EXIT
cleanup

echo "== api graph ok"

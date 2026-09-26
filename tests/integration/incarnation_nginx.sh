#!/usr/bin/env bash
#
# A stream is owned by the deterministic hash slot, not by the worker that
# accepted the API request or publisher.  This regression drives a publisher
# through the routing layer, deletes its stream, and creates the same logical
# name again while the old session is still sending.  The old routed session
# must not attach to the new object.
#
# The test also proves the owner consistency invariant: the API's owner is
# worker 1, while the single SRT endpoint is accepted by worker 0, and the
# routed-open log must appear before media reaches the owner's program feed.
#
# What it catches: a routed slot that outlives its stream, a delete that leaves
# a publisher attached to the identity, and a replica that rebuilds a stream
# from a message in flight when the delete was applied.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/incarnation"
SRT_PORT=24720
HTTP_PORT=18596

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

PUB=0

cleanup() {
    [ "$PUB" != "0" ] && { kill -KILL "$PUB" 2>/dev/null; wait "$PUB" 2>/dev/null; }

    if [ -f "$RUN/logs/nginx.pid" ]; then
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
    fi

    return 0
}
trap cleanup EXIT

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 2;
daemon on;
error_log logs/error.log notice;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/hls;

# one endpoint: worker 0 accepts every publisher, so a stream owned by worker 1
# is fed across the routing layer
media_srt_listen 127.0.0.1:$SRT_PORT;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /media/api/ { media_api; }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null \
    || { echo "configuration rejected" >&2; exit 1; }

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

field() {  # field <json> <name>
    printf '%s' "$1" | grep -o "\"$2\":[^,}]*" | head -1 | cut -d: -f2
}

echo "== a stream whose owner is worker 1, so its publisher is routed"

NAME=""
OWNER=""

for i in $(seq 1 200); do
    CANDIDATE="inc-$i"

    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"application\":\"live\",\"name\":\"$CANDIDATE\"}" \
        "$API/streams" >/dev/null

    OWNER="$(field "$(curl -fsS "$API/streams/live/$CANDIDATE")" owner)"

    if [ "$OWNER" = "1" ]; then
        NAME="$CANDIDATE"
        break
    fi

    curl -fsS -X DELETE "$API/streams/live/$CANDIDATE" >/dev/null
done

[ -n "$NAME" ] || { echo "no stream maps to worker 1" >&2; exit 1; }

echo "   live/$NAME, owned by worker $OWNER"

timeout 120 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -t 90 -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/$NAME,m=publish,s=enc-a" \
    >"$RUN/pub.log" 2>&1 &
PUB=$!

for _ in $(seq 1 200); do
    grep -q "routed source opened stream=live/$NAME" "$RUN/logs/error.log" \
        && break
    sleep 0.1
done

grep -q "routed source opened stream=live/$NAME" "$RUN/logs/error.log" \
    || { echo "the publisher was not routed to its owner" >&2
         tail -5 "$RUN/logs/error.log" >&2; exit 1; }

STATE="$(curl -fsS "$API/streams/live/$NAME")"

FRAMES="$(field "$STATE" program_frames)"

for _ in $(seq 1 300); do
    [ "${FRAMES:-0}" -gt 0 ] && break

    sleep 0.1

    STATE="$(curl -fsS "$API/streams/live/$NAME")"
    FRAMES="$(field "$STATE" program_frames)"
done

echo "   routed and carrying frames: ${FRAMES:-0}"

[ "${FRAMES:-0}" -gt 0 ] \
    || { echo "the routed program carried no frames" >&2
         printf '%s\n' "$STATE" >&2; exit 1; }
INCARNATION="$(field "$STATE" incarnation)"
echo "   routed stream incarnation: ${INCARNATION:-missing}"

[ -n "$INCARNATION" ] && [ "$INCARNATION" != "0" ] \
    || { echo "the stream did not expose a non-zero incarnation" >&2
         printf '%s\n' "$STATE" >&2; exit 1; }

echo "== the stream is deleted while the publisher is still sending"

curl -fsS -X DELETE "$API/streams/live/$NAME" >"$RUN/delete.json"

grep -q '"deleted":true' "$RUN/delete.json" \
    || { echo "the delete did not report the stream gone" >&2
         cat "$RUN/delete.json" >&2; exit 1; }

echo "== and created again under the same name"

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"application\":\"live\",\"name\":\"$NAME\"}" "$API/streams")"

[ "$STATUS" = "201" ] \
    || { echo "the stream could not be created again: $STATUS" >&2; exit 1; }

# the publisher's session is still up and its frames are still on the wire
kill -0 "$PUB" 2>/dev/null \
    || { echo "the publisher had already gone away: the case proves nothing" >&2
         cat "$RUN/pub.log" >&2; exit 1; }

sleep 5

STATE="$(curl -fsS "$API/streams/live/$NAME")"

NEW_INCARNATION="$(field "$STATE" incarnation)"
FRAMES="$(field "$STATE" program_frames)"
SOURCES="$(field "$STATE" sources)"

echo "   the new incarnation holds: incarnation=${NEW_INCARNATION:-missing}"
echo "frames=${FRAMES:-0} sources=${SOURCES:-?}"

[ -n "$NEW_INCARNATION" ] && [ "$NEW_INCARNATION" != "$INCARNATION" ] \
    || { echo "the recreated stream reused its incarnation" >&2
         printf '%s\n' "$STATE" >&2; exit 1; }

# the old session's frames belong to the stream that was deleted: they are
# dropped at the owner, because the routed slot went with it, and the new
# incarnation stays empty until a publisher opens against it
[ "${FRAMES:-1}" = "0" ] \
    || { echo "the old publisher's frames reached the new stream" >&2
         printf '%s\n' "$STATE" >&2; exit 1; }

printf '%s' "$SOURCES" | grep -q '"frames_in":[1-9]' \
    && { echo "the old publisher's source is attached to the new stream" >&2
         printf '%s\n' "$STATE" >&2; exit 1; }

echo "   nothing from the old incarnation reached it"

trap - EXIT
cleanup

echo "== incarnation ok"

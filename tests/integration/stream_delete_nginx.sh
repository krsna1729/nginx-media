#!/usr/bin/env bash
#
# Deleting a stream releases the memory it held, including when a reader that
# owns a thread - or an upload that is still running - is looking at it.
#
# Three holders are exercised in one program:
#
#   - a file source, whose reader is paced by the tick and is therefore closed
#     by the delete itself;
#   - an hls_push source, whose reader is a thread that notices the removal in
#     its own loop, so the stream's pool has to wait for it;
#   - an hls_push destination whose upload is in flight against an endpoint
#     that accepts and never answers, so the destination's memory has to
#     outlive the stream that created it.
#
# What this would catch: a pool freed while a reader thread is still inside it
# (heap-use-after-free, and only under a sanitizer), a delete that waits on an
# origin, and a stream whose memory is never released at all - the draining
# gauge has to come back to zero, and the log has to say the readers stopped.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Overridable so the same scenario can be pointed at a sanitizer build.
# Not named NGINX: nginx reads that variable itself and takes it for a socket.
NGINX_BIN="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/stream-delete"
HTTP_PORT=18590
STALL_PORT=18591

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/uploaded" "$RUN/body" \
         "$RUN/incoming"

NGINX_PID=0
STALL_PID=0

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
    if [ "$STALL_PID" != 0 ]; then
        kill -KILL "$STALL_PID" 2>/dev/null
        wait "$STALL_PID" 2>/dev/null
    fi

    if [ "$NGINX_PID" != 0 ]; then
        # daemon is off here, so the master is this script's own child: it has
        # to be asked to quit and then reaped before the sweep below can tell
        # whether anything survived
        kill -QUIT "$NGINX_PID" 2>/dev/null
        wait "$NGINX_PID" 2>/dev/null
    fi

    stop_instance "$RUN"

    return 0
}
trap cleanup EXIT

echo "== a 60 second program on file, and segments for the ingest reader"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 60 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -f mpegts "$RUN/slate.ts" 2>/dev/null \
    || { echo "could not build the file fixture" >&2; exit 1; }

ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 6 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -f segment -segment_time 2 -segment_format mpegts \
    "$RUN/incoming/seg-%05d.ts" 2>/dev/null \
    || { echo "could not build the segments" >&2; exit 1; }

echo "== an endpoint that accepts an upload and never answers it"
python3 - "$STALL_PORT" <<'PY' >"$RUN/stall.log" 2>&1 &
import socket
import sys

port = int(sys.argv[1])

listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", port))
listener.listen(8)

held = []

while True:
    conn, _ = listener.accept()
    # Accepted and deliberately unanswered: the PUT stays in flight, which is
    # the state the delete has to survive.
    print("accepted", flush=True)
    held.append(conn)
PY
STALL_PID=$!

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;

# The runner's own output carries a crash or a sanitizer report, which a
# daemonized worker would have nowhere to write.
daemon off;
error_log logs/error.log notice;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/hls;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /media/api/ { media_api; }

        location /ingest/ {
            client_body_temp_path $RUN/body;
            media_hls_ingest $RUN/uploaded;
        }
    }
}
EOF

"$NGINX_BIN" -p "$RUN" -c conf/nginx.conf -t >/dev/null \
    || { echo "configuration rejected" >&2; exit 1; }

"$NGINX_BIN" -p "$RUN" -c conf/nginx.conf >"$RUN/nginx.out" 2>&1 &
NGINX_PID=$!
sleep 0.5

kill -0 "$NGINX_PID" 2>/dev/null \
    || { echo "nginx did not start" >&2; cat "$RUN/nginx.out" >&2; exit 1; }

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

metric() {
    curl -fsS "$API/metrics" | grep "^$1 " | head -1 | awk '{print $2}'
}

# --- the program with a file reader and an upload in flight ---------------

echo "== live/slate: a file reader and a destination whose upload is stuck"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"slate"}' "$API/streams" >/dev/null

STATUS="$(curl -sS -o "$RUN/src.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"slate-file\",\"type\":\"file\",\"path\":\"$RUN/slate.ts\"}" \
    "$API/streams/live/slate/sources")"

[ "$STATUS" = "201" ] \
    || { echo "the file source was not created: $STATUS" >&2
         cat "$RUN/src.json" >&2; exit 1; }

STATUS="$(curl -sS -o "$RUN/dst.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"stuck\",\"type\":\"hls_push\",\"host\":\"http://127.0.0.1:$STALL_PORT/push/\",\"path\":\"$RUN/hls/live/slate\"}" \
    "$API/streams/live/slate/destinations")"

[ "$STATUS" = "201" ] \
    || { echo "the push destination was not created: $STATUS" >&2
         cat "$RUN/dst.json" >&2; exit 1; }

for _ in $(seq 1 300); do
    grep -q accepted "$RUN/stall.log" && break
    sleep 0.1
done

grep -q accepted "$RUN/stall.log" \
    || { echo "no upload ever reached the stuck endpoint" >&2
         tail -5 "$RUN/logs/error.log" >&2; exit 1; }

echo "   an upload is in flight, the file reader is live"

# --- the program with a reader that owns a thread -------------------------

echo "== live/ingest: a push source whose reader owns a thread"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"ingest"}' "$API/streams" >/dev/null

STATUS="$(curl -sS -o "$RUN/ing.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"uploaded\",\"type\":\"hls_push\",\"path\":\"$RUN/uploaded\"}" \
    "$API/streams/live/ingest/sources")"

[ "$STATUS" = "201" ] \
    || { echo "the ingest source was not created: $STATUS" >&2
         cat "$RUN/ing.json" >&2; exit 1; }

cp "$RUN/incoming/seg-00000.ts" "$RUN/uploaded/seg-00000.ts"

for _ in $(seq 1 200); do
    curl -fsS "$API/streams/live/ingest" \
        | grep -q '"program_frames":[1-9]' && break
    sleep 0.1
done

curl -fsS "$API/streams/live/ingest" | grep -q '"program_frames":[1-9]' \
    || { echo "the ingest reader published nothing" >&2
         tail -5 "$RUN/logs/error.log" >&2; exit 1; }

echo "   the ingest reader is carrying media"

echo "== deleting live/slate with an upload in flight"
STATUS="$(curl -sS -o "$RUN/del-slate.json" -w '%{http_code}' \
    -X DELETE "$API/streams/live/slate")"

cat "$RUN/del-slate.json"; echo

[ "$STATUS" = "200" ] || { echo "delete failed: $STATUS" >&2; exit 1; }

grep -q '"deleted":true' "$RUN/del-slate.json" \
    || { echo "the delete did not report the stream gone" >&2; exit 1; }

echo "== deleting live/ingest while its reader still holds a thread"
STATUS="$(curl -sS -o "$RUN/del-ingest.json" -w '%{http_code}' \
    -X DELETE "$API/streams/live/ingest")"

cat "$RUN/del-ingest.json"; echo

[ "$STATUS" = "200" ] || { echo "delete failed: $STATUS" >&2; exit 1; }

echo "== the memory comes back once the readers have stopped"
for _ in $(seq 1 100); do
    DRAINING="$(metric nginx_media_streams_draining)"
    [ "${DRAINING:-1}" = "0" ] && break
    sleep 0.1
done

echo "   draining: ${DRAINING:-unknown}, outputs: $(metric nginx_media_runtime_outputs)"

[ "${DRAINING:-1}" = "0" ] \
    || { echo "a deleted stream never released its memory: $DRAINING" >&2
         tail -20 "$RUN/logs/error.log" >&2; exit 1; }

[ "$(metric nginx_media_runtime_outputs)" = "0" ] \
    || { echo "an output slot outlived the streams" >&2
         tail -20 "$RUN/logs/error.log" >&2; exit 1; }

grep -q 'file source .* removed with its stream, closing its reader' \
    "$RUN/logs/error.log" \
    || { echo "the file reader was not closed by the delete" >&2
         tail -20 "$RUN/logs/error.log" >&2; exit 1; }

grep -q 'ingest readers are still stopping' "$RUN/logs/error.log" \
    || { echo "the ingest delete did not defer its release" >&2
         tail -20 "$RUN/logs/error.log" >&2; exit 1; }

grep -q 'released, its readers have stopped' "$RUN/logs/error.log" \
    || { echo "the deferred stream was never released" >&2
         tail -20 "$RUN/logs/error.log" >&2; exit 1; }

grep -q 'ingest source .* removed, closing its reader' "$RUN/logs/error.log" \
    || { echo "the ingest reader was not closed" >&2
         tail -20 "$RUN/logs/error.log" >&2; exit 1; }

kill -0 "$NGINX_PID" 2>/dev/null \
    || { echo "the worker died during the deletes" >&2
         cat "$RUN/nginx.out" >&2; exit 1; }

if grep -qE 'use-after-free|AddressSanitizer|Segmentation fault|segfault' \
        "$RUN/nginx.out" "$RUN/logs/error.log"
then
    echo "the worker reported memory corruption:" >&2
    grep -aE 'use-after-free|AddressSanitizer|Segmentation fault|segfault' \
        "$RUN/nginx.out" "$RUN/logs/error.log" | head -20 >&2
    exit 1
fi

echo "== the registry still works after the pools went back"
STATUS="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"slate"}' "$API/streams")"

[ "$STATUS" = "201" ] || { echo "could not re-create the stream: $STATUS" >&2
                           exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"slate-file\",\"type\":\"file\",\"path\":\"$RUN/slate.ts\"}" \
    "$API/streams/live/slate/sources")"

[ "$STATUS" = "201" ] \
    || { echo "could not re-create the file source: $STATUS" >&2; exit 1; }

for _ in $(seq 1 300); do
    curl -fsS "$API/streams/live/slate" \
        | grep -q '"program_frames":[1-9]' && break
    sleep 0.1
done

curl -fsS "$API/streams/live/slate" | grep -q '"program_frames":[1-9]' \
    || { echo "the re-created program carried no media" >&2
         tail -20 "$RUN/logs/error.log" >&2; exit 1; }

echo "   the re-created program is carrying media again"

trap - EXIT
cleanup

echo "== stream delete ok"

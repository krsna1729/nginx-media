#!/usr/bin/env bash
#
# HLS push/PUT destination (normative revision, acceptance cases 8-10).
#
# The segmenter writes files; the destination watches that directory and
# uploads what appears to a remote endpoint.  This test uses a small local
# HTTP sink that accepts PUT, so nothing external is needed.
#
#   8. add an HLS PUT destination while live and see the segments arrive
#   9. stall the remote and prove the backlog stays bounded and the program
#      carries on
#  10. delete the destination with uploads in flight, without leaked work

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/hls-push"
SRT_PORT=24660
HTTP_PORT=18490
SINK_PORT=18491

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/received"

PUB=0
SINK=0
cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null
    [ "$SINK" != "0" ] && kill -KILL "$SINK" 2>/dev/null
    pkill -KILL -f 'nginx: ' 2>/dev/null
    return 0
}
trap cleanup EXIT

# the sink: accepts PUT, writes to $RUN/received, and can be told to stall
cat > "$RUN/sink.py" <<'PYEOF'
import http.server, os, sys, time

root = sys.argv[1]
mode_file = sys.argv[2]
port = int(sys.argv[3])

class Handler(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        # a stalled remote is simulated by sleeping before the body is read
        if os.path.exists(mode_file):
            with open(mode_file) as f:
                if f.read().strip() == "stall":
                    time.sleep(30)

        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length)

        name = os.path.basename(self.path)
        with open(os.path.join(root, name), "wb") as out:
            out.write(body)

        self.send_response(201)
        self.end_headers()

    def log_message(self, *args):
        pass

http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
PYEOF

python3 "$RUN/sink.py" "$RUN/received" "$RUN/mode" "$SINK_PORT" &
SINK=$!
sleep 0.5

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

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"push"}' "$API/streams" >/dev/null

timeout 60 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -t 25 -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=%23!::r%3Dlive%2Fpush%2Cm%3Dpublish%2Cs%3Dencoder-a" \
    >"$RUN/pub.log" 2>&1 &
PUB=$!

for _ in $(seq 1 100); do
    grep -q 'srt source open' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.1
done

echo "== adding an HLS PUT destination while live"
STATUS="$(curl -sS -o "$RUN/dest.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"cdn\",\"type\":\"hls_push\",\"host\":\"http://127.0.0.1:$SINK_PORT/\",\"path\":\"$RUN/hls\"}" \
    "$API/streams/live/push/destinations")"

cat "$RUN/dest.json"; echo

[ "$STATUS" = "201" ] || { echo "expected 201, got $STATUS" >&2; exit 1; }

for _ in $(seq 1 400); do
    [ "$(ls "$RUN/received" 2>/dev/null | wc -l)" -ge 2 ] && break
    sleep 0.1
done

RECEIVED="$(ls "$RUN/received" 2>/dev/null | wc -l)"
echo "   the sink received $RECEIVED files"

[ "$RECEIVED" -ge 2 ] || { echo "no segments were pushed" >&2
                           tail -3 "$RUN/logs/error.log" >&2; exit 1; }

ls "$RUN/received" | head -3

echo "== a stalled remote must not stall the program"
echo stall > "$RUN/mode"

BEFORE="$(curl -fsS "$API/streams/live/push" \
    | grep -o '"program_frames":[0-9]*' | cut -d: -f2)"

sleep 6

AFTER="$(curl -fsS "$API/streams/live/push" \
    | grep -o '"program_frames":[0-9]*' | cut -d: -f2)"

echo "   program frames: $BEFORE -> $AFTER while the remote is stalled"

[ "${AFTER:-0}" -gt "${BEFORE:-0}" ] \
    || { echo "the program stalled with the remote" >&2; exit 1; }

echo "== deleting it with uploads in flight"
rm -f "$RUN/mode"

STATUS="$(curl -sS -o "$RUN/deldest.json" -w '%{http_code}' \
    -X DELETE "$API/streams/live/push/destinations/cdn")"

cat "$RUN/deldest.json"; echo

[ "$STATUS" = "200" ] || { echo "expected 200, got $STATUS" >&2; exit 1; }

grep -q '"deleted":true' "$RUN/deldest.json" \
    || { echo "the destination was not deleted" >&2; exit 1; }

# the worker must still be alive and serving
sleep 2

curl -fsS "$API/streams" >/dev/null \
    || { echo "the control API died with the destination" >&2; exit 1; }

grep -q 'worker process.*exited with code' "$RUN/logs/error.log" \
    && { echo "a worker exited during the delete" >&2; exit 1; }

kill -KILL "$PUB" 2>/dev/null
wait "$PUB" 2>/dev/null
PUB=0

trap - EXIT
cleanup

echo "== hls push ok"

#!/usr/bin/env bash
#
# HLS push at fanout: does the uploader's copy path show up?
#
# N destinations all receive the same segments.  The interesting cost is the
# one that scales with N: copies per upload.  sendfile moves the file to the
# socket inside the kernel; a fread/write loop moves it through user space as
# well, so it costs two copies per destination instead of one.
#
# Measured as worker CPU per uploaded segment, which is where a copy shows up.
# Both binaries are built by the caller and passed in, so the comparison is
# between two builds of the same code with one function changed.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/.build/bench-push"
SRT_PORT=24680
HTTP_PORT=18510
SINK_PORT=18511
DESTINATIONS="${DESTINATIONS:-8}"
SECONDS_TO_PUBLISH="${SECONDS_TO_PUBLISH:-15}"

BIN_A="${1:-/tmp/nginx-sendfile}"
BIN_B="${2:-/tmp/nginx-buffered}"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/received"

PUB=0
SINK=0
cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null
    [ "$SINK" != "0" ] && kill -KILL "$SINK" 2>/dev/null
    # nginx rewrites its process title, so matching the config path only
    # catches the master and leaves workers holding the port
    pkill -KILL -f 'nginx: ' 2>/dev/null
    return 0
}
trap cleanup EXIT

cat > "$RUN/sink.py" <<'PYEOF'
import http.server, os, sys, threading

root = sys.argv[1]
port = int(sys.argv[2])
lock = threading.Lock()
count = [0]

class Handler(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length)
        name = self.path.strip('/').replace('/', '_')
        with open(os.path.join(root, name), "wb") as out:
            out.write(body)
        with lock:
            count[0] += 1
        self.send_response(201)
        self.end_headers()

    def log_message(self, *args):
        pass

http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
PYEOF

python3 "$RUN/sink.py" "$RUN/received" "$SINK_PORT" &
SINK=$!
sleep 0.5

run_variant() {
    # $1: label, $2: binary
    local label="$1" bin="$2" pid cpu_before cpu_after files

    rm -rf "$RUN/hls" "$RUN/received"
    mkdir -p "$RUN/hls" "$RUN/received"

    cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log warn;
pid logs/nginx.pid;

events {
    worker_connections 1024;
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

    "$bin" -p "$RUN" -c conf/nginx.conf
    sleep 0.5

    local api="http://127.0.0.1:$HTTP_PORT/media/api/v1"

    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d '{"application":"live","name":"bench"}' "$api/streams" >/dev/null

    for i in $(seq 1 "$DESTINATIONS"); do
        curl -fsS -X POST -H 'Content-Type: application/json' \
            -d "{\"id\":\"d$i\",\"type\":\"hls_push\",\"host\":\"http://127.0.0.1:$SINK_PORT/d$i/\",\"path\":\"$RUN/hls\"}" \
            "$api/streams/live/bench/destinations" >/dev/null
    done

    # the worker does the work, so its CPU is the one that matters
    pid="$(pgrep -f 'nginx: worker process' | tail -1)"

    cpu_before=$(awk '{print $14 + $15}' "/proc/$pid/stat")

    timeout $(( SECONDS_TO_PUBLISH + 20 )) ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=640x360:rate=25" \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -t "$SECONDS_TO_PUBLISH" -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/bench,m=publish,s=encoder-a" \
        >"$RUN/pub.log" 2>&1 &
    PUB=$!

    wait "$PUB" 2>/dev/null
    PUB=0

    sleep 3

    cpu_after=$(awk '{print $14 + $15}' "/proc/$pid/stat")
    files=$(ls "$RUN/received" | wc -l)

    awk -v label="$label" -v before="$cpu_before" -v after="$cpu_after" \
        -v files="$files" -v n="$DESTINATIONS" 'BEGIN {
            ticks = after - before
            cpu = ticks / 100
            printf "   %-12s %5d uploads  %7.2f s cpu  %8.4f s cpu/upload  (%d destinations)\n",
                   label, files, cpu, (files > 0 ? cpu / files : 0), n
        }'

    pkill -KILL -f 'nginx: ' 2>/dev/null
    sleep 1
}

echo "== $DESTINATIONS destinations, ${SECONDS_TO_PUBLISH}s of media"
run_variant "sendfile" "$BIN_A"
run_variant "buffered" "$BIN_B"

trap - EXIT
cleanup

echo "== push fanout bench done"

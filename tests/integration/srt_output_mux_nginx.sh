#!/usr/bin/env bash
#
# SRT output multiplexer groups, against the real SRT library.
#
# A destination connects in its logical lane's multiplexer group: the lane's
# destinations share one local UDP endpoint, and the library runs one send and
# one receive thread for it instead of a pair per destination.  What this
# checks:
#
#   1. forty destinations run on at most one library send thread per lane
#      (plus the ingest listener's), not forty - the thread count follows the
#      lanes, not the fanout;
#   2. every destination carries media;
#   3. deleting every destination and adding them again works: once a group's
#      last session is gone its port is forgotten (another process may own it
#      by then), and the new sessions connect on fresh endpoints and carry
#      media again;
#   4. destinations churned one at a time while the others run keep working,
#      which is the path where a group's port is reused by a live endpoint.
#
# The receiver is the capacity benchmark's SRT fanout sink: one process that
# accepts every destination by its s= stream id and counts bytes per
# destination.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/srt-output-mux"
BASE=$(( 21000 + ($$ % 40) * 16 ))
HTTP_PORT=$BASE
SINK_PORT=$(( BASE + 4 ))
COUNT=40
LANES=16

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/media"

SINK_PID=""

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
    [ -n "$SINK_PID" ] && kill -TERM "$SINK_PID" 2>/dev/null
    stop_instance
    return 0
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    tail -20 "$RUN/logs/error.log" >&2
    exit 1
}

pkg-config --exists srt || { echo "libsrt development files are required"; exit 1; }
read -r -a cflags <<< "$(pkg-config --cflags srt)"
read -r -a libs <<< "$(pkg-config --libs srt)"
cc -O2 -Wall -Wextra -Werror -std=c11 "${cflags[@]}" \
    "$ROOT/tests/bench/srt_fanout_sink.c" -o "$RUN/sink" "${libs[@]}" \
    || fail "could not build the SRT fanout sink"

ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 60 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -f mpegts "$RUN/media/source.ts" 2>/dev/null \
    || fail "could not build the source"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 512; }

http {
    access_log off;
    server {
        listen 127.0.0.1:$HTTP_PORT;
        location /media/api/ { media_api; }
    }
}
EOF

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

"$RUN/sink" "$SINK_PORT:4" "$COUNT" "$RUN/sink.ready" "$RUN/sink.csv" quality \
    >"$RUN/sink.log" 2>&1 &
SINK_PID=$!

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 || fail "config rejected"
"$NGINX" -p "$RUN" -c conf/nginx.conf || fail "nginx did not start"
sleep 0.5
WORKER="$(pgrep -P "$(cat "$RUN/logs/nginx.pid")" | head -1)"

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"mux"}' "$API/streams" >/dev/null \
    || fail "stream not created"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"id":"file1","type":"file","path":"'"$RUN"'/media/source.ts","priority":10}' \
    "$API/streams/live/mux/sources" >/dev/null || fail "file source not added"

add_destination() {   # <index>
    local id port
    id="$(printf 'd%04d' "$1")"
    port=$(( SINK_PORT + $1 % 4 ))
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"id\":\"$id\",\"type\":\"srt\",\"host\":\"127.0.0.1\",\"port\":$port,\"streamid\":\"#!::r=live/mux,m=publish,s=$id\"}" \
        "$API/streams/live/mux/destinations" >/dev/null
}

delete_destination() {   # <index>
    curl -fsS -X DELETE \
        "$API/streams/live/mux/destinations/$(printf 'd%04d' "$1")" >/dev/null
}

# bytes per destination, from a fresh receiver snapshot
snapshot() {   # <output>
    rm -f "$RUN/sink.csv.snapshot"
    kill -USR1 "$SINK_PID" || fail "sink exited"
    for _ in $(seq 1 100); do
        [ -s "$RUN/sink.csv.snapshot" ] && break
        sleep 0.05
    done
    tail -n +2 "$RUN/sink.csv.snapshot" | cut -d, -f1,2 | sort > "$1"
}

# every destination's byte count grew between two snapshots
all_grew() {   # <before> <after>
    join -t, "$1" "$2" | awk -F, -v n="$COUNT" '
        $3 > $2 { grew++ } END { exit !(grew == n) }'
}

wait_all_growing() {   # <label>
    local attempt
    for attempt in $(seq 1 30); do
        snapshot "$RUN/a.csv"
        sleep 1
        snapshot "$RUN/b.csv"
        all_grew "$RUN/a.csv" "$RUN/b.csv" && return 0
    done
    fail "$1: not every destination carried media"
}

sndq_threads() {
    local n=0 t
    for t in /proc/"$WORKER"/task/*/comm; do
        case "$(cat "$t" 2>/dev/null)" in SRT:SndQ*) n=$(( n + 1 )) ;; esac
    done
    echo "$n"
}

echo "== $COUNT SRT destinations in lane multiplexer groups"
for i in $(seq 0 $(( COUNT - 1 ))); do
    add_destination "$i" || fail "destination $i not added"
done
for _ in $(seq 1 300); do
    [ -f "$RUN/sink.ready" ] && break
    sleep 0.1
done
[ -f "$RUN/sink.ready" ] || fail "the sink did not see all $COUNT destinations"
wait_all_growing "initial"

threads="$(sndq_threads)"
echo "   library send threads in the worker: $threads for $COUNT destinations"
[ "$threads" -ge 1 ] && [ "$threads" -le "$LANES" ] \
    || fail "expected at most one send thread per lane ($LANES), got $threads"

echo "== every destination removed and added again"
for i in $(seq 0 $(( COUNT - 1 ))); do
    delete_destination "$i" || fail "destination $i not deleted"
done
sleep 2
for i in $(seq 0 $(( COUNT - 1 ))); do
    add_destination "$i" || fail "destination $i not re-added"
done
wait_all_growing "re-added"
threads="$(sndq_threads)"
echo "   library send threads after re-adding: $threads"
[ "$threads" -le "$LANES" ] \
    || fail "send threads grew past one per lane after re-adding: $threads"

echo "== destinations churned one at a time while the others run"
for i in 0 1 2 17 33; do
    delete_destination "$i" || fail "churn delete $i"
    add_destination "$i" || fail "churn add $i"
done
wait_all_growing "churned"

if grep -qE '\[(alert|emerg)\]|signal [0-9]+|AddressSanitizer' "$RUN/logs/error.log"; then
    fail "worker reported an alert or crash"
fi

echo "== srt output multiplexer groups ok"

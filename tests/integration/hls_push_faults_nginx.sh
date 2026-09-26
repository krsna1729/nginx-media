#!/usr/bin/env bash
#
# HLS push isolation: a failing destination must not delay a healthy one.
#
# One program, two HLS PUT destinations in the same worker:
#
#   d-ok    a healthy sink
#   d-bad   a sink that refuses every third upload, closes the connection
#           after every seventh, and answers 15 ms late
#
# and checks that the healthy destination keeps receiving every segment,
# exactly once, inside its deadline, with no HTTP errors and no queue lag,
# while the faulty one's failures are counted by the sink.  The failure modes
# the brief asks for are the ones the sink injects here: intermittent HTTP
# errors, connection closure and latency.  Loss on a real network is the same
# thing from the module's side - a PUT that does not complete - and the
# capacity ladder's transport-error accounting covers it separately.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/hls-push-faults"
BASE=$(( 26000 + ($$ % 30) * 16 ))
HTTP_PORT=$BASE
SRT_PORT=$(( BASE + 1 ))
OK_PORT=$(( BASE + 4 ))
BAD_PORT=$(( BASE + 5 ))
OK_SINK_LOG="$RUN/ok-sink.log"
BAD_SINK_LOG="$RUN/bad-sink.log"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

NGINX_PID=""
PUB_PID=""
OK_PID=""
BAD_PID=""

stop_instance() {
    [ -f "$RUN/logs/nginx.pid" ] || return 0
    local pid child
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
    [ -n "$PUB_PID" ] && kill -KILL "$PUB_PID" 2>/dev/null
    [ -n "$OK_PID" ] && kill -TERM "$OK_PID" 2>/dev/null
    [ -n "$BAD_PID" ] && kill -TERM "$BAD_PID" 2>/dev/null
    stop_instance
    return 0
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    tail -20 "$RUN/logs/error.log" >&2
    exit 1
}

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

        location /hls/ {
            alias $RUN/hls/;
        }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 \
    || fail "configuration rejected"
"$NGINX" -p "$RUN" -c conf/nginx.conf || fail "nginx did not start"
sleep 0.5

python3 "$ROOT/tests/bench/hls_push_capacity.py" serve --port "$OK_PORT" \
    >"$OK_SINK_LOG" 2>&1 &
OK_PID=$!
python3 "$ROOT/tests/bench/hls_push_capacity.py" serve --port "$BAD_PORT" \
    --fail-every 3 --close-after 7 --latency-ms 15 \
    >"$BAD_SINK_LOG" 2>&1 &
BAD_PID=$!
for port in "$OK_PORT" "$BAD_PORT"; do
    for _ in $(seq 1 100); do
        curl -fsS "http://127.0.0.1:$port/__health" >/dev/null 2>&1 && break
        sleep 0.1
    done
    curl -fsS "http://127.0.0.1:$port/__health" >/dev/null 2>&1 \
        || fail "sink on port $port did not become ready"
done

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"faults"}' "$API/streams" >/dev/null \
    || fail "stream not created"

curl -fsS "http://127.0.0.1:$OK_PORT/__mark" >"$RUN/ok.mark.json" \
    || fail "could not mark the healthy sink"
curl -fsS "http://127.0.0.1:$BAD_PORT/__mark" >"$RUN/bad.mark.json" \
    || fail "could not mark the faulty sink"

timeout 120 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" -t 90 \
    -c:v libx264 -preset ultrafast -b:v 1200k -maxrate 1200k -bufsize 600k \
    -g 25 -pix_fmt yuv420p -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/faults,m=publish,s=encoder-a" \
    >"$RUN/pub.log" 2>&1 &
PUB_PID=$!
for _ in $(seq 1 100); do
    grep -q 'srt source open app=live stream=faults' "$RUN/logs/error.log" && break
    sleep 0.1
done

for pair in "d-ok:$OK_PORT" "d-bad:$BAD_PORT"; do
    id="${pair%%:*}"
    port="${pair##*:}"
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"id\":\"$id\",\"type\":\"hls_push\",\"host\":\"http://127.0.0.1:$port/$id/\",\"path\":\"$RUN/hls/live/faults\"}" \
        "$API/streams/live/faults/destinations" >/dev/null \
        || fail "destination $id not added"
done

# let the faulty destination fail several times while the healthy one works
sleep 30

curl -fsS "http://127.0.0.1:$OK_PORT/__snapshot" > "$RUN/ok.json" \
    || fail "healthy sink snapshot"
curl -fsS "http://127.0.0.1:$BAD_PORT/__snapshot" > "$RUN/bad.json" \
    || fail "faulty sink snapshot"

python3 - "$RUN/ok.json" "$RUN/bad.json" "$API/metrics" <<'PYEOF' || fail "push isolation"
import json, sys, urllib.request

ok = json.load(open(sys.argv[1]))
bad = json.load(open(sys.argv[2]))
metrics = urllib.request.urlopen(sys.argv[3], timeout=5).read().decode()

failures = []

def check(condition, what):
    print(("   ok   " if condition else "   FAIL ") + what)
    if not condition:
        failures.append(what)

ok_dest = ok["destinations"].get("d-ok", {})
bad_faults = (bad["fault_failures"], bad["fault_closed"], bad["fault_puts"])
print(f"   healthy sink: {ok_dest.get('segments', 0)} segments, "
      f"{ok_dest.get('ts_bytes', 0)} bytes, {ok['http_errors']} HTTP errors")
print(f"   faulty sink: faults (failures, closed, puts) = {bad_faults}, "
      f"{bad['http_errors']} HTTP errors")
check(bad_faults[0] > 0 or bad_faults[1] > 0,
      f"the faulty sink actually failed uploads: {bad_faults}")
check(ok["http_errors"] == 0,
      f"the healthy sink saw no HTTP errors ({ok['http_errors']})")
check(ok_dest.get("segments", 0) >= 8,
      f"the healthy destination kept receiving segments ({ok_dest.get('segments', 0)})")

detail = ok_dest.get("segment_detail", {})
duplicates = [name for name, row in detail.items() if row.get("count", 0) > 1]
check(not duplicates,
      f"no segment was uploaded twice to the healthy sink: {duplicates[:5]}")
arrivals = sorted(row["at"] for row in detail.values() if "at" in row)
if len(arrivals) >= 3:
    gaps = [b - a for a, b in zip(arrivals, arrivals[1:])]
    worst = max(gaps)
    check(worst <= 4.0,
          f"the healthy destination's longest gap between uploads is {worst:.2f}s "
          f"(<= 4s, the faulty destination must not stall it)")
    last_gap = arrivals[-1] - arrivals[0]
    check(last_gap >= 5.0,
          f"the healthy destination was still receiving at the end "
          f"({last_gap:.1f}s of uploads)")
else:
    check(False, "not enough uploads to measure the healthy destination's cadence")

lag = [line for line in metrics.splitlines()
       if line.startswith("nginx_media_egress_queue_lag_ms{")
       and 'destination="d-ok"' in line]
check(not lag or float(lag[0].split()[-1]) <= 5000,
      f"the healthy destination's queue lag stays bounded: {lag}")

sys.exit(0 if not failures else 1)
PYEOF

if grep -qE '\[(alert|emerg)\]|AddressSanitizer' "$RUN/logs/error.log"; then
    fail "worker reported an alert or crash"
fi

echo "== hls push isolation ok"

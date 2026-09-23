#!/usr/bin/env bash
#
# Acceptance case 14: churn streams, sources and destinations during fanout
# load without violating the health/fanout deadline.
#
# Conditions (goal doc 28 - a number without them means nothing):
#   one worker, single host, no netem, no CPU contention beyond the churn
#   a paced file source, 25 fps, 320x240, MPEG-TS
#   three consumers draining the same program feed: HLS, an SRT destination
#   and an RTMP destination
#   graph mutations every 250ms for the duration
#
# The metric is the document's own: fanout_delay = dispatch_time -
# program_publish_time, reported at p99.  The deadline here is 500ms, which is
# a live-media budget rather than a measured one - the point of the case is
# that churn does not move it, not that this machine is fast.
#
# What this would catch: a mutation path that holds a lock the tick needs, a
# teardown that walks a list the fanout is reading, an allocation on the
# dispatch path that the churn makes expensive.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/churn"
HTTP_PORT=18580
RTMP_PORT=18582
SINK_SRT_PORT=18583
DEADLINE_MS=500

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/media" "$RUN/sink/conf" \
         "$RUN/sink/logs"

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
    stop_instance "$RUN"
    stop_instance "$RUN/sink"
    return 0
}
trap cleanup EXIT

echo "== a program carried to three consumers while the graph is churned"

# paced media: a file source reads one chunk per tick, so the program advances
# in real time rather than arriving in a burst
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 40 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 32k -f mpegts "$RUN/media/source.ts" 2>/dev/null

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 512; }

# The main instance listens on nothing: its destinations connect outward to
# the sink.  Listening on the same port a destination targets would make it
# publish to itself, which is a test bug that looks like a code bug.
media_hls $RUN/hls;

http {
    access_log off;
    server {
        listen 127.0.0.1:$HTTP_PORT;
        location /media/api/ { media_api; }
        location /hls/ { alias $RUN/hls/; }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || exit 1
"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

# A second nginx receives both destinations.  Each one drains the same
# program feed, which is the fanout this case is about.
cat > "$RUN/sink/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen 127.0.0.1:$SINK_SRT_PORT;
media_rtmp_listen 127.0.0.1:$RTMP_PORT;
EOF

"$NGINX" -p "$RUN/sink" -c conf/nginx.conf -t >/dev/null || exit 1
"$NGINX" -p "$RUN/sink" -c conf/nginx.conf
sleep 0.5

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"main"}' "$API/streams" >/dev/null

# The consumers are established loudly: a destination that failed to start
# would make the whole case meaningless, and silencing the create is how that
# goes unnoticed.
# The local HLS output is not a destination: it belongs to the stream and is
# always draining the feed.  So the three consumers here are that output plus
# the two socket destinations below.
for d in '{"id":"srt1","type":"srt","host":"127.0.0.1","port":'"$SINK_SRT_PORT"'}' \
         '{"id":"rtmp1","type":"rtmp","host":"127.0.0.1","port":'"$RTMP_PORT"'}'
do
    STATUS="$(curl -sS -o "$RUN/dest.json" -w '%{http_code}' \
        -X POST -H 'Content-Type: application/json' -d "$d" \
        "$API/streams/live/main/destinations")"

    [ "$STATUS" = "201" ] \
        || { echo "destination was not started: $STATUS" >&2; cat "$RUN/dest.json" >&2; exit 1; }
done

STATUS="$(curl -sS -o "$RUN/src.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"id":"file1","type":"file","path":"'"$RUN"'/media/source.ts","priority":10}' \
    "$API/streams/live/main/sources")"

[ "$STATUS" = "201" ] \
    || { echo "the file source was not created: $STATUS" >&2; cat "$RUN/src.json" >&2; exit 1; }

sleep 3

echo "== churning the graph while the program is carried"
CHURN=0
for i in $(seq 1 40); do

    # a stream that comes and goes
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d '{"application":"live","name":"churn'"$i"'"}' "$API/streams" \
        >/dev/null 2>&1

    # a source added and removed under the live program
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d '{"id":"tmp'"$i"'","type":"srt","host":"127.0.0.1","port":'"$SINK_SRT_PORT"',"priority":1}' \
        "$API/streams/live/main/sources" >/dev/null 2>&1

    curl -fsS -X DELETE \
        "$API/streams/live/main/sources/tmp'"$i" >/dev/null 2>&1

    # A destination added and removed.  It points at the live sink on purpose:
    # an SRT or RTMP destination needs a peer, and a mutation that is expected
    # to fail would be testing the failure path, not the churn.
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d '{"id":"tmpd'"$i"'","type":"srt","host":"127.0.0.1","port":'"$SINK_SRT_PORT"'}' \
        "$API/streams/live/main/destinations" >/dev/null 2>&1

    curl -fsS -X DELETE \
        "$API/streams/live/main/destinations/tmpd'"$i" >/dev/null 2>&1

    curl -fsS -X DELETE "$API/streams/live/churn'"$i" >/dev/null 2>&1

    CHURN=$((CHURN + 1))
    sleep 0.25
done

echo "   churned $CHURN times"

sleep 2

echo "== the deadline"
STATS="$(curl -fsS "$API/streams/live/main")"
printf '%s\n' "$STATS" | head -c 600; echo

FANOUT="$(printf '%s' "$STATS" | grep -o '"fanout_ms":{[^}]*}' | head -1)"
echo "   $FANOUT"

P99="$(printf '%s' "$FANOUT" | sed -n 's/.*"p99":\([0-9]*\).*/\1/p')"
DISPATCHED="$(printf '%s' "$STATS" | grep -o '"dispatched":[0-9]*' | head -1 | cut -d: -f2)"

python3 - "$STATS" <<'PY'
import json
import sys

doc = json.loads(sys.argv[1])
fanout = doc["fanout_ms"]
bounds = fanout["bucket_upper_ms"]
counts = fanout["bucket_counts"]
expected = [1 << i for i in range(15)]

assert bounds[:15] == expected and bounds[15] is None, bounds
assert len(counts) == 16 and all(isinstance(n, int) and n >= 0
                                 for n in counts), counts
assert sum(counts) == doc["dispatched"], (sum(counts), doc["dispatched"])
assert isinstance(fanout["max"], int) and 0 <= fanout["max"] <= 60000
print(f"   fanout histogram: {sum(counts)} samples in {len(counts)} bins")
PY

[ -n "$P99" ] || { echo "no fanout percentile in the response" >&2; exit 1; }
[ -n "$DISPATCHED" ] && [ "$DISPATCHED" -gt 0 ] \
    || { echo "nothing was dispatched: $DISPATCHED" >&2; exit 1; }

echo "   dispatched $DISPATCHED units, p99 ${P99}ms (deadline ${DEADLINE_MS}ms)"

[ "$P99" -le "$DEADLINE_MS" ] \
    || { echo "FAIL: p99 fanout delay ${P99}ms exceeds the ${DEADLINE_MS}ms deadline" >&2; exit 1; }

echo "== the program still carries media after the churn"
PRODUCED="$(curl -fsS "$API/streams/live/main/sources" \
    | grep -o '"frames_out":[0-9]*' | head -1 | cut -d: -f2)"

echo "   frames out: $PRODUCED"

[ -n "$PRODUCED" ] && [ "$PRODUCED" -gt 100 ] \
    || { echo "the program stopped carrying media: $PRODUCED frames" >&2; exit 1; }

echo "== the other consumers are still attached"
curl -fsS "$API/streams/live/main" | grep -q '"id":"srt1"' \
    || { echo "the srt destination did not survive the churn" >&2; exit 1; }
curl -fsS "$API/streams/live/main" | grep -q '"id":"rtmp1"' \
    || { echo "the rtmp destination did not survive the churn" >&2; exit 1; }

trap - EXIT
cleanup

echo "== churn ok"

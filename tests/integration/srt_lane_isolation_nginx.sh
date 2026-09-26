#!/usr/bin/env bash
#
# SRT lane isolation: an impaired destination does not take its lane-mates
# down with it.
#
# A lane's destinations share one library endpoint - one UDP socket, one
# SRT:SndQ thread - so the failure domain of the lane multiplexer is the lane:
# a destination that loses packets, retransmits and backs up shares that
# socket and that thread with every healthy destination of its lane.  This
# puts two destinations in the same lane on purpose and impairs one of them:
#
#   d0     lane 0, through a relay that drops 30% of datagrams each way,
#          delays them, and caps the path at half the stream's bitrate - it
#          retransmits constantly and can never keep up
#   d16    lane 0 too (lanes are slot % 16), on a clean path
#   d1-15  one per other lane, on clean paths: the control
#
# and checks that d16 delivers what the control lanes deliver, that its queue
# lag stays bounded, and that the impairment really happened (the relay
# dropped and capped, d0 fell behind).  The relay is a userspace UDP proxy
# rather than netem, so it runs unprivileged, in a container, and on a kernel
# without sch_netem.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/srt-lane-isolation"
BASE=$(( 22000 + ($$ % 40) * 16 ))
HTTP_PORT=$BASE
SRT_PORT=$(( BASE + 1 ))
SINK_PORT=$(( BASE + 4 ))
RELAY_PORT=$(( BASE + 10 ))
COUNT=17
WINDOW=15

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/media"

SINK_PID=""
RELAY_PID=""
PUB_PID=""

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
    [ -n "$RELAY_PID" ] && kill -TERM "$RELAY_PID" 2>/dev/null
    [ -n "$PUB_PID" ] && kill -KILL "$PUB_PID" 2>/dev/null
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

# The stream is published live (-re), so every lane sees the same steady
# rate for the whole window; a file source plays faster than real time and
# can end inside the window, which measures a burst instead.
SOURCE_BPS=1500000
CAP_BPS=$(( SOURCE_BPS / 2 ))

# The impaired path: every datagram, both ways, is dropped with probability
# LOSS, delayed by DELAY, and sent no faster than CAP bytes/s towards the
# receiver (a token bucket that drops what does not fit, as a congested link
# would).  Counts go to a JSON file on SIGUSR1 and at exit.
cat > "$RUN/relay.py" <<'PYEOF'
import asyncio, json, random, signal, sys, time

listen_port, target_port, loss, delay, cap_bps, out = sys.argv[1:7]
listen_port, target_port = int(listen_port), int(target_port)
loss, delay, cap = float(loss), float(delay), float(cap_bps) / 8
stats = {"to_receiver": 0, "to_sender": 0, "lost": 0, "over_cap": 0}
bucket = {"tokens": cap, "at": time.monotonic()}

def dump(*_):
    with open(out, "w") as f:
        json.dump(stats, f)

class Relay(asyncio.DatagramProtocol):
    def __init__(self):
        self.client = None
        self.upstream = None
    def connection_made(self, transport):
        self.transport = transport
    def datagram_received(self, data, addr):
        if addr[1] == target_port:
            direction, dest, via = "to_sender", self.client, self.transport
        else:
            self.client = addr
            direction, dest, via = "to_receiver", ("127.0.0.1", target_port), self.transport
            now = time.monotonic()
            bucket["tokens"] = min(cap, bucket["tokens"] + (now - bucket["at"]) * cap)
            bucket["at"] = now
            if bucket["tokens"] < len(data):
                stats["over_cap"] += 1
                return
            bucket["tokens"] -= len(data)
        if dest is None:
            return
        if random.random() < loss:
            stats["lost"] += 1
            return
        stats[direction] += 1
        asyncio.get_running_loop().call_later(delay, via.sendto, data, dest)

async def main():
    loop = asyncio.get_running_loop()
    loop.add_signal_handler(signal.SIGUSR1, dump)
    stop = loop.create_future()
    loop.add_signal_handler(signal.SIGTERM, stop.set_result, None)
    await loop.create_datagram_endpoint(Relay, local_addr=("127.0.0.1", listen_port))
    await stop
    dump()

asyncio.run(main())
PYEOF

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 512; }

media_srt_listen 127.0.0.1:$SRT_PORT;

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
python3 "$RUN/relay.py" "$RELAY_PORT" "$SINK_PORT" 0.30 0.040 "$CAP_BPS" \
    "$RUN/relay.json" &
RELAY_PID=$!
sleep 0.5

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 || fail "config rejected"
"$NGINX" -p "$RUN" -c conf/nginx.conf || fail "nginx did not start"
sleep 0.5

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"iso"}' "$API/streams" >/dev/null \
    || fail "stream not created"
timeout 120 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x360:rate=25" -t 90 \
    -c:v libx264 -preset ultrafast -b:v 1200k -maxrate 1200k -bufsize 600k \
    -g 25 -pix_fmt yuv420p -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/iso,m=publish,s=enc" \
    >"$RUN/pub.log" 2>&1 &
PUB_PID=$!
for _ in $(seq 1 100); do
    grep -q 'srt source open app=live stream=iso' "$RUN/logs/error.log" && break
    sleep 0.1
done

# in slot order, so d<i> is in lane i % 16
for i in $(seq 0 $(( COUNT - 1 ))); do
    id="$(printf 'd%02d' "$i")"
    if [ "$i" -eq 0 ]; then
        port="$RELAY_PORT"
    else
        port=$(( SINK_PORT + i % 4 ))
    fi
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"id\":\"$id\",\"type\":\"srt\",\"host\":\"127.0.0.1\",\"port\":$port,\"streamid\":\"#!::r=live/iso,m=publish,s=$id\"}" \
        "$API/streams/live/iso/destinations" >/dev/null \
        || fail "destination $id not added"
done

for _ in $(seq 1 300); do
    [ -f "$RUN/sink.ready" ] && break
    sleep 0.1
done
[ -f "$RUN/sink.ready" ] || fail "the sink did not see all $COUNT destinations"

# d00 connects through the lossy path, which can take several handshake
# attempts on a slow host; measure only once it is really connected
for _ in $(seq 1 600); do
    grep -q 'srt output 0 connected' "$RUN/logs/error.log" && break
    sleep 0.1
done
grep -q 'srt output 0 connected' "$RUN/logs/error.log" \
    || fail "the impaired destination never connected"

metric() {   # <metric> <destination>
    curl -fsS "$API/metrics" \
        | awk -v m="$1" -v d="destination=\"$2\"" \
            'index($0, m "{") == 1 && index($0, d) { print $NF; exit }'
}
placement() {   # <destination>
    curl -fsS "$API/metrics" \
        | grep "^nginx_media_egress_queue_lag_ms{" | grep "destination=\"$1\"" \
        | sed -n 's/.*placement="\([0-9]*\)".*/\1/p' | head -1
}

P0="$(placement d00)"; P16="$(placement d16)"; P1="$(placement d01)"
echo "== lanes: d00=$P0 d16=$P16 d01=$P1"
[ -n "$P0" ] && [ "$P0" = "$P16" ] && [ "$P0" != "$P1" ] \
    || fail "d00 and d16 are not lane-mates (placements $P0/$P16/$P1)"

snapshot() {   # <output>
    rm -f "$RUN/sink.csv.snapshot"
    kill -USR1 "$SINK_PID" || fail "sink exited"
    for _ in $(seq 1 100); do
        [ -s "$RUN/sink.csv.snapshot" ] && break
        sleep 0.05
    done
    tail -n +2 "$RUN/sink.csv.snapshot" | cut -d, -f1,2 | sort > "$1"
}

sleep 5
snapshot "$RUN/a.csv"
sleep "$WINDOW"
snapshot "$RUN/b.csv"
kill -USR1 "$RELAY_PID"
sleep 0.3

LAG16="$(metric nginx_media_egress_queue_lag_ms d16)"
LAG0="$(metric nginx_media_egress_queue_lag_ms d00)"
LAG1="$(metric nginx_media_egress_queue_lag_ms d01)"

python3 - "$RUN/a.csv" "$RUN/b.csv" "$RUN/relay.json" "$WINDOW" \
    "${LAG0:-}" "${LAG16:-}" "${LAG1:-}" <<'PYEOF' || fail "lane isolation"
import json, statistics, sys

a_path, b_path, relay_path, window, lag0, lag16, lag1 = sys.argv[1:8]
window = float(window)

def load(path):
    rows = {}
    for line in open(path):
        name, value = line.strip().split(",")[:2]
        rows[name.split("s=")[-1]] = int(value)
    return rows

a, b = load(a_path), load(b_path)
rate = {k: (b[k] - a.get(k, 0)) * 8 / window for k in b}
control = [rate["d%02d" % i] for i in range(1, 16) if "d%02d" % i in rate]
median = statistics.median(control)
relay = json.load(open(relay_path))
ok = True

def check(cond, what):
    global ok
    print(("   ok   " if cond else "   FAIL ") + what)
    ok = ok and cond

print("   control lanes d01-d15: median %.0f bit/s, min %.0f" % (median, min(control)))
print("   d16 (lane-mate of the impaired d00): %.0f bit/s" % rate.get("d16", 0))
print("   d00 (impaired): %.0f bit/s" % rate.get("d00", 0))
print("   relay: %s" % relay)
print("   queue lag ms: d00=%s d16=%s d01=%s" % (lag0, lag16, lag1))

check(relay["lost"] + relay["over_cap"] > 200 and relay["over_cap"] > 0,
      "the impairment happened (%d lost, %d over the cap)"
      % (relay["lost"], relay["over_cap"]))
check(rate.get("d00", 0) < 0.8 * median,
      "d00 fell behind its path (%.0f%% of the control median)"
      % (100 * rate.get("d00", 0) / median))
check(max(control) <= 1.05 * median,
      "the control lanes are steady (max %.1f%% of their median)"
      % (100 * max(control) / median))
check(rate.get("d16", 0) >= 0.97 * median,
      "d16 delivered %.1f%% of the control median (>= 97%%)"
      % (100 * rate.get("d16", 0) / median))
check(rate.get("d16", 0) >= 0.97 * min(control),
      "d16 is no worse than the slowest control lane")
if lag16 not in ("", None):
    check(float(lag16) <= 1000, "d16 queue lag %s ms stays bounded (<= 1000)" % lag16)
sys.exit(0 if ok else 1)
PYEOF

if grep -qE '\[(alert|emerg)\]|signal [0-9]+|AddressSanitizer' "$RUN/logs/error.log"; then
    fail "worker reported an alert or crash"
fi

echo "== srt lane isolation ok"

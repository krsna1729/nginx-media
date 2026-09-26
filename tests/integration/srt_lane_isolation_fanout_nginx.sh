#!/usr/bin/env bash
#
# SRT lane isolation at fanout: several impaired destinations sharing one lane
# with healthy ones, plus churn and a graceful reload.
#
# The merged lane-isolation test shows that one impaired destination does not
# disturb one healthy lane-mate.  This is the same question at the scale the
# architecture is meant for: a lane is a shared UDP socket and a shared libsrt
# SndQ thread, so if the lane is a real failure domain, four destinations in
# one lane with two of them impaired will show it.
#
# Layout (64 destinations, lanes are slot % 16, four per lane):
#
#   lane 0  d00  through a relay that drops 30% each way, delays 40 ms and
#                caps the path at half the stream rate - it retransmits
#                constantly and can never keep up
#           d16  through a relay that blackholes everything for 2 s in every
#                6 s and delays 20 ms otherwise - repeated connection stalls
#           d32  healthy lane-mate (the measurement)
#           d48  healthy lane-mate (the measurement)
#   lane 1  d01 d17 d33 d49   the control lane, all on clean paths
#   lanes 2-15  the rest, on clean paths
#
# What is asserted:
#
#   * the impairment is real (the relays report loss and blackholes, and d00
#     and d16 fall behind the control lane);
#   * the healthy lane-mates deliver what the control lane delivers - no
#     correlated failure through the shared socket or the shared SndQ thread;
#   * no destination outside the two impaired ones is affected;
#   * the healthy lane-mates' queue lag, reconnects, dropped units and
#     transport errors stay clean while the lane's own retransmit counter
#     shows the cost the lane carried;
#   * churn: deleting a destination in the control lane, re-adding it and
#     reloading the worker leaves every healthy destination delivering and
#     leaves no destination or lane behind.
#
# The relays are userspace UDP proxies, not netem: the suite runs
# unprivileged, in a container, on kernels without sch_netem.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/srt-lane-isolation-fanout"
BASE=$(( 24000 + ($$ % 40) * 16 ))
HTTP_PORT=$BASE
SRT_PORT=$(( BASE + 1 ))
SINK_PORT=$(( BASE + 4 ))
RELAY_PORT=$(( BASE + 10 ))
FLAP_PORT=$(( BASE + 11 ))
COUNT=64
LANE_IMPAIRED=(0 16)
LANE_HEALTHY=(32 48)
LANE_CONTROL=(1 17 33 49)
WINDOW="${WINDOW:-20}"

SINK_PID=""
RELAY_PID=""
FLAP_PID=""
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
    [ -n "$FLAP_PID" ] && kill -TERM "$FLAP_PID" 2>/dev/null
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

SOURCE_BPS=1500000
CAP_BPS=$(( SOURCE_BPS / 2 ))

# The lossy path: every datagram, both ways, is dropped with probability LOSS,
# delayed by DELAY, and sent no faster than CAP bytes/s towards the receiver.
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

# The flapping path: blackholes everything for BLACKHOLE seconds in every
# PERIOD seconds, delaying the rest.  This is a connection that stalls and
# recovers while its lane-mates keep sending.
cat > "$RUN/relay_flap.py" <<'PYEOF'
import asyncio, json, signal, sys, time

listen_port, target_port, delay, period, blackhole, out = sys.argv[1:7]
listen_port, target_port = int(listen_port), int(target_port)
delay, period, blackhole = float(delay), float(period), float(blackhole)
stats = {"to_receiver": 0, "to_sender": 0, "blackholed": 0}
started = time.monotonic()

def dump(*_):
    with open(out, "w") as f:
        json.dump(stats, f)

def blackholed_now():
    return (time.monotonic() - started) % period < blackhole

class Relay(asyncio.DatagramProtocol):
    def __init__(self):
        self.client = None
    def connection_made(self, transport):
        self.transport = transport
    def datagram_received(self, data, addr):
        if addr[1] == target_port:
            direction, dest = "to_sender", self.client
        else:
            self.client = addr
            direction, dest = "to_receiver", ("127.0.0.1", target_port)
        if dest is None:
            return
        if blackholed_now():
            stats["blackholed"] += 1
            return
        stats[direction] += 1
        asyncio.get_running_loop().call_later(delay, self.transport.sendto,
                                              data, dest)

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

events { worker_connections 2048; }

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
python3 "$RUN/relay_flap.py" "$FLAP_PORT" "$SINK_PORT" 0.020 6 2 \
    "$RUN/relay-flap.json" &
FLAP_PID=$!
sleep 0.5

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 || fail "config rejected"
"$NGINX" -p "$RUN" -c conf/nginx.conf || fail "nginx did not start"
sleep 0.5

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"fan"}' "$API/streams" >/dev/null \
    || fail "stream not created"
timeout 900 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x360:rate=25" -t 600 \
    -c:v libx264 -preset ultrafast -b:v 1200k -maxrate 1200k -bufsize 600k \
    -g 25 -pix_fmt yuv420p -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/fan,m=publish,s=enc" \
    >"$RUN/pub.log" 2>&1 &
PUB_PID=$!
for _ in $(seq 1 100); do
    grep -q 'srt source open app=live stream=fan' "$RUN/logs/error.log" && break
    sleep 0.1
done

add_destination() {   # <slot>
    local i="$1" id port
    id="$(printf 'd%02d' "$i")"
    if [ "$i" -eq 0 ]; then
        port="$RELAY_PORT"
    elif [ "$i" -eq 16 ]; then
        port="$FLAP_PORT"
    else
        port=$(( SINK_PORT + i % 4 ))
    fi
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"id\":\"$id\",\"type\":\"srt\",\"host\":\"127.0.0.1\",\"port\":$port,\"streamid\":\"#!::r=live/fan,m=publish,s=$id\"}" \
        "$API/streams/live/fan/destinations" >/dev/null \
        || fail "destination $id not added"
}

delete_destination() {   # <slot>
    local id
    id="$(printf 'd%02d' "$1")"
    curl -fsS -X DELETE \
        "$API/streams/live/fan/destinations/$id" >/dev/null \
        || fail "destination $id not deleted"
}

for i in $(seq 0 $(( COUNT - 1 ))); do
    add_destination "$i"
done

for _ in $(seq 1 600); do
    [ -f "$RUN/sink.ready" ] && break
    sleep 0.1
done
[ -f "$RUN/sink.ready" ] || fail "the sink did not see all $COUNT destinations"
for _ in $(seq 1 600); do
    grep -q 'srt output 0 connected' "$RUN/logs/error.log" \
        && grep -q 'srt output 16 connected' "$RUN/logs/error.log" && break
    sleep 0.1
done

metric() {   # <metric> <destination>
    curl -fsS "$API/metrics" \
        | awk -v m="$1" -v d="destination=\"$2\"" \
            'index($0, m "{") == 1 && index($0, d) { print $NF; exit }'
}
shard_metric() {   # <metric> <shard>
    curl -fsS "$API/metrics" \
        | awk -v m="$1" -v s="shard=\"$2\"" \
            'index($0, m "{") == 1 && index($0, s) { print $NF; exit }'
}
placement() {   # <destination>
    curl -fsS "$API/metrics" \
        | grep "^nginx_media_egress_queue_lag_ms{" | grep "destination=\"$1\"" \
        | sed -n 's/.*placement="\([0-9]*\)".*/\1/p' | head -1
}
destination_count() {
    # the shards' own view of how many destinations they hold: the refcount
    # that has to come back after a delete, a re-add and a reload
    curl -fsS "$API/metrics" \
        | awk '/^nginx_media_srt_egress_shard_destinations\{/ { s += $NF } END { print s + 0 }'
}

lane_of() { placement "$(printf 'd%02d' "$1")"; }

P_IMPAIRED0="$(lane_of 0)"; P_IMPAIRED1="$(lane_of 16)"
P_HEALTHY0="$(lane_of 32)"; P_HEALTHY1="$(lane_of 48)"
P_CONTROL0="$(lane_of 1)"; P_CONTROL1="$(lane_of 17)"
P_CONTROL2="$(lane_of 33)"; P_CONTROL3="$(lane_of 49)"
echo "== lanes: d00=$P_IMPAIRED0 d16=$P_IMPAIRED1 d32=$P_HEALTHY0 d48=$P_HEALTHY1"
echo "==       d01=$P_CONTROL0 d17=$P_CONTROL1 d33=$P_CONTROL2 d49=$P_CONTROL3"
[ -n "$P_IMPAIRED0" ] \
    && [ "$P_IMPAIRED0" = "$P_IMPAIRED1" ] \
    && [ "$P_IMPAIRED0" = "$P_HEALTHY0" ] \
    && [ "$P_IMPAIRED0" = "$P_HEALTHY1" ] \
    || fail "the impaired destinations and their lane-mates are not in one lane"
[ "$P_CONTROL0" = "$P_CONTROL1" ] && [ "$P_CONTROL0" = "$P_CONTROL2" ] \
    && [ "$P_CONTROL0" = "$P_CONTROL3" ] \
    || fail "the control destinations are not in one lane"
[ "$P_IMPAIRED0" != "$P_CONTROL0" ] \
    || fail "the impaired lane and the control lane are the same"

RSS_BEFORE="$(awk '/^VmRSS/ { print $2 }' /proc/"$(pgrep -P "$(cat "$RUN/logs/nginx.pid")" | head -1)"/status)"

snapshot() {   # <output>
    rm -f "$RUN/sink.csv.snapshot"
    kill -USR1 "$SINK_PID" || fail "sink exited"
    for _ in $(seq 1 200); do
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
kill -USR1 "$FLAP_PID"
sleep 0.3

declare -A LAG RECONNECT DROPPED TRANSPORT
for i in 0 16 32 48 1 17 33 49; do
    id="$(printf 'd%02d' "$i")"
    LAG[$i]="$(metric nginx_media_egress_queue_lag_ms "$id")"
    RECONNECT[$i]="$(metric nginx_media_egress_reconnects_total "$id")"
    DROPPED[$i]="$(metric nginx_media_egress_dropped_units_total "$id")"
    TRANSPORT[$i]="$(metric nginx_media_egress_transport_errors_total "$id")"
done
RETRANS_LANE="$(shard_metric nginx_media_srt_egress_shard_retransmitted_packets_total "$P_IMPAIRED0")"
RETRANS_CONTROL="$(shard_metric nginx_media_srt_egress_shard_retransmitted_packets_total "$P_CONTROL0")"

python3 - "$RUN/a.csv" "$RUN/b.csv" "$RUN/relay.json" "$RUN/relay-flap.json" \
    "$WINDOW" "$RETRANS_LANE" "$RETRANS_CONTROL" \
    "${LAG[32]:-}" "${LAG[48]:-}" "${LAG[0]:-}" "${LAG[16]:-}" \
    "${RECONNECT[32]:-}" "${DROPPED[32]:-}" "${TRANSPORT[32]:-}" \
    "${RECONNECT[48]:-}" "${DROPPED[48]:-}" "${TRANSPORT[48]:-}" <<'PYEOF' \
    || fail "lane isolation at fanout"
import json, statistics, sys

(a_path, b_path, relay_path, flap_path, window, retrans_lane, retrans_control,
 lag32, lag48, lag0, lag16, reconnects32, dropped32, transport32,
 reconnects48, dropped48, transport48) = sys.argv[1:19]
window = float(window)

def load(path):
    rows = {}
    for line in open(path):
        name, value = line.strip().split(",")[:2]
        rows[name.split("s=")[-1]] = int(value)
    return rows

a, b = load(a_path), load(b_path)
rate = {k: (b[k] - a.get(k, 0)) * 8 / window for k in b}
control = [rate["d%02d" % i] for i in (1, 17, 33, 49) if "d%02d" % i in rate]
others = [rate[k] for k in sorted(rate)
          if k not in ("d00", "d16", "d32", "d48", "d01", "d17", "d33", "d49")]
median = statistics.median(control)
relay = json.load(open(relay_path))
flap = json.load(open(flap_path))
ok = True

def check(cond, what):
    global ok
    print(("   ok   " if cond else "   FAIL ") + what)
    ok = ok and cond

print("   control lane (d01/d17/d33/d49): median %.0f bit/s, min %.0f" % (median, min(control)))
print("   healthy lane-mates: d32 %.0f d48 %.0f bit/s" % (rate.get("d32", 0), rate.get("d48", 0)))
print("   impaired lane-mates: d00 %.0f d16 %.0f bit/s" % (rate.get("d00", 0), rate.get("d16", 0)))
print("   other 56 destinations: min %.0f bit/s" % (min(others) if others else 0))
print("   relay: %s" % relay)
print("   flapping relay: %s" % flap)
print("   shard retransmitted packets: impaired lane %s, control lane %s"
      % (retrans_lane, retrans_control))
print("   queue lag ms: d00=%s d16=%s d32=%s d48=%s" % (lag0, lag16, lag32, lag48))
print("   healthy lane-mates: reconnects d32=%s d48=%s dropped d32=%s d48=%s "
      "transport errors d32=%s d48=%s"
      % (reconnects32, reconnects48, dropped32, dropped48, transport32, transport48))

check(relay["lost"] + relay["over_cap"] > 200 and relay["over_cap"] > 0,
      "the lossy impairment happened (%d lost, %d over the cap)"
      % (relay["lost"], relay["over_cap"]))
check(flap["blackholed"] > 100,
      "the flapping impairment happened (%d datagrams blackholed)"
      % flap["blackholed"])
check(rate.get("d00", 0) < 0.8 * median,
      "d00 fell behind its path (%.0f%% of the control median)"
      % (100 * rate.get("d00", 0) / median))
check(rate.get("d16", 0) < 0.95 * median,
      "d16 paid for the blackholes (%.0f%% of the control median)"
      % (100 * rate.get("d16", 0) / median))
check(max(control) <= 1.05 * median,
      "the control lane is steady (max %.1f%% of its median)"
      % (100 * max(control) / median))
for name, value in (("d32", rate.get("d32", 0)), ("d48", rate.get("d48", 0))):
    check(value >= 0.97 * median,
          "%s (lane-mate of two impaired destinations) delivered %.1f%% of "
          "the control median (>= 97%%)" % (name, 100 * value / median))
    check(value >= 0.97 * min(control),
          "%s is no worse than the slowest control destination" % name)
if others:
    check(min(others) >= 0.97 * median,
          "no destination outside the impaired lane fell behind (min %.1f%% "
          "of the control median)" % (100 * min(others) / median))
if retrans_lane is not None and retrans_control is not None:
    check(float(retrans_lane) > float(retrans_control) + 100,
          "the impaired lane's shared SndQ carried the retransmissions "
          "(%s vs %s) while its healthy lane-mates kept delivering"
          % (retrans_lane, retrans_control))
for label, lag in (("d32", lag32), ("d48", lag48)):
    if lag not in ("", None):
        check(float(lag) <= 1000,
              "%s queue lag %s ms stays bounded (<= 1000)" % (label, lag))
for label, value in (("d32", reconnects32), ("d48", reconnects48),
                     ("d32", dropped32), ("d48", dropped48),
                     ("d32", transport32), ("d48", transport48)):
    if value not in ("", None):
        check(float(value) == 0,
              "%s %s is zero" % (label, value))
sys.exit(0 if ok else 1)
PYEOF

# --- churn: delete, re-add, reload, and check the lane is intact ------------

echo "== churn: delete a control destination, re-add it, reload the worker"
COUNT_BEFORE="$(destination_count)"
delete_destination 49
for _ in $(seq 1 100); do
    [ "$(destination_count)" -lt "$COUNT_BEFORE" ] && break
    sleep 0.1
done
[ "$(destination_count)" -lt "$COUNT_BEFORE" ] \
    || fail "the destination count did not drop after a delete"
add_destination 49
for _ in $(seq 1 300); do
    [ "$(destination_count)" -ge "$COUNT_BEFORE" ] && break
    sleep 0.1
done
[ "$(destination_count)" -ge "$COUNT_BEFORE" ] \
    || fail "the destination count did not recover after a re-add"

# A reload replaces the worker, and the runtime graph is worker memory:
# every stream, destination and lane goes with the old worker.  What has to
# survive is nothing - and what has to come back is the same stable lane
# placement, with no lane or destination left behind by the old incarnation.
declare -A PLACEMENT_BEFORE
for i in $(seq 0 $(( COUNT - 1 ))); do
    PLACEMENT_BEFORE[$i]="$(lane_of "$i")"
done

MASTER="$(cat "$RUN/logs/nginx.pid")"
WORKER_BEFORE="$(pgrep -P "$MASTER" | head -1)"
READY_BEFORE="$(grep -c 'srt listener ready' "$RUN/logs/error.log" || true)"
kill -HUP "$MASTER"
for _ in $(seq 1 300); do
    WORKER_AFTER="$(pgrep -P "$MASTER" | head -1)"
    [ -n "$WORKER_AFTER" ] && [ "$WORKER_AFTER" != "$WORKER_BEFORE" ] \
        && [ "$(grep -c 'srt listener ready' "$RUN/logs/error.log" || true)" \
             -gt "$READY_BEFORE" ] \
        && break
    sleep 0.1
done
[ "$(pgrep -P "$MASTER" | head -1)" != "$WORKER_BEFORE" ] \
    || fail "the reload did not replace the worker"
[ "$(cat "$RUN/logs/nginx.pid")" = "$MASTER" ] \
    || fail "the master was replaced by the reload"

for _ in $(seq 1 100); do
    [ "$(destination_count)" -eq 0 ] && break
    sleep 0.1
done
[ "$(destination_count)" -eq 0 ] \
    || fail "the reloaded worker still holds $(destination_count) destinations"

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"fan"}' "$API/streams" >/dev/null \
    || fail "the stream could not be re-created after the reload"
for i in $(seq 0 $(( COUNT - 1 ))); do
    add_destination "$i"
done
for _ in $(seq 1 300); do
    [ "$(destination_count)" -eq "$COUNT" ] && break
    sleep 0.1
done
[ "$(destination_count)" -eq "$COUNT" ] \
    || fail "the shards hold $(destination_count) destinations after re-establishing $COUNT"

stable=1
for i in $(seq 0 $(( COUNT - 1 ))); do
    after="$(lane_of "$i")"
    [ "$after" = "${PLACEMENT_BEFORE[$i]}" ] || {
        echo "   d$i moved from lane ${PLACEMENT_BEFORE[$i]} to $after" >&2
        stable=0
    }
done
[ "$stable" -eq 1 ] \
    || fail "lane placement is not stable across a worker reload"
echo "   lane placement is identical for all $COUNT destinations after the reload"

WORKER_PID="$(pgrep -P "$(cat "$RUN/logs/nginx.pid")" | head -1)"
RSS_AFTER="$(awk '/^VmRSS/ { print $2 }' /proc/"$WORKER_PID"/status)"
python3 - "$RSS_BEFORE" "$RSS_AFTER" <<'PYEOF' || fail "worker memory grew without bound"
import sys
before, after = float(sys.argv[1]), float(sys.argv[2])
grew = after - before
print("   worker RSS %d -> %d kB (%+d kB over the whole run)" % (before, after, grew))
sys.exit(0 if grew < 262144 else 1)
PYEOF

if grep -qE '\[(alert|emerg)\]|signal [0-9]+|AddressSanitizer' "$RUN/logs/error.log"; then
    fail "worker reported an alert or crash"
fi

echo "== srt lane isolation at fanout ok"

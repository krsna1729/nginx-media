#!/usr/bin/env bash
#
# Single-host impairment topology (goal doc 29): network namespaces joined to
# the host by veth pairs, with tc netem applied independently per path.  No
# second VM and no second machine is involved.
#
#   ns-pub-a   10.77.0.2/30  --veth a--  10.77.0.1/30  \
#   ns-pub-b   10.77.0.6/30  --veth b--  10.77.0.5/30   >-- host nginx
#   ns-view-0  10.77.0.10/30 --veth v--  10.77.0.9/30  /
#
# Every link carries its own netem qdisc in each direction, so delay, jitter,
# loss, reordering, duplication and rate are set independently per path, and
# they are applied to real packets: media leaving a publisher namespace, SRT
# feedback returning to it, and the HLS a viewer pulls in the other direction.
# The hard link failure is real as well - the carrier is taken down inside the
# namespace - and the program is expected to notice it through the normal
# selection path.
#
# Phases:
#   1  two publishers in two namespaces reach the host over independently
#      impaired links; the program carries media, the preferred source stays
#      active, HLS keeps segmenting, and a viewer in a third namespace pulls a
#      byte-identical segment over its own impaired link at a rate the shaper
#      on that link can account for
#   2  hard link failure on the active publisher's carrier: the source is no
#      longer usable, the selector switches to the standby, media keeps flowing
#      over the standby's equally impaired link and the worker stays up
#   3  the carrier returns and its publisher reconnects: the selector switches
#      back on the normal priority/recovery path
#   4  the active path is impaired past usefulness (80% loss) while its
#      publisher is still alive: a second failover through the selector
#
# Every qdisc is read back out of the kernel before it is relied on, so a run
# that silently failed to install netem fails instead of passing.
#
# The host firewall is part of the topology: this machine runs ufw with a
# default-deny input policy, and a veth is just another interface to it, so
# packets arriving on a test path are dropped while they belong to no
# connection the host started.  The run therefore adds one input rule per host
# end of its own veth pairs, scoped to that interface, deletes it on every exit
# path and asserts it is gone at the end.  Nothing else on the host is touched,
# nothing is written to a firewall configuration file, and the rule cannot
# outlive the interfaces it names.  On a host whose firewall already admits the
# paths, no rule is added.
#
# Not covered, deliberately: SRT bonding (goal doc 11.4).  A bond needs both
# ends to carry one logical connection over two paths, and in this environment
# neither end can express it.  The publisher side: this ffmpeg's libsrt binding
# rejects group syntax outright ("Query string option 'group' does not exist").
# The listener side: nginx accepts exactly one media_srt_listen endpoint (a
# second directive is "[emerg] media_srt_listen directive is duplicate") and
# accepts connections with srt_accept_bond() over that single socket, so a
# group caller's legs could not arrive as one bonded source over two namespace
# paths.  A bonding case here could only pretend, so there is none.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/netns"
BASE=$(( 20100 + ($$ % 80) * 4 ))
SRT_PORT="${NETNS_SRT_PORT:-$BASE}"
HTTP_PORT="${NETNS_HTTP_PORT:-$(( BASE + 1 ))}"

TAG="$$"
NSA="mnetns-a-$TAG"
NSB="mnetns-b-$TAG"
NSV="mnetns-v-$TAG"

# host end of each pair, and the name the other end gets inside its namespace
HV_A="mvh-a-$TAG"; HV_B="mvh-b-$TAG"; HV_V="mvh-v-$TAG"
NV_A="mvn-a";     NV_B="mvn-b";     NV_V="mvn-v"

IP_A_HOST=10.77.0.1;  IP_A_NS=10.77.0.2
IP_B_HOST=10.77.0.5;  IP_B_NS=10.77.0.6
IP_V_HOST=10.77.0.9;  IP_V_NS=10.77.0.10

# The impairment per path.  "<up>" is what the far side sends - the media - and
# "<down>" is what comes back to it, which is where SRT's acknowledgements and
# retransmission requests travel.  Word splitting into netem's arguments is
# intended.
NETEM_A_UP="delay 25ms 8ms loss 1% duplicate 1% reorder 1% 50% rate 2mbit"
NETEM_A_DOWN="delay 25ms 8ms loss 1%"
NETEM_B_UP="delay 40ms 10ms loss 1% rate 1500kbit"
NETEM_B_DOWN="delay 40ms loss 1%"
NETEM_V_DOWN="delay 40ms 10ms loss 2% duplicate 1% reorder 1% 50% rate 1mbit"
NETEM_V_UP="delay 40ms 10ms loss 1%"

# the rate above, which the viewer's fetch time is checked against
V_RATE_BPS=1000000

STREAMID='%23!::r%3Dlive%2Fnetns%2Cm%3Dpublish%2Cs%3D'
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

PUB_A=0
PUB_B=0

fail() {
    echo "$*" >&2
    exit 1
}

priv() {
    sudo -n "$@"
}

ns_exists() {
    [ -e "/run/netns/$1" ]
}

# The links this run created, and the input rules that let their own traffic
# reach the host nginx.  The rules are runtime state, like the namespaces and
# the qdiscs, and they are removed with them.
FIREWALL_IFACES=""

firewall_open() {
    local iface

    command -v iptables >/dev/null 2>&1 || return 0

    for iface in "$HV_A" "$HV_B" "$HV_V"; do
        if priv iptables -w -C INPUT -i "$iface" -j ACCEPT 2>/dev/null; then
            continue
        fi

        if priv iptables -w -I INPUT 1 -i "$iface" -j ACCEPT 2>/dev/null; then
            FIREWALL_IFACES="$FIREWALL_IFACES $iface"
        fi
    done
}

firewall_close() {
    local iface

    for iface in $FIREWALL_IFACES; do
        priv iptables -w -D INPUT -i "$iface" -j ACCEPT 2>/dev/null || true
    done

    FIREWALL_IFACES=""
}

# Removing an interface destroys its qdisc with it, so the netem rules are
# covered by the namespace teardown; the host ends are deleted as well in case
# the run died between creating a pair and moving one end into its namespace.
netns_down() {
    local ns pid iface

    firewall_close

    for ns in "$NSA" "$NSB" "$NSV"; do
        ns_exists "$ns" || continue

        for pid in $(priv ip netns pids "$ns" 2>/dev/null || true); do
            priv kill -KILL "$pid" 2>/dev/null || true
        done

        priv ip netns del "$ns" 2>/dev/null || true
    done

    for iface in "$HV_A" "$HV_B" "$HV_V"; do
        priv ip link del "$iface" 2>/dev/null || true
    done
}

cleanup() {
    local master

    [ "$PUB_A" != "0" ] && kill -KILL "$PUB_A" 2>/dev/null || true
    [ "$PUB_B" != "0" ] && kill -KILL "$PUB_B" 2>/dev/null || true

    master="$(cat "$RUN/logs/nginx.pid" 2>/dev/null || true)"

    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true

    # The instance has to be gone before this run is over: a re-run reuses the
    # prefix, and an instance still on its way out would delete the pid file
    # the next run just wrote.
    if [ -n "$master" ]; then
        for _ in $(seq 1 100); do
            kill -0 "$master" 2>/dev/null || break
            sleep 0.05
        done

        if kill -0 "$master" 2>/dev/null; then
            kill -9 "$master" 2>/dev/null || true
        fi
    fi

    netns_down
}
trap cleanup EXIT

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

for tool in ip tc ffmpeg curl python3; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool is required"
done

priv true 2>/dev/null \
    || fail "root is required for ip netns and tc; sudo -n must work without a password"

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf" "$RUN/hls"

# --------------------------------------------------------------------------
# the topology
# --------------------------------------------------------------------------

link_up() { # <ns> <host-dev> <ns-dev> <host-ip> <ns-ip>
    local ns="$1" hdev="$2" ndev="$3" hip="$4" nip="$5"

    priv ip netns add "$ns"
    priv ip link add "$hdev" type veth peer name "$ndev"
    priv ip link set "$ndev" netns "$ns"

    priv ip addr add "$hip/30" dev "$hdev"
    priv ip link set "$hdev" up

    priv ip netns exec "$ns" ip link set lo up
    priv ip netns exec "$ns" ip addr add "$nip/30" dev "$ndev"
    priv ip netns exec "$ns" ip link set "$ndev" up
    priv ip netns exec "$ns" ip route add default via "$hip"
}

# <ns|-> <dev> <netem arguments...>; "-" means the host namespace
netem_set() {
    local ns="$1" dev="$2"
    shift 2

    if [ "$ns" = "-" ]; then
        priv tc qdisc del dev "$dev" root 2>/dev/null || true
        priv tc qdisc add dev "$dev" root netem "$@"
    else
        priv ip netns exec "$ns" tc qdisc del dev "$dev" root 2>/dev/null || true
        priv ip netns exec "$ns" tc qdisc add dev "$dev" root netem "$@"
    fi
}

netem_show() {
    if [ "$1" = "-" ]; then
        priv tc qdisc show dev "$2"
    else
        priv ip netns exec "$1" tc qdisc show dev "$2"
    fi
}

# records the qdisc for one path and prints it, so the impairment the case
# relies on is visible in the output rather than assumed
netem_report() { # <label> <ns|-> <dev>
    netem_show "$2" "$3" > "$RUN/qdisc-$1.txt"
    printf '   %-8s %s\n' "$1" "$(cat "$RUN/qdisc-$1.txt")"
}

netem_has() { # <label> <extended-regex>
    grep -Eq "$2" "$RUN/qdisc-$1.txt" \
        || fail "path $1: the qdisc does not show $2: $(cat "$RUN/qdisc-$1.txt")"
}

echo "== namespaces, veth pairs and per-path netem"
link_up "$NSA" "$HV_A" "$NV_A" "$IP_A_HOST" "$IP_A_NS"
link_up "$NSB" "$HV_B" "$NV_B" "$IP_B_HOST" "$IP_B_NS"
link_up "$NSV" "$HV_V" "$NV_V" "$IP_V_HOST" "$IP_V_NS"

firewall_open
echo "   input rules added:${FIREWALL_IFACES:- none}"

netem_set "$NSA" "$NV_A" $NETEM_A_UP
netem_set -      "$HV_A" $NETEM_A_DOWN
netem_set "$NSB" "$NV_B" $NETEM_B_UP
netem_set -      "$HV_B" $NETEM_B_DOWN
netem_set -      "$HV_V" $NETEM_V_DOWN
netem_set "$NSV" "$NV_V" $NETEM_V_UP

netem_report a-up   "$NSA" "$NV_A"
netem_report a-down -      "$HV_A"
netem_report b-up   "$NSB" "$NV_B"
netem_report b-down -      "$HV_B"
netem_report v-up   "$NSV" "$NV_V"
netem_report v-down -      "$HV_V"

# delay with jitter prints as two times, so two times is what proves jitter
netem_has a-up 'netem.*delay 25ms +8ms'
netem_has a-up 'loss 1%'
netem_has a-up 'duplicate 1%'
netem_has a-up 'reorder 1% 50%'
netem_has a-up 'rate 2Mbit'
netem_has a-down 'netem.*delay 25ms +8ms'
netem_has a-down 'loss 1%'
netem_has b-up 'netem.*delay 40ms +10ms'
netem_has b-up 'loss 1%'
netem_has b-up 'rate 1500Kbit'
netem_has v-down 'netem.*delay 40ms +10ms'
netem_has v-down 'loss 2%'
netem_has v-down 'duplicate 1%'
netem_has v-down 'reorder 1% 50%'
netem_has v-down 'rate 1Mbit'
netem_has v-up 'netem.*delay 40ms +10ms'

# --------------------------------------------------------------------------
# host nginx
# --------------------------------------------------------------------------

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_failover_failure_timeout 1200;
media_failover_recovery_timeout 400;
media_failover_switchback auto;

media_hls $RUN/hls;

media_srt_listen 0.0.0.0:$SRT_PORT;
media_srt_source_priority encoder-a 100;
media_srt_source_priority encoder-b 90;

http {
    access_log off;

    # the operator side: api and the reference copy of the hls output
    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /media/api/ {
            media_api;
        }

        location /hls/ {
            alias $RUN/hls/;
        }
    }

    # the viewer namespace only reaches the host on its own veth address
    server {
        listen $IP_V_HOST:$HTTP_PORT;

        location /hls/ {
            alias $RUN/hls/;
        }
    }
}
EOF

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

grep -q 'srt listener ready' "$RUN/logs/error.log" \
    || fail "the srt listener never became ready"

echo "== the host answers its namespaces"
REACH="$(priv ip netns exec "$NSV" curl -sS --max-time 5 -o /dev/null \
    -w '%{http_code}' "http://$IP_V_HOST:$HTTP_PORT/hls/index.m3u8" 2>/dev/null || true)"

echo "   $NSV -> $IP_V_HOST:$HTTP_PORT: ${REACH:-no answer}"

case "$REACH" in
    200|404) ;;
    *) fail "a namespace cannot reach host nginx over its veth (${REACH:-no answer}): nothing on this path is testable until that holds" ;;
esac

# --------------------------------------------------------------------------
# api helpers
# --------------------------------------------------------------------------

stream_json() {
    curl -fsS "$API/streams/live/netns" 2>/dev/null \
        || { echo "the api did not answer for live/netns" >&2; exit 1; }
}

# The api's own spelling of a scalar is what comes back: true/false for its
# booleans, bare values for numbers and strings.
stream_field() { # <field>
    stream_json | python3 -c '
import json, sys
value = json.load(sys.stdin)[sys.argv[1]]
print("true" if value is True else "false" if value is False else value)
' "$1"
}

source_field() { # <id> <field>; "missing" when the source is not registered
    curl -fsS "$API/streams/live/netns/sources" 2>/dev/null | python3 -c '
import json, sys
for source in json.load(sys.stdin)["sources"]:
    if source["id"] == sys.argv[1]:
        value = source[sys.argv[2]]
        print("true" if value is True else "false" if value is False else value)
        break
else:
    print("missing")
' "$1" "$2"
}

number() { # <description> <value>
    case "$2" in
        ''|*[!0-9]*) fail "$1: expected a number, got \"$2\"" ;;
    esac
    printf '%s' "$2"
}

# a source's frames_out only grows once it is the active source, so what says
# "this path carries usable media" is frames_in, which counts frames demuxed
# from that session
source_in() { # <id>
    local value

    value="$(source_field "$1" frames_in)"

    case "$value" in
        ''|*[!0-9]*) return 1 ;;
    esac

    printf '%s' "$value"
}

wait_for_source() { # <id> [tries]
    local value

    for _ in $(seq 1 "${2:-100}"); do
        if value="$(source_in "$1")" && [ "$value" -gt 0 ]; then
            return 0
        fi
        sleep 0.1
    done

    echo "timed out waiting for $1 to carry media over its veth" >&2
    curl -fsS "$API/streams/live/netns/sources" >&2 || true
    return 1
}

active() {
    stream_field active
}

switch_count() {
    number "switches" "$(stream_field switches)"
}

program_frames() {
    number "program_frames" "$(stream_field program_frames)"
}

wait_for_active() { # <source> [tries]
    local want="$1" tries="${2:-100}"

    for _ in $(seq 1 "$tries"); do
        if [ "$(active)" = "$want" ]; then
            return 0
        fi
        sleep 0.1
    done

    echo "timed out waiting for active=$want" >&2
    stream_json >&2 || true
    return 1
}

wait_for_frames() { # <description> <floor over four seconds>
    local what="$1" floor="$2" before after

    before="$(program_frames)"
    sleep 4
    after="$(program_frames)"

    echo "   $what: program frames $before -> $after"
    [ "$after" -gt "$before" ] \
        || fail "$what: the program stopped publishing frames ($before -> $after)"
    [ $(( after - before )) -ge "$floor" ] \
        || fail "$what: only $(( after - before )) frames in four seconds"
}

worker_up() {
    local master

    master="$(cat "$RUN/logs/nginx.pid" 2>/dev/null || true)"
    [ -n "$master" ] || return 1
    kill -0 "$master" 2>/dev/null || return 1
    [ -n "$(pgrep -P "$master" 2>/dev/null || true)" ]
}

worker_survived() { # <what happened>
    worker_up || fail "the worker died when $1"
    grep -aqE 'signal [0-9]+ \(core dumped\)|exited on signal' \
        "$RUN/logs/error.log" \
        && fail "the worker crashed when $1"
    return 0
}

hls_segments() {
    grep -c '^#EXTINF' "$RUN/hls/index.m3u8" 2>/dev/null || true
}

newest_segment() {
    grep -v '^#' "$RUN/hls/index.m3u8" 2>/dev/null | grep -v '^$' | tail -1 || true
}

publish() { # <ns> <host-ip> <source> <freq> <seconds>
    local ns="$1" host="$2" source="$3" freq="$4" seconds="$5"

    priv ip netns exec "$ns" ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -f lavfi -i "sine=frequency=$freq:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$seconds" -f mpegts \
        "srt://$host:$SRT_PORT?mode=caller&streamid=$STREAMID$source" \
        >>"$RUN/pub-$source.log" 2>&1 &

    echo $!
}

# --------------------------------------------------------------------------
# 1. media across impaired links
# --------------------------------------------------------------------------

echo "== two publishers in two namespaces, both links impaired"
PUB_A="$(publish "$NSA" "$IP_A_HOST" encoder-a 440 600)"
sleep 1
PUB_B="$(publish "$NSB" "$IP_B_HOST" encoder-b 880 600)"

wait_for_active encoder-a 200 \
    || fail "the preferred publisher never became the active source over its veth"

wait_for_source encoder-a 200 \
    || fail "encoder-a never carried media across its impaired veth; a publisher namespace that cannot reach the host is not a working path"

wait_for_source encoder-b 200 \
    || fail "encoder-b never carried media across its impaired veth; a publisher namespace that cannot reach the host is not a working path"

A_IN="$(number "encoder-a frames_in" "$(source_in encoder-a)")"
B_IN="$(number "encoder-b frames_in" "$(source_in encoder-b)")"
A_OUT="$(number "encoder-a frames_out" "$(source_field encoder-a frames_out)")"

echo "   active=$(active) encoder-a frames in=$A_IN out=$A_OUT, encoder-b frames in=$B_IN"

[ "$A_IN" -gt 0 ] && [ "$B_IN" -gt 0 ] \
    || fail "a publisher carried no media across its veth"
[ "$A_OUT" -gt 0 ] \
    || fail "the active source published no frames to the program"

wait_for_frames "under delay, jitter, loss, reordering, duplication and rate" 25

# The impairment has to be survivable, and that is a statement about where the
# program is, not about a health flag: loss on a link produces the odd
# container error, and the model clears and re-establishes health around it by
# design, so a momentary unhealthy reading says nothing.  What would say
# something is the program leaving the preferred source, or the standby
# stopping taking media, on a path that is only impaired.
[ "$(active)" = "encoder-a" ] \
    || fail "the program left the preferred source while both links were only impaired"

B_IN_LATER="$(number "encoder-b frames_in" "$(source_in encoder-b)")"
[ "$B_IN_LATER" -gt "$B_IN" ] \
    || fail "the standby stopped taking media over its impaired link ($B_IN -> $B_IN_LATER)"

SEGMENTS="$(number "hls segments" "$(hls_segments)")"
[ "$SEGMENTS" -ge 2 ] \
    || fail "hls segmented $SEGMENTS times while the links were impaired"

echo "== a viewer in its own namespace pulls a segment over its impaired link"
PLAYLIST="$(priv ip netns exec "$NSV" curl -fsS --max-time 30 \
    "http://$IP_V_HOST:$HTTP_PORT/hls/index.m3u8")"

VIEWED_SEGMENTS="$(printf '%s\n' "$PLAYLIST" | grep -c '^#EXTINF' || true)"
[ "$VIEWED_SEGMENTS" -ge 2 ] \
    || fail "the viewer namespace fetched only $VIEWED_SEGMENTS segments"

# the second to last entry: the last one is complete in the playlist but the
# oldest ones are about to be retired, so this is the one both ends can fetch
SEGMENT="$(printf '%s\n' "$PLAYLIST" | grep -v '^#' | grep -v '^$' | tail -2 | head -1)"
[ -n "$SEGMENT" ] || fail "the playlist the viewer fetched named no segment"

VIEWER_START="$(date +%s%3N)"
priv ip netns exec "$NSV" curl -fsS --max-time 30 -o "$RUN/viewer-$SEGMENT" \
    "http://$IP_V_HOST:$HTTP_PORT/hls/$SEGMENT"
VIEWER_MS=$(( $(date +%s%3N) - VIEWER_START ))

curl -fsS -o "$RUN/host-$SEGMENT" "http://127.0.0.1:$HTTP_PORT/hls/$SEGMENT"

VIEWER_SHA="$(sha256sum "$RUN/viewer-$SEGMENT" | cut -d' ' -f1)"
HOST_SHA="$(sha256sum "$RUN/host-$SEGMENT" | cut -d' ' -f1)"

[ "$VIEWER_SHA" = "$HOST_SHA" ] \
    || fail "the segment fetched across the impaired link differs from the one the host serves"

BYTES="$(wc -c < "$RUN/viewer-$SEGMENT")"
MIN_MS=$(( BYTES * 8000 / V_RATE_BPS / 2 ))

echo "   $SEGMENT: $BYTES bytes, sha256 $VIEWER_SHA, pulled in ${VIEWER_MS}ms"

# half the shaper's theoretical best case: an unshaped link pulls the same
# segment in single-digit milliseconds, so this tells shaping from no shaping
[ "$VIEWER_MS" -ge "$MIN_MS" ] \
    || fail "the viewer pulled $BYTES bytes in ${VIEWER_MS}ms across a $V_RATE_BPS bps shaper (at least ${MIN_MS}ms is the floor): the rate limit is not shaping that path"

# --------------------------------------------------------------------------
# 2. hard link failure
# --------------------------------------------------------------------------

# Capture the count first: loss on an impaired link produces the odd container
# error, and the health model reports that as a brief loss of health, so a
# link that is merely impaired can move the count on its own.  Every failover
# below is therefore asserted as a delta.
SWITCHES_BEFORE="$(switch_count)"
echo "   switches before the failure: $SWITCHES_BEFORE"

echo "== hard link failure on the active publisher's carrier"
priv ip netns exec "$NSA" ip link set "$NV_A" down
priv ip netns exec "$NSA" ip link show "$NV_A" > "$RUN/link-a-down.txt"
cat "$RUN/link-a-down.txt"

grep -q 'state DOWN' "$RUN/link-a-down.txt" \
    || fail "the carrier on path a was not actually taken down"

SEGMENT_BEFORE="$(newest_segment)"

wait_for_active encoder-b 250 \
    || fail "the selector did not fail over after the active carrier was lost"

SWITCHES="$(switch_count)"
[ "$SWITCHES" -gt "$SWITCHES_BEFORE" ] \
    || fail "the selector did not count a switch when the active carrier was lost ($SWITCHES_BEFORE -> $SWITCHES); this was not the normal selection path"

echo "   active=$(active) switches=$SWITCHES"

worker_survived "the active carrier was lost"

wait_for_frames "after the carrier was lost" 25

B_IN_AFTER="$(number "encoder-b frames_in" "$(source_in encoder-b)")"
[ "$B_IN_AFTER" -gt "$B_IN" ] \
    || fail "the standby carried no further media after the failover ($B_IN -> $B_IN_AFTER)"

for _ in $(seq 1 100); do
    SEGMENT_NOW="$(newest_segment)"
    [ "$SEGMENT_NOW" != "$SEGMENT_BEFORE" ] && break
    sleep 0.1
done

[ "$SEGMENT_NOW" != "$SEGMENT_BEFORE" ] \
    || fail "hls stopped producing segments when the carrier was lost"

echo "   hls moved on from $SEGMENT_BEFORE to $SEGMENT_NOW"

# --------------------------------------------------------------------------
# 3. the carrier returns and the publisher reconnects
# --------------------------------------------------------------------------

echo "== the carrier returns and its publisher reconnects"
priv ip netns exec "$NSA" ip link set "$NV_A" up

netem_report a-up "$NSA" "$NV_A"
netem_has a-up 'loss 1%'

PUB_A="$(publish "$NSA" "$IP_A_HOST" encoder-a 440 600)"

wait_for_active encoder-a 300 \
    || fail "the selector did not switch back to the recovered preferred source"

SWITCHES_BACK="$(switch_count)"
[ "$SWITCHES_BACK" -gt "$SWITCHES" ] \
    || fail "the switchback was not counted ($SWITCHES -> $SWITCHES_BACK)"

echo "   active=$(active) switches=$SWITCHES_BACK"

wait_for_frames "after the switchback" 25

# --------------------------------------------------------------------------
# 4. the active path impaired past usefulness
# --------------------------------------------------------------------------

echo "== the active path impaired past usefulness (80% loss)"
netem_set "$NSA" "$NV_A" delay 25ms loss 80%

netem_report a-up "$NSA" "$NV_A"
netem_has a-up 'loss 80%'

# the publisher is not killed: what fails here is the path, not the process
kill -0 "$PUB_A" 2>/dev/null \
    || fail "the preferred publisher was already gone before its path was impaired"

wait_for_active encoder-b 300 \
    || fail "the selector did not fail over from a path impaired past usefulness"

SWITCHES_AGAIN="$(switch_count)"
[ "$SWITCHES_AGAIN" -gt "$SWITCHES_BACK" ] \
    || fail "the second failover was not counted ($SWITCHES_BACK -> $SWITCHES_AGAIN)"

echo "   active=$(active) switches=$SWITCHES_AGAIN"

worker_survived "the active path was impaired past usefulness"

wait_for_frames "on the standby after the second failover" 25

# --------------------------------------------------------------------------
# teardown
# --------------------------------------------------------------------------

echo "== stop"
[ "$PUB_A" != "0" ] && kill -KILL "$PUB_A" 2>/dev/null || true
PUB_A=0
[ "$PUB_B" != "0" ] && kill -KILL "$PUB_B" 2>/dev/null || true
PUB_B=0

PID="$(cat "$RUN/logs/nginx.pid")"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit

for _ in $(seq 1 400); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.05
done

if kill -0 "$PID" 2>/dev/null; then
    kill -9 "$PID" || true
    fail "nginx did not shut down"
fi

grep -aq 'exited with code 0' "$RUN/logs/error.log" \
    || fail "the worker did not exit cleanly"

echo "== tear down the topology"
cleanup

for ns in "$NSA" "$NSB" "$NSV"; do
    ns_exists "$ns" && fail "namespace $ns survived the run"
done

for iface in "$HV_A" "$HV_B" "$HV_V"; do
    priv ip link show "$iface" >/dev/null 2>&1 && fail "interface $iface survived the run"
done

for addr in "$IP_A_HOST" "$IP_B_HOST" "$IP_V_HOST"; do
    priv ip -4 addr show | grep -q "$addr/" \
        && fail "address $addr survived the run"
done

if command -v iptables >/dev/null 2>&1; then
    for iface in "$HV_A" "$HV_B" "$HV_V"; do
        priv iptables -w -C INPUT -i "$iface" -j ACCEPT 2>/dev/null \
            && fail "the input rule for $iface survived the run"
    done
fi

echo "   namespaces left: $(priv ip netns list | wc -l)"
echo "   netem rules left: $(priv tc qdisc show | grep -c netem || true)"

echo "== netns ok"

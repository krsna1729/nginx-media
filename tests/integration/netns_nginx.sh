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
#   5  SRT bonding (goal doc 11.4): one bonded publisher carries its media over
#      two impaired paths at once and is registered as ONE source; taking one
#      of its paths away leaves that one source carrying media
#
# Phase 5 is the bond.  A bond is one logical connection carried over two
# paths, and it has to stay below ngx_media_source_t: the selector must see one
# source and never select an individual leg.  Both ends of it need something
# this machine's distro libsrt does not have, so the phase says exactly what it
# needs and how to get it rather than pretending:
#
#   the caller    ffmpeg's libsrt binding has no group option, so ffmpeg
#                 cannot express a bond at all.  The phase therefore builds a
#                 small group caller (srt_group_send, from a C file this script
#                 writes into its own scratch directory) that joins two legs
#                 into one broadcast group and reads the MPEG-TS ffmpeg
#                 produces on a fifo.
#   both ends     Arch's libsrt, like the Debian and Ubuntu packages, ships the
#                 public group declarations but compiles bonding out
#                 (ENABLE_BONDING defaults to OFF upstream): srt_create_group()
#                 fails and SRTO_GROUPCONNECT is an unknown option.  The phase
#                 uses a bonding-enabled libsrt when one is present, and skips
#                 with that reason when it is not.  To build one, in a prefix
#                 this phase defaults to:
#
#                   git clone --depth 1 -b v1.5.6 \
#                       https://github.com/Haivision/srt.git .build/srt-src
#                   cmake -S .build/srt-src -B .build/srt-bonding-build \
#                       -DCMAKE_BUILD_TYPE=Release -DENABLE_BONDING=ON \
#                       -DENABLE_TESTING=OFF -DUSE_ENCLIB=openssl-evp \
#                       -DCMAKE_INSTALL_PREFIX="$PWD/.build/srt-bonding"
#                   cmake --build .build/srt-bonding-build -j
#                   cmake --install .build/srt-bonding-build
#
#                 NETNS_BOND_SRT_PREFIX names a different prefix and
#                 NETNS_REQUIRE_BONDING=1 turns the skip into a failure, so a
#                 run that is meant to gate bonding cannot pass by skipping it.
#
# Because an SRT group is one process, the two legs of the bond are the two
# uplinks of one encoder: one namespace with two veth pairs, each with its own
# netem, which is what the two ISP paths of goal doc 29 are - the legs of one
# bonded source, not two sources.
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
BOND_SRT_PORT="${NETNS_BOND_SRT_PORT:-$(( BASE + 2 ))}"
BOND_HTTP_PORT="${NETNS_BOND_HTTP_PORT:-$(( BASE + 3 ))}"

TAG="$$"
NSA="mnetns-a-$TAG"
NSB="mnetns-b-$TAG"
NSV="mnetns-v-$TAG"
NS_BOND="mnetns-bond-$TAG"

# host end of each pair, and the name the other end gets inside its namespace
HV_A="mvh-a-$TAG"; HV_B="mvh-b-$TAG"; HV_V="mvh-v-$TAG"
NV_A="mvn-a";     NV_B="mvn-b";     NV_V="mvn-v"

# the bonded publisher has one namespace and two uplinks (see the header)
BHV_1="mvb1-$TAG"; BHV_2="mvb2-$TAG"
BNV_1="mnb1";      BNV_2="mnb2"

IP_A_HOST=10.77.0.1;  IP_A_NS=10.77.0.2
IP_B_HOST=10.77.0.5;  IP_B_NS=10.77.0.6
IP_V_HOST=10.77.0.9;  IP_V_NS=10.77.0.10

IP_B1_HOST=10.78.0.1; IP_B1_NS=10.78.0.2
IP_B2_HOST=10.78.0.5; IP_B2_NS=10.78.0.6

# host-side interface names of every path this run creates, for the firewall
# rules and for the teardown checks
HOST_IFACES="$HV_A $HV_B $HV_V $BHV_1 $BHV_2"
ALL_NS="$NSA $NSB $NSV $NS_BOND"

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

# the bonded publisher's two uplinks (see phase 5)
NETEM_B1_UP="delay 30ms 5ms loss 1%"
NETEM_B1_DOWN="delay 30ms 5ms loss 1%"
NETEM_B2_UP="delay 45ms 8ms loss 1%"
NETEM_B2_DOWN="delay 45ms loss 1%"

# the rate above, which the viewer's fetch time is checked against
V_RATE_BPS=1000000

STREAMID='#!::r=live/netns,m=publish,s='
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

# The bonded publisher of phase 5.  Its stream id is handed to libsrt
# literally: the listener's parser does not decode percent escapes, and only
# ffmpeg's URL syntax needs them.
BOND_STREAMID='#!::r=live/bond,m=publish,s=encoder-bond'
BOND_API="http://127.0.0.1:$BOND_HTTP_PORT/media/api/v1"
BOND_PREFIX="${NETNS_BOND_SRT_PREFIX:-$ROOT/.build/srt-bonding}"
BOND_SENDER_BIN="$RUN/srt_group_send"

PUB_A=0
PUB_B=0

# the bonded phase: ffmpeg feeding the group caller, and the group caller
BOND_FFMPEG=0
BOND_SENDER=0
BOND_STARTED=0

fail() {
    echo "$*" >&2
    exit 1
}

# The bonding phase needs a libsrt with bonding compiled in, which no
# distribution package ships.  Skipping says so and names the build, because a
# silent pass would read as "bonding works here" when nothing was exercised.
bond_skip() { # <reason>
    if [ "${NETNS_REQUIRE_BONDING:-0}" = "1" ]; then
        fail "SRT bonding is required but $1"
    fi

    echo "   SKIPPED (SRT bonding): $1"
    echo "   the bonded listener and the bonded publisher are not exercised; set NETNS_REQUIRE_BONDING=1 to make that a failure"
}

priv() {
    # Inside a container the suite runs as root and sudo may not exist at all,
    # which is how this has to work in all three environments.
    if [ "$(id -u)" = "0" ]; then
        "$@"
    else
        sudo -n "$@"
    fi
}

ns_exists() {
    [ -e "/run/netns/$1" ]
}

# The links this run created, and the input rules that let their own traffic
# reach the host nginx.  The rules are runtime state, like the namespaces and
# the qdiscs, and they are removed with them.
FIREWALL_IFACES=""

firewall_open() { # <host-interface>...
    local iface

    command -v iptables >/dev/null 2>&1 || return 0

    for iface in "$@"; do
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

    for ns in $ALL_NS; do
        ns_exists "$ns" || continue

        for pid in $(priv ip netns pids "$ns" 2>/dev/null || true); do
            priv kill -KILL "$pid" 2>/dev/null || true
        done

        priv ip netns del "$ns" 2>/dev/null || true
    done

    for iface in $HOST_IFACES; do
        priv ip link del "$iface" 2>/dev/null || true
    done
}

cleanup() {
    local master

    [ "$PUB_A" != "0" ] && kill -KILL "$PUB_A" 2>/dev/null || true
    [ "$PUB_B" != "0" ] && kill -KILL "$PUB_B" 2>/dev/null || true
    # reaped, so bash does not announce them as killed on the way out
    if [ "$BOND_FFMPEG" != "0" ]; then
        kill -KILL "$BOND_FFMPEG" 2>/dev/null || true
        wait "$BOND_FFMPEG" 2>/dev/null || true
    fi

    if [ "$BOND_SENDER" != "0" ]; then
        kill -KILL "$BOND_SENDER" 2>/dev/null || true
        wait "$BOND_SENDER" 2>/dev/null || true
    fi

    if [ "$BOND_STARTED" = "1" ]; then
        "$NGINX" -p "$RUN/bond" -c conf/nginx.conf -s quit 2>/dev/null || true
    fi

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

# One more uplink for a namespace that already exists.  The bonded publisher is
# a single process and so a single namespace (see the header), and its second
# path is reached by the connected route its own address installs.
uplink_up() { # <ns> <host-dev> <ns-dev> <host-ip> <ns-ip>
    local ns="$1" hdev="$2" ndev="$3" hip="$4" nip="$5"

    priv ip link add "$hdev" type veth peer name "$ndev"
    priv ip link set "$ndev" netns "$ns"

    priv ip addr add "$hip/30" dev "$hdev"
    priv ip link set "$hdev" up

    priv ip netns exec "$ns" ip addr add "$nip/30" dev "$ndev"
    priv ip netns exec "$ns" ip link set "$ndev" up
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

firewall_open "$HV_A" "$HV_B" "$HV_V"
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

# The bonded publisher's two uplinks.  An SRT group caller is one process, so
# the two legs of the bond are two interfaces of one namespace rather than two
# namespaces: they are the two ISP paths of goal doc 29, not two publishers.
echo "== the bonded publisher's two uplinks"
link_up   "$NS_BOND" "$BHV_1" "$BNV_1" "$IP_B1_HOST" "$IP_B1_NS"
uplink_up "$NS_BOND" "$BHV_2" "$BNV_2" "$IP_B2_HOST" "$IP_B2_NS"

firewall_open "$BHV_1" "$BHV_2"

netem_set "$NS_BOND" "$BNV_1" $NETEM_B1_UP
netem_set -         "$BHV_1" $NETEM_B1_DOWN
netem_set "$NS_BOND" "$BNV_2" $NETEM_B2_UP
netem_set -         "$BHV_2" $NETEM_B2_DOWN

netem_report b1-up   "$NS_BOND" "$BNV_1"
netem_report b1-down -          "$BHV_1"
netem_report b2-up   "$NS_BOND" "$BNV_2"
netem_report b2-down -          "$BHV_2"

netem_has b1-up 'netem.*delay 30ms +5ms'
netem_has b1-up 'loss 1%'
netem_has b1-down 'netem.*delay 30ms +5ms'
netem_has b2-up 'netem.*delay 45ms +8ms'
netem_has b2-up 'loss 1%'
netem_has b2-down 'netem.*delay 45ms'

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
    -w '%{http_code}' "http://$IP_V_HOST:$HTTP_PORT/hls/live/netns/index.m3u8" 2>/dev/null || true)"

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

# ---- the bonded publisher (phase 5) ----
#
# A bond is one source, so what these read is how many sources one bonded
# publisher produced.  If the listener ever went back to treating each leg as
# its own connection, this number would be two: the same encoder would appear
# twice and the selector would be free to pick between the legs of one bond,
# which is exactly what goal doc 11.4 forbids.

bond_sources() {
    curl -fsS "$BOND_API/streams/live/bond/sources" 2>/dev/null || true
}

bond_source_count() {
    bond_sources | python3 -c '
import json, sys
try:
    print(len(json.load(sys.stdin)["sources"]))
except Exception:
    print(0)
' 2>/dev/null || echo 0
}

bond_source_ids() { # every registered identity, or "missing"
    bond_sources | python3 -c '
import json, sys
try:
    sources = json.load(sys.stdin)["sources"]
except Exception:
    sources = []
print(",".join(sorted(s["id"] for s in sources)) if sources else "missing")
' 2>/dev/null || echo missing
}

bond_source_in() {
    bond_sources | python3 -c '
import json, sys
try:
    sources = json.load(sys.stdin)["sources"]
except Exception:
    sources = []
print(sources[0]["frames_in"] if sources else 0)
' 2>/dev/null || echo 0
}

bond_program_frames() {
    curl -fsS "$BOND_API/streams/live/bond" 2>/dev/null | python3 -c '
import json, sys
print(json.load(sys.stdin)["program_frames"])
' 2>/dev/null || echo 0
}

# the group caller is fed by ffmpeg on a fifo, so its pid and ffmpeg's are both
# this script's to stop
publish_bond() { # <seconds>
    local fifo="$RUN/bond-in"

    rm -f "$fifo"
    mkfifo "$fifo"

    priv ip netns exec "$NS_BOND" "$BOND_SENDER_BIN" "$BOND_STREAMID" \
        "$IP_B2_HOST:$BOND_SRT_PORT" "$IP_B1_HOST:$BOND_SRT_PORT" \
        <"$fifo" >>"$RUN/bond-pub.log" 2>&1 &
    BOND_SENDER=$!

    ffmpeg -hide_banner -loglevel error -y -re \
        -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -f lavfi -i "sine=frequency=1320:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$1" -f mpegts "$fifo" >>"$RUN/bond-pub.log" 2>&1 &
    BOND_FFMPEG=$!
}

# the publisher process is inside its namespace, which is also how the run
# stops it, so this is what says "the path failed, not the encoder"
bond_sender_alive() {
    [ -n "$(priv ip netns pids "$NS_BOND" 2>/dev/null || true)" ]
}

bond_worker_up() {
    local master

    master="$(cat "$RUN/bond/logs/nginx.pid" 2>/dev/null || true)"
    [ -n "$master" ] || return 1
    kill -0 "$master" 2>/dev/null || return 1
    [ -n "$(pgrep -P "$master" 2>/dev/null || true)" ]
}

bond_worker_survived() { # <what happened>
    bond_worker_up || fail "the bonded worker died when $1"
    grep -aqE 'signal [0-9]+ \(core dumped\)|exited on signal' \
        "$RUN/bond/logs/error.log" \
        && fail "the bonded worker crashed when $1"
    return 0
}

bond_frames_grow() { # <description> <floor over four seconds>
    local what="$1" floor="$2" before after

    before="$(number "bonded program_frames" "$(bond_program_frames)")"
    sleep 4
    after="$(number "bonded program_frames" "$(bond_program_frames)")"

    echo "   $what: program frames $before -> $after"
    [ "$after" -gt "$before" ] \
        || fail "$what: the program stopped publishing frames ($before -> $after)"
    [ $(( after - before )) -ge "$floor" ] \
        || fail "$what: only $(( after - before )) frames in four seconds"
}

worker_survived() { # <what happened>
    worker_up || fail "the worker died when $1"
    grep -aqE 'signal [0-9]+ \(core dumped\)|exited on signal' \
        "$RUN/logs/error.log" \
        && fail "the worker crashed when $1"
    return 0
}

hls_segments() {
    grep -c '^#EXTINF' "$RUN/hls/live/netns/index.m3u8" 2>/dev/null || true
}

newest_segment() {
    grep -v '^#' "$RUN/hls/live/netns/index.m3u8" 2>/dev/null | grep -v '^$' | tail -1 || true
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
    "http://$IP_V_HOST:$HTTP_PORT/hls/live/netns/index.m3u8")"

VIEWED_SEGMENTS="$(printf '%s\n' "$PLAYLIST" | grep -c '^#EXTINF' || true)"
[ "$VIEWED_SEGMENTS" -ge 2 ] \
    || fail "the viewer namespace fetched only $VIEWED_SEGMENTS segments"

# the second to last entry: the last one is complete in the playlist but the
# oldest ones are about to be retired, so this is the one both ends can fetch
SEGMENT="$(printf '%s\n' "$PLAYLIST" | grep -v '^#' | grep -v '^$' | tail -2 | head -1)"
[ -n "$SEGMENT" ] || fail "the playlist the viewer fetched named no segment"

VIEWER_START="$(date +%s%3N)"
priv ip netns exec "$NSV" curl -fsS --max-time 30 -o "$RUN/viewer-$SEGMENT" \
    "http://$IP_V_HOST:$HTTP_PORT/hls/live/netns/$SEGMENT"
VIEWER_MS=$(( $(date +%s%3N) - VIEWER_START ))

curl -fsS -o "$RUN/host-$SEGMENT" "http://127.0.0.1:$HTTP_PORT/hls/live/netns/$SEGMENT"

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
# 5. SRT bonding
# --------------------------------------------------------------------------

# Goal doc 11.4: the selector sees one source and never an individual bond
# member.  What this phase asserts is that number: one source before and after
# a path is taken away, carrying media, while the publisher keeps both legs
# alive.  A regression that accepted the legs as separate connections would
# show two sources with one identity here, and a listener that lost group
# acceptance entirely would show none at all.

if ! command -v cc >/dev/null 2>&1; then
    bond_skip "there is no C compiler to build the group caller with"
    echo "   (final phase; 1-4 above still hold)"

elif [ ! -f "$BOND_PREFIX/lib/libsrt.so" ] \
     || [ ! -f "$BOND_PREFIX/include/srt/srt.h" ]; then
    bond_skip "no bonding-enabled libsrt is installed at $BOND_PREFIX, and a distribution libsrt compiles bonding out (ENABLE_BONDING defaults to OFF upstream), so srt_create_group() fails and SRTO_GROUPCONNECT is unknown to the library"

else
    echo "== a bonded publisher: two impaired paths, one source"
    echo "   both ends use the bonding-enabled libsrt in $BOND_PREFIX"

    mkdir -p "$RUN/bond/logs" "$RUN/bond/conf" "$RUN/bond/hls"

    # ffmpeg's libsrt binding has no option for a group, so the caller is
    # built here: one broadcast group, one connection per leg.
    cat > "$RUN/srt_group_send.c" <<'CEOF'
/*
 * An SRT group (bonded) caller (goal doc 11.4).
 *
 * ffmpeg's libsrt binding cannot express a group, so the bonding case needs a
 * caller that can: a broadcast group whose legs are separate connections, all
 * fed the same MPEG-TS.  MPEG-TS is read on stdin so the test can keep using
 * ffmpeg to produce it.
 *
 * usage: srt_group_send <streamid> <host:port> [host:port ...]
 */

#include <srt/srt.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LEGS  4
#define PAYLOAD   1316   /* SRT_LIVE_DEF_PLSIZE: seven TS packets */

static int
parse_target(const char *text, struct sockaddr_in *out)
{
    const char  *colon;
    char         host[64];
    size_t       len;
    long         port;

    colon = strrchr(text, ':');
    if (colon == NULL) {
        return -1;
    }

    len = (size_t) (colon - text);
    if (len == 0 || len >= sizeof(host)) {
        return -1;
    }

    memcpy(host, text, len);
    host[len] = '\0';

    port = strtol(colon + 1, NULL, 10);
    if (port < 1 || port > 65535) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t) port);

    return (inet_pton(AF_INET, host, &out->sin_addr) == 1) ? 0 : -1;
}

int
main(int argc, char **argv)
{
    SRT_SOCKGROUPCONFIG  targets[MAX_LEGS];
    struct sockaddr_in   addr[MAX_LEGS];
    SRTSOCKET            group;
    char                 buf[PAYLOAD];
    int                  legs, i, timeout = 1000;
    ssize_t              n;

    if (argc < 3 || argc - 2 > MAX_LEGS) {
        fprintf(stderr, "usage: %s <streamid> <host:port> [host:port ...]\n",
                argv[0]);
        return 2;
    }

    legs = argc - 2;

    for (i = 0; i < legs; i++) {
        if (parse_target(argv[i + 2], &addr[i]) != 0) {
            fprintf(stderr, "GROUP_SEND bad target %s\n", argv[i + 2]);
            return 2;
        }
    }

    if (srt_startup() == SRT_ERROR) {
        fprintf(stderr, "GROUP_SEND srt_startup: %s\n", srt_getlasterror_str());
        return 1;
    }

    group = srt_create_group(SRT_GTYPE_BROADCAST);
    if (group == SRT_INVALID_SOCK) {
        fprintf(stderr, "GROUP_SEND srt_create_group: %s\n",
                srt_getlasterror_str());
        return 3;
    }

    if (srt_setsockopt(group, 0, SRTO_STREAMID, argv[1],
                       (int) strlen(argv[1]) + 1) == SRT_ERROR
        || srt_setsockopt(group, 0, SRTO_SNDTIMEO, &timeout,
                          sizeof(timeout)) == SRT_ERROR)
    {
        fprintf(stderr, "GROUP_SEND srt_setsockopt: %s\n",
                srt_getlasterror_str());
        return 1;
    }

    for (i = 0; i < legs; i++) {
        targets[i] = srt_prepare_endpoint(NULL, (struct sockaddr *) &addr[i],
                                          sizeof(addr[i]));
    }

    if (srt_connect_group(group, targets, legs) == SRT_ERROR) {
        fprintf(stderr, "GROUP_SEND srt_connect_group: %s\n",
                srt_getlasterror_str());
        return 1;
    }

    /* Every leg has to be up: a group that silently lost one would still look
     * like one source to the listener, and the case would prove nothing. */
    for (i = 0; i < legs; i++) {
        if (targets[i].errorcode != SRT_SUCCESS) {
            fprintf(stderr, "GROUP_SEND leg %d (%s) did not connect: error %d\n",
                    i, argv[i + 2], targets[i].errorcode);
            return 1;
        }
    }

    fprintf(stderr, "GROUP_SEND connected legs=%d\n", legs);
    fflush(stderr);

    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;

        while (written < n) {
            int sent = srt_sendmsg(group, buf + written,
                                   (int) (n - written), -1, 1);

            if (sent > 0) {
                written += sent;
                continue;
            }

            if (sent == SRT_ERROR) {
                int err = srt_getlasterror(NULL);

                /* backpressure, not a broken session: keep the buffer */
                if (err == SRT_ETIMEOUT || err == SRT_EASYNCSND) {
                    continue;
                }

                fprintf(stderr, "GROUP_SEND srt_sendmsg: %s\n",
                        srt_getlasterror_str());
                goto done;
            }
        }
    }

done:
    fprintf(stderr, "GROUP_SEND closing\n");
    srt_close(group);
    srt_cleanup();

    return 0;
}
CEOF

    # the rpath is what lets the caller find the bonding-enabled library when
    # it is started inside its namespace as root
    cc -O1 -g -Wall -Wextra -Werror -std=c11 \
        -I"$BOND_PREFIX/include" -L"$BOND_PREFIX/lib" \
        -Wl,-rpath,"$BOND_PREFIX/lib" \
        -o "$BOND_SENDER_BIN" "$RUN/srt_group_send.c" -lsrt -lpthread

    cat > "$RUN/bond/conf/nginx.conf" <<EOF
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

media_hls $RUN/bond/hls;

# the two local addresses of the bonded listener: the two uplinks of the
# bonded publisher arrive on one of each
media_srt_listen $IP_B1_HOST:$BOND_SRT_PORT;
media_srt_listen_bond $IP_B2_HOST;
media_srt_source_priority encoder-bond 100;

http {
    access_log off;

    server {
        listen 127.0.0.1:$BOND_HTTP_PORT;

        location /media/api/ {
            media_api;
        }
    }
}
EOF

    echo "== starting the bonded nginx"
    LD_LIBRARY_PATH="$BOND_PREFIX/lib" \
        "$NGINX" -p "$RUN/bond" -c conf/nginx.conf -t
    LD_LIBRARY_PATH="$BOND_PREFIX/lib" \
        "$NGINX" -p "$RUN/bond" -c conf/nginx.conf
    BOND_STARTED=1

    for _ in $(seq 1 200); do
        grep -q 'srt listener ready' "$RUN/bond/logs/error.log" 2>/dev/null \
            && break
        sleep 0.05
    done

    # both addresses have to be bound before the bonds below mean anything
    grep -q "srt listener ready on $IP_B1_HOST:$BOND_SRT_PORT bonded with $IP_B2_HOST" \
        "$RUN/bond/logs/error.log" \
        || fail "the bonded listener did not come up on both addresses: $(tail -3 "$RUN/bond/logs/error.log")"

    echo "   listening on $IP_B1_HOST:$BOND_SRT_PORT and $IP_B2_HOST:$BOND_SRT_PORT"

    echo "== one bonded publisher over both impaired paths"
    publish_bond 600
    sleep 1

    for _ in $(seq 1 250); do
        [ "$(bond_source_count)" = "1" ] && [ "$(bond_source_in)" -gt 0 ] \
            && break
        sleep 0.1
    done

    BOND_COUNT="$(bond_source_count)"
    [ "$BOND_COUNT" = "1" ] \
        || fail "the bonded publisher registered $BOND_COUNT sources, not one: the legs of the bond reached the core as separate sources, which is what goal doc 11.4 forbids ($(bond_source_ids))"

    BOND_ID="$(bond_source_ids)"
    [ "$BOND_ID" = "encoder-bond" ] \
        || fail "the bonded publisher registered as \"$BOND_ID\", not one encoder-bond source"

    BOND_IN="$(number "encoder-bond frames_in" "$(bond_source_in)")"
    [ "$BOND_IN" -gt 0 ] \
        || fail "the bonded source carried no media over either path: $(tail -3 "$RUN/bond-pub.log")"

    echo "   one source: $BOND_ID, frames in=$BOND_IN"

    bond_frames_grow "with both bonded paths impaired" 25

    echo "== one of the bonded paths is taken away"
    priv ip netns exec "$NS_BOND" ip link set "$BNV_1" down
    priv ip netns exec "$NS_BOND" ip link show "$BNV_1" \
        > "$RUN/bond-link-1-down.txt"
    cat "$RUN/bond-link-1-down.txt"

    grep -q 'state DOWN' "$RUN/bond-link-1-down.txt" \
        || fail "the carrier on the bonded path 1 was not actually taken down"

    # the bond is transport redundancy: losing one leg must not turn one source
    # into two, into none, or into a stall
    BOND_IN_BEFORE="$BOND_IN"

    for _ in $(seq 1 150); do
        [ "$(bond_source_count)" = "1" ] \
            && [ "$(bond_source_in)" -gt "$BOND_IN_BEFORE" ] \
            && break
        sleep 0.1
    done

    BOND_COUNT_AFTER="$(bond_source_count)"
    [ "$BOND_COUNT_AFTER" = "1" ] \
        || fail "the bonded publisher became $BOND_COUNT_AFTER sources when one path was lost ($(bond_source_ids))"

    BOND_IN_AFTER="$(number "encoder-bond frames_in" "$(bond_source_in)")"
    [ "$BOND_IN_AFTER" -gt "$BOND_IN_BEFORE" ] \
        || fail "the bonded source stopped carrying media when one path was lost ($BOND_IN_BEFORE -> $BOND_IN_AFTER): $(tail -3 "$RUN/bond-pub.log")"

    echo "   still one source, frames in $BOND_IN_BEFORE -> $BOND_IN_AFTER"

    bond_sender_alive \
        || fail "the bonded publisher itself died when one of its paths was lost: this was meant to be a path failure, not an encoder failure"

    bond_worker_survived "one of the bonded paths was lost"

    bond_frames_grow "on the surviving bonded path" 25

    grep -q 'GROUP_SEND connected legs=2' "$RUN/bond-pub.log" \
        || fail "the group caller did not report both legs up, so the bond was never actually carried over two paths: $(tail -3 "$RUN/bond-pub.log")"

    echo "   the group caller connected both legs"

    # the bonded instance is stopped here rather than left for the teardown of
    # the other one: it is this phase's, and its assertion is its own
    echo "== stop the bonded instance"

    # reaping them here is also what keeps bash's "Killed" notice for them out
    # of this run's output
    if [ "$BOND_FFMPEG" != "0" ]; then
        kill -KILL "$BOND_FFMPEG" 2>/dev/null || true
        wait "$BOND_FFMPEG" 2>/dev/null || true
        BOND_FFMPEG=0
    fi

    if [ "$BOND_SENDER" != "0" ]; then
        kill -KILL "$BOND_SENDER" 2>/dev/null || true
        wait "$BOND_SENDER" 2>/dev/null || true
        BOND_SENDER=0
    fi

    BOND_PID="$(cat "$RUN/bond/logs/nginx.pid")"
    LD_LIBRARY_PATH="$BOND_PREFIX/lib" \
        "$NGINX" -p "$RUN/bond" -c conf/nginx.conf -s quit

    for _ in $(seq 1 400); do
        kill -0 "$BOND_PID" 2>/dev/null || break
        sleep 0.05
    done

    if kill -0 "$BOND_PID" 2>/dev/null; then
        kill -9 "$BOND_PID" || true
        fail "the bonded nginx did not shut down"
    fi

    BOND_STARTED=0

    grep -aq 'exited with code 0' "$RUN/bond/logs/error.log" \
        || fail "the bonded worker did not exit cleanly"

    echo "== bonded source: one source through both paths and through a lost path"
fi

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

for ns in $ALL_NS; do
    ns_exists "$ns" && fail "namespace $ns survived the run"
done

for iface in $HOST_IFACES; do
    priv ip link show "$iface" >/dev/null 2>&1 \
        && fail "interface $iface survived the run"
done

for addr in "$IP_A_HOST" "$IP_B_HOST" "$IP_V_HOST" "$IP_B1_HOST" "$IP_B2_HOST"; do
    priv ip -4 addr show | grep -q "$addr/" \
        && fail "address $addr survived the run"
done

if command -v iptables >/dev/null 2>&1; then
    for iface in $HOST_IFACES; do
        priv iptables -w -C INPUT -i "$iface" -j ACCEPT 2>/dev/null \
            && fail "the input rule for $iface survived the run"
    done
fi

echo "   namespaces left: $(priv ip netns list | wc -l)"
echo "   netem rules left: $(priv tc qdisc show | grep -c netem || true)"

echo "== netns ok"

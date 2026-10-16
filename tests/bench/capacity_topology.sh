#!/usr/bin/env bash
#
# The capacity run's network topology, in one of two modes, with the kernel
# settings each mode needs.
#
#   namespace  sender and receiver each in their own network namespace, joined
#              by one multi-queue veth pair: two UDP stacks, two device queues,
#              nothing shared but the machine.
#   loopback   one host and one 127.0.0.1 stack, shared by sender and receivers.
#
#   tests/bench/capacity_topology.sh namespace up|down|status [ns-sut] [ns-recv]
#   tests/bench/capacity_topology.sh loopback  up|down|status
#   eval "$(tests/bench/capacity_topology.sh namespace up)"   # harness env, then
#   PHASES=capacity-quality-ladder python3 tests/bench/capacity_harness.py
#
# `up` creates the topology, applies the settings below and exports the harness
# environment on stdout; `down` removes it and checks for residue. Everything
# the script configures is read back, so a setting that did not take effect
# fails the run instead of quietly changing what the numbers mean.
#
# Receive steering: a packet arriving on a software device is processed in
# softirq on the CPU that transmitted it unless RPS moves it to another CPU's
# backlog. The high-volume direction is the egress fanout, one packet stream per
# destination, so both modes steer that direction onto the receiver CPUs. A
# device that ignores steering shows up here as a run whose sender cores carry
# the receivers' ingress work, which is what `status` and the run evidence are
# for; a steered device is what makes the sender's CPU a sender measurement.

set -uo pipefail

MODE="${1:-}"
ACTION="${2:-up}"
NS_SUT="${3:-nmsut}"
NS_RECV="${4:-nmrecv}"
SUT_IF="veth-sut"
RECV_IF="veth-recv"
SUT_ADDR="10.200.0.1/24"
RECV_ADDR="10.200.0.2/24"
LO_IF="lo"
MTU=1500
QUEUES=8
# The SRT sink needs socket buffers this large; below it the kernel drops, not
# the engine. These limits are host-global here (a namespace write is refused),
# so they are checked, never set.
SOCKMEM=67108864
# Which CPUs pay for receive processing: the receivers, and for the return
# direction the sender's own cores.
RPS_RECV_CPUS="${RPS_RECEIVER_CPUS:-12-19}"
RPS_SUT_CPUS="${RPS_SENDER_CPUS:-0,2,4,6,8,10}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HELPER_DIR="$ROOT/.build/capacity-veth"
HELPER="$HELPER_DIR/nsenter-net"
# The loopback mask is a host setting, so what it was before the run is kept
# here and restored by `down`, including after an interrupted sweep.
LO_STATE="$HELPER_DIR/loopback-rps.before"

fail() { echo "capacity_topology: $*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || exec sudo -n "$0" "$@"
}

exists() { ip netns list 2>/dev/null | awk '{print $1}' | grep -qx "$1"; }

# Run a command on this host, or in one namespace.
in_ns() {   # <ns|-> command...
    local ns="$1"; shift
    if [ "$ns" = "-" ]; then "$@"; else ip netns exec "$ns" "$@"; fi
}

# A CPU list, as the hex bitmap /sys/.../rps_cpus takes.
mask_of() {   # "12-19" -> ff000
    local spec="${1//,/ }" part lo hi cpu mask=0
    for part in $spec; do
        case "$part" in
            *-*) lo="${part%%-*}"; hi="${part##*-}" ;;
            *)   lo="$part"; hi="$part" ;;
        esac
        for ((cpu = lo; cpu <= hi; cpu++)); do
            { [ "$cpu" -ge 0 ] && [ "$cpu" -le 63 ]; } \
                || fail "cpu $cpu in '$1' is outside 0-63"
            mask=$((mask | (1 << cpu)))
        done
    done
    printf '%x' "$mask"
}

# Steer every receive queue of one device to one CPU mask, and read it back.
set_rps() {   # <ns|-> <device> <mask> <label>
    local ns="$1" device="$2" mask="$3" label="$4" queue read_back
    for queue in $(in_ns "$ns" sh -c "ls /sys/class/net/$device/queues | grep '^rx-'"); do
        in_ns "$ns" sh -c "echo $mask > /sys/class/net/$device/queues/$queue/rps_cpus" \
            || fail "cannot steer $device/$queue ($label)"
        read_back="$(in_ns "$ns" cat "/sys/class/net/$device/queues/$queue/rps_cpus" 2>/dev/null)"
        [ "$((16#${read_back:-0}))" -eq "$((16#$mask))" ] \
            || fail "$device/$queue ($label) reads back $read_back, wanted $mask"
    done
    echo "capacity_topology: $label steering: $device rx -> $mask" >&2
}

tune_limits() {   # <ns|->
    local ns="$1" key value
    for key in net.core.rmem_max net.core.wmem_max; do
        value="$(in_ns "$ns" sysctl -n "$key")" || fail "cannot read $key in ${ns/-/host}"
        [ "$value" -ge "$SOCKMEM" ] \
            || fail "$key is $value in ${ns/-/host}; the SRT sink needs $SOCKMEM (sudo sysctl -w $key=$SOCKMEM)"
    done
}

# Namespaces, the pair (destroyed with them) and a half-created host-side end.
remove_net() {
    local ns
    for ns in "$NS_SUT" "$NS_RECV"; do
        ! exists "$ns" || ip netns delete "$ns"
    done
    ip link delete "$SUT_IF" 2>/dev/null
    ip link delete "$RECV_IF" 2>/dev/null
    return 0
}

# net.ipv4.tcp_tw_reuse is per namespace, so it is set, not only checked.
tune_ns() {   # <namespace>
    ip netns exec "$1" sysctl -qw net.ipv4.tcp_tw_reuse=1 >/dev/null \
        || fail "cannot set tcp_tw_reuse in $1"
    tune_limits "$1"
}

# Copy then rename: a running helper cannot be overwritten in place (ETXTBSY).
make_helper() {
    local nsenter
    nsenter="$(command -v nsenter)" || fail "nsenter not found"
    mkdir -p "$HELPER_DIR" || fail "cannot create $HELPER_DIR"
    cp "$nsenter" "$HELPER.tmp" || fail "cannot copy nsenter"
    setcap cap_sys_admin,cap_net_admin+ep "$HELPER.tmp" \
        || fail "cannot grant namespace-entry capabilities to $HELPER"
    mv -f "$HELPER.tmp" "$HELPER" || fail "cannot install $HELPER"
}

create_net() {
    ip netns add "$NS_SUT" || return 1
    ip netns add "$NS_RECV" || return 1
    ip link add "$SUT_IF" numtxqueues "$QUEUES" numrxqueues "$QUEUES" \
        type veth peer name "$RECV_IF" \
        numtxqueues "$QUEUES" numrxqueues "$QUEUES" || return 1
    ip link set "$SUT_IF" netns "$NS_SUT" || return 1
    ip link set "$RECV_IF" netns "$NS_RECV" || return 1
    ip -n "$NS_SUT" addr add "$SUT_ADDR" dev "$SUT_IF" || return 1
    ip -n "$NS_SUT" link set "$SUT_IF" mtu "$MTU" up || return 1
    ip -n "$NS_SUT" link set lo up || return 1
    ip -n "$NS_RECV" addr add "$RECV_ADDR" dev "$RECV_IF" || return 1
    ip -n "$NS_RECV" link set "$RECV_IF" mtu "$MTU" up || return 1
    ip -n "$NS_RECV" link set lo up
}

namespace_up() {
    # Reuse only a complete topology; anything partial is rebuilt.
    if exists "$NS_SUT" && exists "$NS_RECV" \
        && ip -n "$NS_SUT" -br addr show "$SUT_IF" 2>/dev/null | grep -q "$SUT_ADDR" \
        && ip -n "$NS_RECV" -br addr show "$RECV_IF" 2>/dev/null | grep -q "$RECV_ADDR"; then
        echo "capacity_topology: namespaces $NS_SUT and $NS_RECV already exist; reusing them" >&2
    else
        remove_net
        create_net || { remove_net; fail "cannot create the veth topology"; }
    fi
    tune_ns "$NS_SUT"
    tune_ns "$NS_RECV"
    set_rps "$NS_RECV" "$RECV_IF" "$(mask_of "$RPS_RECV_CPUS")" "receiver ingress"
    set_rps "$NS_SUT" "$SUT_IF" "$(mask_of "$RPS_SUT_CPUS")" "sender ingress"
    make_helper
    echo "CAPACITY_SENDER_EXEC='$HELPER --net=/run/netns/$NS_SUT --no-fork'"
    echo "CAPACITY_RECEIVER_EXEC='$HELPER --net=/run/netns/$NS_RECV --no-fork'"
    echo "CAPACITY_RECEIVER_ADDR='${RECV_ADDR%%/*}'"
    echo "CAPACITY_SENDER_ADDR='${SUT_ADDR%%/*}'"
}

namespace_down() {
    local left=0
    remove_net
    rm -f "$HELPER" "$HELPER.tmp"
    rmdir "$HELPER_DIR" 2>/dev/null
    for ns in "$NS_SUT" "$NS_RECV"; do
        exists "$ns" && { echo "capacity_topology: $ns still exists" >&2; left=1; }
    done
    [ -e "$HELPER" ] && { echo "capacity_topology: $HELPER still exists" >&2; left=1; }
    for link in "$SUT_IF" "$RECV_IF"; do
        ip link show "$link" >/dev/null 2>&1 \
            && { echo "capacity_topology: host link $link remains" >&2; left=1; }
    done
    [ "$left" -eq 0 ] || fail "teardown left residue"
    echo "capacity_topology: $NS_SUT and $NS_RECV removed; no residue"
}

# One stack, so the single loopback device carries both directions and one mask
# has to serve them: the fanout direction is the volume that matters.
loopback_up() {
    tune_limits "-"
    # Whatever this host had before the run is what it gets back: the mask and
    # the reuse flag are host-global settings that outlive the sweep.
    local mask_before reuse_before
    mask_before="$(cat /sys/class/net/$LO_IF/queues/rx-0/rps_cpus 2>/dev/null)" \
        || fail "cannot read $LO_IF steering"
    reuse_before="$(sysctl -n net.ipv4.tcp_tw_reuse 2>/dev/null)" \
        || fail "cannot read net.ipv4.tcp_tw_reuse"
    if [ ! -e "$LO_STATE" ]; then
        mkdir -p "$(dirname "$LO_STATE")" || fail "cannot write $LO_STATE"
        printf '%s %s\n' "${mask_before:-0}" "${reuse_before:-0}" > "$LO_STATE" \
            || fail "cannot record $LO_STATE"
    fi
    # Every destination of one rung shares 127.0.0.1, so the TCP mixes need the
    # sockets of the previous rung reusable; one namespace gets this per
    # namespace, a shared stack needs it here.
    sysctl -qw net.ipv4.tcp_tw_reuse=1 || fail "cannot set net.ipv4.tcp_tw_reuse"
    set_rps "-" "$LO_IF" "$(mask_of "$RPS_RECV_CPUS")" "shared ingress"
}

loopback_down() {
    local state="0 0"
    [ ! -e "$LO_STATE" ] || state="$(cat "$LO_STATE")"
    set_rps "-" "$LO_IF" "${state%% *}" "shared ingress"
    sysctl -qw net.ipv4.tcp_tw_reuse="${state##* }" \
        || fail "cannot restore net.ipv4.tcp_tw_reuse"
    rm -f "$LO_STATE"
}

status_common() {
    # net.core.rmem_max and wmem_max exist per namespace; netdev_max_backlog
    # does not (it is host-global), so it is read from the host either way.
    local ns="$1"
    printf 'rmem_max/wmem_max: %s; netdev_max_backlog (host): %s\n' \
        "$(in_ns "$ns" sysctl -n net.core.rmem_max net.core.wmem_max 2>/dev/null | tr '\n' ' ')" \
        "$(cat /proc/sys/net/core/netdev_max_backlog 2>/dev/null)"
}

case "$MODE:$ACTION" in
    namespace:up)
        need_root "$@"; namespace_up ;;
    namespace:down)
        need_root "$@"; namespace_down ;;
    namespace:status)
        need_root "$@"
        for ns in "$NS_SUT" "$NS_RECV"; do
            exists "$ns" || { echo "capacity_topology: no namespace $ns" >&2; exit 1; }
        done
        ip -n "$NS_SUT" -br addr show "$SUT_IF"
        ip -n "$NS_RECV" -br addr show "$RECV_IF"
        echo "queues: $(ip -n "$NS_SUT" -d link show "$SUT_IF" 2>/dev/null \
            | grep -o 'numtxqueues [0-9]*\|numrxqueues [0-9]*' | tr '\n' ' ')"
        echo "steering: $SUT_IF rx=$(in_ns "$NS_SUT" cat /sys/class/net/$SUT_IF/queues/rx-0/rps_cpus) \
$RECV_IF rx=$(in_ns "$NS_RECV" cat /sys/class/net/$RECV_IF/queues/rx-0/rps_cpus)"
        echo "limits: $(status_common "$NS_SUT")"
        echo "helper: $([ -x "$HELPER" ] && echo present || echo absent)"
        echo "host filter rules for $SUT_IF/$RECV_IF: $(iptables -S 2>/dev/null \
            | grep -c "$SUT_IF\|$RECV_IF")"
        ;;
    loopback:up)
        need_root "$@"; loopback_up ;;
    loopback:down)
        need_root "$@"; loopback_down
        echo "capacity_topology: $LO_IF steering restored" ;;
    loopback:status)
        need_root "$@"
        echo "steering: $LO_IF rx=$(cat /sys/class/net/$LO_IF/queues/rx-0/rps_cpus)"
        echo "limits: $(status_common -)"
        ;;
    mask:*)
        case "$ACTION" in
            ''|*[!0-9,-]*) fail "usage: capacity_topology.sh mask <cpu-list>" ;;
            *) mask_of "$ACTION" ;;
        esac ;;
    :*)
        fail "usage: capacity_topology.sh <namespace|loopback> up|down|status [ns-sut] [ns-recv]" ;;
esac

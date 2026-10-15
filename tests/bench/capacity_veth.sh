#!/usr/bin/env bash
#
# The veth topology for capacity runs: two network namespaces with their own
# interfaces, addresses, MTU, kernel UDP stacks and firewall tables, joined by
# one multi-queue veth pair.  It exists because loopback is not a network -
# there is no device, no queueing discipline, no per-interface counters and no
# MTU - and a sender/receiver split should be exercised over a real device
# path before anyone claims a number from one.
#
#   tests/bench/capacity_veth.sh up [ns-sut] [ns-recv]   create, print the env
#   tests/bench/capacity_veth.sh down [ns-sut] [ns-recv] remove everything
#   tests/bench/capacity_veth.sh status [ns-sut] [ns-recv]
#
# Then run the harness with what `up` prints, for example:
#
#   eval "$(tests/bench/capacity_veth.sh up)"
#   CAPACITY_QUALITY_MIXES=pure-srt CAPACITY_QUALITY_STEPS="1 16 32" \
#   CAPACITY_QUALITY_SECONDS=30 PHASES=capacity-quality-ladder \
#   tests/bench/ingest_egress_fanout.sh
#
# Why two namespaces and not one
# ------------------------------
# A sender on the host with the receiver in a namespace still sends every
# receiver-initiated packet (HLS readers, RTMP sink readiness) through the
# host's INPUT chain, where a default-drop firewall drops it.  The usual
# workaround is an iptables ACCEPT rule for the veth interface, which adds a
# per-packet netfilter traversal to the measurement path.  With the sender in
# its own namespace the traffic never enters the host's netfilter at all: the
# pair is a point-to-point wire between two non-root network namespaces, and
# both namespaces start with empty filter tables.  No rule is installed, so
# nothing has to be removed, and no packet in the measured path is filtered.
#
# What this topology does and does not establish:
#
#   * it does establish that every sender and receiver works over a
#     non-loopback address, through a device with an MTU, with each side's
#     kernel UDP path, per-interface counters and a firewall boundary in
#     play, and that the harness's addressing, readiness and artifact
#     plumbing do not depend on 127.0.0.1;
#   * it does not establish anything about a NIC: the veth pair is a kernel
#     software device, so its bandwidth is memory bandwidth and it has no
#     offloads, no ring sizes and no interrupts;
#   * it does not separate CPU: both sides are still the same machine, so CPU
#     placement (CAPACITY_NGINX_CPUS, CAPACITY_RECEIVER_CPUS) is what keeps
#     the attribution honest.
#
# Host residue
# ------------
# `up` creates exactly four things: two network namespaces, one veth pair and
# the capability-bearing namespace-entry helper the harness uses to launch
# processes inside a namespace as the invoking user.  `down` removes all four
# and re-checks.  Nothing is written to /usr/local, nothing is added to any
# firewall, and no host interface is created.
#
# HLS-origin readers reach the sender's media-only listener over the veth.

set -uo pipefail

ACTION="${1:-up}"
NS_SUT="${2:-nmsut}"
NS_RECV="${3:-nmrecv}"
SUT_IF="veth-sut"
RECV_IF="veth-recv"
SUT_ADDR="10.200.0.1/24"
RECV_ADDR="10.200.0.2/24"
SUT_IP="${SUT_ADDR%%/*}"
RECV_IP="${RECV_ADDR%%/*}"
MTU="${CAPACITY_VETH_MTU:-1500}"
QUEUES="${CAPACITY_VETH_QUEUES:-8}"
BACKLOG="${CAPACITY_VETH_BACKLOG:-100000}"
SOCKMEM="${CAPACITY_VETH_SOCKMEM:-67108864}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The helper lives under .build, which is already scratch, and `down` deletes
# it: a capability-bearing binary is never left behind for the next run to
# find and trust.
HELPER_DIR="${CAPACITY_VETH_BIN_DIR:-$ROOT/.build/capacity-veth}"
HELPER="$HELPER_DIR/nsenter-net"

fail() { echo "capacity_veth: $*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || exec sudo -n "$0" "$@"
}

exists() { ip netns list 2>/dev/null | awk '{print $1}' | grep -qx "$1"; }

# The user the harness runs as; namespace entry must not silently change it.
invoking_user() { echo "${SUDO_USER:-$(id -un)}"; }

# net.core.netdev_max_backlog is per-CPU softnet state, not per namespace,
# but rmem/wmem maxima and the port range are namespaced, so each side gets
# its own.  A namespace starts with the initial namespace's values copied,
# so setting them here is what makes the two sides independent.
tune_ns() {   # <namespace>
    local ns="$1"
    ip netns exec "$ns" sysctl -qw \
        net.core.netdev_max_backlog="$BACKLOG" \
        net.core.rmem_max="$SOCKMEM" \
        net.core.wmem_max="$SOCKMEM" \
        net.core.rmem_default=212992 \
        net.core.wmem_default=212992 \
        net.ipv4.tcp_tw_reuse=1 2>/dev/null || true
}

make_helper() {
    mkdir -p "$HELPER_DIR" || fail "cannot create $HELPER_DIR"
    cp "$(command -v nsenter)" "$HELPER" || fail "cannot copy nsenter"
    setcap cap_sys_admin,cap_net_admin+ep "$HELPER" \
        || fail "cannot grant namespace-entry capabilities to $HELPER"
}

remove_helper() {
    rm -f "$HELPER" 2>/dev/null || true
    rmdir "$HELPER_DIR" 2>/dev/null || true
}

case "$ACTION" in
    up)
        need_root "$@"
        tune_requested=0
        if exists "$NS_SUT" && exists "$NS_RECV"; then
            echo "capacity_veth: namespaces $NS_SUT and $NS_RECV already exist; reusing them" >&2
        else
            for ns in "$NS_SUT" "$NS_RECV"; do
                exists "$ns" && ip netns delete "$ns" 2>/dev/null || true
            done
            ip netns add "$NS_SUT" || fail "cannot create namespace $NS_SUT"
            ip netns add "$NS_RECV" || fail "cannot create namespace $NS_RECV"
            ip link add "$SUT_IF" numtxqueues "$QUEUES" numrxqueues "$QUEUES" \
                type veth peer name "$RECV_IF" \
                numtxqueues "$QUEUES" numrxqueues "$QUEUES" \
                || fail "cannot create the veth pair"
            ip link set "$SUT_IF" netns "$NS_SUT" || fail "cannot move $SUT_IF"
            ip link set "$RECV_IF" netns "$NS_RECV" || fail "cannot move $RECV_IF"
            ip -n "$NS_SUT" addr add "$SUT_ADDR" dev "$SUT_IF" \
                || fail "cannot address $SUT_IF"
            ip -n "$NS_SUT" link set "$SUT_IF" up || fail "cannot bring up $SUT_IF"
            ip -n "$NS_SUT" link set "$SUT_IF" mtu "$MTU" || fail "cannot set the MTU"
            ip -n "$NS_SUT" link set lo up || fail "cannot bring up the $NS_SUT loopback"
            ip -n "$NS_RECV" addr add "$RECV_ADDR" dev "$RECV_IF" \
                || fail "cannot address $RECV_IF"
            ip -n "$NS_RECV" link set "$RECV_IF" up || fail "cannot bring up $RECV_IF"
            ip -n "$NS_RECV" link set "$RECV_IF" mtu "$MTU" || fail "cannot set the MTU"
            ip -n "$NS_RECV" link set lo up || fail "cannot bring up the $NS_RECV loopback"
        fi
        tune_ns "$NS_SUT"
        tune_ns "$NS_RECV"
        make_helper
        echo "CAPACITY_SENDER_EXEC='$HELPER --net=/run/netns/$NS_SUT --no-fork'"
        echo "CAPACITY_RECEIVER_EXEC='$HELPER --net=/run/netns/$NS_RECV --no-fork'"
        echo "CAPACITY_RECEIVER_ADDR='$RECV_IP'"
        echo "CAPACITY_SENDER_ADDR='$SUT_IP'"
        echo "CAPACITY_VETH_USER='$(invoking_user)'"
        ;;
    down)
        need_root "$@"
        for ns in "$NS_SUT" "$NS_RECV"; do
            exists "$ns" && ip netns delete "$ns" || true
        done
        # The pair is destroyed with its namespaces; these two only catch a
        # half-created run.
        ip link delete "$SUT_IF" 2>/dev/null || true
        ip link delete "$RECV_IF" 2>/dev/null || true
        remove_helper
        left=0
        for ns in "$NS_SUT" "$NS_RECV"; do
            exists "$ns" && { echo "capacity_veth: $ns still exists" >&2; left=1; }
        done
        [ -e "$HELPER" ] && { echo "capacity_veth: $HELPER still exists" >&2; left=1; }
        [ -n "$(ip -br link show type veth 2>/dev/null)" ] \
            && { echo "capacity_veth: a host veth link remains" >&2; left=1; }
        [ "$left" -eq 0 ] \
            || fail "teardown left residue"
        echo "capacity_veth: $NS_SUT and $NS_RECV removed; no residue"
        ;;
    status)
        need_root "$@"
        for ns in "$NS_SUT" "$NS_RECV"; do
            exists "$ns" || { echo "capacity_veth: no namespace $ns"; exit 1; }
        done
        ip -n "$NS_SUT" -br addr show "$SUT_IF"
        ip -n "$NS_RECV" -br addr show "$RECV_IF"
        echo "queues: $(ip -n "$NS_SUT" -d link show "$SUT_IF" 2>/dev/null \
            | grep -o 'numtxqueues [0-9]*\|numrxqueues [0-9]*' | tr '\n' ' ')"
        echo "helper: $([ -x "$HELPER" ] && echo present || echo absent)"
        echo "host filter rules for $SUT_IF/$RECV_IF: $(iptables -S 2>/dev/null \
            | grep -c "$SUT_IF\|$RECV_IF")"
        ;;
    *)
        fail "unknown action $ACTION (up, down, status)"
        ;;
esac

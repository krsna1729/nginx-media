#!/usr/bin/env bash
#
# Veth topology for capacity runs: sender namespace and receiver namespace,
# each with its own UDP stack and empty firewall, joined by one multi-queue
# veth pair. It is a software device, not a NIC; CPU placement still decides
# attribution because both sides share the machine.
#
#   tests/bench/capacity_veth.sh up|down|status [ns-sut] [ns-recv]
#   eval "$(tests/bench/capacity_veth.sh up)"   # exports the harness env
#
# `up` creates two namespaces, one veth pair and a capability-bearing nsenter
# copy (so the harness enters a namespace as the invoking user); `down` removes
# all of it and re-checks. Nothing is added to any host firewall.

set -uo pipefail

ACTION="${1:-up}"
NS_SUT="${2:-nmsut}"
NS_RECV="${3:-nmrecv}"
SUT_IF="veth-sut"
RECV_IF="veth-recv"
SUT_ADDR="10.200.0.1/24"
RECV_ADDR="10.200.0.2/24"
MTU=1500
QUEUES=8
SOCKMEM=67108864

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HELPER_DIR="$ROOT/.build/capacity-veth"
HELPER="$HELPER_DIR/nsenter-net"

fail() { echo "capacity_veth: $*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || exec sudo -n "$0" "$@"
}

exists() { ip netns list 2>/dev/null | awk '{print $1}' | grep -qx "$1"; }

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

# Socket-buffer maxima and tw_reuse are per namespace; netdev_max_backlog is
# host-global and not visible inside one, so it is not set here.
tune_ns() {   # <namespace>
    ip netns exec "$1" sysctl -qw \
        net.core.rmem_max="$SOCKMEM" \
        net.core.wmem_max="$SOCKMEM" \
        net.core.rmem_default=212992 \
        net.core.wmem_default=212992 \
        net.ipv4.tcp_tw_reuse=1 >/dev/null || fail "cannot tune namespace $1"
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

case "$ACTION" in
    up)
        need_root "$@"
        # Reuse only a complete topology; anything partial is rebuilt.
        if exists "$NS_SUT" && exists "$NS_RECV" \
            && ip -n "$NS_SUT" -br addr show "$SUT_IF" 2>/dev/null | grep -q "$SUT_ADDR" \
            && ip -n "$NS_RECV" -br addr show "$RECV_IF" 2>/dev/null | grep -q "$RECV_ADDR"; then
            echo "capacity_veth: namespaces $NS_SUT and $NS_RECV already exist; reusing them" >&2
        else
            remove_net
            create_net || { remove_net; fail "cannot create the veth topology"; }
        fi
        tune_ns "$NS_SUT"
        tune_ns "$NS_RECV"
        make_helper
        echo "CAPACITY_SENDER_EXEC='$HELPER --net=/run/netns/$NS_SUT --no-fork'"
        echo "CAPACITY_RECEIVER_EXEC='$HELPER --net=/run/netns/$NS_RECV --no-fork'"
        echo "CAPACITY_RECEIVER_ADDR='${RECV_ADDR%%/*}'"
        echo "CAPACITY_SENDER_ADDR='${SUT_ADDR%%/*}'"
        ;;
    down)
        need_root "$@"
        remove_net
        rm -f "$HELPER" "$HELPER.tmp"
        rmdir "$HELPER_DIR" 2>/dev/null
        left=0
        for ns in "$NS_SUT" "$NS_RECV"; do
            exists "$ns" && { echo "capacity_veth: $ns still exists" >&2; left=1; }
        done
        [ -e "$HELPER" ] && { echo "capacity_veth: $HELPER still exists" >&2; left=1; }
        for link in "$SUT_IF" "$RECV_IF"; do
            ip link show "$link" >/dev/null 2>&1 \
                && { echo "capacity_veth: host link $link remains" >&2; left=1; }
        done
        [ "$left" -eq 0 ] || fail "teardown left residue"
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

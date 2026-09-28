#!/usr/bin/env bash
#
# The veth topology for capacity runs: a network namespace with its own
# interface, address, MTU and kernel UDP stack, reached from the host over a
# veth pair.  It exists because loopback is not a network - there is no
# device, no queueing discipline, no per-interface counters and no MTU - and
# a sender/receiver split should be exercised over a real device path before
# anyone claims a number from one.
#
#   tests/bench/capacity_veth.sh up [ns]     create, and print the env to use
#   tests/bench/capacity_veth.sh down [ns]   remove
#   tests/bench/capacity_veth.sh status [ns]
#
# Then run the harness with what `up` prints, for example:
#
#   eval "$(tests/bench/capacity_veth.sh up)"
#   CAPACITY_QUALITY_MIXES=pure-srt CAPACITY_QUALITY_STEPS="1 16 32" \
#   CAPACITY_QUALITY_SECONDS=30 PHASES=capacity-quality-ladder \
#   tests/bench/ingest_egress_fanout.sh
#
# What this topology does and does not establish:
#
#   * it does establish that every receiver works over a non-loopback
#     address, through a device with an MTU, with the kernel's UDP path and
#     per-interface counters in play, and that the harness's addressing,
#     readiness and artifact plumbing do not depend on 127.0.0.1;
#   * it does not establish anything about a NIC: the veth pair is a kernel
#     software device, so its bandwidth is memory bandwidth and it has no
#     offloads, no ring sizes and no interrupts;
#   * it does not separate CPU: sender and receivers are still the same
#     machine, so CPU placement (CAPACITY_NGINX_CPUS, CAPACITY_RECEIVER_CPUS)
#     is what keeps the attribution honest.
#
# The HLS-origin workload is not available over this topology: its readers
# fetch a playlist from the sender's own nginx, which listens on the host's
# loopback.  SRT, RTMP and HLS push are.

set -uo pipefail

ACTION="${1:-up}"
NS="${2:-nmrecv}"
HOST_IF="nmveth0"
NS_IF="nmveth1"
HOST_ADDR="10.200.0.1/24"
NS_ADDR="10.200.0.2/24"
MTU="${CAPACITY_VETH_MTU:-1500}"

fail() { echo "capacity_veth: $*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || exec sudo -n "$0" "$@"
}

exists() { ip netns list 2>/dev/null | awk '{print $1}' | grep -qx "$NS"; }

case "$ACTION" in
    up)
        need_root "$@"
        if exists; then
            echo "capacity_veth: namespace $NS already exists; reusing it" >&2
        else
            ip netns add "$NS" || fail "cannot create namespace $NS"
            ip link add "$HOST_IF" type veth peer name "$NS_IF" \
                || fail "cannot create the veth pair"
            ip link set "$NS_IF" netns "$NS" || fail "cannot move $NS_IF"
            ip addr add "$HOST_ADDR" dev "$HOST_IF" || fail "cannot address $HOST_IF"
            ip link set "$HOST_IF" up || fail "cannot bring up $HOST_IF"
            ip link set "$HOST_IF" mtu "$MTU" || fail "cannot set the MTU"
            ip -n "$NS" addr add "$NS_ADDR" dev "$NS_IF" \
                || fail "cannot address $NS_IF"
            ip -n "$NS" link set "$NS_IF" up || fail "cannot bring up $NS_IF"
            ip -n "$NS" link set "$NS_IF" mtu "$MTU" || fail "cannot set the MTU"
            ip -n "$NS" link set lo up || fail "cannot bring up the namespace loopback"
        fi
        ip netns exec "$NS" sysctl -qw net.core.rmem_max=8388608 2>/dev/null || true
        ip netns exec "$NS" sysctl -qw net.core.wmem_max=8388608 2>/dev/null || true
        echo "CAPACITY_RECEIVER_EXEC='nsenter --net=/run/netns/$NS --no-fork'"
        echo "CAPACITY_RECEIVER_ADDR='10.200.0.2'"
        ;;
    down)
        need_root "$@"
        if exists; then
            ip netns delete "$NS" || fail "cannot delete namespace $NS"
        fi
        ip link delete "$HOST_IF" 2>/dev/null || true
        echo "capacity_veth: $NS removed"
        ;;
    status)
        need_root "$@"
        exists || { echo "capacity_veth: no namespace $NS"; exit 1; }
        ip -br addr show "$HOST_IF"
        ip -n "$NS" -br addr show "$NS_IF"
        ip -n "$NS" route
        ;;
    *)
        fail "unknown action $ACTION (up, down, status)"
        ;;
esac

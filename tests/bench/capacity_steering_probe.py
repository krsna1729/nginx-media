#!/usr/bin/env python3
"""Measure where a host charges receive processing for one UDP flow.

The capacity topologies steer receive processing onto the receiver CPUs,
because a packet arriving on a software device is otherwise processed on the
CPU that transmitted it, and the sender then pays for the receivers\' ingress.
That behaviour is a property of the kernel and the device, not of this
harness, so a new machine is checked rather than assumed:

  capacity_steering_probe.py run <label> <address> <port> <sender-cpu> \
      <receiver-cpu> <seconds> [--send-ns <ns>] [--recv-ns <ns>]

It sends one fixed UDP flow between two pinned processes and prints the
per-CPU softirq and system time the kernel spent. Run it once with the
steering mask set and once with it cleared, as `make bench-topology-*` or
`capacity_topology.sh` leaves it: with the mask cleared the sender\'s own CPU
should carry the receive softirq, and with it set that time should appear on a
receiver CPU instead, with the sender sending more packets for the same
window. A host or device where the mask changes nothing cannot separate the
two sides by steering, and its capacity numbers will read low.
"""
import json
import os
import socket
import subprocess
import sys
import time

FIELDS = ("user", "nice", "system", "idle", "iowait", "irq", "softirq")


def per_cpu():
    rows = {}
    with open("/proc/stat") as source:
        for line in source:
            parts = line.split()
            if len(parts) < 8 or not parts[0].startswith("cpu") or parts[0] == "cpu":
                continue
            if not parts[0][3:].isdigit():
                continue
            rows[int(parts[0][3:])] = {name: int(value) for name, value in
                                       zip(FIELDS, parts[1:8])}
    return rows


def receiver(address, port, seconds, ready):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 << 20)
    sock.bind((address, port))
    sock.settimeout(seconds + 5)
    ready.write("ready\n")
    ready.flush()
    packets = 0
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if not sock.recv(65536):
                break
        except socket.timeout:
            break
        packets += 1
    print(json.dumps({"packets_received": packets}))


def sender(address, port, cpu, seconds, ready_path):
    for _ in range(200):  # wait for the receiver to bind, without blocking on it
        if os.path.exists(ready_path) and os.path.getsize(ready_path):
            break
        time.sleep(0.01)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 << 20)
    payload = bytes(1200)
    packets = 0
    deadline = time.monotonic() + seconds
    proc_cpu = os.sched_getaffinity(0)
    while time.monotonic() < deadline:
        sock.sendto(payload, (address, port))
        packets += 1
    print(json.dumps({"packets_sent": packets, "sender_cpus": sorted(proc_cpu)}))


def command_prefix(namespace):
    return ["sudo", "-n", "ip", "netns", "exec", namespace] if namespace else []


def main():
    options, positional = {}, []
    index = 0
    argv = sys.argv[1:]
    while index < len(argv):
        if argv[index].startswith("--"):
            options[argv[index].lstrip("-")] = argv[index + 1]
            index += 2
        else:
            positional.append(argv[index])
            index += 1
    if positional and positional[0] == "run":
        positional = positional[1:]
    label, address, port, sender_cpu, receiver_cpu, seconds = (
        positional[0], positional[1], int(positional[2]), int(positional[3]),
        int(positional[4]), float(positional[5]))
    send_ns, recv_ns = options.get("send-ns", ""), options.get("recv-ns", "")
    ready_path = "/tmp/rps-probe.ready"
    if os.path.exists(ready_path):
        os.remove(ready_path)
    ready = open(ready_path, "w")
    receiver_process = subprocess.Popen(
        command_prefix(recv_ns) + ["taskset", "-c", str(receiver_cpu), sys.executable, __file__,
                                   "receive", address, str(port), str(seconds), ready_path])
    time.sleep(0.3)
    before = per_cpu()
    started = time.monotonic()
    sender_output = subprocess.run(
        command_prefix(send_ns) + ["taskset", "-c", str(sender_cpu), sys.executable, __file__,
                                   "send", address, str(port), str(sender_cpu), str(seconds),
                                   ready_path],
        capture_output=True, text=True, timeout=seconds + 30).stdout
    elapsed = time.monotonic() - started
    receiver_output = receiver_process.communicate(timeout=seconds + 30)[0]
    after = per_cpu()
    hz = os.sysconf("SC_CLK_TCK")

    def delta(cpu, field):
        return (after[cpu][field] - before[cpu][field]) / hz / elapsed * 100

    report = {
        "label": label, "address": address, "port": port, "seconds": round(elapsed, 2),
        "sender_cpu": sender_cpu, "receiver_cpu": receiver_cpu,
        "sender_cpu_system_pct": round(delta(sender_cpu, "system"), 2),
        "sender_cpu_softirq_pct": round(delta(sender_cpu, "softirq"), 2),
        "receiver_cpu_system_pct": round(delta(receiver_cpu, "system"), 2),
        "receiver_cpu_softirq_pct": round(delta(receiver_cpu, "softirq"), 2),
        "softirq_pct_by_cpu": {cpu: round(delta(cpu, "softirq"), 2)
                               for cpu in sorted(after)
                               if delta(cpu, "softirq") >= 1.0},
        "sender": json.loads(sender_output or "{}"),
        "receiver": json.loads(receiver_output or "{}"),
    }
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "send":
        sender(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5]), sys.argv[6])
    elif len(sys.argv) > 2 and sys.argv[1] == "receive":
        receiver(sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), open(sys.argv[5], "w"))
    else:
        main()

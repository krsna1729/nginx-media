#!/usr/bin/env python3
"""Decide whether a host pair can carry an offered load, before measuring it.

A capacity number is only a statement about the software if the host, the
network and the receivers were able to carry the load in the first place.
This is the preflight that says so: it fingerprints the sender, the receiver
and the path between them, and compares the measured headroom with what the
requested rung needs.  When something is short it returns
`infrastructure-limited` with the number that was short - never a capacity
claim about nginx.

Subcommands:

  host   [--role sender|receiver] [--json out]
         one host's resources: CPU model and count, the permitted set, the
         cgroup quota, frequency and throttling, NUMA placement, kernel,
         memory, every interface's speed/driver/offloads/MTU, the kernel's
         UDP buffer limits, and the transport library a binary links.

  probe --listen --port N --seconds S --json out
         receive side of the path probe: counts what arrives, echoes pings.

  probe --peer HOST --port N --seconds S --json out
         send side: measures round-trip time and loss on a ping sample, then
         offers as much UDP payload as the path will take and reports what it
         sent.  The receiver's own count is the one that matters; the judge
         compares them.

  judge --destinations N --bitrate-bps B --sender a.json --network b.json
        [--receiver c.json] [--sender-cpu-per-gbps X] [--json out]
         compares the measurements with the request and returns a verdict:
         "ok" or "infrastructure-limited", with every limit that was hit and
         the evidence for it.

The overhead model is explicit and conservative: SRT carries a 1316-byte
payload in a 44-byte IPv4/UDP/SRT data header (3.3%), and the default
retransmission allowance is 15% on top, so the network requirement is the
offered payload times 1.20.  `--overhead-factor` overrides it.  CPU has no
model here on purpose: a core count cannot say how many cores per delivered
Gbit/s the software needs, so that number is either supplied from a measured
rung (`--sender-cpu-per-gbps`, `--receiver-cpu-per-gbps`) or reported as
unknown - and an unknown is never a pass.
"""

import argparse
import glob
import json
import os
import platform
import re
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proc_cpu_sampler  # noqa: E402

MAGIC = b"NMPREFL1"  # exactly the 8 bytes the header carries
PING = 0x01
FLOOD = 0x02
PING_REPLY = 0x03
HEADER = struct.Struct("!8sB3xQ")  # magic, kind, padding, sequence
SRT_PAYLOAD_BYTES = 1316
SRT_HEADER_BYTES = 44  # 20 IPv4 + 8 UDP + 16 SRT data header
DEFAULT_OVERHEAD_FACTOR = 1.20
DEFAULT_PACKET_BYTES = 1400
PING_COUNT = 20
PING_TIMEOUT = 0.5


def read_first_line(path):
    try:
        with open(path, encoding="utf-8") as source:
            return source.readline().strip()
    except OSError:
        return None


def cpu_model():
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as source:
            for line in source:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return None


def numa_nodes():
    nodes = sorted(glob.glob("/sys/devices/system/node/node[0-9]*"))
    result = {}
    for node in nodes:
        name = os.path.basename(node)
        cpus = read_first_line(os.path.join(node, "cpulist"))
        result[name] = cpus
    return result


def interface_facts():
    """Speed, driver, MTU and offload state per interface, as the kernel
    reports them.  A virtual interface (lo, veth, docker0) has no speed, and
    saying so is better than assuming one."""
    interfaces = {}
    for name in sorted(os.listdir("/sys/class/net")):
        base = os.path.join("/sys/class/net", name)
        facts = {
            "operstate": read_first_line(os.path.join(base, "operstate")),
            "mtu": read_first_line(os.path.join(base, "mtu")),
            "speed_mbps": read_first_line(os.path.join(base, "speed")),
            "driver": os.path.basename(
                os.path.realpath(os.path.join(base, "device/driver")))
            if os.path.exists(os.path.join(base, "device/driver")) else None,
            "virtual": os.path.exists(os.path.join(base, "device")) is False,
        }
        features = read_first_line(os.path.join(base, "features"))
        if features:
            facts["offloads"] = features.split()
        interfaces[name] = facts
    return interfaces


def ethtool_facts(name):
    """`ethtool -k`/`-i` when the tool is present: the offload state the NIC
    actually applies, which /sys/class/net/features does not always match."""
    facts = {}
    for args, key in ((["-k", name], "offloads"),
                      (["-i", name], "driver"),
                      (["-S", name], "statistics")):
        try:
            out = subprocess.run(["ethtool"] + args, capture_output=True,
                                 text=True, timeout=10)
        except (OSError, subprocess.SubprocessError):
            return facts
        if out.returncode != 0:
            continue
        if key == "statistics":
            drops = {}
            for line in out.stdout.splitlines():
                parts = line.split(":")
                if len(parts) == 2 and any(
                        word in parts[0] for word in ("drop", "error", "miss")):
                    try:
                        drops[parts[0].strip()] = int(parts[1].strip())
                    except ValueError:
                        continue
            if drops:
                facts[key] = drops
            continue
        if key == "driver":
            facts[key] = out.stdout.splitlines()[0].strip() if out.stdout else None
            continue
        facts[key] = {}
        for line in out.stdout.splitlines():
            if line.startswith("Features for") or not line.strip():
                continue
            parts = line.split(":")
            if len(parts) == 2:
                facts[key][parts[0].strip()] = parts[1].strip()
    return facts


def kernel_udp_limits():
    limits = {}
    for key in ("net.core.rmem_max", "net.core.wmem_max", "net.core.rmem_default",
                "net.core.wmem_default", "net.core.netdev_max_backlog",
                "net.ipv4.udp_mem", "net.core.somaxconn"):
        path = "/proc/sys/" + key.replace(".", "/")
        value = read_first_line(path)
        if value is not None:
            limits[key] = value
    return limits


def softnet():
    stats = {}
    try:
        with open("/proc/net/softnet_stat", encoding="utf-8") as source:
            for index, line in enumerate(source):
                fields = line.split()
                if len(fields) >= 11:
                    stats[f"cpu{index}"] = {
                        "processed": int(fields[0], 16),
                        "dropped": int(fields[1], 16),
                        "time_squeeze": int(fields[2], 16),
                    }
    except OSError:
        pass
    return stats


def shared_library_version(path):
    """The library's own version string, from the file itself."""
    try:
        out = subprocess.run(["strings", "-a", path], capture_output=True,
                             text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    for line in out.stdout.splitlines():
        match = re.search(r"^(\d+\.\d+\.\d+)$", line.strip())
        if match:
            return match.group(1)
    return None


def transport_version(binary):
    """Which libsrt a binary carries, and which build of it.

    The soname alone cannot say whether a run used the distribution's library
    or the pinned one built from source - both are libsrt.so.1.5 - so the
    resolved path and the library's own version are reported with it.  A
    statically linked robotweax build has no resolved path and reports its
    version string instead.
    """
    if not binary or not os.path.exists(binary):
        return None
    version = None
    try:
        out = subprocess.run(["strings", "-a", binary], capture_output=True,
                             text=True, timeout=30)
        for line in out.stdout.splitlines():
            if "robotweax" in line.lower():
                version = "robotweax-srt"
                break
            if version is None and line.startswith("libsrt"):
                version = line
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        out = subprocess.run(["ldd", binary], capture_output=True, text=True,
                             timeout=10)
        for line in out.stdout.splitlines():
            # both names end in srt: the reference library is libsrt.so, the
            # clean-slate one librobotweax-srt.so
            if "srt" not in line or "=>" not in line:
                continue
            soname = line.split()[0].strip()
            resolved = line.split("=>")[-1].strip().split(" ")[0] \
                if "=>" in line else ""
            library_version = shared_library_version(resolved) \
                if resolved and os.path.exists(resolved) else None
            label = version or soname
            if resolved:
                return (f"{label} {library_version} ({resolved})"
                        if library_version else f"{label} ({resolved})")
            return label
    except (OSError, subprocess.SubprocessError):
        pass
    return version


def host_report(role, binary=None, pid=None):
    busy_start = proc_cpu_sampler.proc_stat_cpu()
    time.sleep(0.5)
    busy_end = proc_cpu_sampler.proc_stat_cpu()
    report = {
        "role": role,
        "kernel": platform.release(),
        "cpu_model": cpu_model(),
        "nproc_online": os.cpu_count(),
        "permitted_cpus": sorted(os.sched_getaffinity(0)),
        "cgroup": proc_cpu_sampler.cgroup_info(),
        "cpu_freq_khz": proc_cpu_sampler.cpu_frequencies(),
        "governor": read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/"
                                    "scaling_governor"),
        "no_turbo": read_first_line("/sys/devices/system/cpu/intel_pstate/"
                                    "no_turbo"),
        "energy_preference": read_first_line(
            "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"),
        "numa": numa_nodes(),
        "loadavg": read_first_line("/proc/loadavg"),
        "host_busy_fraction": busy_fraction(busy_start, busy_end),
        "memory": memory_facts(),
        "interfaces": interface_facts(),
        "kernel_udp_limits": kernel_udp_limits(),
        "softnet": softnet(),
        "transport": transport_version(binary),
    }
    if pid:
        report["process"] = proc_cpu_sampler.process_info(pid)
    for name in ("eth0", "enp0s31f6", "wlo1"):
        if name in report["interfaces"]:
            report["interfaces"][name].update(ethtool_facts(name))
    return report


def memory_facts():
    facts = {}
    try:
        with open("/proc/meminfo", encoding="utf-8") as source:
            for line in source:
                key, _, rest = line.partition(":")
                if key in ("MemTotal", "MemAvailable", "MemFree"):
                    facts[key] = rest.strip()
    except OSError:
        pass
    return facts


def busy_fraction(before, after):
    """Fraction of all online CPUs busy over the sample, from /proc/stat."""
    try:
        total_before = sum(before.values())
        total_after = sum(after.values())
        idle_before = before["idle"] + before.get("iowait", 0)
        idle_after = after["idle"] + after.get("iowait", 0)
        total = total_after - total_before
        idle = idle_after - idle_before
        if total <= 0:
            return None
        return round(1.0 - idle / total, 4)
    except (KeyError, TypeError):
        return None


def udp_socket_drops(port):
    """Per-socket UDP drops for the probe's own port, from /proc/net/udp.

    A receiver that cannot drain its socket is a receiver limit, and it must
    not be read as a slow path.  With SO_REUSEPORT there is one socket per
    thread, so the drops of every socket bound to the port are summed."""
    total = None
    try:
        with open("/proc/net/udp", encoding="utf-8") as source:
            next(source)
            for line in source:
                fields = line.split()
                if len(fields) < 13:
                    continue
                try:
                    local_port = int(fields[1].split(":")[1], 16)
                except (IndexError, ValueError):
                    continue
                if local_port != port:
                    continue
                # rx_queue tx_queue ... drops is the 13th column
                total = (total or 0) + int(fields[12])
    except (OSError, StopIteration):
        return None
    return total


def probe_listen(port, seconds, packet_bytes, bind, threads=1):
    """Receive side.  One socket per thread with SO_REUSEPORT: a single
    Python socket cannot drain more than a few Gbit/s, and a probe that
    reports its own drain rate as the path's capacity is worse than no
    probe."""
    sockets = []
    for _ in range(max(1, threads)):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        sock.bind((bind, port))
        sock.settimeout(0.5)
        sockets.append(sock)

    counters = [{"bytes": 0, "packets": 0, "pings": 0, "largest": 0,
                 "first_ns": None, "last_ns": None} for _ in sockets]

    def drain(sock, counter):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                data, peer = sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            now = time.monotonic_ns()
            if counter["first_ns"] is None:
                counter["first_ns"] = now
            counter["last_ns"] = now
            counter["largest"] = max(counter["largest"], len(data))
            if data.startswith(MAGIC) and len(data) >= HEADER.size:
                _, kind, sequence = HEADER.unpack_from(data)
                if kind == PING:
                    counter["pings"] += 1
                    reply = HEADER.pack(MAGIC, PING_REPLY, sequence) + data[
                        HEADER.size:]
                    try:
                        sock.sendto(reply, peer)
                    except OSError:
                        pass
                    continue
            counter["bytes"] += len(data)
            counter["packets"] += 1

    workers = [threading.Thread(target=drain, args=(sock, counter))
               for sock, counter in zip(sockets, counters)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()

    received_bytes = sum(c["bytes"] for c in counters)
    received_packets = sum(c["packets"] for c in counters)
    pings = sum(c["pings"] for c in counters)
    largest = max((c["largest"] for c in counters), default=0)
    starts = [c["first_ns"] for c in counters if c["first_ns"]]
    ends = [c["last_ns"] for c in counters if c["last_ns"]]
    window = ((max(ends) - min(starts)) / 1e9) if starts and ends else 0.0
    drops = udp_socket_drops(port)
    for sock in sockets:
        sock.close()
    return {
        "side": "receiver",
        "seconds": seconds,
        "threads": len(sockets),
        "packet_bytes": packet_bytes,
        "received_bytes": received_bytes,
        "received_packets": received_packets,
        "largest_datagram": largest,
        "pings": pings,
        "socket_drops": drops,
        "window_s": round(window, 6),
        "rate_gbps": round(received_bytes * 8 / window / 1e9, 4) if window else None,
    }


def probe_send(peer, port, seconds, packet_bytes, threads=1):
    """Send side: one ping sample for the path's latency and loss, then as
    much UDP payload as the threads can push.  The receiver's count is the
    measurement; this side's count says whether the load was ever offered."""
    address = (peer, port)
    ping_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ping_sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    ping_sock.settimeout(PING_TIMEOUT)
    rtts = []
    for sequence in range(PING_COUNT):
        packet = HEADER.pack(MAGIC, PING, sequence) + bytes(64)
        start = time.monotonic_ns()
        try:
            ping_sock.sendto(packet, address)
            data, _ = ping_sock.recvfrom(2048)
        except (socket.timeout, OSError):
            continue
        if data.startswith(MAGIC) and HEADER.unpack_from(data)[1] == PING_REPLY:
            rtts.append((time.monotonic_ns() - start) / 1e6)
    ping_sock.close()

    payload = bytes(packet_bytes)
    counters = [{"bytes": 0, "packets": 0} for _ in range(max(1, threads))]

    def flood(counter):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
        deadline = time.monotonic() + seconds
        sequence = 0
        while time.monotonic() < deadline:
            packet = HEADER.pack(MAGIC, FLOOD, sequence) + payload[HEADER.size:]
            try:
                sent = sock.sendto(packet, address)
            except OSError:
                break
            counter["bytes"] += sent
            counter["packets"] += 1
            sequence += 1
        sock.close()

    workers = [threading.Thread(target=flood, args=(counter,))
               for counter in counters]
    start = time.monotonic()
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    elapsed = time.monotonic() - start
    sent_bytes = sum(c["bytes"] for c in counters)
    sent_packets = sum(c["packets"] for c in counters)
    rtts.sort()
    return {
        "side": "sender",
        "peer": peer,
        "port": port,
        "seconds": seconds,
        "threads": len(counters),
        "packet_bytes": packet_bytes,
        "sent_bytes": sent_bytes,
        "sent_packets": sent_packets,
        "send_window_s": round(elapsed, 6),
        "send_rate_gbps": round(sent_bytes * 8 / elapsed / 1e9, 4) if elapsed else None,
        "rtt_ms_p50": round(rtts[len(rtts) // 2], 3) if rtts else None,
        "rtt_ms_p95": round(rtts[min(len(rtts) - 1, int(len(rtts) * 0.95))], 3)
        if rtts else None,
        "ping_loss": round(1 - len(rtts) / PING_COUNT, 4),
    }


def parse_cpu_list(spec):
    """`0,2,4-6` or `0 2 4-6` into a set of CPU numbers."""
    cpus = set()
    for part in (spec or "").replace(",", " ").split():
        if "-" in part:
            first, _, last = part.partition("-")
            try:
                cpus.update(range(int(first), int(last) + 1))
            except ValueError:
                continue
        else:
            try:
                cpus.add(int(part))
            except ValueError:
                continue
    return cpus


def per_cpu_busy(window_s=0.5):
    """Busy fraction per CPU over a short window, from /proc/stat.

    The aggregate line cannot say which CPU was busy, and that is the whole
    question when a run is pinned: work on a CPU the run does not use costs
    the measurement nothing, work on one it does use contaminates it."""
    before = proc_cpu_sampler.proc_stat_per_cpu() or {}
    time.sleep(window_s)
    after = proc_cpu_sampler.proc_stat_per_cpu() or {}
    busy = {}
    for name, first in before.items():
        second = after.get(name)
        if not second:
            continue
        total = sum(second.values()) - sum(first.values())
        idle = (second["idle"] + second.get("iowait", 0)
                - first["idle"] - first.get("iowait", 0))
        if total > 0:
            busy[name] = round(1.0 - idle / total, 4)
    return busy


def foreign_cpu_consumers(window_s=0.5, limit=5):
    """Processes that used CPU during the window, by pid, biggest first.

    The benchmark's own programs are named by their command lines; anything
    else that is running is foreign to the measurement and is worth naming
    before an hour of rungs is read as a capacity result."""
    own = ("ingest_egress_fanout", "capacity_preflight", "srt_fanout_sink",
           "hls_push_capacity", "hls_capacity_readers", "proc_cpu_sampler",
           "nginx", "omarchy-benchmark", "taskset")
    first = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/stat", encoding="utf-8") as source:
                fields = source.read().rsplit(")", 1)[1].split()
            first[entry] = int(fields[11]) + int(fields[12])
        except (OSError, IndexError, ValueError):
            continue
    time.sleep(window_s)
    consumers = []
    for pid, used in first.items():
        try:
            with open(f"/proc/{pid}/stat", encoding="utf-8") as source:
                fields = source.read().rsplit(")", 1)[1].split()
            after = int(fields[11]) + int(fields[12])
            with open(f"/proc/{pid}/cmdline", encoding="utf-8",
                      errors="replace") as source:
                cmdline = source.read().replace("\0", " ").strip()
        except (OSError, IndexError, ValueError):
            continue
        seconds = (after - used) / os.sysconf("SC_CLK_TCK")
        if seconds <= 0.01:
            continue
        if any(marker in cmdline for marker in own):
            continue
        consumers.append({"pid": int(pid), "cpu_seconds": round(seconds, 3),
                          "command": cmdline[:120] or f"pid {pid}"})
    consumers.sort(key=lambda item: item["cpu_seconds"], reverse=True)
    return consumers[:limit]


def judge(args):
    sender = load(args.sender)
    network = load(args.network)
    if args.probe_sender:
        # The path is measured from both ends: the receiver's count is the
        # measurement, the sender's count says whether the load was offered.
        probe = load(args.probe_sender)
        for key in ("sent_bytes", "sent_packets", "send_window_s",
                    "rtt_ms_p50", "rtt_ms_p95", "ping_loss"):
            if key in probe and key not in network:
                network[key] = probe[key]
        network.setdefault("sender_side", probe)
    receivers = [load(path) for path in (args.receiver or [])]
    limits = []
    unknown = []

    payload_bps = args.destinations * args.bitrate_bps
    required_bps = payload_bps * args.overhead_factor
    details = {
        "destinations": args.destinations,
        "bitrate_bps": args.bitrate_bps,
        "payload_gbps": round(payload_bps / 1e9, 4),
        "overhead_factor": args.overhead_factor,
        "required_network_gbps": round(required_bps / 1e9, 4),
    }

    # --- the path: what the receiver counted, and what did not arrive
    received = network.get("received_bytes")
    sent = network.get("sent_bytes")
    window = network.get("window_s")
    send_window = network.get("send_window_s")
    if received is None or not window:
        unknown.append({"side": "network", "resource": "throughput",
                        "reason": "the path probe produced no receiver window"})
    else:
        measured_bps = received * 8 / window
        offered_bps = (sent * 8 / send_window) if sent and send_window else None
        details["measured_network_gbps"] = round(measured_bps / 1e9, 4)
        details["offered_network_gbps"] = (round(offered_bps / 1e9, 4)
                                           if offered_bps else None)
        drops = network.get("socket_drops")
        details["probe_receiver_socket_drops"] = drops
        if measured_bps < required_bps:
            if drops:
                # The probe's own receiver dropped datagrams, so the probe
                # stopped being a valid instrument at that rate: it cannot say
                # whether the path could carry more.  The benchmark's own
                # receivers are sharded C programs and may still carry it, so
                # this is evidence about the probe - reported, never turned
                # into a limit.  The receiver's *CPU* is the number that
                # decides the receiver side (see the CPU check below).
                unknown.append({
                    "side": "probe", "resource": "receiver-socket-drops",
                    "reason": f"the probe's own receiver counted "
                              f"{measured_bps / 1e9:.2f} Gbit/s and dropped "
                              f"{drops} datagrams"
                              + (f" while the sender offered "
                                 f"{offered_bps / 1e9:.2f} Gbit/s"
                                 if offered_bps else "")
                              + f", below the {required_bps / 1e9:.2f} Gbit/s "
                              f"requested; the probe cannot say whether the "
                              f"path carries more (more probe threads, or the "
                              f"CPU check)"})
            elif offered_bps is not None and offered_bps < required_bps:
                # Nothing was lost, but the load was never offered: that is a
                # limit of the probe, not a statement about the path.
                unknown.append({
                    "side": "network", "resource": "throughput",
                    "reason": f"the probe offered {offered_bps / 1e9:.2f} Gbit/s "
                              f"while {required_bps / 1e9:.2f} Gbit/s was "
                              f"requested, so the path was never asked to "
                              f"carry the load; the measured "
                              f"{measured_bps / 1e9:.2f} Gbit/s is a lower "
                              f"bound"})
            else:
                limits.append({
                    "side": "network", "resource": "throughput",
                    "measured": round(measured_bps / 1e9, 4),
                    "required": round(required_bps / 1e9, 4), "unit": "Gbit/s",
                    "reason": f"the path carried {measured_bps / 1e9:.2f} Gbit/s "
                              f"of UDP payload while {args.destinations} "
                              f"destinations at {args.bitrate_bps / 1e6:.1f} "
                              f"Mbit/s need {required_bps / 1e9:.2f} Gbit/s "
                              f"including {args.overhead_factor:.2f}x overhead"})
    if sent and received is not None and sent > 0:
        loss = 1 - received / sent
        details["probe_loss"] = round(loss, 5)
        offered_bps = details.get("offered_network_gbps")
        offered_bps = offered_bps * 1e9 if offered_bps else None
        # Loss measured while the path was driven far harder than the request
        # needs says nothing about the request, so it is only a limit when
        # the probe was run near the operating point.
        if (loss > args.max_probe_loss and offered_bps
                and required_bps <= offered_bps <= 4 * required_bps):
            limits.append({
                "side": "network", "resource": "loss",
                "measured": round(loss, 5), "required": args.max_probe_loss,
                "unit": "fraction",
                "reason": f"{loss:.2%} of the probe's datagrams never arrived "
                          f"at {offered_bps / 1e9:.2f} Gbit/s offered, which is "
                          f"the rate the request needs"})

    # --- the sender and receiver CPUs, against a measured cost per Gbit/s
    for role, report, cost, stated in (
            ("sender", sender, args.sender_cpu_per_gbps, args.sender_cpus),
            ("receiver", receivers[0] if receivers else None,
             args.receiver_cpu_per_gbps, args.receiver_cpus)):
        if not report:
            if cost is not None:
                # A cost without a host to spend it on is not a verdict: the
                # receiver's report was required and did not arrive.
                unknown.append({
                    "side": role, "resource": "cpu",
                    "reason": f"a measured CPU per delivered Gbit/s for the "
                              f"{role} but no {role} fingerprint to judge it "
                              f"against"})
            continue
        permitted = (len(parse_cpu_list(stated)) if stated
                     else len(report.get("permitted_cpus") or []))
        details[f"{role}_permitted_cpus"] = permitted
        if cost is None:
            unknown.append({
                "side": role, "resource": "cpu",
                "reason": f"no measured CPU per delivered Gbit/s for the "
                          f"{role}; {permitted} permitted CPUs is a count, not "
                          f"a capacity"})
            continue
        needed = payload_bps / 1e9 * cost / 100.0
        details[f"{role}_cpu_needed_cores"] = round(needed, 2)
        if needed > permitted:
            limits.append({
                "side": role, "resource": "cpu",
                "measured": permitted, "required": round(needed, 2),
                "unit": "cores",
                "reason": f"{payload_bps / 1e9:.2f} Gbit/s at {cost:.0f}% of a "
                          f"core per Gbit/s needs {needed:.1f} cores and this "
                          f"host permits {permitted}"})

    # --- memory: the receiver holds per-destination buffers, the sender holds
    #     queues; only an explicit bound is checked, never a guess
    if args.required_memory_mb:
        for role, report in (("sender", sender),
                             ("receiver", receivers[0] if receivers else None)):
            if not report:
                continue
            available = parse_meminfo_kb(report.get("memory", {})
                                         .get("MemAvailable"))
            if available is None:
                unknown.append({"side": role, "resource": "memory",
                                "reason": "MemAvailable not readable"})
                continue
            if available / 1024 < args.required_memory_mb:
                limits.append({
                    "side": role, "resource": "memory",
                    "measured": round(available / 1024, 1),
                    "required": args.required_memory_mb, "unit": "MiB",
                    "reason": f"{available / 1024:.0f} MiB available, "
                              f"{args.required_memory_mb} MiB requested"})

    # --- the CPUs the run is pinned to: a foreign load there is the run's
    #     limit, and it must be named before the rungs are believed
    # Only an explicitly stated set is checked: without it the preflight
    # would sample whatever CPUs the caller happened to allow, which on a
    # desktop is every CPU and every CPU is a little busy.
    benchmark_cpus = parse_cpu_list(args.benchmark_cpus)
    if benchmark_cpus:
        injected = None
        if args.benchmark_cpu_busy_json:
            injected = load(args.benchmark_cpu_busy_json) or {}
        if injected is not None:
            busy = injected.get("busy") or {}
            consumers = injected.get("consumers") or []
        else:
            # Two windows, and the second is the measurement: the first still
            # carries this process's own start-up, which would otherwise be
            # read as foreign work on whichever CPU the interpreter ran on.
            per_cpu_busy(1.0)
            busy = per_cpu_busy(1.0)
            consumers = foreign_cpu_consumers()
        busy_names = [f"cpu{cpu}" for cpu in sorted(benchmark_cpus)
                      if busy.get(f"cpu{cpu}", 0.0) > args.max_foreign_cpu_busy]
        details["benchmark_cpus"] = sorted(benchmark_cpus)
        details["benchmark_cpu_busy"] = {name: busy.get(name)
                                         for name in sorted(busy)
                                         if name in
                                         {f"cpu{c}" for c in benchmark_cpus}}
        if busy_names:
            details["foreign_cpu_consumers"] = consumers
            limits.append({
                "side": "host", "resource": "cpu",
                "measured": busy_names,
                "required": f"<= {args.max_foreign_cpu_busy:.2f} busy per "
                            f"benchmark CPU",
                "unit": "cpu",
                "reason": f"{', '.join(busy_names)} of the pinned set "
                          f"{details['benchmark_cpus']} "
                          + ("is" if len(busy_names) == 1 else "are")
                          + " busy with work that is not this run"
                          + (": " + "; ".join(
                              f"pid {item['pid']} {item['command']}"
                              for item in consumers) if consumers else "")
                          + ".  A capacity measured beside a foreign load is "
                            "the host's number, not the software's."})

    verdict = "infrastructure-limited" if limits else (
        "insufficient-evidence" if unknown else "ok")
    result = {
        "schema": "nginx-media.capacity-preflight/1",
        "request": details,
        "verdict": verdict,
        "limits": limits,
        "unknown": unknown,
        "network": network,
        "sender": sender,
        "receivers": receivers,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as output:
            json.dump(result, output, indent=1, default=str)
    print(f"preflight_verdict={verdict}")
    for limit in limits:
        print(f"preflight_limit={limit['side']}/{limit['resource']} "
              f"measured={limit['measured']}{limit['unit']} "
              f"required={limit['required']}{limit['unit']}")
    for item in unknown:
        print(f"preflight_unknown={item['side']}/{item['resource']}: "
              f"{item['reason']}")
    return 0 if verdict == "ok" else (2 if verdict == "infrastructure-limited"
                                      else 3)


def parse_meminfo_kb(value):
    if not value:
        return None
    parts = str(value).split()
    try:
        return float(parts[0])
    except (IndexError, ValueError):
        return None


def load(path):
    if not path:
        return {}
    with open(path, encoding="utf-8") as source:
        return json.load(source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    h = sub.add_parser("host")
    h.add_argument("--role", default="sender", choices=("sender", "receiver"))
    h.add_argument("--binary")
    h.add_argument("--pid", type=int)
    h.add_argument("--json", required=True)

    p = sub.add_parser("probe")
    p.add_argument("--listen", action="store_true")
    p.add_argument("--peer")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--packet-bytes", type=int, default=DEFAULT_PACKET_BYTES)
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--threads", type=int, default=1,
                   help="sockets and threads per side; one Python socket "
                        "cannot drain a multi-Gbit/s path")
    p.add_argument("--json", required=True)

    j = sub.add_parser("judge")
    j.add_argument("--destinations", type=int, required=True)
    j.add_argument("--bitrate-bps", type=float, required=True)
    j.add_argument("--overhead-factor", type=float,
                   default=DEFAULT_OVERHEAD_FACTOR)
    j.add_argument("--max-probe-loss", type=float, default=0.01)
    j.add_argument("--sender-cpu-per-gbps", type=float)
    j.add_argument("--receiver-cpu-per-gbps", type=float)
    j.add_argument("--sender-cpus",
                   help="the CPUs the sender is actually pinned to; without "
                        "it the fingerprint's own affinity is counted, which "
                        "is the harness's, not nginx's")
    j.add_argument("--receiver-cpus",
                   help="the CPUs the receivers are actually pinned to")
    j.add_argument("--required-memory-mb", type=float)
    j.add_argument("--benchmark-cpus",
                   help="the CPUs the run is pinned to (a list or a range "
                        "expression, as taskset takes); a busy CPU in this set "
                        "is a foreign load, not a capacity")
    j.add_argument("--benchmark-cpu-busy-json",
                   help="read the per-CPU busy sample (and, if present, its "
                        "\"consumers\") from this file instead of measuring "
                        "the live host: the classification is testable, the "
                        "sampling is not")
    j.add_argument("--max-foreign-cpu-busy", type=float, default=0.5,
                   help="fraction of one benchmark CPU that may be busy with "
                        "foreign work before the verdict is "
                        "infrastructure-limited (default 0.5: half a CPU's "
                        "worth, which separates a competing job from a "
                        "desktop's own spread)")
    j.add_argument("--sender")
    j.add_argument("--network")
    j.add_argument("--probe-sender",
                   help="the sender side of the path probe; its sent bytes "
                        "say whether the requested load was ever offered")
    j.add_argument("--receiver", action="append")
    j.add_argument("--json")
    args = parser.parse_args()

    if args.command == "host":
        report = host_report(args.role, args.binary, args.pid)
        with open(args.json, "w", encoding="utf-8") as output:
            json.dump(report, output, indent=1, default=str)
        print(f"preflight_host_role={args.role}")
        print(f"preflight_host_cpu={report['cpu_model']}")
        print(f"preflight_host_permitted_cpus={len(report['permitted_cpus'])}")
        print(f"preflight_host_json={args.json}")
        return 0
    if args.command == "probe":
        if args.listen == bool(args.peer):
            parser.error("probe needs exactly one of --listen or --peer")
        if args.threads < 1:
            parser.error("--threads must be at least 1")
        if args.listen:
            result = probe_listen(args.port, args.seconds, args.packet_bytes,
                                  args.bind, args.threads)
        else:
            result = probe_send(args.peer, args.port, args.seconds,
                                args.packet_bytes, args.threads)
        with open(args.json, "w", encoding="utf-8") as output:
            json.dump(result, output, indent=1, default=str)
        for key in ("received_bytes", "received_packets", "rate_gbps",
                    "sent_bytes", "sent_packets", "send_rate_gbps",
                    "rtt_ms_p50", "rtt_ms_p95", "ping_loss"):
            if key in result:
                print(f"preflight_probe_{key}={result[key]}")
        return 0
    return judge(args)


if __name__ == "__main__":
    sys.exit(main())

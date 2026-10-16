#!/usr/bin/env python3
"""Decide whether a host pair can carry an offered load, before measuring it.

  host   fingerprint one host (CPU, cgroup, NUMA, NICs, UDP limits, SRT library)
  probe  --listen: count arriving UDP and echo pings; --peer: ping, then flood
  judge  compare the measurements with the request: "ok",
         "infrastructure-limited" or "insufficient-evidence"

The network requirement is payload x 1.20 (SRT header 3.3% plus a 15%
retransmission allowance; `--overhead-factor`).  CPU needs a measured cost per
Gbit/s (`--sender-cpu-per-gbps`); without one it is unknown, never a pass.
"""

import argparse
import glob
import hashlib
import json
import math
import os
import platform
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proc_cpu_sampler  # noqa: E402
from proc_cpu_sampler import read_first_line  # noqa: E402

MAGIC = b"NMPREFL1"  # exactly the 8 bytes the header carries
PING = 0x01
FLOOD = 0x02
PING_REPLY = 0x03
HEADER = struct.Struct("!8sB3xQ")  # magic, kind, padding, sequence
DEFAULT_OVERHEAD_FACTOR = 1.20
DEFAULT_PACKET_BYTES = 1400
PING_COUNT = 20
PING_TIMEOUT = 0.5


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
    return {os.path.basename(node): read_first_line(os.path.join(node, "cpulist"))
            for node in sorted(glob.glob("/sys/devices/system/node/node[0-9]*"))}


def interface_facts():
    """Speed, driver, MTU and offloads per interface; virtual ones have no speed."""
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
    """`ethtool` view of a NIC; /sys features do not always match what it applies."""
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


def file_sha256(path):
    value = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            value.update(block)
    return value.hexdigest()


def bound_metadata(path, schema, hash_key="sha256"):
    """Ignore absent, malformed or stale sidecars, even at a pinned prefix."""
    try:
        with open(os.path.realpath(path) + ".provenance.json", encoding="utf-8") as source:
            evidence = json.load(source)
        expected = (evidence.get("artifact") or {}).get("sha256") \
            if hash_key == "artifact" else evidence.get(hash_key)
        if (evidence.get("schema") == schema
                and expected == file_sha256(path)):
            return evidence
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    return None


def shared_library_version(path):
    """Ask the resolved SRT library's API, in an isolated helper process."""
    program = (
        "import ctypes,sys; lib=ctypes.CDLL(sys.argv[1]); "
        "lib.srt_getversion.restype=ctypes.c_uint; "
        "v=lib.srt_getversion(); "
        "print(f'{v >> 16}.{(v >> 8) & 255}.{v & 255}')")
    try:
        out = subprocess.run([sys.executable, "-c", program, path],
                             capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    value = out.stdout.strip()
    return value if out.returncode == 0 and re.fullmatch(r"\d+\.\d+\.\d+", value) else None


def transport_provenance(binary):
    """Actual library identity; a configured ref is never source evidence."""
    if not binary or not os.path.isfile(binary):
        return None
    evidence = {"source_commit": None, "source_commit_status": "unknown",
                "library_version": None, "linkage": "unknown"}
    try:
        out = subprocess.run(["ldd", binary], capture_output=True, text=True,
                             timeout=10)
    except (OSError, subprocess.SubprocessError):
        return evidence
    for line in out.stdout.splitlines():
        if "srt" not in line or "=>" not in line:
            continue
        resolved = line.split("=>", 1)[1].strip().split()[0]
        evidence["linkage"] = "shared"
        if not os.path.isfile(resolved):
            evidence["source_commit_status"] = "unknown-unresolved-library"
            return evidence
        resolved = os.path.realpath(resolved)
        evidence["library_path"] = resolved
        try:
            evidence["library_sha256"] = file_sha256(resolved)
        except OSError:
            evidence["source_commit_status"] = "unknown-unreadable-library"
            return evidence
        evidence["library_version"] = shared_library_version(resolved)
        evidence["library_version_method"] = "srt_getversion" \
            if evidence["library_version"] else None
        metadata = bound_metadata(resolved, "nginx-media.transport-build/1", "artifact")
        if metadata:
            source = metadata.get("source")
            commit = source.get("commit") if isinstance(source, dict) else None
            if isinstance(commit, str) and re.fullmatch(r"[0-9a-fA-F]{40}", commit):
                evidence["source_commit"] = commit
                evidence["source_commit_status"] = "verified-artifact-metadata"
                evidence["build"] = {key: metadata[key] for key in
                                     ("schema", "source", "artifact", "package", "compiler", "flags",
                                      "cmake", "linkage") if key in metadata}
        return evidence
    # No dynamic SRT dependency is not proof of a static SRT checkout.
    evidence["source_commit_status"] = "unknown-no-resolved-srt-library"
    return evidence


def receiver_tool_versions():
    tools = {"python": {"path": sys.executable, "version": sys.version}}
    for name, flags in (("cc", ["--version"]), ("pkg-config", ["--version"]),
                        ("ffmpeg", ["-version"]), ("curl", ["--version"])):
        path = shutil.which(name)
        entry = {"path": path, "version": None, "status": "unavailable"}
        if path:
            try:
                out = subprocess.run([path] + flags, capture_output=True,
                                     text=True, timeout=10)
                if out.returncode == 0:
                    entry.update(version=out.stdout.strip(), status="observed",
                                 sha256=file_sha256(path))
            except (OSError, subprocess.SubprocessError):
                pass
        tools[name] = entry
    return tools


def transport_version(binary):
    """libsrt label, library version and resolved path: the soname alone cannot
    tell the distribution's libsrt.so.1.5 from the pinned build."""
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
            # libsrt.so and librobotweax-srt.so both contain "srt"
            if "srt" not in line or "=>" not in line:
                continue
            soname = line.split()[0].strip()
            resolved = line.split("=>")[-1].strip().split(" ")[0]
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


def nginx_build(binary):
    """Compiler, configure arguments/flags and content hash of the binary."""
    if not binary or not os.path.exists(binary):
        return None
    build = {}
    metadata = bound_metadata(binary, "nginx-media.binary-build/1")
    if metadata:
        build.update({key: metadata[key] for key in (
            "compiler", "compiler_command", "cflags", "ldflags", "compile_command",
            "source", "package") if key in metadata})
    if not (metadata and metadata.get("source")):
        try:
            out = subprocess.run([binary, "-V"], capture_output=True, text=True,
                                 timeout=10)
            for line in out.stderr.splitlines():
                if line.startswith("built by "):
                    build["compiler"] = line[len("built by "):].strip()
                elif line.startswith("configure arguments:"):
                    build["configure_arguments"] = line.split(":", 1)[1].strip()
        except (OSError, subprocess.SubprocessError):
            pass
    try:
        build["sha256"] = file_sha256(binary)
    except OSError:
        pass
    return build or None


def host_report(role, binary=None):
    busy_start = proc_cpu_sampler.proc_stat_cpu()
    time.sleep(0.5)
    busy_end = proc_cpu_sampler.proc_stat_cpu()
    report = {
        "role": role,
        "hostname": platform.node(),
        "architecture": platform.machine(),
        "boot_id": read_first_line("/proc/sys/kernel/random/boot_id"),
        "binary": os.path.realpath(binary) if binary else None,
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
        "build": nginx_build(binary),
        "transport_provenance": transport_provenance(binary),
    }
    if role == "receiver":
        report["tools"] = receiver_tool_versions()
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
    """UDP drops summed over every socket bound to the probe's port (one per
    SO_REUSEPORT thread): a receiver that cannot drain is not a slow path."""
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
    """Receive side; one SO_REUSEPORT socket per thread, since one Python
    socket cannot drain more than a few Gbit/s."""
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
            if not data.startswith(MAGIC) or len(data) != packet_bytes:
                continue
            if HEADER.unpack_from(data)[1] != FLOOD:
                continue
            now = time.monotonic_ns()
            if counter["first_ns"] is None:
                counter["first_ns"] = now
            counter["last_ns"] = now
            counter["largest"] = max(counter["largest"], len(data))
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
    """Send side: a ping sample for latency and loss, then a UDP flood.  The
    receiver's count is the measurement; this one says what was offered."""
    address = (peer, port)
    rtts = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ping_sock:
        for sequence in range(PING_COUNT):
            start = time.monotonic_ns()
            deadline = time.monotonic() + PING_TIMEOUT
            try:
                ping_sock.sendto(HEADER.pack(MAGIC, PING, sequence) + bytes(64), address)
                # Skip late replies to earlier pings: they would read as short RTTs.
                while (remaining := deadline - time.monotonic()) > 0:
                    ping_sock.settimeout(remaining)
                    data, _ = ping_sock.recvfrom(2048)
                    if (len(data) >= HEADER.size and HEADER.unpack_from(data)
                            == (MAGIC, PING_REPLY, sequence)):
                        rtts.append((time.monotonic_ns() - start) / 1e6)
                        break
            except OSError:
                continue
    if not rtts:
        # Our own listener never answered: wrong namespace, gatewayed/black-holed
        # route or no receiver.  Never flood a path we cannot see the far end of.
        raise SystemExit(f"refusing to probe: no ping reply from {peer}:{port} "
                         f"in {PING_COUNT} attempts")

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
    """Busy fraction per CPU over a short window, from /proc/stat."""
    before = proc_cpu_sampler.proc_stat_per_cpu()
    time.sleep(window_s)
    after = proc_cpu_sampler.proc_stat_per_cpu()
    return {name: value for name, first in before.items()
            if (value := busy_fraction(first, after.get(name) or {})) is not None}


def foreign_cpu_consumers(window_s=0.5, limit=5):
    """Processes outside the benchmark that used CPU in the window, biggest first."""
    own = ("capacity_harness", "capacity_preflight", "srt_fanout_sink",
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
    # Invalid measurements outrank hardware limits.
    errors = []
    for label, path in (("sender", args.sender), ("network", args.network),
                        ("probe-sender", args.probe_sender),
                        *((f"receiver-{i}", p) for i, p in enumerate(args.receiver or []))):
        if not path:
            continue
        try:
            document = load(path)
            if not isinstance(document, dict):
                raise ValueError("expected a JSON object")
            fields = (("received_bytes", "window_s") if label == "network" else
                      ("sent_bytes", "send_window_s") if label == "probe-sender" else ())
            for field in fields:
                value = document.get(field)
                if (isinstance(value, bool) or not isinstance(value, (int, float))
                        or not math.isfinite(value) or value < 0
                        or (field.endswith("_s") and value == 0)):
                    raise ValueError(f"invalid or missing {field}")
            if "socket_drops" in document and document["socket_drops"] is not None:
                drops = document["socket_drops"]
                if isinstance(drops, bool) or not isinstance(drops, int) or drops < 0:
                    raise ValueError("invalid socket_drops")
        except (OSError, ValueError) as error:
            errors.append(f"{label}: {error}")
    if errors:
        result = {"schema": "nginx-media.capacity-preflight/1",
                  "verdict": "setup-failure", "errors": errors,
                  "limits": [], "unknown": []}
        if args.json:
            with open(args.json, "w", encoding="utf-8") as output:
                json.dump(result, output, indent=1)
        print("preflight_verdict=setup-failure")
        for error in errors:
            print(f"preflight_error={error}")
        return 1
    sender = load(args.sender)
    network = load(args.network)
    if args.probe_sender:
        # The receiver's count is the measurement; the sender's says what was offered.
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
                # The probe's own receiver dropped: evidence about the probe,
                # not the path; the receiver side is decided by its CPU check.
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
                # Never offered: a limit of the probe, not of the path.
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
        # A loss limit needs both: the probe offered at least the requirement,
        # and what arrived fell short of it (overshoot loss is not a limit).
        delivered_bps = offered_bps * (1.0 - loss) if offered_bps else None
        if delivered_bps is not None:
            details["probe_delivered_gbps"] = round(delivered_bps / 1e9, 4)
        if (loss > args.max_probe_loss and offered_bps
                and offered_bps >= required_bps
                and delivered_bps < required_bps):
            limits.append({
                "side": "network", "resource": "loss",
                "measured": round(loss, 5), "required": args.max_probe_loss,
                "unit": "fraction",
                "reason": f"{loss:.2%} of the probe's datagrams never arrived, "
                          f"leaving {delivered_bps / 1e9:.2f} Gbit/s against "
                          f"the {required_bps / 1e9:.2f} Gbit/s this request "
                          f"needs (offered {offered_bps / 1e9:.2f} Gbit/s)"})

    # --- the sender and receiver CPUs, against a measured cost per Gbit/s
    for role, report, cost, stated in (
            ("sender", sender, args.sender_cpu_per_gbps, args.sender_cpus),
            ("receiver", receivers[0] if receivers else None,
             args.receiver_cpu_per_gbps, args.receiver_cpus)):
        if not report:
            if cost is not None:
                # A cost with no fingerprint to spend it on is not a verdict.
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
            if role == "sender" and args.sender_under_test:
                # Recorded, not judged: the sender's CPU need is what a
                # capacity ladder measures.
                details["sender_cpu_exceeds_permitted_cpus"] = True
                continue
            limits.append({
                "side": role, "resource": "cpu",
                "measured": permitted, "required": round(needed, 2),
                "unit": "cores",
                "reason": f"{payload_bps / 1e9:.2f} Gbit/s at {cost:.0f}% of a "
                          f"core per Gbit/s needs {needed:.1f} cores and this "
                          f"host permits {permitted}"})

    # --- memory: only an explicit bound is checked, never a guess
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

    # --- foreign load on the explicitly pinned CPUs (every desktop CPU is a
    #     little busy, so only a stated set is checked)
    benchmark_cpus = parse_cpu_list(args.benchmark_cpus)
    if benchmark_cpus:
        if args.benchmark_cpu_busy_json:
            injected = load(args.benchmark_cpu_busy_json) or {}
            busy = injected.get("busy") or {}
            consumers = injected.get("consumers") or []
        else:
            # The first window still carries this interpreter's own start-up.
            per_cpu_busy(1.0)
            busy = per_cpu_busy(1.0)
            consumers = foreign_cpu_consumers()
        unsampled = [f"cpu{cpu}" for cpu in sorted(benchmark_cpus)
                     if busy.get(f"cpu{cpu}") is None]
        if unsampled:
            # A pinned CPU with no sample is unknown, not idle.
            unknown.append({"side": "host", "resource": "cpu",
                            "reason": f"no busy sample for pinned {', '.join(unsampled)}"})
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
    h.add_argument("--json", required=True)

    p = sub.add_parser("probe")
    p.add_argument("--listen", action="store_true")
    p.add_argument("--peer")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--packet-bytes", type=int, default=DEFAULT_PACKET_BYTES)
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--threads", type=int, default=1,
                   help="sockets and threads per side")
    p.add_argument("--json", required=True)

    j = sub.add_parser("judge")
    j.add_argument("--destinations", type=int, required=True)
    j.add_argument("--bitrate-bps", type=float, required=True)
    j.add_argument("--overhead-factor", type=float,
                   default=DEFAULT_OVERHEAD_FACTOR)
    j.add_argument("--max-probe-loss", type=float, default=0.01)
    j.add_argument("--sender-under-test", action="store_true",
                   help="record the sender's CPU need, never judge it a host limit")
    j.add_argument("--sender-cpu-per-gbps", type=float)
    j.add_argument("--receiver-cpu-per-gbps", type=float)
    j.add_argument("--sender-cpus",
                   help="CPUs the sender is pinned to (default: the fingerprint's affinity)")
    j.add_argument("--receiver-cpus",
                   help="CPUs the receivers are pinned to")
    j.add_argument("--required-memory-mb", type=float)
    j.add_argument("--benchmark-cpus",
                   help="CPUs the run is pinned to (taskset list); a busy one is a foreign load")
    j.add_argument("--benchmark-cpu-busy-json",
                   help="read the per-CPU busy sample and \"consumers\" from this file "
                        "instead of the live host (tests)")
    j.add_argument("--max-foreign-cpu-busy", type=float, default=0.5,
                   help="busy fraction of one pinned CPU that counts as a foreign load")
    j.add_argument("--sender")
    j.add_argument("--network")
    j.add_argument("--probe-sender",
                   help="the probe's sender side: whether the load was ever offered")
    j.add_argument("--receiver", action="append")
    j.add_argument("--json")
    args = parser.parse_args()

    if args.command == "host":
        if args.binary and not os.path.isfile(args.binary):
            parser.error("--binary must name the actual existing binary")
        report = host_report(args.role, args.binary)
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

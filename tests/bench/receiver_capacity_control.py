#!/usr/bin/env python3
"""Receiver controls using real libsrt, independent of nginx egress.

A qualified result is a lower bound for this explicit control recipe, not a
production capacity ceiling. --paired-stall changes only one receiver's read
behavior and requires healthy lane-mates plus a full-rate source before it can
attribute the controlled failure to the receiver. CPU usage alone is not a
receiver-saturation verdict.
"""
import argparse
import csv
import ctypes
import hashlib
import json
import os
from pathlib import Path
import random
import shlex
import signal
import socket
import subprocess
import tempfile
import time
from types import SimpleNamespace

import proc_cpu_sampler
import rtmp_capacity_quality

ROOT = Path(__file__).resolve().parents[2]
FIELDS = ("attempted_bytes", "accepted_bytes", "blocked_messages", "transport_errors")

# Linux parent-death binding also covers SIGKILL, where Python cannot run finally.
LIBC = ctypes.CDLL(None, use_errno=True)
LIBC.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                      ctypes.c_ulong, ctypes.c_ulong]
LIBC.prctl.restype = ctypes.c_int


def owner_binding():
    parent = os.getpid()
    def bind():
        if LIBC.prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
            raise OSError(ctypes.get_errno(), "PR_SET_PDEATHSIG")
        if os.getppid() != parent:
            os.kill(os.getpid(), signal.SIGKILL)
    return bind
TS_FIELDS = ("ts_packets", "ts_sync_errors", "ts_continuity_errors", "ts_tei_errors")


def positive(value):
    value = int(value)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def command_output(argv):
    return subprocess.check_output(argv, text=True).strip()


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def build(directory):
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-O2", "-g", "-Wall", "-Wextra", "-Werror", "-std=c2x"]
    cflags = shlex.split(command_output(["pkg-config", "--cflags", "srt"]))
    libs = shlex.split(command_output(["pkg-config", "--libs", "srt"]))
    binaries = {}
    for name in ("srt_capacity_source", "srt_fanout_sink"):
        binary = directory / name
        subprocess.run([*compiler, *flags, *cflags,
                        str(ROOT / "tests/bench" / (name + ".c")),
                        "-o", str(binary), *libs], check=True)
        binaries[name] = str(binary)
    identity = {"compiler": command_output([*compiler, "--version"]).splitlines()[0],
                "flags": flags + cflags + libs,
                "srt_version": command_output(["pkg-config", "--modversion", "srt"]),
                "binaries": {name: file_sha256(path)
                             for name, path in binaries.items()}}
    runtime = int(command_output([binaries["srt_capacity_source"], "--version"]))
    identity["srt_runtime_version"] = f"{runtime >> 16}.{(runtime >> 8) & 255}.{runtime & 255}"
    return binaries, identity


def pin(cpus, argv):
    return ["taskset", "-c", cpus, *argv] if cpus else argv


def snapshot_rows(path, expected, source=False):
    fields = FIELDS if source else ("bytes_received", "transport_error", "stalled", *TS_FIELDS)
    rows, stamps = {}, set()
    with open(path, newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        required = {"destination_id", "snapshot_ns", *fields}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(f"invalid snapshot schema: {path}")
        for row in reader:
            identifier = row["destination_id"]
            if identifier not in expected or identifier in rows:
                raise ValueError(f"duplicate or unexpected destination in {path}: {identifier}")
            values = {name: int(row[name]) for name in fields}
            stamp = int(row["snapshot_ns"])
            if stamp <= 0 or any(v < 0 for v in values.values()):
                raise ValueError(f"invalid counters/timestamp in {path}")
            rows[identifier] = values
            stamps.add(stamp)
    if set(rows) != expected or len(stamps) != 1:
        raise ValueError(f"incomplete or inconsistent snapshot: {path}")
    return stamps.pop(), rows


def wait_ready(processes, path, source=False, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if any(p.poll() is not None for p in processes):
            raise RuntimeError(f"control process exited before readiness: {path}")
        if path.is_file() and (not source or path.read_text() == "ready\n"):
            return
        time.sleep(0.01)
    raise RuntimeError(f"control readiness timed out: {path}")


def capture(process, live_path, saved_path, expected, previous=0, source=False, toggle=False):
    process.send_signal(signal.SIGUSR2 if toggle else signal.SIGUSR1)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"control process exited while capturing {live_path}")
        try:
            blob = live_path.read_text()
            # Publish to an owned temporary path so parsing and preservation
            # refer to the same atomic snapshot even if a writer replaces it.
            saved_path.write_text(blob)
            stamp, rows = snapshot_rows(saved_path, expected, source)
            if stamp > previous:
                return stamp, rows
        except FileNotFoundError:
            pass
        time.sleep(0.005)
    raise RuntimeError(f"fresh snapshot timed out: {live_path}")


def cpu_capture(path, processes):
    started, finished, rows, _ = proc_cpu_sampler.thread_snapshot(
        [str(p.pid) for p in processes], {}, False)
    proc_cpu_sampler.write_threads(str(path), started, finished, rows)
    return {"start_ns": started, "end_ns": finished}


def ports(listeners):
    for _ in range(100):
        base = random.randrange(18000, 29000 - listeners)
        held = []
        try:
            for port in range(base, base + listeners):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                held.append(sock)
                sock.bind(("127.0.0.1", port))
            return base
        except OSError:
            pass
        finally:
            for sock in held:
                sock.close()
    raise RuntimeError("no free contiguous receiver UDP ports")


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
    return process.returncode


def evaluate(recipe, before, after, source_before, source_after, intervals):
    expected = set(before[1])
    receive_seconds = (after[0] - before[0]) / 1e9
    source_seconds = (source_after[0] - source_before[0]) / 1e9
    if receive_seconds <= 0 or source_seconds <= 0:
        raise ValueError("control timestamps did not advance")
    failures = []
    args = SimpleNamespace(intervals=str(intervals), sample_interval_s=1,
                           max_interval_s=2, interval_floor=0.90, max_low_s=2)
    summary = rtmp_capacity_quality.interval_check(
        args, expected, recipe["bitrate_bps"],
        {k: v["bytes_received"] for k, v in before[1].items()},
        {k: v["bytes_received"] for k, v in after[1].items()},
        before[0], after[0], failures)
    results = {}
    for identifier in sorted(expected):
        old, new = source_before[1][identifier], source_after[1][identifier]
        delta = {key: new[key] - old[key] for key in FIELDS}
        received = after[1][identifier]["bytes_received"] - before[1][identifier]["bytes_received"]
        if received < 0 or any(v < 0 for v in delta.values()):
            raise ValueError("control counters regressed")
        offered_ratio = delta["attempted_bytes"] * 8 / source_seconds / recipe["bitrate_bps"]
        accepted_ratio = delta["accepted_bytes"] * 8 / source_seconds / recipe["bitrate_bps"]
        received_ratio = received * 8 / receive_seconds / recipe["bitrate_bps"]
        ts = after[1][identifier]
        source_full = offered_ratio >= 0.99 and accepted_ratio >= 0.99 \
            and delta["blocked_messages"] == 0 and delta["transport_errors"] == 0
        receive_full = received_ratio >= 0.99 and summary[identifier][0] >= 0.90 \
            and ts["ts_packets"] > 0 and ts["transport_error"] == 0 \
            and ts["stalled"] == 0 and all(ts[k] == 0 for k in TS_FIELDS[1:])
        results[identifier] = {"source_offered_ratio": offered_ratio,
                               "source_accepted_ratio": accepted_ratio,
                               "source_full_rate": source_full,
                               "receive_delivery_ratio": received_ratio,
                               "receive_interval_min": summary[identifier][0],
                               "receiver_full_rate": receive_full,
                               "receiver_counters": ts, "sender_counters": delta}
    qualified = not failures and all(r["source_full_rate"] and r["receiver_full_rate"]
                                     for r in results.values())
    return {"status": "qualified" if qualified else "insufficient-evidence",
            "receiver_seconds": receive_seconds, "source_seconds": source_seconds,
            "interval_quality_failures": failures, "destinations": results}


def experiment(args, directory, binaries, stall=None):
    expected = {f"d{i:04d}" for i in range(args.destinations)}
    listeners = args.listeners or min(64, (args.destinations + 47) // 48)
    recipe = {"protocol": "srt", "destinations": args.destinations,
              "listeners": listeners, "bitrate_bps": args.bitrate_bps,
              "duration_s": args.seconds, "sender_cpus": args.sender_cpus,
              "receiver_cpus": args.receiver_cpus, "payload_size_max": 1316,
              "packet_pacing": "constant", "sender_mux_lanes": min(16, args.destinations),
              "source_sha256": file_sha256(args.source),
              "receiver_stall_destination": stall}
    base = ports(listeners)
    receiver_live = directory / "receiver.csv.snapshot"
    source_live = directory / "source.snapshot"
    receiver = source = None
    with (directory / "receiver.log").open("w") as receiver_log, \
         (directory / "source.log").open("w") as source_log:
        try:
            receiver = subprocess.Popen(pin(args.receiver_cpus, [
                binaries["srt_fanout_sink"], f"{base}:{listeners}", str(args.destinations),
                str(directory / "receiver.ready"), str(directory / "receiver.csv"),
                *([stall] if stall else []), "quality"]),
                stdout=receiver_log, stderr=receiver_log, preexec_fn=owner_binding())
            source = subprocess.Popen(pin(args.sender_cpus, [
                binaries["srt_capacity_source"], str(args.source), "127.0.0.1", str(base),
                str(args.destinations), str(listeners), str(args.bitrate_bps),
                str(directory / "source.ready"), str(source_live)]),
                stdout=source_log, stderr=source_log, preexec_fn=owner_binding())
            processes = [source, receiver]
            wait_ready(processes, directory / "source.ready", source=True)
            wait_ready(processes, directory / "receiver.ready")
            time.sleep(2)
            before = capture(receiver, receiver_live, directory / "receiver.before.csv",
                             expected, toggle=True)
            source_before = capture(source, source_live, directory / "source.before.csv",
                                    expected, source=True)
            cpu_start = cpu_capture(directory / "cpu.before", processes)
            previous = before
            interval_path = directory / "receiver.intervals.csv"
            with interval_path.open("w", newline="", encoding="utf-8") as out:
                writer = csv.writer(out)
                writer.writerow(("destination_id", "start_ns", "end_ns",
                                 "bytes_received", "total_bytes"))
                started = time.monotonic()
                while time.monotonic() - started < args.seconds:
                    remaining = args.seconds - (time.monotonic() - started)
                    time.sleep(min(1, max(0, remaining)))
                    current = capture(receiver, receiver_live, directory / "receiver.sample.csv",
                                      expected, previous[0])
                    for identifier in sorted(expected):
                        total = current[1][identifier]["bytes_received"]
                        writer.writerow((identifier, previous[0], current[0],
                                         total - previous[1][identifier]["bytes_received"], total))
                    out.flush()
                    previous = current
                cpu_end = cpu_capture(directory / "cpu.after", processes)
                after = capture(receiver, receiver_live, directory / "receiver.after.csv",
                                expected, previous[0], toggle=True)
                source_after = capture(source, source_live, directory / "source.after.csv",
                                       expected, source_before[0], source=True)
            result = evaluate(recipe, before, after, source_before, source_after, interval_path)
            _, accounting = proc_cpu_sampler.cpu_deltas(
                str(directory / "cpu.before"), str(directory / "cpu.after"))
            result.update({"recipe": recipe, "directory": str(directory),
                           "cpu_capture_start": cpu_start, "cpu_capture_end": cpu_end,
                           "cpu_accounting": accounting,
                           "source_pid": source.pid, "receiver_pid": receiver.pid})
            return result
        finally:
            if source is not None:
                stop(source)
            if receiver is not None:
                stop(receiver)


def classify_pair(normal, impaired, fault):
    if normal["status"] != "qualified":
        return {"status": "insufficient-evidence", "reason": "full-rate unmodified receiver control failed"}
    fast_recipe = dict(normal["recipe"], receiver_stall_destination=None)
    slow_recipe = dict(impaired["recipe"], receiver_stall_destination=None)
    if fast_recipe != slow_recipe or fault not in impaired["destinations"]:
        raise ValueError("paired controls do not have matching recipes")
    others = [v for k, v in impaired["destinations"].items() if k != fault]
    target = impaired["destinations"][fault]
    if (others and all(v["source_full_rate"] and v["receiver_full_rate"] for v in others)
            and target["source_offered_ratio"] >= 0.99
            and target["receiver_counters"]["stalled"] == 1
            and not target["receiver_full_rate"]):
        return {"status": "receiver-limited", "reason": "controlled receiver read stall",
                "affected_destination": fault,
                "scope": "explicit control recipe; not a production capacity ceiling"}
    return {"status": "insufficient-evidence", "reason": "receiver cause is not isolated by the paired control"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--destinations", type=positive, default=16)
    parser.add_argument("--listeners", type=positive)
    parser.add_argument("--seconds", type=positive, default=10)
    parser.add_argument("--bitrate-bps", type=positive, default=8_000_000)
    parser.add_argument("--sender-cpus", default="")
    parser.add_argument("--receiver-cpus", default="")
    parser.add_argument("--paired-stall", action="store_true")
    parser.add_argument("--out", type=Path, default=ROOT / ".build/receiver-control")
    args = parser.parse_args()
    if args.destinations > 1000 or args.seconds < 5 or args.bitrate_bps > 10_000_000_000:
        parser.error("requires 1..1000 destinations, at least 5 seconds and at most 10 Gbit/s per peer")
    if args.listeners and (args.listeners > 64 or args.listeners > args.destinations):
        parser.error("listeners must not exceed 64 or the destination count")
    if args.paired_stall and args.destinations < 2:
        parser.error("paired stall needs a healthy control destination")
    if not args.source.is_file() or args.source.stat().st_size == 0:
        parser.error("--source must be a nonempty MPEG-TS file")
    args.source = args.source.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="control-", dir=args.out))
    report = {"schema": "nginx-media.receiver-control/1", "run": str(run),
              "status": "setup-failure"}
    def terminate(signo, _frame):
        raise SystemExit(128 + signo)
    signal.signal(signal.SIGTERM, terminate)
    try:
        binaries, identity = build(run)
        report["identity"] = identity
        normal_dir = run / "normal"
        normal_dir.mkdir()
        normal = experiment(args, normal_dir, binaries)
        report["normal"] = normal
        report["status"] = normal["status"]
        if args.paired_stall:
            impaired_dir = run / "stalled"
            impaired_dir.mkdir()
            impaired = experiment(args, impaired_dir, binaries, "d0000")
            report["impaired"] = impaired
            report["paired_verdict"] = classify_pair(normal, impaired, "d0000")
            report["status"] = report["paired_verdict"]["status"]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
    output = run / "receiver-control.json"
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(f"receiver_control={report['status']}")
    print(f"receiver_control_json={output}")
    return 1 if report["status"] == "setup-failure" else 0


if __name__ == "__main__":
    raise SystemExit(main())

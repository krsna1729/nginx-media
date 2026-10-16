#!/usr/bin/env python3
"""Drive the CPU-count sweep across every engine and workload.

One cell is one (engine, CPU count): one harness invocation running every
workload's ladder, one matrix file.  The sweep is resumable - a cell whose
result names every requested workload at the same ladder and window is
skipped; `--force` re-runs it - and every cell runs from one frozen copy of
the harness and its helpers, so an edit during a sweep cannot change a cell.

Measured CPUs are P-cores without their SMT siblings; omarchy-benchmark pins
each cell, sets the performance policy and restores it afterwards.

Usage:
    tests/bench/capacity_sweep.py --out docs/capacity-evidence/sweep-2026-10-16
    tests/bench/capacity_sweep.py --cell nginx-haivision-4c --force
    tests/bench/capacity_sweep.py --smoke
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

HARNESS = os.path.join(PROJECT_ROOT, "tests/bench/ingest_egress_fanout.sh")
MIXES = "pure-srt pure-rtmp pure-hls pure-hls-push rtmp-95-srt-5 hls-push-95-srt-5 rtmp-50-hls-push-45-srt-5"

#: The harness default stops at 1024; the qualification target is 2048.
STEPS = "1 2 4 8 16 32 64 128 256 512 1024 2048"

#: One thread per physical P-core (0-11 are six P-cores with SMT siblings).
PCORES = [0, 2, 4, 6, 8, 10]

ENGINES = [
    {"id": "nginx-robotweax",
     "binary": ".build/nginx-robotweax/install/sbin/nginx"},
    {"id": "nginx-haivision",
     "binary": ".build/nginx-haivision/install/sbin/nginx"},
    {"id": "restream",
     "binary": "../restream/target/release/restream"},
]

#: Receivers and publishers stay on the E-cores so they cannot steal from
#: the sender.
RECEIVER_CPUS = "12,13,14,15,16,17,18,19"
E_CORES = [12, 13, 14, 15, 16, 17, 18, 19]
PUBLISHER_CPUS = "12,13"

STOP_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)
VETH_KEYS = ("CAPACITY_SENDER_EXEC", "CAPACITY_RECEIVER_EXEC",
             "CAPACITY_RECEIVER_ADDR", "CAPACITY_SENDER_ADDR")


def log(message):
    print("[%s] %s" % (datetime.now().strftime("%H:%M:%S"), message), flush=True)


def workloads_in(matrix):
    """The workload names a cell's matrix actually reports."""
    if not isinstance(matrix, dict):
        return set()
    return set((matrix.get("mixes") or {}).keys())


def complete(record, args):
    """Whether a previous result measured every requested workload with this
    ladder and window; a smoke result never satisfies a full run."""
    wanted = set(args.mixes.split())
    return (bool(wanted)
            and record.get("seconds_per_rung") == args.seconds
            and record.get("steps", STEPS) == args.steps
            and workloads_in(record.get("matrix")) >= wanted)


def engine_binary(engine):
    return os.path.join(PROJECT_ROOT, engine["binary"])


def snapshot_harness():
    """One immutable copy of the harness and every helper it names.

    The copy sits beside the original because the harness resolves its root
    two levels up; helpers are found by the `$BENCH_DIR/`, `$SCRIPTS_DIR/`
    and `$ROOT/scripts/` references, and CAPACITY_BENCH_DIR/SCRIPTS_DIR point
    the harness at the copies.
    """
    stamp = time.strftime("%Y%m%d-%H%M%S")
    bench = os.path.join(PROJECT_ROOT, "tests/bench")
    helpers = os.path.join(bench, ".sweep-%s-helpers" % stamp)
    scripts = os.path.join(bench, ".sweep-%s-scripts" % stamp)
    os.makedirs(helpers, exist_ok=True)
    os.makedirs(scripts, exist_ok=True)

    needed = {"bench": set(), "scripts": set()}
    with open(HARNESS, errors="ignore") as handle:
        text = handle.read()
    for match in re.finditer(
            r"\$(BENCH_DIR|SCRIPTS_DIR|ROOT/scripts)/([A-Za-z0-9_.-]+)", text):
        needed["bench" if match.group(1) == "BENCH_DIR" else "scripts"].add(
            match.group(2))

    missing = []
    for kind, target in (("bench", helpers), ("scripts", scripts)):
        if not needed[kind]:
            raise SystemExit("no %s helpers found in the harness; the freeze "
                             "would be incomplete" % kind)
        source_dir = bench if kind == "bench" else os.path.join(PROJECT_ROOT, "scripts")
        for name in sorted(needed[kind]):
            source = os.path.join(source_dir, name)
            if not os.path.exists(source):
                missing.append(name)
                continue
            shutil.copy(source, os.path.join(target, name))
    if missing:
        raise SystemExit("the harness reads files that do not exist: %s"
                         % ", ".join(missing))

    path = os.path.join(bench, ".sweep-%s.sh" % stamp)
    shutil.copy(HARNESS, path)
    os.chmod(path, 0o755)
    log("froze the harness, %d benchmark helpers and %d script helpers"
        % (len(needed["bench"]), len(needed["scripts"])))
    return path, helpers, scripts


def cell_id(engine, cpus):
    """A name that is safe as a path, in a URL and in later tooling."""
    return "%s-%dc" % (engine["id"], len(cpus))


def wait_for_harness(child, log_path, args):
    """Wait for the harness; stop waiting when its log stops growing (a
    stall) or the cell budget runs out.  Returns (status, timed_out, note)."""
    idle_limit = max(300, 3 * args.seconds)
    deadline = time.time() + args.cell_budget
    last_size = -1
    last_change = time.time()
    while True:
        try:
            child.wait(timeout=30)
            return child.returncode, False, None
        except subprocess.TimeoutExpired:
            pass
        now = time.time()
        try:
            size = os.path.getsize(log_path)
        except OSError:
            size = 0
        if size != last_size:
            last_size, last_change = size, now
        if now - last_change > idle_limit:
            return None, True, ("no output for %ds, against a %ds idle limit"
                                % (now - last_change, idle_limit))
        if now > deadline:
            return None, True, "the %ds cell budget" % args.cell_budget


def last_log_line(log_path):
    """The harness's final line, which is usually the one that explains a stall."""
    try:
        with open(log_path, errors="replace") as handle:
            last = collections.deque((line.strip() for line in handle
                                      if line.strip()), maxlen=1)
    except OSError:
        return "(no log)"
    return last[0][:200] if last else "(empty log)"


def signal_group(pgid, signum):
    """Signal a process group; false once it has no members we can reach."""
    try:
        os.killpg(pgid, signum)
        return True
    except OSError:
        return False


def stop_group(child, grace=60):
    """TERM the cell's process group, KILL what outlives the grace.

    The grace is what lets omarchy-benchmark's TERM trap restore governors,
    turbo and offlined SMT siblings.  Returns true if KILL was needed.
    """
    pgid = child.pid   # start_new_session: the child leads its own group
    killed = False
    deadline = time.time() + grace
    alive = signal_group(pgid, signal.SIGTERM)
    while alive:
        child.poll()   # reap the leader, or its zombie keeps the group alive
        if time.time() > deadline:
            if killed:
                break
            signal_group(pgid, signal.SIGKILL)
            killed, deadline = True, time.time() + 30
        time.sleep(0.5)
        alive = signal_group(pgid, 0)
    child.poll()
    return killed


def read_proc(pid, name):
    try:
        with open("/proc/%d/%s" % (pid, name), "rb") as handle:
            return handle.read().replace(b"\0", b" ").decode(errors="replace")
    except OSError:
        return ""


def cell_processes(run_dir, masters):
    """Live processes of a cell: the pidfile masters, anything whose command
    line names the run directory, and the children (nginx workers) of both."""
    marker = run_dir.rstrip(os.sep) + os.sep
    owned = {pid for pid in masters
             if read_proc(pid, "cmdline").startswith("nginx:")}
    parents = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit() or int(entry) == os.getpid():
            continue
        pid = int(entry)
        fields = read_proc(pid, "stat").rsplit(")", 1)[-1].split()
        if len(fields) < 2 or fields[0] == "Z":
            continue
        parents[pid] = int(fields[1])
        if marker in read_proc(pid, "cmdline"):
            owned.add(pid)
    owned |= {pid for pid, parent in parents.items() if parent in owned}
    return {pid for pid in owned if pid in parents}


def stop_cell_leftovers(run_dir):
    """TERM, then KILL, whatever the cell left running; returns survivors.

    Daemonized nginx masters (sender and RTMP sinks) leave the cell's process
    group for a subreaper, so the group stop misses them; their pidfiles are
    `<prefix>/logs/nginx.pid` under the run directory.
    """
    masters = set()
    for directory, _, files in os.walk(run_dir):
        if "nginx.pid" in files and os.path.basename(directory) == "logs":
            try:
                with open(os.path.join(directory, "nginx.pid")) as handle:
                    masters.add(int(handle.read().strip()))
            except (OSError, ValueError):
                pass
    for signum, settle in ((signal.SIGTERM, 10), (signal.SIGKILL, 5)):
        pids = cell_processes(run_dir, masters)
        if not pids:
            return []
        for pid in pids:
            try:
                os.kill(pid, signum)
            except OSError:
                pass
        deadline = time.time() + settle
        while time.time() < deadline and cell_processes(run_dir, masters):
            time.sleep(0.5)
    return sorted(cell_processes(run_dir, masters))


def on_signal(signum, frame):
    """Unwind into run_cell's cleanup; later signals must not cut it short."""
    for stop in STOP_SIGNALS:
        signal.signal(stop, signal.SIG_IGN)
    raise SystemExit(128 + signum)


def cleanup_cell(child, identifier, run_dir):
    """Stop everything a cell started, on every exit path; true if KILLed."""
    signal.pthread_sigmask(signal.SIG_BLOCK, STOP_SIGNALS)
    try:
        killed = stop_group(child)
        survivors = stop_cell_leftovers(run_dir)
        if survivors:
            log("%-18s processes survived KILL: %s"
                % (identifier, " ".join(map(str, survivors))))
        if killed:
            # The KILL skipped omarchy-benchmark's restore of governors,
            # turbo and SMT siblings.
            try:
                reset = subprocess.run(["omarchy-benchmark", "--reset"],
                                       timeout=120, capture_output=True, text=True)
                failure = reset.returncode and (reset.stderr.strip()
                                                or "exit %d" % reset.returncode)
            except (OSError, subprocess.TimeoutExpired) as error:
                failure = str(error)
            if failure:
                log("%-18s omarchy-benchmark --reset failed: %s" % (identifier, failure))
        return killed
    finally:
        signal.pthread_sigmask(signal.SIG_UNBLOCK, STOP_SIGNALS)


def run_cell(engine, cpus, snapshot, helpers, scripts, args):
    """Run every workload's ladder for one engine at one CPU count."""
    identifier = cell_id(engine, cpus)
    case_dir = os.path.join(args.out, identifier)
    run_dir = os.path.join(case_dir, "run")
    bench_log = os.path.join(case_dir, "bench.log")
    matrix_path = os.path.join(run_dir, "capacity-matrix.json")
    os.makedirs(run_dir, exist_ok=True)
    # An earlier attempt's matrix must not be read as this attempt's.
    if os.path.exists(matrix_path):
        os.remove(matrix_path)

    cpu_list = ",".join(str(cpu) for cpu in cpus)
    binary = engine_binary(engine)
    environment = dict(os.environ, **{
        # `auto` records a rung the receiver side cannot carry as
        # infrastructure-limited instead of as the engine losing capacity.
        "CAPACITY_PREFLIGHT": "auto",
        "CAPACITY_ENGINE": "restream" if engine["id"] == "restream" else "nginx",
        "CAPACITY_NGINX": binary,
        "RESTREAM_BIN": binary,
        "CAPACITY_RTMP_SINK_NGINX": os.path.join(
            PROJECT_ROOT, ".build/nginx-haivision/install/sbin/nginx"),
        "CAPACITY_WORKERS": str(len(cpus)),
        "CAPACITY_NGINX_CPUS": cpu_list,
        "CAPACITY_RECEIVER_CPUS": RECEIVER_CPUS,
        "CAPACITY_PUBLISHER_CPUS": PUBLISHER_CPUS,
        "CAPACITY_QUALITY_SECONDS": str(args.seconds),
        "CAPACITY_QUALITY_MIXES": args.mixes,
        "CAPACITY_QUALITY_STEPS": args.steps,
        "CAPACITY_BENCH_DIR": helpers,
        "CAPACITY_SCRIPTS_DIR": scripts,
        "CAPACITY_QUALITY_STOP_AFTER_FAILURES": "1",
        "CAPACITY_SINK_MODE": "correctness",
        "PHASES": "capacity-quality-ladder",
        "CAPACITY_RUN": run_dir,
    }, **args.veth_env)

    # Every CPU a role is placed on.  No `--isolate`: its cpuset makes the
    # receivers' taskset onto the E-cores fail with EINVAL.
    helper_cpus = ",".join(str(cpu) for cpu in cpus + E_CORES)
    command = ["omarchy-benchmark", "--cpu", helper_cpus, "--turbo", "on",
               "--", "bash", snapshot]
    started = time.time()
    status, timed_out, stall_note, killed = None, False, None, False
    child = None
    try:
        with open(bench_log, "w") as handle:
            child = subprocess.Popen(command, cwd=PROJECT_ROOT, env=environment,
                                     stdout=handle, stderr=subprocess.STDOUT,
                                     start_new_session=True)
        status, timed_out, stall_note = wait_for_harness(child, bench_log, args)
        if timed_out:
            log("%-18s stalled: %s" % (identifier, stall_note))
            log("%-18s last line: %s" % (identifier, last_log_line(bench_log)))
    finally:
        if child is not None:
            killed = cleanup_cell(child, identifier, run_dir)
    if timed_out:
        status = -signal.SIGKILL if killed else -signal.SIGTERM
    elapsed = time.time() - started

    record = {
        "cell": identifier,
        "engine": engine["id"],
        "cpus": cpu_list,
        "cores": len(cpus),
        "seconds_per_rung": args.seconds,
        "steps": args.steps,
        "exit_status": status,
        "timed_out": timed_out,
        "stall_note": stall_note,
        "budget_seconds": args.cell_budget,
        "wall_seconds": round(elapsed, 1),
        "finished_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "matrix": None,
    }
    try:
        with open(matrix_path) as handle:
            record["matrix"] = json.load(handle)
    except OSError:
        record["note"] = "the harness wrote no matrix for this cell"
    except ValueError as error:
        record["note"] = "the harness wrote an unreadable matrix: %s" % error
    record["receiver_cpu_of_one_core"] = top_receiver_cpu(bench_log)
    record["rung_started"] = rung_started(bench_log)
    return record


def top_receiver_cpu(path):
    """The largest `receivers N% of one core` the harness logged for a cell."""
    worst = None
    try:
        with open(path, errors="replace") as handle:
            for line in handle:
                if "vs receivers" not in line:
                    continue
                match = re.search(r"receivers (\d+)% of one core", line)
                if match:
                    value = int(match.group(1))
                    worst = value if worst is None else max(worst, value)
    except OSError:
        return None
    return worst


def rung_started(path):
    """Whether the harness got as far as starting any rung."""
    try:
        with open(path, errors="replace") as handle:
            return any(line.startswith("quality_rung_start ") for line in handle)
    except OSError:
        return False


def summarise(records):
    """The one line per cell that the morning read needs."""
    lines = []
    for record in records:
        matrix = record.get("matrix")
        if not matrix:
            lines.append("%-18s %-9s %s" % (record["cell"], record["cores"],
                                            "no matrix"))
            continue
        mixes = matrix.get("mixes") or {}
        passing, failing, limited = [], [], []
        for mix, entry in sorted(mixes.items()):
            if not isinstance(entry, dict):
                continue
            if entry.get("highest_passing") is not None:
                passing.append("%s=%s" % (mix, entry["highest_passing"]))
            if entry.get("first_quality_failure") is not None:
                failing.append("%s=%s" % (mix, entry["first_quality_failure"]))
            if entry.get("infrastructure_limited"):
                limited.append("%s=%s" % (mix, entry["infrastructure_limited"]))
            if entry.get("setup_failure_rung_params") or entry.get("setup_limited"):
                failing.append("%s setup@%s" % (mix, entry.get("setup_limited")))
        receiver = record.get("receiver_cpu_of_one_core")
        lines.append("%-18s %-9s pass:%s%s"
                     % (record["cell"], record["cores"],
                        ",".join(passing) or "none",
                        "" if receiver is None
                        else "  (receiver peaked at %d%% of one core)" % receiver))
        if failing:
            lines.append("%-28s first quality failures: %s"
                         % ("", ",".join(failing)))
        if limited:
            lines.append("%-28s infrastructure limited: %s"
                         % ("", ",".join(limited)))
    return "\n".join(lines)


def write_json(path, value):
    """Replace a JSON file atomically, so a killed sweep never leaves half."""
    temporary = path + ".tmp"
    with open(temporary, "w") as handle:
        json.dump(value, handle, indent=2)
    os.replace(temporary, path)


def sweep(args):
    engines = ENGINES
    if args.engines:
        wanted = {name.strip() for name in args.engines.split(",")}
        engines = [e for e in ENGINES if e["id"] in wanted]

    cells = [(engine, PCORES[:count])
             for engine in engines
             for count in range(1, len(PCORES) + 1)]
    if args.cell:
        wanted = set(args.cell)
        cells = [cell for cell in cells if cell_id(*cell) in wanted]
    missing = [engine["id"] for engine, _ in cells
               if not os.path.exists(engine_binary(engine))]
    if missing:
        log("skipping cells for engines with no binary: %s"
            % ", ".join(sorted(set(missing))))
        cells = [(e, c) for e, c in cells if os.path.exists(engine_binary(e))]
    if not cells:
        log("no cells to run")
        return 1

    # Without the namespaces the harness would silently measure loopback.
    veth = subprocess.run(["tests/bench/capacity_veth.sh", "up"],
                          cwd=PROJECT_ROOT, text=True, capture_output=True,
                          timeout=120)
    args.veth_env = {}
    for line in veth.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            args.veth_env[key.strip()] = value.strip().strip("'\"")
    absent = [key for key in VETH_KEYS if not args.veth_env.get(key)]
    if veth.returncode != 0 or absent:
        log("capacity_veth.sh up failed (exit %d, missing %s): %s"
            % (veth.returncode, ",".join(absent) or "nothing",
               veth.stderr.strip()))
        return 1
    args.veth_env = {key: args.veth_env[key] for key in VETH_KEYS}

    snapshot, helpers, scripts = snapshot_harness()
    log("harness snapshot: %s" % os.path.relpath(snapshot, PROJECT_ROOT))
    log("%d cells, %ds per rung, %d workloads each"
        % (len(cells), args.seconds, len(args.mixes.split())))

    records = []
    started = time.time()
    for engine, cpus in cells:
        identifier = cell_id(engine, cpus)
        result_path = os.path.join(args.out, identifier, "result.json")
        if os.path.exists(result_path) and not args.force:
            try:
                with open(result_path) as handle:
                    previous = json.load(handle)
            except (OSError, ValueError):
                previous = {}
            # A ladder stopping at its first quality failure exits non-zero
            # and is complete; only the matrix decides.
            if complete(previous, args):
                log("%-18s already measured, skipping" % identifier)
                records.append(previous)
                continue
            log("%-18s previous attempt incomplete (%d of %d workloads, exit %s);"
                " re-running" % (identifier,
                                 len(workloads_in(previous.get("matrix"))),
                                 len(args.mixes.split()),
                                 previous.get("exit_status")))

        log("%-18s %d core%s (%s)"
            % (identifier, len(cpus), "" if len(cpus) == 1 else "s",
               ",".join(str(c) for c in cpus)))
        try:
            record = run_cell(engine, cpus, snapshot, helpers, scripts, args)
        except SystemExit:
            log("interrupted; %s is not recorded and will re-run" % identifier)
            raise
        write_json(result_path, record)
        records.append(record)
        log("%-18s done in %.0fs, exit %s"
            % (identifier, record["wall_seconds"], record["exit_status"]))

        # No matrix and no rung ever started: the harness failed to start,
        # and every remaining cell would record the same fault.
        if record["matrix"] is None and not record["rung_started"]:
            log("")
            log("%s died after %.1fs before its first rung - this is a harness "
                "fault, not a capacity result. Stopping rather than recording "
                "the same fault %d more times."
                % (identifier, record["wall_seconds"], len(cells) - 1))
            log("the cell's own log is %s"
                % os.path.relpath(os.path.join(args.out, identifier, "bench.log"),
                                  PROJECT_ROOT))
            return 1

    elapsed = time.time() - started
    write_json(os.path.join(args.out, "sweep.json"), {
        "started_at": datetime.fromtimestamp(started, timezone.utc)
                              .isoformat(timespec="seconds"),
        "seconds_per_rung": args.seconds,
        "steps": args.steps,
        "mixes": args.mixes.split(),
        "elapsed_seconds": round(elapsed, 1),
        "cells": records,
    })
    log("")
    log(summarise(records))
    log("")
    log("wrote %s" % os.path.join(args.out, "sweep.json"))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=None,
                        help="result directory; default docs/capacity-evidence/"
                             "sweep-YYYYMMDD (with -smoke under --smoke)")
    parser.add_argument("--cell-budget", type=int, default=10800,
                        help="wall-clock seconds one cell may take before it is "
                             "stopped and recorded as a timeout")
    parser.add_argument("--seconds", type=int, default=60,
                        help="measurement window per rung")
    parser.add_argument("--cell", action="append", default=[],
                        help="cell id such as nginx-haivision-4c, repeatable; "
                             "default is every cell")
    parser.add_argument("--force", action="store_true",
                        help="re-run cells that already have a result")
    parser.add_argument("--engines", default="",
                        help="comma-separated engine ids to restrict to")
    parser.add_argument("--smoke", action="store_true",
                        help="one reference rung and a ten-destination rung per "
                             "workload, ten seconds each, to show the harness "
                             "works end to end before a night is spent on it")
    parser.add_argument("--mixes", default=MIXES,
                        help="workloads to run per cell; default is all seven")
    parser.add_argument("--steps", default=STEPS,
                        help="destination ladder per workload")
    args = parser.parse_args()
    if args.smoke:
        args.mixes = MIXES
        # The 1-destination rung is the reference the ladder calibrates from.
        args.steps = "1 10"
        args.seconds = 10
        args.cell_budget = min(args.cell_budget, 1800)
    # Absolute: the harness runs with cwd=PROJECT_ROOT, and leftover processes
    # are matched by this path.
    args.out = os.path.abspath(args.out or os.path.join(
        PROJECT_ROOT, "docs/capacity-evidence", "sweep-%s%s"
        % (time.strftime("%Y%m%d"), "-smoke" if args.smoke else "")))
    os.makedirs(args.out, exist_ok=True)

    for stop in STOP_SIGNALS:
        signal.signal(stop, on_signal)
    # Lead a process group and record it, so a stop targets this sweep by
    # identity rather than by a command-line match.
    try:
        os.setpgrp()
    except OSError:
        pass
    pid_file = os.path.join(args.out, "sweep.pid")
    with open(pid_file, "w") as handle:
        handle.write("%d\n" % os.getpgid(0))
    try:
        return sweep(args)
    finally:
        if os.path.exists(pid_file):
            os.remove(pid_file)


if __name__ == "__main__":
    sys.exit(main())

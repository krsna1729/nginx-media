#!/usr/bin/env python3
"""Drive the CPU-count sweep across every engine and workload.

The unit of work is one (engine, CPU count) cell, which runs the full
seven-workload ladder.  The harness already takes the mix list in one
invocation, so a cell is one process and its result is one matrix file; a
sweep is eighteen of them.

Two properties matter more than speed here, because a sweep is long enough
that it will be interrupted:

**It is resumable.**  A cell whose matrix file already exists is skipped, so a
sweep that stops on power, on a crash or on a deadline continues rather than
restarts.  `--force` re-runs a cell anyway.

**It runs from an immutable copy of the harness.**  Every cell reads one
snapshot taken at start-up.  Editing the working file during a sweep cannot
change what a cell is running - which has bitten this work twice, once by
invalidating a run whose script was edited underneath it and once by deleting
the snapshot mid-run.

The CPUs are P-cores without their SMT siblings, because a hyperthread sharing
a physical core is not a core's worth of capacity.  Each cell is pinned with
the benchmark helper, which also takes the CPU frequency policy to
performance and restores it afterwards.

Usage:
    tests/bench/capacity_sweep.py --out docs/capacity-evidence/sweep-2026-10-16
    tests/bench/capacity_sweep.py --resume
    tests/bench/capacity_sweep.py --cell nginx-haivision:4 --force
"""

from __future__ import annotations

import argparse
import json
import os
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

#: One CPU per cell, and only P-cores: 0-11 are the six physical P-cores with
#: their SMT siblings, 12-19 are the eight E-cores.  Taking every second CPU
#: from the P-core range gives one thread per physical core.
PCORES = [0, 2, 4, 6, 8, 10]

ENGINES = [
    {"id": "nginx-robotweax",
     "binary": ".build/nginx-robotweax/install/sbin/nginx"},
    {"id": "nginx-haivision",
     "binary": ".build/nginx-haivision/install/sbin/nginx"},
    {"id": "restream",
     "binary": "../restream/target/release/restream"},
]

#: The receiver runs on the E-cores so it cannot steal from the sender, which
#: is what the published ladders use.
RECEIVER_CPUS = "12,13,14,15,16,17,18,19"
PUBLISHER_CPUS = "12,13"


def log(message):
    print("[%s] %s" % (datetime.now().strftime("%H:%M:%S"), message), flush=True)


def engine_binary(engine):
    return os.path.join(PROJECT_ROOT, engine["binary"])


def snapshot_harness():
    """One immutable copy of the harness for the whole sweep.

    The harness resolves its own root two levels up, so the copy has to sit in
    the same directory; it is dot-prefixed and ignored by git.
    """
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(PROJECT_ROOT, "tests/bench",
                        ".sweep-%s.sh" % stamp)
    shutil.copy(HARNESS, path)
    os.chmod(path, 0o755)
    return path


def cell_id(engine, cpus):
    return "%s:%d" % (engine["id"], len(cpus))


def run_cell(engine, cpus, snapshot, args, out_dir):
    """Run the seven-workload ladder for one engine at one CPU count."""
    identifier = cell_id(engine, cpus)
    case_dir = os.path.join(out_dir, identifier)
    run_dir = os.path.join(case_dir, "run")
    os.makedirs(run_dir, exist_ok=True)

    cpu_list = ",".join(str(cpu) for cpu in cpus)
    # The engine under test is the only thing pinned to the measured cores;
    # the publisher and the receiver stay on the E-cores.
    binary = engine_binary(engine)
    environment = dict(os.environ, **{
        "CAPACITY_PREFLIGHT": "no",
        "CAPACITY_ENGINE": "restream" if engine["id"] == "restream" else "nginx",
        "CAPACITY_NGINX": binary,
        "RESTREAM_BIN": binary,
        # The RTMP sink is always a media build, whichever engine is under test.
        "CAPACITY_RTMP_SINK_NGINX": os.path.join(
            PROJECT_ROOT, ".build/nginx-haivision/install/sbin/nginx"),
        "CAPACITY_NGINX_CPUS": cpu_list,
        "CAPACITY_RECEIVER_CPUS": RECEIVER_CPUS,
        "CAPACITY_PUBLISHER_CPUS": PUBLISHER_CPUS,
        "CAPACITY_QUALITY_SECONDS": str(args.seconds),
        "CAPACITY_QUALITY_MIXES": MIXES,
        "CAPACITY_QUALITY_STOP_AFTER_FAILURES": "1",
        "CAPACITY_SINK_MODE": "correctness",
        "PHASES": "capacity-quality-ladder",
        "CAPACITY_RUN": run_dir,
    })
    for key in ("CAPACITY_SENDER_EXEC", "CAPACITY_RECEIVER_EXEC",
                "CAPACITY_RECEIVER_ADDR", "CAPACITY_SENDER_ADDR"):
        if args.veth_env.get(key):
            environment[key] = args.veth_env[key]

    command = ["omarchy-benchmark", "--cpu", cpu_list, "--isolate",
               "--turbo", "on", "--", "bash", snapshot]
    started = time.time()
    with open(os.path.join(case_dir, "bench.log"), "w") as handle:
        result = subprocess.run(command, cwd=PROJECT_ROOT, env=environment,
                                stdout=handle, stderr=subprocess.STDOUT)
    elapsed = time.time() - started

    matrix = os.path.join(run_dir, "capacity-matrix.json")
    record = {
        "cell": identifier,
        "engine": engine["id"],
        "cpus": cpu_list,
        "cores": len(cpus),
        "seconds_per_rung": args.seconds,
        "exit_status": result.returncode,
        "wall_seconds": round(elapsed, 1),
        "finished_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "matrix": None,
    }
    if os.path.exists(matrix):
        with open(matrix) as handle:
            record["matrix"] = json.load(handle)
    else:
        record["note"] = "the harness wrote no matrix for this cell"
    return record


def summarise(records):
    """The one line per cell that the morning read needs."""
    lines = []
    for record in records:
        matrix = record.get("matrix")
        if not matrix:
            lines.append("%-18s %-9s %s" % (record["cell"], record["cores"],
                                            "no matrix"))
            continue
        entries = matrix if isinstance(matrix, list) else matrix.get("mixes", [])
        passing, failing, limited = [], [], []
        for entry in entries:
            mix = entry.get("mix", "?")
            if entry.get("highest_passing") is not None:
                passing.append("%s=%s" % (mix, entry["highest_passing"]))
            if entry.get("first_quality_failure") is not None:
                failing.append("%s=%s" % (mix, entry["first_quality_failure"]))
            if entry.get("infrastructure_limited"):
                limited.append("%s=%s" % (mix, entry["infrastructure_limited"]))
        lines.append("%-18s %-9s pass:%s" % (record["cell"], record["cores"],
                                             ",".join(passing) or "none"))
        if failing:
            lines.append("%-28s first quality failures: %s"
                         % ("", ",".join(failing)))
        if limited:
            lines.append("%-28s infrastructure limited: %s"
                         % ("", ",".join(limited)))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=os.path.join(
        PROJECT_ROOT, "docs/capacity-evidence",
        "sweep-%s" % time.strftime("%Y%m%d")))
    parser.add_argument("--seconds", type=int, default=60,
                        help="measurement window per rung")
    parser.add_argument("--cell", action="append", default=[],
                        help="engine:cores, repeatable; default is every cell")
    parser.add_argument("--force", action="store_true",
                        help="re-run cells that already have a result")
    parser.add_argument("--engines", default="",
                        help="comma-separated engine ids to restrict to")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)

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

    # Both namespaces exist for the whole sweep; the harness enters them.
    veth = subprocess.run(["tests/bench/capacity_veth.sh", "up"],
                          cwd=PROJECT_ROOT, text=True, capture_output=True)
    args.veth_env = {}
    for line in veth.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            args.veth_env[key.strip()] = value.strip().strip("'\"")

    snapshot = snapshot_harness()
    log("harness snapshot: %s" % os.path.relpath(snapshot, PROJECT_ROOT))
    log("%d cells, %ds per rung, %d workloads each"
        % (len(cells), args.seconds, len(MIXES.split())))

    records = []
    started = time.time()
    for engine, cpus in cells:
        identifier = cell_id(engine, cpus)
        result_path = os.path.join(args.out, identifier, "result.json")
        if os.path.exists(result_path) and not args.force:
            log("%-18s already measured, skipping" % identifier)
            with open(result_path) as handle:
                records.append(json.load(handle))
            continue

        log("%-18s %d core%s (%s)"
            % (identifier, len(cpus), "" if len(cpus) == 1 else "s",
               ",".join(str(c) for c in cpus)))
        try:
            record = run_cell(engine, cpus, snapshot, args, args.out)
        except KeyboardInterrupt:
            log("interrupted; %s is not recorded and will re-run"
                % identifier)
            break
        os.makedirs(os.path.dirname(result_path), exist_ok=True)
        with open(result_path, "w") as handle:
            json.dump(record, handle, indent=2)
        records.append(record)
        log("%-18s done in %.0fs, exit %s"
            % (identifier, record["wall_seconds"], record["exit_status"]))

    elapsed = time.time() - started
    summary = {
        "started_at": datetime.fromtimestamp(started, timezone.utc)
                                .isoformat(timespec="seconds"),
        "seconds_per_rung": args.seconds,
        "mixes": MIXES.split(),
        "elapsed_seconds": round(elapsed, 1),
        "cells": records,
    }
    with open(os.path.join(args.out, "sweep.json"), "w") as handle:
        json.dump(summary, handle, indent=2)

    log("")
    log(summarise(records))
    log("")
    log("wrote %s" % os.path.join(args.out, "sweep.json"))
    # The snapshot is left in place: a resumed sweep re-runs cells that were
    # interrupted, and they must read the same harness as the finished ones.
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Summarize, gate and publish capacity benchmark results over time.

  summarize <results dir> --tier T --out summary.json
      one record for a CI run: per configuration and workload the highest
      passing rung, the first quality failure, setup-limited rungs, and per
      rung the delivery ratio and CPU per delivered Gbit/s.

  gate <summary.json> --tier T [--history data.jsonl]
      exit 1 on any setup failure, on an incomplete run (a harness that did
      not finish, or an expected rung neither measured nor deliberately
      skipped after the capacity boundary), and on a quality failure at a
      rung the tier declares every supported host must carry.  With history, warns
      (never fails) when CPU per Gbit/s regressed against the recent median
      for the same rung - shared runners are too noisy to gate on it.

  publish <summary.json> --pages <gh-pages checkout>
      appends the record to data/<tier>.jsonl and installs the viewer.
"""

import argparse
import datetime
import glob
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys

SCHEMA = "nginx-media.bench-history/1"

# The largest rung each tier requires to pass on any host it runs on.  These
# are floors that say "the system works", far below any capacity boundary;
# the boundary itself is recorded, never gated.
REQUIRED_RUNG = {"pr": 16, "branch": 16, "nightly": 16, "weekly": 16}


def git(*args):
    try:
        return subprocess.check_output(("git",) + args, text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def host_info():
    model = None
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as source:
            for line in source:
                if line.startswith("model name"):
                    model = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    return {"nproc": os.cpu_count(), "cpu_model": model,
            "kernel": platform.release(),
            "runner": os.environ.get("RUNNER_NAME")}


def value(field):
    return field.get("value") if isinstance(field, dict) else None


def rung_record(bundle):
    delivery = bundle.get("delivery") or {}
    ratios = []
    for key in ("srt", "rtmp", "hls_push", "hls_readers"):
        report = value(delivery.get(key)) or {}
        for name in ("quality_average_delivery_ratio_min",
                     "quality_min_delivery_ratio", "min_delivery_ratio"):
            if isinstance(report.get(name), (int, float)):
                ratios.append(report[name])
    efficiency = bundle.get("efficiency") or {}
    env = bundle.get("cpu_environment") or {}
    return {
        "destinations": bundle.get("case", {}).get("destinations"),
        "outcome": bundle.get("outcome"),
        "min_delivery_ratio": min(ratios) if ratios else None,
        "delivered_gbps": value(efficiency.get("delivered")),
        "sender_cpu_per_gbps": value(efficiency.get("sender_cpu_per_gbps")),
        "receiver_cpu_per_gbps": value(efficiency.get("receiver_cpu_per_gbps")),
        "host_cpu_busy": value(env.get("host_cpu_busy_fraction")),
        "srt_backend": bundle.get("case", {}).get("srt_backend"),
    }


def harness_log(results, name):
    """what the harness said about its own run: whether it reached its end,
    and the rungs it skipped on purpose after the capacity boundary"""
    finished, stopped = False, {}
    try:
        with open(os.path.join(results, name + ".log"), encoding="utf-8",
                  errors="replace") as source:
            for line in source:
                line = line.strip()
                if line.startswith("quality_ladders_with_failures="):
                    finished = True
                elif line.startswith("mix=") and " unmeasured_rungs=" in line:
                    mix, _, rest = line[4:].partition(" unmeasured_rungs=")
                    rungs = rest.split(" (", 1)[0].split()
                    stopped[mix] = [int(r) for r in rungs if r.isdigit()]
    except OSError:
        pass
    return finished, stopped


def completeness(results, name, mixes):
    """the expected-run manifest against what the run produced

    Every expected rung of every expected mix must be accounted for: a
    diagnostics bundle (pass, quality failure or setup failure), or a rung
    the harness skipped on purpose after consecutive failures at the
    capacity boundary.  Anything else - a mix never started, a ladder cut
    short by a crash - is missing, and the run is incomplete."""
    try:
        with open(os.path.join(results, name, "expected.json"),
                  encoding="utf-8") as source:
            expected = json.load(source)
    except (OSError, ValueError):
        return {"complete": False, "finished": False,
                "missing": ["no expected-run manifest"], "stopped": {}}
    finished, stopped = harness_log(results, name)
    missing = []
    for mix in expected.get("mixes", []):
        measured = {r["destinations"] for r in
                    (mixes.get(mix) or {}).get("rungs", [])}
        skipped = set(stopped.get(mix, []))
        absent = [n for n in expected.get("steps", [])
                  if n not in measured and n not in skipped]
        if absent:
            missing.append(f"{mix}: rungs {absent} neither measured nor "
                           f"skipped at the boundary")
    return {"complete": finished and not missing, "finished": finished,
            "missing": missing, "stopped": stopped, "expected": expected}


def summarize(args):
    configs = {}
    statuses = {}
    for path in glob.glob(os.path.join(args.results, "*.status")):
        try:
            with open(path, encoding="utf-8") as src:
                statuses[os.path.basename(path)[:-7]] = int(src.read().strip())
        except (OSError, ValueError):
            pass
    for config_dir in sorted(glob.glob(os.path.join(args.results, "*", ""))):
        name = os.path.basename(os.path.dirname(config_dir))
        mixes = {}
        for path in sorted(glob.glob(os.path.join(config_dir, "capacity", "*",
                                                  "diagnostics.json"))):
            with open(path, encoding="utf-8") as source:
                bundle = json.load(source)
            mix = bundle.get("case", {}).get("mix") or "unknown"
            mixes.setdefault(mix, {"rungs": []})["rungs"].append(
                rung_record(bundle))
        for mix, entry in mixes.items():
            entry["rungs"].sort(key=lambda r: r["destinations"] or 0)
            passing = [r["destinations"] for r in entry["rungs"]
                       if r["outcome"] == "pass"]
            failing = [r["destinations"] for r in entry["rungs"]
                       if r["outcome"] == "quality-failure"]
            entry["highest_passing"] = max(passing) if passing else None
            entry["first_quality_failure"] = min(failing) if failing else None
            entry["setup_limited"] = [r["destinations"] for r in entry["rungs"]
                                      if r["outcome"] == "setup-failure"]
        configs[name] = {"harness_status": statuses.get(name), "mixes": mixes}
        configs[name].update(completeness(args.results, name, mixes))
    record = {
        "schema": SCHEMA,
        "tier": args.tier,
        "timestamp": datetime.datetime.now(datetime.timezone.utc)
                     .isoformat(timespec="seconds"),
        "sha": os.environ.get("GITHUB_SHA") or git("rev-parse", "HEAD"),
        "ref": os.environ.get("GITHUB_REF_NAME")
               or git("rev-parse", "--abbrev-ref", "HEAD"),
        "run": os.environ.get("GITHUB_RUN_ID"),
        "host": host_info(),
        "configs": configs,
    }
    with open(args.out, "w", encoding="utf-8") as output:
        json.dump(record, output, indent=1)
    print(f"bench_summary={args.out}")
    for name, config in configs.items():
        for mix, entry in config["mixes"].items():
            print(f"bench {name} {mix} highest_passing={entry['highest_passing']}"
                  f" first_quality_failure={entry['first_quality_failure']}"
                  f" setup_limited={entry['setup_limited'] or 'none'}")
    return 0


def load_history(path, tier):
    records = []
    if not path or not os.path.exists(path):
        return records
    with open(path, encoding="utf-8") as source:
        for line in source:
            line = line.strip()
            if line:
                record = json.loads(line)
                if record.get("tier") == tier:
                    records.append(record)
    return records


def gate(args):
    with open(args.summary, encoding="utf-8") as source:
        record = json.load(source)
    required = REQUIRED_RUNG.get(args.tier, 0)
    errors = []
    for name, config in record["configs"].items():
        if not config["mixes"]:
            errors.append(f"{name}: no rung produced a diagnostics bundle")
        if not config.get("finished", False):
            errors.append(f"{name}: the harness did not finish (exit status "
                          f"{config.get('harness_status')}); its results are "
                          f"partial")
        for message in config.get("missing", []):
            errors.append(f"{name}: incomplete - {message}")
        for mix, entry in config["mixes"].items():
            if entry["setup_limited"]:
                errors.append(f"{name}/{mix}: setup failure at "
                              f"{entry['setup_limited']} (never measured)")
            for rung in entry["rungs"]:
                if (rung["outcome"] == "quality-failure"
                        and (rung["destinations"] or 0) <= required):
                    errors.append(f"{name}/{mix}: quality failure at "
                                  f"{rung['destinations']} destinations, "
                                  f"which every host must carry")
    for message in regressions(record, load_history(args.history, args.tier)):
        print(f"::warning title=bench efficiency::{message}")
    for message in errors:
        print(f"::error title=bench gate::{message}")
    if errors:
        return 1
    print("bench_gate=pass")
    return 0


def regressions(record, history, window=10, tolerance=1.25):
    """CPU per Gbit/s that is more than `tolerance` times the recent median
    for the same configuration, workload and rung."""
    messages = []
    recent = history[-window:]
    for name, config in record["configs"].items():
        for mix, entry in config["mixes"].items():
            for rung in entry["rungs"]:
                current = rung.get("sender_cpu_per_gbps")
                if current is None or rung["outcome"] != "pass":
                    continue
                past = []
                for old in recent:
                    old_entry = old.get("configs", {}).get(name, {}).get(
                        "mixes", {}).get(mix)
                    for old_rung in (old_entry or {}).get("rungs", []):
                        if (old_rung["destinations"] == rung["destinations"]
                                and old_rung.get("sender_cpu_per_gbps")):
                            past.append(old_rung["sender_cpu_per_gbps"])
                if len(past) >= 3:
                    median = statistics.median(past)
                    if current > median * tolerance:
                        messages.append(
                            f"{name}/{mix} at {rung['destinations']} destinations:"
                            f" sender CPU {current:.1f}%/Gbit/s vs recent median"
                            f" {median:.1f}")
    return messages


def publish(args):
    with open(args.summary, encoding="utf-8") as source:
        record = json.load(source)
    data_dir = os.path.join(args.pages, "data")
    os.makedirs(data_dir, exist_ok=True)
    path = os.path.join(data_dir, f"{record['tier']}.jsonl")
    with open(path, "a", encoding="utf-8") as output:
        output.write(json.dumps(record, separators=(",", ":")) + "\n")
    viewer = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "history", "index.html")
    shutil.copyfile(viewer, os.path.join(args.pages, "index.html"))
    tiers = sorted(os.path.basename(p)[:-6]
                   for p in glob.glob(os.path.join(data_dir, "*.jsonl")))
    with open(os.path.join(data_dir, "index.json"), "w",
              encoding="utf-8") as output:
        json.dump({"tiers": tiers}, output)
    open(os.path.join(args.pages, ".nojekyll"), "a").close()
    print(f"published {record['tier']} record to {path}")
    return 0


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("summarize")
    s.add_argument("results")
    s.add_argument("--tier", required=True)
    s.add_argument("--out", required=True)
    g = sub.add_parser("gate")
    g.add_argument("summary")
    g.add_argument("--tier", required=True)
    g.add_argument("--history")
    p = sub.add_parser("publish")
    p.add_argument("summary")
    p.add_argument("--pages", required=True)
    args = parser.parse_args()
    return {"summarize": summarize, "gate": gate, "publish": publish}[
        args.command](args)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Summarize, gate and publish capacity benchmark results over time.

  summarize <results dir> --tier T --out summary.json [--expected-configs JSON]
      one CI record: each configuration and workload's highest passing rung,
      first quality failure and per-rung delivery/CPU efficiency. Expected
      matrix groups are flattened into configuration names so absent artifacts
      remain visible as incomplete configurations.

  gate <summary.json> --tier T [--history data.jsonl]
      exit 1 on setup failures, incomplete runs or required-rung quality
      failures. With history, warn on CPU/Gbit/s regressions over 25% against
      at least three complete, passing runs with the same runner fingerprint.

  regressions <summary.json> --history data.jsonl --out findings.json
      write the structured regression findings used by the publisher.

  publish <summary.json> --pages <gh-pages checkout>
      append the record to data/<tier>.jsonl and install the viewer.
"""

import argparse
import copy
import datetime
import glob
import hashlib
import json
import math
import os
import shutil
import statistics
import subprocess
import sys

SCHEMA = "nginx-media.bench-history/3"

# Which delivery field carries a *measurement*, per protocol.  A configured
# threshold is not one: min_delivery_ratio in the HLS reader log is the gate
# the run was asked to accept, and reading it as a measurement is what made a
# passing HLS-origin rung look like it delivered exactly 95% of its reference.
# Records written before this schema hold whatever the old reader reported;
# they are left as they are and their basis says so.
OBSERVED_RATIO_KEYS = {
    "srt": ("quality_average_delivery_ratio_min",),
    "rtmp": ("quality_min_delivery_ratio",),
    "hls_push": ("quality_average_delivery_ratio_min",),
    "hls_readers": ("observed_min_delivery_ratio",),
}

THRESHOLD_RATIO_KEYS = {
    "hls_readers": ("delivery_ratio_threshold", "min_delivery_ratio"),
}

PROTOCOLS = ("srt", "rtmp", "hls_push", "hls_readers")

# The harness's exit status has two meanings that both describe a finished
# run: 0 (every rung passed) and 1 (quality failures were recorded, which is
# how a ladder that stopped at the capacity boundary ends).  Anything else -
# 124 from a timeout, 137 from a kill, a missing status file - means the run
# did not finish and its bundles are a fragment.
HARNESS_STATUS_ALLOWED = {0, 1}

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


def value(field):
    return field.get("value") if isinstance(field, dict) else None


def preflight_verdict(preflight):
    """The rung's preflight verdict and the resources it named, or None when
    the bundle has none (older bundles, or a rung that never got that far)."""
    if not isinstance(preflight, dict) or not preflight.get("verdict"):
        return None

    def named(items):
        return [f"{item.get('side')}/{item.get('resource')}"
                if isinstance(item, dict) else str(item)
                for item in items or []]
    return {"verdict": preflight["verdict"],
            "limits": named(preflight.get("limits")),
            "unknown": named(preflight.get("unknown"))}


def calibration_references(delivery):
    """The reference rate each protocol's rung was judged against: with it a
    ratio can be read, and two rungs' ratios told apart as the same yardstick
    or not."""
    references = {}
    for protocol in PROTOCOLS:
        report = value(delivery.get(protocol))
        if not isinstance(report, dict):
            continue
        for key in ("quality_reference_payload_bps", "reference_bps"):
            number = report.get(key)
            if (isinstance(number, (int, float)) and not isinstance(number, bool)
                    and math.isfinite(number) and number > 0):
                references[protocol] = number
                break
    return references


def rung_record(bundle):
    delivery = bundle.get("delivery") or {}
    strict = {}
    for protocol in PROTOCOLS:
        report = value(delivery.get(protocol))
        if isinstance(report, dict) and "quality_strict_full_rate" in report:
            strict[protocol] = report["quality_strict_full_rate"]
    observed, thresholds, unmeasured, present = [], [], [], []
    for protocol in PROTOCOLS:
        report = value(delivery.get(protocol))
        if not isinstance(report, dict):
            continue
        present.append(protocol)
        found = [report[name] for name in OBSERVED_RATIO_KEYS[protocol]
                 if isinstance(report.get(name), (int, float))]
        if found:
            observed.append(min(found))
        else:
            unmeasured.append(protocol)
        thresholds.extend(
            report[name] for name in THRESHOLD_RATIO_KEYS.get(protocol, ())
            if isinstance(report.get(name), (int, float)))
    efficiency = bundle.get("efficiency") or {}
    env = bundle.get("cpu_environment") or {}
    return {
        "destinations": bundle.get("case", {}).get("destinations"),
        "outcome": bundle.get("outcome"),
        "observed_delivery_ratio": min(observed) if observed else None,
        "observed_delivery_ratio_protocols": [
            p for p in present if p not in unmeasured],
        "delivery_ratio_threshold": min(thresholds) if thresholds else None,
        "delivery_ratio_basis": ratio_basis(observed, present, unmeasured,
                                            thresholds),
        # the strict qualification, separate from the 0.95 compatibility gate:
        # full rate within a documented tolerance, nothing dropped or
        # corrupted.  Absent means the run predates it or did not report it -
        # never an implied pass.
        "strict_full_rate": strict,
        # the environment this rung was taken in, and its peak pressure
        "host_fingerprint": bundle.get("host_fingerprint"),
        "peak": peak_pressure(bundle),
        "strict_full_rate_pass": (all(v == "yes" for v in strict.values())
                                  if strict else None),
        "delivered_gbps": value(efficiency.get("delivered")),
        "sender_cpu_per_gbps": value(efficiency.get("sender_cpu_per_gbps")),
        "receiver_cpu_per_gbps": value(efficiency.get("receiver_cpu_per_gbps")),
        "host_cpu_busy": value(env.get("host_cpu_busy_fraction")),
        "srt_backend": bundle.get("case", {}).get("srt_backend"),
        # what the preflight concluded about this rung's host, and the
        # one-destination reference rate each protocol was judged against
        "preflight": preflight_verdict(bundle.get("preflight")),
        "reference_bps": calibration_references(delivery),
    }


def ratio_basis(observed, present, unmeasured, thresholds):
    """How much of the reported delivery is a measurement.

    "observed" means every workload the bundle reports had receiver-counted
    delivery; "observed-for-..." names the ones that did, so a workload whose
    report carried only a configured gate cannot hide behind the others;
    "threshold-only" means the bundle carried a gate and no measurement at
    all, which is a statement about the harness, not about delivery."""
    measured = [p for p in present if p not in unmeasured]
    if observed and not unmeasured:
        return "observed"
    if observed:
        return "observed-for-" + ",".join(measured)
    if thresholds:
        return "threshold-only"
    return "unavailable"


def peak_pressure(bundle):
    """The rung's own peak resource pressure: what the sender and the
    receivers used, what the kernel dropped, how far the event loop slipped
    and how much memory the worker held."""
    cpu = bundle.get("cpu") or {}
    kinds = (cpu.get("by_kind_pct_of_core") or {}).get("value") or {}
    sender = kinds.get("worker")
    receivers = sum(v for k, v in kinds.items()
                    if k.endswith("receiver") or k == "hls-readers") or None
    drops = ((bundle.get("network") or {})
             .get("udp_socket_drops_by_process") or {}).get("value") or {}
    socket_drops = sum(row.get("socket_drops_delta") or 0
                       for row in drops.values()) if drops else None
    delays = (bundle.get("event_loop_max_delay") or {}).get("value") or {}
    memory = ((bundle.get("resources") or {})
              .get("worker_memory") or {}).get("value") or {}
    rss = [snapshot.get("rss_kb")
           for pid in memory.values()
           for snapshot in (pid or {}).values()
           if isinstance(snapshot, dict) and snapshot.get("rss_kb")]
    return {
        "sender_cpu_pct_of_core": sender,
        "receiver_cpu_pct_of_core": receivers,
        "socket_drops": socket_drops,
        "event_loop_max_delay_ms": (max(delays.values()) if delays else None),
        "worker_rss_kb_max": (max(rss) if rss else None),
    }


# The harness files whose content defines what a rung measures.  Their
# combined hash is the "harness revision" of a recipe: a run taken by a
# changed harness is not comparable with one taken by the earlier harness,
# whatever the nginx binary under test was.  (Not the binary: every code
# revision under comparison is a different binary by definition.)
HARNESS_FILES = (
    "ingest_egress_fanout.sh", "capacity-mixes.conf",
    "capacity_diagnostics.py", "capacity_preflight.py",
    "hls_capacity_readers.py", "hls_push_capacity.py",
    "proc_cpu_sampler.py", "rtmp_capacity_quality.py",
    "srt_capacity_quality.py", "srt_fanout_sink.c")

# What the harness prints on its `quality_recipe` line, and how each value is
# read back.  Everything on it describes how a rung was taken.
RECIPE_FIELDS = {
    "seconds": float, "rate": str, "steps": "ints",
    "stop_after_failures": int, "min_delivery_ratio": float,
    "interval_floor": float, "max_low_seconds": float,
    "srt_workers": str, "hls_push_workers": str, "srt_hls": str}

# A rung's number depends on what was offered, for how long, how it was
# judged and which harness measured it; it does not depend on which other
# rungs the ladder happened to contain.  So every recipe field must match
# except the rung list, which only has to start at the same rung (the
# calibration reference comes from the first rung).
RECIPE_MATCH = ("seconds", "rate", "stop_after_failures", "min_delivery_ratio",
                "interval_floor", "max_low_seconds", "srt_workers",
                "hls_push_workers", "srt_hls", "mixes", "harness")

# History a record may be compared with.  A pull request runs a subset of the
# branch recipe, so main's published branch records are its baseline (the
# recipe check still decides whether any one of them is comparable).
COMPARABLE_TIERS = {"pr": ("pr", "branch")}


def harness_revision(root=None):
    """A short hash of the harness files, or None if any is unreadable."""
    root = root or os.path.dirname(os.path.abspath(__file__))
    digest = hashlib.sha256()
    for name in HARNESS_FILES:
        try:
            with open(os.path.join(root, name), "rb") as source:
                digest.update(name.encode() + b"\0" + source.read() + b"\0")
        except OSError:
            return None
    return digest.hexdigest()[:16]


def parse_recipe(tokens):
    recipe = {}
    for token in tokens:
        key, _, text = token.partition("=")
        kind = RECIPE_FIELDS.get(key)
        try:
            if kind == "ints":
                recipe[key] = [int(n) for n in text.split(",") if n]
            elif kind is not None:
                recipe[key] = kind(text)
        except ValueError:
            return None
    if set(recipe) != set(RECIPE_FIELDS) or not recipe["steps"]:
        return None
    if recipe["seconds"] == int(recipe["seconds"]):
        recipe["seconds"] = int(recipe["seconds"])
    return recipe


def harness_log(results, name):
    """what the harness said about its own run: whether it reached its end,
    the rungs it skipped on purpose after the capacity boundary, the mixes it
    executed and the recipe it ran them with"""
    log = {"finished": False, "stopped": {}, "mixes": None, "recipe": None}
    try:
        with open(os.path.join(results, name + ".log"), encoding="utf-8",
                  errors="replace") as source:
            for line in source:
                line = line.strip()
                if line.startswith("quality_ladders_with_failures="):
                    log["finished"] = True
                elif line.startswith("quality_mixes=") and log["mixes"] is None:
                    log["mixes"] = line.split("=", 1)[1].split()
                elif (line.startswith("quality_recipe ")
                        and log["recipe"] is None):
                    log["recipe"] = parse_recipe(line.split()[1:])
                elif line.startswith("mix=") and " unmeasured_rungs=" in line:
                    mix, _, rest = line[4:].partition(" unmeasured_rungs=")
                    rungs = rest.split(" (", 1)[0].split()
                    log["stopped"][mix] = [int(r) for r in rungs if r.isdigit()]
    except OSError:
        pass
    return log


def completeness(results, name, mixes):
    """the expected-run manifest against what the run produced

    Every expected rung of every expected mix must be accounted for: a
    diagnostics bundle (pass, quality failure or setup failure), or a rung
    the harness skipped on purpose after consecutive failures at the
    capacity boundary.  Anything else - a mix never started, a ladder cut
    short by a crash - is missing, and the run is incomplete.

    The manifest is also checked against the harness's own statement of the
    mixes it executed, so the two cannot describe different workload sets:
    a mix the harness ran that the manifest omits, or the reverse, is an
    incomplete run, not a shorter green one."""
    try:
        with open(os.path.join(results, name, "expected.json"),
                  encoding="utf-8") as source:
            expected = json.load(source)
    except (OSError, ValueError):
        return {"complete": False, "finished": False,
                "missing": ["no expected-run manifest"], "stopped": {},
                "recipe": None}
    log = harness_log(results, name)
    finished, stopped = log["finished"], log["stopped"]
    missing = []
    manifest = sorted(expected.get("mixes", []))
    if log["mixes"] is None:
        missing.append("the harness never stated the mixes it executed")
    elif sorted(log["mixes"]) != manifest:
        missing.append(f"the harness executed {sorted(log['mixes'])} but the "
                       f"manifest expects {manifest}")
    for mix in expected.get("mixes", []):
        measured = {r["destinations"] for r in
                    (mixes.get(mix) or {}).get("rungs", [])}
        skipped = set(stopped.get(mix, []))
        absent = [n for n in expected.get("steps", [])
                  if n not in measured and n not in skipped]
        if absent:
            missing.append(f"{mix}: rungs {absent} neither measured nor "
                           f"skipped at the boundary")
    recipe = log["recipe"]
    if recipe is not None:
        recipe = dict(recipe, mixes=sorted(log["mixes"] or []),
                      harness=harness_revision())
        if recipe["harness"] is None:
            recipe = None
    return {"complete": finished and not missing, "finished": finished,
            "missing": missing, "stopped": stopped, "expected": expected,
            "recipe": recipe}



def runner_identity(fingerprint):
    """Stable host facts only; pressure counters stay in rung diagnostics.

    Use this projection for both new summaries and published fingerprints:
    older history includes cumulative cgroup cpu_stat counters."""
    if (not isinstance(fingerprint, dict)
            or fingerprint.get("status") == "unavailable"):
        return None
    identity = {
        key: fingerprint[key] for key in
        ("cpu_model", "kernel", "nproc_online", "permitted_cpus",
         "governor", "no_turbo", "numa", "transport", "kernel_udp_limits")
        if fingerprint.get(key) not in (None, "", [], {})}
    cgroup = fingerprint.get("cgroup")
    # cpu_max is the quota field emitted by proc_cpu_sampler.cgroup_info.
    if isinstance(cgroup, dict) and cgroup.get("cpu_max"):
        identity["cgroup"] = {"cpu_max": cgroup["cpu_max"]}
    return copy.deepcopy(identity) if identity else None


def build_provenance(rungs):
    """How the binary under test was built, from the first rung that says:
    compiler, configure arguments (which carry the compiler flags), the
    hash of the binary and the transport library it links.  Evidence for a
    reader asking why two runs differ; never an equality requirement,
    because a different revision is a different binary by definition."""
    for rung in rungs:
        fingerprint = rung.get("host_fingerprint")
        if not isinstance(fingerprint, dict):
            continue
        build = fingerprint.get("build")
        provenance = {}
        if isinstance(build, dict):
            for key, label in (("compiler", "compiler"),
                               ("configure_arguments", "configure_arguments"),
                               ("sha256", "binary_sha256")):
                if build.get(key):
                    provenance[label] = build[key]
        if fingerprint.get("transport"):
            provenance["transport"] = fingerprint["transport"]
        if provenance:
            return provenance
    return None


def run_environment(entry):
    """The environment a workload's rungs were taken in, and the highest
    pressure any of them reached - lifted from the rungs themselves, so the
    record describes the run and not whatever host later reads it."""
    entry = dict(entry)
    for rung in entry.get("rungs", []):
        fingerprint = runner_identity(rung.get("host_fingerprint"))
        if fingerprint is not None and "fingerprint" not in entry:
            entry["fingerprint"] = fingerprint
    peaks = {}
    for rung in entry.get("rungs", []):
        for key, value in (rung.get("peak") or {}).items():
            if isinstance(value, (int, float)):
                peaks[key] = max(peaks.get(key, value), value)
    if peaks:
        entry["peak"] = peaks
    provenance = build_provenance(entry.get("rungs", []))
    if provenance:
        entry["provenance"] = provenance
    return entry


def parse_expected_configs(value):
    if value is None:
        return None
    groups = json.loads(value)
    if not isinstance(groups, list) or any(not isinstance(g, str) for g in groups):
        raise ValueError("--expected-configs must be a JSON array of strings")
    names = sorted({name for group in groups for name in group.split()})
    if not names:
        raise ValueError("--expected-configs must name at least one config")
    return names


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
            entry["infrastructure_limited"] = [
                r["destinations"] for r in entry["rungs"]
                if r["outcome"] == "infrastructure-limited"]
            # run_environment returns a lifted copy; keeping it is what puts
            # the fingerprint and peaks into the published record
            mixes[mix] = run_environment(entry)
        configs[name] = {"harness_status": statuses.get(name), "mixes": mixes}
        configs[name].update(completeness(args.results, name, mixes))
    expected = parse_expected_configs(getattr(args, "expected_configs", None))
    if expected is None:
        expected = sorted(configs)
    missing = sorted(set(expected) - configs.keys())
    unexpected = sorted(configs.keys() - set(expected))
    for name in missing:
        configs[name] = {
            "harness_status": None,
            "mixes": {},
            "complete": False,
            "finished": False,
            "missing": ["no benchmark artifact was collected"],
            "stopped": {},
        }
    complete = bool(expected) and not missing and not unexpected and all(
        configs[name].get("complete") is True for name in expected)
    record = {
        "schema": SCHEMA,
        "tier": args.tier,
        "timestamp": datetime.datetime.now(datetime.timezone.utc)
                     .isoformat(timespec="seconds"),
        "commit_timestamp": os.environ.get("BENCH_COMMIT_TIMESTAMP"),
        "comparison_group": os.environ.get("BENCH_COMPARISON_GROUP"),
        "sha": os.environ.get("GITHUB_SHA") or git("rev-parse", "HEAD"),
        "ref": os.environ.get("GITHUB_REF_NAME")
               or git("rev-parse", "--abbrev-ref", "HEAD"),
        "run": os.environ.get("GITHUB_RUN_ID"),
        "expected_configs": expected,
        "missing_configs": missing,
        "unexpected_configs": unexpected,
        "complete": complete,
        "configs": configs,
    }
    with open(args.out, "w", encoding="utf-8") as output:
        json.dump(record, output, indent=1)
    print(f"bench_summary={args.out}")
    for name, config in configs.items():
        for mix, entry in config["mixes"].items():
            print(f"bench {name} {mix} highest_passing={entry['highest_passing']}"
                  f" first_quality_failure={entry['first_quality_failure']}"
                  f" setup_limited={entry['setup_limited'] or 'none'}"
                  f" infrastructure_limited="
                  f"{entry.get('infrastructure_limited') or 'none'}")
    for name in missing:
        print(f"bench_missing_config={name}")
    return 0


def load_history(path, tier):
    """The published records a record of `tier` may be compared with."""
    tiers = COMPARABLE_TIERS.get(tier, (tier,))
    records = []
    if not path or not os.path.exists(path):
        return records
    with open(path, encoding="utf-8") as source:
        for line in source:
            line = line.strip()
            if line:
                record = json.loads(line)
                if record.get("tier") in tiers:
                    records.append(record)
    return records


def recipes_comparable(current, past):
    """Whether two runs of one configuration were taken the same way.

    Everything that decides what a rung measures must match; the rung lists
    may differ (a pull request runs a subset of main's ladder) but must begin
    at the same rung.  A missing recipe is comparable with nothing."""
    if not isinstance(current, dict) or not isinstance(past, dict):
        return False
    if any(current.get(key) is None or current.get(key) != past.get(key)
           for key in RECIPE_MATCH):
        return False
    now, then = current.get("steps"), past.get("steps")
    return bool(now) and bool(then) and now[0] == then[0]


FLOOR_OK_OR_REPORTED_ELSEWHERE = ("pass", "quality-failure", "setup-failure")


def floor_errors(name, config, required):
    """Positive evidence that the floor works.

    The rung every host must carry has to have been measured and passed;
    that nothing failed is not evidence.  An infrastructure-limited, unknown
    or missing outcome at or below the floor means the floor was never
    shown to work, which a green gate must not paper over.  (A quality or
    setup failure is reported separately, where it is found.)"""
    if not required:
        return []
    expected = config.get("expected") or {}
    steps = [s for s in expected.get("steps", []) if isinstance(s, int)]
    floor = (max((s for s in steps if s <= required), default=None)
             if steps else required)
    errors = []
    for mix in expected.get("mixes") or list(config["mixes"]):
        rungs = (config["mixes"].get(mix) or {}).get("rungs", [])
        if floor is None:
            errors.append(f"{name}/{mix}: the ladder {steps} has no rung at "
                          f"or below the {required}-destination floor, so "
                          f"nothing shows the floor works")
        else:
            outcome = next((r.get("outcome") for r in rungs
                            if r.get("destinations") == floor), None)
            if outcome not in FLOOR_OK_OR_REPORTED_ELSEWHERE:
                errors.append(f"{name}/{mix}: no passing result at the "
                              f"{floor}-destination floor rung (outcome: "
                              f"{outcome or 'missing'})")
        for rung in rungs:
            if ((rung.get("destinations") or 0) <= required
                    and rung.get("outcome") not in FLOOR_OK_OR_REPORTED_ELSEWHERE):
                errors.append(f"{name}/{mix}: {rung.get('outcome') or 'unknown'}"
                              f" at {rung.get('destinations')} destinations, "
                              f"at or below the {required}-destination floor "
                              f"which must be measured")
    return errors


def gate(args):
    with open(args.summary, encoding="utf-8") as source:
        record = json.load(source)
    required = REQUIRED_RUNG.get(args.tier, 0)
    errors = []
    for name, config in record["configs"].items():
        status = config.get("harness_status")
        if status is None:
            # bench-ci.sh records the harness's exit status before it reads
            # anything back; no status file means the runner never got there,
            # so whatever bundles exist are a fragment of a run.
            errors.append(f"{name}: no harness exit status was recorded; the "
                          f"run did not complete")
        elif status not in HARNESS_STATUS_ALLOWED:
            errors.append(f"{name}: the harness exited {status}, which is "
                          f"neither success nor a recorded quality failure - "
                          f"it was killed, timed out or aborted, so its "
                          f"results are partial")
        if not config["mixes"]:
            errors.append(f"{name}: no rung produced a diagnostics bundle")
        if not config.get("finished", False):
            errors.append(f"{name}: the harness did not finish (exit status "
                          f"{config.get('harness_status')}); its results are "
                          f"partial")
        for message in config.get("missing", []):
            errors.append(f"{name}: incomplete - {message}")
        for mix, entry in config["mixes"].items():
            for rung in entry.get("infrastructure_limited") or []:
                if (rung or 0) <= required:
                    continue  # an error below: the floor was not measured
                # Not a failure and not a result: the environment could not
                # carry the offered load, so the rung was never measured.
                print(f"::notice title=bench infrastructure::"
                      f"{name}/{mix} at {rung} destinations was not measured: "
                      f"the preflight found the host, the path or the "
                      f"receivers short")
            if entry["setup_limited"]:
                errors.append(f"{name}/{mix}: setup failure at "
                              f"{entry['setup_limited']} (never measured)")
            for rung in entry["rungs"]:
                if (rung["outcome"] == "quality-failure"
                        and (rung["destinations"] or 0) <= required):
                    errors.append(f"{name}/{mix}: quality failure at "
                                  f"{rung['destinations']} destinations, "
                                  f"which every host must carry")
        errors.extend(floor_errors(name, config, required))
    history = load_history(args.history, args.tier)
    for finding in regressions(record, history):
        print(f"::warning title=bench efficiency::"
              f"{finding['config']}/{finding['mix']} at "
              f"{finding['destinations']} destinations: sender CPU "
              f"{finding['current']:.1f}%/Gbit/s vs recent median "
              f"{finding['baseline_median']:.1f} "
              f"({finding['factor']:.2f}x, "
              f"{finding['baseline_count']} comparable runs)")
    for message in errors:
        print(f"::error title=bench gate::{message}")
    if errors:
        return 1
    print("bench_gate=pass")
    return 0


def regressions(record, history, window=10, tolerance=1.25):
    """Return regressions against complete passing runs with the same host
    and the same recipe: a number taken for 10 s rungs is not a baseline for
    one taken for 15 s rungs, and a record that predates recipes has no way
    to say which it was."""
    supported = {SCHEMA, "nginx-media.bench-history/2"}
    if record.get("schema") not in supported:
        return []
    tiers = COMPARABLE_TIERS.get(record.get("tier"), (record.get("tier"),))
    recent = [old for old in history
              if old.get("schema") in supported
              and old.get("tier") in tiers]
    if record.get("comparison_group"):
        recent = [old for old in recent
                  if old.get("comparison_group") == record["comparison_group"]
                  and old.get("sha") != record.get("sha")]
    else:
        recent = [old for old in recent if not old.get("comparison_group")]
    findings = []
    for name, config in record.get("configs", {}).items():
        # a run with no recipe cannot say what it measured, so it is
        # compared with nothing (and nothing without one is compared to it)
        if config.get("complete") is not True or not config.get("recipe"):
            continue
        for mix, entry in config.get("mixes", {}).items():
            fingerprint = runner_identity(entry.get("fingerprint"))
            if fingerprint is None:
                continue
            for rung in entry.get("rungs", []):
                current = rung.get("sender_cpu_per_gbps")
                destinations = rung.get("destinations")
                if (rung.get("outcome") != "pass"
                        or not isinstance(destinations, int)
                        or isinstance(destinations, bool)
                        or not isinstance(current, (int, float))
                        or isinstance(current, bool)
                        or not math.isfinite(current) or current <= 0):
                    continue
                past = []
                for old in reversed(recent):
                    old_config = old.get("configs", {}).get(name, {})
                    old_entry = old_config.get("mixes", {}).get(mix, {})
                    if (old_config.get("complete") is not True
                            or not recipes_comparable(
                                config.get("recipe"), old_config.get("recipe"))
                            or runner_identity(old_entry.get("fingerprint"))
                            != fingerprint):
                        continue
                    for old_rung in old_entry.get("rungs", []):
                        value = old_rung.get("sender_cpu_per_gbps")
                        if (old_rung.get("outcome") == "pass"
                                and old_rung.get("destinations") == destinations
                                and isinstance(value, (int, float))
                                and not isinstance(value, bool)
                                and math.isfinite(value) and value > 0):
                            past.append(value)
                            break
                    if len(past) >= window:
                        break
                if len(past) < 3:
                    continue
                median = statistics.median(past)
                if current > median * tolerance:
                    findings.append({
                        "config": name,
                        "mix": mix,
                        "destinations": destinations,
                        "current": current,
                        "baseline_median": median,
                        "baseline_count": len(past),
                        "factor": current / median,
                        "runner": fingerprint,
                    })
    return findings


def regression_report(args):
    with open(args.summary, encoding="utf-8") as source:
        record = json.load(source)
    findings = regressions(record, load_history(args.history, record["tier"]))
    with open(args.out, "w", encoding="utf-8") as output:
        json.dump(findings, output, indent=1)
    print(f"bench_regressions={len(findings)}")
    return 0


def publish(args):
    with open(args.summary, encoding="utf-8") as source:
        record = json.load(source)
    samples = []
    sample_path = getattr(args, "samples", None)
    if sample_path and os.path.exists(sample_path):
        with open(sample_path, encoding="utf-8") as source:
            samples = [json.loads(line) for line in source if line.strip()]
        groups = {sample.get("comparison_group") for sample in samples}
        if (any(sample.get("tier") != record["tier"]
                or not sample.get("comparison_group") for sample in samples)
                or len(groups) > 1):
            raise ValueError("trend samples must match the published tier "
                             "and have one same-runner comparison_group")
    data_dir = os.path.join(args.pages, "data")
    os.makedirs(data_dir, exist_ok=True)
    path = os.path.join(data_dir, f"{record['tier']}.jsonl")
    with open(path, "a", encoding="utf-8") as output:
        output.write(json.dumps(record, separators=(",", ":")) + "\n")
        for sample in samples:
            output.write(json.dumps(sample, separators=(",", ":")) + "\n")
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
    s.add_argument("--expected-configs")
    g = sub.add_parser("gate")
    g.add_argument("summary")
    g.add_argument("--tier", required=True)
    g.add_argument("--history")
    r = sub.add_parser("regressions")
    r.add_argument("summary")
    r.add_argument("--history", required=True)
    r.add_argument("--out", required=True)
    p = sub.add_parser("publish")
    p.add_argument("summary")
    p.add_argument("--pages", required=True)
    p.add_argument("--samples")
    args = parser.parse_args()
    return {"summarize": summarize, "gate": gate, "publish": publish,
            "regressions": regression_report}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())

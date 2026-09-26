#!/usr/bin/env python3
"""Build one machine-readable diagnostics bundle for a capacity rung.

Reads the artifacts a capacity case leaves in its directory (thread CPU
snapshots, host/network snapshots, per-worker Prometheus scrapes, receiver
interval CSVs and the per-protocol quality reports) and writes
diagnostics.json.  Every measurement is an object

    {"value": ..., "unit": "...", "status": "ok"}

or, when it could not be collected,

    {"value": null, "unit": "...", "status": "unavailable", "reason": "..."}

so a missing field is never mistaken for a zero.  The CPU measurement
interval, the media measurement interval and the queue sampling interval are
recorded separately because they are different intervals.

  capacity_diagnostics.py build <case dir> --meta key=value ...
  capacity_diagnostics.py matrix <run dir> [--out capacity-matrix.json]
"""

import argparse
import csv
import glob
import json
import math
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proc_cpu_sampler  # noqa: E402

LABEL_RE = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:\\.|[^"])*)"')
SAMPLE_RE = re.compile(
    r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{(.*?)\})?\s+"
    r"([-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)"
)


def ok(value, unit, **extra):
    field = {"value": value, "unit": unit, "status": "ok"}
    field.update(extra)
    return field


def missing(unit, reason):
    return {"value": None, "unit": unit, "status": "unavailable",
            "reason": reason}


def load_json(path):
    try:
        with open(path, encoding="utf-8") as source:
            return json.load(source)
    except (OSError, ValueError):
        return None


def load_kv(path):
    values = {}
    try:
        with open(path, encoding="utf-8") as source:
            for line in source:
                line = line.strip()
                if "=" in line and not line.startswith("#"):
                    key, value = line.split("=", 1)
                    if key not in values:
                        values[key] = value
    except OSError:
        return None
    return values


def load_csv(path):
    try:
        with open(path, newline="", encoding="utf-8") as source:
            return list(csv.DictReader(source))
    except OSError:
        return None


def parse_metrics(path):
    samples = []
    try:
        with open(path, encoding="utf-8") as source:
            for line in source:
                if line.startswith("#"):
                    continue
                match = SAMPLE_RE.match(line.strip())
                if not match:
                    continue
                labels = dict(LABEL_RE.findall(match.group(2) or ""))
                samples.append((match.group(1), labels, float(match.group(3))))
    except OSError:
        return None
    return samples


def metric_sum(samples, name, **match):
    total = None
    for metric, labels, value in samples or ():
        if metric != name:
            continue
        if any(labels.get(k) != v for k, v in match.items()):
            continue
        total = (total or 0.0) + value
    return total


def metric_values(samples, name):
    return [(labels, value) for metric, labels, value in samples or ()
            if metric == name]


def as_number(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return int(number) if number.is_integer() else number


def parse_cpu_list(text):
    cpus = set()
    if not text:
        return cpus
    for part in text.split(","):
        part = part.strip()
        if "-" in part:
            low, high = part.split("-", 1)
            cpus.update(range(int(low), int(high) + 1))
        elif part:
            cpus.add(int(part))
    return cpus


def read_processes(case_dir):
    rows = load_csv(os.path.join(case_dir, "processes.tsv").replace(
        ".tsv", ".tsv"))
    processes = {}
    path = os.path.join(case_dir, "processes.tsv")
    try:
        with open(path, encoding="utf-8") as source:
            reader = csv.DictReader(source, delimiter="\t")
            for row in reader:
                processes[row["pid"]] = (row["kind"], row["label"])
    except OSError:
        pass
    del rows
    return processes


def cpu_section(case_dir, processes, host_after):
    before = os.path.join(case_dir, "cpu.before")
    after = os.path.join(case_dir, "cpu.after")
    if not (os.path.exists(before) and os.path.exists(after)):
        return {"status": "unavailable",
                "reason": "no CPU snapshots in case directory"}, None
    totals, acct = proc_cpu_sampler.cpu_deltas(before, after)
    by_kind = {}
    by_process = {}
    roles = {}
    for (pid, role), pct in totals.items():
        kind, label = processes.get(pid, ("unknown", f"pid{pid}"))
        by_kind[kind] = by_kind.get(kind, 0.0) + pct
        key = f"{label}:{pid}" if kind == "publisher" else label
        entry = by_process.setdefault(key, {"pid": int(pid), "kind": kind,
                                            "total_pct_of_core": 0.0,
                                            "roles": {}})
        entry["total_pct_of_core"] += pct
        entry["roles"][role] = entry["roles"].get(role, 0.0) + pct
        if kind == "worker":
            roles.setdefault(label, {})[role] = pct

    permitted = None
    if host_after:
        permitted = len(host_after.get("host", {}).get("sampler_affinity")
                        or []) or None
    total = sum(by_kind.values())
    section = {
        "unit_note": "percent of one core; 100 = one fully busy core",
        "interval": ok(acct["window_s"], "s"),
        "clk_tck": ok(acct["clk_tck"], "ticks/s"),
        "thread_accounting": ok(acct, "threads"),
        "by_kind_pct_of_core": ok({k: round(v, 2) for k, v in by_kind.items()},
                                  "%core"),
        "sampled_total_pct_of_core": ok(round(total, 2), "%core"),
    }
    if permitted:
        section["sampled_total_pct_of_permitted"] = ok(
            round(total / permitted, 2), "%of permitted CPUs",
            permitted_cpus=permitted)
    else:
        section["sampled_total_pct_of_permitted"] = missing(
            "%", "permitted CPU set not captured")
    workers = {}
    for label, role_map in roles.items():
        srt_senders = {r: v for r, v in role_map.items()
                       if r.startswith("srt-egress-")}
        active = {r: v for r, v in srt_senders.items() if v >= 1.0}
        workers[label] = {
            "total": ok(round(sum(role_map.values()), 2), "%core"),
            "event_loop": _role(role_map, "worker",
                                "event-loop thread not labelled (no gdb?)"),
            "srt_senders_individual": ok(
                {r: round(v, 2) for r, v in sorted(srt_senders.items())},
                "%core") if srt_senders else missing("%core",
                                                     "no srt-egress threads"),
            "srt_senders_combined": ok(round(sum(srt_senders.values()), 2),
                                       "%core"),
            "srt_senders_busy_ge_1pct": ok(len(active), "threads"),
            "libsrt_sndq": _role(role_map, "SRT:SndQ", "no SRT:SndQ threads"),
            "libsrt_rcvq": _role(role_map, "SRT:RcvQ", "no SRT:RcvQ threads"),
            "libsrt_tsbpd": _role(role_map, "SRT:TsbPd",
                                  "no SRT:TsbPd threads"),
            "hls_uploaders": _role(role_map, "hls-push",
                                   "no hls-push threads"),
            "other": ok(round(sum(v for r, v in role_map.items()
                                  if not (r.startswith("srt-egress-")
                                          or r in ("worker", "SRT:SndQ",
                                                   "SRT:RcvQ", "SRT:TsbPd",
                                                   "hls-push"))), 2),
                        "%core"),
        }
    section["workers"] = workers
    section["processes"] = {
        k: {**v, "total_pct_of_core": round(v["total_pct_of_core"], 2),
            "roles": {r: round(p, 2) for r, p in v["roles"].items()}}
        for k, v in by_process.items()}
    return section, total


def _role(role_map, role, reason):
    if role in role_map:
        return ok(round(role_map[role], 2), "%core")
    return missing("%core", reason)


def environment_section(host_before, host_after, processes):
    if not host_after:
        return {"status": "unavailable",
                "reason": "no extended host snapshot"}
    host = host_after["host"]
    section = {
        "online_cpus": ok(host.get("nproc_online"), "cpus"),
        "permitted_cpus": ok(host.get("sampler_affinity"), "cpu ids"),
        "loadavg": ok(host.get("loadavg"), "text"),
    }
    cgroup = host.get("cgroup") or {}
    if cgroup.get("cpu_max"):
        quota, _, period = cgroup["cpu_max"].partition(" ")
        if quota == "max":
            section["cgroup_cpu_quota"] = ok(None, "cpus",
                                             note="unlimited (cpu.max=max)")
        else:
            section["cgroup_cpu_quota"] = ok(
                round(int(quota) / int(period), 3), "cpus")
    else:
        section["cgroup_cpu_quota"] = missing("cpus", "cpu.max not readable")
    stat_b = ((host_before or {}).get("host", {}).get("cgroup") or {}).get(
        "cpu_stat") or {}
    stat_a = cgroup.get("cpu_stat") or {}
    if "nr_throttled" in stat_a and "nr_throttled" in stat_b:
        section["cgroup_throttled_periods"] = ok(
            stat_a["nr_throttled"] - stat_b["nr_throttled"], "periods")
        section["cgroup_throttled_time"] = ok(
            (stat_a.get("throttled_usec", 0) - stat_b.get("throttled_usec", 0))
            / 1e6, "s")
    else:
        section["cgroup_throttled_periods"] = missing(
            "periods", "cgroup cpu.stat not readable")
    freqs = host.get("cpu_freq_khz") or {}
    section["cpu_frequency"] = (ok(freqs, "kHz") if freqs else missing(
        "kHz", "cpufreq not exposed (VM or container)"))
    cpu_b = (host_before or {}).get("host", {}).get("proc_stat_cpu")
    cpu_a = host.get("proc_stat_cpu")
    if cpu_a and cpu_b:
        delta = {k: cpu_a[k] - cpu_b.get(k, 0) for k in cpu_a}
        total = sum(delta[k] for k in ("user", "nice", "system", "idle",
                                       "iowait", "irq", "softirq", "steal"))
        if total > 0:
            section["host_cpu_busy_fraction"] = ok(
                round(1 - (delta["idle"] + delta["iowait"]) / total, 4),
                "fraction of all online CPUs")
            section["host_cpu_steal_fraction"] = ok(
                round(delta["steal"] / total, 4), "fraction")
            section["host_softirq_fraction"] = ok(
                round(delta["softirq"] / total, 4), "fraction")
    threads = host_after.get("threads") or {}
    affinity = {}
    for tid, info in threads.items():
        kind, label = processes.get(str(info.get("pid")), ("unknown", "?"))
        key = f"{label}/{info.get('role')}"
        affinity.setdefault(key, set()).add(info.get("cpus_allowed"))
    section["thread_affinity"] = ok(
        {k: sorted(x for x in v if x) for k, v in sorted(affinity.items())},
        "cpu list")
    section["threads_per_process"] = ok(
        {processes.get(pid, ("?", f"pid{pid}"))[1] + f":{pid}": p.get("threads")
         for pid, p in host_after.get("processes", {}).items()}, "threads")
    section["nonvoluntary_ctxt_switches"] = ok(
        _ctx_delta(host_before, host_after, processes), "switches")
    return section


def _ctx_delta(before, after, processes):
    result = {}
    for pid, p in (after or {}).get("processes", {}).items():
        b = (before or {}).get("processes", {}).get(pid, {})
        if p.get("nonvoluntary_ctxt_switches") is None:
            continue
        label = processes.get(pid, ("?", f"pid{pid}"))[1] + f":{pid}"
        result[label] = (p["nonvoluntary_ctxt_switches"]
                         - (b.get("nonvoluntary_ctxt_switches") or 0))
    return result


def network_section(host_before, host_after, processes):
    if not (host_before and host_after):
        return {"status": "unavailable", "reason": "no extended snapshots"}
    snmp_b = host_before["net"]["snmp"]
    snmp_a = host_after["net"]["snmp"]

    def delta(key):
        if key in snmp_a and key in snmp_b:
            return ok(snmp_a[key] - snmp_b[key], "count")
        return missing("count", f"{key} not in /proc/net/snmp")

    section = {
        "udp_in_datagrams": delta("Udp.InDatagrams"),
        "udp_out_datagrams": delta("Udp.OutDatagrams"),
        "udp_rcvbuf_errors": delta("Udp.RcvbufErrors"),
        "udp_sndbuf_errors": delta("Udp.SndbufErrors"),
        "udp_in_errors": delta("Udp.InErrors"),
        "tcp_retrans_segs": delta("Tcp.RetransSegs"),
    }
    ns_b = host_before["net"]["netstat"]
    ns_a = host_after["net"]["netstat"]
    for key in ("TcpExt.ListenDrops", "TcpExt.TCPBacklogDrop"):
        if key in ns_a and key in ns_b:
            section[key.split(".")[1]] = ok(ns_a[key] - ns_b[key], "count")
    sd_b = host_before["net"].get("softnet_dropped")
    sd_a = host_after["net"].get("softnet_dropped")
    section["softnet_dropped"] = (ok(sd_a - sd_b, "count")
                                  if sd_a is not None and sd_b is not None
                                  else missing("count", "softnet_stat"))
    per_pid = {}
    sockets_b = host_before["net"].get("udp_sockets_by_pid", {})
    for pid, row in host_after["net"].get("udp_sockets_by_pid", {}).items():
        prior = sockets_b.get(pid, {})
        kind, label = processes.get(pid, ("unknown", f"pid{pid}"))
        per_pid[f"{label}:{pid}"] = {
            "kind": kind,
            "udp_sockets": row["sockets"],
            "socket_drops_delta": row["drops"] - prior.get("drops", 0),
            "rx_queue_bytes": row["rx_queue_bytes"],
        }
    section["udp_socket_drops_by_process"] = ok(per_pid, "datagrams",
                                               note="/proc/net/udp drops "
                                               "column; sockets counted "
                                               "only if alive at both ends")
    return section


def srt_section(case_dir, workers_before, workers_after):
    samples = load_csv(os.path.join(case_dir, "queue-samples.csv"))
    after_all = [m for m in workers_after.values() if m]
    before_all = [m for m in workers_before.values() if m]
    if not after_all or metric_sum(sum(after_all, []),
                                   "nginx_media_srt_egress_shard_destinations") in (None, 0):
        return {"status": "unavailable", "reason": "no SRT destinations"}
    section = {}

    def delta(name):
        a = metric_sum(sum(after_all, []), name)
        b = metric_sum(sum(before_all, []), name)
        if a is None or b is None:
            return missing("count", f"{name} not exported")
        return ok(int(a - b), "count")

    section["sent_bytes"] = delta(
        "nginx_media_srt_egress_shard_sent_bytes_total")
    section["blocked_sends"] = delta(
        "nginx_media_srt_egress_shard_blocked_sends_total")
    section["retransmitted_packets"] = delta(
        "nginx_media_srt_egress_shard_retransmitted_packets_total")
    section["feed_drops"] = delta(
        "nginx_media_srt_egress_shard_feed_queue_dropped_total")
    section["destination_drops"] = delta(
        "nginx_media_srt_egress_shard_output_dropped_total")
    section["transport_errors"] = delta(
        "nginx_media_egress_transport_errors_total")
    active = [v for m in after_all for labels, v in
              metric_values(m, "nginx_media_egress_active_workers")
              if labels.get("engine") == "srt_shard"]
    section["active_senders_end"] = (ok(max(active), "threads")
                                     if active else missing("threads",
                                                            "not exported"))
    lags = [v for m in after_all for labels, v in
            metric_values(m, "nginx_media_egress_queue_lag_ms")
            if labels.get("protocol") == "srt"]
    section["enqueue_to_send_lag_end"] = (
        ok({"max": max(lags), "median": statistics.median(lags)}, "ms")
        if lags else missing("ms", "no per-destination lag rows"))
    if samples:
        by_round = {}
        per_lane = {}
        for row in samples:
            rnd = by_round.setdefault(row["round_id"], {
                "ns": int(row["sample_ns"]), "feed": 0.0, "out": 0.0})
            rnd["feed"] += float(row["feed_queue_units"] or 0)
            rnd["out"] += float(row["output_queue_units"] or 0)
            # A lane is a shard *of one worker*: every worker numbers its
            # shards from zero, so keying by shard alone merges different
            # lanes' counters into one series and the rate it reports is
            # whichever worker's last sample happened to be read last - a
            # lane serving 156 Mbit/s showed as 0 that way.
            lane = per_lane.setdefault(
                f"w{row['worker']}:s{row['shard']}", [])
            lane.append((int(row["sample_ns"]), float(row["sent_bytes"] or 0)))
        rounds = sorted(by_round.values(), key=lambda r: r["ns"])
        section["queue_timeline"] = ok(
            [{"t_s": round((r["ns"] - rounds[0]["ns"]) / 1e9, 3),
              "feed_units": r["feed"], "output_units": r["out"]}
             for r in rounds], "units")
        rates = {}
        for lane, points in per_lane.items():
            points.sort()
            if len(points) >= 2 and points[-1][0] > points[0][0]:
                rates[lane] = round((points[-1][1] - points[0][1]) * 8
                                    / ((points[-1][0] - points[0][0]) / 1e9))
        section["per_lane_service_rate"] = ok(rates, "bit/s")
        section["queue_sampling_interval"] = ok(
            round((rounds[-1]["ns"] - rounds[0]["ns"]) / 1e9
                  / max(1, len(rounds) - 1), 3) if len(rounds) > 1 else None,
            "s")
    else:
        section["queue_timeline"] = missing("units", "no queue-samples.csv")
    return section


def rtmp_section(case_dir, workers_before, workers_after):
    after = [m for m in workers_after.values() if m]
    before = [m for m in workers_before.values() if m]
    flat_a, flat_b = sum(after, []), sum(before, [])
    visits = metric_sum(flat_a, "nginx_media_rtmp_scheduler_destinations_visited_total")
    if visits is None:
        return {"status": "unavailable", "reason": "no RTMP scheduler metrics"}

    def delta(name, unit="count"):
        a, b = metric_sum(flat_a, name), metric_sum(flat_b, name)
        if a is None or b is None:
            return missing(unit, f"{name} not exported")
        return ok(a - b, unit)

    section = {
        "destinations_visited": delta(
            "nginx_media_rtmp_scheduler_destinations_visited_total"),
        "units_pumped": delta("nginx_media_rtmp_scheduler_units_pumped_total"),
        "bytes_queued": delta("nginx_media_rtmp_scheduler_bytes_queued_total",
                              "bytes"),
        "service_time": delta("nginx_media_rtmp_scheduler_service_us_total",
                              "us"),
        "reposts": delta("nginx_media_rtmp_scheduler_reposts_total"),
    }
    for name, key in (("nginx_media_rtmp_scheduler_visit_service_us",
                       "last_visit_service_us"),
                      ("nginx_media_rtmp_scheduler_visit_destinations",
                       "last_visit_destinations"),
                      ("nginx_media_rtmp_scheduler_visit_units_pumped",
                       "last_visit_units"),
                      ("nginx_media_rtmp_scheduler_visit_bytes_queued",
                       "last_visit_bytes")):
        value = metric_sum(flat_a, name)
        section[key] = ok(value, "") if value is not None else missing(
            "", f"{name} not exported")
    quality = load_kv(os.path.join(case_dir, "rtmp-quality.txt")) or {}
    for key in ("quality_runnable_destinations", "quality_write_blocked_destinations"):
        if key in quality:
            section[key] = ok(as_number(quality[key]), "destinations")
    return section


def hls_push_section(case_dir, workers_before, workers_after):
    quality = load_kv(os.path.join(case_dir, "hls-push-quality.txt"))
    if quality is None:
        return {"status": "unavailable", "reason": "no HLS push destinations"}
    section = {k: ok(as_number(v) if as_number(v) is not None else v, "")
               for k, v in quality.items() if not k.startswith("quality_failure=")}
    active = [v for m in workers_after.values() if m for labels, v in
              metric_values(m, "nginx_media_egress_active_workers")
              if labels.get("engine") == "hls_upload_pool"]
    section["active_uploaders_end"] = (ok(max(active), "threads") if active
                                       else missing("threads", "not exported"))
    mark = load_json(os.path.join(case_dir, "hls-push-readiness.json"))
    section["first_segment_latency_all"] = (
        ok(mark.get("elapsed_s"), "s") if mark else
        missing("s", "no readiness progress"))
    return section


def hls_reader_stats(case_dir):
    """Observed HLS-origin delivery, straight from the per-reader CSV.

    hls-reader.log reports the configured threshold in `min_delivery_ratio`;
    the measurement is the readers' own `reference_ratio`, which is why the
    CSV is the source of the observed numbers and not the log."""
    rows = load_csv(os.path.join(case_dir, "hls-readers.csv"))
    if not rows:
        return None
    ratios, durations, byte_total, unreadable = [], [], 0, 0
    for row in rows:
        try:
            ratios.append(float(row["reference_ratio"]))
            byte_total += int(float(row["bytes_received"]))
            durations.append(float(row["duration_s"]))
        except (KeyError, TypeError, ValueError):
            unreadable += 1
    if not ratios:
        return None
    ratios.sort()
    duration = max(durations) if durations else 0.0
    return {
        "observed_min_delivery_ratio": round(ratios[0], 6),
        "observed_p5_delivery_ratio": round(ratios[percentile_index(ratios, 0.05)], 6),
        "observed_p50_delivery_ratio": round(ratios[percentile_index(ratios, 0.50)], 6),
        "observed_p95_delivery_ratio": round(ratios[percentile_index(ratios, 0.95)], 6),
        "observed_ratio_basis": "reader-counted-bytes",
        "receiver_bytes_total": byte_total,
        "receiver_measurement_s": round(duration, 6),
        "delivered_gbps": (round(byte_total * 8 / duration / 1e9, 6)
                           if duration > 0 else None),
        "readers": len(ratios),
        "unreadable_reader_rows": unreadable,
    }


def percentile_index(values, fraction):
    rank = max(1, math.ceil(fraction * len(values)))
    return min(rank, len(values)) - 1


def merge_hls_reader_csv(section, case_dir):
    """Fold the per-reader observations into the reader entry.

    The log is the primary record and the CSV must agree with it; the observed
    fields are added only where the log does not already carry them, so a log
    written by the fixed benchmark is never overwritten by the CSV."""
    stats = hls_reader_stats(case_dir)
    if stats is None:
        return
    entry = section.get("hls_readers")
    if entry is None:
        section["hls_readers"] = ok(stats, "mixed", sources=["hls-readers.csv"])
        return
    values = entry.get("value")
    if not isinstance(values, dict):
        return
    added = False
    for key, value in stats.items():
        if key not in values:
            values[key] = value
            added = True
    if added:
        entry.setdefault("sources", []).append("hls-readers.csv")


def delivery_section(case_dir):
    section = {}
    for name, path in (("srt", "quality-report.txt"),
                       ("rtmp", "rtmp-quality.txt"),
                       ("hls_push", "hls-push-quality.txt"),
                       ("hls_readers", "hls-reader.log")):
        values = load_kv(os.path.join(case_dir, path))
        if values is None:
            continue
        keep = {}
        for key, value in values.items():
            if key.startswith("quality_failure") or key == "quality_failure":
                continue
            number = as_number(value)
            keep[key] = number if number is not None else value
        failures = []
        try:
            with open(os.path.join(case_dir, path), encoding="utf-8") as src:
                failures = [line.split("=", 1)[1].strip() for line in src
                            if line.startswith("quality_failure=")]
        except OSError:
            pass
        keep["failures_sample"] = failures[:20]
        keep["failures_total"] = len(failures)
        if name == "hls_readers" and "min_delivery_ratio" in keep:
            # The reader log's min_delivery_ratio is the configured gate, not
            # a measurement; say so in the field name as well as keeping the
            # original one, so nothing downstream has to guess.
            keep.setdefault("delivery_ratio_threshold",
                            keep["min_delivery_ratio"])
        section[name] = ok(keep, "mixed")
    merge_hls_reader_csv(section, case_dir)
    intervals = load_csv(os.path.join(case_dir, "interval-throughput.csv"))
    if intervals:
        ratios = [float(r["reference_ratio"]) for r in intervals
                  if r.get("kind") == "sample" and r.get("reference_ratio")]
        if ratios:
            ratios.sort()
            section["srt_short_interval_ratio"] = ok({
                "min": ratios[0],
                "p01": ratios[int(len(ratios) * 0.01)],
                "p50": ratios[len(ratios) // 2],
                "samples": len(ratios)}, "ratio of reference")
    if not section:
        return {"status": "unavailable", "reason": "no quality reports"}
    return section


def resources_section(case_dir, cpu_total, host_after):
    section = {}
    memory = {}
    for which in ("before", "after"):
        try:
            with open(os.path.join(case_dir, f"memory.{which}"),
                      encoding="utf-8") as source:
                for line in source:
                    pid, rss, pss = line.split()[:3]
                    memory.setdefault(pid, {})[which] = {"rss_kb": int(rss),
                                                         "pss_kb": int(pss)}
        except OSError:
            pass
    section["worker_memory"] = (ok(memory, "kB") if memory
                                else missing("kB", "no memory snapshots"))
    sock = {}
    for which in ("before", "after"):
        try:
            with open(os.path.join(case_dir, f"{which}.sockstat"),
                      encoding="utf-8") as source:
                sock[which] = source.read().strip()
        except OSError:
            pass
    section["sockstat"] = ok(sock, "text") if sock else missing(
        "text", "no sockstat snapshots")
    if host_after and host_after.get("sampler_cpu_ms"):
        section["cpu_sampling_overhead"] = ok(host_after["sampler_cpu_ms"],
                                              "ms of sampler CPU per snapshot")
        section["extended_snapshot_capture"] = ok(host_after.get("capture_ms"),
                                                  "ms")
    return section


def saturation_summary(cpu, env, net):
    """Plain statements about which side saturated first; no thresholds that
    depend on the host - only relations between measured quantities."""
    notes = []
    busy = env.get("host_cpu_busy_fraction", {}).get("value") if isinstance(env, dict) else None
    if busy is not None:
        notes.append(f"host CPU busy {busy:.1%} of all online CPUs")
    if isinstance(net, dict):
        drops = net.get("udp_socket_drops_by_process", {}).get("value") or {}
        worst = sorted(drops.items(), key=lambda kv: -kv[1]["socket_drops_delta"])
        for label, row in worst[:3]:
            if row["socket_drops_delta"] > 0:
                notes.append(f"UDP socket drops in {label} ({row['kind']}): "
                             f"{row['socket_drops_delta']} across "
                             f"{row['udp_sockets']} sockets")
    if isinstance(cpu, dict):
        kinds = (cpu.get("by_kind_pct_of_core") or {}).get("value") or {}
        senders = kinds.get("worker")
        receivers = sum(v for k, v in kinds.items()
                        if k.endswith("receiver") or k == "hls-readers")
        if senders is not None and receivers:
            notes.append(f"sender {senders:.0f}% vs receivers "
                         f"{receivers:.0f}% of one core")
        for label, worker in (cpu.get("workers") or {}).items():
            for role in ("libsrt_sndq", "srt_senders_combined", "event_loop"):
                value = worker.get(role, {}).get("value")
                if value is not None and value >= 80:
                    notes.append(f"{label} {role} at {value:.0f}% of one core")
    return notes


def efficiency_section(bundle):
    """CPU per delivered Gbit/s.  Far less host-bound than a destination
    count - a wider host moves the capacity boundary, not this ratio - but
    not host-independent: CPU microarchitecture and frequency, kernel, SRT
    library version, compiler and IRQ/NIC topology all move it, so history
    is compared within one runner class.

    Delivered means counted by the receivers, never by the sender: a sender
    counter (bytes queued, bytes written) keeps counting what a congested or
    failing receiver never got, which would make a failing rung look cheap.
    Each protocol's bytes are divided by that protocol's own receiver-side
    measurement duration when it reports one."""
    shell_seconds = (bundle["intervals"]["media_measurement"] or {}).get("value")
    cpu = bundle.get("cpu") or {}
    kinds = (cpu.get("by_kind_pct_of_core") or {}).get("value") or {}
    delivery = bundle.get("delivery") or {}
    gbps = 0.0
    sources = []
    for proto, keys, seconds_keys in (
            ("srt", ("quality_total_receiver_bytes",),
             ("quality_measurement_s",)),
            ("rtmp", ("quality_aggregate_bytes",),
             ("quality_measurement_s",)),
            ("hls_push", ("quality_total_receiver_bytes", "quality_total_bytes"),
             ("quality_measurement_s",)),
            ("hls_readers", ("receiver_bytes_total",),
             ("receiver_measurement_s",))):
        report = (delivery.get(proto) or {}).get("value") or {}
        for key in keys:
            value = report.get(key)
            if isinstance(value, (int, float)) and value > 0:
                seconds = next((report.get(k) for k in seconds_keys
                                if isinstance(report.get(k), (int, float))
                                and report.get(k) > 0), None)
                basis = "receiver window"
                if seconds is None:
                    seconds, basis = shell_seconds, "shell window"
                if not seconds:
                    break
                gbps += value * 8 / seconds / 1e9
                sources.append(f"{proto} {key} over the {basis}")
                break
    if gbps <= 0:
        return {"status": "unavailable",
                "reason": "no receiver-counted byte total or measurement interval"}
    result = {"delivered": ok(round(gbps, 4), "Gbit/s", sources=sources)}
    receivers = sum(v for k, v in kinds.items()
                    if k.endswith("receiver") or k == "hls-readers")
    if "worker" in kinds:
        result["sender_cpu_per_gbps"] = ok(round(kinds["worker"] / gbps, 2),
                                           "%core per Gbit/s")
    if receivers:
        result["receiver_cpu_per_gbps"] = ok(round(receivers / gbps, 2),
                                             "%core per Gbit/s")
    return result


def build(args):
    case_dir = args.case_dir
    meta = {}
    for item in args.meta or ():
        key, _, value = item.partition("=")
        number = as_number(value)
        meta[key] = number if number is not None and value.strip() != "" else value
    processes = read_processes(case_dir)
    host_before = load_json(os.path.join(case_dir, "host.before.json"))
    host_after = load_json(os.path.join(case_dir, "host.after.json"))
    workers_before = {os.path.basename(p).split(".")[-1]: parse_metrics(p)
                      for p in glob.glob(os.path.join(case_dir, "workers.before.*"))}
    workers_after = {os.path.basename(p).split(".")[-1]: parse_metrics(p)
                     for p in glob.glob(os.path.join(case_dir, "workers.after.*"))}
    cpu, cpu_total = cpu_section(case_dir, processes, host_after)
    env = environment_section(host_before, host_after, processes)
    net = network_section(host_before, host_after, processes)
    event_loop = {}
    for pid, samples in workers_after.items():
        label = processes.get(pid, ("worker", f"pid{pid}"))[1]
        delay = metric_sum(samples, "nginx_media_worker_event_loop_max_delay_ms")
        event_loop[label] = delay
    bundle = {
        "schema": "nginx-media.capacity-diagnostics/2",
        "case": meta,
        "outcome": meta.get("outcome", "unknown"),
        "intervals": {
            "media_measurement": ok(meta.get("measurement_s"), "s"),
            "cpu": cpu.get("interval", missing("s", "no CPU window"))
            if isinstance(cpu, dict) else missing("s", "no CPU window"),
            "requested": ok(meta.get("requested_s"), "s"),
        },
        "cpu": cpu,
        "cpu_environment": env,
        "network": net,
        "event_loop_max_delay": ok(event_loop, "ms") if event_loop else
        missing("ms", "no worker metrics"),
        "srt": srt_section(case_dir, workers_before, workers_after),
        "rtmp": rtmp_section(case_dir, workers_before, workers_after),
        "hls_push": hls_push_section(case_dir, workers_before, workers_after),
        "delivery": delivery_section(case_dir),
        "resources": resources_section(case_dir, cpu_total, host_after),
    }
    bundle["efficiency"] = efficiency_section(bundle)
    bundle["saturation_notes"] = saturation_summary(cpu, env, net)
    out = args.out or os.path.join(case_dir, "diagnostics.json")
    with open(out, "w", encoding="utf-8") as output:
        json.dump(bundle, output, indent=1, sort_keys=False, default=str)
    if args.print_summary:
        for note in bundle["saturation_notes"]:
            print(f"   diag: {note}")
        print(f"   diagnostics_json={out}")


def matrix(args):
    """Summarize every rung's diagnostics into one capacity matrix per mix."""
    mixes = {}
    for path in sorted(glob.glob(os.path.join(args.run_dir, "capacity", "*",
                                              "diagnostics.json"))):
        bundle = load_json(path)
        if not bundle:
            continue
        case = bundle.get("case", {})
        mix = case.get("mix")
        if not mix:
            continue
        entry = mixes.setdefault(mix, {"rungs": []})
        entry["rungs"].append({
            "destinations": case.get("destinations"),
            "outcome": bundle.get("outcome"),
            "srt_senders": case.get("srt_senders"),
            "hls": case.get("hls"),
            "observed_delivery_ratio": observed_ratio(bundle),
            "delivery_ratio_threshold": threshold_ratio(bundle),
            "delivery_ratio_basis": ratio_basis(bundle),
            "diagnostics": os.path.relpath(path, args.run_dir),
        })
    for mix, entry in mixes.items():
        rungs = sorted(entry["rungs"], key=lambda r: r["destinations"] or 0)
        entry["rungs"] = rungs
        passing = [r["destinations"] for r in rungs if r["outcome"] == "pass"]
        failing = [r["destinations"] for r in rungs
                   if r["outcome"] == "quality-failure"]
        setup = [r["destinations"] for r in rungs
                 if r["outcome"] == "setup-failure"]
        entry["highest_passing"] = max(passing) if passing else None
        entry["first_quality_failure"] = min(failing) if failing else None
        entry["setup_limited"] = setup
    host = {}
    first = next(iter(glob.glob(os.path.join(args.run_dir, "capacity", "*",
                                             "host.after.json"))), None)
    if first:
        snap = load_json(first) or {}
        host = {"online_cpus": snap.get("host", {}).get("nproc_online"),
                "permitted_cpus": snap.get("host", {}).get("sampler_affinity"),
                "cgroup_cpu_max": (snap.get("host", {}).get("cgroup") or {}).get("cpu_max")}
    result = {"schema": "nginx-media.capacity-matrix/1", "host": host,
              "mixes": mixes}
    out = args.out or os.path.join(args.run_dir, "capacity-matrix.json")
    with open(out, "w", encoding="utf-8") as output:
        json.dump(result, output, indent=1)
    print(f"capacity_matrix_json={out}")
    for mix, entry in mixes.items():
        print(f"matrix mix={mix} highest_passing={entry['highest_passing']} "
              f"first_quality_failure={entry['first_quality_failure']} "
              f"setup_limited={','.join(map(str, entry['setup_limited'])) or 'none'}")


# Which field carries the observed ratio, per protocol.  A configured
# threshold is never one of them: min_delivery_ratio in the HLS reader log is
# the gate the run was asked to accept, and reading it as a measurement is
# what made a passing rung look like a 95% delivery rate.
OBSERVED_RATIO_KEYS = {
    "srt": ("quality_average_delivery_ratio_min",),
    "rtmp": ("quality_min_delivery_ratio",),
    "hls_push": ("quality_average_delivery_ratio_min",),
    "hls_readers": ("observed_min_delivery_ratio",),
}

THRESHOLD_RATIO_KEYS = {
    "hls_readers": ("delivery_ratio_threshold", "min_delivery_ratio"),
}


def ratio_fields(bundle):
    """(observed values, threshold values, protocols that reported no
    measurement, protocols the bundle reports at all)."""
    delivery = bundle.get("delivery") or {}
    observed, thresholds, unmeasured, present = [], [], [], []
    for protocol in ("srt", "rtmp", "hls_push", "hls_readers"):
        report = (delivery.get(protocol) or {}).get("value")
        if not isinstance(report, dict):
            continue
        present.append(protocol)
        found = [report[name] for name in OBSERVED_RATIO_KEYS.get(protocol, ())
                 if isinstance(report.get(name), (int, float))]
        if found:
            observed.append(min(found))
        else:
            unmeasured.append(protocol)
        thresholds.extend(
            report[name] for name in THRESHOLD_RATIO_KEYS.get(protocol, ())
            if isinstance(report.get(name), (int, float)))
    return observed, thresholds, unmeasured, present


def observed_ratio(bundle):
    return min(ratio_fields(bundle)[0], default=None)


def threshold_ratio(bundle):
    return min(ratio_fields(bundle)[1], default=None)


def ratio_basis(bundle):
    observed, thresholds, unmeasured, present = ratio_fields(bundle)
    if observed and not unmeasured:
        return "observed"
    if observed:
        return "observed-for-" + ",".join(
            p for p in present if p not in unmeasured)
    if thresholds:
        return "threshold-only"
    return "unavailable"


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build")
    b.add_argument("case_dir")
    b.add_argument("--meta", action="append")
    b.add_argument("--out")
    b.add_argument("--print-summary", action="store_true")
    m = sub.add_parser("matrix")
    m.add_argument("run_dir")
    m.add_argument("--out")
    args = parser.parse_args()
    if args.command == "build":
        build(args)
    else:
        matrix(args)


if __name__ == "__main__":
    main()

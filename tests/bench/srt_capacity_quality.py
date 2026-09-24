#!/usr/bin/env python3
"""Measure per-destination SRT delivery rate, MPEG-TS continuity, and queues."""

import argparse
import csv
import glob
import http.client
import math
import os
import re
import signal
import sys
import time
from collections import defaultdict
from threading import Event

PACKET_DROP_METRICS = {
    "nginx_media_srt_egress_shard_feed_queue_dropped_total",
    "nginx_media_srt_egress_shard_output_dropped_total",
}
QUEUE_METRICS = {
    "nginx_media_srt_egress_shard_destinations",
    "nginx_media_srt_egress_shard_feed_queue_units",
    "nginx_media_srt_egress_shard_feed_queue_bytes",
    "nginx_media_srt_egress_shard_feed_queue_dropped_total",
    "nginx_media_srt_egress_shard_output_queue_units",
    "nginx_media_srt_egress_shard_output_queue_bytes",
    "nginx_media_srt_egress_shard_output_dropped_total",
    "nginx_media_srt_egress_shard_sent_bytes_total",
    "nginx_media_srt_egress_shard_blocked_sends_total",
    "nginx_media_srt_egress_shard_retransmitted_packets_total",
}
LABEL_RE = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:\\.|[^"])*)"')
SAMPLE_RE = re.compile(
    r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{(.*?)\})?\s+"
    r"([-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)"
)


def read_receiver_snapshot(path):
    with open(path, newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        required = {"destination_id", "bytes_received", "snapshot_ns"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(f"receiver snapshot has an invalid schema: {path}")
        values = {}
        stamp = None
        for row in reader:
            destination = row["destination_id"]
            try:
                received = int(row["bytes_received"])
                row_stamp = int(row["snapshot_ns"])
            except (TypeError, ValueError):
                raise ValueError(f"invalid snapshot values for {destination}")
            if received < 0 or row_stamp <= 0 or destination in values:
                raise ValueError(f"invalid or duplicate receiver row: {destination}")
            if stamp is not None and row_stamp != stamp:
                raise ValueError("receiver snapshot has inconsistent timestamps")
            stamp = row_stamp
            values[destination] = received
    if not values or stamp is None:
        raise ValueError(f"receiver snapshot is empty: {path}")
    return stamp, values


def wait_snapshot(path, after_ns, stop_event, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not stop_event.is_set():
        try:
            stamp, values = read_receiver_snapshot(path)
            if stamp > after_ns:
                return stamp, values
        except (OSError, ValueError):
            pass
        stop_event.wait(0.01)
    if stop_event.is_set():
        return None
    raise RuntimeError(f"receiver did not write a fresh snapshot: {path}")


def sample_receivers(args):
    stop_event = Event()
    signal.signal(signal.SIGTERM, lambda _signo, _frame: stop_event.set())
    signal.signal(signal.SIGINT, lambda _signo, _frame: stop_event.set())
    previous_ns, previous = read_receiver_snapshot(args.baseline)
    with open(args.output, "w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=("destination_id", "start_ns", "end_ns",
                        "bytes_received", "total_bytes"),
        )
        writer.writeheader()
        output.flush()
        while not stop_event.wait(args.interval):
            try:
                os.unlink(args.snapshot)
            except FileNotFoundError:
                pass
            try:
                os.kill(args.receiver_pid, signal.SIGUSR1)
            except ProcessLookupError:
                raise RuntimeError("SRT receiver exited during interval sampling")
            result = wait_snapshot(args.snapshot, previous_ns, stop_event)
            if result is None:
                break
            current_ns, current = result
            if set(current) != set(previous):
                raise RuntimeError("receiver destination set changed during measurement")
            if current_ns <= previous_ns:
                raise RuntimeError("receiver snapshot timestamp did not advance")
            for destination in sorted(current):
                delta = current[destination] - previous[destination]
                if delta < 0:
                    raise RuntimeError(f"receiver counter decreased for {destination}")
                writer.writerow({
                    "destination_id": destination,
                    "start_ns": previous_ns,
                    "end_ns": current_ns,
                    "bytes_received": delta,
                    "total_bytes": current[destination],
                })
            output.flush()
            previous_ns, previous = current_ns, current
    return 0


def parse_metrics(blob):
    worker_pid = None
    values = defaultdict(dict)
    for line in blob.splitlines():
        match = SAMPLE_RE.match(line)
        if match is None:
            continue
        name, label_text, raw_value = match.groups()
        labels = dict(LABEL_RE.findall(label_text or ""))
        try:
            value = float(raw_value)
        except ValueError:
            continue
        if not math.isfinite(value):
            continue
        if name == "nginx_media_worker_info" and "pid" in labels and value == 1:
            worker_pid = labels["pid"]
        if name in QUEUE_METRICS and "worker" in labels and "shard" in labels:
            values[(labels["worker"], labels["shard"])][name] = value
    return worker_pid, values


def fetch_worker_round(port, expected_pids):
    received = {}
    attempts = max(100, len(expected_pids) * 50)
    for _ in range(attempts):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2.0)
        try:
            connection.request("GET", "/media/api/v1/metrics",
                              headers={"Connection": "close"})
            response = connection.getresponse()
            if response.status != 200:
                response.read()
                continue
            blob = response.read().decode("utf-8", errors="replace")
            worker_pid, shard_values = parse_metrics(blob)
            if worker_pid not in expected_pids:
                continue
            received.setdefault(worker_pid,
                                (time.monotonic_ns(), shard_values))
            if len(received) == len(expected_pids):
                return received
        except (OSError, http.client.HTTPException):
            pass
        finally:
            connection.close()
    missing = sorted(expected_pids - set(received), key=int)
    raise RuntimeError(f"could not sample worker metrics for PIDs {missing}")


def sample_queues(args):
    expected = set(args.worker_pids)
    if not expected:
        raise ValueError("queue sampler requires worker PIDs")
    stop_event = Event()
    signal.signal(signal.SIGTERM, lambda _signo, _frame: stop_event.set())
    signal.signal(signal.SIGINT, lambda _signo, _frame: stop_event.set())
    round_id = 0
    with open(args.output, "w", newline="", encoding="utf-8") as output:
        fields = ("round_id", "sample_ns", "worker_pid", "worker", "shard",
                  "destinations", "feed_queue_units", "feed_queue_bytes",
                  "feed_drops", "output_queue_units", "output_queue_bytes",
                  "output_drops", "sent_bytes", "blocked_sends",
                  "retransmitted_packets")
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        output.flush()
        while not stop_event.wait(args.interval):
            samples = fetch_worker_round(args.port, expected)
            for pid in sorted(samples, key=int):
                sample_ns, shard_values = samples[pid]
                for (worker, shard), values in sorted(shard_values.items()):
                    writer.writerow({
                        "round_id": round_id,
                        "sample_ns": sample_ns,
                        "worker_pid": pid,
                        "worker": worker,
                        "shard": shard,
                        "destinations": values.get("nginx_media_srt_egress_shard_destinations", 0),
                        "feed_queue_units": values.get("nginx_media_srt_egress_shard_feed_queue_units", 0),
                        "feed_queue_bytes": values.get("nginx_media_srt_egress_shard_feed_queue_bytes", 0),
                        "feed_drops": values.get("nginx_media_srt_egress_shard_feed_queue_dropped_total", 0),
                        "output_queue_units": values.get("nginx_media_srt_egress_shard_output_queue_units", 0),
                        "output_queue_bytes": values.get("nginx_media_srt_egress_shard_output_queue_bytes", 0),
                        "output_drops": values.get("nginx_media_srt_egress_shard_output_dropped_total", 0),
                        "sent_bytes": values.get("nginx_media_srt_egress_shard_sent_bytes_total", 0),
                        "blocked_sends": values.get("nginx_media_srt_egress_shard_blocked_sends_total", 0),
                        "retransmitted_packets": values.get("nginx_media_srt_egress_shard_retransmitted_packets_total", 0),
                    })
            output.flush()
            round_id += 1
    return 0


def read_metric_files(prefix):
    values = defaultdict(dict)
    paths = sorted(glob.glob(prefix + ".*"))
    if not paths:
        raise ValueError(f"no worker metrics files match {prefix}.*")
    for path in paths:
        with open(path, encoding="utf-8") as source:
            for line in source:
                match = SAMPLE_RE.match(line)
                if match is None:
                    continue
                name, label_text, raw_value = match.groups()
                if name not in QUEUE_METRICS:
                    continue
                labels = dict(LABEL_RE.findall(label_text or ""))
                if "worker" not in labels or "shard" not in labels:
                    continue
                try:
                    number = float(raw_value)
                except ValueError:
                    continue
                if math.isfinite(number):
                    values[(labels["worker"], labels["shard"])][name] = number
    return values


def read_stream_metric(path, metric):
    total = 0.0
    with open(path, encoding="utf-8") as source:
        for line in source:
            match = SAMPLE_RE.match(line)
            if match is None or match.group(1) != metric:
                continue
            try:
                value = float(match.group(3))
            except ValueError:
                continue
            if math.isfinite(value):
                total += value
    return total


def jain(values):
    total = sum(values)
    squares = sum(value * value for value in values)
    return total * total / (len(values) * squares) if values and squares else 0.0


def quality_report(args):
    failures = []
    baseline_ns, baseline = read_receiver_snapshot(args.baseline)
    final_ns, final = read_receiver_snapshot(args.final)
    expected = {f"d{i:04d}" for i in range(args.destinations)}
    if set(baseline) != expected or set(final) != expected:
        raise ValueError("receiver snapshot IDs do not match the quality rung")
    if final_ns <= baseline_ns:
        raise ValueError("receiver measurement timestamps did not advance")
    duration_s = (final_ns - baseline_ns) / 1e9
    if duration_s <= 0:
        raise ValueError("receiver measurement duration is invalid")

    interval_rows = defaultdict(list)
    with open(args.intervals, newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        required = {"destination_id", "start_ns", "end_ns", "bytes_received", "total_bytes"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError("receiver interval CSV has an invalid schema")
        for row in reader:
            destination = row["destination_id"]
            try:
                values = (int(row["start_ns"]), int(row["end_ns"]),
                          int(row["bytes_received"]), int(row["total_bytes"]))
            except (TypeError, ValueError):
                raise ValueError(f"invalid receiver interval for {destination}")
            if (destination not in expected or values[0] < 0
                    or values[1] <= values[0] or values[2] < 0 or values[3] < 0):
                raise ValueError(f"invalid receiver interval row for {destination}")
            interval_rows[destination].append(values)

    receiver_results = {}
    with open(args.receiver_results, newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        required = {"destination_id", "ts_packets", "ts_sync_errors",
                    "ts_continuity_errors", "ts_tei_errors", "stalled"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError("receiver results CSV lacks quality counters")
        for row in reader:
            destination = row["destination_id"]
            if destination in receiver_results:
                raise ValueError(f"duplicate receiver result for {destination}")
            try:
                receiver_results[destination] = {
                    key: int(row[key]) for key in
                    ("ts_packets", "ts_sync_errors", "ts_continuity_errors", "ts_tei_errors")
                } | {"stalled": row["stalled"].lower() in {"1", "true", "yes"}}
            except (TypeError, ValueError):
                raise ValueError(f"invalid TS counters for {destination}")
    if set(receiver_results) != expected:
        raise ValueError("receiver results IDs do not match the quality rung")

    rows = []
    all_rates = []
    reference_bps = args.reference_bps
    if reference_bps <= 0:
        if args.destinations != 1:
            raise ValueError("one-destination quality case is required to calibrate rate")
        reference_bps = (final[next(iter(expected))] - baseline[next(iter(expected))]) * 8 / duration_s
        if reference_bps <= 0:
            raise ValueError("one-destination receiver delivered no calibration payload")
    prepared_source_bps = (
        os.path.getsize(args.prepared_source) * 8 / args.source_duration_s
    )
    if prepared_source_bps <= 0:
        raise ValueError("prepared MPEG-TS source is empty")
    reference_vs_source_ratio = reference_bps / prepared_source_bps
    if reference_vs_source_ratio < args.min_delivery_ratio:
        failures.append(
            f"one-destination reference rate {reference_vs_source_ratio:.4f} "
            f"of prepared MPEG-TS bitrate is below {args.min_delivery_ratio:.4f}"
        )

    interval_file = open(args.interval_report, "w", newline="", encoding="utf-8")
    interval_writer = csv.writer(interval_file)
    interval_writer.writerow(("destination_id", "kind", "start_ns", "end_ns",
                              "duration_s", "bytes_received", "rate_bps",
                              "reference_ratio"))
    for destination in sorted(expected):
        total_bytes = final[destination] - baseline[destination]
        if total_bytes < 0:
            raise ValueError(f"receiver byte counter decreased for {destination}")
        rate_bps = total_bytes * 8 / duration_s
        ratio = rate_bps / reference_bps
        intervals = sorted(interval_rows[destination])
        if not intervals:
            failures.append(f"{destination}: no short-interval samples")
        previous_end = baseline_ns
        previous_total = baseline[destination]
        low_start = None
        low_end = None
        min_interval_ratio = None
        largest_interval_s = 0.0
        interval_bytes = 0
        for start_ns, end_ns, byte_count, cumulative in intervals:
            span_s = (end_ns - start_ns) / 1e9
            largest_interval_s = max(largest_interval_s, span_s)
            if start_ns != previous_end:
                failures.append(f"{destination}: interval sampling gap or overlap")
            if cumulative < previous_total or cumulative - previous_total != byte_count:
                failures.append(f"{destination}: interval counters do not reconcile")
            if span_s > args.max_interval_s:
                failures.append(f"{destination}: sample interval {span_s:.2f}s exceeds limit")
            interval_ratio = (byte_count * 8 / span_s) / reference_bps
            min_interval_ratio = interval_ratio if min_interval_ratio is None else min(min_interval_ratio, interval_ratio)
            interval_writer.writerow((destination, "sample", start_ns, end_ns,
                                      f"{span_s:.6f}", byte_count,
                                      f"{byte_count * 8 / span_s:.2f}",
                                      f"{interval_ratio:.6f}"))
            interval_bytes += byte_count
            if interval_ratio < args.interval_floor:
                if low_start is None:
                    low_start = start_ns
                low_end = end_ns
                if (low_end - low_start) / 1e9 > args.max_low_s:
                    failures.append(f"{destination}: under-rate persisted {(low_end-low_start)/1e9:.2f}s")
            else:
                low_start = low_end = None
            previous_end = end_ns
            previous_total = cumulative
        last_end = intervals[-1][1] if intervals else baseline_ns
        residual_bytes = final[destination] - previous_total
        if residual_bytes < 0:
            failures.append(f"{destination}: interval counter exceeds final receiver counter")
            residual_bytes = 0
        if final_ns > last_end:
            residual_s = (final_ns - last_end) / 1e9
            largest_interval_s = max(largest_interval_s, residual_s)
            if residual_s > args.max_interval_s:
                failures.append(f"{destination}: final sample gap {residual_s:.2f}s exceeds limit")
            residual_ratio = (residual_bytes * 8 / residual_s) / reference_bps
            min_interval_ratio = residual_ratio if min_interval_ratio is None else min(min_interval_ratio, residual_ratio)
            interval_writer.writerow((destination, "final-residual", last_end, final_ns,
                                      f"{residual_s:.6f}", residual_bytes,
                                      f"{residual_bytes * 8 / residual_s:.2f}",
                                      f"{residual_ratio:.6f}"))
            if residual_ratio < args.interval_floor:
                if low_start is None:
                    low_start = last_end
                low_end = final_ns
                if (low_end - low_start) / 1e9 > args.max_low_s:
                    failures.append(f"{destination}: final under-rate persisted {(low_end-low_start)/1e9:.2f}s")
        elif final_ns < last_end:
            failures.append(f"{destination}: interval sample extends past final snapshot")
        if interval_bytes + residual_bytes != total_bytes:
            failures.append(f"{destination}: interval bytes do not reconcile with final counter")
        if ratio < args.min_delivery_ratio:
            failures.append(f"{destination}: average delivery ratio {ratio:.4f} below {args.min_delivery_ratio:.4f}")
        ts = receiver_results[destination]
        if ts["stalled"]:
            failures.append(f"{destination}: receiver marked stalled")
        if ts["ts_packets"] <= 0:
            failures.append(f"{destination}: no MPEG-TS packets parsed")
        if any(ts[key] != 0 for key in ("ts_sync_errors", "ts_continuity_errors", "ts_tei_errors")):
            failures.append(f"{destination}: MPEG-TS continuity/sync/TEI errors "
                            f"{ts['ts_sync_errors']}/{ts['ts_continuity_errors']}/{ts['ts_tei_errors']}")
        rows.append((destination, total_bytes, rate_bps, ratio,
                     min_interval_ratio or 0.0, largest_interval_s, ts))
        all_rates.append(rate_bps)
    interval_file.close()

    before = read_metric_files(args.metrics_before)
    after = read_metric_files(args.metrics_after)
    queue_drops = {name: 0.0 for name in PACKET_DROP_METRICS}
    keys = set(before) | set(after)
    for key in keys:
        for metric in PACKET_DROP_METRICS:
            old = before.get(key, {}).get(metric, 0.0)
            new = after.get(key, {}).get(metric, 0.0)
            if new < old:
                failures.append(f"{key[0]}/{key[1]}: {metric} counter regressed")
            queue_drops[metric] += max(0.0, new - old)
    if any(queue_drops.values()):
        failures.append("sender feed/output application drop counters increased")

    for metric in ("nginx_media_stream_feed_overruns_total",
                   "nginx_media_stream_feed_evictions_total"):
        old = read_stream_metric(args.stream_before, metric)
        new = read_stream_metric(args.stream_after, metric)
        if new < old:
            failures.append(f"{metric} counter regressed")
        elif new > old:
            failures.append(f"{metric} increased by {new - old:g}")

    queue_rows = defaultdict(dict)
    with open(args.queues, newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        required = {"round_id", "sample_ns", "worker_pid", "worker", "shard",
                    "destinations", "feed_queue_units", "feed_queue_bytes",
                    "feed_drops", "output_queue_units", "output_queue_bytes",
                    "output_drops", "sent_bytes", "blocked_sends", "retransmitted_packets"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError("queue sample CSV has an invalid schema")
        for row in reader:
            try:
                round_id = int(row["round_id"])
                sample_ns = int(row["sample_ns"])
                data = {name: float(row[name]) for name in (
                    "destinations", "feed_queue_units", "feed_queue_bytes", "feed_drops",
                    "output_queue_units", "output_queue_bytes", "output_drops",
                    "sent_bytes", "blocked_sends", "retransmitted_packets")}
            except (TypeError, ValueError):
                raise ValueError("invalid queue sample row")
            queue_rows[round_id][(row["worker"], row["shard"])] = (sample_ns, data)
    rounds = sorted(queue_rows)
    if len(rounds) < 3:
        failures.append(f"only {len(rounds)} complete queue samples; at least 3 required")

    peak_feed = 0.0
    peak_output = 0.0
    pressure_streak = defaultdict(int)
    blocked_streak = defaultdict(int)
    prior_values = {}
    for round_id in rounds:
        current = queue_rows[round_id]
        for shard, (sample_ns, data) in current.items():
            if data["destinations"] <= 0:
                continue
            old = prior_values.get(shard)
            consecutive = False
            if old is not None:
                old_round, old_ns, old_data = old
                gap_s = (sample_ns - old_ns) / 1e9
                if round_id > old_round + 1:
                    failures.append(f"{shard[0]}/{shard[1]}: queue samples missed "
                                    f"{round_id - old_round - 1} rounds")
                elif round_id == old_round + 1 and gap_s > args.max_interval_s:
                    failures.append(f"{shard[0]}/{shard[1]}: queue sample gap "
                                    f"{gap_s:.2f}s exceeds limit")
                consecutive = (
                    round_id == old_round + 1
                    and gap_s <= args.max_interval_s
                )
            if not consecutive:
                pressure_streak[(shard, "feed")] = 0
                pressure_streak[(shard, "output")] = 0
                blocked_streak[shard] = 0

            feed_util = max(data["feed_queue_units"] / 64,
                            data["feed_queue_bytes"] / (8 * 1024 * 1024))
            output_capacity = data["destinations"] * 256
            output_bytes_capacity = data["destinations"] * 8 * 1024 * 1024
            output_util = max(data["output_queue_units"] / output_capacity,
                              data["output_queue_bytes"] / output_bytes_capacity)
            peak_feed = max(peak_feed, feed_util)
            peak_output = max(peak_output, output_util)
            for kind, utilization in (("feed", feed_util), ("output", output_util)):
                if utilization >= args.queue_pressure:
                    pressure_streak[(shard, kind)] += 1
                    if pressure_streak[(shard, kind)] == args.pressure_samples:
                        failures.append(f"{shard[0]}/{shard[1]}: sustained {kind} queue pressure "
                                        f"{utilization:.3f} for {pressure_streak[(shard, kind)]} samples")
                else:
                    pressure_streak[(shard, kind)] = 0
            if consecutive:
                if data["blocked_sends"] > old_data["blocked_sends"]:
                    blocked_streak[shard] += 1
                else:
                    blocked_streak[shard] = 0
                if blocked_streak[shard] == args.blocked_samples:
                    failures.append(f"{shard[0]}/{shard[1]}: blocked sends increased for "
                                    f"{blocked_streak[shard]} consecutive samples")
            prior_values[shard] = (round_id, sample_ns, data)

    with open(args.destination_report, "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(("destination_id", "delivered_bytes", "average_bps",
                         "average_delivery_ratio", "minimum_interval_ratio",
                         "largest_interval_s", "ts_packets", "ts_sync_errors",
                         "ts_continuity_errors", "ts_tei_errors"))
        for destination, received, rate, ratio, low_ratio, max_interval, ts in rows:
            writer.writerow((destination, received, f"{rate:.2f}", f"{ratio:.5f}",
                             f"{low_ratio:.5f}", f"{max_interval:.3f}", ts["ts_packets"],
                             ts["ts_sync_errors"], ts["ts_continuity_errors"], ts["ts_tei_errors"]))

    print(f"quality_pass={'no' if failures else 'yes'}")
    print(f"quality_reference_payload_bps={reference_bps:.2f}")
    print(f"quality_reference_payload_mbps={reference_bps / 1_000_000:.4f}")
    print(f"quality_prepared_source_payload_bps={prepared_source_bps:.2f}")
    print(f"quality_reference_vs_prepared_ratio={reference_vs_source_ratio:.5f}")
    print(f"quality_measurement_s={duration_s:.3f}")
    print(f"quality_total_receiver_bytes={sum(row[1] for row in rows)}")
    print(f"quality_average_delivery_ratio_min={min(row[3] for row in rows):.5f}")
    print(f"quality_interval_delivery_ratio_min={min(row[4] for row in rows):.5f}")
    print(f"quality_destination_fairness_jain={jain(all_rates):.5f} (supplementary)")
    print(f"quality_sender_feed_drops={queue_drops['nginx_media_srt_egress_shard_feed_queue_dropped_total']:g}")
    print(f"quality_sender_output_drops={queue_drops['nginx_media_srt_egress_shard_output_dropped_total']:g}")
    print(f"quality_peak_feed_queue_utilization={peak_feed:.4f}")
    print(f"quality_peak_output_queue_utilization={peak_output:.4f}")
    print(f"quality_complete_queue_samples={len(rounds)}")
    unique_failures = list(dict.fromkeys(failures))
    print(f"quality_failure_count={len(unique_failures)}")
    for failure in unique_failures[:25]:
        print(f"quality_failure={failure}")
    if len(unique_failures) > 25:
        print(f"quality_failure_more={len(unique_failures) - 25}")
    return 0


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    receivers = subparsers.add_parser("sample-receivers")
    receivers.add_argument("receiver_pid", type=int)
    receivers.add_argument("snapshot")
    receivers.add_argument("baseline")
    receivers.add_argument("output")
    receivers.add_argument("--interval", type=float, default=1.0)
    receivers.set_defaults(run=sample_receivers)
    queues = subparsers.add_parser("sample-queues")
    queues.add_argument("port", type=int)
    queues.add_argument("output")
    queues.add_argument("worker_pids", nargs="+")
    queues.add_argument("--interval", type=float, default=1.0)
    queues.set_defaults(run=sample_queues)
    report = subparsers.add_parser("report")
    report.add_argument("--baseline", required=True)
    report.add_argument("--final", required=True)
    report.add_argument("--intervals", required=True)
    report.add_argument("--queues", required=True)
    report.add_argument("--receiver-results", required=True)
    report.add_argument("--metrics-before", required=True)
    report.add_argument("--metrics-after", required=True)
    report.add_argument("--stream-before", required=True)
    report.add_argument("--stream-after", required=True)
    report.add_argument("--destination-report", required=True)
    report.add_argument("--interval-report", required=True)
    report.add_argument("--destinations", type=int, required=True)
    report.add_argument("--prepared-source", required=True)
    report.add_argument("--source-duration-s", type=float, default=30.0)
    report.add_argument("--reference-bps", type=float, default=0.0)
    report.add_argument("--min-delivery-ratio", type=float, default=0.95)
    report.add_argument("--interval-floor", type=float, default=0.80)
    report.add_argument("--max-low-s", type=float, default=2.0)
    report.add_argument("--max-interval-s", type=float, default=2.0)
    report.add_argument("--queue-pressure", type=float, default=0.90)
    report.add_argument("--pressure-samples", type=int, default=3)
    report.add_argument("--blocked-samples", type=int, default=3)
    report.set_defaults(run=quality_report)
    args = parser.parse_args()
    if getattr(args, "interval", 1.0) <= 0:
        parser.error("sampling interval must be positive")
    try:
        return args.run(args)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"srt_capacity_quality: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

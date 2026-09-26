#!/usr/bin/env python3
"""Discarding HLS PUT sink and per-destination capacity-quality report.

Throughput is judged on identified segments, not on bytes that happened to
complete inside a window: with multi-second segments a window edge moves a
whole segment in or out, which reads as a 10-20% rate change that is not one.
The report takes the exact set of segments first delivered (to any
destination) inside the window and asks, per destination, whether each of them
arrived, exactly once, with the same size, and how long after the first
destination had it.  A playlist PUT that references a segment the destination
has not completely received yet is counted as an ordering violation.
"""

import argparse
import csv
import http.server
import json
import os
import re
import sys
import threading
import time
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


class HlsPushSink(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        parsed = urlsplit(self.path)
        if parsed.path == "/__health":
            self._reply(200)
            return

        if parsed.path == "/__mark":
            with self.server.destinations_lock:
                if self.server.mark_monotonic is None:
                    self.server.mark_monotonic = time.monotonic()
                marked = self.server.mark_monotonic
            self._reply(200, json.dumps({"mark_monotonic": marked}).encode(),
                        "application/json")
            return

        if parsed.path in ("/__ready", "/__progress"):
            try:
                query = parse_qs(parsed.query)
                expected = int(query["destinations"][0])
                offset = int(query.get("offset", ["0"])[0])
                if expected < 0 or offset < 0:
                    raise ValueError
            except (KeyError, IndexError, ValueError):
                self._reply(400)
                return
            expected_ids = {
                f"d{destination:04d}"
                for destination in range(offset, offset + expected)
            }
            with self.server.destinations_lock:
                now = time.monotonic()
                received = self.server.ts_destinations & expected_ids
                progress = {
                    "monotonic": now,
                    "elapsed_s": (
                        now - self.server.mark_monotonic
                        if self.server.mark_monotonic is not None else None
                    ),
                    "expected": expected,
                    "received": len(received),
                    "missing_ids": sorted(expected_ids - received),
                    "first_segment_latencies_ms": sorted(
                        (self.server.ts_first_at[destination]
                         - self.server.mark_monotonic) * 1000
                        for destination in received
                        if self.server.mark_monotonic is not None
                    ),
                }
            body = json.dumps(progress).encode()
            if parsed.path == "/__ready":
                self._reply(200 if not progress["missing_ids"] else 503,
                            count_error=False)
            else:
                self._reply(200, body, "application/json")
            return

        if parsed.path == "/__snapshot":
            with self.server.destinations_lock:
                snapshot = {
                    "monotonic": time.monotonic(),
                    "mark_monotonic": self.server.mark_monotonic,
                    "http_errors": self.server.http_errors,
                    "destinations": {
                        destination: {
                            "ts_bytes": self.server.ts_bytes[destination],
                            "segments": self.server.ts_segments[destination],
                            "first_segment_monotonic":
                                self.server.ts_first_at[destination],
                            "first_segment_latency_ms": (
                                (self.server.ts_first_at[destination]
                                 - self.server.mark_monotonic) * 1000
                                if self.server.mark_monotonic is not None else None
                            ),
                            "upload_durations_ms":
                                list(self.server.ts_upload_durations_ms[destination]),
                            "segment_detail":
                                dict(self.server.ts_detail.get(destination, {})),
                            "playlist_puts":
                                self.server.playlist_puts.get(destination, 0),
                            "playlist_violations":
                                self.server.playlist_violations.get(destination, 0),
                        }
                        for destination in self.server.ts_destinations
                    },
                }
            body = json.dumps(snapshot).encode()
            self._reply(200, body, "application/json")
            return
        self._reply(404)

    def do_PUT(self):
        upload_started = time.monotonic()
        parsed = urlsplit(self.path)
        parts = parsed.path.strip("/").split("/", 1)
        if not parts[0]:
            self._reply(400)
            return

        try:
            remaining = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            self._reply(400)
            return
        if remaining < 0:
            self._reply(411)
            return

        received = 0
        keep = parsed.path.lower().endswith(".m3u8")
        body = []
        while remaining:
            chunk = self.rfile.read(min(65536, remaining))
            if not chunk:
                self._reply(400)
                return
            remaining -= len(chunk)
            received += len(chunk)
            if keep:
                body.append(chunk)
        if keep:
            self._check_playlist(parts[0], parts[1] if len(parts) > 1 else "",
                                 b"".join(body))

        name = parts[1] if len(parts) > 1 else ""
        if parsed.path.lower().endswith(".ts"):
            with self.server.destinations_lock:
                destination = parts[0]
                detail = self.server.ts_detail.setdefault(destination, {})
                record = detail.get(name)
                if record is None:
                    detail[name] = {"bytes": received, "at": time.monotonic(),
                                    "count": 1}
                else:
                    record["count"] += 1
                    record["bytes"] = received
                self.server.ts_destinations.add(destination)
                self.server.ts_bytes[destination] = (
                    self.server.ts_bytes.get(destination, 0) + received
                )
                self.server.ts_segments[destination] = (
                    self.server.ts_segments.get(destination, 0) + 1
                )
                self.server.ts_first_at.setdefault(destination, time.monotonic())
                self.server.ts_upload_durations_ms.setdefault(destination, []).append(
                    (time.monotonic() - upload_started) * 1000
                )
        self._reply(201)

    def _check_playlist(self, destination, name, body):
        base = os.path.dirname(name)
        referenced = [
            os.path.normpath(os.path.join(base, line.strip()))
            for line in body.decode("utf-8", "replace").splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with self.server.destinations_lock:
            detail = self.server.ts_detail.get(destination, {})
            missing = [uri for uri in referenced if uri not in detail]
            self.server.playlist_puts[destination] = (
                self.server.playlist_puts.get(destination, 0) + 1)
            if missing:
                self.server.playlist_violations[destination] = (
                    self.server.playlist_violations.get(destination, 0)
                    + len(missing))

    def _reply(self, status, body=b"", content_type=None, count_error=True):
        if count_error and status >= 400:
            with self.server.destinations_lock:
                self.server.http_errors += 1
        self.send_response(status)
        if content_type:
            self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def log_message(self, *_args):
        pass



def serve(port):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), HlsPushSink)
    server.daemon_threads = True
    server.destinations_lock = threading.Lock()
    server.ts_destinations = set()
    server.ts_bytes = {}
    server.ts_segments = {}
    server.ts_first_at = {}
    server.ts_upload_durations_ms = {}
    server.ts_detail = {}
    server.playlist_puts = {}
    server.playlist_violations = {}
    server.mark_monotonic = None
    server.http_errors = 0
    server.serve_forever(poll_interval=0.25)


METRIC_NAMES = {
    "nginx_media_egress_dropped_units_total": "dropped_units",
    "nginx_media_egress_transport_errors_total": "transport_errors",
    "nginx_media_egress_queue_bytes": "queue_bytes",
    "nginx_media_egress_queue_lag_ms": "queue_lag_ms",
}
LABEL = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)="((?:\\.|[^"\\])*)"')


def metric_snapshot(prefix, stream):
    prefix = Path(prefix)
    paths = sorted(prefix.parent.glob(prefix.name + ".*"))
    if not paths:
        raise RuntimeError(f"no worker metric snapshots for {prefix}")

    result = {}
    for path in paths:
        with path.open(encoding="utf-8") as source:
            for line in source:
                metric, separator, rest = line.partition("{")
                if not separator or metric not in METRIC_NAMES:
                    continue
                labels_text, separator, value_text = rest.partition("}")
                if not separator:
                    continue
                labels = dict(LABEL.findall(labels_text))
                if labels.get("protocol") != "hls_push" or labels.get("name") != stream:
                    continue
                try:
                    value = int(float(value_text.strip()))
                except ValueError:
                    raise RuntimeError(f"invalid {metric} value in {path}")
                destination = labels.get("destination")
                if not destination:
                    raise RuntimeError(f"HLS push metric has no destination in {path}")
                result.setdefault(destination, {})[METRIC_NAMES[metric]] = value
    return result


def active_worker_snapshot(prefix, engine):
    prefix = Path(prefix)
    paths = sorted(prefix.parent.glob(prefix.name + ".*"))
    active = []
    for path in paths:
        with path.open(encoding="utf-8") as source:
            for line in source:
                metric, separator, rest = line.partition("{")
                if metric != "nginx_media_egress_active_workers" or not separator:
                    continue
                labels_text, separator, value_text = rest.partition("}")
                if not separator:
                    continue
                labels = dict(LABEL.findall(labels_text))
                if labels.get("engine") != engine:
                    continue
                try:
                    active.append(int(float(value_text.strip())))
                except ValueError:
                    raise RuntimeError(f"invalid active worker value in {path}")
    if not active:
        raise RuntimeError(f"no {engine} active-worker metric in {prefix}")
    return max(active)


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[int((len(ordered) - 1) * fraction)] if ordered else 0


def readiness_report(args):
    metrics = metric_snapshot(args.metrics_prefix, args.stream)
    active = active_worker_snapshot(args.metrics_prefix, "hls_upload_pool")
    with Path(args.sink_snapshot).open(encoding="utf-8") as source:
        sink = json.load(source)
    with Path(args.progress).open(encoding="utf-8") as source:
        progress = json.load(source)

    expected = [
        f"d{index:04d}"
        for index in range(args.destination_offset,
                           args.destination_offset + args.destinations)
    ]
    destinations = sink["destinations"]
    received = [destination for destination in expected
                if destination in destinations]
    missing = progress["missing_ids"]
    latencies = [
        destinations[destination]["first_segment_latency_ms"]
        for destination in received
        if destinations[destination]["first_segment_latency_ms"] is not None
    ]
    upload_durations = [
        duration
        for destination in received
        for duration in destinations[destination]["upload_durations_ms"]
    ]
    queue_lags = [
        metrics[destination]["queue_lag_ms"]
        for destination in expected
        if "queue_lag_ms" in metrics.get(destination, {})
    ]
    queue_bytes = [
        metrics[destination]["queue_bytes"]
        for destination in expected
        if "queue_bytes" in metrics.get(destination, {})
    ]
    drops = sum(
        metrics.get(destination, {}).get("dropped_units", 0)
        for destination in expected
    )
    errors = sum(
        metrics.get(destination, {}).get("transport_errors", 0)
        for destination in expected
    )

    print(f"readiness_received_destinations={len(received)}/{args.destinations}")
    print(f"readiness_elapsed_s={progress['elapsed_s']}")
    print(f"readiness_missing_ids={','.join(missing)}")
    print(f"readiness_active_uploaders={active}")
    print(f"readiness_first_segment_latency_p50_ms={percentile(latencies, 0.50):.3f}")
    print(f"readiness_first_segment_latency_p95_ms={percentile(latencies, 0.95):.3f}")
    print(f"readiness_first_segment_latency_p99_ms={percentile(latencies, 0.99):.3f}")
    print(f"readiness_first_segment_latency_max_ms={max(latencies, default=0):.3f}")
    print(f"readiness_upload_duration_p50_ms={percentile(upload_durations, 0.50):.3f}")
    print(f"readiness_upload_duration_p95_ms={percentile(upload_durations, 0.95):.3f}")
    print(f"readiness_upload_duration_max_ms={max(upload_durations, default=0):.3f}")
    print(f"readiness_queue_metric_destinations={len(queue_lags)}")
    print(f"readiness_queue_age_p50_ms={percentile(queue_lags, 0.50)}")
    print(f"readiness_queue_age_p95_ms={percentile(queue_lags, 0.95)}")
    print(f"readiness_queue_age_max_ms={max(queue_lags, default=0)}")
    print(f"readiness_queue_bytes_max={max(queue_bytes, default=0)}")
    print(f"readiness_dropped_units={drops}")
    print(f"readiness_transport_errors={errors}")
    print(f"readiness_sink_http_errors={sink['http_errors']}")


def report(args):
    before = metric_snapshot(args.before_prefix, args.stream)
    after = metric_snapshot(args.after_prefix, args.stream)
    with Path(args.sink_before).open(encoding="utf-8") as source:
        sink_before = json.load(source)
    with Path(args.sink_after).open(encoding="utf-8") as source:
        sink_after = json.load(source)

    duration = sink_after["monotonic"] - sink_before["monotonic"]
    if duration <= 0:
        raise RuntimeError("HLS push sink snapshots have a nonpositive interval")
    before_active = active_worker_snapshot(args.before_prefix, "hls_upload_pool")
    after_active = active_worker_snapshot(args.after_prefix, "hls_upload_pool")

    records = []
    for index in range(args.destinations):
        destination = f"d{index + args.destination_offset:04d}"
        final_sink = sink_after["destinations"].get(destination)
        initial_sink = sink_before["destinations"].get(destination, {})
        if final_sink is None:
            raise RuntimeError(f"no final HLS segment delivery for {destination}")

        delta_bytes = final_sink["ts_bytes"] - initial_sink.get("ts_bytes", 0)
        segments = final_sink["segments"] - initial_sink.get("segments", 0)
        if destination not in after:
            raise RuntimeError(f"no final HLS push egress metrics for {destination}")
        final_metrics = after[destination]
        initial_metrics = before.get(destination, {})
        dropped = final_metrics.get("dropped_units", 0) - initial_metrics.get(
            "dropped_units", 0
        )
        errors = final_metrics.get("transport_errors", 0) - initial_metrics.get(
            "transport_errors", 0
        )
        if min(delta_bytes, segments, dropped, errors) < 0:
            raise RuntimeError(f"HLS push counters regressed for {destination}")

        durations = final_sink["upload_durations_ms"]
        measured_durations = durations[len(initial_sink.get(
            "upload_durations_ms", []
        )):]
        if not measured_durations:
            measured_durations = durations
        first_latency = final_sink["first_segment_latency_ms"]
        if first_latency is None:
            raise RuntimeError("HLS push sink has no first-segment start mark")
        records.append({
            "destination": destination,
            "bytes": delta_bytes,
            "segments": segments,
            "rate_bps": delta_bytes * 8 / duration,
            "dropped_units": dropped,
            "transport_errors": errors,
            "queue_bytes": final_metrics.get("queue_bytes", 0),
            "queue_lag_ms": final_metrics.get("queue_lag_ms", 0),
            "first_segment_latency_ms": first_latency,
            "upload_durations_ms": measured_durations,
        })

    segment_summary = segment_accounting(args, sink_before, sink_after,
                                         records)

    reference = args.reference_bps
    if reference <= 0:
        reference = records[0]["rate_bps"]
    if reference <= 0:
        raise RuntimeError("single-destination HLS push reference rate is zero")

    failures = []
    ratios = []
    window_ratios = []
    for record in records:
        # The byte rate over the window is kept for reference; the gate is
        # the identified-segment ratio, which a window edge cannot move.
        ratio = record["rate_bps"] / reference
        record["window_byte_ratio"] = ratio
        ratio = record["segment_ratio"]
        record["ratio"] = ratio
        ratios.append(ratio)
        window_ratios.append(record["window_byte_ratio"])
        if record["segments"] == 0:
            failures.append(f"{record['destination']}: no HLS segments delivered")
        if ratio < args.min_delivery_ratio:
            failures.append(
                f"{record['destination']}: segment delivery ratio {ratio:.5f} "
                f"below {args.min_delivery_ratio:.5f}"
            )
        if record["missing_segments"]:
            failures.append(f"{record['destination']}: "
                            f"{record['missing_segments']} window segments missing")
        if record["duplicate_segments"]:
            failures.append(f"{record['destination']}: "
                            f"{record['duplicate_segments']} segments uploaded twice")
        if record["size_mismatches"]:
            failures.append(f"{record['destination']}: "
                            f"{record['size_mismatches']} segments differ in size")
        if record["late_segments"]:
            failures.append(f"{record['destination']}: {record['late_segments']} "
                            f"segments later than {segment_summary['lag_limit_s']:.1f}s")
        if record["playlist_violations"]:
            failures.append(f"{record['destination']}: playlist referenced "
                            f"{record['playlist_violations']} segments before "
                            "their upload completed")
        if record["dropped_units"]:
            failures.append(
                f"{record['destination']}: {record['dropped_units']} queued files dropped"
            )
        if record["transport_errors"]:
            failures.append(
                f"{record['destination']}: {record['transport_errors']} upload errors"
            )

    if args.report:
        report_path = Path(args.report)
        report_path.parent.mkdir(parents=True, exist_ok=True)
        with report_path.open("w", newline="", encoding="utf-8") as output:
            writer = csv.writer(output)
            writer.writerow(("destination", "delivered_bytes", "segments", "rate_bps",
                             "delivery_ratio", "window_byte_ratio",
                             "missing_segments", "duplicate_segments",
                             "segment_lag_max_s", "playlist_violations",
                             "dropped_units", "transport_errors",
                             "queue_bytes", "queue_lag_ms",
                             "first_segment_latency_ms", "upload_duration_p95_ms"))
            for record in records:
                writer.writerow((record["destination"], record["bytes"],
                                 record["segments"], f"{record['rate_bps']:.2f}",
                                 f"{record['ratio']:.5f}",
                                 f"{record['window_byte_ratio']:.5f}",
                                 record["missing_segments"],
                                 record["duplicate_segments"],
                                 f"{record['lag_max_s']:.3f}",
                                 record["playlist_violations"],
                                 record["dropped_units"],
                                 record["transport_errors"], record["queue_bytes"],
                                 record["queue_lag_ms"],
                                 f"{record['first_segment_latency_ms']:.3f}",
                                 f"{percentile(record['upload_durations_ms'], 0.95):.3f}"))

    first_latencies = [record["first_segment_latency_ms"] for record in records]
    upload_durations = [
        duration_ms
        for record in records
        for duration_ms in record["upload_durations_ms"]
    ]
    queue_lags = [record["queue_lag_ms"] for record in records]
    http_errors = sink_after["http_errors"] - sink_before["http_errors"]
    if http_errors < 0:
        raise RuntimeError("HLS push sink HTTP error counter regressed")
    if http_errors:
        failures.append(f"sink returned {http_errors} HTTP errors")

    print(f"quality_pass={'no' if failures else 'yes'}")
    print(f"quality_destinations={len(records)}")
    print(f"quality_measurement_s={duration:.3f}")
    print(f"quality_reference_payload_bps={reference:.2f}")
    print(f"quality_average_delivery_ratio_min={min(ratios):.5f}")
    print("quality_ratio_basis=identified-segments")
    print(f"quality_window_byte_ratio_min={min(window_ratios):.5f}")
    print(f"quality_window_segments={segment_summary['window_segments']}")
    print(f"quality_window_segment_bytes={segment_summary['window_bytes']}")
    print(f"quality_segment_interval_s={segment_summary['segment_interval_s']:.3f}")
    print(f"quality_segment_lag_limit_s={segment_summary['lag_limit_s']:.3f}")
    print(f"quality_segment_lag_p95_s={segment_summary['lag_p95_s']:.3f}")
    print(f"quality_segment_lag_max_s={segment_summary['lag_max_s']:.3f}")
    print(f"quality_missing_segments={sum(r['missing_segments'] for r in records)}")
    print(f"quality_duplicate_segments={sum(r['duplicate_segments'] for r in records)}")
    print(f"quality_playlist_violations={sum(r['playlist_violations'] for r in records)}")
    print(f"quality_total_bytes={segment_summary['delivered_bytes']}")
    print(f"quality_total_segments={sum(r['segments'] for r in records)}")
    print(f"quality_dropped_units={sum(r['dropped_units'] for r in records)}")
    print(f"quality_transport_errors={sum(r['transport_errors'] for r in records)}")
    print(f"quality_sink_http_errors={http_errors}")
    print(f"quality_active_uploaders_before={before_active}")
    print(f"quality_active_uploaders_after={after_active}")
    print(f"quality_first_segment_latency_p50_ms={percentile(first_latencies, 0.50):.3f}")
    print(f"quality_first_segment_latency_p95_ms={percentile(first_latencies, 0.95):.3f}")
    print(f"quality_first_segment_latency_p99_ms={percentile(first_latencies, 0.99):.3f}")
    print(f"quality_all_destinations_ready_s={max(first_latencies) / 1000:.3f}")
    print(f"quality_upload_duration_p50_ms={percentile(upload_durations, 0.50):.3f}")
    print(f"quality_upload_duration_p95_ms={percentile(upload_durations, 0.95):.3f}")
    print(f"quality_upload_duration_p99_ms={percentile(upload_durations, 0.99):.3f}")
    print(f"quality_upload_duration_max_ms={max(upload_durations, default=0):.3f}")
    print(f"quality_queue_lag_p50_ms={percentile(queue_lags, 0.50)}")
    print(f"quality_queue_lag_p95_ms={percentile(queue_lags, 0.95)}")
    print(f"quality_queue_lag_p99_ms={percentile(queue_lags, 0.99)}")
    print(f"quality_max_queue_lag_ms={max(queue_lags)}")
    print(f"quality_failure_count={len(failures)}")
    for failure in failures[:12]:
        print(f"quality_failure={failure}")
    if len(failures) > 12:
        print(f"quality_failure_more={len(failures) - 12}")


def segment_accounting(args, sink_before, sink_after, records):
    """Per-destination delivery of the exact segment set of the window."""
    start = sink_before["monotonic"]
    end = sink_after["monotonic"]
    first_at = {}
    sizes = {}
    for record in records:
        detail = sink_after["destinations"][record["destination"]].get(
            "segment_detail", {})
        for name, entry in detail.items():
            if name not in first_at or entry["at"] < first_at[name]:
                first_at[name] = entry["at"]
            sizes.setdefault(name, []).append(entry["bytes"])
    produced = sorted(t for t in first_at.values())
    gaps = [b - a for a, b in zip(produced, produced[1:]) if b > a]
    interval = sorted(gaps)[len(gaps) // 2] if gaps else 0.0
    # A segment first seen within the last lag limit of the window may still
    # be on its way to slower destinations: it is outside the judged set.
    lag_limit = args.segment_lag_limit_s or max(2 * interval, 2.0)
    window = sorted(name for name, at in first_at.items()
                    if start <= at <= end - lag_limit)
    canonical = {name: max(set(v), key=v.count) for name, v in sizes.items()}
    window_bytes = sum(canonical[name] for name in window)
    lags = []
    delivered = 0
    for record in records:
        final = sink_after["destinations"][record["destination"]]
        detail = final.get("segment_detail", {})
        initial = sink_before["destinations"].get(record["destination"], {})
        got = 0
        missing = duplicates = mismatches = late = 0
        lag_max = 0.0
        for name in window:
            entry = detail.get(name)
            if entry is None:
                missing += 1
                continue
            if entry["count"] > 1:
                duplicates += 1
            if entry["bytes"] != canonical[name]:
                mismatches += 1
            lag = entry["at"] - first_at[name]
            lags.append(lag)
            lag_max = max(lag_max, lag)
            if lag > lag_limit:
                late += 1
            got += entry["bytes"]
        delivered += got
        record["segment_ratio"] = got / window_bytes if window_bytes else 0.0
        record["missing_segments"] = missing
        record["duplicate_segments"] = duplicates
        record["size_mismatches"] = mismatches
        record["late_segments"] = late
        record["lag_max_s"] = lag_max
        record["playlist_violations"] = (
            final.get("playlist_violations", 0)
            - initial.get("playlist_violations", 0))
    if not window:
        raise RuntimeError("no identified segment was produced inside the "
                           "window; lengthen the measurement")
    lags.sort()
    return {
        "window_segments": len(window),
        "window_bytes": window_bytes,
        "segment_interval_s": interval,
        "lag_limit_s": lag_limit,
        "lag_p95_s": lags[int((len(lags) - 1) * 0.95)] if lags else 0.0,
        "lag_max_s": lags[-1] if lags else 0.0,
        "delivered_bytes": delivered,
    }


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    sink_parser = subparsers.add_parser("serve")
    sink_parser.add_argument("--port", type=int, required=True)

    report_parser = subparsers.add_parser("report")
    report_parser.add_argument("--before-prefix", required=True)
    report_parser.add_argument("--after-prefix", required=True)
    report_parser.add_argument("--stream", required=True)
    report_parser.add_argument("--destinations", type=int, required=True)
    report_parser.add_argument("--destination-offset", type=int, default=0)
    report_parser.add_argument("--sink-before", required=True)
    report_parser.add_argument("--sink-after", required=True)
    report_parser.add_argument("--reference-bps", type=float, default=0)
    report_parser.add_argument("--min-delivery-ratio", type=float, default=0.95)
    report_parser.add_argument("--report")
    report_parser.add_argument("--segment-lag-limit-s", type=float, default=0,
                               help="0: twice the observed segment interval")
    readiness_parser = subparsers.add_parser("readiness-report")
    readiness_parser.add_argument("--metrics-prefix", required=True)
    readiness_parser.add_argument("--stream", required=True)
    readiness_parser.add_argument("--destinations", type=int, required=True)
    readiness_parser.add_argument("--destination-offset", type=int, default=0)
    readiness_parser.add_argument("--sink-snapshot", required=True)
    readiness_parser.add_argument("--progress", required=True)


    args = parser.parse_args()

    if args.command == "serve":
        serve(args.port)
    else:
        try:
            if args.command == "readiness-report":
                readiness_report(args)
            else:
                report(args)
        except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
            print(f"HLS push report failed: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

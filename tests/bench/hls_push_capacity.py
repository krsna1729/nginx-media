#!/usr/bin/env python3
"""Discarding HLS PUT sink and per-destination capacity-quality report."""

import argparse
import csv
import http.server
import json
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
        while remaining:
            chunk = self.rfile.read(min(65536, remaining))
            if not chunk:
                self._reply(400)
                return
            remaining -= len(chunk)
            received += len(chunk)

        if parsed.path.lower().endswith(".ts"):
            with self.server.destinations_lock:
                destination = parts[0]
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

    reference = args.reference_bps
    if reference <= 0:
        reference = records[0]["rate_bps"]
    if reference <= 0:
        raise RuntimeError("single-destination HLS push reference rate is zero")

    failures = []
    ratios = []
    for record in records:
        ratio = record["rate_bps"] / reference
        record["ratio"] = ratio
        ratios.append(ratio)
        if record["segments"] == 0:
            failures.append(f"{record['destination']}: no HLS segments delivered")
        if ratio < args.min_delivery_ratio:
            failures.append(
                f"{record['destination']}: delivery ratio {ratio:.5f} below "
                f"{args.min_delivery_ratio:.5f}"
            )
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
                             "delivery_ratio", "dropped_units", "transport_errors",
                             "queue_bytes", "queue_lag_ms",
                             "first_segment_latency_ms", "upload_duration_p95_ms"))
            for record in records:
                writer.writerow((record["destination"], record["bytes"],
                                 record["segments"], f"{record['rate_bps']:.2f}",
                                 f"{record['ratio']:.5f}", record["dropped_units"],
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

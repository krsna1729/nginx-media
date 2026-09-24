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
        if self.path == "/__health":
            self._reply(200)
            return

        parsed = urlsplit(self.path)
        if parsed.path == "/__ready":
            try:
                expected = int(parse_qs(parsed.query)["destinations"][0])
            except (KeyError, IndexError, ValueError):
                self._reply(400)
                return
            with self.server.destinations_lock:
                ready = len(self.server.ts_destinations) >= expected
            self._reply(200 if ready else 503)
            return

        if parsed.path == "/__snapshot":
            with self.server.destinations_lock:
                snapshot = {
                    "monotonic": time.monotonic(),
                    "destinations": {
                        destination: {
                            "ts_bytes": self.server.ts_bytes[destination],
                            "segments": self.server.ts_segments[destination],
                        }
                        for destination in self.server.ts_destinations
                    },
                }
            body = json.dumps(snapshot).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self._reply(404)

    def do_PUT(self):
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
                self.server.ts_destinations.add(parts[0])
                self.server.ts_bytes[parts[0]] = self.server.ts_bytes.get(parts[0], 0) + received
                self.server.ts_segments[parts[0]] = self.server.ts_segments.get(parts[0], 0) + 1
        self._reply(201)

    def _reply(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *_args):
        pass


def serve(port):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), HlsPushSink)
    server.daemon_threads = True
    server.destinations_lock = threading.Lock()
    server.ts_destinations = set()
    server.ts_bytes = {}
    server.ts_segments = {}
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
        records.append({
            "destination": destination,
            "bytes": delta_bytes,
            "segments": segments,
            "rate_bps": delta_bytes * 8 / duration,
            "dropped_units": dropped,
            "transport_errors": errors,
            "queue_bytes": final_metrics.get("queue_bytes", 0),
            "queue_lag_ms": final_metrics.get("queue_lag_ms", 0),
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
                             "queue_bytes", "queue_lag_ms"))
            for record in records:
                writer.writerow((record["destination"], record["bytes"],
                                 record["segments"], f"{record['rate_bps']:.2f}",
                                 f"{record['ratio']:.5f}", record["dropped_units"],
                                 record["transport_errors"], record["queue_bytes"],
                                 record["queue_lag_ms"]))

    print(f"quality_pass={'no' if failures else 'yes'}")
    print(f"quality_destinations={len(records)}")
    print(f"quality_measurement_s={duration:.3f}")
    print(f"quality_reference_payload_bps={reference:.2f}")
    print(f"quality_average_delivery_ratio_min={min(ratios):.5f}")
    print(f"quality_total_segments={sum(r['segments'] for r in records)}")
    print(f"quality_dropped_units={sum(r['dropped_units'] for r in records)}")
    print(f"quality_transport_errors={sum(r['transport_errors'] for r in records)}")
    print(f"quality_max_queue_lag_ms={max(r['queue_lag_ms'] for r in records)}")
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

    args = parser.parse_args()
    if args.command == "serve":
        serve(args.port)
    else:
        try:
            report(args)
        except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
            print(f"HLS push quality report failed: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

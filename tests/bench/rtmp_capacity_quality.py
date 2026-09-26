#!/usr/bin/env python3
"""Assess per-destination RTMP payload delivery from worker metric snapshots.

Three uses:

  scrape URL OUT          one receiver scrape, stamped with the monotonic
                          clock immediately before and after the request, so
                          the delivery rate divides by the interval that
                          actually brackets the byte counters rather than by
                          a duration the shell measured somewhere else.
  sample URL[,URL] BASE OUT
                          scrapes every --interval seconds until SIGTERM and
                          writes per-destination interval deltas, for the
                          short-interval and stall checks.
  (no subcommand)         the quality report.
"""

import argparse
import csv
import glob
import math
import os
import re
import signal
import sys
import time
import urllib.request
from threading import Event

STAMP_PREFIX = "# scrape_monotonic_ns "

METRIC = "nginx_media_source_payload_bytes_in_total"
QUEUE_METRIC = "nginx_media_egress_queue_bytes"
SAMPLE_RE = re.compile(
    r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{(.*?)\})?\s+"
    r"([-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)"
    r"(?:\s+\S+)?\s*$"
)
LABEL_RE = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:\\.|[^"\\])*)"')


def decode_label(value):
    return re.sub(r"\\([\\n\"])", lambda match: {"\\": "\\", "n": "\n", '"': '"'}[match.group(1)], value)


def read_snapshot(prefix):
    paths = sorted(glob.glob(prefix + ".*"))
    if not paths:
        raise ValueError(f"no worker metrics files match {prefix}.*")
    totals = {}
    for path in paths:
        with open(path, encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                match = SAMPLE_RE.match(line)
                if match is None:
                    # Other exposition lines are irrelevant; malformed target samples are not.
                    if line.startswith(METRIC):
                        raise ValueError(f"malformed {METRIC} sample in {path}:{line_number}")
                    continue
                name, raw_labels, raw_value = match.groups()
                if name != METRIC:
                    continue
                labels = {}
                if raw_labels:
                    for item in LABEL_RE.finditer(raw_labels):
                        key, value = item.groups()
                        if key in labels:
                            raise ValueError(f"duplicate label {key} in {path}:{line_number}")
                        labels[key] = decode_label(value)
                    # Ensure the complete label set was parsed, not a partial regex match.
                    if ",".join(f'{key}="{value}"' for key, value in labels.items()) != raw_labels:
                        # Compare after tokenization while allowing whitespace around commas.
                        remainder = LABEL_RE.sub("", raw_labels)
                        if remainder.strip(" ,\t"):
                            raise ValueError(f"malformed labels in {path}:{line_number}")
                try:
                    value = float(raw_value)
                except ValueError as error:
                    raise ValueError(f"invalid {METRIC} value in {path}:{line_number}") from error
                if not math.isfinite(value) or value < 0 or not value.is_integer():
                    raise ValueError(f"invalid {METRIC} counter in {path}:{line_number}")
                if (labels.get("application") == "live"
                        and labels.get("name") is not None
                        and labels.get("source") == labels.get("name")):
                    identifier = labels["name"]
                    totals[identifier] = totals.get(identifier, 0) + int(value)
    return totals


def fetch(url):
    start = time.monotonic_ns()
    with urllib.request.urlopen(url, timeout=10) as response:
        body = response.read().decode("utf-8")
    end = time.monotonic_ns()
    return start, end, body


def scrape(args):
    start, end, body = fetch(args.url)
    with open(args.output, "w", encoding="utf-8") as output:
        output.write(f"{STAMP_PREFIX}{start} {end}\n")
        output.write(body)
    return 0


def read_stamp(prefix):
    """Midpoint of the scrape that produced a snapshot, or None."""
    stamps = []
    for path in sorted(glob.glob(prefix + ".*")):
        with open(path, encoding="utf-8") as source:
            first = source.readline()
        if first.startswith(STAMP_PREFIX):
            start, end = (int(v) for v in first[len(STAMP_PREFIX):].split())
            stamps.append((start + end) // 2)
    if not stamps:
        return None
    if max(stamps) - min(stamps) > 1_000_000_000:
        raise ValueError(f"snapshot files for {prefix} are more than 1s apart")
    return sum(stamps) // len(stamps)


def parse_totals(blob):
    totals = {}
    for line in blob.splitlines():
        match = SAMPLE_RE.match(line.strip())
        if match is None or match.group(1) != METRIC:
            continue
        labels = dict(LABEL_RE.findall(match.group(2) or ""))
        if (labels.get("application") == "live" and labels.get("name")
                and labels.get("source") == labels.get("name")):
            totals[labels["name"]] = totals.get(labels["name"], 0) + int(float(match.group(3)))
    return totals


def sample(args):
    stop = Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    previous_ns = read_stamp(args.baseline_prefix)
    if previous_ns is None:
        raise ValueError("baseline snapshot carries no scrape timestamp")
    previous = read_snapshot(args.baseline_prefix)
    with open(args.output, "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(("destination_id", "start_ns", "end_ns",
                         "bytes_received", "total_bytes"))
        output.flush()
        urls = [u for u in args.url.split(",") if u]
        while not stop.wait(args.interval):
            # one scrape per receiver instance; the sample's time is the
            # midpoint of the whole set
            start = end = None
            current = {}
            for url in urls:
                one_start, one_end, blob = fetch(url)
                start = one_start if start is None else start
                end = one_end
                for identifier, value in parse_totals(blob).items():
                    current[identifier] = current.get(identifier, 0) + value
            current_ns = (start + end) // 2
            for identifier in sorted(previous):
                if identifier not in current:
                    continue
                delta = current[identifier] - previous[identifier]
                if delta < 0:
                    raise ValueError(f"counter decreased for {identifier}")
                writer.writerow((identifier, previous_ns, current_ns, delta,
                                 current[identifier]))
            output.flush()
            previous, previous_ns = current, current_ns
    return 0


def interval_check(args, expected, reference, before, after, before_ns,
                   after_ns, failures):
    """Short-interval delivery and stall duration, per destination."""
    rows = {}
    with open(args.intervals, newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            rows.setdefault(row["destination_id"], []).append(
                (int(row["start_ns"]), int(row["end_ns"]),
                 int(row["bytes_received"]), int(row["total_bytes"])))
    summary = {}
    sampling_gaps = {}
    for identifier in expected:
        if identifier not in before or identifier not in after:
            continue
        intervals = sorted(rows.get(identifier, []))
        if not intervals:
            failures.append(f"{identifier}: no short-interval samples")
            continue
        previous_end, previous_total = before_ns, before[identifier]
        low_start = None
        min_ratio = None
        longest_stall = 0.0
        stall_start = None
        raw = list(intervals)
        if after_ns > intervals[-1][1]:
            raw.append((intervals[-1][1], after_ns,
                        after[identifier] - intervals[-1][3],
                        after[identifier]))
        # A scrape that ran late is followed by one that comes early; the
        # short interval between them holds whatever TCP delivered in a
        # fraction of a second and says nothing about the rate.  Intervals
        # shorter than half the sampling period are merged into the next.
        spans = []
        pending = None
        for start_ns, end_ns, count, cumulative in raw:
            if pending is not None:
                start_ns = pending[0]
                count += pending[2]
                pending = None
            if (end_ns - start_ns) / 1e9 < args.sample_interval_s / 2:
                pending = (start_ns, end_ns, count, cumulative)
                continue
            spans.append((start_ns, end_ns, count, cumulative))
        if pending is not None:
            if spans:
                last = spans.pop()
                spans.append((last[0], pending[1], last[2] + pending[2],
                              pending[3]))
            else:
                spans.append(pending)
        for start_ns, end_ns, count, cumulative in spans:
            span = (end_ns - start_ns) / 1e9
            if span <= 0:
                continue
            if start_ns != previous_end or cumulative - previous_total != count:
                failures.append(f"{identifier}: interval counters do not reconcile")
            if span > args.max_interval_s:
                # the sampler, not the stream, was late: counted, and the
                # floor and stall rules below still apply across the span
                sampling_gaps[identifier] = max(
                    sampling_gaps.get(identifier, 0.0), span)
            ratio = (count * 8 / span) / reference
            min_ratio = ratio if min_ratio is None else min(min_ratio, ratio)
            if ratio < args.interval_floor:
                low_start = start_ns if low_start is None else low_start
                if (end_ns - low_start) / 1e9 > args.max_low_s:
                    failures.append(f"{identifier}: under-rate persisted "
                                    f"{(end_ns - low_start) / 1e9:.2f}s")
            else:
                low_start = None
            if count == 0:
                stall_start = start_ns if stall_start is None else stall_start
                longest_stall = max(longest_stall, (end_ns - stall_start) / 1e9)
            else:
                stall_start = None
            previous_end, previous_total = end_ns, cumulative
        summary[identifier] = (min_ratio or 0.0, longest_stall)
    if sampling_gaps:
        summary["__sampling_gap_max__"] = (max(sampling_gaps.values()),
                                           len(sampling_gaps))
    return summary


def read_queue_snapshot(prefix):
    paths = sorted(glob.glob(prefix + ".*"))
    if not paths:
        raise ValueError(f"no worker metrics files match {prefix}.*")
    values = {}
    for path in paths:
        with open(path, encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                match = SAMPLE_RE.match(line.strip())
                if match is None:
                    continue
                name, raw_labels, raw_value = match.groups()
                if name != QUEUE_METRIC:
                    continue
                labels = dict(LABEL_RE.findall(raw_labels or ""))
                if labels.get("protocol") != "rtmp":
                    continue
                identifier = labels.get("destination")
                if not identifier:
                    raise ValueError(
                        f"RTMP queue metric has no destination in {path}:{line_number}"
                    )
                try:
                    value = float(raw_value)
                except ValueError as error:
                    raise ValueError(
                        f"invalid RTMP queue metric in {path}:{line_number}"
                    ) from error
                if not math.isfinite(value) or value < 0 or not value.is_integer():
                    raise ValueError(
                        f"invalid RTMP queue byte gauge in {path}:{line_number}"
                    )
                values[identifier] = max(values.get(identifier, 0), int(value))
    return values


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[int((len(ordered) - 1) * fraction)] if ordered else 0


def report_queue_snapshot(prefix, label, expected):
    values = read_queue_snapshot(prefix)
    observed = [values[identifier] for identifier in expected
                if identifier in values]
    missing = [identifier for identifier in expected if identifier not in values]
    print(f"rtmp_queue_bytes_{label}_destinations={len(observed)}/{len(expected)}")
    print(f"rtmp_queue_bytes_{label}_total={sum(observed)}")
    print(f"rtmp_queue_bytes_{label}_p50={percentile(observed, 0.50)}")
    print(f"rtmp_queue_bytes_{label}_p95={percentile(observed, 0.95)}")
    print(f"rtmp_queue_bytes_{label}_max={max(observed, default=0)}")
    if missing:
        print(f"rtmp_queue_bytes_{label}_missing_ids={','.join(missing)}")


def expected_ids(programs, destinations, offset):
    if programs == 1:
        return [f"d{index:04d}" for index in range(offset, offset + destinations)]
    return [f"p{program:04d}-d{index:04d}"
            for program in range(programs)
            for index in range(offset, offset + destinations)]


def quality_report(args):
    before = read_snapshot(args.before_prefix)
    after = read_snapshot(args.after_prefix)
    expected = expected_ids(args.programs, args.destinations, args.destination_offset)
    before_ns = read_stamp(args.before_prefix)
    after_ns = read_stamp(args.after_prefix)
    if before_ns is not None and after_ns is not None:
        if after_ns <= before_ns:
            raise ValueError("receiver scrape timestamps did not advance")
        measurement_s = (after_ns - before_ns) / 1e9
        timing = "receiver-scrape-monotonic"
    else:
        measurement_s = args.measurement_s
        timing = "shell-window"
    failures = []
    rows = []
    rates = []
    total_bytes = 0
    for identifier in expected:
        if identifier not in before or identifier not in after:
            missing = []
            if identifier not in before:
                missing.append("before")
            if identifier not in after:
                missing.append("after")
            failures.append(f"{identifier}: missing metric sample in {','.join(missing)} snapshot")
            rows.append((identifier, before.get(identifier, ""), after.get(identifier, ""), "", "", ""))
            continue
        start, end = before[identifier], after[identifier]
        if end < start:
            raise ValueError(f"counter decreased for {identifier}: {start} -> {end}")
        delta = end - start
        rate = delta * 8.0 / measurement_s
        total_bytes += delta
        rates.append(rate)
        rows.append((identifier, start, end, delta, f"{rate:.2f}", ""))
        if delta <= 0:
            failures.append(f"{identifier}: no positive payload delivery")

    if args.reference_bps == 0:
        if len(expected) != 1:
            raise ValueError("reference-bps 0 requires exactly one expected destination")
        reference = rates[0] if rates else 0.0
        if reference <= 0:
            raise ValueError("cannot calibrate reference from a nonpositive delivery rate")
    else:
        reference = args.reference_bps

    result_rows = []
    for row in rows:
        identifier, start, end, delta, raw_rate, _ = row
        if raw_rate == "":
            result_rows.append((identifier, start, end, delta, raw_rate, ""))
            continue
        rate = float(raw_rate)
        ratio = rate / reference if reference > 0 else 0.0
        result_rows.append((identifier, start, end, delta, raw_rate, f"{ratio:.5f}"))
        if rate <= 0 or ratio < args.min_delivery_ratio:
            failures.append(f"{identifier}: delivery ratio {ratio:.5f} below {args.min_delivery_ratio:.5f}")

    with open(args.report, "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(("destination_id", "before_bytes", "after_bytes", "delivered_bytes",
                         "delivery_bps", "delivery_ratio"))
        writer.writerows(result_rows)

    interval_summary = {}
    if args.intervals:
        if before_ns is None or after_ns is None:
            raise ValueError("interval checks need timestamped snapshots")
        interval_summary = interval_check(args, expected, reference, before,
                                          after, before_ns, after_ns, failures)

    ratios = [float(row[5]) for row in result_rows if row[5] != ""]
    min_ratio = min(ratios, default=0.0)
    if len(ratios) != len(expected):
        min_ratio = 0.0
    aggregate_bps = total_bytes * 8.0 / measurement_s
    if args.queue_before_prefix or args.queue_after_prefix:
        if not args.queue_before_prefix or not args.queue_after_prefix:
            raise ValueError("both queue snapshot prefixes are required")
        report_queue_snapshot(args.queue_before_prefix, "start", expected)
        report_queue_snapshot(args.queue_after_prefix, "end", expected)

    unique_failures = list(dict.fromkeys(failures))
    print(f"quality_pass={'no' if unique_failures else 'yes'}")
    print(f"quality_reference_payload_bps={reference:.2f}")
    print(f"quality_min_delivery_ratio={min_ratio:.5f}")
    print(f"quality_measurement_s={measurement_s:.6f}")
    print(f"quality_shell_window_s={args.measurement_s:.6f}")
    print(f"quality_timing={timing}")
    if interval_summary:
        gap = interval_summary.pop("__sampling_gap_max__", None)
        print("quality_interval_delivery_ratio_min="
              f"{min(v[0] for v in interval_summary.values()):.5f}")
        print("quality_longest_stall_s="
              f"{max(v[1] for v in interval_summary.values()):.3f}")
        if gap is not None:
            print(f"quality_sampling_gap_max_s={gap[0]:.3f}")
            print(f"quality_sampling_gap_destinations={gap[1]}")
    print(f"quality_receiver_count={len(rates)}")
    print(f"quality_aggregate_bytes={total_bytes}")
    print(f"quality_aggregate_bps={aggregate_bps:.2f}")
    print(f"quality_failure_count={len(unique_failures)}")
    for failure in unique_failures:
        print(f"quality_failure={failure}")
    return 0


def positive_int(value):
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def nonnegative_int(value):
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return parsed


def positive_float(value):
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return parsed


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ("scrape", "sample"):
        sub = argparse.ArgumentParser()
        sub.add_argument("command")
        sub.add_argument("url")
        if sys.argv[1] == "sample":
            sub.add_argument("baseline_prefix")
        sub.add_argument("output")
        sub.add_argument("--interval", type=positive_float, default=1.0)
        args = sub.parse_args()
        try:
            return scrape(args) if args.command == "scrape" else sample(args)
        except (OSError, ValueError) as error:
            print(f"rtmp_capacity_quality: {error}", file=sys.stderr)
            return 1
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before-prefix", required=True)
    parser.add_argument("--after-prefix", required=True)
    parser.add_argument("--programs", type=positive_int, required=True)
    parser.add_argument("--destinations", type=positive_int, required=True)
    parser.add_argument("--destination-offset", type=nonnegative_int, default=0)
    parser.add_argument("--measurement-s", type=positive_float, required=True)
    parser.add_argument("--reference-bps", type=float, required=True)
    parser.add_argument("--min-delivery-ratio", type=float, default=0.95)
    parser.add_argument("--report", required=True)
    parser.add_argument("--queue-before-prefix")
    parser.add_argument("--queue-after-prefix")
    parser.add_argument("--intervals")
    parser.add_argument("--interval-floor", type=float, default=0.80)
    parser.add_argument("--max-low-s", type=float, default=2.0)
    parser.add_argument("--max-interval-s", type=float, default=2.0)
    parser.add_argument("--sample-interval-s", type=float, default=1.0)
    args = parser.parse_args()
    if not math.isfinite(args.reference_bps) or args.reference_bps < 0:
        parser.error("--reference-bps must be finite and nonnegative")
    if not math.isfinite(args.min_delivery_ratio) or not 0 <= args.min_delivery_ratio <= 1:
        parser.error("--min-delivery-ratio must be between 0 and 1")
    try:
        return quality_report(args)
    except (OSError, ValueError) as error:
        print(f"rtmp_capacity_quality: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

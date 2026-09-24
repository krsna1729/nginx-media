#!/usr/bin/env python3
"""Assess per-destination RTMP payload delivery from worker metric snapshots."""

import argparse
import csv
import glob
import math
import os
import re
import sys

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
        rate = delta * 8.0 / args.measurement_s
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

    ratios = [float(row[5]) for row in result_rows if row[5] != ""]
    min_ratio = min(ratios, default=0.0)
    if len(ratios) != len(expected):
        min_ratio = 0.0
    aggregate_bps = total_bytes * 8.0 / args.measurement_s
    if args.queue_before_prefix or args.queue_after_prefix:
        if not args.queue_before_prefix or not args.queue_after_prefix:
            raise ValueError("both queue snapshot prefixes are required")
        report_queue_snapshot(args.queue_before_prefix, "start", expected)
        report_queue_snapshot(args.queue_after_prefix, "end", expected)

    unique_failures = list(dict.fromkeys(failures))
    print(f"quality_pass={'no' if unique_failures else 'yes'}")
    print(f"quality_reference_payload_bps={reference:.2f}")
    print(f"quality_min_delivery_ratio={min_ratio:.5f}")
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

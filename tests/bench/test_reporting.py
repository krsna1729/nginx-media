#!/usr/bin/env python3
"""Tests for the capacity reporting pipeline: what it measures, and what it
refuses to call a measurement.

The two defects this suite exists to keep out of the published numbers are

  * a configured acceptance threshold reported as an observed delivery ratio
    (the HLS reader log's min_delivery_ratio is the gate, never a result), and
  * a sender-side byte counter standing in for receiver-counted delivery.

It also pins the completeness gate: a run that never started a workload, never
measured a rung it declared, or died half way is an incomplete run, while a
ladder that stopped on purpose after the capacity boundary is a finished one.

Run it with plain python3 - no pytest, no network, no nginx:

    python3 tests/bench/test_reporting.py
"""

import argparse
import copy
import csv
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
READERS = os.path.join(HERE, "hls_capacity_readers.py")
DIAGNOSTICS = os.path.join(HERE, "capacity_diagnostics.py")
HISTORY = os.path.join(HERE, "bench_history.py")
PREFLIGHT = os.path.join(HERE, "capacity_preflight.py")

checks = 0
failures = 0


def check(condition, message):
    global checks, failures
    checks += 1
    if not condition:
        failures += 1
        print(f"FAIL: {message}")


def check_close(actual, expected, message, tolerance=1e-6):
    check(isinstance(actual, (int, float))
          and abs(actual - expected) <= tolerance,
          f"{message}: expected {expected}, got {actual!r}")


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as output:
        output.write(text)


def write_kv(path, pairs):
    write(path, "".join(f"{key}={value}\n" for key, value in pairs.items()))


def run(args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, **kwargs)


def write_json(path, payload):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as output:
        json.dump(payload, output)


def preflight(*args):
    return run([sys.executable, PREFLIGHT] + list(args))


def test_preflight_names_a_foreign_load_on_the_pinned_cpus(work):
    """A run pinned to a CPU that something else is using is the host's
    number, not the software's, and the preflight has to say so before the
    rungs are read as a capacity - naming the CPU and the process."""
    case = os.path.join(work, "preflight-foreign")
    sender = os.path.join(case, "sender.json")
    write_json(sender, {"role": "sender", "permitted_cpus": [0, 1],
                        "cpu_model": "test"})
    busy = os.path.join(case, "busy.json")
    write_json(busy, {"busy": {"cpu0": 1.0, "cpu1": 0.02},
                      "consumers": [{"pid": 4242, "cpu_seconds": 1.0,
                                     "command": "bessd -k"}]})

    result = preflight("judge", "--destinations", "64", "--bitrate-bps",
                       "8000000", "--sender", sender,
                       "--benchmark-cpus", "0,1",
                       "--benchmark-cpu-busy-json", busy,
                       "--json", os.path.join(case, "judge.json"))
    check(result.returncode == 2,
          f"a busy pinned CPU is an infrastructure limit: {result.stdout}")
    check("preflight_limit=host/cpu" in result.stdout,
          f"the host must be the named side: {result.stdout}")
    with open(os.path.join(case, "judge.json"), encoding="utf-8") as source:
        judged = json.load(source)
    host_limits = [limit for limit in judged["limits"]
                   if limit["side"] == "host"]
    check(len(host_limits) == 1,
          f"the host must carry one limit: {judged.get('limits')}")
    reason = host_limits[0]["reason"] if host_limits else ""
    check("cpu0" in reason and "bessd" in reason,
          f"the CPU and the consumer must be named: {reason}")

    # a desktop's own spread is not a competing job: below half a CPU the
    # measurement stands
    write_json(busy, {"busy": {"cpu0": 0.2, "cpu1": 0.1}, "consumers": []})
    result = preflight("judge", "--destinations", "64", "--bitrate-bps",
                       "8000000", "--sender", sender,
                       "--benchmark-cpus", "0,1",
                       "--benchmark-cpu-busy-json", busy,
                       "--json", os.path.join(case, "judge2.json"))
    check("preflight_limit=host/cpu" not in result.stdout,
          f"a busy desktop is not a competing job: {result.stdout}")


def test_preflight_classifies_a_short_environment(work):
    """The preflight's whole job: say which side is short, and never turn an
    environment that cannot carry the load into a result about the sender."""
    case = os.path.join(work, "preflight")
    sender = os.path.join(case, "sender.json")
    rx = os.path.join(case, "rx.json")
    tx = os.path.join(case, "tx.json")
    write_json(sender, {"role": "sender", "permitted_cpus": [0, 1, 2, 3],
                        "cpu_model": "test",
                        "memory": {"MemAvailable": "8000000 kB"}})
    # the receiver counted 5.36 Gbit/s and dropped datagrams: the receiver
    # side is short for a 9.6 Gbit/s request
    write_json(rx, {"side": "receiver", "received_bytes": 6_700_000_000,
                    "received_packets": 4_700_000, "window_s": 10.0,
                    "socket_drops": 178465, "rate_gbps": 5.36})
    write_json(tx, {"side": "sender", "sent_bytes": 7_300_000_000,
                    "sent_packets": 5_200_000, "send_window_s": 10.0,
                    "ping_loss": 0.0})

    result = preflight("judge", "--destinations", "1000", "--bitrate-bps",
                       "8000000", "--sender", sender, "--network", rx,
                       "--probe-sender", tx, "--json",
                       os.path.join(case, "judge.json"))
    # The probe's own receiver dropped datagrams, so the probe cannot say
    # whether the path carries more: that is evidence about the probe, and
    # the measurement it did take is still reported.
    check(result.returncode == 3,
          f"a probe that dropped its own datagrams is not a limit: "
          f"{result.stdout}")
    check("preflight_unknown=probe/receiver-socket-drops" in result.stdout,
          f"the probe's own limit must be named: {result.stdout}")
    with open(os.path.join(case, "judge.json"), encoding="utf-8") as source:
        judged = json.load(source)
    check_close(judged["request"]["required_network_gbps"], 9.6,
                "the request's network need must be explicit")
    check_close(judged["request"]["measured_network_gbps"], 5.36,
                "the measurement it did take is reported")

    # ... but a measured cost per delivered Gbit/s that does not fit the
    # permitted CPUs is a limit, and that is the number the ladder feeds it
    # from the rung that ran before
    result = preflight("judge", "--destinations", "1000", "--bitrate-bps",
                       "8000000", "--sender", sender, "--network", rx,
                       "--probe-sender", tx, "--sender-cpu-per-gbps", "140")
    check(result.returncode == 2,
          f"a sender that cannot afford the load is a limit: {result.stdout}")
    check("preflight_limit=sender/cpu" in result.stdout,
          f"the short side must be the sender: {result.stdout}")

    # a measured cost with no fingerprint to spend it on is not a verdict:
    # the receiver's CPU cannot be judged without the receiver's host
    result = preflight("judge", "--destinations", "64", "--bitrate-bps",
                       "8000000", "--sender", sender, "--network", rx,
                       "--probe-sender", tx, "--sender-cpu-per-gbps", "140",
                       "--receiver-cpu-per-gbps", "30")
    check(result.returncode == 3,
          f"a cost without a host is insufficient evidence: {result.stdout}")
    check("preflight_unknown=receiver/cpu" in result.stdout,
          f"the missing side must be named: {result.stdout}")

    # the same measurements, with both hosts, for a request that fits
    receiver = os.path.join(case, "receiver.json")
    write_json(receiver, {"role": "receiver", "permitted_cpus": [2, 3],
                          "cpu_model": "test"})
    result = preflight("judge", "--destinations", "64", "--bitrate-bps",
                       "8000000", "--sender", sender, "--receiver", receiver,
                       "--network", rx, "--probe-sender", tx,
                       "--sender-cpu-per-gbps", "140",
                       "--receiver-cpu-per-gbps", "30")
    check(result.returncode == 0, f"a load that fits must pass: {result.stdout}")

    # nothing dropped, but the probe never offered the requested load: that
    # is a limit of the measurement, not a statement about the path
    write_json(rx, {"side": "receiver", "received_bytes": 6_700_000_000,
                    "window_s": 10.0, "socket_drops": 0, "rate_gbps": 5.36})
    write_json(tx, {"side": "sender", "sent_bytes": 6_700_000_000,
                    "send_window_s": 10.0})
    result = preflight("judge", "--destinations", "1000", "--bitrate-bps",
                       "8000000", "--sender", sender, "--network", rx,
                       "--probe-sender", tx)
    check(result.returncode == 3,
          f"an unoffered load must be insufficient evidence: {result.stdout}")
    check("preflight_unknown=network/throughput" in result.stdout,
          f"the unknown must be explicit: {result.stdout}")

    # the path carried the offered load but the sender has no measured cost
    # per Gbit/s: a core count is not a capacity, so it is not a pass
    result = preflight("judge", "--destinations", "64", "--bitrate-bps",
                       "8000000", "--sender", sender, "--network", rx,
                       "--probe-sender", tx)
    check(result.returncode == 3,
          f"unknown CPU cost must not pass: {result.stdout}")
    check("preflight_unknown=sender/cpu" in result.stdout,
          f"the unknown CPU must be reported: {result.stdout}")

    # and the receiver's CPU, when a measurement says what it costs
    result = preflight("judge", "--destinations", "1000", "--bitrate-bps",
                       "8000000", "--sender", sender, "--network", rx,
                       "--probe-sender", tx, "--receiver", sender,
                       "--receiver-cpu-per-gbps", "200")
    check(result.returncode == 2,
          f"an unaffordable receiver must be a limit: {result.stdout}")
    check("preflight_limit=receiver/cpu" in result.stdout,
          f"the short side must be the receiver: {result.stdout}")


def test_preflight_probe_is_measured_at_both_ends(work):
    """A path probe over loopback: the receiver's count is the measurement,
    the sender's count says the load was offered, and neither is inferred."""
    case = os.path.join(work, "probe")
    os.makedirs(case, exist_ok=True)
    port = 20977
    rx = os.path.join(case, "rx.json")
    listener = subprocess.Popen(
        [sys.executable, PREFLIGHT, "probe", "--listen", "--port", str(port),
         "--seconds", "4", "--json", rx],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.5)
    tx = os.path.join(case, "tx.json")
    result = preflight("probe", "--peer", "127.0.0.1", "--port", str(port),
                       "--seconds", "2", "--json", tx)
    out, err = listener.communicate(timeout=30)
    check(listener.returncode == 0, f"listener failed: {err.strip()}")
    check(result.returncode == 0, f"sender probe failed: {result.stderr}")
    with open(rx, encoding="utf-8") as source:
        received = json.load(source)
    with open(tx, encoding="utf-8") as source:
        sent = json.load(source)
    check(received["received_bytes"] > 0, "the receiver must count bytes")
    check(sent["sent_bytes"] > 0, "the sender must count bytes")
    check_close(sent["ping_loss"], 0.0, "loopback loses no pings")
    check(received["received_bytes"] <= sent["sent_bytes"],
          "a receiver cannot count more than was sent")
    check(received["window_s"] > 0, "the receiver's own window must be recorded")


# --- the HLS reader benchmark ------------------------------------------------

def ts_packet():
    """188 bytes of plausible MPEG-TS: the reader validates the sync byte and
    the packet length, and nothing else."""
    body = bytes([0x47]) + bytes(range(1, 188))
    return body


SEGMENT = ts_packet() * 8
POLL = 0.2


class PlaylistHandler(BaseHTTPRequestHandler):
    """A playlist that keeps producing segments, so readers have something to
    measure after warmup."""

    protocol_version = "HTTP/1.1"  # the readers reuse their connection
    started = None
    seconds_per_segment = 0.1

    def do_GET(self):  # noqa: N802 - http.server's interface
        if self.path.startswith("/media/seg"):
            self.send_response(200)
            self.send_header("Content-Type", "video/mp2t")
            self.send_header("Content-Length", str(len(SEGMENT)))
            self.end_headers()
            self.wfile.write(SEGMENT)
            return
        first = 0
        count = 1
        if self.started is not None:
            elapsed = time.monotonic() - self.started
            count = max(1, int(elapsed / self.seconds_per_segment) + 1)
            first = max(0, count - 12)
        body = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:1",
                f"#EXT-X-MEDIA-SEQUENCE:{first}"]
        for index in range(first, first + count):
            body.append("#EXTINF:0.100,")
            body.append(f"seg{index:05d}.ts")
        payload = ("\n".join(body) + "\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.apple.mpegurl")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


def test_reader_reports_observed_not_threshold(work):
    """The reader benchmark's reported minimum must be the readers' own
    observation, and must be distinguishable from the configured gate."""
    os.makedirs(work, exist_ok=True)
    PlaylistHandler.started = None
    server = ThreadingHTTPServer(("127.0.0.1", 0), PlaylistHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        report = os.path.join(work, "hls-readers.csv")
        ready = os.path.join(work, "hls.ready")
        url = f"http://127.0.0.1:{server.server_port}/media/playlist.m3u8"
        # a reference far below what one reader delivers, so the observed
        # ratios sit well above the gate and a threshold cannot be mistaken
        # for an observation
        args = [sys.executable, READERS, "--url", url, "--readers", "2",
                "--duration", "2", "--report", report, "--ready-file", ready,
                "--reference-bps", "1", "--min-delivery-ratio", "0.95",
                "--poll-interval", str(POLL)]
        process = subprocess.Popen(args, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 30
        while not os.path.exists(ready) and time.monotonic() < deadline:
            if process.poll() is not None:
                break
            time.sleep(0.05)
        if os.path.exists(ready):
            PlaylistHandler.started = time.monotonic()
            process.send_signal(10)  # SIGUSR1 starts the measurement
        out, err = process.communicate(timeout=60)
        check(process.returncode == 0,
              f"reader benchmark exited {process.returncode}: {err.strip()}")
        values = dict(line.split("=", 1) for line in out.splitlines()
                      if "=" in line)
        check(values.get("delivery_ratio_threshold") == "0.950000",
              f"threshold reported as {values.get('delivery_ratio_threshold')}")
        check(values.get("min_delivery_ratio") == "0.950000",
              "min_delivery_ratio must keep its original threshold meaning")
        observed = float(values.get("observed_min_delivery_ratio", "nan"))
        check(observed > 1.0,
              f"observed minimum {observed} should be far above the 0.95 gate")
        check(values.get("observed_ratio_basis") == "reader-counted-bytes",
              "the observed ratio must declare its basis")
        percentiles = [values.get(name) for name in
                       ("observed_p5_delivery_ratio",
                        "observed_p50_delivery_ratio",
                        "observed_p95_delivery_ratio")]
        check(all(value is not None for value in percentiles),
              f"observed percentiles missing from the reader report: {values}")
        if all(value is not None for value in percentiles):
            ordered = [float(value) for value in percentiles]
            check(ordered == sorted(ordered),
                  f"percentiles must be ordered, got {ordered}")
        with open(report, newline="", encoding="utf-8") as source:
            rows = list(csv.DictReader(source))
        ratios = sorted(float(row["reference_ratio"]) for row in rows)
        check(len(ratios) == 2, f"expected two reader rows, got {len(ratios)}")
        check_close(observed, ratios[0],
                    "observed minimum must be the CSV's lowest reader ratio",
                    tolerance=1e-6)
        check_close(float(values["receiver_bytes_total"]),
                    sum(int(row["bytes_received"]) for row in rows),
                    "receiver_bytes_total must be the readers' own byte count")
        check(float(values["receiver_measurement_s"]) > 0,
              "the receiver measurement interval must be reported")
        check_close(float(values["delivered_gbps"]),
                    float(values["receiver_bytes_total"]) * 8
                    / float(values["receiver_measurement_s"]) / 1e9,
                    "delivered Gbit/s must come from receiver bytes and the "
                    "receiver interval", tolerance=1e-6)
    finally:
        server.shutdown()
        server.server_close()


def test_reader_percentile_helpers():
    module = load_module(READERS, "hls_capacity_readers_under_test")
    check(module.percentile([], 0.5) is None, "empty percentile is None")
    check(module.percentile([0.5], 0.95) == 0.5, "single sample percentile")
    values = [i / 100 for i in range(1, 101)]
    check_close(module.percentile(values, 0.05), 0.05,
                "p5 of a uniform list is its fifth sample")
    check_close(module.percentile(values, 0.95), 0.95,
                "p95 of a uniform list is its ninety-fifth sample")

    class Item:
        def __init__(self, reader_id, rate):
            self.reader_id = reader_id
            self.rate_bps = rate

    stats = [Item(7, 100.0), Item(9, 40.0), Item(11, 80.0)]
    observed = module.delivery_ratios(stats, 100.0)
    check_close(observed["min"], 0.40, "min ratio is the slowest reader")
    check(observed["min_reader_id"] == 9,
          f"min ratio must name its reader, got {observed['min_reader_id']}")


# --- diagnostics bundles -----------------------------------------------------

def srt_case(work):
    write_kv(os.path.join(work, "quality-report.txt"), {
        "quality_pass": "yes",
        "quality_measurement_s": "10.000",
        "quality_total_receiver_bytes": "5000000",
        "quality_average_delivery_ratio_min": "0.998",
    })


def rtmp_case(work):
    write_kv(os.path.join(work, "rtmp-quality.txt"), {
        "quality_pass": "yes",
        "quality_measurement_s": "10.000",
        "quality_aggregate_bytes": "2500000",
        "quality_min_delivery_ratio": "0.995",
    })


def hls_push_case(work):
    write_kv(os.path.join(work, "hls-push-quality.txt"), {
        "quality_pass": "yes",
        "quality_measurement_s": "10.000",
        "quality_total_bytes": "1000000",
        "quality_average_delivery_ratio_min": "0.990",
    })


def hls_reader_case(work, ratios=(0.99, 0.97, 0.93), threshold=0.95,
                    durations=(10.0, 10.0, 10.0), bytes_each=(2000000, 0, 0),
                    log_observed=False, rows=None):
    """One HLS-origin case: the reader log plus the per-reader CSV."""
    log = {"quality_pass": "yes", "readers": len(ratios),
           "duration_s": "10.000", "reference_bps": "2000000.00",
           "delivery_ratio_threshold": f"{threshold:.6f}",
           "min_delivery_ratio": f"{threshold:.6f}",
           "total_bytes": "6000000"}
    if log_observed:
        log["observed_min_delivery_ratio"] = f"{min(ratios):.6f}"
    write_kv(os.path.join(work, "hls-reader.log"), log)
    if rows is None:
        rows = [{"reader_id": index, "bytes_received": received,
                 "reference_ratio": f"{ratio:.6f}",
                 "duration_s": f"{duration:.6f}"}
                for index, (ratio, duration, received)
                in enumerate(zip(ratios, durations, bytes_each))]
    path = os.path.join(work, "hls-readers.csv")
    os.makedirs(work, exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=("reader_id",
                                                    "bytes_received",
                                                    "reference_ratio",
                                                    "duration_s"))
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def bundle_for(case_dir):
    module = load_module(DIAGNOSTICS, "capacity_diagnostics_under_test")
    delivery = module.delivery_section(case_dir)
    bundle = {"intervals": {"media_measurement": {"value": 30.0}},
              "cpu": {"by_kind_pct_of_core": {"value": {"worker": 100.0,
                                                        "hls-readers": 20.0,
                                                        "srt-receiver": 10.0}}},
              "delivery": delivery}
    bundle["efficiency"] = module.efficiency_section(bundle)
    return module, bundle


def test_hls_origin_bundle_uses_observed_ratios(work):
    case = os.path.join(work, "hls-origin")
    hls_reader_case(case)
    module, bundle = bundle_for(case)
    report = bundle["delivery"]["hls_readers"]["value"]
    check_close(report["observed_min_delivery_ratio"], 0.93,
                "observed minimum comes from the CSV, not the gate")
    check_close(report["observed_p50_delivery_ratio"], 0.97,
                "median observed ratio comes from the CSV")
    check_close(report["delivery_ratio_threshold"], 0.95,
                "the configured threshold is reported as a threshold")
    check_close(report["min_delivery_ratio"], 0.95,
                "the log's original field keeps its threshold meaning")
    check_close(report["receiver_bytes_total"], 2000000,
                "receiver bytes are the readers' own count")
    check_close(report["receiver_measurement_s"], 10.0,
                "the reader's own measurement interval is used")
    check_close(report["delivered_gbps"], 2000000 * 8 / 10 / 1e9,
                "delivered Gbit/s for HLS origin")
    check(report["observed_ratio_basis"] == "reader-counted-bytes",
          "the observed basis must be explicit")
    check_close(module.observed_ratio(bundle), 0.93,
                "the bundle's observed ratio is the CSV minimum")
    check_close(module.threshold_ratio(bundle), 0.95,
                "the bundle's threshold is the configured gate")
    check(module.ratio_basis(bundle) == "observed",
          f"a bundle whose only workload measured is observed, got "
          f"{module.ratio_basis(bundle)}")


def test_old_bundle_threshold_is_never_a_measurement(work):
    """A bundle written before the reader reported observations carries the
    gate in min_delivery_ratio and nothing measured.  It must not be summarized
    as a 95% delivery."""
    case = os.path.join(work, "hls-origin-old")
    hls_reader_case(case, log_observed=False)
    os.remove(os.path.join(case, "hls-readers.csv"))
    module, bundle = bundle_for(case)
    report = bundle["delivery"]["hls_readers"]["value"]
    check("observed_min_delivery_ratio" not in report,
          "an old log has no observation to report")
    check_close(module.threshold_ratio(bundle), 0.95,
                "its threshold is still reported as a threshold")
    check(module.observed_ratio(bundle) is None,
          "an old log must not yield an observed ratio")
    check(module.ratio_basis(bundle) == "threshold-only",
          f"basis must say the bundle has no measurement, got "
          f"{module.ratio_basis(bundle)}")


def test_absent_and_malformed_inputs(work):
    empty = os.path.join(work, "empty")
    os.makedirs(empty, exist_ok=True)
    module = load_module(DIAGNOSTICS, "capacity_diagnostics_absent")
    check(module.delivery_section(empty) == {"status": "unavailable",
                                             "reason": "no quality reports"},
          "a case with no reports is unavailable, never zero")
    check(module.hls_reader_stats(empty) is None,
          "no CSV means no reader statistics")

    malformed = os.path.join(work, "malformed")
    hls_reader_case(malformed, rows=[
        {"reader_id": 0, "bytes_received": 2000000,
         "reference_ratio": "0.990000", "duration_s": "10.000000"},
        {"reader_id": 1, "bytes_received": "", "reference_ratio": "not-a-number",
         "duration_s": "10.000000"},
        {"reader_id": 2, "bytes_received": 2000000,
         "reference_ratio": "0.930000", "duration_s": "10.000000"},
    ])
    stats = module.hls_reader_stats(malformed)
    check_close(stats["observed_min_delivery_ratio"], 0.93,
                "a malformed row is skipped, not read as zero")
    check(stats["unreadable_reader_rows"] == 1,
          f"the unreadable row must be counted, got "
          f"{stats.get('unreadable_reader_rows')}")
    check_close(stats["receiver_bytes_total"], 4000000,
                "unreadable rows contribute no bytes")

    # a CSV with no usable rows at all must not produce statistics
    unusable = os.path.join(work, "unusable")
    hls_reader_case(unusable, rows=[
        {"reader_id": 0, "bytes_received": "x", "reference_ratio": "",
         "duration_s": ""}])
    check(module.hls_reader_stats(unusable) is None,
          "no usable row means no statistics")

    # a truncated file (no header, unparsable) is not a crash
    truncated = os.path.join(work, "truncated")
    write(os.path.join(truncated, "hls-readers.csv"), "not,a,header\n")
    check(module.hls_reader_stats(truncated) is None,
          "an unreadable CSV is unavailable, not an exception")


def test_bundle_carries_the_host_fingerprint(work):
    """A rung's numbers are unreadable without the host they were taken on:
    CPU, cgroup, affinity, interfaces, transport library.  The assertion is on
    the built bundle, not on the file this test wrote."""
    case = os.path.join(work, "fingerprint")
    os.makedirs(case, exist_ok=True)
    srt_case(case)
    module = load_module(DIAGNOSTICS, "capacity_diagnostics_fingerprint")
    fingerprint = {
        "role": "sender", "cpu_model": "test-cpu", "permitted_cpus": [0, 1],
        "cgroup": {"cpu_max": "max 100000"}, "transport": "libsrt.so.1",
        "interfaces": {"eth0": {"speed_mbps": "25000"}},
    }
    write_json(os.path.join(case, "host-fingerprint.json"), fingerprint)

    bundle_path = os.path.join(case, "diagnostics.json")
    args = argparse.Namespace(case_dir=case, meta=[], command="build",
                              out=bundle_path, run_dir=None,
                              print_summary=False)
    module.build(args)
    with open(bundle_path, encoding="utf-8") as source:
        built = json.load(source)
    check(built["host_fingerprint"] == fingerprint,
          f"the bundle must carry the fingerprint it was given: "
          f"{built.get('host_fingerprint')}")

    # and a case without one reports absence rather than inventing a host
    bare = os.path.join(work, "fingerprint-bare")
    os.makedirs(bare, exist_ok=True)
    srt_case(bare)
    bare_path = os.path.join(bare, "diagnostics.json")
    args.case_dir = bare
    args.out = bare_path
    module.build(args)
    with open(bare_path, encoding="utf-8") as source:
        built = json.load(source)
    check(built["host_fingerprint"]["status"] == "unavailable"
          and built["host_fingerprint"]["value"] is None,
          f"no fingerprint file means no fingerprint: "
          f"{built.get('host_fingerprint')}")


def test_efficiency_uses_receiver_bytes_for_every_protocol(work):
    case = os.path.join(work, "all-protocols")
    srt_case(case)
    rtmp_case(case)
    hls_push_case(case)
    hls_reader_case(case)
    module, bundle = bundle_for(case)
    delivered = bundle["efficiency"]["delivered"]
    expected = (5000000 + 2500000 + 1000000 + 2000000) * 8 / 10 / 1e9
    check_close(delivered["value"], expected,
                "delivered Gbit/s sums every protocol's receiver bytes over "
                "its own receiver interval")
    check(any(source.startswith("hls_readers") for source in delivered["sources"]),
          f"HLS origin must contribute to delivered Gbit/s: {delivered['sources']}")
    check_close(bundle["efficiency"]["receiver_cpu_per_gbps"]["value"],
                30.0 / expected, "receiver CPU per Gbit/s", tolerance=0.01)
    check_close(bundle["efficiency"]["sender_cpu_per_gbps"]["value"],
                100.0 / expected, "sender CPU per Gbit/s", tolerance=0.01)


def test_efficiency_refuses_a_sender_only_counter(work):
    """A case whose only byte number is sender-side must report no efficiency,
    not a cheap one: queued bytes are not delivery."""
    case = os.path.join(work, "sender-only")
    write_kv(os.path.join(case, "quality-report.txt"), {
        "quality_pass": "yes",
        "quality_measurement_s": "10.000",
        "quality_sender_queued_bytes": "5000000",
        "quality_average_delivery_ratio_min": "0.998",
    })
    module = load_module(DIAGNOSTICS, "capacity_diagnostics_sender_only")
    bundle = {"intervals": {"media_measurement": {"value": 30.0}},
              "cpu": {"by_kind_pct_of_core": {"value": {"worker": 100.0}}},
              "delivery": module.delivery_section(case)}
    efficiency = module.efficiency_section(bundle)
    check(efficiency.get("status") == "unavailable",
          f"sender-queued bytes must not become delivered Gbit/s: {efficiency}")


# --- history and the completeness gate ---------------------------------------

RUNNER = {"cpu_model": "runner-a", "kernel": "7.2", "nproc_online": 4,
          "transport": "libsrt.so.1 1.5.4 (/usr/lib/libsrt.so.1.5)"}

# the recipe a published record carries for one configuration: how its rungs
# were taken, as bench_history.completeness derives it from the harness log
RECIPE = {"seconds": 15, "rate": "8M", "steps": [1, 16, 32, 64, 128],
          "stop_after_failures": 2, "min_delivery_ratio": 0.95,
          "interval_floor": 0.8, "max_low_seconds": 2,
          "srt_workers": "adaptive", "hls_push_workers": "adaptive",
          "srt_hls": "yes", "mixes": ["pure-srt"], "harness": "harness-a"}


def diagnostics_bundle(destinations, outcome, delivery, mix="pure-srt",
                       host=None, cpu=100.0):
    bundle = {"schema": "nginx-media.capacity-diagnostics/2",
              "case": {"destinations": destinations, "mix": mix},
              "outcome": outcome, "delivery": delivery,
              "cpu_environment": {"host_cpu_busy_fraction": {"value": 0.5}},
              "efficiency": {"delivered": {"value": 1.0},
                             "sender_cpu_per_gbps": {"value": cpu},
                             "receiver_cpu_per_gbps": {"value": 50.0}}}
    if host is not None:
        bundle["host_fingerprint"] = host
    return bundle


def srt_delivery(ratio):
    return {"srt": {"value": {"quality_average_delivery_ratio_min": ratio}}}


def hls_origin_delivery(threshold, observed=None):
    value = {"min_delivery_ratio": threshold,
             "delivery_ratio_threshold": threshold}
    if observed is not None:
        value["observed_min_delivery_ratio"] = observed
    return {"hls_readers": {"value": value}}


def make_config(results, name, rungs, status=0, finished=True, expected=None,
                stopped=None, ran_mixes=None, recipe=None, host=None,
                cpu=100.0):
    """One configuration's results directory, as bench-ci.sh leaves it.

    A rung is (destinations, outcome, delivery) or, for a workload other
    than pure-srt, (destinations, outcome, delivery, mix).  The log carries
    what the real harness prints: the mixes it executed (ran_mixes; default
    the manifest's; "silent" prints none) and its recipe line (recipe=False
    prints none; a dict overrides fields)."""
    base = os.path.join(results, name)
    os.makedirs(base, exist_ok=True)
    expected = expected or {"mixes": ["pure-srt"], "steps": [1, 16, 32]}
    for rung in rungs:
        destinations, outcome, delivery = rung[:3]
        mix = rung[3] if len(rung) > 3 else "pure-srt"
        case = os.path.join(base, "capacity",
                            f"quality-{mix}-{destinations}")
        os.makedirs(case, exist_ok=True)
        with open(os.path.join(case, "diagnostics.json"), "w",
                  encoding="utf-8") as output:
            json.dump(diagnostics_bundle(destinations, outcome, delivery,
                                         mix, host, cpu), output)
    with open(os.path.join(results, f"{name}.log"), "w",
              encoding="utf-8") as output:
        if ran_mixes != "silent":
            output.write("quality_mixes=" + " ".join(
                expected["mixes"] if ran_mixes is None else ran_mixes) + "\n")
        if recipe is not False:
            fields = {"seconds": 10, "rate": "8M",
                      "steps": ",".join(str(s) for s in expected["steps"]),
                      "stop_after_failures": 2, "min_delivery_ratio": 0.95,
                      "interval_floor": 0.80, "max_low_seconds": 2,
                      "srt_workers": "adaptive", "hls_push_workers": "adaptive",
                      "srt_hls": "yes"}
            fields.update(recipe or {})
            output.write("quality_recipe " + " ".join(
                f"{key}={value}" for key, value in fields.items()) + "\n")
        for mix, skipped in (stopped or {}).items():
            output.write(f"mix={mix} unmeasured_rungs={skipped} "
                         f"(after 2 consecutive failures)\n")
        if finished:
            output.write("quality_ladders_with_failures=none\n")
    if status is not None:
        with open(os.path.join(results, f"{name}.status"), "w",
                  encoding="utf-8") as output:
            output.write(f"{status}\n")
    with open(os.path.join(base, "expected.json"), "w", encoding="utf-8") as out:
        json.dump(expected, out)
    return base


def summarize(results, out, expected_configs=None, tier="pr", env=None):
    command = [sys.executable, HISTORY, "summarize", results, "--tier", tier,
               "--out", out]
    if expected_configs is not None:
        command.extend(["--expected-configs", expected_configs])
    return run(command, env=env)


def gate(summary, tier="pr", history=None):
    command = [sys.executable, HISTORY, "gate", summary, "--tier", tier]
    if history is not None:
        command.extend(["--history", history])
    return run(command)


def test_history_carries_the_environment_and_peak_pressure(work):
    """A history record has to say which host produced the number and how
    hard that host was pushed."""
    module = load_module(HISTORY, "bench_history_peak")
    rung = module.rung_record({
        "case": {"destinations": 128}, "outcome": "pass",
        "host_fingerprint": {"cpu_model": "test-cpu", "kernel": "7.2.5",
                             "permitted_cpus": [0, 2], "transport": "libsrt.so.1",
                             "interfaces": {"eth0": {"speed_mbps": "25000"}}},
        "cpu": {"by_kind_pct_of_core": {"value": {"worker": 490.5,
                                                  "srt-receiver": 418.2,
                                                  "publisher": 1.6}}},
        "network": {"udp_socket_drops_by_process": {"value": {
            "w0": {"kind": "worker", "socket_drops_delta": 12},
            "r0": {"kind": "srt-receiver", "socket_drops_delta": 390737}}}},
        "event_loop_max_delay": {"value": {"w0": 41.0, "w1": 116.0}},
        "resources": {"worker_memory": {"value": {
            "1": {"after": {"rss_kb": 250000}}}}},
    })
    peak = rung["peak"]
    check_close(peak["sender_cpu_pct_of_core"], 490.5, "sender peak CPU")
    check_close(peak["receiver_cpu_pct_of_core"], 418.2, "receiver peak CPU")
    check(peak["socket_drops"] == 390749,
          f"every socket's drops count, not only the sender's: {peak}")
    check_close(peak["event_loop_max_delay_ms"], 116.0, "worst event loop slip")
    check(peak["worker_rss_kb_max"] == 250000, "peak worker memory")
    check(rung["host_fingerprint"]["cpu_model"] == "test-cpu",
          "the rung keeps the environment it was taken in")

    # the run-level record lifts the fingerprint and the maxima
    entry = {"rungs": [rung, dict(rung, peak=dict(peak, socket_drops=5))]}
    entry = module.run_environment(entry)
    check(entry["fingerprint"]["cpu_model"] == "test-cpu",
          f"the run carries the host: {entry.get('fingerprint')}")
    check(entry["peak"]["socket_drops"] == 390749,
          f"the run's peak is the worst rung: {entry.get('peak')}")


def test_history_carries_the_strict_result_separately(work):
    """The 0.95 gate and the strict full-rate qualification are different
    questions; a record must carry both, and must not imply a pass it never
    measured."""
    module = load_module(HISTORY, "bench_history_strict")
    passing = module.rung_record({"case": {"destinations": 128}, "outcome": "pass",
                                  "delivery": {"srt": {"value": {
                                      "quality_average_delivery_ratio_min": 0.9995,
                                      "quality_strict_full_rate": "yes",
                                      "quality_strict_ratio_min": 0.9995}}}})
    check(passing["strict_full_rate"] == {"srt": "yes"},
          f"the strict result must be recorded: {passing['strict_full_rate']}")
    check(passing["strict_full_rate_pass"] is True,
          "a strict pass must read as one")

    marginal = module.rung_record({"case": {"destinations": 160},
                                   "outcome": "quality-failure",
                                   "delivery": {"srt": {"value": {
                                       "quality_average_delivery_ratio_min": 0.4978,
                                       "quality_strict_full_rate": "no"}}}})
    check(marginal["strict_full_rate_pass"] is False,
          "a strict failure must read as one")
    old_bundle = module.rung_record({"case": {"destinations": 64}, "outcome": "pass",
                                     "delivery": {"srt": {"value": {
                                         "quality_average_delivery_ratio_min": 0.999}}}})
    check(old_bundle["strict_full_rate_pass"] is None,
          "a run that never reported the strict result must not imply one")


def test_history_separates_observed_from_threshold(work):
    results = os.path.join(work, "results")
    make_config(results, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "pass", dict(srt_delivery(0.99),
                          **hls_origin_delivery(0.95, 0.972))),
    ], status=1)
    out = os.path.join(work, "summary.json")
    result = summarize(results, out)
    check(result.returncode == 0, f"summarize failed: {result.stderr}")
    with open(out, encoding="utf-8") as source:
        record = json.load(source)
    check(record["schema"] == "nginx-media.bench-history/3",
          f"schema is {record['schema']}")
    rungs = record["configs"]["srt"]["mixes"]["pure-srt"]["rungs"]
    hls_rung = [r for r in rungs if r["destinations"] == 16][0]
    check_close(hls_rung["observed_delivery_ratio"], 0.972,
                "the observed ratio is the measured one")
    check_close(hls_rung["delivery_ratio_threshold"], 0.95,
                "the threshold is recorded separately")
    check(hls_rung["delivery_ratio_basis"] == "observed",
          f"basis is {hls_rung['delivery_ratio_basis']}")
    check("hls_readers" in hls_rung["observed_delivery_ratio_protocols"],
          "the measured protocols are named")

    # the same bundle without an observation must not become a 95% result
    results_old = os.path.join(work, "results-old")
    make_config(results_old, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "pass", dict(srt_delivery(0.99), **hls_origin_delivery(0.95))),
    ], status=1)
    out_old = os.path.join(work, "summary-old.json")
    result = summarize(results_old, out_old)
    check(result.returncode == 0, f"summarize failed: {result.stderr}")
    with open(out_old, encoding="utf-8") as source:
        record = json.load(source)
    rung = [r for r in record["configs"]["srt"]["mixes"]["pure-srt"]["rungs"]
            if r["destinations"] == 16][0]
    check_close(rung["observed_delivery_ratio"], 0.99,
                "a threshold-only protocol must not drag the observed minimum "
                "to the gate value")
    check_close(rung["delivery_ratio_threshold"], 0.95,
                "the gate is still reported, as a gate")
    check(rung["delivery_ratio_basis"] == "observed-for-srt",
          f"basis is {rung['delivery_ratio_basis']}")
    check("hls_readers" not in rung["observed_delivery_ratio_protocols"],
          "an unmeasured protocol is not listed as measured")


def test_summary_marks_uncollected_expected_configs(work):
    results = os.path.join(work, "expected-configs")
    make_config(results, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "pass", srt_delivery(0.99)),
        (32, "quality-failure", srt_delivery(0.80)),
    ], status=1)
    out = os.path.join(work, "expected-configs.json")
    result = summarize(results, out, '["srt missing"]')
    check(result.returncode == 0, f"summarize failed: {result.stderr}")
    with open(out, encoding="utf-8") as source:
        record = json.load(source)
    check(record["expected_configs"] == ["missing", "srt"],
          f"matrix config groups must flatten: {record['expected_configs']}")
    check(record["missing_configs"] == ["missing"],
          f"the absent artifact must be named: {record['missing_configs']}")
    check(record["configs"]["missing"]["complete"] is False
          and record["configs"]["missing"]["mixes"] == {},
          "an absent matrix job must be an explicit incomplete config")
    check(record["configs"]["srt"]["complete"] is True
          and record["complete"] is False,
          "a complete config stays valid while the missing group makes the "
          "whole run incomplete")
    check("host" not in record,
          "the separate publisher machine must not be recorded as benchmark host")


def test_summary_persists_same_runner_comparison_metadata(work):
    results = os.path.join(work, "trend-meta")
    make_config(results, "srt", [(1, "pass", srt_delivery(1.0))])
    out = os.path.join(work, "trend-meta.json")
    env = os.environ.copy()
    env["BENCH_COMMIT_TIMESTAMP"] = "2026-09-28T12:30:00+00:00"
    env["BENCH_COMPARISON_GROUP"] = "workflow-42-attempt-1"
    env["GITHUB_SHA"] = "sampled-commit"
    result = run([sys.executable, HISTORY, "summarize", results, "--tier",
                  "branch", "--out", out], env=env)
    check(result.returncode == 0, f"trend summary succeeds: {result.stderr}")
    with open(out, encoding="utf-8") as source:
        record = json.load(source)
    check(record.get("comparison_group") == "workflow-42-attempt-1",
          f"summary retains the same-runner cohort: {record}")
    check(record.get("sha") == "sampled-commit",
          f"each profile records its source revision: {record}")
    check(record.get("commit_timestamp") == "2026-09-28T12:30:00+00:00",
          f"summary retains source commit time for graph ordering: {record}")

def test_regressions_require_complete_passing_same_runner_baselines():
    module = load_module(HISTORY, "bench_history_regressions")
    host = {"cpu_model": "runner-a", "kernel": "7.2", "transport": "libsrt"}

    def record(value, schema="nginx-media.bench-history/2", complete=True,
               fingerprint=host, outcome="pass"):
        return {
            "schema": schema,
            "tier": "nightly",
            "configs": {"srt": {
                "complete": complete,
                "recipe": copy.deepcopy(RECIPE),
                "mixes": {"pure-srt": {
                    "fingerprint": fingerprint,
                    "rungs": [{"destinations": 16, "outcome": outcome,
                               "sender_cpu_per_gbps": value}],
                }},
            }},
        }

    current = record(14, schema="nginx-media.bench-history/3")
    history = [
        record(10),
        record(12),
        record(11),
        record(100, outcome="quality-failure"),
        record(100, complete=False),
        record(100, fingerprint={"cpu_model": "runner-b"}),
        record(100, schema="nginx-media.bench-history/1"),
        record(100, fingerprint=None),
    ]
    findings = module.regressions(current, history)
    check(len(findings) == 1, f"one valid regression expected: {findings}")
    if findings:
        check_close(findings[0]["baseline_median"], 11,
                    "only valid comparable baselines contribute")
        check(findings[0]["baseline_count"] == 3,
              f"failed, incomplete and other-host samples are excluded: {findings}")
        check(findings[0]["factor"] > 1.25,
              f"the threshold is measured against the comparable median: {findings}")
    check(module.regressions(record(14, schema="nginx-media.bench-history/3",
                                    complete=False), history) == [],
          "an incomplete current configuration cannot raise an alert")
    interleaved_history = ([record(10)]
                           + [record(50, fingerprint={"cpu_model": "runner-b"})
                              for _ in range(12)]
                           + [record(12), record(11)])
    interleaved_findings = module.regressions(current, interleaved_history,
                                              window=10)
    check(len(interleaved_findings) == 1,
          f"backward search must locate same-runner baselines across other-runner runs: {interleaved_findings}")
    if interleaved_findings:
        check_close(interleaved_findings[0]["baseline_median"], 11,
                    "median matches the 3 same-runner runs")
        check(interleaved_findings[0]["baseline_count"] == 3,
              "all 3 same-runner runs found despite intervening runs")


def test_regression_identity_ignores_pressure_but_preserves_environment():
    module = load_module(HISTORY, "bench_history_stable_identity")

    def record(value, index):
        host = {
            "role": "sender",
            "cpu_model": "AMD EPYC 7763 64-Core Processor",
            "kernel": "6.11.0-1018-azure",
            "nproc_online": 4,
            "permitted_cpus": [0, 1],
            "cgroup": {
                "cpu_max": "200000 100000",
                "cpu_stat": {"usage_usec": 1000000 * index,
                             "user_usec": 900000 * index,
                             "system_usec": 100000 * index,
                             "nr_periods": 1000 * index,
                             "nr_throttled": 10 * index,
                             "throttled_usec": 5000 * index},
            },
            "governor": "performance",
            "no_turbo": "0",
            "numa": {"node0": "0-3"},
            "transport": "libsrt.so.1.5 1.5.4 (/usr/lib/libsrt.so.1.5)",
            "kernel_udp_limits": {"net.core.rmem_max": "212992",
                                  "net.core.wmem_max": "212992"},
            "loadavg": f"{index}.00 0.10 0.20 1/100 1000",
            "host_busy_fraction": index / 10,
            "cpu_freq_khz": {"0": 2200000 + index},
            "softnet": {"dropped": index},
        }
        # Published summaries before the fix kept the entire cgroup in
        # their identity, while rung diagnostics retained the raw host.
        fingerprint = {key: copy.deepcopy(host[key]) for key in
                       ("cpu_model", "kernel", "nproc_online",
                        "permitted_cpus", "cgroup", "governor", "no_turbo",
                        "numa", "transport", "kernel_udp_limits")}
        return {
            "schema": "nginx-media.bench-history/3",
            "tier": "branch",
            "sha": f"sampled-revision-{index}",
            "comparison_group": "36882115060-attempt-1",
            "configs": {"srt": {
                "complete": True,
                "recipe": copy.deepcopy(RECIPE),
                "mixes": {"pure-srt": {
                    "fingerprint": fingerprint,
                    "rungs": [{"destinations": 16, "outcome": "pass",
                               "sender_cpu_per_gbps": value,
                               "host_fingerprint": host}],
                }},
            }},
        }

    history = [record(10, index) for index in range(1, 4)]
    history[0]["schema"] = "nginx-media.bench-history/2"
    current = record(15, 4)
    old_current = copy.deepcopy(current)
    raw_entry = current["configs"]["srt"]["mixes"]["pure-srt"]
    del raw_entry["fingerprint"]
    before_lift = copy.deepcopy(raw_entry)
    entry = module.run_environment(raw_entry)
    current["configs"]["srt"]["mixes"]["pure-srt"] = entry
    check(raw_entry == before_lift,
          "lifting stable identity must not mutate the caller's entry")
    check(entry["rungs"][0]["host_fingerprint"]
          == before_lift["rungs"][0]["host_fingerprint"],
          "cumulative counters and raw host pressure stay in diagnostics")
    check(entry["fingerprint"]["cgroup"] == {"cpu_max": "200000 100000"},
          "new summaries retain the quota, not cumulative cgroup pressure")

    before_compare = copy.deepcopy((current, history, old_current))
    findings = module.regressions(current, history)
    check(len(findings) == 1,
          f"same quota and environment compare across cpu_stat changes: {findings}")
    if findings:
        check_close(findings[0]["factor"], 1.5,
                    "same-runner pressure changes cannot hide a 50% regression")
        check(findings[0]["baseline_count"] == 3,
              "three published baselines with different counters all contribute")
    check(len(module.regressions(old_current, history)) == 1,
          "old-shaped current and historical fingerprints normalize equally")
    check((current, history, old_current) == before_compare,
          "comparison cannot rewrite published history or raw diagnostics")

    changes = {
        "cpu_model": "Intel Xeon Platinum 8370C",
        "kernel": "6.12.0-azure",
        "nproc_online": 8,
        "permitted_cpus": [2, 3],
        "cgroup": {"cpu_max": "100000 100000"},
        "governor": "powersave",
        "no_turbo": "1",
        "numa": {"node0": "0-1", "node1": "2-3"},
        "transport": "libsrt.so.1.5 1.5.3 (/usr/lib/libsrt.so.1.5)",
        "kernel_udp_limits": {"net.core.rmem_max": "425984",
                              "net.core.wmem_max": "212992"},
    }
    for key, value in changes.items():
        changed = copy.deepcopy(current)
        changed["configs"]["srt"]["mixes"]["pure-srt"]["fingerprint"][key] = value
        check(module.regressions(changed, history) == [],
              f"a changed {key} must isolate a different runner environment")

    for unknown in (None, {}, {"status": "unavailable", "value": None},
                    {"cgroup": {"cpu_stat": {"usage_usec": 100}}},
                    {"cpu_model": None, "kernel": None},
                    {"cpu_model": "", "numa": {}, "kernel_udp_limits": {},
                     "cgroup": {"cpu_max": None}}):
        unknown_current = copy.deepcopy(current)
        unknown_entry = unknown_current["configs"]["srt"]["mixes"]["pure-srt"]
        unknown_entry["fingerprint"] = unknown
        unknown_history = copy.deepcopy(history)
        for old in unknown_history:
            old["configs"]["srt"]["mixes"]["pure-srt"]["fingerprint"] = unknown
        check(module.regressions(unknown_current, unknown_history) == [],
              f"unknown identities must not become comparable: {unknown!r}")
        check(module.regressions(current, unknown_history + history[:2]) == [],
              f"unknown history cannot supply the third baseline: {unknown!r}")
        unknown_source = {"rungs": [{"host_fingerprint": unknown}]}
        check("fingerprint" not in module.run_environment(unknown_source),
              f"an unavailable raw host must not acquire an identity: {unknown!r}")

    at_threshold = copy.deepcopy(current)
    threshold_rung = at_threshold["configs"]["srt"]["mixes"]["pure-srt"]["rungs"][0]
    threshold_rung["sender_cpu_per_gbps"] = 12.5
    check(module.regressions(at_threshold, history) == [],
          "exactly 25% higher is not a regression")
    threshold_rung["sender_cpu_per_gbps"] = 12.5001
    check(len(module.regressions(at_threshold, history)) == 1,
          "strictly more than 25% higher is a regression")


def test_regression_cohorts_do_not_mix_with_qualification_history():
    module = load_module(HISTORY, "bench_history_cohorts")
    host = {"cpu_model": "runner-a", "kernel": "7.2", "transport": "libsrt"}

    def record(value, sha, group=None):
        return {
            "schema": "nginx-media.bench-history/3",
            "tier": "branch",
            "sha": sha,
            "comparison_group": group,
            "configs": {"srt": {
                "complete": True,
                "recipe": copy.deepcopy(RECIPE),
                "mixes": {"pure-srt": {
                    "fingerprint": host,
                    "rungs": [{"destinations": 16, "outcome": "pass",
                               "sender_cpu_per_gbps": value}],
                }},
            }},
        }

    qualification = record(14, "qualified-head")
    trend_only = [record(value, f"trend-{index}", "workflow-a")
                  for index, value in enumerate((10, 11, 12, 100))]
    check(module.regressions(qualification, trend_only) == [],
          "same-runner probe records cannot become qualification baselines")

    current = record(14, "sampled-head", "workflow-b")
    history = ([record(10, "b1", "workflow-b"),
                record(11, "b2", "workflow-b"),
                record(12, "b3", "workflow-b"),
                record(100, "a1", "workflow-a")]
               + [record(14, f"qualified-{index}") for index in range(4)])
    findings = module.regressions(current, history)
    check(len(findings) == 1,
          f"same-runner samples must compare within their workflow cohort: {findings}")
    if findings:
        check(findings[0]["baseline_count"] == 3,
              f"other cohorts and qualifications must be excluded: {findings}")


def test_publish_appends_same_runner_samples(work):
    pages = os.path.join(work, "trend-pages")
    os.makedirs(pages, exist_ok=True)
    summary = os.path.join(work, "trend-summary.json")
    samples = os.path.join(work, "trend-samples.jsonl")
    write_json(summary, {"tier": "branch", "sha": "qualified"})
    sample_records = [
        {"tier": "branch", "sha": "old", "comparison_group": "one"},
        {"tier": "branch", "sha": "head", "comparison_group": "one"},
    ]
    with open(samples, "w", encoding="utf-8") as output:
        for record in sample_records:
            output.write(json.dumps(record) + "\n")
    result = run([sys.executable, HISTORY, "publish", summary, "--pages", pages,
                  "--samples", samples])
    check(result.returncode == 0,
          f"publishing same-runner records succeeds: {result.stderr}")
    with open(os.path.join(pages, "data", "branch.jsonl"),
              encoding="utf-8") as source:
        rows = [json.loads(line) for line in source if line.strip()]
    check([row["sha"] for row in rows] == ["qualified", "old", "head"],
          f"qualification then trend samples are appended: {rows}")

def test_parse_expected_configs_validation():
    module = load_module(HISTORY, "bench_history_config_parser")
    check(module.parse_expected_configs(None) is None,
          "None value returns None")
    check(module.parse_expected_configs('["srt", "rtmp"]') == ["rtmp", "srt"],
          "list of configs parsed and sorted")
    check(module.parse_expected_configs('["srt rtmp", "hls"]') == ["hls", "rtmp", "srt"],
          "whitespace-separated groups flattened and deduplicated")
    for bad in ('"not-a-list"', "[1, 2]", "[]", '["   "]'):
        try:
            module.parse_expected_configs(bad)
            check(False, f"expected ValueError for {bad}")
        except ValueError:
            check(True, "expected ValueError raised")

def test_gate_accepts_a_finished_ladder_that_stopped_at_the_boundary(work):
    results = os.path.join(work, "ok")
    make_config(results, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "pass", srt_delivery(0.99)),
        (32, "quality-failure", srt_delivery(0.80)),
    ], status=1, stopped={"pure-srt": "64 128"})
    out = os.path.join(work, "ok.json")
    check(summarize(results, out).returncode == 0, "summarize must succeed")
    result = gate(out)
    check(result.returncode == 0,
          f"a completed ladder that failed past its boundary must pass: "
          f"{result.stdout}{result.stderr}")
    check("bench_gate=pass" in result.stdout, "the gate must say it passed")


def test_gate_accepts_an_infrastructure_limited_rung(work):
    """A host that cannot carry the load is not a software failure: the rung
    is recorded as infrastructure-limited, the ladder stops there, and the
    gate reports it without failing."""
    results = os.path.join(work, "limited")
    make_config(results, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "pass", srt_delivery(0.99)),
        (128, "infrastructure-limited", srt_delivery(1.0)),
    ], status=1, stopped={"pure-srt": "128 256 512"},
        expected={"mixes": ["pure-srt"], "steps": [1, 16, 128, 256, 512]})
    out = os.path.join(work, "limited.json")
    check(summarize(results, out).returncode == 0, "summarize must succeed")
    with open(out, encoding="utf-8") as source:
        record = json.load(source)
    entry = record["configs"]["srt"]["mixes"]["pure-srt"]
    check(entry["infrastructure_limited"] == [128],
          f"the matrix must list the rung separately: {entry}")
    check(entry["first_quality_failure"] is None,
          "an infrastructure limit is not a quality failure")
    result = gate(out)
    check(result.returncode == 0,
          f"an infrastructure limit must not fail the gate: {result.stdout}")
    check("infrastructure" in result.stdout + result.stderr,
          f"the gate must report the limit: {result.stdout}{result.stderr}")


def test_gate_rejects_quality_failure_below_the_required_rung(work):
    results = os.path.join(work, "low")
    make_config(results, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "quality-failure", srt_delivery(0.80)),
    ], status=1)
    out = os.path.join(work, "low.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1,
          "a quality failure at a rung every host must carry has to fail")


def test_gate_rejects_missing_rungs_and_workloads(work):
    results = os.path.join(work, "missing")
    make_config(results, "srt", [(1, "pass", srt_delivery(1.0))], status=0,
                expected={"mixes": ["pure-srt", "pure-rtmp"],
                          "steps": [1, 16]})
    out = os.path.join(work, "missing.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1, "an incomplete run must fail the gate")
    check("neither measured nor skipped" in result.stdout
          or "neither measured nor skipped" in result.stderr,
          f"the gate must say which rungs are missing: {result.stdout}")
    check("pure-rtmp" in result.stdout or "pure-rtmp" in result.stderr,
          f"the gate must name the workload that never ran: {result.stdout}")


def test_gate_rejects_an_aborted_harness(work):
    results = os.path.join(work, "aborted")
    make_config(results, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "pass", srt_delivery(0.99)),
    ], status=None, finished=False, stopped={"pure-srt": "32 64"})
    out = os.path.join(work, "aborted.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1, "an aborted harness must fail the gate")
    check("did not finish" in result.stdout or "did not finish" in result.stderr
          or "no harness exit status" in result.stdout
          or "no harness exit status" in result.stderr,
          f"the gate must say the run did not finish: {result.stdout}")


def test_gate_rejects_a_killed_harness(work):
    """A nonzero status that is not "the ladder recorded failures" means the
    runner was killed or timed out, whatever the log managed to print."""
    results = os.path.join(work, "killed")
    make_config(results, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "pass", srt_delivery(0.99)),
    ], status=137, finished=False)
    out = os.path.join(work, "killed.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1, "a killed harness must fail the gate")

    # a timeout is the same story even if the log looks finished: the status
    # is the record of how the run ended
    timed_out = os.path.join(work, "timed-out")
    make_config(timed_out, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "pass", srt_delivery(0.99)),
    ], status=124, finished=True)
    out = os.path.join(work, "timed-out.json")
    summarize(timed_out, out)
    result = gate(out)
    check(result.returncode == 1,
          "a timed-out harness must fail the gate even when the log ends "
          "with the finished marker")


def test_gate_rejects_a_run_with_no_status_file(work):
    """No status file at all is not a pass: bench-ci writes it after the
    harness returns, so its absence means the runner never returned."""
    results = os.path.join(work, "nostatus")
    make_config(results, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "pass", srt_delivery(0.99)),
    ], status=None, finished=True)
    out = os.path.join(work, "nostatus.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1,
          "a run with no recorded exit status must fail the gate")
    check("no harness exit status" in result.stdout
          or "no harness exit status" in result.stderr,
          f"the gate must say the status is missing: {result.stdout}")


def test_gate_rejects_absent_diagnostics(work):
    results = os.path.join(work, "nodata")
    make_config(results, "srt", [], status=0, finished=True)
    out = os.path.join(work, "nodata.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1,
          "a configuration with no diagnostics bundle must fail the gate")
    check("no rung produced a diagnostics bundle" in result.stdout
          or "no rung produced a diagnostics bundle" in result.stderr,
          f"the gate must say the diagnostics are absent: {result.stdout}")


def test_gate_rejects_a_setup_failure(work):
    results = os.path.join(work, "setup")
    make_config(results, "srt", [
        (1, "pass", srt_delivery(1.0)),
        (16, "setup-failure", {"status": "unavailable"}),
    ], status=0, stopped={"pure-srt": "32"})
    out = os.path.join(work, "setup.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1, "a setup failure must fail the gate")


def test_run_queue_pressure_is_recorded(work):
    """Busy CPUs are not the whole story: the bundle must also say how long
    tasks waited for one and how deep the queue was."""
    module = load_module(DIAGNOSTICS, "capacity_diagnostics_pressure")
    before = {"host": {"cpu_pressure": {"some_total": 1_000_000.0,
                                        "full_total": 500_000.0},
                       "procs_running": 3}}
    after = {"host": {"cpu_pressure": {"some_total": 1_500_000.0,
                                       "full_total": 700_000.0},
                      "procs_running": 9}}
    pressure = module.run_queue_pressure(before, after, 2.0)
    check_close(pressure["some_stalled_pct"], 25.0,
                "a quarter of the window stalled on CPU")
    check_close(pressure["some_stalled_s"], 0.5, "stalled seconds")
    check_close(pressure["full_stalled_pct"], 10.0, "fully stalled share")
    check(pressure["procs_running_before"] == 3
          and pressure["procs_running_after"] == 9,
          f"queue depth at both edges: {pressure}")
    check(module.run_queue_pressure({}, {}, 2.0) == {},
          "a host without PSI reports nothing, never zero")


def test_per_lane_rate_keeps_workers_apart(work):
    """Two workers number their shards from zero: the per-lane rate must not
    merge them, and must not report zero for a lane that was serving."""
    case = os.path.join(work, "lanes")
    os.makedirs(case, exist_ok=True)
    rows = ["round_id,sample_ns,worker_pid,worker,shard,destinations,"
            "feed_queue_units,feed_queue_bytes,feed_drops,output_queue_units,"
            "output_queue_bytes,output_drops,sent_bytes,blocked_sends,"
            "retransmitted_packets"]
    # worker 0: shard 0 serves 1000 bytes per sample, shard 1 serves 2000
    # worker 3: shard 0 exists but serves nothing
    for index, ns in enumerate((1_000_000_000, 2_000_000_000)):
        rows.append(f"{index},{ns},111,0,0,16,0,0,0,0,0,0,{1000 * index},0,0")
        rows.append(f"{index},{ns},111,0,1,16,0,0,0,0,0,0,{2000 * index},0,0")
        rows.append(f"{index},{ns},222,3,0,0,0,0,0,0,0,0,0,0,0")
    write(os.path.join(case, "queue-samples.csv"), "\n".join(rows) + "\n")
    module = load_module(DIAGNOSTICS, "capacity_diagnostics_lanes")

    def metrics(sent_bytes):
        return [
            ("nginx_media_srt_egress_shard_destinations",
             {"worker": "w0", "shard": "0"}, 16.0),
            ("nginx_media_srt_egress_shard_sent_bytes_total",
             {"worker": "w0", "shard": "0"}, sent_bytes),
        ]

    section = module.srt_section(case, {"111": metrics(0)},
                                 {"111": metrics(3000)})
    rates = section["per_lane_service_rate"]["value"]
    check(rates.get("w0:s0") == 8000,
          f"worker 0's shard 0 rate must be its own: {rates}")
    check(rates.get("w0:s1") == 16000,
          f"worker 0's shard 1 rate must be its own: {rates}")
    check(rates.get("w3:s0") == 0,
          f"an empty shard reports zero, not another worker's bytes: {rates}")
def test_per_cpu_busy_separates_idle_from_saturated(work):
    """A short delivery on idle pinned cores is a different finding from one
    on saturated cores: the bundle must carry both the per-CPU numbers and
    what the pinned sets did."""
    module = load_module(DIAGNOSTICS, "capacity_diagnostics_percpu")

    def snapshot(values):
        return {"host": {"cpu_per_cpu": {
            f"cpu{index}": {"user": user, "nice": 0, "system": 0, "idle": idle,
                            "iowait": 0, "irq": 0, "softirq": 0, "steal": 0}
            for index, (user, idle) in enumerate(values)}}}

    # cpu0 works the whole window, cpu1 is idle, cpu2 is half busy
    before = snapshot([(0, 1000), (0, 1000), (0, 1000)])
    after = snapshot([(1000, 1000), (0, 2000), (500, 1500)])
    busy = module.per_cpu_busy(before, after)
    check_close(busy["cpu0"], 100.0, "a fully busy core reads 100%")
    check_close(busy["cpu1"], 0.0, "an idle core reads 0%")
    check_close(busy["cpu2"], 50.0, "a half busy core reads 50%")

    check(module.parse_cpu_list("0,2,4-6") == [0, 2, 4, 5, 6],
          "the harness's placement format must parse")
    check(module.parse_cpu_list("") == [] and module.parse_cpu_list(None) == [],
          "an unpinned run has no CPU list")

    headroom = module.pinned_headroom(busy, "0,1", "sender")
    check(headroom["value"]["mean_busy_pct"] == 50.0,
          f"pinned mean busy: {headroom}")
    check(headroom["value"]["idle_cpus"] == 1
          and headroom["value"]["saturated_cpus"] == 1,
          f"idle and saturated cores must be counted: {headroom}")
    check(module.pinned_headroom(busy, "9", "sender") is None,
          "a CPU with no counters reports nothing, never zero")


def test_matrix_separates_observed_from_threshold(work):
    run_dir = os.path.join(work, "run")
    case = os.path.join(run_dir, "capacity", "quality-hls-16")
    hls_reader_case(case, ratios=(0.99, 0.96), threshold=0.95,
                    bytes_each=(1000000, 1000000), log_observed=True)
    module, bundle = bundle_for(case)
    bundle["case"] = {"destinations": 16, "mix": "pure-hls"}
    bundle["outcome"] = "pass"
    with open(os.path.join(case, "diagnostics.json"), "w",
              encoding="utf-8") as output:
        json.dump(bundle, output)
    result = run([sys.executable, DIAGNOSTICS, "matrix", run_dir])
    check(result.returncode == 0, f"matrix failed: {result.stderr}")
    with open(os.path.join(run_dir, "capacity-matrix.json"),
              encoding="utf-8") as source:
        matrix = json.load(source)
    rung = matrix["mixes"]["pure-hls"]["rungs"][0]
    check_close(rung["observed_delivery_ratio"], 0.96,
                "the matrix reports the observed minimum")
    check_close(rung["delivery_ratio_threshold"], 0.95,
                "the matrix reports the threshold separately")
    check("min_delivery_ratio" not in rung,
          "the matrix must not publish a threshold under a measurement name")


# --- benchmark reporting truth -----------------------------------------------

HARNESS = os.path.join(HERE, "ingest_egress_fanout.sh")
MIXES_CONF = os.path.join(HERE, "capacity-mixes.conf")
SRT_JUDGE = os.path.join(HERE, "srt_capacity_quality.py")
RTMP_JUDGE = os.path.join(HERE, "rtmp_capacity_quality.py")
HLS_PUSH_JUDGE = os.path.join(HERE, "hls_push_capacity.py")

# the seven workloads bench-ci.sh used to promise, and the two contention
# ladders the harness ran on top of them without the manifest saying so
PUBLISHED_MIXES = ["pure-srt", "pure-rtmp", "pure-hls", "pure-hls-push",
                   "rtmp-95-srt-5", "hls-push-95-srt-5",
                   "rtmp-50-hls-push-45-srt-5"]
CONTENTION_MIXES = ["srt-50-rtmp-25-hls-push-25", "srt-25-rtmp-25-hls-push-50"]


def read_text(path):
    with open(path, encoding="utf-8") as source:
        return source.read()


def read_json(path):
    with open(path, encoding="utf-8") as source:
        return json.load(source)


def read_mix_conf():
    try:
        lines = read_text(MIXES_CONF).splitlines()
    except OSError:
        return []
    return [line.split(":")[0] for line in lines
            if line.strip() and not line.lstrip().startswith("#")]


def mix_rungs(mixes, steps=(1, 16, 32)):
    return [(destinations, "pass", srt_delivery(1.0), mix)
            for mix in mixes for destinations in steps]


def test_gate_requires_positive_evidence_of_the_floor_rung(work):
    """The gate asked only whether anything failed at the floor.  A floor
    rung that was infrastructure-limited, unknown or never reached is neither
    a failure nor a pass, and a run whose floor was never shown to work must
    not come out green."""
    one = srt_delivery(1.0)
    cases = {
        "infra-at-floor": ([(1, "pass", one),
                            (16, "infrastructure-limited", one)],
                           {"pure-srt": "16 32"}),
        "floor-skipped": ([(1, "pass", one)], {"pure-srt": "16 32"}),
        "unknown-at-floor": ([(1, "pass", one), (16, "unknown", one),
                              (32, "pass", one)], None),
        "infra-below-floor": ([(1, "infrastructure-limited", one)],
                              {"pure-srt": "16 32"}),
    }
    for label, (rungs, stopped) in cases.items():
        results = os.path.join(work, f"floor-{label}")
        make_config(results, "srt", rungs, status=1, stopped=stopped)
        out = os.path.join(work, f"floor-{label}.json")
        check(summarize(results, out).returncode == 0,
              f"{label}: summarize must succeed")
        result = gate(out)
        check(result.returncode == 1,
              f"{label}: no passing floor rung must fail the gate: "
              f"{result.stdout}")
        check("::error" in result.stdout and "floor" in result.stdout,
              f"{label}: the gate must say the floor was not shown: "
              f"{result.stdout}")

    # the floor of a ladder that stops below the tier floor is its highest
    # rung at or below it - still positive evidence, not an exemption
    short = {"mixes": ["pure-srt"], "steps": [1, 8]}
    results = os.path.join(work, "floor-short-pass")
    make_config(results, "srt", [(1, "pass", one), (8, "pass", one)],
                expected=short)
    out = os.path.join(work, "floor-short-pass.json")
    summarize(results, out)
    check(gate(out).returncode == 0,
          "a short ladder whose highest floor-side rung passed is evidence")
    results = os.path.join(work, "floor-short-infra")
    make_config(results, "srt", [(1, "pass", one),
                                 (8, "infrastructure-limited", one)],
                expected=short, status=1, stopped={"pure-srt": "8"})
    out = os.path.join(work, "floor-short-infra.json")
    summarize(results, out)
    check(gate(out).returncode == 1,
          "the floor of a short ladder must itself have passed")


def write_rows(path, header, rows):
    write(path, ",".join(header) + "\n"
          + "".join(",".join(str(v) for v in row) + "\n" for row in rows))


def judge_passed(result):
    return result.returncode == 0 and "quality_pass=yes" in result.stdout


def srt_judge_rung(directory, delivered=1_250_000, queue_units=0):
    """One SRT quality rung as the harness leaves it: a single destination
    receiving `delivered` bytes over a 10 s window, once per second."""
    t0, seconds = 100 * 10**9, 10

    def p(name):
        return os.path.join(directory, name)
    header = ("destination_id", "bytes_received", "snapshot_ns")
    write_rows(p("baseline.csv"), header, [("d0000", 0, t0)])
    write_rows(p("final.csv"), header,
               [("d0000", delivered, t0 + seconds * 10**9)])
    step = delivered // seconds
    write_rows(p("intervals.csv"),
               ("destination_id", "start_ns", "end_ns", "bytes_received",
                "total_bytes"),
               [("d0000", t0 + i * 10**9, t0 + (i + 1) * 10**9, step,
                 step * (i + 1)) for i in range(seconds)])
    write_rows(p("results.csv"),
               ("destination_id", "ts_packets", "ts_sync_errors",
                "ts_continuity_errors", "ts_tei_errors", "stalled",
                "transport_error"), [("d0000", 5000, 0, 0, 0, "no", 0)])
    write_rows(p("queues.csv"),
               ("round_id", "sample_ns", "worker_pid", "worker", "shard",
                "destinations", "feed_queue_units", "feed_queue_bytes",
                "feed_drops", "output_queue_units", "output_queue_bytes",
                "output_drops", "sent_bytes", "blocked_sends",
                "retransmitted_packets"),
               [(r - 1, t0 + r * 10**9, 4242, "0", "0", 1, queue_units, 0, 0, 0, 0,
                 0, r * 1000, 0, 0) for r in range(1, 10)])
    metrics = (
        "destinations", "feed_queue_units", "feed_queue_bytes",
        "feed_queue_dropped_total", "output_queue_units", "output_queue_bytes",
        "output_dropped_total", "sent_bytes_total", "blocked_sends_total",
        "retransmitted_packets_total",
    )
    for name in ("metrics.before.0", "metrics.after.0"):
        write(p(name), "".join(
            f'nginx_media_srt_egress_shard_{metric}{{worker="0",shard="0"}} '
            f'{1 if metric == "destinations" else 0}\n' for metric in metrics))
    for name in ("stream.before", "stream.after"):
        write(p(name), 'nginx_media_stream_feed_overruns_total{name="live"} 0\n'
              'nginx_media_stream_feed_evictions_total{name="live"} 0\n')
    with open(p("source.ts"), "wb") as source:
        source.write(bytes(3_750_000))  # 1 Mbit/s over the 30 s it stands for
    return p


def run_srt_judge(p, *extra):
    return run([sys.executable, SRT_JUDGE, "report",
                "--baseline", p("baseline.csv"), "--final", p("final.csv"),
                "--intervals", p("intervals.csv"),
                "--queues", p("queues.csv"),
                "--receiver-results", p("results.csv"),
                "--metrics-before", p("metrics.before"),
                "--metrics-after", p("metrics.after"),
                "--stream-before", p("stream.before"),
                "--stream-after", p("stream.after"),
                "--destination-report", p("destination.csv"),
                "--interval-report", p("interval.csv"),
                "--destinations", "1", "--prepared-source", p("source.ts"),
                "--reference-bps", "1000000"] + list(extra))


def test_srt_judge_rejects_non_finite_inputs(work):
    """A comparison with NaN is false both ways, so a NaN threshold,
    reference or measurement used to make every check beneath it pass."""
    good = srt_judge_rung(os.path.join(work, "srt-good"))
    check(judge_passed(run_srt_judge(good)),
          "control: a full-rate rung must pass the judge")
    short = srt_judge_rung(os.path.join(work, "srt-short"), delivered=625_000)
    check(not judge_passed(run_srt_judge(short, "--interval-floor", "0")),
          "control: half the reference rate must fail the average ratio")
    check(not judge_passed(run_srt_judge(short, "--min-delivery-ratio", "0")),
          "control: half the reference rate must fail the interval floor")
    for label, extra in {
            "NaN delivery threshold": ("--min-delivery-ratio", "nan",
                                       "--interval-floor", "0"),
            "NaN interval floor": ("--min-delivery-ratio", "0",
                                   "--interval-floor", "nan"),
            "infinite low-rate allowance": ("--min-delivery-ratio", "0",
                                            "--max-low-s", "inf"),
            "NaN reference": ("--reference-bps", "nan"),
            "infinite reference": ("--reference-bps", "inf",
                                   "--min-delivery-ratio", "0"),
    }.items():
        result = run_srt_judge(short, *extra)
        check(not judge_passed(result),
              f"{label} must not make a short rung pass: {result.stdout}")
    pressure = srt_judge_rung(os.path.join(work, "srt-pressure"),
                              queue_units=64)
    check(not judge_passed(run_srt_judge(pressure)),
          "control: a full feed queue for five samples must fail")
    result = run_srt_judge(pressure, "--queue-pressure", "nan")
    check(not judge_passed(result),
          f"a NaN queue pressure limit must not clear a full queue: "
          f"{result.stdout}")
    masked = srt_judge_rung(os.path.join(work, "srt-nan-queue"),
                            queue_units="nan")
    result = run_srt_judge(masked)
    check(not judge_passed(result),
          f"a NaN queue measurement must fail, not pass: {result.stdout}")


def rtmp_judge_rung(directory, delivered=1_250_000):
    t0, seconds = 100 * 10**9, 10

    def p(name):
        return os.path.join(directory, name)
    metric = ('nginx_media_source_payload_bytes_in_total{application="live",'
              'name="d0000",source="d0000"} ')
    write(p("receiver.before.0"),
          f"# scrape_monotonic_ns {t0 - 1000} {t0 + 1000}\n{metric}0\n")
    write(p("receiver.after.0"),
          f"# scrape_monotonic_ns {t0 + seconds * 10**9 - 1000} "
          f"{t0 + seconds * 10**9 + 1000}\n{metric}{delivered}\n")
    step = delivered // seconds
    write_rows(p("intervals.csv"),
               ("destination_id", "start_ns", "end_ns", "bytes_received",
                "total_bytes"),
               [("d0000", t0 + i * 10**9, t0 + (i + 1) * 10**9, step,
                 step * (i + 1)) for i in range(seconds)])
    return p


def run_rtmp_judge(p, *extra):
    return run([sys.executable, RTMP_JUDGE,
                "--before-prefix", p("receiver.before"),
                "--after-prefix", p("receiver.after"),
                "--programs", "1", "--destinations", "1",
                "--measurement-s", "10", "--intervals", p("intervals.csv"),
                "--report", p("report.csv"),
                "--reference-bps", "1000000"] + list(extra))


def test_rtmp_judge_rejects_non_finite_inputs(work):
    good = rtmp_judge_rung(os.path.join(work, "rtmp-good"))
    check(judge_passed(run_rtmp_judge(good)),
          "control: a full-rate rung must pass the judge")
    short = rtmp_judge_rung(os.path.join(work, "rtmp-short"),
                            delivered=625_000)
    check(not judge_passed(run_rtmp_judge(short, "--interval-floor", "0")),
          "control: half the reference rate must fail the average ratio")
    check(not judge_passed(run_rtmp_judge(short, "--min-delivery-ratio", "0")),
          "control: half the reference rate must fail the interval floor")
    for label, extra in {
            "NaN delivery threshold": ("--min-delivery-ratio", "nan",
                                       "--interval-floor", "0"),
            "NaN interval floor": ("--min-delivery-ratio", "0",
                                   "--interval-floor", "nan"),
            "infinite low-rate allowance": ("--min-delivery-ratio", "0",
                                            "--max-low-s", "inf"),
            "NaN reference": ("--reference-bps", "nan"),
    }.items():
        result = run_rtmp_judge(short, *extra)
        check(not judge_passed(result),
              f"{label} must not make a short rung pass: {result.stdout}")

def test_receiver_evidence_is_required(work):
    header = ("destination_id", "start_ns", "end_ns", "bytes_received", "total_bytes")
    t0 = 100 * 10**9
    normal = [("d0000", t0 + i * 10**9, t0 + (i + 1) * 10**9,
               125000, (i + 1) * 125000) for i in range(10)]
    corrupt = {
        "missing": [],
        "unknown": [("other", *normal[0][1:])] + normal[1:],
        "truncated": normal[:5],
        "duplicate": normal[:1] + normal,
        "overlap": normal[:1] + [("d0000", t0, t0 + 2 * 10**9, 125000, 250000)] + normal[2:],
        "gap": normal[:1] + normal[2:],
        "outside": normal + [("d0000", t0 + 10 * 10**9, t0 + 11 * 10**9, 0, 1250000)],
        "long-before-jitter": [("d0000", t0, t0 + 2100000000, 262500, 262500),
                               ("d0000", t0 + 2100000000, t0 + 2200000000, 12500, 275000),
                               ("d0000", t0 + 2200000000, t0 + 3 * 10**9, 100000, 375000)] + normal[3:],
        "long-raw": [("d0000", t0, t0 + 3 * 10**9, 375000, 375000)] + normal[3:],
        "nonpositive": [("d0000", t0, t0, 0, 0)] + normal,
    }
    for protocol, fixture, judge in (
            ("srt", srt_judge_rung, run_srt_judge),
            ("rtmp", rtmp_judge_rung, run_rtmp_judge)):
        for label, rows in corrupt.items():
            p = fixture(os.path.join(work, f"{protocol}-evidence-{label}"))
            write_rows(p("intervals.csv"), header, rows)
            result = judge(p)
            check(result.returncode != 0 and "quality_measurement_valid=no" in result.stdout
                  and not judge_passed(result) and "quality_strict_full_rate=yes" not in result.stdout,
                  f"{protocol} {label} evidence is setup failure, not delivery quality: {result.stdout}")
        p = fixture(os.path.join(work, f"{protocol}-missing-snapshot-destination"))
        if protocol == "srt":
            write_rows(p("final.csv"), ("destination_id", "bytes_received", "snapshot_ns"),
                       [("other", 1250000, t0 + 10 * 10**9)])
        else:
            write(p("receiver.after.0"),
                  f"# scrape_monotonic_ns {t0 + 10 * 10**9} {t0 + 10 * 10**9}\n")
        result = judge(p)
        check(result.returncode != 0 and "quality_measurement_valid=no" in result.stdout,
              f"{protocol} missing snapshot destination is setup failure: {result.stdout}")
        p = fixture(os.path.join(work, f"{protocol}-zero-delivery"), delivered=0)
        result = judge(p)
        check(result.returncode == 0 and "quality_measurement_valid=yes" in result.stdout
              and "quality_pass=no" in result.stdout,
              f"{protocol} complete zero-delivery counters are quality failure: {result.stdout}")
    p = rtmp_judge_rung(os.path.join(work, "rtmp-jitter-zero"))
    jitter = [("d0000", t0, t0 + 100000000, 0, 0),
              ("d0000", t0 + 100000000, t0 + 10**9, 125000, 125000)] + normal[1:]
    write_rows(p("intervals.csv"), header, jitter)
    check(judge_passed(run_rtmp_judge(p)),
          "healthy short jitter and zero-byte sample remain valid")
    for label in ("missing-shard", "missing-metric", "truncated-queue"):
        p = srt_judge_rung(os.path.join(work, f"srt-{label}"))
        if label == "missing-shard":
            # Preserve complete rounds but erase the only active shard.
            with open(p("queues.csv"), encoding="utf-8") as source:
                rows = list(csv.reader(source))
            for row in rows[1:]:
                row[4] = "1"
            write_rows(p("queues.csv"), rows[0], rows[1:])
        elif label == "missing-metric":
            write(p("metrics.after.0"),
                  'nginx_media_srt_egress_shard_destinations{worker="0",shard="0"} 1\n')
        else:
            with open(p("queues.csv"), encoding="utf-8") as source:
                rows = list(csv.reader(source))
            write_rows(p("queues.csv"), rows[0], rows[1:5])
        result = run_srt_judge(p)
        check(result.returncode != 0 and "quality_measurement_valid=no" in result.stdout
              and "quality_strict_full_rate=yes" not in result.stdout,
              f"{label} cannot pass strict qualification: {result.stdout}")
    # Two real active sender shards, with shard 1 absent from every queue
    # round: seeing complete rows for shard 0 is not complete evidence.
    p = srt_judge_rung(os.path.join(work, "srt-whole-shard-vanished"))
    for name in ("baseline.csv", "final.csv", "intervals.csv", "results.csv"):
        with open(p(name), encoding="utf-8") as source:
            rows = list(csv.reader(source))
        second = [["d0001"] + row[1:] for row in rows[1:]]
        write_rows(p(name), rows[0], rows[1:] + second)
    for name in ("metrics.before.0", "metrics.after.0"):
        with open(p(name), encoding="utf-8") as source:
            blob = source.read()
        write(p(name), blob + blob.replace('shard="0"', 'shard="1"'))
    result = run_srt_judge(p, "--destinations", "2")
    check(result.returncode != 0 and "quality_measurement_valid=no" in result.stdout
          and "quality_strict_full_rate=yes" not in result.stdout,
          f"whole missing shard invalidates otherwise healthy rounds: {result.stdout}")



def hls_push_judge_rung(directory, latency_ms=250.0):
    def p(name):
        return os.path.join(directory, name)
    segments = {f"seg{i}.ts": {"bytes": 100_000, "at": 101.0 + i, "count": 1}
                for i in range(6)}
    destination = {"ts_bytes": 600_000, "segments": 6,
                   "first_segment_monotonic": 100.25,
                   "first_segment_latency_ms": latency_ms,
                   "upload_durations_ms": [12.0] * 6,
                   "segment_detail": segments, "playlist_violations": 0}
    write_json(p("sink.before.json"),
               {"monotonic": 100.0, "http_errors": 0, "destinations": {}})
    write_json(p("sink.after.json"),
               {"monotonic": 110.0, "http_errors": 0,
                "destinations": {"d0000": destination}})
    labels = 'protocol="hls_push",name="live",destination="d0000"'
    pool = 'nginx_media_egress_active_workers{engine="hls_upload_pool"} 2\n'
    write(p("workers.before.0"), pool)
    write(p("workers.after.0"), pool
          + f"nginx_media_egress_dropped_units_total{{{labels}}} 0\n"
          + f"nginx_media_egress_transport_errors_total{{{labels}}} 0\n")
    return p


def run_hls_push_judge(p, *extra):
    return run([sys.executable, HLS_PUSH_JUDGE, "report",
                "--before-prefix", p("workers.before"),
                "--after-prefix", p("workers.after"), "--stream", "live",
                "--destinations", "1", "--sink-before", p("sink.before.json"),
                "--sink-after", p("sink.after.json"),
                "--report", p("report.csv"),
                "--reference-bps", "480000"] + list(extra))


def test_hls_push_judge_rejects_non_finite_inputs(work):
    good = hls_push_judge_rung(os.path.join(work, "push-good"))
    check(judge_passed(run_hls_push_judge(good)),
          "control: a complete rung must pass the judge")
    for label, extra in {
            "NaN reference": ("--reference-bps", "nan"),
            "NaN delivery threshold": ("--min-delivery-ratio", "nan"),
            "infinite delivery threshold": ("--min-delivery-ratio", "inf"),
            "NaN lag limit": ("--segment-lag-limit-s", "nan"),
    }.items():
        result = run_hls_push_judge(good, *extra)
        check(not judge_passed(result),
              f"{label} must not pass: {result.stdout}")
    # the sink's own JSON can carry NaN; a NaN measurement is not a pass
    nan_latency = hls_push_judge_rung(os.path.join(work, "push-nan"),
                                      latency_ms=float("nan"))
    result = run_hls_push_judge(nan_latency)
    check(not judge_passed(result),
          f"a NaN first-segment latency must fail: {result.stdout}")


def bash_function(name):
    match = re.search(rf"^{name}\(\) \{{.*?^\}}\n", read_text(HARNESS),
                      re.M | re.S)
    check(match is not None, f"the harness must define {name}")
    return match.group(0) if match else None


def test_calibration_is_stored_only_from_a_passing_positive_rung(work):
    """A calibration rung's rate is the yardstick for every rung above it.
    It used to be stored whether or not that rung passed, and a rate of 0.00
    matched the number pattern."""
    functions = [bash_function("capacity_positive_rate"),
                 bash_function("capacity_store_reference")]
    if None in functions:
        return
    script = "\n".join(functions) + (
        '\nREF=unset\ncapacity_store_reference REF tag Label "$1" "$2" '
        '2>/dev/null\necho "rc=$? ref=$REF"\n')

    def store(quality_pass, rate):
        result = run(["bash", "-c", script, "calibration", quality_pass, rate])
        lines = result.stdout.strip().splitlines()
        return lines[-1] if lines else ""
    check(store("yes", "8000000.5") == "rc=0 ref=8000000.5",
          "a passing rung with a positive rate is stored")
    check(store("no", "8000000.5") == "rc=0 ref=unset",
          "a failed calibration rung's rate must not become the reference")
    for rate in ("0", "0.00", "nan", "inf", "-5", "1e6", ""):
        for quality_pass in ("yes", "no"):
            check(store(quality_pass, rate) == "rc=1 ref=unset",
                  f"rate {rate!r} ({quality_pass}) is not a finite positive "
                  f"number: {store(quality_pass, rate)}")


def test_harness_runs_the_mixes_of_the_one_list(work):
    """The harness's own mix selection, with the ladders stubbed: `all` must
    be the file's list, an explicit list must be honoured, and the harness
    must state what it ran in the form the history parser reads."""
    function = bash_function("phase_capacity_quality_ladder")
    listed = read_mix_conf()
    check(len(listed) == 9 and set(CONTENTION_MIXES) <= set(listed),
          f"capacity-mixes.conf must hold all nine mixes: {listed}")
    if function is None:
        return
    script = function + (
        '\ncapacity_quality_ladder() { echo "ladder $1"; }\n'
        "phase_capacity_quality_ladder\n")
    env = dict(os.environ, ROOT=ROOT, RUN=os.path.join(work, "stub-run"),
               CAPACITY_QUALITY_STEPS="1 16 32", CAPACITY_QUALITY_RATE="8M",
               CAPACITY_QUALITY_SECONDS="15",
               CAPACITY_QUALITY_STOP_AFTER_FAILURES="2",
               CAPACITY_QUALITY_MIN_DELIVERY_RATIO="0.95",
               CAPACITY_QUALITY_INTERVAL_FLOOR="0.80",
               CAPACITY_QUALITY_MAX_LOW_SECONDS="2",
               CAPACITY_FIXED_SRT_WORKERS="adaptive",
               CAPACITY_FIXED_HLS_PUSH_WORKERS="adaptive",
               CAPACITY_SRT_HLS="yes")

    def select(mixes):
        result = run(["bash", "-c", script],
                     env=dict(env, CAPACITY_QUALITY_MIXES=mixes))
        ladders = [line.split()[1] for line in result.stdout.splitlines()
                   if line.startswith("ladder ")]
        stated = [line for line in result.stdout.splitlines()
                  if line.startswith("quality_mixes=")]
        return result, ladders, stated
    result, ladders, stated = select("all")
    check(ladders == listed,
          f"`all` must run exactly the listed mixes in order: {ladders}")
    check(stated == ["quality_mixes=" + " ".join(listed)],
          f"the harness must state the mixes it ran: {stated}")
    module = load_module(HISTORY, "bench_history_harness_contract")
    recipe_lines = [line for line in result.stdout.splitlines()
                    if line.startswith("quality_recipe ")]
    parsed = (module.parse_recipe(recipe_lines[0].split()[1:])
              if recipe_lines else None)
    check(parsed is not None and parsed["seconds"] == 15
          and parsed["steps"] == [1, 16, 32]
          and parsed["stop_after_failures"] == 2,
          f"the recipe line must parse back into the recipe: {recipe_lines}")
    chosen = ["pure-srt", "srt-50-rtmp-25-hls-push-25"]
    result, ladders, stated = select(" ".join(chosen))
    check(ladders == chosen and stated == ["quality_mixes=" + " ".join(chosen)],
          f"an explicit list runs exactly itself: {ladders} {stated}")
    result, ladders, stated = select("pure-srt no-such-mix")
    check(result.returncode != 0 and not ladders
          and "unknown capacity quality mix" in result.stderr,
          f"an unknown mix is refused before anything runs: {result.stderr}")


def test_bench_ci_manifest_is_the_harness_mix_list(work):
    """bench-ci.sh used to promise seven workloads for `all` while the
    harness ran nine.  Run the real driver against a harness stub that
    echoes what it was handed."""
    tree = os.path.join(work, "ci-tree")
    for relative in ("scripts/bench-ci.sh", "tests/bench/bench_history.py",
                     "tests/bench/capacity-mixes.conf"):
        source = os.path.join(ROOT, relative)
        if os.path.exists(source):
            os.makedirs(os.path.dirname(os.path.join(tree, relative)),
                        exist_ok=True)
            shutil.copyfile(source, os.path.join(tree, relative))
    stub = os.path.join(tree, "tests", "bench", "ingest_egress_fanout.sh")
    write(stub, '#!/usr/bin/env bash\n'
                'echo "quality_mixes=$CAPACITY_QUALITY_MIXES"\n'
                'echo "quality_ladders_with_failures=none"\n')
    os.chmod(stub, 0o755)
    out = os.path.join(work, "ci-out")
    env = dict(os.environ, BENCH_TREND="1", BENCH_STEPS="1 16",
               BENCH_SECONDS="10")
    env.pop("BENCH_CONFIGS", None)
    run(["bash", os.path.join(tree, "scripts", "bench-ci.sh"), "weekly", out],
        env=env)
    listed = read_mix_conf()
    manifest = read_json(os.path.join(out, "all", "expected.json"))
    check(manifest["mixes"] == listed and len(listed) == 9,
          f"the manifest must list every mix the harness executes: "
          f"{manifest['mixes']} vs {listed}")
    log = read_text(os.path.join(out, "all.log"))
    check(f"quality_mixes={' '.join(manifest['mixes'])}\n" in log,
          f"the harness must be handed the manifest's explicit list: {log}")
    config = read_json(os.path.join(out, "summary.json"))["configs"]["all"]
    check(not any("executed" in message for message in config["missing"]),
          f"manifest and execution agree, so no mix mismatch: {config}")


def test_gate_rejects_a_manifest_that_differs_from_the_executed_mixes(work):
    one = srt_delivery(1.0)
    seven = {"mixes": PUBLISHED_MIXES, "steps": [1, 16, 32]}
    nine = {"mixes": PUBLISHED_MIXES + CONTENTION_MIXES,
            "steps": [1, 16, 32]}

    # the old situation: the harness ran nine, the manifest promised seven
    results = os.path.join(work, "mixes-seven")
    make_config(results, "all", mix_rungs(PUBLISHED_MIXES), expected=seven,
                ran_mixes=nine["mixes"])
    out = os.path.join(work, "mixes-seven.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1 and "executed" in result.stdout
          and CONTENTION_MIXES[0] in result.stdout,
          f"a manifest that omits an executed mix must fail: {result.stdout}")

    # a harness that does not say what it ran is not evidence of anything
    results = os.path.join(work, "mixes-silent")
    make_config(results, "all", mix_rungs(PUBLISHED_MIXES), expected=seven,
                ran_mixes="silent")
    out = os.path.join(work, "mixes-silent.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1 and "never stated" in result.stdout,
          f"a harness that states no mixes must fail: {result.stdout}")

    # a contention ladder that never ran is missing, not a shorter green run
    results = os.path.join(work, "mixes-missing")
    make_config(results, "all", mix_rungs(PUBLISHED_MIXES), expected=nine,
                ran_mixes=nine["mixes"])
    out = os.path.join(work, "mixes-missing.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1 and CONTENTION_MIXES[1] in result.stdout,
          f"a missing contention ladder must fail: {result.stdout}")

    results = os.path.join(work, "mixes-nine")
    make_config(results, "all", mix_rungs(nine["mixes"]), expected=nine,
                ran_mixes=nine["mixes"])
    out = os.path.join(work, "mixes-nine.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 0,
          f"control: nine executed, nine promised, nine measured: "
          f"{result.stdout}")

    # Old bundles can still fill every rung while the current harness runs a
    # different ladder. The harness's recipe must agree with the manifest.
    results = os.path.join(work, "steps-mismatch")
    make_config(results, "all", mix_rungs(PUBLISHED_MIXES), expected=seven,
                recipe={"steps": "1,16"})
    out = os.path.join(work, "steps-mismatch.json")
    summarize(results, out)
    result = gate(out)
    check(result.returncode == 1 and "steps" in result.stdout,
          f"a recipe with different steps must fail: {result.stdout}")


def test_trend_stub_summary_for_every_tier(work):
    """bench-trend.sh records a stub for a revision that did not build.  The
    weekly branch used to leave the variable the stub reads unset, which
    `set -u` turns into a dead script."""
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
    for tier, names in (("branch", ["srt", "rtmp", "rtmp-srt"]),
                        ("nightly", ["srt-rtmp", "hls", "mix-srt-share",
                                     "mix-all-protocols"]),
                        ("weekly", ["all"])):
        tree = os.path.join(work, f"trend-{tier}")
        for relative in ("scripts/bench-trend.sh",
                         "tests/bench/bench_commit_selection.py"):
            os.makedirs(os.path.dirname(os.path.join(tree, relative)),
                        exist_ok=True)
            shutil.copyfile(os.path.join(ROOT, relative),
                            os.path.join(tree, relative))
        write(os.path.join(tree, "scripts", "build-nginx.sh"),
              "#!/usr/bin/env bash\necho 'stub build failure' >&2\nexit 1\n")
        os.chmod(os.path.join(tree, "scripts", "build-nginx.sh"), 0o755)
        for directory in (".build/nginx-1.30.5", ".build/srt-haivision"):
            os.makedirs(os.path.join(tree, directory))
        write(os.path.join(tree, ".gitignore"), ".build/\n")
        check(run(["git", "init", "-q"], cwd=tree).returncode == 0,
              f"{tier}: git init")
        run(git + ["add", "-A"], cwd=tree)
        check(run(git + ["commit", "-q", "-m", "seed"],
                  cwd=tree).returncode == 0, f"{tier}: seed commit")
        env = dict(os.environ, GITHUB_RUN_ID="7", GITHUB_RUN_ATTEMPT="1")
        env.pop("GITHUB_SHA", None)
        result = run(["bash", os.path.join(tree, "scripts", "bench-trend.sh"),
                      tier], env=env, cwd=tree)
        check(result.returncode == 0,
              f"{tier}: a revision that did not build must still produce a "
              f"stub sample: {result.stdout}{result.stderr}")
        samples = os.path.join(tree, ".build", "bench-trend", "samples.jsonl")
        rows = ([json.loads(line) for line in read_text(samples).splitlines()
                 if line.strip()] if os.path.exists(samples) else [])
        check(len(rows) >= 1 and all(list(row["configs"]) == names
                                     and row["expected_configs"] == names
                                     and row["complete"] is False
                                     for row in rows),
              f"{tier}: the stub must name the tier's configurations: {rows}")


def recipe_record(value, tier="branch", recipe=None, provenance=None,
                  steps=None, mixes=("pure-srt",)):
    """A published record shaped as summarize writes one: complete, one
    configuration, one passing rung at 16 destinations."""
    config_recipe = copy.deepcopy(RECIPE)
    config_recipe.update(recipe or {})
    entry = {"fingerprint": RUNNER,
             "rungs": [{"destinations": 16, "outcome": "pass",
                        "sender_cpu_per_gbps": value}]}
    if provenance:
        entry["provenance"] = provenance
    if steps is not None:
        config_recipe["steps"] = steps
    return {"schema": "nginx-media.bench-history/3", "tier": tier,
            "sha": f"sha-{value}",
            "configs": {"srt": {"complete": True,
                                "recipe": None if recipe is False
                                else config_recipe,
                                "mixes": {mix: copy.deepcopy(entry)
                                          for mix in mixes}}}}


def test_regressions_compare_only_equal_recipes():
    module = load_module(HISTORY, "bench_history_recipe_equality")

    def findings(current, past):
        return module.regressions(current, past)
    base = [10, 11, 12]
    current = recipe_record(14)
    check(len(findings(current, [recipe_record(v) for v in base])) == 1,
          "control: three equal-recipe baselines make the regression visible")
    for label, change in {
            "PR 10 s against branch 15 s": {"seconds": 10},
            "a different source rate": {"rate": "20M"},
            "a different stop-after rule": {"stop_after_failures": 0},
            "a different judge threshold": {"min_delivery_ratio": 0.9},
            "a different sender worker setting": {"srt_workers": "1"},
            "a different harness revision": {"harness": "harness-b"},
            "a different mix set": {"mixes": ["pure-rtmp"]},
            "a different first rung": {"steps": [4, 16, 32, 64, 128]},
    }.items():
        check(findings(current, [recipe_record(v, recipe=change)
                                 for v in base]) == [],
              f"{label} must not be a baseline")
    check(findings(current, [recipe_record(v, recipe=False) for v in base])
          == [], "records without a recipe are not comparable with new ones")
    check(findings(recipe_record(14, recipe=False),
                   [recipe_record(v) for v in base]) == [],
          "a current record without a recipe is compared with nothing")
    # a different ladder is fine if it starts at the same rung: a pull
    # request runs a prefix of main's ladder
    pr = recipe_record(14, tier="pr", steps=[1, 16, 32])
    check(len(findings(pr, [recipe_record(v) for v in base])) == 1,
          "a subset ladder starting at the same rung is comparable")
    # incomparable runs are skipped, not counted, and do not crowd out
    # comparable ones from the window
    mixed = ([recipe_record(v) for v in base]
             + [recipe_record(90, recipe={"seconds": 5}) for _ in range(12)])
    found = findings(current, mixed)
    check(len(found) == 1 and found[0]["baseline_count"] == 3
          and found[0]["baseline_median"] == 11,
          f"only equal-recipe runs form the baseline: {found}")
    # binary hashes and compiler strings are provenance, never equality
    built = [recipe_record(v, provenance={"binary_sha256": f"{v:064x}",
                                          "compiler": f"gcc-{v}"})
             for v in base]
    check(len(findings(recipe_record(14, provenance={
        "binary_sha256": "f" * 64, "compiler": "gcc-99"}), built)) == 1,
          "a different binary hash or compiler must not stop a comparison")


def test_pr_records_compare_with_the_branch_history(work):
    module = load_module(HISTORY, "bench_history_pr_tier")
    base = [10, 11, 12]
    pr = recipe_record(14, tier="pr")
    check(len(module.regressions(
        pr, [recipe_record(v) for v in base])) == 1,
          "a pull request is compared with main's branch records")
    for tier in ("nightly", "weekly"):
        check(module.regressions(
            pr, [recipe_record(v, tier=tier) for v in base]) == [],
              f"{tier} records are not a pull request's baseline")
    check(module.regressions(
        recipe_record(14), [recipe_record(v, tier="pr") for v in base]) == [],
          "pull request records are not main's baseline")

    # end to end through the CLI the workflow runs: summarize a PR run, load
    # the branch history file the job fetched, and gate
    results = os.path.join(work, "pr-cli")
    make_config(results, "srt", [(1, "pass", srt_delivery(1.0)),
                                 (16, "pass", srt_delivery(1.0)),
                                 (32, "pass", srt_delivery(1.0))],
                host=RUNNER, cpu=140.0)
    out = os.path.join(work, "pr-cli.json")
    check(summarize(results, out).returncode == 0, "summarize the PR run")
    summary = read_json(out)

    def history_file(name, seconds):
        path = os.path.join(work, name)
        with open(path, "w", encoding="utf-8") as output:
            for index in range(3):
                old = copy.deepcopy(summary)
                old["tier"] = "branch"
                old["sha"] = f"main-{index}"
                config = old["configs"]["srt"]
                config.setdefault("recipe", {})["seconds"] = seconds
                for rung in config["mixes"]["pure-srt"]["rungs"]:
                    rung["sender_cpu_per_gbps"] = 100.0 + index
                output.write(json.dumps(old) + "\n")
        return path
    same = gate(out, history=history_file("branch-same.jsonl", 10))
    check(same.returncode == 0 and "::warning title=bench efficiency" in
          same.stdout,
          f"a PR 1.4x main's CPU at the same recipe must warn: {same.stdout}")
    other = gate(out, history=history_file("branch-other.jsonl", 15))
    check(other.returncode == 0 and "bench efficiency" not in other.stdout,
          f"main's 15 s rungs are not a baseline for 10 s rungs: "
          f"{other.stdout}")


def test_summary_records_recipe_fingerprint_and_provenance(work):
    module = load_module(HISTORY, "bench_history_recipe_record")
    host = dict(RUNNER, build={
        "compiler": "gcc version 15.2.1 20250813 (GCC)",
        "configure_arguments": "--with-cc-opt=-std=gnu2x -O2",
        "sha256": "ab" * 32})
    results = os.path.join(work, "recipe-record")
    make_config(results, "srt", [(1, "pass", srt_delivery(1.0)),
                                 (16, "pass", srt_delivery(1.0)),
                                 (32, "pass", srt_delivery(1.0))],
                host=host, recipe={"seconds": 10})
    out = os.path.join(work, "recipe-record.json")
    check(summarize(results, out).returncode == 0, "summarize must succeed")
    config = read_json(out)["configs"]["srt"]
    revision = getattr(module, "harness_revision", lambda root=None: None)
    recipe = config.get("recipe") or {}
    check(recipe.get("seconds") == 10 and recipe.get("rate") == "8M"
          and recipe.get("steps") == [1, 16, 32]
          and recipe.get("stop_after_failures") == 2
          and recipe.get("mixes") == ["pure-srt"],
          f"the record carries how its rungs were taken: {recipe}")
    check(recipe.get("harness") == revision()
          and len(recipe.get("harness") or "") == 16,
          f"the record carries the harness revision: {recipe}")
    entry = config["mixes"]["pure-srt"]
    check((entry.get("fingerprint") or {}).get("cpu_model") == "runner-a",
          f"the run's fingerprint must reach the published record: "
          f"{entry.get('fingerprint')}")
    check("build" not in (entry.get("fingerprint") or {}),
          "binary provenance is not part of the comparison identity")
    check(entry.get("provenance") == {
        "compiler": "gcc version 15.2.1 20250813 (GCC)",
        "configure_arguments": "--with-cc-opt=-std=gnu2x -O2",
        "binary_sha256": "ab" * 32, "transport": RUNNER["transport"]},
          f"compiler, flags, binary hash and transport are recorded: {entry}")

    bare = os.path.join(work, "recipe-absent")
    make_config(bare, "srt", [(1, "pass", srt_delivery(1.0)),
                              (16, "pass", srt_delivery(1.0)),
                              (32, "pass", srt_delivery(1.0))], recipe=False)
    out = os.path.join(work, "recipe-absent.json")
    summarize(bare, out)
    check(read_json(out)["configs"]["srt"].get("recipe", 1) is None,
          "a log without a recipe line yields no recipe, not a default")

    # the harness revision follows the harness files' content
    copied = os.path.join(work, "harness-copy")
    os.makedirs(copied)
    for name in getattr(module, "HARNESS_FILES", ()):
        shutil.copyfile(os.path.join(HERE, name), os.path.join(copied, name))
    before = revision(copied)
    with open(os.path.join(copied, "capacity-mixes.conf"), "a",
              encoding="utf-8") as output:
        output.write("pure-extra:srt:0\n")
    check(before and before != revision(copied),
          "an edited harness is a different revision")
    os.remove(os.path.join(copied, "srt_fanout_sink.c"))
    check(before and revision(copied) is None,
          "a harness revision that cannot be computed is not invented")


def test_rung_record_keeps_the_preflight_verdict_and_reference():
    """A rung that the preflight called infrastructure-limited, or judged
    against a calibration rate, has to say so in the record."""
    module = load_module(HISTORY, "bench_history_rung_preflight")
    bundle = diagnostics_bundle(128, "infrastructure-limited", {
        "srt": {"value": {"quality_average_delivery_ratio_min": 1.0,
                          "quality_reference_payload_bps": 8_000_000.0}},
        "hls_readers": {"value": {"reference_bps": 4_000_000.0}}})
    bundle["preflight"] = {
        "schema": "nginx-media.capacity-preflight/1",
        "verdict": "infrastructure-limited",
        "limits": [{"side": "network", "resource": "throughput",
                    "measured": 4.1, "required": 8.0, "unit": "Gbit/s"}],
        "unknown": [{"side": "probe", "resource": "receiver-socket-drops",
                     "reason": "probe receiver dropped datagrams"}]}
    rung = module.rung_record(bundle)
    check(rung["preflight"] == {"verdict": "infrastructure-limited",
                                "limits": ["network/throughput"],
                                "unknown": ["probe/receiver-socket-drops"]},
          f"the rung keeps the preflight's verdict and what it named: "
          f"{rung['preflight']}")
    check(rung["reference_bps"] == {"srt": 8_000_000.0,
                                    "hls_readers": 4_000_000.0},
          f"the rung keeps the rates it was judged against: "
          f"{rung['reference_bps']}")
    missing = module.rung_record(diagnostics_bundle(
        1, "pass", srt_delivery(1.0)))
    check(missing["preflight"] is None and missing["reference_bps"] == {},
          "no preflight and no reference are recorded as absent, not as ok")


def main():
    work = tempfile.mkdtemp(prefix="nginx-media-reporting-")
    try:
        test_preflight_classifies_a_short_environment(work)
        test_preflight_names_a_foreign_load_on_the_pinned_cpus(work)
        test_preflight_probe_is_measured_at_both_ends(work)
        test_reader_percentile_helpers()
        test_reader_reports_observed_not_threshold(os.path.join(work, "readers"))
        test_hls_origin_bundle_uses_observed_ratios(work)
        test_old_bundle_threshold_is_never_a_measurement(work)
        test_absent_and_malformed_inputs(work)
        test_bundle_carries_the_host_fingerprint(work)
        test_efficiency_uses_receiver_bytes_for_every_protocol(work)
        test_efficiency_refuses_a_sender_only_counter(work)
        test_history_carries_the_environment_and_peak_pressure(work)
        test_history_carries_the_strict_result_separately(work)
        test_history_separates_observed_from_threshold(work)
        test_summary_marks_uncollected_expected_configs(work)
        test_summary_persists_same_runner_comparison_metadata(work)
        test_regressions_require_complete_passing_same_runner_baselines()
        test_regression_identity_ignores_pressure_but_preserves_environment()
        test_regression_cohorts_do_not_mix_with_qualification_history()
        test_publish_appends_same_runner_samples(work)
        test_parse_expected_configs_validation()
        test_gate_accepts_a_finished_ladder_that_stopped_at_the_boundary(work)
        test_gate_accepts_an_infrastructure_limited_rung(work)
        test_gate_rejects_quality_failure_below_the_required_rung(work)
        test_gate_rejects_missing_rungs_and_workloads(work)
        test_gate_rejects_an_aborted_harness(work)
        test_gate_rejects_a_killed_harness(work)
        test_gate_rejects_a_run_with_no_status_file(work)
        test_gate_rejects_absent_diagnostics(work)
        test_gate_rejects_a_setup_failure(work)
        test_run_queue_pressure_is_recorded(work)
        test_per_lane_rate_keeps_workers_apart(work)
        test_per_cpu_busy_separates_idle_from_saturated(work)
        test_matrix_separates_observed_from_threshold(work)
        test_gate_requires_positive_evidence_of_the_floor_rung(work)
        test_srt_judge_rejects_non_finite_inputs(work)
        test_rtmp_judge_rejects_non_finite_inputs(work)
        test_receiver_evidence_is_required(work)
        test_hls_push_judge_rejects_non_finite_inputs(work)
        test_calibration_is_stored_only_from_a_passing_positive_rung(work)
        test_harness_runs_the_mixes_of_the_one_list(work)
        test_bench_ci_manifest_is_the_harness_mix_list(work)
        test_gate_rejects_a_manifest_that_differs_from_the_executed_mixes(work)
        test_trend_stub_summary_for_every_tier(work)
        test_regressions_compare_only_equal_recipes()
        test_pr_records_compare_with_the_branch_history(work)
        test_summary_records_recipe_fingerprint_and_provenance(work)
        test_rung_record_keeps_the_preflight_verdict_and_reference()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"test_reporting.py: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

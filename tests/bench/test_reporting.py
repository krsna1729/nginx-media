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

import csv
import importlib.util
import io
import json
import os
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

    # the same measurements for a request the environment can carry
    result = preflight("judge", "--destinations", "64", "--bitrate-bps",
                       "8000000", "--sender", sender, "--network", rx,
                       "--probe-sender", tx, "--sender-cpu-per-gbps", "140",
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

def diagnostics_bundle(destinations, outcome, delivery):
    return {"schema": "nginx-media.capacity-diagnostics/2",
            "case": {"destinations": destinations, "mix": "pure-srt"},
            "outcome": outcome, "delivery": delivery,
            "cpu_environment": {"host_cpu_busy_fraction": {"value": 0.5}},
            "efficiency": {"delivered": {"value": 1.0},
                           "sender_cpu_per_gbps": {"value": 100.0},
                           "receiver_cpu_per_gbps": {"value": 50.0}}}


def srt_delivery(ratio):
    return {"srt": {"value": {"quality_average_delivery_ratio_min": ratio}}}


def hls_origin_delivery(threshold, observed=None):
    value = {"min_delivery_ratio": threshold,
             "delivery_ratio_threshold": threshold}
    if observed is not None:
        value["observed_min_delivery_ratio"] = observed
    return {"hls_readers": {"value": value}}


def make_config(results, name, rungs, status=0, finished=True, expected=None,
                stopped=None):
    """One configuration's results directory, as bench-ci.sh leaves it."""
    base = os.path.join(results, name)
    os.makedirs(base, exist_ok=True)
    for destinations, outcome, delivery in rungs:
        case = os.path.join(base, "capacity", f"quality-pure-srt-{destinations}")
        os.makedirs(case, exist_ok=True)
        with open(os.path.join(case, "diagnostics.json"), "w",
                  encoding="utf-8") as output:
            json.dump(diagnostics_bundle(destinations, outcome, delivery), output)
    with open(os.path.join(results, f"{name}.log"), "w",
              encoding="utf-8") as output:
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
        json.dump(expected or {"mixes": ["pure-srt"], "steps": [1, 16, 32]}, out)
    return base


def summarize(results, out):
    return run([sys.executable, HISTORY, "summarize", results, "--tier", "pr",
                "--out", out])


def gate(summary, tier="pr"):
    return run([sys.executable, HISTORY, "gate", summary, "--tier", tier])


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
    check(record["schema"] == "nginx-media.bench-history/2",
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


def main():
    work = tempfile.mkdtemp(prefix="nginx-media-reporting-")
    try:
        test_preflight_classifies_a_short_environment(work)
        test_preflight_probe_is_measured_at_both_ends(work)
        test_reader_percentile_helpers()
        test_reader_reports_observed_not_threshold(os.path.join(work, "readers"))
        test_hls_origin_bundle_uses_observed_ratios(work)
        test_old_bundle_threshold_is_never_a_measurement(work)
        test_absent_and_malformed_inputs(work)
        test_efficiency_uses_receiver_bytes_for_every_protocol(work)
        test_efficiency_refuses_a_sender_only_counter(work)
        test_history_carries_the_strict_result_separately(work)
        test_history_separates_observed_from_threshold(work)
        test_gate_accepts_a_finished_ladder_that_stopped_at_the_boundary(work)
        test_gate_accepts_an_infrastructure_limited_rung(work)
        test_gate_rejects_quality_failure_below_the_required_rung(work)
        test_gate_rejects_missing_rungs_and_workloads(work)
        test_gate_rejects_an_aborted_harness(work)
        test_gate_rejects_a_killed_harness(work)
        test_gate_rejects_a_run_with_no_status_file(work)
        test_gate_rejects_absent_diagnostics(work)
        test_gate_rejects_a_setup_failure(work)
        test_per_lane_rate_keeps_workers_apart(work)
        test_per_cpu_busy_separates_idle_from_saturated(work)
        test_matrix_separates_observed_from_threshold(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"test_reporting.py: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

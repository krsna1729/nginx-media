"""Measuring receivers: RTMP sinks, the SRT fan-out sink, HLS readers, the
HLS push sink and the HLS offer observer.

Receivers run in the receiver namespace pinned to the receiver CPUs; every
request to one goes through the harness namespace. Receivers are local, so
they share the controller's monotonic clock and a native stamp is the edge.
Captures take `gate`, called after preparation and right before the measured
edge, so a caller can release several captures together."""

import concurrent.futures
import contextlib
import csv
import json
import os
import re
import shutil
import signal
import time

from . import config, proc
from .proc import HARNESS, RECEIVER, Failed

CLOCK = "controller-monotonic"
_PAYLOAD = re.compile(r"^nginx_media_source_payload_bytes_in_total\{(.*?)\}\s+([0-9]+)[ \t]*$", re.M)
_LABEL = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:\\.|[^"])*)"')
_RTMP_CONF = """worker_processes {workers};
error_log {errlog};
pid logs/nginx.pid;

events {{ {events} }}

media_rtmp_listen {addr}:{port};

http {{
    server {{
        listen {addr}:{http};
        location /media/api/ {{ media_api; }}
    }}
}}
"""


def _ids(programs, destinations, offset=0):
    """Destination ids as capacity_client.py provisions them."""
    return {"d%04d" % d if programs == 1 else "p%04d-d%04d" % (p, d)
            for p in range(programs) for d in range(offset, offset + destinations)}


def _python(cfg, script, *args):
    return ["python3", cfg.helper(script)] + [str(a) for a in args]


def _gone(*paths):
    for path in paths:
        with contextlib.suppress(FileNotFoundError):
            os.remove(path)


def _exists(path):
    return lambda: os.path.exists(path)


def _tail(path, lines=20):
    try:
        with open(path, errors="replace") as handle:
            return "".join(handle.readlines()[-lines:]).strip()
    except OSError:
        return ""


def _write(path, text):
    with open(path, "w") as out:
        out.write(text)


def _gate(gate):
    if gate:
        gate()


def _edge(window, phase, request_ns, reply_ns, native, evidence):
    """Record a measured edge (start|end) in a receiver window file."""
    if not request_ns <= native <= reply_ns:
        raise Failed("%s: receiver stamp %d outside its request bracket" % (evidence, native))
    _write(evidence + ".request.json", json.dumps({"request_ns": request_ns, "reply_ns": reply_ns}))
    data = {}
    if phase == "end":
        with open(window) as handle:
            data = json.load(handle)
    data.update({"clock_domain": CLOCK, phase + "_ns": native, phase + "_uncertainty_ns": 0,
                 "native_%s_ns" % phase: native, phase + "_request_ns": request_ns,
                 phase + "_reply_ns": reply_ns})
    _write(window, json.dumps(data))


def rtmp_sink_port(cfg, k):
    return cfg.base + 128 + k


def rtmp_sink_http_port(cfg, k):
    return cfg.base + 192 + k


def build_srt_sink(cfg):
    """Compile srt_fanout_sink; the build script writes provenance beside it."""
    binary = cfg.path("srt_fanout_sink")
    proc.run(["python3", os.path.join(cfg.scripts_dir, "build-capacity-receiver.py"),
              "--source", cfg.helper("srt_fanout_sink.c"), "--binary", binary], timeout=660)
    if not os.path.isfile(binary + ".provenance.json"):
        raise Failed("SRT sink build left no provenance at %s.provenance.json" % binary)
    return binary


def prepare_media(cfg, source, case_dir):
    proc.run(_python(cfg, "capacity_media_offer.py", "prepare", source,
                     os.path.join(case_dir, "prepared-media.json")), timeout=120)


def observe_offer(scope, cfg, playlist, case_dir, ns=None, cookie=""):
    """Record the segments the sender produces (hls-production.json); a served
    playlist URL is polled from `ns`, a file path read directly."""
    return scope.spawn("HLS offer observer",
                       _python(cfg, "capacity_media_offer.py", "observe", playlist,
                               os.path.join(case_dir, "hls-production.json"), "--cookie", cookie),
                       ns=ns, monitor=True, log=os.path.join(case_dir, "hls-production.log"))


class RtmpSinks:
    """nginx RTMP receivers, one per peers-per-sink destinations: a single
    instance ingesting a large fanout stalls its own event loop, which reads
    as a delivery stall."""

    def __init__(self, cfg, dirs):
        self.cfg = cfg
        self.dirs = dirs
        self.count = len(dirs)
        self.masters = []
        self.workers = []
        self.sampler = None

    @classmethod
    def start(cls, scope, cfg, destinations, sink_mode):
        fast = sink_mode == "fast"
        peers = max(cfg.rtmp_receiver_peers, 256) if fast else cfg.rtmp_receiver_peers
        count = min(64, -(-destinations // peers))
        if count < 1:
            raise Failed("RTMP sinks need at least one destination")
        print("   rtmp_receivers=%d (<= %d destinations each)" % (count, peers), flush=True)
        # the sender's binary, unless the sender is restream and ships none
        nginx = cfg.rtmp_sink_nginx if cfg.engine == "restream" else cfg.nginx
        sinks = cls(cfg, [cfg.path("rtmp-sink-%d" % k) for k in range(count)])
        for k, path in enumerate(sinks.dirs):
            shutil.rmtree(os.path.join(path, "logs"), ignore_errors=True)
            os.makedirs(os.path.join(path, "conf"), exist_ok=True)
            os.makedirs(os.path.join(path, "logs"))
            _write(os.path.join(path, "conf", "nginx.conf"), _RTMP_CONF.format(
                workers=2 if fast else 1,
                errlog="/dev/null notice" if fast else "logs/error.log info",
                events="worker_connections 16384; use epoll;" if fast else "worker_connections 8192;",
                addr=cfg.receiver_addr, port=rtmp_sink_port(cfg, k), http=rtmp_sink_http_port(cfg, k)))
            argv = [nginx, "-p", path, "-c", "conf/nginx.conf"]
            proc.run(argv + ["-t"], ns=RECEIVER, timeout=30)
            sinks.masters.append(scope.spawn("RTMP sink %d" % k, argv + ["-g", "daemon off;"], ns=RECEIVER,
                                             cpus=cfg.receiver_cpus, log=os.path.join(path, "logs", "nginx.out")))
        for k, master in enumerate(sinks.masters):
            try:
                proc.wait_until("RTMP sink %d API" % k, lambda: sinks._answers(k), 30, alive=[master])
            except Failed as error:
                raise type(error)("%s\n%s" % (error, _tail(master.log)))
            with open("/proc/%d/task/%d/children" % (master.pid, master.pid)) as handle:
                children = sorted(int(pid) for pid in handle.read().split())
            if not children:
                raise Failed("RTMP sink %d has no worker after its API answered" % k)
            sinks.workers.append((children[0], "rtmp-sink-%d" % k))
        return sinks

    def _answers(self, k):
        return proc.curl(self.api(k) + "/metrics", HARNESS, timeout=2, check=False)[0] == 200

    @property
    def pids(self):
        return [pid for pid, _ in self.workers]

    def api(self, k):
        return "http://%s:%d/media/api/v1" % (self.cfg.receiver_addr, rtmp_sink_http_port(self.cfg, k))

    def metric_urls(self):
        return ",".join(self.api(k) + "/metrics" for k in range(self.count))

    def scrape(self, prefix):
        """Stamped metrics per sink as `prefix.<worker pid>`, taken together."""
        with concurrent.futures.ThreadPoolExecutor(self.count) as pool:
            jobs = [pool.submit(proc.run, _python(self.cfg, "rtmp_capacity_quality.py", "scrape",
                                                  self.api(k) + "/metrics", "%s.%d" % (prefix, pid)),
                                ns=HARNESS, timeout=20)
                    for k, (pid, _) in enumerate(self.workers)]
            failed = [str(job.exception()) for job in jobs if job.exception()]
        if failed:
            raise Failed("could not scrape every RTMP sink: " + "; ".join(failed))

    def wait_receiving(self, programs, destinations, offset, timeout):
        """Until every expected destination has delivered payload to a sink."""
        expected = _ids(programs, destinations, offset)

        def receiving():
            received = {}
            for k in range(self.count):
                for labels, value in _PAYLOAD.findall(proc.curl(self.api(k) + "/metrics", HARNESS, timeout=5)[1]):
                    label = dict(_LABEL.findall(labels))
                    name = label.get("name")
                    if label.get("application") == "live" and name in expected and label.get("source") == name:
                        received[name] = int(value)
            return all(received.get(name, 0) > 0 for name in expected)
        proc.wait_until("RTMP receivers accepting media", receiving, timeout, interval=0.1)

    def start_sampler(self, scope, case_dir):
        ready = os.path.join(case_dir, "rtmp-sampler.ready")
        _gone(ready, ready + ".paused", os.path.join(case_dir, "rtmp-sampler.baseline"))
        self.sampler = scope.spawn(
            "RTMP quality sampler",
            _python(self.cfg, "rtmp_capacity_quality.py", "sample", self.metric_urls(),
                    os.path.join(case_dir, "rtmp-receiver.before"),
                    os.path.join(case_dir, "rtmp-receiver-intervals.csv"), "--interval", "1",
                    "--ready-file", ready, "--baseline-ready", os.path.join(case_dir, "rtmp-sampler.baseline")),
            monitor=True, ns=HARNESS, log=os.path.join(case_dir, "rtmp-sampler.log"))
        proc.wait_until("RTMP sampler ready", _exists(ready), 15, alive=[self.sampler])
        return self.sampler

    def capture(self, case_dir, phase, gate=None):
        """rtmp-receiver.before|after.<pid>; the sampler pauses before the end
        scrape and starts counting after the start one."""
        if phase == "end" and self.sampler:
            self.sampler.signal(signal.SIGUSR2)
            # it acknowledges after its in-flight scrape (up to 10 s)
            proc.wait_until("RTMP sampler pause", _exists(os.path.join(case_dir, "rtmp-sampler.ready.paused")),
                            30, interval=0.01, alive=[self.sampler])
        _gate(gate)
        self.scrape(os.path.join(case_dir, "rtmp-receiver." + ("before" if phase == "start" else "after")))
        if phase == "start" and self.sampler:
            _write(os.path.join(case_dir, "rtmp-sampler.baseline"), "")


class SrtSink:
    """The native SRT fan-out sink: snapshot on USR1, measurement edge on USR2,
    result CSV on TERM. It serialises its whole peer table for each, so every
    deadline scales with the peers."""

    def __init__(self, cfg, case_dir, destinations):
        self.cfg = cfg
        self.case_dir = case_dir
        self.total = destinations
        self.budget = 10 + destinations // 8
        self.port = cfg.base + 64
        self.listeners = max(1, min(256, -(-destinations // cfg.srt_receiver_peers)))
        self.ready = os.path.join(case_dir, "srt.ready")
        self.csv = os.path.join(case_dir, "srt.csv")
        self.snap = self.csv + ".snapshot"
        self.log = os.path.join(case_dir, "srt-receiver.log")
        self.proc = self.sampler = None

    @classmethod
    def start(cls, scope, cfg, case_dir, destinations, sink_mode):
        sink = cls(cfg, case_dir, destinations)
        print("   srt_receiver_listeners=%d (<= %d destinations each)"
              % (sink.listeners, cfg.srt_receiver_peers), flush=True)
        _gone(sink.ready, sink.csv, sink.snap)
        sink.proc = scope.spawn(
            "SRT receiver",
            ["env", "SRT_SINK_BIND=" + cfg.receiver_addr, "SRT_SINK_MODE=" + sink_mode,
             cfg.path("srt_fanout_sink"), "%d:%d" % (sink.port, sink.listeners), destinations,
             sink.ready, sink.csv, "quality"],
            ns=RECEIVER, cpus=cfg.receiver_cpus, log=sink.log)
        return sink

    @property
    def pid(self):
        return self.proc.pid

    def wait_ready(self, timeout, programs, destinations):
        """Until every peer connected; otherwise the reason names who is missing."""
        try:
            proc.wait_until("SRT receiver accepting every peer", _exists(self.ready), timeout,
                            interval=0.1, alive=[self.proc])
        except proc.Timeout:
            incomplete = os.path.join(self.case_dir, "srt.incomplete.csv")
            try:
                self.snapshot(incomplete)
                detail = self._incomplete(incomplete, programs, destinations)
            except Failed as error:
                detail = "no incomplete snapshot: %s" % error
            raise Failed("SRT receiver accepted fewer than %d peers in a %ss connect window: %s; "
                         "the receiver's CPU and %s separate a sender that offered too few from "
                         "a receiver that accepted too few" % (self.total, timeout, detail, self.log))

    @staticmethod
    def _incomplete(path, programs, destinations):
        expected = _ids(programs, destinations)
        with open(path, newline="") as source:
            reader = csv.DictReader(source)
            if not reader.fieldnames or "destination_id" not in reader.fieldnames:
                raise Failed("SRT snapshot %s does not match its schema" % path)
            accepted = {row["destination_id"] for row in reader}
        return "accepted %d of %d peers; missing=%s unexpected=%s" % (
            len(accepted), len(expected), sorted(expected - accepted)[:16], sorted(accepted - expected)[:4])

    def _signal_and_wait(self, signo, what, gate):
        _gone(self.snap)
        _gate(gate)
        begin = time.monotonic_ns()
        self.proc.signal(signo)
        proc.wait_until("SRT receiver %s" % what, _exists(self.snap), self.budget,
                        interval=0.002, alive=[self.proc])
        end = time.monotonic_ns()
        if not os.path.getsize(self.snap):
            raise Failed("SRT receiver %s was empty" % what)
        return begin, end

    def snapshot(self, output, gate=None):
        _gone(output)
        self._signal_and_wait(signal.SIGUSR1, "snapshot", gate)
        shutil.copyfile(self.snap, output)

    def start_sampler(self, scope):
        """Per-interval receiver deltas; starts counting once srt.before.csv exists."""
        ready = os.path.join(self.case_dir, "receiver-sampler.ready")
        before = os.path.join(self.case_dir, "srt.before.csv")
        _gone(ready, ready + ".paused", before)
        self.sampler = scope.spawn(
            "SRT receiver sampler",
            _python(self.cfg, "srt_capacity_quality.py", "sample-receivers", self.pid, self.snap, before,
                    os.path.join(self.case_dir, "receiver-intervals.csv"), "--interval", "1",
                    "--ready-file", ready),
            monitor=True, ns=RECEIVER, log=os.path.join(self.case_dir, "receiver-sampler.log"))
        proc.wait_until("SRT receiver sampler ready", _exists(ready), 15, alive=[self.sampler, self.proc])
        return self.sampler

    def measure(self, phase, gate=None):
        """USR2 edge -> srt.before|after.csv and receiver.measurement.window."""
        if phase == "end" and self.sampler:
            # its outstanding USR1 snapshot completes before it acknowledges
            self.sampler.signal(signal.SIGUSR2)
            proc.wait_until("SRT receiver sampler pause",
                            _exists(os.path.join(self.case_dir, "receiver-sampler.ready.paused")),
                            self.budget, interval=0.002, alive=[self.sampler])
        output = os.path.join(self.case_dir, "srt.%s.csv" % ("before" if phase == "start" else "after"))
        _gone(output)
        begin, end = self._signal_and_wait(signal.SIGUSR2, "measurement %s snapshot" % phase, gate)
        shutil.copyfile(self.snap, output + ".tmp")
        os.replace(output + ".tmp", output)
        with open(output, newline="") as source:
            stamps = {int(row["snapshot_ns"]) for row in csv.DictReader(source)}
        if len(stamps) != 1:
            raise Failed("SRT receiver %s snapshot boundaries disagree (%d stamps)" % (phase, len(stamps)))
        _edge(os.path.join(self.case_dir, "receiver.measurement.window"), phase, begin, end,
              stamps.pop(), output)

    def stop(self):
        """TERM; the sink writes its result CSV on the way out."""
        grace = 2 * self.budget
        self.proc.signal(signal.SIGTERM)
        try:
            status = self.proc.wait(grace)
        except proc.Timeout:
            self.proc.stop(0)
            raise Failed("SRT receiver did not exit within %ss of TERM" % grace)
        if status != 0:
            raise Failed("SRT receiver exited %s: %s" % (status, _tail(self.log)))
        if not os.path.isfile(self.csv) or not os.path.getsize(self.csv):
            raise Failed("SRT receiver produced no result CSV")
        return self.csv


class HlsReaders:
    """hls_capacity_readers.py pulling the sender's playlist; USR1/USR2 open
    and seal its measurement window."""

    def __init__(self, case_dir, process):
        self.case_dir = case_dir
        self.proc = process
        self.report = os.path.join(case_dir, "hls-readers.csv")
        self.ready = os.path.join(case_dir, "hls.ready")
        self.log = os.path.join(case_dir, "hls-reader.log")

    @classmethod
    def start(cls, scope, cfg, case_dir, url, readers, seconds, reference_bps, cookie=""):
        report, ready = os.path.join(case_dir, "hls-readers.csv"), os.path.join(case_dir, "hls.ready")
        _gone(report, ready)
        return cls(case_dir, scope.spawn(
            "HLS readers",
            _python(cfg, "hls_capacity_readers.py", "--url", url, "--readers", readers,
                    "--duration", "%g" % (seconds + 60), "--report", report, "--ready-file", ready,
                    "--reference-bps", reference_bps, "--clock-domain", CLOCK,
                    "--min-delivery-ratio", cfg.min_delivery_ratio, "--cookie", cookie),
            monitor=True, ns=RECEIVER, cpus=cfg.receiver_cpus, log=os.path.join(case_dir, "hls-reader.log")))

    @property
    def pid(self):
        return self.proc.pid

    def wait_ready(self, timeout):
        try:
            proc.wait_until("HLS readers warm", _exists(self.ready), timeout, interval=0.1, alive=[self.proc])
        except Failed as error:
            raise type(error)("%s\n%s" % (error, _tail(self.log)))

    def boundary(self, phase, gate=None):
        """Open (start) or seal (end) the readers' window -> hls-readers.measurement.window."""
        ack = "%s.%s.json" % (self.report, phase)
        _gone(ack)
        _gate(gate)
        begin = time.monotonic_ns()
        self.proc.signal(signal.SIGUSR1 if phase == "start" else signal.SIGUSR2)
        proc.wait_until("HLS readers %s acknowledgment" % phase, _exists(ack), 10,
                        interval=0.002, alive=[self.proc])
        end = time.monotonic_ns()
        with open(ack) as handle:
            data = json.load(handle)
        if data["clock_domain"] != CLOCK:
            raise Failed("HLS readers acknowledged on %s" % data["clock_domain"])
        _edge(os.path.join(self.case_dir, "hls-readers.measurement.window"), phase, begin, end,
              data["native_ns"], ack)


class HlsPushSink:
    """hls_push_capacity.py serve: the destination of every HLS push upload."""

    def __init__(self, cfg, case_dir):
        self.cfg = cfg
        self.case_dir = case_dir
        self.port = cfg.base + 30
        self.log = os.path.join(case_dir, "hls-push-sink.log")
        self.progress_json = os.path.join(case_dir, "hls-push-readiness.json")
        self.progress_csv = os.path.join(case_dir, "hls-push-readiness.csv")
        self.proc = None

    @classmethod
    def start(cls, scope, cfg, case_dir, destinations):
        sink = cls(cfg, case_dir)
        # Every destination of one rung shares one sink port, so the addresses
        # are what give each destination its own source-port budget.
        addresses = config.receiver_addresses(cfg.receiver_addr, cfg.receiver_addr_count,
                                              destinations)
        binds = [flag for address in addresses for flag in ("--bind", address)]
        sink.addresses = addresses
        sink.proc = scope.spawn(
            "HLS push sink",
            _python(cfg, "hls_push_capacity.py", "serve", "--port", sink.port, *binds),
            ns=RECEIVER, cpus=cfg.receiver_cpus, log=sink.log)
        try:
            proc.wait_until("HLS push sink health", lambda: sink._get("__health", 2, False)[0] == 200,
                            15, interval=0.1, alive=[sink.proc])
        except Failed as error:
            raise type(error)("%s\n%s" % (error, _tail(sink.log)))
        return sink

    @property
    def pid(self):
        return self.proc.pid

    def _get(self, path, timeout=10, check=True):
        return proc.curl("http://%s:%d/%s" % (self.cfg.receiver_addr, self.port, path), HARNESS,
                         timeout=timeout, check=check)

    def mark(self):
        """Start the first-segment clock (hls-push.mark.json)."""
        _write(os.path.join(self.case_dir, "hls-push.mark.json"), self._get("__mark")[1])

    def _progress(self, query, destinations):
        body = self._get("__progress?" + query)[1]
        _write(self.progress_json, body)
        progress = json.loads(body)
        with open(self.progress_csv, "a") as out:
            out.write("%.3f,%s,%d,%d\n" % (progress["elapsed_s"] or 0, progress["received"],
                                           destinations, len(progress["missing_ids"])))
        return progress

    def wait_ready(self, destinations, offset, timeout):
        """Until every destination delivered a TS segment; progress goes to
        hls-push-readiness.{csv,json} and stdout."""
        query = "destinations=%d&offset=%d" % (destinations, offset)
        _write(self.progress_csv, "elapsed_s,received,expected,remaining\n")
        state = {"next": 0.0, "received": None}

        def ready():
            status = self._get("__ready?" + query, 5, False)[0]
            if status == 200 or time.monotonic() >= state["next"]:
                state["next"] = time.monotonic() + 1
                progress = self._progress(query, destinations)
                if progress["received"] != state["received"]:
                    state["received"] = progress["received"]
                    print("   HLS push first-segment readiness: elapsed=%.3fs received=%s/%d remaining=%d"
                          % (progress["elapsed_s"] or 0, progress["received"], destinations,
                             len(progress["missing_ids"])), flush=True)
            return status == 200
        try:
            proc.wait_until("HLS push first segment from every destination", ready, timeout,
                            interval=0.1, alive=[self.proc])
        except proc.Timeout:
            progress = self._progress(query, destinations)
            raise proc.Timeout("HLS push readiness timed out: received=%s/%s remaining_ids=%s (%s)"
                               % (progress["received"], progress["expected"],
                                  ",".join(progress["missing_ids"]), self.progress_csv))
        with open(self.progress_json) as handle:
            progress = json.load(handle)
        print("   HLS push all-destination first-segment latency=%.3fs (%s/%s)"
              % (progress["elapsed_s"] or 0, progress["received"], progress["expected"]))
        print("   HLS push readiness progress=%s" % self.progress_csv, flush=True)

    def diagnose_readiness(self, stream, programs, destinations, offset, metrics_prefix):
        """After a readiness timeout: sender vs sink view of the missing
        destinations (hls-push-readiness-report.txt), given worker metrics."""
        self._progress("destinations=%d&offset=%d" % (destinations, offset), destinations)
        snapshot = os.path.join(self.case_dir, "hls-push.readiness.json")
        _write(snapshot, self._get("__snapshot")[1])
        text = proc.run(_python(self.cfg, "hls_push_capacity.py", "readiness-report",
                                "--metrics-prefix", metrics_prefix, "--stream", stream,
                                "--destinations", destinations, "--programs", programs,
                                "--destination-offset", offset, "--sink-snapshot", snapshot,
                                "--progress", self.progress_json), timeout=60).stdout
        _write(os.path.join(self.case_dir, "hls-push-readiness-report.txt"), text)
        return text

    def snapshot(self, phase, gate=None):
        """hls-push.before|after.json and hls-push.measurement.window."""
        output = os.path.join(self.case_dir, "hls-push.%s.json" % ("before" if phase == "start" else "after"))
        _gate(gate)
        begin = time.monotonic_ns()
        body = self._get("__snapshot")[1]
        end = time.monotonic_ns()
        _write(output, body)
        _edge(os.path.join(self.case_dir, "hls-push.measurement.window"), phase, begin, end,
              int(json.loads(body)["monotonic"] * 1e9), output)

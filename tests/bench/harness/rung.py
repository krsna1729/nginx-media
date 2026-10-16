"""One quality rung: the sender and the measuring receivers for one mix and
size, one measured window, every protocol judged against its one-destination
reference, and the evidence capacity_diagnostics.py reads."""

import collections
import dataclasses
import functools
import glob
import itertools
import json
import math
import os
import re
import shutil
import sqlite3
import sys
import threading
import time

from . import api, cpu, media, proc, receivers, sender

KEYS = ("srt", "rtmp", "hls", "hls_push")
LABEL = {"srt": "SRT", "rtmp": "RTMP", "hls": "HLS", "hls_push": "HLS push"}
TAG = {"srt": "quality", "rtmp": "rtmp_quality", "hls": "hls_quality", "hls_push": "hls_push_quality"}
PROVENANCE = "controller-stream-counter-collection-brackets"
_CASE_IDS = itertools.count(1)
_SAMPLE = re.compile(r"^([A-Za-z_:][\w:]*)(?:\{(.*?)\})?\s+(\S+)")
_PAIR = re.compile(r'([A-Za-z_]\w*)="((?:\\.|[^"])*)"')


class Infrastructure(proc.Failed):
    """The host or the path cannot carry the offer: unmeasured, not a failure."""


@dataclasses.dataclass
class Spec:
    mix: str
    protocol: str
    destinations: int
    counts: dict
    sink_mode: str
    programs: int = 1


def split(protocol, destinations, srt_share=0, hls_push_share=0):
    """Destinations per protocol, with the ladder's nearest-integer share rounding."""
    counts = dict.fromkeys(KEYS, 0)
    counts["srt"] = destinations if protocol == "srt" else (destinations * srt_share + 50) // 100
    primary = destinations - counts["srt"]
    if protocol == "rtmp-hls-push":
        counts["hls_push"] = (destinations * hls_push_share + 50) // 100
        counts["rtmp"] = primary - counts["hls_push"]
    elif protocol != "srt":
        counts[{"rtmp": "rtmp", "hls": "hls", "hls-push": "hls_push"}[protocol]] = primary
    return counts


def positive_rate(text):
    """A finite positive decimal (no sign, exponent, nan or inf)."""
    return bool(re.fullmatch(r"[0-9]+(\.[0-9]+)?", text or "")) and float(text) > 0


def store_reference(refs, key, quality_pass, rate):
    """Keep a calibration rung's rate as `key`'s reference only if the rung
    passed; a rate that is not a positive number is an analyzer error."""
    if not positive_rate(rate):
        raise proc.Failed("%s quality analyzer returned an invalid reference rate" % LABEL[key])
    if quality_pass != "yes":
        print("   %s calibration rung failed its quality checks; its rate %s is not stored "
              "as the reference" % (LABEL[key], rate), file=sys.stderr)
        return False
    refs[key] = rate
    print("   calibrated_%s_reference_bps=%s" % (TAG[key], rate))
    return True


def controller_boundary(path, phase, begin, end, provenance):
    """Record a controller collection bracket; at the end, the span in seconds."""
    if begin <= 0 or end < begin:
        raise proc.Failed("invalid controller collection bracket for %s" % path)
    window = {}
    if phase == "end":
        with open(path) as handle:
            window = json.load(handle)
    window.update(clock_domain=receivers.CLOCK, provenance=provenance)
    window.update({phase + "_request_ns": begin, phase + "_reply_ns": end,
                   phase + "_ns": (begin + end) // 2, phase + "_uncertainty_ns": (end - begin) // 2})
    with open(path, "w") as handle:
        json.dump(window, handle)
    if phase == "end":
        span = (window["end_ns"] - window["start_ns"]) / 1e9
        if span <= 0:
            raise proc.Failed("controller collection interval did not advance")
        return span
    return None


def together(captures, released=None, prepare=50, join=120):
    """A cpu_window boundary hook: each capture runs in its own thread and is
    parked at one gate once prepared. Returns the callable that opens the gate
    (then `released`) and joins every capture; any failure is raised."""
    parked = threading.Barrier(len(captures) + 1)
    release = threading.Event()
    errors = []

    def gate():
        parked.wait(prepare)
        if not release.wait(prepare):
            raise proc.Timeout("the capture gate never opened")

    def body(capture):
        try:
            capture(gate)
        except BaseException as error:  # noqa: BLE001 - reported by the caller's thread
            errors.append(error)
            parked.abort()

    def first_error(default):
        real = [e for e in errors if not isinstance(e, threading.BrokenBarrierError)]
        return real[0] if real else default
    threads = [threading.Thread(target=body, args=(c,), daemon=True) for c in captures]
    for thread in threads:
        thread.start()
    try:
        parked.wait(prepare)
    except threading.BrokenBarrierError:
        for thread in threads:
            thread.join(5)
        raise first_error(proc.Timeout("captures not prepared within %ss" % prepare))

    def fire():
        release.set()
        if released:
            released()
        deadline = time.monotonic() + join
        for thread in threads:
            thread.join(max(0, deadline - time.monotonic()))
        if any(thread.is_alive() for thread in threads):
            raise proc.Timeout("boundary captures did not finish within %ss" % join)
        if errors:
            raise first_error(proc.Failed("a boundary capture failed"))
    return fire


def _samples(prefix):
    """(metric, labels, value) of every Prometheus sample in prefix.*"""
    for path in glob.glob(prefix + ".*"):
        with open(path, errors="replace") as handle:
            for match in filter(None, map(_SAMPLE.match, handle)):
                try:
                    yield match.group(1), dict(_PAIR.findall(match.group(2) or "")), float(match.group(3))
                except ValueError:
                    continue


def shard_report(before_prefix, after_prefix):
    """Per SRT sender shard: queue gauges after the window and counter deltas across it."""
    gauges = ("destinations", "feed_queue_units", "feed_queue_bytes",
              "output_queue_units", "output_queue_bytes")
    deltas = {"sent_bytes": "sent_bytes_total", "sent_bursts": "sent_bursts_total",
              "blocked_sends": "blocked_sends_total",
              "retransmitted_packets": "retransmitted_packets_total",
              "feed_drops": "feed_queue_dropped_total", "output_drops": "output_dropped_total"}
    lines = []
    snapshots = []
    for label, prefix in (("before", before_prefix), ("after", after_prefix)):
        values, active = {}, []
        for name, labels, value in _samples(prefix):
            if name == "nginx_media_egress_active_workers" and labels.get("engine") == "srt_shard":
                active.append(int(value))
            elif (name.startswith("nginx_media_srt_egress_shard_") and "worker" in labels
                  and "shard" in labels and math.isfinite(value)):
                values[(labels["worker"], labels["shard"], name[len("nginx_media_srt_egress_shard_"):])] = value
        lines.append("srt_active_senders_%s=%s" % (label, max(active) if active else "unavailable"))
        snapshots.append(values)
    before, after = snapshots
    every = {key[:2] for key in set(before) | set(after)}
    shards = sorted(k for k in every if after.get(k + ("destinations",), 0) > 0
                    or before.get(k + ("destinations",), 0) > 0)
    if not every or not shards:
        return lines + ["srt_shard_metrics=%s" % ("unavailable" if not every else "no_active_destinations")]
    sent = []
    for shard in shards:
        fields = [(name, after.get(shard + (name,), 0.0)) for name in gauges]
        fields += [(name + "_delta", after.get(shard + (metric,), 0.0) - before.get(shard + (metric,), 0.0))
                   for name, metric in deltas.items()]
        sent.append(fields[len(gauges)][1])
        lines.append("shard worker=%s shard=%s " % shard + " ".join(
            "%s=%s" % (name, int(v) if v.is_integer() else "%g" % v) for name, v in fields))
    squares = sum(v * v for v in sent)
    lines.append("shard_sent_bytes_spread=%.0f..%.0f range=%.0f jain=%.4f" % (
        min(sent), max(sent), max(sent) - min(sent),
        sum(sent) ** 2 / (len(sent) * squares) if squares else 0.0))
    return lines


@functools.lru_cache(maxsize=None)
def srt_library(nginx):
    """robotweax/srt links statically and reports libsrt's version, so ldd alone cannot tell."""
    try:
        with open(nginx, "rb") as handle:
            if b"robotweax" in handle.read():
                return "robotweax-srt"
    except OSError:
        return ""
    done = proc.run(["ldd", nginx], timeout=30, check=False)
    return next((line.split()[0] for line in done.stdout.splitlines() if "libsrt" in line), "")


def _kv(text):
    values = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            values.setdefault(key.strip(), value.strip())
    return values


def _read(path):
    with open(path, errors="replace") as handle:
        return handle.read()


def _write(path, text):
    with open(path, "w") as handle:
        handle.write(text)


class Rung:
    """One rung's state, so a failure at any step can still say what ran."""

    def __init__(self, cfg, case_dir, spec):
        self.cfg, self.dir, self.spec = cfg, case_dir, spec
        n = spec.counts
        self.total = {key: spec.programs * n[key] for key in KEYS}
        self.workers = 1 if n["rtmp"] else cfg.workers  # RTMP receiver metrics are worker-local
        self.hls = not ((n["rtmp"] and not (n["srt"] or n["hls"] or n["hls_push"]))
                        or (spec.protocol == "srt" and cfg.srt_hls == "no"))
        self.inst = self.rtmp = self.srt = self.push = self.readers = self.queue_sampler = None
        self.source = None

    def path(self, *names):
        return os.path.join(self.dir, *names)

    def meta(self):
        n = self.spec.counts
        return {"destinations": self.spec.destinations, "programs": self.spec.programs,
                "rate": self.cfg.quality_rate, "protocol": self.spec.protocol, "srt": n["srt"],
                "rtmp": n["rtmp"], "hls_readers": n["hls"], "hls_push": n["hls_push"],
                "hls": "yes" if self.hls else "no", "workers": self.workers}

    def diagnostics(self, outcome, **extra):
        """diagnostics.json via capacity_diagnostics.py build, with the runtime
        logs of whatever this rung started; a bundle that cannot be built fails."""
        cfg, logs = self.cfg, self.path("runtime-logs")
        if self.inst and os.path.isdir(cfg.path("logs")):
            shutil.copytree(cfg.path("logs"), os.path.join(logs, "sender"), dirs_exist_ok=True)
        if self.inst and self.inst.restream:
            for name in ("logs", "db", "cookie.txt"):
                source, target = cfg.path("restream", name), self.path("restream", name)
                if os.path.isdir(source):
                    shutil.copytree(source, target, dirs_exist_ok=True)
                elif os.path.exists(source):
                    shutil.copyfile(source, target)
            database = cfg.path("restream", "db", "restream.db")
            if os.path.exists(database):
                with sqlite3.connect(database) as db:
                    rows = db.execute("select desired_state, count(*) from outputs group by desired_state")
                    _write(self.path("restream-outputs.csv"), "desired_state,outputs\n" + "".join(
                        "%s,%s\n" % row for row in rows))
        for source in self.rtmp.dirs if self.rtmp else ():
            shutil.copytree(source, os.path.join(logs, os.path.basename(source)), dirs_exist_ok=True)
        if not os.path.exists(self.path("host-fingerprint.json")):
            done = proc.run([sys.executable, cfg.helper("capacity_preflight.py"), "host", "--role",
                             "sender", "--binary", cfg.nginx, "--json", self.path("host-fingerprint.json")],
                            timeout=60, check=False)
            _write(self.path("host-fingerprint.log"), done.stdout + done.stderr)
            if done.returncode != 0:
                print("   host fingerprint could not be taken for %s" % self.dir, file=sys.stderr)
        meta = {"outcome": outcome, "mix": self.spec.mix, "sink_mode": self.spec.sink_mode,
                "srt_senders": cfg.srt_workers, "hls_push_uploaders": cfg.hls_push_workers,
                "srt_backend": srt_library(cfg.nginx), "nginx_cpus": cfg.nginx_cpus or "unpinned",
                "receiver_cpus": cfg.receiver_cpus or "unpinned",
                "receiver_topology": "namespace" if proc.prefix(proc.RECEIVER) else "loopback",
                "receiver_address": cfg.receiver_addr}
        meta.update(self.meta(), **extra)
        argv = [sys.executable, cfg.helper("capacity_diagnostics.py"), "build", self.dir]
        for key, value in meta.items():
            argv += ["--meta", "%s=%s" % (key, value)]
        done = proc.run(argv + ["--print-summary"], timeout=300, check=False)
        sys.stdout.write(done.stdout)
        if done.returncode != 0:
            raise proc.Failed("diagnostics bundle could not be written for %s: %s"
                              % (self.dir, done.stderr.strip()[-400:]))

    def _judge(self, refs, key, argv, report, rate_key="quality_reference_payload_bps"):
        """Run one protocol's quality analyzer; a rung without that protocol's
        reference is its calibration. True when the protocol passed."""
        reference = refs.get(key, "0")
        done = proc.run(argv + ["--reference-bps", reference], timeout=120 + self.total[key])
        with open(report, "a" if key == "hls" else "w") as handle:
            handle.write(done.stdout)
        text = _read(report)
        sys.stdout.write(text)
        values = _kv(text)
        status = values.get("quality_pass")
        if status not in ("yes", "no"):
            raise proc.Failed("%s quality analyzer returned no status" % LABEL[key])
        if reference == "0":
            store_reference(refs, key, status, values.get(rate_key, ""))
        return status == "yes"

    def _bracket(self, window, phase, provenance, collect):
        begin = time.monotonic_ns()
        collect()
        return controller_boundary(self.path(window), phase, begin, time.monotonic_ns(), provenance)

    def _stream(self, name, phase):
        def collect():
            text = api.owner_metrics(self.cfg, name)
            if text is None and not self.inst.restream:
                raise proc.Failed("could not capture %s metrics for %s" % (phase, name))
            _write(self.path("stream.%s.%s" % (name, phase)), text or "")
        return self._bracket("stream.measurement.window", "start" if phase == "before" else "end",
                             "controller stream-counter collection brackets", collect)

    def _workers(self, phase):
        self._bracket("workers.measurement.window", "start" if phase == "before" else "end",
                      "controller worker-metric collection brackets",
                      lambda: api.worker_metrics(self.cfg, self.inst, self.path("workers." + phase),
                                                 timeout=20 + self.workers * 5))

    def _boundary(self, action):
        phase = "start" if action == "start" else "end"
        captures = []
        if self.rtmp:
            captures.append(lambda gate: self.rtmp.capture(self.dir, phase, gate))
        if self.srt:
            captures.append(lambda gate: self.srt.measure(phase, gate))
        if self.push:
            captures.append(lambda gate: self.push.snapshot(phase, gate))
        if self.readers:
            captures.append(lambda gate: self.readers.boundary(phase, gate))
        baseline = self.path("queue-sampler.baseline") if self.srt and phase == "start" else None
        return together(captures, released=baseline and (lambda: _write(baseline, "")),
                        join=60 + sum(self.total.values()) // 4)

    def prepare(self):
        """What the preflight and the window need: the SRT sink and the source."""
        n = self.spec.counts
        if self.cfg.engine == "restream" and (n["hls"] or n["hls_push"]):
            raise proc.Failed("restream HLS cannot be judged: the HLS offer observer reads the nginx playlist")
        if n["srt"]:
            receivers.build_srt_sink(self.cfg)
        self.source = self.cfg.path("media", "capacity-%s.ts" % self.cfg.quality_rate)
        media.make_source(self.cfg, self.source, self.cfg.quality_rate)
        receivers.prepare_media(self.cfg, self.source, self.dir)

    def run(self, scope, refs, preflight_note=None):
        """Measure and judge the rung; True when every protocol passed."""
        cfg, spec, n, total, source = self.cfg, self.spec, self.spec.counts, self.total, self.source
        rate_bps = media.rate_bps(cfg.quality_rate)
        all_dest = sum(total.values())
        name = api.owner_names(0, self.workers, 1, "cc%d-" % next(_CASE_IDS))[0]
        print()
        print("== capacity %s %s: workers=%d programs=%d" % (spec.protocol, os.path.basename(self.dir),
                                                            self.workers, spec.programs))
        print("   video_rate=%s offered_ingress=%.1fMbit/s" % (cfg.quality_rate, spec.programs * rate_bps / 1e6))
        print("   destinations_per_program=%d total_destinations=%d" % (spec.destinations, all_dest))
        print("   per_program_outputs: SRT=%d RTMP=%d HLS_readers=%d HLS_push=%d"
              % (n["srt"], n["rtmp"], n["hls"], n["hls_push"]))
        print("   egress_workers: SRT=%s HLS_push=%s" % (cfg.srt_workers, cfg.hls_push_workers))
        if spec.protocol == "srt":
            print("   srt_hls_preparation=%s" % cfg.srt_hls)
        print("   offered_egress=%.1fMbit/s window=%gs" % (all_dest * rate_bps / 1e6, cfg.quality_seconds),
              flush=True)
        self.inst = inst = sender.start(scope, cfg, self.workers, hls=self.hls, rtmp=False)
        if n["rtmp"]:
            self.rtmp = receivers.RtmpSinks.start(scope, cfg, total["rtmp"], spec.sink_mode)
        if n["srt"]:
            self.srt = receivers.SrtSink.start(scope, cfg, self.dir, total["srt"], spec.sink_mode)
        if n["hls_push"]:
            self.push = receivers.HlsPushSink.start(scope, cfg, self.dir)
        restream = cfg.engine == "restream"
        if not restream:
            proc.curl(inst.api + "/streams", proc.SENDER, method="POST",
                      headers=("Content-Type: application/json",),
                      data=json.dumps({"application": "live", "name": name}))
            owner = api.owner_of(cfg, name)
            if owner != 0:
                raise proc.Failed("%s owner %s did not match expected worker 0" % (name, owner))
        argv = [sys.executable, cfg.helper("capacity_client.py"), "--api", inst.api, "--workers",
                cfg.workers, "provision", "--engine", cfg.engine, "--stream", "live/" + name,
                "--spec", "srt:%d" % n["srt"], "--spec", "rtmp:%d" % n["rtmp"],
                "--spec", "hls_push:%d" % n["hls_push"], "--program-index", 0,
                "--programs", spec.programs, "--receiver-addr", cfg.receiver_addr,
                "--srt-base-port", self.srt.port if self.srt else cfg.base + 64,
                "--srt-listeners", self.srt.listeners if self.srt else 1, "--rtmp-base", cfg.base,
                "--rtmp-sinks", self.rtmp.count if self.rtmp else 1,
                "--hls-push-port", self.push.port if self.push else 0,
                "--path", cfg.path("hls", "live", name)]
        if restream:
            argv += ["--pipeline", inst.pipeline_id, "--cookie-file", inst.cookie]
        done = proc.run(argv, ns=proc.SENDER, timeout=60 + all_dest // 2, check=False)
        if done.returncode != 0:
            raise proc.Failed("could not provision the destinations for %s: %s"
                              % (name, (done.stdout + done.stderr).strip()[-800:]))
        connect = 30 + all_dest / 10
        if not restream:
            def started():
                log = _read(inst.error_log) if os.path.exists(inst.error_log) else ""
                return all(len(re.findall(r"media: %s destination .* started" % kind, log)) >= total[key]
                           for kind, key in (("srt", "srt"), ("rtmp", "rtmp"), ("hls push", "hls_push")))
            proc.wait_until("%d SRT, %d RTMP and %d HLS push destinations started"
                            % (total["srt"], total["rtmp"], total["hls_push"]), started, connect, 0.1)
        if self.push:
            self.push.mark()
        playlist = cfg.path("hls", "live", name, "index.m3u8")
        if n["hls"] or n["hls_push"]:
            receivers.observe_offer(scope, cfg, playlist, self.dir)
        publisher = media.publish(scope, cfg, inst.srt_ports[0], name, source)
        if self.srt:
            self.srt.wait_ready(connect, spec.programs, n["srt"])
        if not restream:
            labels = {"application": "live", "name": name}

            def progressing():
                text = api.owner_metrics(cfg, name, timeout=5) or ""
                return all((api.metric(text, series, labels) or 0) > 0 for series in (
                    "nginx_media_stream_program_frames", "nginx_media_stream_dispatched_total"))
            proc.wait_until("capacity stream program/output progress", progressing, 30, 0.1,
                            alive=[publisher])
        if self.rtmp:
            self.rtmp.wait_receiving(spec.programs, n["rtmp"], n["srt"], connect)
        if n["hls"]:
            print("   waiting for first completed HLS segment", flush=True)
            proc.wait_until("HLS segmenter publishing %s" % playlist,
                            lambda: os.path.exists(playlist) and os.path.getsize(playlist) > 0,
                            connect, 0.1, alive=[publisher])
            url = ("http://127.0.0.1:%d" % cfg.api_port if cfg.sender_addr == "127.0.0.1"
                   else "http://%s:%d" % (cfg.sender_addr, cfg.api_port + 31))
            self.readers = receivers.HlsReaders.start(
                scope, cfg, self.dir, "%s/hls/live/%s/index.m3u8" % (url, name), n["hls"],
                cfg.quality_seconds, refs.get("hls", "0"))
            self.readers.wait_ready(connect)
        if self.push:
            offset = n["srt"] + n["rtmp"]
            try:
                self.push.wait_ready(n["hls_push"], offset, connect)
            except proc.Timeout:
                prefix = self.path("workers.readiness")
                try:
                    api.worker_metrics(cfg, inst, prefix)
                    print(self.push.diagnose_readiness(name, spec.programs, n["hls_push"], offset, prefix))
                except proc.Failed as error:
                    print("HLS push readiness diagnostics unavailable: %s" % error, file=sys.stderr)
                raise
        if self.rtmp:
            self.rtmp.start_sampler(scope, self.dir)
        if self.srt:
            ready = self.path("queue-sampler.ready")
            if restream:
                for empty in (ready, self.path("queue-sampler.baseline"), self.path("queue-samples.csv")):
                    _write(empty, "")
            else:
                self.queue_sampler = scope.spawn("SRT queue sampler", [
                    sys.executable, cfg.helper("srt_capacity_quality.py"), "sample-queues",
                    inst.http_port, self.path("queue-samples.csv")] + inst.worker_pids() + [
                    "--interval", "1", "--ready-file", ready, "--baseline-ready",
                    self.path("queue-sampler.baseline")],
                    monitor=True, ns=proc.SENDER, log=self.path("queue-sampler.log"))
            self.srt.start_sampler(scope)
            proc.wait_until("SRT queue sampler ready", lambda: os.path.exists(ready), 15,
                            alive=[p for p in (self.queue_sampler,) if p])
        self._stream(name, "before")
        workers = inst.worker_pids()
        sinks = dict((pid, "w" + label) for pid, label in self.rtmp.workers) if self.rtmp else {}
        cpu.socket_snapshot(cfg, self.path("before"), workers)
        cpu.memory_snapshot(self.path("memory.before"), workers + list(sinks))
        self._workers("before")
        receivers_kind = [(p.pid, kind) for p, kind in ((self.srt, "srt-receiver"),
                          (self.readers, "hls-readers"), (self.push, "hls-push-receiver")) if p]
        _write(self.path("processes.tsv"), "pid\tkind\tlabel\n" + "".join(
            "%d\t%s\t%s\n" % row for row in
            [(pid, "worker", inst.slot_name(pid)) for pid in workers]
            + [(pid, kind, kind) for pid, kind in receivers_kind]
            + [(pid, "rtmp-receiver", label) for pid, label in sinks.items()]
            + [(publisher.pid, "publisher", "publisher")]))
        roles = {**inst.roles, **{pid: "worker" for pid in sinks}}
        window = cpu.cpu_window(cfg, self.dir, cfg.quality_seconds,
                                workers + [pid for pid, _ in receivers_kind] + list(sinks) + [publisher.pid],
                                roles, boundary=self._boundary)
        scope.stop_monitors()
        if not publisher.alive():
            raise proc.Failed("a capacity publisher exited during the window (%s)" % publisher.returncode)
        publisher.stop()
        measure = self._stream(name, "after")
        self._workers("after")
        cpu.socket_snapshot(cfg, self.path("after"), workers)
        cpu.memory_snapshot(self.path("memory.after"), workers + list(sinks))
        print("   measurement_s=%.6f requested_window_s=%g stream_counter_measurement_window_s=%.6f "
              "cpu_window_s=%g cpu_sampling_before_ms=%g cpu_sampling_after_ms=%g" % (
                  measure, cfg.quality_seconds, measure, window.seconds, window.before_ms, window.after_ms))
        print("   measurement_provenance=" + PROVENANCE, flush=True)
        if not restream:
            self._stream_report(name, measure)
        passed = self._judge_all(refs, name, source, measure)
        self._reports(window, workers, sinks)
        meta = {"measurement_s": "%.6f" % measure, "measurement_provenance": PROVENANCE,
                "requested_s": "%g" % cfg.quality_seconds}
        if preflight_note:
            meta["preflight_verdict"] = preflight_note
        self.diagnostics("pass" if passed else "quality-failure", **meta)
        return passed

    def _stream_report(self, name, measure):
        before, after = (_read(self.path("stream.%s.%s" % (name, phase))) for phase in ("before", "after"))
        frames = api.json_field(after, "program_frames") - api.json_field(before, "program_frames")
        dispatched = api.json_field(after, "dispatched") - api.json_field(before, "dispatched")
        counts_before = api.json_field(before, "fanout_ms.bucket_counts")
        counts_after = api.json_field(after, "fanout_ms.bucket_counts")
        bounds = api.json_field(after, "fanout_ms.bucket_upper_ms")
        if not len(counts_before) == len(counts_after) == len(bounds) == 16:
            raise proc.Failed("%s returned an invalid fanout histogram" % name)
        bins = [b - a for a, b in zip(counts_before, counts_after)]
        if min(bins) < 0:
            raise proc.Failed("%s fanout bucket counter regressed" % name)
        if sum(bins) != dispatched:
            raise proc.Failed("%s fanout histogram has %d samples, but dispatched delta is %d"
                              % (name, sum(bins), dispatched))
        if dispatched <= 0 or frames <= 0:
            raise proc.Failed("%s produced no fanout samples or program progress" % name)
        lows = [0] + [high + 1 for high in bounds[:-1]]
        text = ",".join("%s:%d" % (low if low == high else "%d-%d" % (low, high), value)
                        for low, high, value in zip(lows, bounds, bins))
        labels = {"application": "live", "name": name}

        def value(blob, series, **extra):
            return api.metric(blob, series, dict(labels, **extra)) or 0

        def delta(series, **extra):
            return value(after, series, **extra) - value(before, series, **extra)
        p = [api.percentile(after, name, q) or 0 for q in (50, 95, 99, 99.9)]
        print("   %s owner=w0 source_frames=%s" % (name, delta("nginx_media_source_frames_in",
                                                              source="enc-" + name)))
        print("      program_frames=%d" % frames)
        print("      progress_fps=%.1f fanout_samples=%d" % (frames / measure, dispatched))
        print("      fanout_p50/p95/p99/p99.9=%s/%s/%s/%sms" % tuple(p))
        print("      fanout.max=%s ms buckets_ms=[%s]" % (api.json_field(after, "fanout_ms.max"), text))
        print("      feed_retained=%su/%sB" % (value(after, "nginx_media_stream_feed_units"),
                                              value(after, "nginx_media_stream_feed_bytes")))
        print("      feed_high_water=%su/%sB" % (value(after, "nginx_media_stream_feed_high_water_units"),
                                                value(after, "nginx_media_stream_feed_high_water_bytes")))
        print("      feed_evictions_delta=%s" % delta("nginx_media_stream_feed_evictions_total"))
        print("      feed_overruns_delta=%s" % delta("nginx_media_stream_feed_overruns_total"))
        print("   program_frame_fairness_jain=1.0000 (1.0 is equal progress)", flush=True)

    def _judge_all(self, refs, name, source, measure):
        cfg, n, path = self.cfg, self.spec.counts, self.path
        tool = lambda script, *args: [sys.executable, cfg.helper(script)] + [str(a) for a in args]  # noqa: E731
        common = ["--min-delivery-ratio", cfg.min_delivery_ratio]
        window = ["--measurement-window", path("cpu.before"), path("cpu.after"),
                  "--clock-domain", receivers.CLOCK]
        passed = True
        if self.srt:
            csv_path = self.srt.stop()
            print("   per-destination rates, MPEG-TS counters, and queue quality:")
            passed &= self._judge(refs, "srt", tool(
                "srt_capacity_quality.py", "report", "--clock-domain", receivers.CLOCK,
                "--baseline", path("srt.before.csv"), "--final", path("srt.after.csv"),
                "--intervals", path("receiver-intervals.csv"), "--queues", path("queue-samples.csv"),
                "--queue-window", path("receiver.measurement.window"), "--receiver-results", csv_path,
                "--metrics-before", path("workers.before"), "--metrics-after", path("workers.after"),
                "--stream-before", path("stream.%s.before" % name),
                "--stream-after", path("stream.%s.after" % name),
                "--destination-report", path("destination-quality.csv"),
                "--interval-report", path("interval-throughput.csv"), "--destinations", n["srt"],
                "--programs", self.spec.programs, "--prepared-source", source,
                "--source-duration-s", 30, "--interval-floor", cfg.interval_floor,
                "--max-low-s", cfg.max_low_seconds, "--max-interval-s", 2,
                "--queue-pressure", cfg.queue_pressure, "--pressure-samples", cfg.pressure_samples,
                "--blocked-samples", cfg.blocked_samples) + common, path("quality-report.txt"))
            print("   destination_quality_csv=%s" % path("destination-quality.csv"))
            print("   interval_throughput_csv=%s" % path("interval-throughput.csv"))
        if self.rtmp:
            passed &= self._judge(refs, "rtmp", tool(
                "rtmp_capacity_quality.py", "--before-prefix", path("rtmp-receiver.before"),
                "--after-prefix", path("rtmp-receiver.after"),
                "--queue-before-prefix", path("workers.before"),
                "--queue-after-prefix", path("workers.after"), "--programs", self.spec.programs,
                "--destinations", n["rtmp"], "--destination-offset", n["srt"],
                "--measurement-s", "%.6f" % measure, "--intervals", path("rtmp-receiver-intervals.csv"),
                "--interval-floor", cfg.interval_floor, "--max-low-s", cfg.max_low_seconds,
                "--prepared-media", path("prepared-media.json"),
                "--report", path("rtmp-quality.csv")) + common, path("rtmp-quality.txt"))
            print("   rtmp_destination_quality_csv=%s" % path("rtmp-quality.csv"))
        if self.readers:
            passed &= self._judge(refs, "hls", tool(
                "hls_capacity_readers.py", "judge", "--report", self.readers.report,
                "--prepared-media", path("prepared-media.json"),
                "--produced-segments", path("hls-production.json"),
                "--receiver-window", path("hls-readers.measurement.window")) + window + common,
                self.readers.log, rate_key="reference_bps")
            if not os.path.exists(self.readers.report) or not os.path.getsize(self.readers.report):
                raise proc.Failed("HLS reader quality report is missing")
            print("   hls_destination_quality_csv=%s" % self.readers.report)
        if self.push:
            passed &= self._judge(refs, "hls_push", tool(
                "hls_push_capacity.py", "report", "--before-prefix", path("workers.before"),
                "--after-prefix", path("workers.after"), "--stream", name,
                "--destination-offset", n["srt"] + n["rtmp"], "--destinations", n["hls_push"],
                "--programs", self.spec.programs, "--sink-before", path("hls-push.before.json"),
                "--sink-after", path("hls-push.after.json"),
                "--prepared-media", path("prepared-media.json"),
                "--produced-segments", path("hls-production.json"),
                "--report", path("hls-push-quality.csv")) + window + common, path("hls-push-quality.txt"))
            print("   hls_push_destination_quality_csv=%s" % path("hls-push-quality.csv"))
        return bool(passed)

    def _reports(self, window, workers, sinks):
        """CPU, memory and socket evidence for the log; diagnostics.json holds the same."""
        cfg, inst, path = self.cfg, self.inst, self.path
        rows = cpu.report_cpu(cfg, window)
        label = lambda pid: sinks.get(pid) or inst.slot_name(pid)  # noqa: E731

        def block(title, aggregate, keep, prefix="worker="):
            print("   " + title)
            totals = collections.defaultdict(float)
            for pid, role, pct in rows:
                if keep(pid, role):
                    who = prefix + (label(pid) if prefix == "worker=" else "pid%d" % pid)
                    print("      %s thread=%s cpu=%.1f%%" % (who, role, pct))
                    totals[who] += pct
            for who, pct in sorted(totals.items()) if len(totals) > 1 else ():
                print("      %s %s=%.1f%%" % (who, aggregate, pct))
            print("      %s=%.1f%%" % (aggregate, sum(totals.values())) if totals else "      no thread rows")
        if self.srt:
            print("   SRT sender-shard counters and queue snapshots:")
            lines = shard_report(path("workers.before"), path("workers.after"))
            _write(path("srt-shards.txt"), "\n".join(lines) + "\n")
            print("\n".join(lines))
            block("sender shard CPU from /proc thread comm srt-egress-*:", "aggregate_egress_shard_cpu",
                  lambda pid, role: role.startswith("srt-egress-"))
            block("libsrt SRT:SndQ CPU:", "aggregate_libsrt_sndq_cpu",
                  lambda pid, role: role == "SRT:SndQ")
        for proc_, title, aggregate in ((self.srt, "shared SRT receiver thread CPU:", "aggregate_receiver_cpu"),
                                        (self.readers, "HLS reader process CPU:", "aggregate_hls_reader_cpu"),
                                        (self.push, "HLS push sink process CPU:", "aggregate_hls_push_sink_cpu")):
            if proc_:
                block(title, aggregate, lambda pid, role, target=proc_.pid: pid == target, prefix="pid=")
        print("   worker process RSS/PSS from smaps_rollup:")
        print("\n".join(cpu.memory_report(path("memory.before"), path("memory.after"))))
        print("   worker visit and scheduling metrics:")
        by_label = cpu.cpu_by_label(rows, label)
        for pid in workers:
            text = {phase: _read(path("workers.%s.%d" % (phase, pid))) for phase in ("before", "after")}

            def delta(series):
                return (api.metric(text["after"], series) or 0) - (api.metric(text["before"], series) or 0)
            print("      %s pid=%d cpu=%.1f%%" % (inst.slot_name(pid), pid, by_label.get(inst.slot_name(pid), 0)))
            print("         visit_max=%sms event_loop_max_delay=%sms" % (
                api.metric(text["after"], "nginx_media_worker_max_service_ms") or 0,
                api.metric(text["after"], "nginx_media_worker_event_loop_max_delay_ms") or 0))
            print("         periodic_visits_delta=%s" % delta("nginx_media_worker_periodic_visits_total"))
            print("         budget_reposts_delta=%s" % delta("nginx_media_worker_budget_reposts_total"))
            print("         late_ticks_delta=%s" % delta("nginx_media_worker_late_ticks_total"))
        print("      aggregate_worker_cpu=%.1f%% of one core"
              % sum(by_label.get(label, 0) for label in {inst.slot_name(pid) for pid in workers}))
        if sinks:
            print("      RTMP sink CPU=%.1f%% of one core (%d separate receiver workers)"
                  % (sum(pct for pid, _, pct in rows if pid in sinks), len(sinks)))
        print("   NGINX socket skmem (ss -m):")
        tables = {}
        for phase in ("before", "after"):
            tables[phase] = {line.split("\t")[0]: line.split("\t") for line in _read(path(phase)).splitlines()}
        for pid in workers:
            before, after = (tables[phase].get(str(pid), [0] * 7) for phase in ("before", "after"))
            print("      %s sockets=%s skmem_bytes=%s delta_bytes=%d" % (
                inst.slot_name(pid), after[5], after[6], int(after[6]) - int(before[6])))
        print("   host /proc/net/sockstat (global, not process-attributed):")
        for proto, field in (("TCP", "mem"), ("UDP", "mem"), ("TCP", "inuse"), ("UDP", "inuse")):
            values = [cpu.sockstat_value(path(phase + ".sockstat"), proto + ":", field) or 0
                      for phase in ("before", "after")]
            print("      %s %s=%d->%d" % (proto, "mem_pages" if field == "mem" else field, *values))
        sys.stdout.flush()

"""Developer phases: the floor, ingest, egress, placement and topology tables.

Each table row runs in its own Scope, so the sender, publishers, receivers and
readers of one row are stopped before the next starts. A row that cannot be
measured prints "unmeasured" (or why it failed) and makes the phase return 1.
"""

import collections
import contextlib
import json
import os
import re
import threading
import time

from . import api, cpu, media, proc, sender
from .proc import HARNESS, SENDER, Scope

INGEST_HEADER = ("workers", "publ.", "delivered", "chunks/s", "MiB/s", "CPU per worker (total)")
UNMEASURED = "unmeasured"
Cpu = collections.namedtuple("Cpu", "seconds rows label")


def _lines(path):
    try:
        with open(path, errors="replace") as handle:
            return handle.read().splitlines()
    except FileNotFoundError:
        return []


def _count(path, needle):
    return sum(needle in line for line in _lines(path))


def per_s(value, seconds, fmt):
    """value/seconds, or "unmeasured" when either side is missing."""
    return UNMEASURED if value is None or not seconds else fmt % (value / seconds)


def mibps(nbytes, seconds):
    return per_s(None if nbytes is None else nbytes / 1048576, seconds, "%.1f")


def owner_names(slot, workers, count, prefix):
    names = api.owner_names(slot, workers, count, prefix)
    if len(names) < count:
        raise proc.Failed("could not name %d streams with owner %d of %d" % (count, slot, workers))
    return names


def ingest_totals(inst):
    """Summed drained counters, or None when no worker has logged them."""
    try:
        return inst.ingest_totals()
    except proc.Failed:
        return None


def _delta(before, after):
    if before is None or after is None:
        return {}
    return {key: after[key] - before[key] for key in after}


def measure(cfg, inst, seconds):
    """The sender's CPU over one window, or None when the window failed - and
    then no rate is printed."""
    try:
        window = cpu.cpu_window(cfg, None, seconds, inst.worker_pids(), inst.roles)
        return Cpu(window.seconds, cpu.report_cpu(cfg, window), inst.slot_name)
    except proc.Failed as err:
        print("   cpu window failed: %s" % err)
        return None


def _dur(window):
    return window.seconds if window else None


def cpu_total(window):
    return UNMEASURED if window is None else cpu.cpu_total(window.rows, window.label)


def detail_rows(label, window):
    if window is None:
        return
    for slot, role, pct in sorted(cpu.detail_cpu(window.rows, window.label)):
        print("   %-4s %-10s %-6s %s%%" % (label, role, slot,
                                          "%d" % pct if role == "other" else "%.3f" % pct))


def row(window, *cells):
    print("%-8s %-6s %-11s %-11s %-9s %s" % (cells + (cpu_total(window),)))


def _step(label, fn, *args):
    """One table row; a failure is printed and makes the phase fail, the
    remaining rows still run."""
    try:
        return fn(*args)
    except proc.Failed as err:
        print("   %s failed: %s" % (label, err))
        return 1


def _source(cfg, name, bitrate):
    path = cfg.path("media", name)
    media.make_source(cfg, path, bitrate)
    return path


def _program_frames(cfg, names, timeout):
    """Sum of the programs' frame counters; None when one cannot be read."""
    values = [api.field(cfg, name, "program_frames", timeout) for name in names]
    return None if None in values else sum(values)


# --- floor and ingest ----------------------------------------------------------

def phase_floor(cfg):
    print("\n== floor: what a worker burns with no publisher and no destination\n")
    print("%-8s %-10s %s" % ("workers", "idle CPU", "per-thread"))
    status = 0
    for workers in cfg.lo_workers:
        status |= _step("floor %d" % workers, _floor_row, cfg, workers)
    print("   (no media has been published at this point in the run)")
    return status


def _floor_row(cfg, workers):
    with Scope() as scope:
        inst = sender.start(scope, cfg, workers, hls=False, rtmp=False)
        window = measure(cfg, inst, cfg.floor_window)
    send = "" if window is None else "".join(
        "%s=%d%% " % (inst.slot_name(pid), pct)
        for pid, role, pct in sorted(window.rows, key=lambda r: str(r[0])) if role == "srt-send")
    print("%-8s %-10s %s" % (workers, cpu_total(window), send))
    return int(window is None)


def _ingest(cfg, workers, streams, source, rate=None, placement=False):
    """Publishers [(endpoint index, name)] on a fresh sender, settled for one
    window; then drained counters and CPU around a second window."""
    with Scope() as scope:
        inst = sender.start(scope, cfg, workers, hls=False, rtmp=False)
        if placement:
            print("   endpoints:%s" % inst.placement)
        for endpoint, name in streams:
            media.publish(scope, cfg, inst.srt_ports[endpoint], name, source, rate)
        time.sleep(cfg.window)
        delivered = sum(api.field(cfg, name, "program_frames", 1.2) is not None
                        for _, name in streams)
        before = ingest_totals(inst)
        window = measure(cfg, inst, cfg.window)
        delta = _delta(before, ingest_totals(inst))
        log = inst.error_log
        late = _count(log, "RCV-DROPPED")
        noroom = _count(log, "No room to store incoming packet")
    return delivered, delta, window, late, noroom


def phase_ingest_high(cfg):
    print("\n== ingest: %d publishers on one endpoint, one worker, with the" % cfg.hi_pubs)
    print("   offered rate raised (%s per publisher, -muxrate)" % " ".join(cfg.hi_rates))
    print("   %d is the per-worker session ceiling, so the only thing that" % cfg.hi_pubs)
    print("   rises here is the packet rate one port has to carry\n")
    print("%-8s %-10s %-11s %-9s %-8s %s" % ("rate", "chunks/s", "MiB/s", "dropped",
                                            "seqno-late", "CPU per worker (total)"))
    source = _source(cfg, "hi.ts", cfg.lo_bitrate)
    status = 0
    for rate in cfg.hi_rates:
        status |= _step("rate %s" % rate, _high_row, cfg, rate, source)
    return status


def _high_row(cfg, rate, source):
    streams = [(0, "hi%d" % i) for i in range(1, cfg.hi_pubs + 1)]
    delivered, delta, window, late, noroom = _ingest(cfg, 1, streams, source, rate)
    print("%-8s %-10s %-11s %-9s %-8s %s" % (
        rate, per_s(delta.get("chunks"), _dur(window), "%d"),
        mibps(delta.get("bytes"), _dur(window)),
        delta.get("dropped_chunks", UNMEASURED), late + noroom, cpu_total(window)))
    print("   %-4s sessions %s  fanout-delay drops %s  receive-buffer drops %s"
          % (rate, "%d/%d" % (delivered, cfg.hi_pubs), late, noroom))
    detail_rows(rate, window)
    return int(window is None or not delta)


def _ingest_table(cfg, title, rows):
    """The shared table of the publisher/endpoint phases: rows are
    (workers, [(endpoint index, name)], placement)."""
    print("\n" + title + "\n")
    print("%-8s %-6s %-11s %-11s %-9s %s" % INGEST_HEADER)
    source = _source(cfg, "lo.ts", cfg.lo_bitrate)
    status = 0
    for workers, streams, placement in rows:
        status |= _step("%d workers, %d publishers" % (workers, len(streams)),
                        _ingest_row, cfg, workers, streams, source, placement)
    return status


def _ingest_row(cfg, workers, streams, source, placement):
    n = len(streams)
    delivered, delta, window, _, _ = _ingest(cfg, workers, streams, source, placement=placement)
    row(window, workers, n, "%d/%d" % (delivered, n),
        per_s(delta.get("chunks"), _dur(window), "%d"), mibps(delta.get("bytes"), _dur(window)))
    detail_rows(n, window)
    if delta.get("dropped_chunks", 0) > 0:
        print("   dropped chunks in the window: %d" % delta["dropped_chunks"])
    return int(window is None or not delta)


def phase_ingest_publishers(cfg):
    return _ingest_table(
        cfg, "== ingest: publishers grown on one endpoint at ~%s each" % cfg.lo_bitrate,
        [(1, [(0, "pu%d" % i) for i in range(1, k + 1)], False) for k in cfg.pub_steps])


def phase_ingest_endpoints(cfg):
    title = ("== ingest: the same publishers per endpoint, across worker counts\n"
             "   ~%s publishers, %d per endpoint, spread\n"
             "   round-robin over the endpoints the instance binds" % (cfg.lo_bitrate, cfg.lo_per_port))
    return _ingest_table(cfg, title, [
        (w, [((i - 1) % w, "lo%d" % i) for i in range(1, w * cfg.lo_per_port + 1)], True)
        for w in cfg.lo_workers])


def phase_ingest_one_port(cfg):
    title = ("== ingest: %d publishers on one endpoint, from a single\n"
             "   worker and from an instance with four workers bound (three idle)" % cfg.lo_per_port)
    return _ingest_table(cfg, title, [
        (w, [(0, "op%d" % i) for i in range(1, cfg.lo_per_port + 1)], False) for w in (1, 4)])


# --- egress --------------------------------------------------------------------

def segments_closed(cfg):
    return sum(line.startswith("#EXTINF") for line in _lines(cfg.path("hls/live/eg/index.m3u8")))


class HlsStorm(threading.Thread):
    """Readers pulling every listed segment of the program with curl --parallel,
    from the sender's namespace (the HLS server listens on its loopback). Each
    pull is its own session, stopped as a group when the window ends."""

    def __init__(self, cfg, inst):
        super().__init__(daemon=True)
        self.cfg, self.inst = cfg, inst
        self.lock = threading.Lock()
        self.done = threading.Event()
        self.pull = None

    def run(self):
        playlist, urls = self.cfg.path("hls/live/eg/index.m3u8"), self.cfg.path("urls.txt")
        while not self.done.is_set():
            names = [line for line in _lines(playlist)
                     if line.endswith(".ts") and not line.startswith("#")]
            if not names:
                self.done.wait(0.2)
                continue
            with open(urls, "w") as handle:
                for name in names:
                    handle.write('url = "http://127.0.0.1:%d/hls/live/eg/%s"\noutput = "/dev/null"\n'
                                 % (self.inst.http_port, name))
            with self.lock:
                if self.done.is_set():
                    break
                # 300 s bounds one pull without ever truncating a real one
                self.pull = proc.Proc("hls storm", [
                    "curl", "-fsS", "--connect-timeout", "3", "--max-time", "300", "--parallel",
                    "--parallel-max", self.cfg.hls_readers, "--config", urls], ns=SENDER)
            self.pull.popen.wait()
            self.done.wait(0.02)

    def stop(self):
        with self.lock:
            self.done.set()
            if self.pull:
                self.pull.stop(grace=2)
        if self.is_alive():
            self.join(10)


def phase_egress_hls(cfg):
    print("\n== egress: %d concurrent HLS readers against one program" % cfg.hls_readers)
    print("   the segmenter runs on the owner; each reader is served by")
    print("   whichever worker the kernel hashed it to\n")
    print("%-8s %-12s %-11s %-9s %s" % ("workers", "requests/s", "MiB/s", "segments",
                                        "CPU per worker (total)"))
    source = _source(cfg, "lo.ts", cfg.lo_bitrate)
    status = 0
    for workers in cfg.egress_workers:
        status |= _step("hls %d workers" % workers, _hls_row, cfg, workers, source)
    return status


def _hls_row(cfg, workers, source):
    access = cfg.path("logs", "access.log")
    with Scope() as scope:
        inst = sender.start(scope, cfg, workers, hls=True, rtmp=False)
        pub = media.publish(scope, cfg, inst.srt_ports[0], "eg", source)
        proc.wait_until("three closed HLS segments", lambda: segments_closed(cfg) >= 3,
                        40, interval=0.1, alive=[pub])
        time.sleep(4)
        storm = HlsStorm(cfg, inst)
        scope.callback(storm.stop)
        storm.start()
        window = measure(cfg, inst, cfg.hls_seconds)
        storm.stop()
        time.sleep(0.5)  # aborted pulls reach the access log
        served = [line.split() for line in _lines(access)]
    nbytes = sum(int(fields[1]) for fields in served if len(fields) > 1 and fields[1].isdigit())
    print("%-8s %-12s %-11s %-9s %s" % (
        workers, per_s(len(served), _dur(window), "%d"), mibps(nbytes, _dur(window)),
        segments_closed(cfg), cpu_total(window)))
    print("   requests per worker (the access log's $pid):")
    per_worker = {}
    for fields in served:
        slot = inst.slot_name(int(fields[0])) if fields and fields[0].isdigit() else "?"
        per_worker[slot] = per_worker.get(slot, 0) + 1
    for slot in sorted(per_worker):
        print("     %-6s %s requests" % (slot, per_worker[slot]))
    detail_rows("hls", window)
    return int(window is None)


def _udp_listening(ports):
    out = proc.run(["ss", "-Hlun"], ns=HARNESS, timeout=10).stdout
    bound = {line.split()[3].rpartition(":")[2] for line in out.splitlines() if len(line.split()) > 3}
    return all(str(port) in bound for port in ports)


def phase_egress_live(cfg):
    print("\n== egress: %d live SRT destinations on one program" % cfg.srt_dests)
    print("   each is pushed to an ffmpeg receiver of its own, prepared and fed")
    print("   by the owner worker in process\n")
    print("%-8s %-13s %-11s %s" % ("workers", "started", "frames/s", "CPU per worker (total)"))
    source = _source(cfg, "lv.ts", cfg.live_bitrate)
    status = 0
    for workers in cfg.egress_workers:
        status |= _step("live %d workers" % workers, _live_row, cfg, workers, source)
    return status


def _live_row(cfg, workers, source):
    started = re.compile(r"media: srt destination sink[0-9]+ started")
    problems = []
    with Scope() as scope:
        inst = sender.start(scope, cfg, workers, hls=False, rtmp=True)
        pub = media.publish(scope, cfg, inst.srt_ports[0], "lv", source, cfg.live_rate or None)
        proc.wait_until("lv program frames", lambda: api.field(cfg, "lv", "program_frames", 1.6)
                        is not None, 30, alive=[pub])
        owner = api.owner_of(cfg, "lv")
        # the receivers listen in the harness namespace, on the address the sender reaches
        ports = [cfg.base + 20 + d for d in range(1, cfg.srt_dests + 1)]
        sinks = [scope.spawn("srt sink%d" % d, [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-i",
            "srt://%s:%d?mode=listener" % (cfg.receiver_addr, port), "-c", "copy", "-f", "null", "-"],
            ns=HARNESS, log=cfg.path("sink-%d.log" % d)) for d, port in enumerate(ports, 1)]
        proc.wait_until("SRT sinks listening", lambda: _udp_listening(ports), 10, alive=sinks)
        posted = 0
        for d, port in enumerate(ports, 1):
            try:
                api.post_owner(cfg, "/streams/live/lv/destinations", json.dumps({
                    "id": "sink%d" % d, "type": "srt", "host": cfg.receiver_addr, "port": port,
                    "streamid": "#!::r=live/lv,m=publish,s=sink%d" % d}))
                posted += 1
            except proc.Failed as err:
                problems.append("sink%d not posted: %s" % (d, err))

        def dests():
            return sum(bool(started.search(line)) for line in _lines(inst.error_log))
        with contextlib.suppress(proc.Timeout):
            proc.wait_until("destinations started", lambda: dests() >= posted, 10, alive=[pub])
        count = dests()
        f0 = api.field(cfg, "lv", "program_frames", 1.6)
        window = measure(cfg, inst, cfg.live_seconds)
        f1 = api.field(cfg, "lv", "program_frames", 1.6)
        problems += ["%s exited (%s)" % (s.name, s.returncode) for s in sinks if not s.alive()]
    frames = None if f0 is None or f1 is None else f1 - f0
    print("%-8s %-13s %-11s %s" % (workers, "%d/%d" % (count, cfg.srt_dests),
                                   per_s(frames, _dur(window), "%.1f"), cpu_total(window)))
    print("   owner reported: %s; destinations started: %d" % ("?" if owner is None else owner, count))
    detail_rows("live", window)
    if count < cfg.srt_dests:
        problems.append("%d of %d destinations started" % (count, cfg.srt_dests))
    for problem in problems:
        print("   live %d workers failed: %s" % (workers, problem))
    return int(bool(problems) or frames is None or window is None)


# --- placed versus routed --------------------------------------------------------

def phase_misplace(cfg):
    w, n = cfg.mp_workers, cfg.mp_pubs
    print("\n== placement: %d publishers on their owner's endpoint, and the same" % n)
    print("   %d on another worker's endpoint, where every one is routed\n" % n)
    if w < 2:
        print("   misplace needs at least two workers")
        return 1
    source = _source(cfg, "mp.ts", cfg.mp_bitrate)
    owners, routed_logged, status = {}, {}, 0
    for case, endpoint, names in (("placed", 0, owner_names(0, w, n, "p")),
                                  ("routed", 1, owner_names(0, w, n, "r"))):
        result = _step(case, _misplace_case, cfg, case, endpoint, names, source)
        if isinstance(result, tuple):
            owners[case], routed_logged[case], ports = result
        else:
            status = 1
    if len(owners) < 2:
        return 1

    def shown(value):
        return "?" if value is None else value
    print("\n   owner from the graph: placed=%s routed=%s" % (shown(owners["placed"]), shown(owners["routed"])))
    print("   routed sessions logged: %d of %d in the routed case, %d of %d in the placed case"
          % (routed_logged["routed"], n, routed_logged["placed"], n))
    print("   placed: endpoint %d is worker 0's, so every publisher" % ports[0])
    print("           is accepted by the worker that owns its program")
    print("   routed: endpoint %d is worker 1's, so every frame" % ports[1])
    print("           crosses the SOCK_SEQPACKET transport to worker 0")
    return status


def _misplace_case(cfg, case, endpoint, names, source):
    with Scope() as scope:
        # HLS gives the program a consumer, so the fanout delay histogram records
        # dispatches; the segmenter runs on the owner in both cases and cancels out
        inst = sender.start(scope, cfg, cfg.mp_workers, hls=True, rtmp=False)
        print("   endpoints:%s" % inst.placement)
        for name in names:
            media.publish(scope, cfg, inst.srt_ports[endpoint], name, source, cfg.mp_rate)
        time.sleep(6)
        # rates divide by the interval the deltas cover, not by the window
        t0 = time.monotonic()
        before = ingest_totals(inst)
        f0 = _program_frames(cfg, names, 1.6)
        window = measure(cfg, inst, cfg.window)
        f1 = _program_frames(cfg, names, 1.6)
        delta = _delta(before, ingest_totals(inst))
        span = time.monotonic() - t0
        metrics = [api.owner_metrics(cfg, name) for name in names]
        owner = api.owner_of(cfg, names[0])
        logged = _count(inst.error_log, "srt publisher routed to the owner stream=live/" + case[0])
    units = [None if text is None else api.metric(text, "nginx_media_stream_feed_units", _stream(name))
             for text, name in zip(metrics, names)]
    p50, p99 = ((None, None) if metrics[0] is None else
                (api.percentile(metrics[0], names[0], p) for p in ("50", "99")))
    frames = None if f0 is None or f1 is None else f1 - f0
    print("   %-7s programs %s  frames/s %s  owner backlog %s units" % (
        case, len(names), per_s(frames, span, "%.1f"),
        UNMEASURED if None in units else _num(sum(units))))
    print("   %-7s drained %s chunks/s  %s MiB/s  dropped %s  fanout p50 %sms p99 %sms" % (
        case, per_s(delta.get("chunks"), span, "%d"), mibps(delta.get("bytes"), span),
        delta.get("dropped_chunks", UNMEASURED), _num(p50, "?"), _num(p99, "?")))
    detail_rows(case, window)
    return owner, logged, inst.srt_ports


def _stream(name, source=False):
    labels = {"application": "live", "name": name}
    if source:
        labels["source"] = "enc-" + name
    return labels


def _num(value, missing=UNMEASURED):
    if value is None:
        return missing
    return "%d" % value if float(value).is_integer() else "%s" % value


# --- deterministic topology matrix -------------------------------------------------

ROUTE = ("identity_mismatches", "slot_overflows", "no_slot", "no_payload",
         "reassembly_errors", "publish_errors")


def phase_topology(cfg):
    """One stream named for each owner slot, published locally, routed,
    alternating and as one-program controls; each row is the owner's own
    metrics for the program."""
    w, window = cfg.topology_workers, cfg.topology_window
    if w < 2:
        print("topology needs at least two workers")
        return 1
    names = [owner_names(i, w, 1, "topology-w")[0] for i in range(w)]
    print("\n== deterministic worker topology matrix")
    print("   workers: %d, source: pre-encoded mpegts, window: %ss" % (w, _num(window)))
    print("   names are selected by the 64-bit FNV-1a owner hash")
    for i, name in enumerate(names):
        print("   owner w%d: %s" % (i, name))
    source = _source(cfg, "lo.ts", cfg.lo_bitrate)
    cases = {
        "all-local": [(name, i) for i, name in enumerate(names)],
        "all-routed": [(name, (i + 1) % w) for i, name in enumerate(names)],
        "alternating": [(name, i if i % 2 == 0 else (i + 1) % w) for i, name in enumerate(names)],
        "one-local": [(names[0], 0)],
        "one-routed": [(names[0], 1)],
    }
    status = 0
    for case, streams in cases.items():
        print("\n== topology case: %s" % case)
        status |= _step(case, _topology_case, cfg, case, streams, source)
    return status


def _topology_case(cfg, case, streams, source):
    status = 0
    with Scope() as scope:
        inst = sender.start(scope, cfg, cfg.topology_workers, hls=True, rtmp=False)
        for name, endpoint in streams:
            media.publish(scope, cfg, inst.srt_ports[endpoint], name, source)
        for name, _ in streams:
            if api.field(cfg, name, "program_frames", 16) is None:
                print("   %s reported no program frames within 16s" % name)
                status = 1
        time.sleep(cfg.topology_window)
        print("   %-16s %s" % ("case", case))
        for name, endpoint in streams:
            metrics = api.owner_metrics(cfg, name)
            owner = api.owner_of(cfg, name)
            landing = inst.endpoint_worker(inst.srt_ports[endpoint]) or "unknown"
            status |= _topology_row(name, owner, endpoint, landing, metrics)
    return status


def _topology_row(name, owner, endpoint, landing, metrics):
    head = "   %s owner=%s landing=%s path=%s" % (name, "?" if owner is None else owner, landing,
                                                 "local" if owner == endpoint else "routed")
    if metrics is None:
        print(head, "metrics=unmeasured")
        return 1

    def label(metric):
        return api.metric(metrics, "nginx_media_stream_" + metric, _stream(name))

    def src(metric):
        return api.metric(metrics, "nginx_media_source_" + metric, _stream(name, source=True))
    route = [api.metric(metrics, "nginx_media_runtime_routed_%s_total" % metric) for metric in ROUTE]
    values = [src("frames_in"), label("program_frames"), label("feed_units"), label("feed_bytes"),
              label("feed_high_water_units"), label("feed_high_water_bytes"),
              label("feed_evictions_total"), label("feed_overruns_total"),
              label("feed_generation_mismatches_total"), label("feed_publish_errors_total"),
              src("preroll_high_water_units"), src("preroll_high_water_bytes"),
              src("preroll_overflows_total"), src("preroll_unit_overflows_total"),
              src("preroll_byte_overflows_total"), None if None in route else sum(route)] + route
    print(head, "input_frames=%s program_frames=%s "
          "feed_retained=%su/%sB feed_high_water=%su/%sB feed_evictions=%s feed_overruns=%s "
          "feed_generation_mismatches=%s feed_publish_errors=%s preroll_high_water=%su/%sB "
          "preroll_overflows=%s preroll_unit_overflows=%s preroll_byte_overflows=%s "
          "route_failures=%s route_identity=%s route_slots=%s route_no_slot=%s "
          "route_no_payload=%s route_reassembly=%s route_publish=%s"
          % tuple(_num(value) for value in values))
    return int(None in values or owner is None)


def phase_ingest(cfg):
    return (phase_ingest_high(cfg) | phase_ingest_publishers(cfg)
            | phase_ingest_endpoints(cfg) | phase_ingest_one_port(cfg))


def phase_egress(cfg):
    return phase_egress_hls(cfg) | phase_egress_live(cfg)


PHASES = {"floor": phase_floor, "ingest": phase_ingest, "egress": phase_egress,
          "misplace": phase_misplace, "topology": phase_topology}


def conditions(cfg):
    """The run footer: what every table above was measured under."""
    print("\n== conditions")
    print("   source:     pre-encoded 720p25 mpegts, publishers use -c copy")
    print("   endpoints:  one media_srt_listen per worker, worker i binds entry i")
    print("   api:        listen ... reuseport; mutations retried until the owner")
    print("               answers, metrics read on the owner's own connection")
    print("   throughput: chunks/s and bytes/s are the workers' own drained totals")
    print("   cpu:        per thread, percent of one core, over the measured window")
    if "capacity-quality-ladder" in cfg.phases:
        print("   receivers:  separate SRT/RTMP sinks, HLS readers and HLS PUT sink")
        print("   receiver CPU: sampled for external SRT, RTMP and HLS sink workers")
    print("   labels:     SRT:* from comm; this module's threads from a stack backtrace (%s)"
          % ("attached and labelled" if sender.labelled() else "NOT AVAILABLE: comm names only"))
    print("   host:       single host, no netem, %d cpus" % os.cpu_count())
    print("== ingest/egress fanout done")

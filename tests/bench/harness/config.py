"""Every knob the harness reads, in one place. Names are the environment
variables Makefile, scripts/bench-ci.sh and capacity_sweep.py set."""

import dataclasses
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def _root():
    """Nearest ancestor with a Makefile and src/: the sweep runs a frozen copy
    of tests/bench one directory deeper, so a fixed depth is wrong."""
    path = HERE
    while path != os.path.dirname(path):
        if os.path.isfile(os.path.join(path, "Makefile")) and os.path.isdir(os.path.join(path, "src")):
            return path
        path = os.path.dirname(path)
    raise SystemExit("no repository root above %s" % HERE)


ROOT = _root()


def _env(name, default=""):
    return os.environ.get(name, default)


def _words(name, default):
    return _env(name, default).split()


def receiver_addresses(base, count, destinations):
    """The destination addresses one rung spreads over.

    A TCP connection is one (source address, source port, destination address,
    destination port) tuple, and the source ports come from one budget per
    destination address, so a rung whose destinations all share one address
    also shares one budget.  Loopback carries 127.0.0.0/8 whole, and a veth
    carries whatever its namespace is given, so a second address costs nothing
    to use.  Returns the addresses for `destinations` destinations, at most
    `count` of them, in the order their destinations are numbered.
    """
    if not 1 <= count <= 254:
        raise ValueError("receiver address count %r is outside 1-254" % count)
    parts = base.split(".")
    if len(parts) != 4:
        raise ValueError("receiver address %r is not an IPv4 address" % base)
    last = int(parts[3])
    if last + count - 1 > 254:
        raise ValueError("receiver addresses from %s would leave the subnet" % base)
    wanted = max(1, -(-destinations // RECEIVER_ADDRESS_BLOCK))
    return [".".join(parts[:3] + [str(last + index)])
            for index in range(min(count, wanted))]


RECEIVER_ADDRESS_BLOCK = 256


@dataclasses.dataclass(frozen=True)
class Config:
    root: str
    bench_dir: str
    scripts_dir: str
    run: str
    nginx: str
    rtmp_sink_nginx: str
    engine: str
    restream_bin: str
    phases: list
    base: int
    # placement
    workers: int
    nginx_cpus: str
    receiver_cpus: str
    publisher_cpus: str
    sender_addr: str
    receiver_addr: str
    receiver_addr_count: int
    media_addr: str
    # quality ladder
    quality_rate: str
    quality_seconds: float
    quality_steps: list
    quality_mixes: str
    quality_programs: int
    stop_after_failures: int
    min_delivery_ratio: str
    interval_floor: str
    max_low_seconds: str
    queue_pressure: str
    pressure_samples: str
    blocked_samples: str
    srt_workers: str
    hls_push_workers: str
    srt_hls: str
    sink_mode: str
    rtmp_receiver_peers: int
    srt_receiver_peers: int
    owner_retry_budget: float
    # preflight
    preflight: str
    preflight_min_destinations: int
    preflight_seconds: str
    preflight_threads: str
    preflight_sender_cpu_per_gbps: str
    # developer phases
    window: float
    floor_window: float
    hi_pubs: int
    hi_rates: list
    pub_steps: list
    lo_bitrate: str
    lo_per_port: int
    lo_workers: list
    egress_workers: list
    hls_readers: int
    hls_seconds: float
    srt_dests: int
    live_seconds: float
    live_bitrate: str
    live_rate: str
    mp_workers: int
    mp_pubs: int
    mp_bitrate: str
    mp_rate: str
    topology_workers: int
    topology_window: float
    restream_http_port: int
    restream_rtmp_port: int
    restream_srt_port: int

    @property
    def api_port(self):
        return self.base

    def path(self, *parts):
        return os.path.join(self.run, *parts)

    def helper(self, name):
        return os.path.join(self.bench_dir, name)


def load():
    sender_addr = _env("CAPACITY_SENDER_ADDR", "127.0.0.1")
    receiver_addr = _env("CAPACITY_RECEIVER_ADDR", "127.0.0.1")
    receiver_addr_count = int(_env("CAPACITY_RECEIVER_ADDR_COUNT", "16"))
    return Config(
        root=ROOT,
        bench_dir=_env("CAPACITY_BENCH_DIR", os.path.join(ROOT, "tests/bench")),
        scripts_dir=_env("CAPACITY_SCRIPTS_DIR", os.path.join(ROOT, "scripts")),
        run=os.path.abspath(_env("CAPACITY_RUN", os.path.join(ROOT, ".build/ingest-egress-fanout"))),
        nginx=_env("CAPACITY_NGINX", os.path.join(ROOT, ".build/nginx-install/sbin/nginx")),
        rtmp_sink_nginx=_env("CAPACITY_RTMP_SINK_NGINX",
                             os.path.join(ROOT, ".build/nginx-haivision/install/sbin/nginx")),
        engine=_env("CAPACITY_ENGINE", "nginx"),
        restream_bin=_env("RESTREAM_BIN", os.path.expanduser("~/Projects/restream/target/release/restream")),
        phases=_words("PHASES", "floor ingest egress misplace"),
        base=int(_env("CAPACITY_BASE", str(10240 + (os.getpid() % 15) * 512))),
        workers=int(_env("CAPACITY_WORKERS", "4")),
        nginx_cpus=_env("CAPACITY_NGINX_CPUS"),
        receiver_cpus=_env("CAPACITY_RECEIVER_CPUS"),
        publisher_cpus=_env("CAPACITY_PUBLISHER_CPUS"),
        sender_addr=sender_addr,
        receiver_addr=receiver_addr,
        receiver_addr_count=receiver_addr_count,
        # A namespaced sender binds its own namespace address for media, the
        # only one its peers can reach; a host sender binds loopback.
        media_addr=_env("CAPACITY_MEDIA_ADDR",
                        sender_addr if _env("CAPACITY_SENDER_EXEC") else "127.0.0.1"),
        quality_rate=_env("CAPACITY_QUALITY_RATE", "8M"),
        quality_seconds=float(_env("CAPACITY_QUALITY_SECONDS", "120")),
        quality_steps=[int(s) for s in _words("CAPACITY_QUALITY_STEPS",
                                              "1 2 4 8 16 32 64 128 256 512 1024")],
        quality_mixes=_env("CAPACITY_QUALITY_MIXES", "all"),
        quality_programs=int(_env("CAPACITY_QUALITY_PROGRAMS", "1")),
        stop_after_failures=int(_env("CAPACITY_QUALITY_STOP_AFTER_FAILURES", "0")),
        min_delivery_ratio=_env("CAPACITY_QUALITY_MIN_DELIVERY_RATIO", "0.95"),
        interval_floor=_env("CAPACITY_QUALITY_INTERVAL_FLOOR", "0.80"),
        max_low_seconds=_env("CAPACITY_QUALITY_MAX_LOW_SECONDS", "2"),
        queue_pressure=_env("CAPACITY_QUALITY_QUEUE_PRESSURE", "0.90"),
        pressure_samples=_env("CAPACITY_QUALITY_PRESSURE_SAMPLES", "3"),
        blocked_samples=_env("CAPACITY_QUALITY_BLOCKED_SAMPLES", "3"),
        srt_workers=_env("CAPACITY_FIXED_SRT_WORKERS", "adaptive"),
        hls_push_workers=_env("CAPACITY_FIXED_HLS_PUSH_WORKERS", "adaptive"),
        srt_hls=_env("CAPACITY_SRT_HLS", "yes"),
        sink_mode=_env("CAPACITY_SINK_MODE", "auto"),
        rtmp_receiver_peers=int(_env("CAPACITY_RTMP_RECEIVER_PEERS", "48")),
        srt_receiver_peers=int(_env("CAPACITY_SRT_RECEIVER_PEERS", "16")),
        owner_retry_budget=float(_env("CAPACITY_OWNER_RETRY_BUDGET", "12")),
        preflight=_env("CAPACITY_PREFLIGHT", "auto"),
        preflight_min_destinations=int(_env("CAPACITY_PREFLIGHT_MIN_DESTINATIONS", "128")),
        preflight_seconds=_env("CAPACITY_PREFLIGHT_SECONDS", "5"),
        preflight_threads=_env("CAPACITY_PREFLIGHT_THREADS", "4"),
        preflight_sender_cpu_per_gbps=_env("CAPACITY_PREFLIGHT_SENDER_CPU_PER_GBPS"),
        window=float(_env("WINDOW", "6")),
        floor_window=float(_env("FLOOR_WINDOW", "4")),
        hi_pubs=int(_env("HI_PUBS", "16")),
        hi_rates=_words("HI_RATES", "20M 40M 80M 120M"),
        pub_steps=[int(s) for s in _words("PUB_STEPS", "4 8 12 16")],
        lo_bitrate=_env("LO_BITRATE", "6M"),
        lo_per_port=int(_env("LO_PER_PORT", "12")),
        lo_workers=[int(s) for s in _words("LO_WORKERS", "1 2 4")],
        egress_workers=[int(s) for s in _words("EGRESS_WORKERS", "1 2 4")],
        hls_readers=int(_env("HLS_READERS", "48")),
        hls_seconds=float(_env("HLS_SECONDS", "12")),
        srt_dests=int(_env("SRT_DESTS", "8")),
        live_seconds=float(_env("LIVE_SECONDS", "12")),
        live_bitrate=_env("LIVE_BITRATE", "20M"),
        live_rate=_env("LIVE_RATE"),
        mp_workers=int(_env("MP_WORKERS", "2")),
        mp_pubs=int(_env("MP_PUBS", "12")),
        mp_bitrate=_env("MP_BITRATE", "20M"),
        mp_rate=_env("MP_RATE", "20M"),
        topology_workers=int(_env("TOPOLOGY_WORKERS", "4")),
        topology_window=float(_env("TOPOLOGY_WINDOW", "8")),
        restream_http_port=int(_env("RESTREAM_HTTP_PORT", "18080")),
        restream_rtmp_port=int(_env("RESTREAM_RTMP_PORT", "11935")),
        restream_srt_port=int(_env("RESTREAM_SRT_PORT", "16000")),
    )

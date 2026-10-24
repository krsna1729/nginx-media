"""The capacity-quality ladder: one ladder per mix in capacity-mixes.conf,
one Scope per rung, and the capacity matrix over every rung's bundle."""

import glob
import json
import os
import re
import shutil
import sys
import traceback

from . import media, proc, rung

PROTOCOL_NAME = {"srt": "srt", "rtmp": "rtmp", "hls": "hls", "hls_push": "hls-push"}
PROTOCOL_LABEL = {"srt": "SRT", "rtmp": "RTMP", "hls": "HLS", "hls_push": "HLS push"}


class ConfigError(Exception):
    """The ladder cannot run as configured; nothing has been measured."""


def read_mixes(cfg):
    """[(name, protocol, srt share, HLS push share)] in capacity-mixes.conf order."""
    mixes = []
    with open(cfg.helper("capacity-mixes.conf"), encoding="utf-8") as source:
        for line in source:
            if line.strip() and not line.lstrip().startswith("#"):
                name, protocol, srt, push = line.strip().split(":")[:4]
                mixes.append((name, protocol, int(srt), int(push)))
    if not mixes:
        raise ConfigError("no capacity quality mixes in capacity-mixes.conf")
    return mixes


def select_mixes(cfg):
    """The mixes CAPACITY_QUALITY_MIXES names (`all` = the file's list)."""
    mixes = read_mixes(cfg)
    if cfg.quality_mixes == "all":
        return mixes
    requested = cfg.quality_mixes.split()
    if not requested:
        raise ConfigError("CAPACITY_QUALITY_MIXES must not be empty")
    by_name = {mix[0]: mix for mix in mixes}
    for name in requested:
        if name not in by_name or re.search(r"[^A-Za-z0-9-]", name):
            raise ConfigError("unknown capacity quality mix: %s" % name)
    return [by_name[name] for name in requested]


def minimum_rung(protocol, srt_share, hls_push_share):
    """The smallest rung at which every protocol of the mix has a destination;
    a three-way mix's first RTMP rung carries exactly one (its calibration)."""
    for n in range(1, 2049):
        counts = rung.split(protocol, n, srt_share, hls_push_share)
        if protocol == "srt" or n - counts["srt"] < 1:
            continue
        if protocol != "rtmp-hls-push" or counts["rtmp"] >= 1:
            return n
    return 1


def recipe_line(cfg):
    return ("quality_recipe seconds=%g rate=%s steps=%s stop_after_failures=%d "
            "min_delivery_ratio=%s interval_floor=%s max_low_seconds=%s srt_workers=%s "
            "hls_push_workers=%s srt_hls=%s" % (
                cfg.quality_seconds, cfg.quality_rate,
                ",".join(map(str, cfg.quality_steps)), cfg.stop_after_failures,
                cfg.min_delivery_ratio, cfg.interval_floor, cfg.max_low_seconds,
                cfg.srt_workers, cfg.hls_push_workers, cfg.srt_hls))


def validate(cfg, mixes):
    steps = cfg.quality_steps
    if not steps:
        raise ConfigError("CAPACITY_QUALITY_STEPS must not be empty")
    if any(n < 1 or n > 2048 for n in steps) or steps != sorted(set(steps)):
        raise ConfigError("quality ladder must be strictly increasing within 1..2048")
    for name, protocol, srt, push in mixes:
        minimum = minimum_rung(protocol, srt, push)
        if steps[0] < minimum:
            raise ConfigError("quality ladder must begin at or above the smallest rung "
                              "%s can form: %d" % (name, minimum))
    if cfg.quality_seconds != int(cfg.quality_seconds) or not 1 <= cfg.quality_seconds <= 3600:
        raise ConfigError("capacity window must be a whole number of seconds from 1 to 3600")
    if cfg.quality_programs != 1:
        raise ConfigError("quality cases require one un-stalled program")
    if cfg.preflight not in ("auto", "yes", "no"):
        raise ConfigError("CAPACITY_PREFLIGHT must be auto, yes or no")
    if cfg.sink_mode not in ("auto", "correctness", "fast"):
        raise ConfigError("CAPACITY_SINK_MODE must be auto, correctness or fast")
    if cfg.workers < 1 or cfg.workers > 16:
        raise ConfigError("CAPACITY_WORKERS must be 1..16")
    if shutil.which("ss") is None:
        raise ConfigError("ss is required for socket memory snapshots")
    if media.rate_bps(cfg.quality_rate) * cfg.quality_programs * steps[-1] > 20_000_000_000:
        raise ConfigError("capacity case nominal egress exceeds 20 Gbit/s")


def run(cfg):
    """The capacity-quality-ladder phase: 0 every ladder passed, 1 a ladder
    recorded failures, 3 the ladder could not run or the matrix failed."""
    try:
        mixes = select_mixes(cfg)
        validate(cfg, mixes)
    except (ConfigError, proc.Failed, OSError, ValueError) as err:
        print(err, file=sys.stderr)
        return 3
    print()
    print("== %d %s delivery-quality ladders" % (len(mixes), cfg.quality_rate))
    print("   each rung uses one program; per-protocol one-destination baselines calibrate before scaling.")
    print("quality_mixes=" + " ".join(mix[0] for mix in mixes))
    print(recipe_line(cfg))
    failed = [mix[0] for mix in mixes if not run_mix(cfg, *mix)]
    done = proc.run([sys.executable, cfg.helper("capacity_diagnostics.py"), "matrix", cfg.run],
                    timeout=300, check=False)
    sys.stdout.write(done.stdout)
    if done.returncode != 0:
        print("capacity matrix failed (%d): %s" % (done.returncode, done.stderr.strip()[-400:]),
              file=sys.stderr)
        return 3
    print("quality_ladders_with_failures=" + (" ".join(failed) or "none"))
    return 1 if failed else 0


def references_needed(protocol, srt_share, hls_push_share, steps):
    """Protocols whose one-destination reference no ladder rung provides: the
    first rung carrying the protocol must carry exactly one, and a mixed SRT
    share is always calibrated on its own."""
    present = [key for key, here in (
        ("srt", protocol == "srt" or srt_share > 0),
        ("rtmp", protocol in ("rtmp", "rtmp-hls-push")),
        ("hls", protocol == "hls"),
        ("hls_push", protocol in ("hls-push", "rtmp-hls-push"))) if here]
    counts = [rung.split(protocol, n, srt_share, hls_push_share) for n in steps]
    return [key for key in present
            if (key == "srt" and protocol != "srt")
            or next((c[key] for c in counts if c[key] > 0), 1) != 1]


def run_mix(cfg, mix, protocol, srt_share, hls_push_share):
    """One mix's ladder; True when no rung failed. Prints the ladder's lines."""
    refs = {}
    sink_mode = "fast" if cfg.sink_mode == "fast" else "correctness"
    steps = cfg.quality_steps
    for key in references_needed(protocol, srt_share, hls_push_share, steps):
        name = PROTOCOL_NAME[key]
        print()
        print("== calibrating the %s reference for %s (one %s destination)"
              % (PROTOCOL_LABEL[key], mix, PROTOCOL_LABEL[key]))
        spec = rung.Spec("%s-%s-reference" % (mix, name), name, 1,
                         rung.split(name, 1), sink_mode)
        if attempt(cfg, spec, refs, "quality-%s-%s-reference" % (mix, name))[0] != "pass" \
                or not refs.get(key):
            print("mix=%s %s reference calibration failed" % (mix, name), file=sys.stderr)
            return False
    print()
    print("== %s %s receiver-quality ladder" % (mix, cfg.quality_rate))
    print("   duration=%gs per rung; reference=one destination per protocol" % cfg.quality_seconds)
    print("   SRT counts use nearest-integer share rounding; actual counts shown per rung.")
    print("   pass: per-destination delivery >=%s of its protocol reference" % cfg.min_delivery_ratio)
    if protocol == "srt":
        print("   SRT quality also requires interval floor=%s," % cfg.interval_floor)
        print("   lag limit=%ss, queue pressure limit=" % cfg.max_low_seconds)
        print("   %s for %s samples," % (cfg.queue_pressure, cfg.pressure_samples))
        print("   and zero MPEG-TS errors/application drops.")
    consecutive, failed, skipped = 0, [], []
    for index, n in enumerate(steps):
        if cfg.stop_after_failures > 0 and consecutive >= cfg.stop_after_failures:
            skipped.append(n)
            continue
        counts = rung.split(protocol, n, srt_share, hls_push_share)
        shown = "srt=%d rtmp=%d hls=%d hls_push=%d" % (
            counts["srt"], counts["rtmp"], counts["hls"], counts["hls_push"])
        print("quality_rung_start mix=%s destinations=%d %s actual_shares_srt=%.2f%%_rtmp=%.2f%%"
              "_hls_push=%.2f%%" % (mix, n, shown, 100 * counts["srt"] / n,
                                    100 * counts["rtmp"] / n, 100 * counts["hls_push"] / n))
        label = "quality-%s-%d" % (mix, n)
        outcome, status = attempt(cfg, rung.Spec(mix, protocol, n, counts, sink_mode), refs, label)
        if outcome == "fail" and cfg.sink_mode == "auto" and sink_mode == "correctness":
            print('quality_mode_switch mix=%s destinations=%d switch_from=correctness '
                  'switch_to=fast reason="correctness sink limit reached; continuing in '
                  'high-performance mode"' % (mix, n))
            sink_mode = "fast"
            supersede(cfg.path("capacity", label), mix)
            outcome, status = attempt(cfg, rung.Spec(mix, protocol, n, counts, sink_mode),
                                      refs, label + "-fast")
        if outcome == "pass":
            print("quality_rung_result mix=%s destinations=%d %s sink_mode=%s result=pass"
                  % (mix, n, shown, sink_mode))
            consecutive = 0
        elif outcome == "infrastructure-limited":
            print("quality_rung_result mix=%s destinations=%d result=infrastructure-limited" % (mix, n))
            print("mix=%s infrastructure_limited_rung=%d" % (mix, n))
            skipped += steps[index:]
            break
        elif outcome == "setup-error":
            print("quality_rung_result mix=%s destinations=%d result=setup-error status=%d"
                  % (mix, n, status))
            print("mix=%s first_failing_destination_rung=%s" % (mix, failed[0] if failed else "unmeasured"))
            print("mix=%s setup_failure_rung=%d (higher rungs unmeasured)" % (mix, n))
            print("quality_ladder_aborted_at=%d" % n)
            return False
        else:
            print("quality_rung_result mix=%s destinations=%d %s result=fail" % (mix, n, shown))
            consecutive += 1
            failed.append(n)
    if skipped:
        print("mix=%s unmeasured_rungs=%s (after %d consecutive failures)"
              % (mix, " ".join(map(str, skipped)), cfg.stop_after_failures))
    print("mix=%s first_failing_destination_rung=%s" % (mix, failed[0] if failed else "none"))
    print("mix=%s quality_failed_rungs=%s" % (mix, " ".join(map(str, failed)) or "none"))
    return not failed


def supersede(case_dir, mix):
    """A correctness attempt the fast retry replaces keeps its evidence but
    leaves the mix's matrix, as the retry is that rung's result."""
    path = os.path.join(case_dir, "diagnostics.json")
    try:
        with open(path, encoding="utf-8") as source:
            bundle = json.load(source)
    except (OSError, ValueError):
        return
    bundle.setdefault("case", {})["mix"] = mix + "-correctness"
    with open(path, "w", encoding="utf-8") as output:
        json.dump(bundle, output, indent=2)


def attempt(cfg, spec, refs, label):
    """One rung in its own Scope -> (outcome, setup status). Every exit,
    including a setup error, stops what the rung started and leaves a bundle."""
    case_dir = cfg.path("capacity", label)
    shutil.rmtree(case_dir, ignore_errors=True)
    os.makedirs(case_dir)
    case, status = rung.Rung(cfg, case_dir, spec), 1
    with proc.Scope() as scope:
        try:
            case.prepare()
            status = 3
            note = preflight(cfg, scope, case_dir, spec)
            status = 1
            return ("pass" if case.run(scope, refs, note) else "fail"), 0
        except rung.Infrastructure as err:
            print("   %s" % err)
            print("capacity_case_result case=%s result=infrastructure-limited" % label)
            outcome, extra = ("infrastructure-limited", 4), {}
        except Exception as err:  # noqa: BLE001 - every setup failure is recorded, then the rung ends
            if isinstance(err, proc.Failed):
                print("%s: %s" % (label, err), file=sys.stderr)
            else:
                traceback.print_exc()
            outcome, extra = ("setup-error", status), {"status": status}
        try:
            case.diagnostics("setup-failure" if extra else outcome[0], **extra)
        except proc.Failed as err:
            print(err, file=sys.stderr)
        return outcome


def calibration(cfg, case_dir, mix):
    """CPU per delivered Gbit/s from the newest passing bundle of this mix,
    re-derived for every rung; CAPACITY_PREFLIGHT_SENDER_CPU_PER_GBPS overrides."""
    found = []
    for path in glob.glob(cfg.path("capacity", "*", "diagnostics.json")):
        if os.path.dirname(path) == case_dir:
            continue
        try:
            with open(path, encoding="utf-8") as source:
                bundle = json.load(source)
        except (OSError, ValueError):
            continue
        if bundle.get("outcome") == "pass" and (bundle.get("case") or {}).get("mix") == mix:
            found.append((os.path.getmtime(path), path, bundle))
    sender, receiver = cfg.preflight_sender_cpu_per_gbps or None, None
    if not found:
        return sender, receiver
    _, path, bundle = max(found, key=lambda item: item[:2])
    efficiency = bundle.get("efficiency") or {}

    def value(name):
        field = efficiency.get(name) or {}
        return field.get("value") if field.get("status") == "ok" else None
    sender = sender or value("sender_cpu_per_gbps")
    receiver = value("receiver_cpu_per_gbps")
    if sender or receiver:
        print("   preflight: calibrated from %s (sender %s%%core/Gbit/s, receiver %s%%core/Gbit/s)"
              % (os.path.basename(os.path.dirname(path)), sender or "?", receiver or "?"))
    return sender, receiver


def _logged(argv, log, ns=None, cpus=None, timeout=60, what=None):
    done = proc.run(argv, ns=ns, cpus=cpus, timeout=timeout, check=False)
    with open(log, "w", encoding="utf-8") as output:
        output.write(done.stdout + done.stderr)
    if done.returncode != 0:
        raise proc.Failed("preflight: %s failed (%d), see %s" % (what, done.returncode, log))


def preflight(cfg, scope, case_dir, spec):
    """Fingerprint sender, receiver and path, then judge whether the host can
    carry this rung. None or "insufficient-evidence"; raises Infrastructure."""
    total = spec.programs * spec.destinations
    if cfg.preflight == "no" or (cfg.preflight == "auto" and total < cfg.preflight_min_destinations):
        return None
    print("   preflight: sender, receiver and path fingerprint for %d destinations" % total)
    tool = [sys.executable, cfg.helper("capacity_preflight.py")]
    path = lambda name: os.path.join(case_dir, name)  # noqa: E731
    _logged(tool + ["host", "--role", "sender", "--binary", cfg.nginx, "--json",
                    path("preflight.sender.json")], path("preflight.sender.log"),
            ns=proc.SENDER, cpus=cfg.nginx_cpus, what="sender fingerprint")
    rtmp_sink = cfg.rtmp_sink_nginx if cfg.engine == "restream" else cfg.nginx
    binary = (cfg.path("srt_fanout_sink") if spec.counts["srt"] else
              rtmp_sink if spec.counts["rtmp"] else shutil.which("python3"))
    _logged(tool + ["host", "--role", "receiver", "--binary", binary, "--json",
                    path("preflight.receiver.json")], path("preflight.receiver.log"),
            ns=proc.RECEIVER, what="receiver fingerprint")
    seconds, port = float(cfg.preflight_seconds), cfg.base + 40
    listener = scope.spawn("preflight probe listener", tool + [
        "probe", "--listen", "--port", port, "--seconds", cfg.preflight_seconds,
        "--threads", cfg.preflight_threads, "--json", path("preflight.rx.json")],
        ns=proc.RECEIVER, log=path("preflight.rx.log"))
    proc.wait_until("preflight probe listener", lambda: proc.run(
        ["ss", "-Hlun", "sport = :%d" % port], ns=proc.RECEIVER, timeout=5).stdout.strip(),
        timeout=10, alive=[listener])
    _logged(tool + ["probe", "--peer", cfg.receiver_addr, "--port", port, "--seconds",
                    "%g" % (seconds - 1), "--threads", cfg.preflight_threads, "--json",
                    path("preflight.tx.json")], path("preflight.tx.log"),
            ns=proc.SENDER, timeout=seconds + 30, what="sender probe")
    if listener.wait(seconds + 30) != 0:
        raise proc.Failed("preflight: receiver probe failed, see %s" % path("preflight.rx.log"))
    argv = tool + ["judge", "--destinations", total, "--bitrate-bps",
                   media.rate_bps(cfg.quality_rate), "--sender", path("preflight.sender.json"),
                   "--network", path("preflight.rx.json"), "--probe-sender",
                   path("preflight.tx.json"), "--json", path("preflight.json"),
                   "--receiver", path("preflight.receiver.json"), "--sender-under-test"]
    if cfg.nginx_cpus:
        argv += ["--sender-cpus", cfg.nginx_cpus]
    if cfg.receiver_cpus:
        argv += ["--receiver-cpus", cfg.receiver_cpus]
    pinned = ",".join(cpus for cpus in (cfg.nginx_cpus, cfg.receiver_cpus) if cpus)
    if pinned:
        argv += ["--benchmark-cpus", pinned]
    sender, receiver = calibration(cfg, case_dir, spec.mix)
    if sender:
        argv += ["--sender-cpu-per-gbps", sender]
    if receiver:
        argv += ["--receiver-cpu-per-gbps", receiver]
    done = proc.run(argv, timeout=120, check=False)
    for line in done.stdout.splitlines():
        print("   " + line)
    sys.stderr.write(done.stderr)
    if done.returncode == 2:
        raise rung.Infrastructure("preflight: the host or path cannot carry %d destinations" % total)
    if done.returncode == 3:
        return "insufficient-evidence"
    if done.returncode != 0:
        raise proc.Failed("preflight: judge exited %d" % done.returncode)
    return None

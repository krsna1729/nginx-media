"""CPU, memory and socket accounting around a measurement window."""

import collections
import os
import re
import sys

from . import proc

Window = collections.namedtuple("Window", "before after seconds before_ms after_ms")

_DETAIL_ROLES = {"SRT:RcvQ", "SRT:SndQ", "SRT:TsbPd", "SRT:GC", "ingest", "worker", "srt-send", "hls-push"}


def _write_roles(path, roles):
    with open(path, "w") as handle:
        handle.writelines("%s\t%s\n" % item for item in roles.items())


def _sampler(cfg):
    return [sys.executable, cfg.helper("proc_cpu_sampler.py")]


def cpu_window(cfg, case_dir, seconds, pids, roles, boundary=None):
    """Per-thread CPU of `pids` over `seconds`, by proc_cpu_sampler.py.

    case_dir None keeps the snapshots in $RUN; a case dir also gets the
    host.before/after.json captures. `boundary(action)`, action 'start' or
    'stop', runs while the sampler is parked at that boundary, before it is
    released; a callable it returns runs right after the release. The
    sampler gives up 60 s after parking, so the hook must finish well inside
    that. Any failure leaves the boundary closed and stops the sampler."""
    if not pids:
        raise proc.Failed("cpu window has no processes to sample")
    folder = case_dir or cfg.run
    _write_roles(cfg.path("roles"), roles)
    _write_roles(os.path.join(folder, "cpu.roles"), roles)
    before, after = os.path.join(folder, "cpu.before"), os.path.join(folder, "cpu.after")
    log = os.path.join(folder, "cpu.window.log")
    argv = _sampler(cfg) + ["sample", "--sleep", seconds, "--roles", cfg.path("roles"),
                            "--before", before, "--after", after]
    if case_dir:
        argv += ["--extended-before", os.path.join(folder, "host.before.json"),
                 "--extended-after", os.path.join(folder, "host.after.json")]
    if boundary:
        argv += ["--boundary-dir", folder]
    for name in os.listdir(folder):
        if name == "cpu.window.log" or boundary and name.startswith(("cpu.ready.", "cpu.release.", "cpu.captured.")):
            os.remove(os.path.join(folder, name))
    with proc.Scope() as scope:
        sampler = scope.spawn("cpu sampler", argv + list(pids), log=log)
        if boundary:
            for phase, action, timeout in (("before", "start", 30), ("after", "stop", seconds + 60)):
                ready = os.path.join(folder, "cpu.ready." + phase)
                proc.wait_until("cpu sampler %s boundary" % phase, lambda: os.path.exists(ready),
                                timeout, interval=0.01, alive=[sampler])
                finish = boundary(action)
                open(os.path.join(folder, "cpu.release." + phase), "w").close()
                if finish:
                    finish()
        status = sampler.wait(seconds + 120)
    with open(log) as handle:
        lines = handle.read().splitlines()
    if status != 0:
        raise proc.Failed("cpu sampler exited %s: %s" % (status, " | ".join(lines[-3:])))
    for line in reversed(lines):
        try:
            return Window(before, after, *map(float, line.split()))
        except (TypeError, ValueError):
            continue
    raise proc.Failed("cpu sampler reported no window in %s" % log)


def report_cpu(cfg, window, pid=None):
    """[(pid, role, percent of one core)] for the window, sorted by role then
    pid; only `pid`'s threads when given."""
    done = proc.run(_sampler(cfg) + ["report", window.before, window.after,
                                     "--window", window.seconds], timeout=120)
    rows = []
    for line in done.stdout.splitlines():
        owner, _, rest = line.partition(" ")
        role, _, pct = rest.rpartition(" ")
        if pid is None or int(owner) == pid:
            rows.append((int(owner), role, float(pct)))
    return sorted(rows, key=lambda row: (row[1], str(row[0])))


def cpu_by_label(rows, label):
    """{label(pid): summed percent}; label is e.g. Instance.slot_name."""
    totals = collections.defaultdict(float)
    for pid, _, pct in rows:
        totals[label(pid)] += pct
    return dict(totals)


def cpu_total(rows, label):
    return " ".join("%s=%d%%" % (name, pct) for name, pct in sorted(cpu_by_label(rows, label).items()))


def detail_cpu(rows, label):
    """[(label, role, percent)] for the roles worth a line, plus one
    'other' row per label summing the rest."""
    detail, other = [], collections.defaultdict(float)
    for pid, role, pct in rows:
        if role in _DETAIL_ROLES or role.startswith("srt-egress-"):
            detail.append((label(pid), role, pct))
        else:
            other[label(pid)] += pct
    return sorted(detail + [(name, "other", pct) for name, pct in other.items()],
                  key=lambda row: row[:2])


def memory_snapshot(path, pids):
    """`pid rss_kb pss_kb` lines from /proc/<pid>/smaps_rollup."""
    lines = []
    for pid in pids:
        try:
            with open("/proc/%d/smaps_rollup" % pid) as handle:
                text = handle.read()
        except OSError as error:
            raise proc.Failed("could not read %d smaps_rollup: %s" % (pid, error))
        rss, pss = (re.search(r"^%s:\s+(\d+)" % key, text, re.M) for key in ("Rss", "Pss"))
        if not (rss and pss):
            raise proc.Failed("no Rss/Pss in %d smaps_rollup" % pid)
        lines.append("%d %s %s\n" % (pid, rss.group(1), pss.group(1)))
    with open(path, "w") as handle:
        handle.writelines(lines)


def memory_report(before, after):
    def rows(path):
        with open(path) as handle:
            return [line.split() for line in handle if line.strip()]
    start = {pid: (int(rss), int(pss)) for pid, rss, pss in rows(before)}
    report = []
    for pid, rss, pss in rows(after):
        rss0, pss0 = start.get(pid, (0, 0))
        rss, pss = int(rss), int(pss)
        report.append("      pid=%s rss_kb=%d->%d delta_kb=%+d pss_kb=%d->%d delta_kb=%+d"
                      % (pid, rss0, rss, rss - rss0, pss0, pss, pss - pss0))
    return report


def socket_snapshot(cfg, prefix, pids):
    """Socket memory per sender process from `ss -m` in the sender namespace
    (prefix.tcp/.udp, summary TSV at prefix) and that namespace's sockstat."""
    if not pids:
        raise proc.Failed("socket snapshot has no sender processes")
    text = {}
    for proto in ("tcp", "udp"):
        text[proto] = proc.run(["ss", "-" + proto[0], "-n", "-a", "-m", "-p"], ns=proc.SENDER,
                               timeout=30).stdout
        with open("%s.%s" % (prefix, proto), "w") as handle:
            handle.write(text[proto])
    # /proc/<pid>/net is the network namespace of that process: the sender's.
    with open("/proc/%d/net/sockstat" % pids[0]) as source, open(prefix + ".sockstat", "w") as handle:
        handle.write(source.read())
    stats = {str(pid): dict.fromkeys(("tcp_sockets", "tcp_bytes", "udp_sockets", "udp_bytes"), 0)
             for pid in pids}
    for proto, body in text.items():
        lines = body.splitlines()
        for index, line in enumerate(lines):
            match = re.search(r"skmem:\(([^)]*)\)", line)
            if not match:
                continue
            owners = set(re.findall(r"pid=(\d+)", "".join(lines[max(0, index - 2):index + 1])))
            fields = {key: int(value) for key, value in re.findall(r"([a-z]+)(\d+)", match.group(1))}
            memory = sum(fields.get(key, 0) for key in ("r", "t", "f", "w", "o", "bl"))
            for pid in owners.intersection(stats):
                stats[pid][proto + "_sockets"] += 1
                stats[pid][proto + "_bytes"] += memory
    with open(prefix, "w") as handle:
        for pid in sorted(stats, key=int):
            row = stats[pid]
            handle.write("\t".join(map(str, (pid, row["tcp_sockets"], row["tcp_bytes"],
                                             row["udp_sockets"], row["udp_bytes"],
                                             row["tcp_sockets"] + row["udp_sockets"],
                                             row["tcp_bytes"] + row["udp_bytes"]))) + "\n")


def sockstat_value(path, proto, field):
    """A field of a /proc/net/sockstat snapshot, e.g. ('TCP:', 'mem')."""
    with open(path) as handle:
        for line in handle:
            parts = line.split()
            if parts and parts[0] == proto:
                pairs = dict(zip(parts[1::2], parts[2::2]))
                return int(pairs[field]) if field in pairs else None
    return None

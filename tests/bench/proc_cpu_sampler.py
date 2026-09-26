#!/usr/bin/env python3
"""Low-overhead per-thread CPU and host/network snapshots for benchmarks.

Two subcommands:

  sample   take a "before" snapshot, sleep, take an "after" snapshot.  Each
           snapshot is a text file of thread rows plus, when --extended-before
           and --extended-after are given, a JSON document with the host,
           cgroup, affinity, kernel network counters and per-socket UDP drop
           counters captured at the same instant.  Prints
           "<window_s> <before_ms> <after_ms>" for the shell harness.

  report   per (pid, role) CPU between two thread snapshots, as a percentage
           of one core.  Threads that exist in only one snapshot, or whose TID
           was reused (different start time), are accounted for explicitly
           rather than silently producing negative or inflated numbers.

The process's own counter (/proc/<pid>/stat) is the authoritative total: it
keeps the CPU of threads that have exited, which no thread snapshot can see
- a thread that ran after the first snapshot and was gone by the second, or
one born and dead between them.  Thread deltas only attribute that total to
roles; what they cannot attribute is reported as the role "(unattributed)"
of its process, so per-process and per-kind sums are the process's real CPU.

The thread snapshot format is one header line followed by rows:

  # clk_tck=<ticks per second> monotonic_ns=<start> <end>
  P <pid> <utime+stime ticks of the whole process> <starttime ticks>
  <pid> <tid> <role> <utime+stime ticks> <starttime ticks>

Percentages are always "percent of one core": 100 is one fully busy core and
a multithreaded process may legitimately exceed 100.  Capacity-relative
utilization is reported separately against the permitted CPU count.
"""

import argparse
import json
import os
import sys
import time

CLK_TCK = os.sysconf("SC_CLK_TCK")


def load_roles(path):
    roles = {}
    if not path:
        return roles
    try:
        with open(path, encoding="utf-8") as source:
            for line in source:
                line = line.rstrip("\n")
                if "\t" not in line:
                    continue
                tid, role = line.split("\t", 1)
                roles[tid] = role
    except FileNotFoundError:
        pass
    return roles


def role_for(tid, comm, roles):
    if comm.startswith("srt-egress-"):
        return comm
    if tid in roles:
        return roles[tid]
    if comm.startswith("SRT:RcvQ"):
        return "SRT:RcvQ"
    if comm.startswith("SRT:SndQ"):
        return "SRT:SndQ"
    if comm.startswith("SRT:TsbPd"):
        return "SRT:TsbPd"
    if comm.startswith("SRT:GC"):
        return "SRT:GC"
    if comm.startswith("hls-push"):
        return "hls-push"
    if comm.startswith("nginx"):
        return "nginx-thread"
    return comm.replace(" ", "_").replace("\t", "_") or "unknown"


def parse_stat(stat):
    comm_start = stat.find("(")
    comm_end = stat.rfind(")")
    if comm_start < 0 or comm_end <= comm_start:
        return None
    fields = stat[comm_end + 2:].split()
    # fields[0] is state (field 3); utime/stime are fields 14/15, starttime
    # is field 22, processor is field 39.
    if len(fields) <= 36:
        return None
    try:
        ticks = int(fields[11]) + int(fields[12])
        start = int(fields[19])
        processor = int(fields[36])
    except ValueError:
        return None
    return stat[comm_start + 1:comm_end], ticks, start, processor


def read_text(path):
    try:
        with open(path, encoding="ascii", errors="replace") as source:
            return source.read()
    except OSError:
        return None


def status_field(text, name):
    if text is None:
        return None
    for line in text.splitlines():
        if line.startswith(name + ":"):
            return line.split(":", 1)[1].strip()
    return None


def thread_snapshot(pids, roles, with_affinity):
    started = time.monotonic_ns()
    rows = []
    affinity = {}
    for pid in pids:
        stat = read_text(f"/proc/{pid}/stat")
        parsed = parse_stat(stat) if stat is not None else None
        if parsed is not None:
            rows.append(("P", pid, None, parsed[1], parsed[2]))
        task_dir = f"/proc/{pid}/task"
        try:
            tasks = list(os.scandir(task_dir))
        except OSError:
            continue
        for task in tasks:
            tid = task.name
            if not tid.isdecimal():
                continue
            stat = read_text(os.path.join(task.path, "stat"))
            if stat is None:
                continue
            parsed = parse_stat(stat)
            if parsed is None:
                continue
            comm, ticks, start, processor = parsed
            role = role_for(tid, comm, roles)
            rows.append((pid, tid, role, ticks, start))
            if with_affinity:
                status = read_text(os.path.join(task.path, "status"))
                affinity[tid] = {
                    "pid": int(pid),
                    "role": role,
                    "cpus_allowed": status_field(status, "Cpus_allowed_list"),
                    "last_cpu": processor,
                    "voluntary_ctxt_switches":
                        _int(status_field(status, "voluntary_ctxt_switches")),
                    "nonvoluntary_ctxt_switches":
                        _int(status_field(status, "nonvoluntary_ctxt_switches")),
                }
    finished = time.monotonic_ns()
    return started, finished, rows, affinity


def _int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def write_threads(path, started, finished, rows):
    with open(path, "w", encoding="ascii") as output:
        output.write(f"# clk_tck={CLK_TCK} monotonic_ns={started} {finished}\n")
        for pid, tid, role, ticks, start in rows:
            if pid == "P":
                output.write(f"P {tid} {ticks} {start}\n")
            else:
                output.write(f"{pid} {tid} {role} {ticks} {start}\n")


def read_threads(path, processes=None):
    clk = 100
    rows = {}
    stamp = None
    with open(path, encoding="ascii") as source:
        for line in source:
            if line.startswith("#"):
                for token in line[1:].split():
                    if token.startswith("clk_tck="):
                        clk = int(token.split("=", 1)[1])
                parts = line.split("monotonic_ns=")
                if len(parts) == 2:
                    a, b = parts[1].split()[:2]
                    stamp = (int(a) + int(b)) / 2
                continue
            fields = line.split()
            if fields and fields[0] == "P":
                if processes is not None and len(fields) >= 4:
                    processes[fields[1]] = (int(fields[2]), int(fields[3]))
                continue
            if len(fields) < 4:
                continue
            start = int(fields[4]) if len(fields) > 4 else -1
            rows[(fields[0], fields[1])] = (fields[2], int(fields[3]), start)
    return clk, stamp, rows


def cpu_deltas(before_path, after_path, window_s=None):
    """Return ({(pid, role): pct}, accounting dict)."""
    procs_b, procs_a = {}, {}
    clk_b, stamp_b, before = read_threads(before_path, procs_b)
    clk_a, stamp_a, after = read_threads(after_path, procs_a)
    clk = clk_a or clk_b
    if window_s is None or window_s <= 0:
        if stamp_a is None or stamp_b is None:
            raise ValueError("snapshots carry no timestamps and no window given")
        window_s = (stamp_a - stamp_b) / 1e9
    totals = {}
    acct = {"threads_both": 0, "threads_born": 0, "threads_exited": 0,
            "tid_reused": 0, "negative_rejected": 0,
            "clk_tck": clk, "window_s": window_s}
    for key, (role, ticks, start) in after.items():
        prior = before.get(key)
        if prior is None:
            # born during the window: every tick it has was spent inside it
            acct["threads_born"] += 1
            delta = ticks
        elif prior[2] != -1 and start != -1 and prior[2] != start:
            acct["tid_reused"] += 1
            delta = ticks
        else:
            acct["threads_both"] += 1
            delta = ticks - prior[1]
            if delta < 0:
                acct["negative_rejected"] += 1
                continue
        pct = delta * 100.0 / (clk * window_s)
        totals[(key[0], role)] = totals.get((key[0], role), 0.0) + pct
    for key in before:
        if key not in after:
            acct["threads_exited"] += 1
    # the process counters: authoritative per-process totals, with whatever
    # the thread deltas could not attribute given its own role
    processes = {}
    for pid, (ticks, start) in procs_a.items():
        prior = procs_b.get(pid)
        if prior is None or prior[1] != start or ticks < prior[0]:
            continue
        total = (ticks - prior[0]) * 100.0 / (clk * window_s)
        attributed = sum(v for (p, _), v in totals.items() if p == pid)
        unattributed = total - attributed
        processes[pid] = {"total_pct": round(total, 3),
                          "attributed_pct": round(attributed, 3),
                          "unattributed_pct": round(unattributed, 3)}
        # snapshots of the threads and of the process are not simultaneous,
        # so a tick either way is noise; beyond it, it is CPU the threads
        # present in the snapshots did not account for
        if unattributed > 100.0 / (clk * window_s):
            totals[(pid, "(unattributed)")] = unattributed
    acct["processes"] = processes
    acct["process_totals"] = bool(processes)
    return totals, acct


def read_first_line(path):
    text = read_text(path)
    return text.strip() if text is not None else None


def proc_stat_cpu():
    text = read_text("/proc/stat")
    if text is None:
        return None
    for line in text.splitlines():
        if line.startswith("cpu "):
            names = ("user", "nice", "system", "idle", "iowait", "irq",
                     "softirq", "steal", "guest", "guest_nice")
            values = [int(v) for v in line.split()[1:]]
            return dict(zip(names, values))
    return None


def snmp(path):
    text = read_text(path)
    result = {}
    if text is None:
        return result
    lines = text.splitlines()
    for header, values in zip(lines[0::2], lines[1::2]):
        name, fields = header.split(":", 1)
        _, numbers = values.split(":", 1)
        for key, value in zip(fields.split(), numbers.split()):
            try:
                result[f"{name}.{key}"] = int(value)
            except ValueError:
                pass
    return result


def softnet_dropped():
    text = read_text("/proc/net/softnet_stat")
    if text is None:
        return None
    total = 0
    for line in text.splitlines():
        parts = line.split()
        if len(parts) > 1:
            total += int(parts[1], 16)
    return total


def socket_inodes(pid):
    inodes = {}
    fd_dir = f"/proc/{pid}/fd"
    try:
        names = os.listdir(fd_dir)
    except OSError:
        return inodes
    for name in names:
        try:
            target = os.readlink(os.path.join(fd_dir, name))
        except OSError:
            continue
        if target.startswith("socket:["):
            inodes[target[8:-1]] = int(pid)
    return inodes


def udp_sockets(pid_roles):
    owners = {}
    for pid in pid_roles:
        owners.update(socket_inodes(pid))
    per_pid = {}
    for path in ("/proc/net/udp", "/proc/net/udp6"):
        text = read_text(path)
        if text is None:
            continue
        for line in text.splitlines()[1:]:
            parts = line.split()
            if len(parts) < 13:
                continue
            inode = parts[9]
            pid = owners.get(inode)
            if pid is None:
                continue
            try:
                drops = int(parts[12])
                tx_q, rx_q = (int(x, 16) for x in parts[4].split(":"))
            except ValueError:
                continue
            row = per_pid.setdefault(str(pid), {"sockets": 0, "drops": 0,
                                                "rx_queue_bytes": 0,
                                                "tx_queue_bytes": 0,
                                                "max_socket_drops": 0})
            row["sockets"] += 1
            row["drops"] += drops
            row["rx_queue_bytes"] += rx_q
            row["tx_queue_bytes"] += tx_q
            row["max_socket_drops"] = max(row["max_socket_drops"], drops)
    return per_pid


def cgroup_info():
    info = {"cpu_max": None, "cpu_stat": None}
    text = read_text("/proc/self/cgroup")
    base = "/sys/fs/cgroup"
    if text:
        for line in text.splitlines():
            if line.startswith("0::"):
                rel = line[3:].strip()
                if rel and rel != "/":
                    base = os.path.join("/sys/fs/cgroup", rel.lstrip("/"))
    info["cpu_max"] = read_first_line(os.path.join(base, "cpu.max"))
    stat = read_text(os.path.join(base, "cpu.stat"))
    if stat:
        info["cpu_stat"] = {k: int(v) for k, v in
                            (line.split() for line in stat.splitlines()
                             if len(line.split()) == 2)}
    return info


def cpu_frequencies():
    freqs = {}
    root = "/sys/devices/system/cpu"
    try:
        names = os.listdir(root)
    except OSError:
        return freqs
    for name in names:
        if not (name.startswith("cpu") and name[3:].isdecimal()):
            continue
        value = read_first_line(os.path.join(root, name,
                                             "cpufreq/scaling_cur_freq"))
        if value is not None:
            freqs[name] = int(value)
    return freqs


def process_info(pid):
    status = read_text(f"/proc/{pid}/status")
    info = {
        "alive": status is not None,
        "cpus_allowed": status_field(status, "Cpus_allowed_list"),
        "threads": _int(status_field(status, "Threads")),
        "voluntary_ctxt_switches":
            _int(status_field(status, "voluntary_ctxt_switches")),
        "nonvoluntary_ctxt_switches":
            _int(status_field(status, "nonvoluntary_ctxt_switches")),
    }
    rollup = read_text(f"/proc/{pid}/smaps_rollup")
    if rollup:
        for line in rollup.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0] in ("Rss:", "Pss:"):
                info[parts[0][:-1].lower() + "_kb"] = int(parts[1])
    return info


def extended_snapshot(pids, affinity, started_ns):
    begin = time.monotonic_ns()
    snap = {
        "monotonic_ns": started_ns,
        "clk_tck": CLK_TCK,
        "host": {
            "nproc_online": os.cpu_count(),
            "sampler_affinity": sorted(os.sched_getaffinity(0)),
            "loadavg": read_first_line("/proc/loadavg"),
            "proc_stat_cpu": proc_stat_cpu(),
            "cpu_freq_khz": cpu_frequencies(),
            "cgroup": cgroup_info(),
        },
        "net": {
            "snmp": snmp("/proc/net/snmp"),
            "netstat": snmp("/proc/net/netstat"),
            "softnet_dropped": softnet_dropped(),
            "udp_sockets_by_pid": udp_sockets(pids),
        },
        "processes": {str(pid): process_info(pid) for pid in pids},
        "threads": affinity,
    }
    snap["capture_ms"] = (time.monotonic_ns() - begin) / 1e6
    return snap


def sample(args):
    if args.sleep < 0:
        raise SystemExit("--sleep must be non-negative")
    roles = load_roles(args.roles)
    extended = bool(args.extended_before and args.extended_after)
    cpu0 = time.process_time_ns()

    b_start, b_end, rows, aff = thread_snapshot(args.pids, roles, extended)
    write_threads(args.before, b_start, b_end, rows)
    if extended:
        with open(args.extended_before, "w", encoding="utf-8") as out:
            json.dump(extended_snapshot(args.pids, aff, b_start), out)
    overhead_before = time.process_time_ns() - cpu0

    time.sleep(args.sleep)

    cpu1 = time.process_time_ns()
    a_start, a_end, rows, aff = thread_snapshot(args.pids, roles, extended)
    write_threads(args.after, a_start, a_end, rows)
    if extended:
        snap = extended_snapshot(args.pids, aff, a_start)
        snap["sampler_cpu_ms"] = {
            "before": overhead_before / 1e6,
            "after": (time.process_time_ns() - cpu1) / 1e6,
        }
        with open(args.extended_after, "w", encoding="utf-8") as out:
            json.dump(snap, out)

    before_mid = (b_start + b_end) / 2
    after_mid = (a_start + a_end) / 2
    window_s = (after_mid - before_mid) / 1_000_000_000
    print(f"{window_s:.6f} {(b_end - b_start) / 1e6:.3f} "
          f"{(a_end - a_start) / 1e6:.3f}")


def report(args):
    totals, acct = cpu_deltas(args.before, args.after, args.window)
    for (pid, role), pct in sorted(totals.items()):
        print(f"{pid} {role} {pct:.3f}")
    if args.accounting:
        with open(args.accounting, "w", encoding="utf-8") as out:
            json.dump(acct, out)


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command")

    s = sub.add_parser("sample")
    s.add_argument("--sleep", type=float, required=True)
    s.add_argument("--roles", required=True)
    s.add_argument("--before", required=True)
    s.add_argument("--after", required=True)
    s.add_argument("--extended-before")
    s.add_argument("--extended-after")
    s.add_argument("pids", nargs="+")

    r = sub.add_parser("report")
    r.add_argument("before")
    r.add_argument("after")
    r.add_argument("--window", type=float)
    r.add_argument("--accounting")

    argv = sys.argv[1:]
    # Backwards compatible: the original interface had no subcommand.
    if argv and argv[0].startswith("--"):
        argv = ["sample"] + argv
    args = parser.parse_args(argv)
    if args.command == "sample":
        sample(args)
    elif args.command == "report":
        report(args)
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())

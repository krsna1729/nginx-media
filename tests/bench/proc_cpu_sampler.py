#!/usr/bin/env python3
"""Take low-overhead benchmark process thread CPU snapshots."""

import argparse
import os
import time


def load_roles(path):
    roles = {}
    if not path:
        return roles
    try:
        with open(path, encoding="utf-8") as source:
            for line in source:
                tid, role = line.rstrip("\n").split("\t", 1)
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
    if comm == "SRT:TsbPd":
        return "SRT:TsbPd"
    if comm == "SRT:GC":
        return "SRT:GC"
    if comm.startswith("nginx"):
        return "nginx-thread"
    return comm.replace(" ", "_").replace("\t", "_") or "unknown"


def snapshot(pids, roles, path):
    started = time.monotonic_ns()
    rows = []
    for pid in pids:
        task_dir = f"/proc/{pid}/task"
        try:
            with os.scandir(task_dir) as tasks:
                for task in tasks:
                    tid = task.name
                    if not tid.isdecimal():
                        continue
                    try:
                        with open(os.path.join(task.path, "stat"),
                                  encoding="ascii") as source:
                            stat = source.read()
                    except OSError:
                        continue
                    comm_start = stat.find("(")
                    comm_end = stat.rfind(")")
                    if comm_start < 0 or comm_end <= comm_start:
                        continue
                    fields = stat[comm_end + 2:].split()
                    if len(fields) <= 12:
                        continue
                    try:
                        ticks = int(fields[11]) + int(fields[12])
                    except ValueError:
                        continue
                    comm = stat[comm_start + 1:comm_end]
                    role = role_for(tid, comm, roles)
                    rows.append(f"{pid} {tid} {role} {ticks}\n")
        except OSError:
            continue
    finished = time.monotonic_ns()
    with open(path, "w", encoding="ascii") as output:
        output.writelines(rows)
    return started, finished, (finished - started) / 1_000_000


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sleep", type=float, required=True)
    parser.add_argument("--roles", required=True)
    parser.add_argument("--before", required=True)
    parser.add_argument("--after", required=True)
    parser.add_argument("pids", nargs="+")
    args = parser.parse_args()
    if args.sleep < 0:
        parser.error("--sleep must be non-negative")

    roles = load_roles(args.roles)
    before_start, before_end, before_ms = snapshot(args.pids, roles,
                                                   args.before)
    time.sleep(args.sleep)
    after_start, after_end, after_ms = snapshot(args.pids, roles,
                                                 args.after)
    before_mid = (before_start + before_end) / 2
    after_mid = (after_start + after_end) / 2
    window_s = (after_mid - before_mid) / 1_000_000_000
    print(f"{window_s:.6f} {before_ms:.3f} {after_ms:.3f}")


if __name__ == "__main__":
    main()

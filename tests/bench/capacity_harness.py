#!/usr/bin/env python3
"""Capacity and developer benchmarks for nginx-media; PHASES selects them
(capacity-quality-ladder, floor, ingest, egress, misplace, topology). Every
knob is an environment variable, read by harness/config.py."""

import glob
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import config, ladder, phases, proc  # noqa: E402


def stop_leftovers(cfg):
    """Stop what an earlier run left in $RUN by its pidfiles, which the reset
    below would otherwise delete first."""
    for prefix in sorted(glob.glob(cfg.path("rtmp-sink-*"))) + [cfg.run]:
        master = proc.nginx_pid(prefix)
        if master and proc.pid_alive(master):
            print("   stopping a leftover master %d from this RUN" % master)
        proc.stop_nginx(prefix)


def host_facts(cfg):
    uname = os.uname()
    try:
        with open("/proc/sys/net/core/rmem_max") as handle:
            rmem = handle.read().strip()
    except OSError:
        rmem = "?"
    try:
        built = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(cfg.nginx)))
    except OSError:
        built = "?"
    print("== host")
    print("   %s %s %s, %d cpus" % (uname.sysname, uname.release, uname.machine, len(os.sched_getaffinity(0))))
    print("   net.core.rmem_max %s" % rmem)
    print("   nginx %s built %s" % (os.path.basename(cfg.nginx), built))
    print()


def main():
    sys.stdout.reconfigure(line_buffering=True)
    proc.install_signal_handlers()
    cfg = config.load()
    unknown = [p for p in cfg.phases if p != "capacity-quality-ladder" and p not in phases.PHASES]
    if unknown:
        print("unknown phase(s): %s (known: capacity-quality-ladder %s)"
              % (" ".join(unknown), " ".join(phases.PHASES)), file=sys.stderr)
        return 3
    stop_leftovers(cfg)
    shutil.rmtree(cfg.run, ignore_errors=True)
    for name in ("conf", "logs", "media"):
        os.makedirs(cfg.path(name))
    host_facts(cfg)
    status = 0
    for phase in cfg.phases:
        try:
            result = ladder.run(cfg) if phase == "capacity-quality-ladder" else phases.PHASES[phase](cfg)
        except proc.Failed as error:
            print("phase %s: %s" % (phase, error), file=sys.stderr)
            return 3
        status = max(status, result)
        if phase == "capacity-quality-ladder" and result:
            return result
    phases.conditions(cfg)
    return status


if __name__ == "__main__":
    sys.exit(main())

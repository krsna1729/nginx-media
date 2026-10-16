#!/usr/bin/env python3
"""Measure what provisioning a destination actually costs, per path.

Three paths exist in the harness's history and each one is timed here, against
the same live sender, so the comparison is measured rather than projected:

  shell-loop    the old per-destination loop: one curl process per attempt and
                a 0.2 s sleep on every 409/404, with the retry bounded by
                attempt count
  client-call   the data plane moved to Python, still one process per
                destination
  client-batch  the data plane in Python, one process per program

The split between the last two is the point: it separates "curl cost a fork
per destination" from "a per-destination loop cost a round trip per
destination", which need different fixes.

Every arm provisions into its own stream, so destination counts are equal and
the sender's per-stream work is the same.  The sender is restarted between
worker counts because `worker_processes` is read at startup, and the owner
routing under test only exists when there is more than one worker.

Usage:
    tests/bench/provision_cost.py --destinations 256 --workers 1,4,6
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

API_PORT = 26384
SRT_BASE_PORT = 27000
SRT_LISTENERS = 16


def log(message):
    print(message, flush=True)


class Sender:
    """A standalone sender instance on loopback with a chosen worker count."""

    def __init__(self, root, binary, workers):
        self.root = root
        self.binary = binary
        self.workers = workers

    def _conf(self):
        """A config built from the harness's own, so nothing is invented."""
        generated = self._harness_conf()
        text = generated.replace("10.200.0.1", "127.0.0.1")
        text = text.replace("/tmp/ns-pc", self.root)
        for old, new in (("16384", "26384"), ("16415", "26415"),
                         ("16386", "26386"), ("16387", "26387"),
                         ("16388", "26388"), ("16389", "26389")):
            text = text.replace(old, new)
        lines = []
        for line in text.splitlines():
            if line.startswith("worker_processes"):
                lines.append("worker_processes %d;" % self.workers)
            else:
                lines.append(line)
        return "\n".join(lines) + "\n"

    def _harness_conf(self):
        """The last harness-generated config, which carries the media directives.

        Reusing it keeps this benchmark honest: the instance under test is
        configured exactly as a capacity rung is.
        """
        for candidate in ("/tmp/ns-pc/conf/nginx.conf", "/tmp/ns-smoke4/conf/nginx.conf"):
            if os.path.exists(candidate):
                return open(candidate).read()
        raise SystemExit(
            "no harness-generated nginx.conf found; run a capacity rung first "
            "so this benchmark uses the real configuration")

    def start(self):
        self.stop()
        shutil.rmtree(self.root, ignore_errors=True)
        os.makedirs(os.path.join(self.root, "logs"), exist_ok=True)
        os.makedirs(os.path.join(self.root, "conf"), exist_ok=True)
        os.makedirs(os.path.join(self.root, "hls"), exist_ok=True)
        shutil.copytree(os.path.join(self.binary, "."), self.root, dirs_exist_ok=True)
        secret = "/tmp/ns-pc/ingest.secret"
        if os.path.exists(secret):
            shutil.copy(secret, self.root)
        with open(os.path.join(self.root, "conf", "nginx.conf"), "w") as handle:
            handle.write(self._conf())
        subprocess.run(
            [os.path.join(self.root, "sbin", "nginx"), "-p", self.root,
             "-c", "conf/nginx.conf"], check=True)
        for _ in range(50):
            if self.master_pid():
                break
            time.sleep(0.1)
        actual = len(self.worker_pids())
        if actual != self.workers:
            raise SystemExit("sender started %d workers, wanted %d"
                             % (actual, self.workers))

    def master_pid(self):
        out = subprocess.run(
            ["pgrep", "-f", os.path.join(self.root, "sbin/nginx")],
            text=True, capture_output=True).stdout.split()
        return int(out[0]) if out else None

    def worker_pids(self):
        master = self.master_pid()
        if master is None:
            return []
        out = subprocess.run(["pgrep", "-P", str(master)],
                             text=True, capture_output=True).stdout.split()
        return [int(p) for p in out]

    def stop(self):
        master = self.master_pid()
        if master is None:
            return
        # Scoped to this instance's own master: never a name-wide kill.
        os.kill(master, signal.SIGQUIT)
        for _ in range(50):
            if self.master_pid() is None:
                return
            time.sleep(0.1)
        os.kill(master, signal.SIGKILL)


def create_stream(port, name):
    body = json.dumps({"application": "live", "name": name}).encode()
    subprocess.run(
        ["curl", "-sS", "-o", "/dev/null", "-f", "-X", "POST",
         "-H", "Content-Type: application/json", "--data-binary", "@-",
         "http://127.0.0.1:%d/media/api/v1/streams" % port],
        input=body, check=True)


def run_shell_loop(port, stream, destinations, workers):
    """The path the harness had: curl per attempt, 0.2 s per miss."""
    script = r'''
set -u
API="http://127.0.0.1:%d/media/api/v1"
N=%d
ADDR=127.0.0.1
PORTBASE=%d
LISTENERS=%d
stream=%s

post_owner_old() {
    local i code
    for i in $(seq 1 60); do
        code="$(curl -sS -o /dev/null -w '%%{http_code}' -X POST \
            -H 'Content-Type: application/json' -d "$2" "$API$1" 2>/dev/null)"
        case "$code" in
            2*) return 0 ;;
            409|404) sleep 0.2 ;;
            *) echo "POST $1 answered $code" >&2; return 1 ;;
        esac
    done
    echo "POST $1 never reached the owner" >&2
    return 1
}

for (( i = 0; i < N; i++ )); do
    id="$(printf 's%%04d' "$i")"
    post_owner_old "/streams/$stream/destinations" \
        "{\"id\":\"$id\",\"type\":\"srt\",\"host\":\"$ADDR\",\"port\":$((PORTBASE + i %% LISTENERS)),\"streamid\":\"#!::r=$stream,m=publish,s=$id\"}" || exit 1
done
''' % (port, destinations, SRT_BASE_PORT, SRT_LISTENERS, stream)
    started = time.monotonic()
    result = subprocess.run(["bash", "-c", script], text=True,
                            capture_output=True)
    elapsed = time.monotonic() - started
    if result.returncode != 0:
        raise SystemExit("shell-loop arm failed: %s" % result.stderr.strip())
    return elapsed


def run_client_arm(port, stream, destinations, workers, batched):
    """The client, either one call per destination or one call per program."""
    client = os.path.join(PROJECT_ROOT, "tests/bench/capacity_client.py")
    api = "http://127.0.0.1:%d/media/api/v1" % port
    started = time.monotonic()

    if batched:
        result = subprocess.run(
            [sys.executable, client, "--api", api, "--workers", str(workers),
             "provision", "--stream", stream, "--spec", "srt:%d" % destinations,
             "--receiver-addr", "127.0.0.1",
             "--srt-base-port", str(SRT_BASE_PORT),
             "--srt-listeners", str(SRT_LISTENERS)],
            text=True, capture_output=True)
        if result.returncode != 0:
            raise SystemExit("client-batch arm failed: %s" % result.stderr.strip())
    else:
        for index in range(destinations):
            sink_id = "c%04d" % index
            body = json.dumps({
                "id": sink_id, "type": "srt", "host": "127.0.0.1",
                "port": SRT_BASE_PORT + index % SRT_LISTENERS,
                "streamid": "#!::r=%s,m=publish,s=%s" % (stream, sink_id),
            })
            result = subprocess.run(
                [sys.executable, client, "--api", api,
                 "--workers", str(workers), "post-owner",
                 "/streams/%s/destinations" % stream, "--body", body],
                text=True, capture_output=True)
            if result.returncode != 0:
                raise SystemExit("client-call arm failed: %s" % result.stderr.strip())

    return time.monotonic() - started


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--destinations", type=int, default=256)
    parser.add_argument("--workers", default="1,4,6",
                        help="comma-separated worker counts to compare")
    parser.add_argument("--binary",
                        default=os.path.join(PROJECT_ROOT,
                                             ".build/nginx-haivision/install"))
    parser.add_argument("--root", default="/tmp/provision-cost")
    parser.add_argument("--json", default="")
    parser.add_argument("--arms", default="shell-loop,client-call,client-batch")
    args = parser.parse_args()

    arms = [a.strip() for a in args.arms.split(",") if a.strip()]
    workers_list = [int(w) for w in args.workers.split(",") if w.strip()]
    results = []

    sender = Sender(args.root, args.binary, workers_list[0])
    for workers in workers_list:
        sender.workers = workers
        log("\n=== %d worker%s ===" % (workers, "" if workers == 1 else "s"))
        sender.start()
        log("  workers running: %d" % len(sender.worker_pids()))

        for arm in arms:
            stream = "live/pc-%s-%dw" % (arm.replace("-", ""), workers)
            create_stream(API_PORT, "pc-%s-%dw" % (arm.replace("-", ""), workers))
            if arm == "shell-loop":
                elapsed = run_shell_loop(API_PORT, stream,
                                         args.destinations, workers)
            elif arm == "client-call":
                elapsed = run_client_arm(API_PORT, stream,
                                         args.destinations, workers, False)
            else:
                elapsed = run_client_arm(API_PORT, stream,
                                         args.destinations, workers, True)
            per_destination = elapsed * 1000.0 / args.destinations
            log("  %-13s %7.2fs total  %7.2f ms/destination"
                % (arm, elapsed, per_destination))
            results.append({
                "workers": workers, "arm": arm,
                "destinations": args.destinations,
                "seconds": round(elapsed, 3),
                "ms_per_destination": round(per_destination, 3),
            })

    sender.stop()

    log("\n%-22s %s" % ("", "  ".join("%9s" % ("%dw" % w) for w in workers_list)))
    for arm in arms:
        cells = []
        for workers in workers_list:
            row = next(r for r in results
                       if r["arm"] == arm and r["workers"] == workers)
            cells.append("%9.2f" % row["ms_per_destination"])
        log("%-22s %s" % (arm + " (ms/dest)", "  ".join(cells)))

    if args.json:
        with open(args.json, "w") as handle:
            json.dump(results, handle, indent=2)
        log("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())

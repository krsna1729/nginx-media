#!/usr/bin/env bash
# Reject unsupported worker counts before initialization, including reloads.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
python3 - "$ROOT" "$NGINX" <<'PY'
import http.client
import os
from pathlib import Path
import resource
import signal
import socket
import subprocess
import sys
import tempfile
import time

root, nginx = Path(sys.argv[1]), sys.argv[2]
with tempfile.TemporaryDirectory(prefix="routing-bounds-", dir=root / ".build") as work:
    prefix = Path(work)
    (prefix / "logs").mkdir()
    (prefix / "html").mkdir()
    (prefix / "html/index.html").write_text("routing-live\n")
    config = prefix / "nginx.conf"
    log = prefix / "logs/error.log"
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    def configure(workers):
        config.write_text(f"""worker_processes {workers};
            daemon off;
            error_log {log} info;
            pid {prefix}/nginx.pid;
            events {{ worker_connections 64; }}
            http {{
                access_log off;
                server {{ listen 127.0.0.1:{port}; root {prefix}/html; }}
            }}
        """)

    command = [nginx, "-p", str(prefix) + "/", "-c", str(config)]
    # The supported maximum creates 4032 routing fds in the config-test process.
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft < 8192:
        if hard != resource.RLIM_INFINITY and hard < 8192:
            raise SystemExit("routing maximum test requires an 8192-fd process limit")
        resource.setrlimit(resource.RLIMIT_NOFILE, (8192, hard))
    for workers in (1, 2, 64, 65, 128):
        configure(workers)
        checked = subprocess.run(command + ["-t"], capture_output=True,
                                 text=True, timeout=20)
        accepted = checked.returncode == 0
        if accepted != (workers <= 64):
            raise SystemExit(f"worker_processes {workers}: unexpected config result "
                             f"{checked.returncode}\n{checked.stderr}")
        print(f"worker_processes {workers}: {'accepted' if accepted else 'rejected'}")

    configure(2)
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    def wait_for(predicate, message):
        deadline = time.monotonic() + 10
        while not predicate():
            if process.poll() is not None:
                raise RuntimeError(f"nginx exited during {message}")
            if time.monotonic() >= deadline:
                raise RuntimeError(f"timed out during {message}")
            time.sleep(0.02)

    def serving():
        try:
            client = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            try:
                client.request("GET", "/")
                response = client.getresponse()
                return response.status == 200 and response.read() == b"routing-live\n"
            finally:
                client.close()
        except OSError:
            return False

    def children():
        path = Path(f"/proc/{process.pid}/task/{process.pid}/children")
        return path.read_text().split() if path.exists() else []

    try:
        wait_for(lambda: serving() and len(children()) == 2, "two-worker startup")
        original_children = children()
        offset = log.stat().st_size
        configure(65)
        process.send_signal(signal.SIGHUP)
        # Severity, not diagnostic wording, synchronizes with rejected reload.
        wait_for(lambda: "[emerg]" in log.read_text()[offset:], "invalid reload rejection")
        if not serving() or children() != original_children:
            raise RuntimeError("invalid reload replaced or disrupted the active workers")
        print("invalid 65-worker reload: active master and workers still serve")
        configure(2)
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGQUIT)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
    if process.returncode != 0:
        raise SystemExit(f"nginx did not shut down cleanly: {process.returncode}")
print("routing worker bounds: ok")
PY

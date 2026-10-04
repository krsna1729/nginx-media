#!/usr/bin/env python3
"""Idle rejected callers cannot exhaust SRT admission or outlive source removal."""

import json
import os
from pathlib import Path
import select
import shlex
import signal
import socket
import subprocess
import tempfile
import time
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
NGINX = Path(os.environ.get("NGINX_BIN", ROOT / ".build/nginx-install/sbin/nginx"))


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def await_condition(description, predicate, seconds=5):
    deadline = time.monotonic() + seconds
    while True:
        if predicate():
            return
        remaining = deadline - time.monotonic()
        assert remaining > 0, f"timed out: {description}"
        time.sleep(min(0.02, remaining))


class Publishers:
    def __init__(self, binary, port, keys, log):
        self.process = subprocess.Popen(
            [str(binary), str(port), *keys], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1,
        )
        try:
            assert self.reply() == f"CONNECTED {len(keys)}", "caller handshake failed"
        except BaseException:
            self.process.kill()
            self.process.wait(timeout=2)
            raise

    def reply(self):
        ready, _, _ = select.select([self.process.stdout], [], [], 6)
        assert ready, "SRT caller helper stopped responding"
        line = self.process.stdout.readline().strip()
        assert line, "SRT caller helper exited unexpectedly"
        return line

    def command(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return self.reply()

    def stop(self, check=True):
        if self.process.poll() is None:
            try:
                self.process.stdin.write("quit\n")
                self.process.stdin.flush()
                self.process.wait(timeout=2)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=2)
        if check:
            assert self.process.returncode == 0, "SRT caller helper failed"


def main():
    (ROOT / ".build").mkdir(exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="srt-admission-", dir=ROOT / ".build"))
    (run / "logs").mkdir()
    helper = run / "idle-publishers"
    cflags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "srt"], text=True))
    libs = shlex.split(subprocess.check_output(
        ["pkg-config", "--libs", "srt"], text=True))
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")), "-O2", "-Wall", "-Wextra",
        "-Werror", "-std=c2x", *cflags,
        str(ROOT / "tests/integration/srt_idle_publishers.c"),
        "-o", str(helper), *libs,
    ], check=True)
    fixture = run / "fixture.ts"
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
        "-i", "testsrc2=size=320x180:rate=25", "-t", "1", "-c:v", "libx264",
        "-preset", "ultrafast", "-g", "25", "-pix_fmt", "yuv420p",
        "-f", "mpegts", str(fixture),
    ], check=True, timeout=20)

    port = free_port(socket.SOCK_DGRAM)
    api_port = free_port(socket.SOCK_STREAM)
    api = f"http://127.0.0.1:{api_port}/media/api/v1"
    log_path = run / "logs/error.log"
    config = run / "nginx.conf"
    config.write_text(f"""worker_processes 1;
daemon off;
error_log logs/error.log notice;
pid logs/nginx.pid;
events {{ worker_connections 256; }}
media_srt_listen 127.0.0.1:{port};
media_ingest_secret {run}/ingest.secret;
http {{
    access_log off;
    server {{
        listen 127.0.0.1:{api_port};
        location /media/api/ {{ media_api; }}
    }}
}}
""")

    def request(path, method="GET", body=None):
        payload = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(api + path, payload, method=method,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=1) as response:
            return json.load(response)

    def log_text():
        return log_path.read_text() if log_path.exists() else ""

    def source_up():
        sources = request("/streams/live/admission")["sources"]
        return any(s["id"] == "camera" and s["evidence"] & 1 for s in sources)

    callers = []
    with (run / "nginx.out").open("w") as nginx_log, \
            (run / "callers.log").open("w") as caller_log:
        nginx = subprocess.Popen([str(NGINX), "-p", str(run), "-c", str(config)],
                                 stdout=nginx_log, stderr=nginx_log)
        try:
            await_condition("SRT listener ready", lambda:
                            "srt listener ready" in log_text())
            request("/streams", "POST", {"application": "live", "name": "admission"})
            key = request("/streams/live/admission/sources", "POST",
                          {"id": "camera", "type": "srt"})["key"]
            wrong_type = request("/streams/live/admission/sources", "POST",
                                 {"id": "rtmp", "type": "rtmp"})["key"]
            unknown = "ZZZZZZZZZZZZZZZZZZZZZZZZZZ"
            # Before the fix all sixteen handshakes complete, the worker logs
            # rejection, and the silent sockets remain connected indefinitely.
            rejected = Publishers(helper, port,
                                  [["", unknown, wrong_type][i % 3] for i in range(16)],
                                  caller_log)
            callers.append(rejected)
            await_condition("all sixteen admission decisions", lambda:
                            log_text().count("srt publisher rejected session=") == 16)
            # libsrt marks a caller broken only if the peer's shutdown arrives
            # after the caller finished connecting; a rejection within a
            # millisecond of the handshake can beat that, so the caller's own
            # view is not evidence.  The server-side proof: with the sixteen
            # rejected callers still holding their sockets, a valid publisher
            # must still get one of the sixteen transport slots.

            valid = Publishers(helper, port, [key], caller_log)
            callers.append(valid)
            await_condition("valid publisher admitted after rejected callers", source_up)
            assert valid.command("alive") == "CONNECTED 1"
            assert valid.command("send " + str(fixture)) == "SENT"
            await_condition("valid publisher carries media", lambda:
                            request("/streams/live/admission")["program_frames"] > 0)
            rejected.stop()
            valid.stop()
            await_condition("publisher close clears transport health", lambda: not source_up())
            print("valid admission and MPEG-TS publication preserved")

            # Reuse the transport slot, but send no data whatsoever.  Deleting
            # its source must close it without a recv event or peer-idle timeout.
            idle = Publishers(helper, port, [key], caller_log)
            callers.append(idle)
            await_condition("idle publisher admitted", source_up)
            assert idle.command("alive") == "CONNECTED 1"
            request("/streams/live/admission/sources/camera", "DELETE")
            assert idle.command("closed") == "CONNECTED 0", "idle source delete left transport open"
            idle.stop()
            assert all(s["id"] != "camera" for s in
                       request("/streams/live/admission")["sources"])
            print("zero-payload source removal disconnected its idle publisher")

            replacement_key = request("/streams/live/admission/sources", "POST",
                                      {"id": "camera", "type": "srt"})["key"]
            replacement = Publishers(helper, port, [replacement_key], caller_log)
            callers.append(replacement)
            await_condition("replacement publisher admitted into reused slot", source_up)
            assert replacement.command("alive") == "CONNECTED 1"
            nginx.send_signal(signal.SIGQUIT)
            assert replacement.command("closed") == "CONNECTED 0", "shutdown left idle transport open"
            replacement.stop()
            nginx.wait(timeout=5)
            assert nginx.returncode == 0, "nginx shutdown failed"
            assert all(secret not in log_text() for secret in
                       (key, replacement_key, wrong_type, unknown)), "credential leaked into nginx log"
            print(f"reused-slot admission, idle shutdown and credential redaction passed ({run})")
        finally:
            for caller in callers:
                if caller.process.poll() is None:
                    caller.stop(check=False)
            if nginx.poll() is None:
                nginx.send_signal(signal.SIGTERM)
                try:
                    nginx.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    nginx.kill()
                    nginx.wait(timeout=2)


if __name__ == "__main__":
    main()

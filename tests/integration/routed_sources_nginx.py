#!/usr/bin/env python3
"""Routed publishers keep provisioned sources and stay independent of each other.

Two workers; the program is owned by worker 1, and publishers are retried until
the kernel lands them on worker 0, so their media travels over the routing
transport:

  1. a provisioned source survives its routed publisher's disconnect, with the
     key and priority the operator configured, and the same key publishes again;
  2. two redundant sources of one program are routed concurrently, and closing
     one leaves the other feeding the program.
"""

import json
import os
import re
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
NGINX = Path(os.environ.get("NGINX_BIN", ROOT / ".build/nginx-install/sbin/nginx"))
APP = "live"


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def fnv(text):
    h = 14695981039346656037
    for byte in text.encode():
        h ^= byte
        h = (h * 1099511628211) & ((1 << 64) - 1)
    return h


def await_condition(description, predicate, seconds=10):
    deadline = time.monotonic() + seconds
    while True:
        if predicate():
            return
        assert time.monotonic() < deadline, f"timed out: {description}"
        time.sleep(0.05)


def main():
    (ROOT / ".build").mkdir(exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="routed-sources-", dir=ROOT / ".build"))
    (run / "logs").mkdir()
    srt_port = free_port(socket.SOCK_DGRAM)
    api_port = free_port(socket.SOCK_STREAM)
    api = f"http://127.0.0.1:{api_port}/media/api/v1"
    log_path = run / "logs/error.log"
    (run / "nginx.conf").write_text(f"""worker_processes 2;
daemon off;
error_log logs/error.log info;
pid logs/nginx.pid;
events {{ worker_connections 256; }}
media_srt_listen 127.0.0.1:{srt_port};
media_ingest_secret {run}/ingest.secret;
http {{
    access_log off;
    server {{
        listen 127.0.0.1:{api_port};
        location /media/api/ {{ media_api; }}
    }}
}}
""")

    # a program owned by worker 1, so a publisher on worker 0 is routed
    name = next(f"news{i}" for i in range(1, 100)
                if fnv(f"{APP}/news{i}") % 2 == 1)

    def request(path, method="GET", body=None):
        payload = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(api + path, payload, method=method,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=2) as response:
            return json.load(response)

    def log_text():
        return log_path.read_text() if log_path.exists() else ""

    def program_frames():
        return request(f"/streams/{APP}/{name}")["program_frames"]

    def source_ids():
        return {s["id"] for s in request(f"/streams/{APP}/{name}")["sources"]}

    publishers = []

    def publish_routed(source, key):
        """Publish until the connection lands on the non-owner worker."""
        marker = f"srt publisher routed to the owner stream={APP}/{name} source={source}"
        for _ in range(20):
            seen = log_text().count(marker)
            process = subprocess.Popen([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-re",
                "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25",
                "-c:v", "libx264", "-preset", "ultrafast", "-g", "25",
                "-pix_fmt", "yuv420p", "-t", "120", "-f", "mpegts",
                f"srt://127.0.0.1:{srt_port}?mode=caller&streamid={key}",
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            publishers.append(process)
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                if log_text().count(marker) > seen:
                    return process
                time.sleep(0.05)
            process.kill()
            process.wait()
        raise AssertionError(f"{source} never landed on the non-owner worker")

    def stop(process):
        process.send_signal(signal.SIGKILL)
        process.wait()

    with (run / "nginx.out").open("w") as out:
        nginx = subprocess.Popen([str(NGINX), "-p", str(run), "-c",
                                  str(run / "nginx.conf")], stdout=out, stderr=out)
        try:
            await_condition("srt listener ready", lambda:
                            "srt listener ready" in log_text())
            request("/streams", "POST", {"application": APP, "name": name})
            key_a = request(f"/streams/{APP}/{name}/sources", "POST",
                            {"id": "enc-a", "type": "srt", "priority": 50})["key"]

            # 1. provisioned state outlives a routed connection
            first = publish_routed("enc-a", key_a)
            await_condition("routed media reaches the program",
                            lambda: program_frames() > 0)
            stop(first)
            await_condition("routed close reaches the owner", lambda:
                            "routed source closed" in log_text())
            time.sleep(0.5)
            assert "enc-a" in source_ids(), \
                "the provisioned source was deleted when its routed publisher left"
            kept = request(f"/streams/{APP}/{name}/sources/enc-a/key")["key"]
            assert kept == key_a, "the source's key changed across a routed connection"
            before = program_frames()
            publish_routed("enc-a", key_a)
            await_condition("the same key publishes again through the route",
                            lambda: program_frames() > before + 25)
            print("provisioned source and key survive a routed disconnect")

            # 2. redundant sources of one program stay independent
            stop(publishers[-1])
            key_b = request(f"/streams/{APP}/{name}/sources", "POST",
                            {"id": "enc-b", "type": "srt", "priority": 10})["key"]
            pub_a = publish_routed("enc-a", key_a)
            pub_b = publish_routed("enc-b", key_b)
            await_condition("both routed sources feed the program", lambda:
                            program_frames() > 0)
            # each connection feeds its own source: the higher-priority source
            # stays the active one for as long as its publisher is healthy.
            # (When connections of one program shared a slot, enc-a received
            # nothing, failed its health check and the program switched.)
            pattern = re.compile(r"routed frames, program=\d+ active=(\S+)")
            seen = len(pattern.findall(log_text()))
            await_condition("owner reports routed progress", lambda:
                            len(pattern.findall(log_text())) >= seen + 2, 30)
            assert set(pattern.findall(log_text())[seen:]) == {"enc-a"}, \
                "a routed publisher did not feed its own source"
            stop(pub_a)
            time.sleep(0.5)
            mark = program_frames()
            await_condition("closing one source leaves the other feeding",
                            lambda: program_frames() > mark + 25)
            assert pub_b.poll() is None, "the surviving publisher exited"
            assert {"enc-a", "enc-b"} <= source_ids(), \
                "a routed close removed a source"
            print("redundant routed sources close independently")

            # the owner numbers what it receives: a healthy route loses nothing
            def metric(name):
                values = []
                for _ in range(12):   # the scrape lands on either worker
                    with urllib.request.urlopen(api + "/metrics", timeout=2) as r:
                        for line in r.read().decode().splitlines():
                            if line.startswith(name + " "):
                                values.append(int(line.split()[1]))
                return values

            gaps = metric("nginx_media_runtime_routed_sequence_gaps_total")
            assert gaps, "the routed sequence gap counter is not exported"
            assert max(gaps) == 0, f"routed frames were lost on a healthy route: {gaps}"
            assert metric("nginx_media_runtime_routed_frame_restarts_total"), \
                "the routed frame restart counter is not exported"
        finally:
            for process in publishers:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            if nginx.poll() is None:
                nginx.send_signal(signal.SIGTERM)
                try:
                    nginx.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    nginx.kill()
                    nginx.wait(timeout=2)
    print(f"routed sources ok ({run})")


if __name__ == "__main__":
    main()

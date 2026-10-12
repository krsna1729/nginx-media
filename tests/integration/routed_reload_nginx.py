#!/usr/bin/env python3
"""Real same-count reload: isolated routes, old drains, PID takeover and peer EOF.

Run after make nginx:
  NGINX_BIN=.build/nginx-install/sbin/nginx python3 tests/integration/routed_reload_nginx.py
The native directory PID/revision regression is route_lifecycle.sh.  This run
uses real keyed RTMP publishers, /proc socket identities and per-worker APIs;
all nginx/publisher processes and ports are isolated to its temporary prefix.
"""
import http.client
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time

from routed_sources_nginx import await_condition, fnv, free_port
from rtmp_preauth_nginx import closed_within

ROOT = Path(__file__).resolve().parents[2]
NGINX = Path(os.environ.get("NGINX_BIN", ROOT / ".build/nginx-install/sbin/nginx")).resolve()
API = "/media/api/v1"


def seqpacket_fds(pid):
    inodes = {line.split()[6] for line in Path("/proc/net/unix").read_text().splitlines()[1:]
              if line.split()[4] == "0005"}
    result = set()
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            target = os.readlink(fd)
        except FileNotFoundError:
            continue
        match = re.fullmatch(r"socket:\[(\d+)\]", target)
        if match and match[1] in inodes:
            result.add(match[1])
    return result


def alive(pid):
    return Path(f"/proc/{pid}").exists()


def main():
    (ROOT / ".build").mkdir(exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="routed-reload-", dir=ROOT / ".build"))
    (run / "logs").mkdir()
    api_port, rtmp_port = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_STREAM)
    while rtmp_port == api_port:
        rtmp_port = free_port(socket.SOCK_STREAM)
    name = next(f"reload{i}" for i in range(100) if fnv(f"live/reload{i}") % 2 == 1)
    stream_path = f"/streams/live/{name}"
    config = run / "nginx.conf"
    log_path = run / "logs/error.log"
    publishers, connections = [], []

    def configure(shutdown_seconds, tls=False):
        tls_directives = (f"media_rtmp_ssl on;\n"
                          f"media_rtmp_ssl_certificate {run}/cert.pem;\n"
                          f"media_rtmp_ssl_certificate_key {run}/key.pem;\n") if tls else ""
        config.write_text(f"""worker_processes 2;
worker_shutdown_timeout {shutdown_seconds}s;
daemon off;
error_log logs/error.log info;
pid logs/nginx.pid;
events {{ worker_connections 256; }}
media_ingest_secret {run}/ingest.secret;
media_rtmp_listen 127.0.0.1:{rtmp_port};
{tls_directives}
http {{
    access_log off;
    server {{
        listen 127.0.0.1:{api_port};
        location /media/api/ {{ media_api; }}
    }}
}}
""")

    def log_text():
        return log_path.read_text() if log_path.exists() else ""

    def worker_generations():
        slots = {0: [], 1: []}
        for pid, slot in re.findall(r"\] (\d+)#\d+: media: worker (\d+) routing ready", log_text()):
            slots[int(slot)].append(int(pid))
        return [{slot: slots[slot][i] for slot in slots}
                for i in range(min(len(pids) for pids in slots.values()))]

    def connection():
        conn = http.client.HTTPConnection("127.0.0.1", api_port, timeout=3)
        connections.append(conn)
        return conn

    def request(conn, path, method="GET", body=None):
        conn.request(method, API + path,
                     body=None if body is None else json.dumps(body),
                     headers={"Content-Type": "application/json"})
        response = conn.getresponse()
        data = response.read()
        assert response.status in (200, 201), (method, path, response.status, data)
        return data.decode() if path == "/metrics" else json.loads(data)

    def fresh(path, method="GET", body=None):
        conn = connection()
        try:
            return request(conn, path, method, body)
        finally:
            conn.close()

    def provision(source):
        fresh("/streams", "POST", {"application": "live", "name": name})
        return fresh(stream_path + "/sources", "POST",
                     {"id": source, "type": "rtmp", "priority": 50})["key"]

    def worker_connections(generation):
        found = {}
        for _ in range(100):
            conn = connection()
            text = request(conn, "/metrics")
            match = re.search(r'nginx_media_worker_info\{worker="(\d+)",pid="(\d+)"\}', text)
            assert match, "worker native identity is missing"
            slot, pid = map(int, match.groups())
            assert generation[slot] == pid, (generation, slot, pid)
            if slot in found:
                conn.close()
            else:
                found[slot] = conn
            if len(found) == 2:
                return found
        raise AssertionError("HTTP requests never reached both workers")

    def stop(process):
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)

    def publish_routed(source, key, generation):
        marker = f"media: rtmp publisher routed to the owner stream=live/{name}"
        for attempt in range(30):
            before = len(log_text())
            output = (run / f"publisher-{source}-{len(publishers)}.log").open("w")
            process = subprocess.Popen([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-re",
                "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                "-c:a", "aac", "-b:a", "32k", "-t", "180", "-f", "flv",
                f"rtmp://127.0.0.1:{rtmp_port}/live/{key}",
            ], stdout=output, stderr=output)
            output.close()
            publishers.append(process)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                text = log_text()[before:]
                match = re.search(r"\] (\d+)#\d+: " + re.escape(marker), text)
                if match:
                    assert int(match[1]) == generation[0], (generation, match[0])
                    await_condition("routed OPEN arrives at this generation's owner", lambda:
                                    re.search(rf"\] {generation[1]}#\d+: media: routed source opened "
                                              rf"stream=live/{name} source={source}(?:\s|$)",
                                              log_text()[before:]) is not None)
                    return process
                if process.poll() is not None:
                    break
                time.sleep(0.05)
            stop(process)
        raise AssertionError(f"publisher {source} never reached the non-owner ({run})")

    def frame_reports(pid, source):
        return [int(value) for value in re.findall(
            rf"\] {pid}#\d+: media: \d+ routed frames, program=(\d+) active={source}(?:\s|$)",
            log_text())]

    def assert_routes(generation, previous=None):
        rows = {slot: seqpacket_fds(pid) for slot, pid in generation.items()}
        assert all(len(row) == 1 for row in rows.values()), rows
        current = rows[0] | rows[1]
        assert len(current) == 2, rows
        assert seqpacket_fds(nginx.pid) == current, "master retained obsolete route descriptors"
        if previous is not None:
            old = seqpacket_fds(previous[0]) | seqpacket_fds(previous[1])
            assert len(old) == 2, "draining generation lost an endpoint"
            assert not (old & current), "socket pairs were reused across worker generations"
        return current

    def reload(generation_number):
        nginx.send_signal(signal.SIGHUP)
        await_condition("new same-count worker generation", lambda:
                        len(worker_generations()) >= generation_number)
        generation = worker_generations()[generation_number - 1]
        await_condition("new listeners ready", lambda:
                        all(re.search(rf"\] {pid}#\d+: media: rtmp listener ready", log_text())
                            for pid in generation.values()))
        previous = worker_generations()[generation_number - 2]
        await_condition("old workers release listeners while draining", lambda:
                        all(re.search(rf"\] {pid}#\d+: gracefully shutting down", log_text())
                            for pid in previous.values()))
        return generation

    configure(45)
    with (run / "nginx.out").open("w") as output:
        nginx = subprocess.Popen([str(NGINX), "-p", str(run), "-c", str(config)],
                                 stdout=output, stderr=output)
        try:
            await_condition("initial listeners ready", lambda:
                            len(worker_generations()) == 1 and
                            log_text().count("media: rtmp listener ready") == 2)
            first = worker_generations()[0]
            old_key = provision("old")
            old_pub = publish_routed("old", old_key, first)
            await_condition("old routed media before reload", lambda:
                            bool(frame_reports(first[1], "old")))
            old_inodes = assert_routes(first)
            second = reload(2)
            assert alive(first[0]) and alive(first[1]), "old routed publisher did not retain its old owner"
            new_inodes = assert_routes(second, first)
            assert not old_inodes & new_inodes
            new_key = provision("new")
            new_pub = publish_routed("new", new_key, second)
            observed = worker_connections(second)
            await_condition("new routed program makes progress", lambda:
                            request(observed[1], stream_path)["program_frames"] > 50)
            old_reports = len(frame_reports(first[1], "old"))
            await_condition("old routed publisher still feeds its old owner", lambda:
                            len(frame_reports(first[1], "old")) > old_reports)
            assert old_pub.poll() is None and new_pub.poll() is None, \
                "a live publisher was disconnected during graceful reload"
            for conn in observed.values():
                state = request(conn, stream_path)
                assert {source["id"] for source in state["sources"]} == {"new"}, state
                metrics = request(conn, "/metrics")
                for counter in ("identity_mismatches", "no_slot", "sequence_gaps"):
                    match = re.search(rf"nginx_media_runtime_routed_{counter}_total (\d+)", metrics)
                    assert match and int(match[1]) == 0, (counter, metrics)
            opened = re.findall(r"\] (\d+)#\d+: media: routed source opened "
                                rf"stream=live/{name} source=(old|new)(?:\s|$)", log_text())
            assert all(int(pid) == (first[1] if source == "old" else second[1])
                       for pid, source in opened), opened
            print("same-count reload keeps OPEN/frame delivery and native socket identities generation-isolated")

            before_close = len(log_text())
            stop(old_pub)
            await_condition("old CLOSE reaches its old owner", lambda:
                            re.search(rf"\] {first[1]}#\d+: media: routed source closed", log_text()[before_close:]) is not None)
            await_condition("old routed workers exit after the actual close", lambda:
                            not any(alive(pid) for pid in first.values()))
            before = request(observed[1], stream_path)["program_frames"]
            await_condition("new PID continues after old owner exit", lambda:
                            request(observed[1], stream_path)["program_frames"] > before + 50)
            await_condition("replica sees live progress of the new owner", lambda:
                            request(observed[0], stream_path)["program_frames"] >= before)
            assert new_pub.poll() is None
            assert_routes(second)
            print("old CLOSE/exit leaves the new owner and its shared liveness/progress intact")

            # The same provisioned key reconnects through this generation's route.
            before_close = len(log_text())
            stop(new_pub)
            await_condition("new CLOSE reaches the current owner", lambda:
                            re.search(rf"\] {second[1]}#\d+: media: routed source closed", log_text()[before_close:]) is not None)
            assert fresh(stream_path + "/sources/new/key")["key"] == new_key
            before = request(observed[1], stream_path)["program_frames"]
            reconnected = publish_routed("new", new_key, second)
            await_condition("reconnect publishes through the current route", lambda:
                            request(observed[1], stream_path)["program_frames"] > before + 50)
            for conn in observed.values():
                conn.close()

            # A killed transport sends no CLOSE: actual endpoint EOF releases
            # its old owner's routed slot instead of keeping that worker stuck.
            configure(2)
            third = reload(3)
            assert_routes(third, second)
            os.kill(second[0], signal.SIGKILL)
            await_condition("orphaned old owner drains on peer EOF", lambda:
                            not any(alive(pid) for pid in second.values()))
            stop(reconnected)
            assert_routes(third)
            print("forced old transport exit releases its old owner without touching the new generation")

            timeout_key = provision("timeout")
            timeout_pub = publish_routed("timeout", timeout_key, third)
            third_connections = worker_connections(third)
            await_condition("timeout generation has an active routed source", lambda:
                            request(third_connections[1], stream_path)["program_frames"] > 25)
            for conn in third_connections.values():
                conn.close()
            fourth = reload(4)
            await_condition("worker_shutdown_timeout retires both routed peers", lambda:
                            not any(alive(pid) for pid in third.values()), seconds=12)
            await_condition("forced timeout disconnects the publisher", lambda:
                            timeout_pub.poll() is not None)
            assert_routes(fourth)
            assert all(alive(pid) for pid in fourth.values()) and nginx.poll() is None
            print("worker_shutdown_timeout force-closes old endpoints; master descriptor count stays bounded across reloads")

            # Start TLS on the same listener, then leave an incomplete TLS
            # record in each old worker. The native TLS handshake normally
            # waits for more bytes; worker_shutdown_timeout must override it.
            with (run / "openssl.log").open("w") as output:
                subprocess.run([
                    "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-days", "1", "-keyout", str(run / "key.pem"),
                    "-out", str(run / "cert.pem"), "-subj", "/CN=localhost",
                ], stdout=output, stderr=output, check=True)
            configure(2, tls=True)
            fifth = reload(5)
            await_condition("idle plain workers retire after enabling TLS", lambda:
                            not any(alive(pid) for pid in fourth.values()), seconds=12)
            assert_routes(fifth)
            pending = {}
            for _ in range(100):
                before = len(log_text())
                sock = socket.create_connection(("127.0.0.1", rtmp_port), timeout=3)
                connections.append(sock)
                sock.sendall(b"\x16\x03\x01\x00\x80\x01")
                await_condition("worker starts the incomplete TLS handshake", lambda:
                                re.search(r"\] (\d+)#\d+: media: rtmps handshake started",
                                          log_text()[before:]) is not None)
                pid = int(re.search(r"\] (\d+)#\d+: media: rtmps handshake started",
                                    log_text()[before:])[1])
                slot = next(slot for slot, worker_pid in fifth.items() if worker_pid == pid)
                if slot in pending:
                    sock.close()
                else:
                    pending[slot] = sock
                if len(pending) == 2:
                    break
            assert len(pending) == 2, "incomplete TLS handshakes did not reach both workers"
            sixth = reload(6)
            assert all(alive(pid) for pid in fifth.values()), \
                "a pending TLS session was discarded before its drain deadline"
            assert_routes(sixth, fifth)
            await_condition("shutdown timeout retires incomplete TLS handshakes", lambda:
                            not any(alive(pid) for pid in fifth.values()), seconds=12)
            assert all(closed_within(sock, 1) for sock in pending.values()), \
                "an incomplete TLS session survived its worker's shutdown timeout"
            assert_routes(sixth)

            # A failed TLS admission releases the same session hold as a
            # successful publisher, rather than pinning a later graceful exit.
            rejected = socket.create_connection(("127.0.0.1", rtmp_port), timeout=3)
            connections.append(rejected)
            rejected.sendall(b"\x03" + b"\x00" * 1536)
            assert closed_within(rejected, 3), "TLS listener accepted a plain RTMP handshake"
            rejected.close()
            seventh = reload(7)
            await_condition("failed TLS admission leaves no session hold", lambda:
                            not any(alive(pid) for pid in sixth.values()), seconds=12)
            assert_routes(seventh)
            assert all(alive(pid) for pid in seventh.values()) and nginx.poll() is None
            print("incomplete and failed TLS handshakes release sessions at forced drain and across repeated reloads")
        finally:
            for conn in connections:
                conn.close()
            for process in publishers:
                stop(process)
            if nginx.poll() is None:
                nginx.send_signal(signal.SIGTERM)
                try:
                    nginx.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    nginx.kill()
                    nginx.wait(timeout=2)
            for generation in worker_generations():
                for pid in generation.values():
                    if alive(pid):
                        os.kill(pid, signal.SIGKILL)
    assert nginx.returncode == 0, ("master did not shut down cleanly", nginx.returncode)
    print(f"routed reload ok ({run})")


if __name__ == "__main__":
    main()

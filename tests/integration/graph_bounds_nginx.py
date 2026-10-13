#!/usr/bin/env python3
"""Production graph codec boundaries, atomic refusal and independent HLS files.

Run: NGINX_BIN=/path/to/nginx python3 tests/integration/graph_bounds_nginx.py
Two persistent HTTP connections observe the owner and a distinct replica;
no encoder/decoder mocks or source inspection are used.
"""

import copy
import hashlib
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
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[2]
NGINX = Path(os.environ.get("NGINX_BIN", ROOT / ".build/nginx-install/sbin/nginx"))
API = "/media/api/v1"
MAX_NAME = 256
MAX_PATH = 4096
MAX_NUMBER = (1 << 32) - 1


def await_condition(description, predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while True:
        if predicate():
            return
        assert time.monotonic() < deadline, f"timed out: {description}"
        time.sleep(0.05)


def identity_directory(text):
    data = text.encode()
    if re.fullmatch(rb"[A-Za-z0-9_-]{1,255}", data):
        return text
    return "~" + str(len(data)) + "".join(
        "/" + data[start:start + 120].hex() for start in range(0, len(data), 120)
    )


def decoded_frame(input_url):
    result = subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(input_url),
        "-map", "0:v:0", "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "-",
    ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
    assert len(result.stdout) == 160 * 120 * 3, "HLS did not decode a complete frame"
    return hashlib.sha256(result.stdout).hexdigest()


def main():
    (ROOT / ".build").mkdir(exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="graph-bounds-", dir=ROOT / ".build"))
    (run / "logs").mkdir()
    (run / "hls").mkdir()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        rtmp_port = sock.getsockname()[1]
    while rtmp_port == port:
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            rtmp_port = sock.getsockname()[1]
    config = run / "nginx.conf"
    config.write_text(f"""worker_processes 2;
daemon off;
error_log logs/error.log info;
pid logs/nginx.pid;
events {{ worker_connections 256; }}
media_hls {run}/hls;
media_ingest_secret {run}/ingest.secret;
media_rtmp_listen 127.0.0.1:{rtmp_port};
http {{
    access_log off;
    client_body_buffer_size 1k;
    client_body_temp_path {run}/body;
    server {{
        listen 127.0.0.1:{port};
        location /media/api/ {{ media_api; }}
        location /hls/ {{ alias {run}/hls/; }}
    }}
}}
""")
    connections = []
    publishers = []

    def connect():
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        connections.append(conn)
        return conn

    def request(conn, path, method="GET", body=None, expected=200):
        payload = None if body is None else json.dumps(body).encode()
        conn.request(method, API + path, body=payload,
                     headers={"Content-Type": "application/json"})
        response = conn.getresponse()
        result = json.loads(response.read())
        assert response.status == expected, (method, path, response.status, result)
        return result

    def fresh(path, method="GET", body=None, expected=200):
        conn = connect()
        try:
            return request(conn, path, method, body, expected)
        finally:
            conn.close()

    app, name, source_id = "a" * MAX_NAME, "n" * MAX_NAME, "s" * MAX_NAME
    stream_path = f"/streams/{app}/{name}"
    boundary = {"streams": [{"application": app, "name": name, "sources": [{
        "id": source_id, "type": "srt", "priority": MAX_NUMBER,
        "path": "/" + "p" * (MAX_PATH - 1), "ca_file": "c" * MAX_PATH,
    }]}]}
    log = run / "logs/error.log"
    with (run / "nginx.out").open("w") as output:
        nginx = subprocess.Popen([str(NGINX), "-p", str(run), "-c", str(config)],
                                 stdout=output, stderr=output)
        try:
            def ready():
                assert nginx.poll() is None, (run / "nginx.out").read_text()
                try:
                    fresh("/streams")
                    return True
                except OSError:
                    return False
            await_condition("API ready", ready)
            fresh("/desired", "PUT", boundary)

            # Keep a connection on each worker. The same stream is local on
            # exactly one worker; it is a replica on the other.
            workers = {}
            def pin_workers():
                conn = connect()
                try:
                    observed = request(conn, stream_path)["observed_here"]
                except AssertionError:
                    conn.close()
                    return False
                if observed in workers:
                    conn.close()
                else:
                    workers[observed] = conn
                return len(workers) == 2
            await_condition("owner and replica connections", pin_workers)

            def desired(conn):
                return request(conn, "/desired")

            def graph_round_trip():
                for conn in workers.values():
                    document = desired(conn)
                    if len(document["streams"]) != 1:
                        return False
                    stream = document["streams"][0]
                    if (stream["application"], stream["name"]) != (app, name):
                        return False
                    if len(stream["sources"]) != 1:
                        return False
                    source = stream["sources"][0]
                    for field in ("id", "priority", "path", "ca_file"):
                        if source.get(field) != boundary["streams"][0]["sources"][0][field]:
                            return False
                return True
            await_condition("maximum graph operation round-trip", graph_round_trip)
            assert desired(workers[True]) == desired(workers[False])
            print("256-byte identities, 4096-byte path and CA, uint32 priority round-trip on both workers")

            fresh(stream_path, "PATCH", {
                "failure_timeout_ms": MAX_NUMBER, "recovery_timeout_ms": MAX_NUMBER,
            })
            def scalar_round_trip():
                snapshots = [request(conn, stream_path) for conn in workers.values()]
                return all(s["failure_timeout_ms"] == MAX_NUMBER
                           and s["recovery_timeout_ms"] == MAX_NUMBER for s in snapshots)
            await_condition("uint32 timeout round-trip", scalar_round_trip)
            before = {local: desired(conn) for local, conn in workers.items()}
            policy_before = {local: request(conn, stream_path) for local, conn in workers.items()}

            def unchanged():
                for local, conn in workers.items():
                    assert desired(conn) == before[local], "refused mutation changed desired state"
                    current = request(conn, stream_path)
                    for field in ("revision", "failure_timeout_ms", "recovery_timeout_ms"):
                        assert current[field] == policy_before[local][field], field

            for invalid_app, invalid_name in ((app + "x", name), (app, name + "x")):
                fresh("/streams", "POST", {"application": invalid_app, "name": invalid_name}, 400)
                fresh(f"/streams/{invalid_app}/{invalid_name}", "DELETE", expected=400)
                unchanged()
            fresh(stream_path + "/sources", "POST", {"id": source_id + "x", "type": "srt"}, 400)
            fresh(stream_path + "/sources", "POST", {
                "id": "bad-priority", "type": "srt", "priority": MAX_NUMBER + 1,
            }, 400)
            fresh(stream_path + "/sources", "POST", {
                "id": "bad-path", "type": "file", "path": "p" * (MAX_PATH + 1),
            }, 400)
            fresh(stream_path + "/sources", "POST", {
                "id": "bad-ca", "type": "hls_pull", "path": "http://127.0.0.1/index.m3u8",
                "ca_file": "c" * (MAX_PATH + 1),
            }, 400)
            fresh(stream_path, "PATCH", {
                "failure_timeout_ms": 1, "recovery_timeout_ms": MAX_NUMBER + 1,
            }, 400)
            unchanged()
            for field, value in (("id", source_id + "x"), ("path", "p" * (MAX_PATH + 1)),
                                 ("ca_file", "c" * (MAX_PATH + 1)), ("priority", MAX_NUMBER + 1)):
                invalid = copy.deepcopy(boundary)
                invalid["streams"][0]["sources"][0][field] = value
                # An earlier valid object must not be created before the
                # later over-budget object is discovered.
                invalid["streams"].insert(0, {"application": "early", "name": "must-not-exist"})
                fresh("/desired", "PUT", invalid, 400)
                unchanged()
            print("next-invalid names, paths, CA and scalars refuse atomically, including replay and mixed PATCH")

            fresh(stream_path, "PATCH", {
                "failure_timeout_ms": 2000, "recovery_timeout_ms": 50,
            })
            await_condition("media policy restored on both workers", lambda:
                            all(request(conn, stream_path)["recovery_timeout_ms"] == 50
                                for conn in workers.values()))

            reader_directory = run / "reader-input"
            reader_directory.mkdir()
            for source_id, source_type, source_path, priority in (
                    ("directory-reader", "hls_push", str(reader_directory), 173),
                    ("origin-reader", "hls_pull",
                     f"http://127.0.0.1:{port}/hls/missing/index.m3u8", 189)):
                request(workers[False], stream_path + "/sources", "POST", {
                    "id": source_id, "type": source_type, "path": source_path,
                    "priority": priority,
                }, 201)

                def reader_priority():
                    for conn in workers.values():
                        sources = request(conn, stream_path + "/sources")["sources"]
                        source = next((s for s in sources if s["id"] == source_id), None)
                        if source is None or source["priority"] != priority:
                            return False
                    return True

                await_condition("reader priority on owner and replica", reader_priority)
                request(workers[True], stream_path + "/sources/" + source_id,
                        "DELETE")
            print("reader-backed priorities survive nonowner creation on owner and replica")

            programs = [("live", "news.one", "red"), ("live", "news_one", "blue"),
                        ("live.one", "news", "green"), ("live_one", "news", "yellow"),
                        ("live", 'news"}\\one', "magenta"), (app, name, "cyan")]
            expected_frames = {}
            for application, program, color in programs:
                fixture = run / f"{color}.ts"
                subprocess.run([
                    "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                    "-i", f"color=c={color}:size=160x120:rate=25", "-t", "8",
                    "-c:v", "libx264", "-preset", "ultrafast", "-g", "25", "-pix_fmt", "yuv420p",
                    "-f", "mpegts", str(fixture),
                ], check=True, timeout=20)
                expected_frames[(application, program)] = decoded_frame(fixture)
                if (application, program) != (app, name):
                    fresh("/streams", "POST", {"application": application, "name": program}, 201)
                path = f"/streams/{quote(application, safe='')}/{quote(program, safe='')}"
                # Creation may precede delivery to the other worker; hold the
                # owner connection for the boundary stream and retry reads.
                await_condition("new program replica", lambda:
                                all(any(s["application"] == application and s["name"] == program
                                        for s in desired(conn)["streams"])
                                    for conn in workers.values()))
                source = fresh(path + "/sources", "POST", {"id": "publisher", "type": "rtmp"}, 201)
                # A tiny flat-color file finishes before the selector's health
                # recovery window. Pace the identical encoded fixture through
                # a real publisher and keep it running while all writers are
                # checked, so the naming assertion exercises active consumers.
                with (run / f"{color}-publisher.log").open("w") as publisher_log:
                    publisher = subprocess.Popen([
                        "ffmpeg", "-hide_banner", "-loglevel", "error",
                        "-re", "-stream_loop", "-1", "-i", str(fixture),
                        "-map", "0:v:0", "-c:v", "copy", "-f", "flv",
                        f"rtmp://127.0.0.1:{rtmp_port}/live/{source['key']}",
                    ], stdout=publisher_log, stderr=publisher_log)
                publishers.append(publisher)

                def selected():
                    assert publisher.poll() is None, (run / f"{color}-publisher.log").read_text()
                    snapshots = [request(conn, path) for conn in workers.values()]
                    owner = next((s for s in snapshots if s["observed_here"]), None)
                    return owner is not None and owner["active"] == "publisher" and owner["program_frames"] > 0
                await_condition("healthy active publisher for " + application + "/" + program, selected)

            playlists = []
            for application, program, _ in programs:
                relative = identity_directory(application) + "/" + identity_directory(program)
                playlist = run / "hls" / relative / "index.m3u8"
                await_condition("complete HLS output for " + relative,
                                lambda p=playlist: p.exists() and "#EXTINF:" in p.read_text())
                playlists.append(playlist)
            assert len({p.resolve() for p in playlists}) == len(programs), "HLS directories alias"
            assert len({p.stat().st_ino for p in playlists}) == len(programs), "playlists overwrite one another"
            # Decode after all six writers have produced output, not before a
            # colliding writer has had a chance to overwrite the first one.
            decoded_frames = set()
            for (application, program, color), playlist in zip(programs, playlists):
                relative = playlist.relative_to(run / "hls").as_posix()
                frame = decoded_frame(f"http://127.0.0.1:{port}/hls/{relative}")
                assert frame == expected_frames[(application, program)], (application, program, "wrong program's media")
                decoded_frames.add(frame)
                print(f"{color}: decoded HLS SHA256={frame} path={relative}")
                for object_name in playlist.read_text().splitlines():
                    if object_name and not object_name.startswith("#"):
                        assert (playlist.parent / object_name).is_file(), object_name
            assert len(decoded_frames) == len(programs), "distinct programs decoded the same media"
            print("punctuation, application/name pairs and 256-byte names retain independent playable HLS outputs")
        finally:
            for publisher in publishers:
                if publisher.poll() is None:
                    publisher.send_signal(signal.SIGINT)
                    try:
                        publisher.wait(timeout=4)
                    except subprocess.TimeoutExpired:
                        publisher.kill()
                        publisher.wait(timeout=2)
            for conn in connections:
                conn.close()
            if nginx.poll() is None:
                nginx.send_signal(signal.SIGTERM)
                try:
                    nginx.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    nginx.kill()
                    nginx.wait(timeout=2)
    text = log.read_text() if log.exists() else ""
    assert not re.search(r"\[(?:alert|emerg)\]|AddressSanitizer|runtime error:|exited on signal", text), text
    assert "malformed graph operation" not in text, text
    print(f"graph bounds and HLS naming ok ({run})")


if __name__ == "__main__":
    main()

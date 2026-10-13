#!/usr/bin/env python3
"""Real configured model quotas and same-program resource/RSS churn.

Run: NGINX_BIN=/path/to/nginx python3 tests/integration/model_churn_nginx.py
Requires ffmpeg; records measured worker RSS and live program frame progress.
"""

import copy
import http.client
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
NGINX = Path(os.environ.get("NGINX_BIN", ROOT / ".build/nginx-install/sbin/nginx"))
API = "/media/api/v1"
STREAM = "/streams/live/keeper"


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def await_condition(description, predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while True:
        if predicate():
            return
        assert time.monotonic() < deadline, f"timed out: {description}"
        time.sleep(0.02)


def main():
    (ROOT / ".build").mkdir(exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="model-churn-", dir=ROOT / ".build"))
    (run / "logs").mkdir()
    (run / "ingest").mkdir()
    (run / "empty.ts").write_bytes(b"")
    (run / "empty.m3u8").write_text(
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:0\n"
    )
    origin = ThreadingHTTPServer(("127.0.0.1", 0),
                                partial(SimpleHTTPRequestHandler, directory=str(run)))
    origin_thread = threading.Thread(target=origin.serve_forever, daemon=True)
    origin_thread.start()
    origin_url = f"http://127.0.0.1:{origin.server_port}/empty.m3u8"
    http_port, rtmp_port = port(), port()
    while rtmp_port == http_port:
        rtmp_port = port()
    config = run / "nginx.conf"
    config.write_text(f"""worker_processes 1;
daemon off;
error_log logs/error.log warn;
pid logs/nginx.pid;
events {{ worker_connections 256; }}
media_stream_limit 2;
media_source_limit 2;
media_destination_limit 1;
media_failover_recovery_timeout 0;
media_ingest_secret {run}/ingest.secret;
media_rtmp_listen 127.0.0.1:{rtmp_port};
http {{
    access_log off;
    server {{
        listen 127.0.0.1:{http_port};
        location /media/api/ {{ media_api; }}
    }}
}}
""")
    # These are real configuration parser checks, not source inspection.
    configured = config.read_text()
    for stream_limit, source_limit, destination_limit, valid in (
        (0, 0, 0, True),
        (256, 4294967295, 4294967295, True),
        (257, 2, 1, False),
        (2, 4294967296, 1, False),
        (2, 2, 4294967296, False),
        (2, -1, 1, False),
        (2, "unlimited", 1, False),
    ):
        candidate = run / "quota-parse.conf"
        candidate.write_text(
            configured.replace("error_log logs/error.log warn;",
                               "error_log logs/quota-parse.log warn;")
            .replace("media_stream_limit 2;", f"media_stream_limit {stream_limit};")
            .replace("media_source_limit 2;", f"media_source_limit {source_limit};")
            .replace("media_destination_limit 1;",
                     f"media_destination_limit {destination_limit};")
        )
        result = subprocess.run([str(NGINX), "-p", str(run), "-c", str(candidate), "-t",
                                 "-e", str(run / "logs/quota-parse.log")],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=10)
        assert (result.returncode == 0) == valid, (stream_limit, source_limit,
                                                 destination_limit, result.stdout.decode())

    connection = http.client.HTTPConnection("127.0.0.1", http_port, timeout=5)
    publisher = None
    worker = None

    def request(path, method="GET", body=None, expected=200):
        payload = None if body is None else json.dumps(body).encode()
        connection.request(method, API + path, body=payload,
                           headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        result = json.loads(response.read())
        if expected is not None:
            assert response.status == expected, (method, path, response.status, result)
        return response.status, result

    def desired():
        document = request("/desired")[1]
        # The keeper's live publisher advances these observational fields even
        # when a request is refused. Every configuration field and revision stays
        # in the snapshot; only the documented live fanout telemetry is excluded.
        for stream in document["streams"]:
            stream.pop("fanout_ms", None)
            stream.pop("dispatched", None)
        return document

    def assert_desired_unchanged(before, path, method, body, status, result, reason):
        after = desired()
        if after != before:
            evidence = {"reason": reason, "method": method, "path": API + path,
                        "body": body, "status": status, "response": result,
                        "before": before, "after": after}
            evidence_path = run / "desired-mutation.json"
            evidence_path.write_text(json.dumps(evidence, indent=2) + "\n")
            raise AssertionError(f"{reason}: {evidence_path}\n"
                                 f"{json.dumps(evidence, sort_keys=True)}")

    def refuse(path, method, body):
        before = desired()
        status, result = request(path, method, body, 503)
        assert_desired_unchanged(before, path, method, body, status, result,
                                 "quota refusal mutated desired configuration")

    def create_when_reclaimed(path, body):
        deadline = time.monotonic() + 15
        while True:
            before = desired()
            status, result = request(path, "POST", body, None)
            if status == 201:
                return result
            assert status == 503, (path, body, status, result)
            assert_desired_unchanged(before, path, "POST", body, status, result,
                                     "retained-slot refusal mutated desired configuration")
            assert time.monotonic() < deadline, (
                "retired object did not release its quota slot", path, body, status, result
            )
            time.sleep(0.02)

    def rss():
        text = Path(f"/proc/{worker}/status").read_text()
        return int(re.search(r"^VmRSS:\s+(\d+) kB$", text, re.MULTILINE)[1]) * 1024

    with (run / "nginx.out").open("w") as output, (run / "publisher.log").open("w") as publog:
        nginx = subprocess.Popen([str(NGINX), "-p", str(run), "-c", str(config)],
                                 stdout=output, stderr=output)
        try:
            def ready():
                assert nginx.poll() is None, (run / "nginx.out").read_text()
                try:
                    request("/streams")
                    return True
                except OSError:
                    connection.close()
                    return False
            await_condition("API ready", ready)
            children = Path(f"/proc/{nginx.pid}/task/{nginx.pid}/children").read_text().split()
            assert len(children) == 1, children
            worker = int(children[0])
            request("/streams", "POST", {"application": "live", "name": "keeper"}, 201)
            key = request(STREAM + "/sources", "POST", {
                "id": "camera", "type": "rtmp", "priority": 100,
            }, 201)[1]["key"]
            publisher = subprocess.Popen([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi",
                "-i", "testsrc2=size=160x120:rate=25", "-c:v", "libx264", "-preset",
                "ultrafast", "-g", "25", "-pix_fmt", "yuv420p", "-f", "flv",
                f"rtmp://127.0.0.1:{rtmp_port}/live/{key}",
            ], stdout=publog, stderr=publog)
            await_condition("real publisher carries frames",
                            lambda: request(STREAM)[1]["program_frames"] > 20)
            incarnation = request(STREAM)[1]["incarnation"]

            # Batch admission must refuse before the first new object is added.
            batch = copy.deepcopy(desired())
            batch["streams"].extend([
                {"application": "live", "name": "batch-a", "sources": []},
                {"application": "live", "name": "batch-b", "sources": []},
            ])
            refuse("/desired", "PUT", batch)
            batch = copy.deepcopy(desired())
            batch["streams"][0]["sources"].extend([
                {"id": "batch-a", "type": "srt"},
                {"id": "batch-b", "type": "srt"},
            ])
            refuse("/desired", "PUT", batch)
            batch = copy.deepcopy(desired())
            batch["streams"][0]["destinations"] = [
                {"id": name, "type": "rtmp", "host": "127.0.0.1", "port": 1}
                for name in ("batch-a", "batch-b")
            ]
            refuse("/desired", "PUT", batch)

            request("/streams", "POST", {"application": "live", "name": "second"}, 201)
            refuse("/streams", "POST", {"application": "live", "name": "third"})
            request("/desired", "PUT", desired())  # replay at stream capacity
            request("/streams/live/second", "DELETE")
            create_when_reclaimed("/streams", {"application": "live", "name": "reused"})
            request("/streams/live/reused", "DELETE")

            create_when_reclaimed(STREAM + "/sources", {"id": "next", "type": "srt"})
            refuse(STREAM + "/sources", "POST", {"id": "overflow", "type": "srt"})
            request("/desired", "PUT", desired())  # replay at source capacity
            request(STREAM + "/sources/next", "DELETE")

            destination = {"id": "edge", "type": "rtmp", "host": "127.0.0.1", "port": 1}
            create_when_reclaimed(STREAM + "/destinations", destination)
            refuse(STREAM + "/destinations", "POST", {**destination, "id": "overflow"})
            request("/desired", "PUT", desired())  # replay at destination capacity
            request(STREAM + "/destinations/edge", "DELETE")
            create_when_reclaimed(STREAM + "/destinations", destination)
            request(STREAM + "/destinations/edge", "DELETE")

            before = desired()
            failed_source = {
                "id": "failed", "type": "file", "path": str(run / "missing.ts"),
            }
            status, result = request(STREAM + "/sources", "POST", failed_source, None)
            assert status >= 400, "unreadable file source was reported as started"
            assert_desired_unchanged(before, STREAM + "/sources", "POST", failed_source,
                                     status, result, "failed file reader admission mutated graph")
            failed_document = copy.deepcopy(before)
            failed_document["streams"].append({
                "application": "live", "name": "reader-admission", "sources": [{
                    "id": "file", "type": "file", "path": str(run / "missing.ts"),
                }],
            })
            status, result = request("/desired", "PUT", failed_document, None)
            assert status >= 400, "desired graph reported an unstarted reader as success"
            assert_desired_unchanged(before, "/desired", "PUT", failed_document,
                                     status, result,
                                     "failed reader graph admission left an implicit stream")
            failed_document["streams"][-1]["sources"][0]["path"] = str(run / "empty.ts")
            request("/desired", "PUT", failed_document)
            request("/streams/live/reader-admission", "DELETE")
            print("configured tiny quotas: next refusal, whole-document no mutation, replay and slot reuse")

            def churn(batch_number):
                # One program and its original publisher survive all model/reader cycles.
                for i in range(512):
                    body = {"id": f"file-{batch_number}-{i}", "type": "file",
                            "path": str(run / "empty.ts"), "priority": 1}
                    create_when_reclaimed(STREAM + "/sources", body)
                    request(STREAM + "/sources/" + body["id"], "DELETE")
                for i in range(32):
                    body = {"id": f"ingest-{batch_number}-{i}", "type": "hls_push",
                            "path": str(run / "ingest"), "priority": 1}
                    create_when_reclaimed(STREAM + "/sources", body)
                    request(STREAM + "/sources/" + body["id"], "DELETE")
                for i in range(16):
                    body = {"id": f"pull-{batch_number}-{i}", "type": "hls_pull",
                            "path": origin_url, "priority": 1}
                    create_when_reclaimed(STREAM + "/sources", body)
                    request(STREAM + "/sources/" + body["id"], "DELETE")
                for i in range(128):
                    create_when_reclaimed(STREAM + "/destinations", destination)
                    request(STREAM + "/destinations/edge", "DELETE")
                    document = desired()
                    camera = document["streams"][0]["sources"][0]
                    camera["path"] = "/" + str(batch_number) + "-" + str(i) + "p" * 4000
                    camera["ca_file"] = "/" + str(i) + "c" * 4000
                    request("/desired", "PUT", document)
                # Force collection to finish before measuring retained resident memory.
                create_when_reclaimed(STREAM + "/sources", {"id": "drained", "type": "srt"})
                request(STREAM + "/sources/drained", "DELETE")
                time.sleep(0.3)
                state = request(STREAM)[1]
                assert state["incarnation"] == incarnation, "churn replaced the program"
                assert publisher.poll() is None, (run / "publisher.log").read_text()
                assert request(STREAM + "/sources/camera/key")[1]["key"] == key, (
                    "same-program churn changed provisioned publisher credentials"
                )
                return {"rss_bytes": rss(), "program_frames": state["program_frames"]}

            samples = [churn(0)]  # allocator/thread/backend warmup
            for batch_number in range(1, 5):
                samples.append(churn(batch_number))
                assert samples[-1]["program_frames"] > samples[-2]["program_frames"], samples
                assert samples[-1]["rss_bytes"] <= samples[0]["rss_bytes"] + 8 * 1024 * 1024, samples
            evidence = {"worker_pid": worker, "incarnation": incarnation,
                        "file_cycles": 2560, "ingest_cycles": 160,
                        "pull_cycles": 80,
                        "destination_cycles": 640, "path_replacements": 1280,
                        "rss_allowance_bytes": 8 * 1024 * 1024, "samples": samples}
            (run / "rss.json").write_text(json.dumps(evidence, indent=2) + "\n")
            print(json.dumps(evidence))
        finally:
            connection.close()
            if publisher is not None and publisher.poll() is None:
                publisher.send_signal(signal.SIGINT)
                try:
                    publisher.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    publisher.kill()
                    publisher.wait(timeout=2)
            if nginx.poll() is None:
                nginx.send_signal(signal.SIGTERM)
                try:
                    nginx.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    nginx.kill()
                    nginx.wait(timeout=2)
            origin.shutdown()
            origin.server_close()
            origin_thread.join(timeout=5)
    log = (run / "logs/error.log").read_text()
    assert not re.search(r"\[(?:alert|emerg)\]|AddressSanitizer|runtime error:|exited on signal", log), log
    print(f"model quotas and same-program RSS plateau ok ({run})")


if __name__ == "__main__":
    main()

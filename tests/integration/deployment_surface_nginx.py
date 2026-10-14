#!/usr/bin/env python3
"""Exercise the protected HTTP boundary of every shipped config with real nginx."""

import http.client
import json
import os
from pathlib import Path
import pwd
import re
import signal
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
NGINX = Path(os.environ.get("NGINX_BIN", ROOT / ".build/nginx-install/sbin/nginx")).resolve()
PUBLIC_PORT = int(os.environ.get("PUBLIC_PORT", "18080"))
ADMIN_PORT = int(os.environ.get("ADMIN_PORT", "18081"))
PLAYLIST = b"#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-ENDLIST\n"


def request(port, path, method="GET", body=None, host=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    headers = {"Content-Type": "application/json"}
    if host is not None:
        headers["Host"] = host
    try:
        connection.request(method, path, body=json.dumps(body) if body else None, headers=headers)
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def check_config(source):
    with tempfile.TemporaryDirectory(prefix="nginx-media-surface-") as directory:
        run = Path(directory)
        for name in ("logs", "media/hls", "media/record", "media/raw", "media/iso", "media/ingest"):
            (run / name).mkdir(parents=True, exist_ok=True)
        (run / "media/hls/surface.m3u8").write_bytes(PLAYLIST)
        (run / "media/record/surface.ts").write_bytes(b"recording surface\n")
        config = source.read_text()
        # Only the fixture copy is adapted. Mounted/operator configs remain untouched.
        config = re.sub(r"(?m)^user .*;$", f"user {pwd.getpwuid(os.geteuid()).pw_name};", config)
        config = config.replace("include /etc/nginx/conf.d/workers.conf;", "worker_processes 1;")
        config = config.replace("/var/lib/nginx/media", str(run / "media"))
        config = config.replace("/var/log/nginx/error.log", str(run / "logs/error.log"))
        config = config.replace("/usr/local/nginx/logs/nginx.pid", str(run / "logs/nginx.pid"))
        config = config.replace("listen 127.0.0.1:8081 reuseport;", f"listen 127.0.0.1:{ADMIN_PORT} reuseport;")
        config = config.replace("listen 8080 reuseport;", f"listen {PUBLIC_PORT} reuseport;")
        config = re.sub(r"(?m)^media_srt_listen 0\.0\.0\.0:\d+;$",
                        f"media_srt_listen 127.0.0.1:{free_port(socket.SOCK_DGRAM)};", config)
        config = re.sub(r"(?m)^media_rtmp_listen 0\.0\.0\.0:\d+;$",
                        f"media_rtmp_listen 127.0.0.1:{free_port(socket.SOCK_STREAM)};", config)
        config += f"\nmedia_ingest_secret {run}/media/ingest.secret;\n"
        (run / "nginx.conf").write_text(config)
        with (run / "startup.log").open("wb") as log:
            process = subprocess.Popen([str(NGINX), "-p", str(run), "-c", "nginx.conf",
                                        "-e", "stderr", "-g", "daemon off;"], stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if process.poll() is not None:
                        raise AssertionError("nginx exited before accepting admin requests")
                    try:
                        status, body = request(ADMIN_PORT, "/media/api/v1/streams")
                        if status == 200:
                            break
                    except OSError:
                        pass
                    time.sleep(0.05)
                else:
                    raise AssertionError("loopback admin never became ready")
                assert json.loads(body)["count"] == 0, body
                for path in ("/media/api", "/media/api/", "/media/api/v1/streams",
                             "/media/%61pi/v1/streams"):
                    for method in ("GET", "HEAD", "POST", "PATCH", "DELETE"):
                        status, body = request(PUBLIC_PORT, path, method,
                                               {"application": "live", "name": "public"},
                                               host=f"127.0.0.1:{ADMIN_PORT}")
                        assert status == 403, (source.name, path, method, status, body)
                status, body = request(ADMIN_PORT, "/media/api/v1/streams", "POST",
                                       {"application": "live", "name": "surface"})
                assert status in (200, 201), (status, body)
                status, body = request(ADMIN_PORT, "/media/api/v1/streams")
                assert status == 200, (status, body)
                streams = json.loads(body)
                assert streams["count"] == 1, streams
                assert streams["streams"][0]["name"] == "surface", streams
                status, body = request(ADMIN_PORT, "/")
                assert status == 403, (status, body)
                if "location /hls/" in config:
                    status, body = request(PUBLIC_PORT, "/hls/surface.m3u8")
                    assert status == 200 and body == PLAYLIST, (status, body)
                if "location /rec/" in config:
                    status, body = request(PUBLIC_PORT, "/rec/surface.ts")
                    assert status == 200 and body == b"recording surface\n", (status, body)
            except Exception:
                print((run / "startup.log").read_text(errors="replace"))
                if (run / "logs/error.log").exists():
                    print((run / "logs/error.log").read_text(errors="replace"))
                raise
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGQUIT)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        raise AssertionError("nginx failed to stop after SIGQUIT")
        assert process.returncode == 0, process.returncode
        print(f"protected deployment surface: {source.relative_to(ROOT)} ok")


def main():
    if not NGINX.is_file() or not os.access(NGINX, os.X_OK):
        raise SystemExit("set NGINX_BIN to a built nginx-media binary, or run make nginx")
    if PUBLIC_PORT == ADMIN_PORT:
        raise SystemExit("PUBLIC_PORT and ADMIN_PORT must differ")
    for source in [ROOT / "container/nginx.conf", *sorted((ROOT / "container/examples").glob("*.conf"))]:
        check_config(source)


if __name__ == "__main__":
    main()

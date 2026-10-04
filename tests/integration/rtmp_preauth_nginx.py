#!/usr/bin/env python3
"""An unadmitted RTMP connection is held to command-sized messages and a deadline.

Before a publish key is accepted (or a play starts) the reader refuses a message
larger than 64 KiB and the connection's unfinished messages together may not
exceed 64 KiB.  After a key is accepted the media bounds apply.
"""

import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
NGINX = Path(os.environ.get("NGINX_BIN", ROOT / ".build/nginx-install/sbin/nginx"))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def connect(port):
    """TCP connect and complete the plain RTMP handshake."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=3)
    sock.sendall(b"\x03" + b"\x00" * 1536)
    received = b""
    while len(received) < 1 + 1536 * 2:
        chunk = sock.recv(4096)
        assert chunk, "server closed during the handshake"
        received += chunk
    sock.sendall(b"\x00" * 1536)
    return sock


def message_header(csid, length, msg_type=20):
    """fmt 0 chunk header: timestamp 0, length, type, stream 0."""
    return (bytes([csid]) + b"\x00\x00\x00" + length.to_bytes(3, "big")
            + bytes([msg_type]) + b"\x00\x00\x00\x00")


def closed_within(sock, seconds):
    sock.settimeout(seconds)
    try:
        while True:
            data = sock.recv(4096)
            if not data:
                return True
    except socket.timeout:
        return False
    except ConnectionResetError:
        return True


def main():
    (ROOT / ".build").mkdir(exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="rtmp-preauth-", dir=ROOT / ".build"))
    (run / "logs").mkdir()
    port = free_port()
    (run / "nginx.conf").write_text(f"""worker_processes 1;
daemon off;
error_log logs/error.log info;
pid logs/nginx.pid;
events {{ worker_connections 256; }}
media_ingest_secret {run}/ingest.secret;
media_rtmp_listen 127.0.0.1:{port};
http {{ access_log off; }}
""")
    log_path = run / "logs/error.log"
    with (run / "nginx.out").open("w") as out:
        nginx = subprocess.Popen([str(NGINX), "-p", str(run), "-c",
                                  str(run / "nginx.conf")], stdout=out, stderr=out)
        try:
            deadline = time.monotonic() + 10
            while "rtmp" not in (log_path.read_text() if log_path.exists() else ""):
                assert time.monotonic() < deadline, "rtmp listener never started"
                time.sleep(0.05)
            time.sleep(0.3)

            # a peer that completes the handshake and then says nothing is
            # dropped at the deadline (checked last, while the rest run)
            idle = connect(port)
            idle_since = time.monotonic()

            # a command-sized message is simply waited for
            sock = connect(port)
            sock.sendall(message_header(3, 4000) + b"\x00" * 128)
            assert not closed_within(sock, 1.0), \
                "a 4000-byte command message was refused"
            sock.close()

            # a message past 64 KiB is refused before it is allocated
            sock = connect(port)
            sock.sendall(message_header(3, 100000) + b"\x00" * 128)
            assert closed_within(sock, 3.0), \
                "a 100000-byte message from an unadmitted peer was accepted"
            sock.close()
            print("oversized pre-admission message refused")

            # many unfinished messages, each small, are bounded together
            sock = connect(port)
            sock.sendall(b"".join(message_header(csid, 30000) + b"\x00" * 128
                                  for csid in range(3, 9)))
            assert closed_within(sock, 3.0), \
                "unfinished messages on six chunk streams (180000 bytes) were held"
            sock.close()
            print("aggregate pre-admission buffering bounded")

            remaining = 40 - (time.monotonic() - idle_since)
            assert closed_within(idle, max(remaining, 1.0)), \
                "a silent unadmitted connection was never dropped"
            print("silent pre-admission connection dropped at the deadline")
        finally:
            if nginx.poll() is None:
                nginx.send_signal(signal.SIGTERM)
                try:
                    nginx.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    nginx.kill()
                    nginx.wait(timeout=2)
    print(f"rtmp preauth ok ({run})")


if __name__ == "__main__":
    main()

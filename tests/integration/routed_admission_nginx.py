#!/usr/bin/env python3
"""Production nginx: 32 idle routed publishers, refusal, and close/reuse.

No media payloads or patched capacity constants: three explicit SRT listeners
land on workers 0..2, while the provisioned program hashes to worker 3.
"""

import http.client
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import struct
import tempfile
import time

from routed_sources_nginx import await_condition, fnv, free_port
from srt_admission_nginx import Publishers

ROOT = Path(__file__).resolve().parents[2]
NGINX = Path(os.environ.get("NGINX_BIN", ROOT / ".build/nginx-install/sbin/nginx")).resolve()

def amf_string(value):
    data = value.encode()
    return b"\x02" + len(data).to_bytes(2, "big") + data


class RTMP:
    """Idle publisher speaking the real handshake, AMF0 and chunk protocol."""
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=4)
        self.chunk_size = 128
        self.streams = {}
        self.sock.sendall(b"\x03" + b"\x00" * 1536)
        self.exact(3073)
        self.sock.sendall(b"\x00" * 1536)

    def exact(self, length):
        result = bytearray()
        while len(result) < length:
            data = self.sock.recv(length - len(result))
            if not data:
                raise EOFError
            result.extend(data)
        return bytes(result)

    def command(self, name, transaction, values, stream=0):
        body = amf_string(name) + b"\x00" + struct.pack(">d", transaction) + values
        header = (b"\x03\x00\x00\x00" + len(body).to_bytes(3, "big")
                  + b"\x14" + stream.to_bytes(4, "little"))
        self.sock.sendall(header + b"\xc3".join(body[i:i + 128] for i in range(0, len(body), 128)))

    def publish(self, key):
        self.command("connect", 1, b"\x03\x00\x03app" + amf_string("live") + b"\x00\x00\x09")
        self.command("createStream", 2, b"\x05")
        self.command("publish", 0, b"\x05" + amf_string(key) + amf_string("live"), stream=1)

    def message(self):
        while True:
            basic = self.exact(1)[0]
            fmt, csid = basic >> 6, basic & 63
            if csid == 0:
                csid = 64 + self.exact(1)[0]
            elif csid == 1:
                data = self.exact(2)
                csid = 64 + data[0] + 256 * data[1]
            state = self.streams.get(csid)
            if fmt == 0:
                header = self.exact(11)
                state = {"length": int.from_bytes(header[3:6], "big"),
                         "type": header[6], "extended": header[:3] == b"\xff\xff\xff",
                         "body": bytearray()}
                self.streams[csid] = state
            else:
                assert state is not None, "RTMP compressed header without predecessor"
                if fmt in (1, 2):
                    header = self.exact(7 if fmt == 1 else 3)
                    state["extended"] = header[:3] == b"\xff\xff\xff"
                    if fmt == 1:
                        state["length"] = int.from_bytes(header[3:6], "big")
                        state["type"] = header[6]
                    state["body"] = bytearray()
                elif len(state["body"]) == state["length"]:
                    state["body"] = bytearray()
            if state["extended"]:
                self.exact(4)
            state["body"].extend(self.exact(min(self.chunk_size, state["length"] - len(state["body"]))))
            if len(state["body"]) == state["length"]:
                body = bytes(state["body"])
                if state["type"] == 1:
                    self.chunk_size = int.from_bytes(body, "big") & 0x7fffffff
                return body

    def decision(self):
        deadline = time.monotonic() + 5
        try:
            while time.monotonic() < deadline:
                body = self.message()
                if b"NetStream.Publish.Start" in body:
                    return "accepted"
                if b"NetStream.Publish.BadName" in body:
                    return "rejected"
        except (EOFError, ConnectionResetError):
            return "closed"
        raise AssertionError("RTMP owner admission did not end within five seconds")

    def close(self):
        self.sock.close()


def accepted_by(client, pid, port):
    """Identify the accepted TCP socket before publishing, not from source text."""
    remote_port = client.sock.getsockname()[1]
    inode = None
    for line in Path("/proc/net/tcp").read_text().splitlines()[1:]:
        fields = line.split()
        if (int(fields[1].split(":")[1], 16) == port
                and int(fields[2].split(":")[1], 16) == remote_port
                and fields[3] == "01"):
            inode = fields[9]
            break
    assert inode is not None, "accepted RTMP socket disappeared"
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            if os.readlink(fd) == f"socket:[{inode}]":
                return True
        except FileNotFoundError:
            pass
    return False



def main():
    (ROOT / ".build").mkdir(exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="routed-admission-", dir=ROOT / ".build"))
    (run / "logs").mkdir()
    helper = run / "idle-publishers"
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "srt"], text=True))
    libs = shlex.split(subprocess.check_output(["pkg-config", "--libs", "srt"], text=True))
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-O2", "-Wall", "-Wextra",
                    "-Werror", "-std=c2x", *flags,
                    str(ROOT / "tests/integration/srt_idle_publishers.c"),
                    "-o", str(helper), *libs], check=True)
    ports = set()
    while len(ports) < 3:
        ports.add(free_port(socket.SOCK_DGRAM))
    ports = sorted(ports)
    api_port = free_port(socket.SOCK_STREAM)
    rtmp_port = free_port(socket.SOCK_STREAM)
    while rtmp_port == api_port:
        rtmp_port = free_port(socket.SOCK_STREAM)
    name = next(f"capacity{i}" for i in range(100) if fnv(f"live/capacity{i}") % 4 == 3)
    stream_path = f"/streams/live/{name}"
    config = run / "nginx.conf"
    log_path = run / "logs/error.log"
    listeners = "\n".join(f"media_srt_listen 127.0.0.1:{port};" for port in ports)
    config.write_text(f"""worker_processes 4;
daemon off;
error_log logs/error.log info;
pid logs/nginx.pid;
events {{ worker_connections 512; }}
{listeners}
media_ingest_secret {run}/ingest.secret;
media_rtmp_listen 127.0.0.1:{rtmp_port};
http {{
    access_log off;
    keepalive_requests 10000;
    keepalive_timeout 60s;
    server {{
        listen 127.0.0.1:{api_port};
        location /media/api/ {{ media_api; }}
    }}
}}
""")

    def log_text():
        return log_path.read_text() if log_path.exists() else ""

    def request(path, method="GET", body=None, conn=None, expected=(200, 201)):
        fresh = conn is None
        conn = conn or http.client.HTTPConnection("127.0.0.1", api_port, timeout=3)
        try:
            conn.request(method, "/media/api/v1" + path,
                         body=None if body is None else json.dumps(body),
                         headers={"Content-Type": "application/json"})
            response = conn.getresponse()
            data = response.read()
            assert response.status in expected, (method, path, response.status, data)
            return data.decode() if path == "/metrics" else json.loads(data)
        finally:
            if fresh:
                conn.close()

    def has_source(path, source_id, conn=None):
        try:
            state = request(path, conn=conn or owner)
        except AssertionError as error:
            if isinstance(error.args[0], tuple) and error.args[0][2] == 404:
                return False
            raise
        return any(source["id"] == source_id for source in state["sources"])

    callers = []
    rtmp_callers = []
    owner = replica = None
    stopped_owner = None
    stopped_peer = None
    repair_peer = None
    with (run / "nginx.out").open("w") as output, (run / "callers.log").open("w") as caller_log:
        nginx = subprocess.Popen([str(NGINX), "-p", str(run), "-c", str(config)],
                                 stdout=output, stderr=output)
        try:
            await_condition("three SRT listeners", lambda: log_text().count("srt listener ready") == 3)
            pinned = {}
            for _ in range(100):
                conn = http.client.HTTPConnection("127.0.0.1", api_port, timeout=3)
                match = re.search(r'nginx_media_worker_info\{worker="(\d+)",pid="(\d+)"\}',
                                  request("/metrics", conn=conn))
                assert match, "worker identity missing"
                worker, pid = map(int, match.groups())
                if worker in (0, 3) and worker not in pinned:
                    pinned[worker] = (conn, pid)
                else:
                    conn.close()
                if len(pinned) == 2:
                    break
            assert len(pinned) == 2, "could not pin owner and replica APIs"
            owner, owner_pid = pinned[3]
            replica, ingress_pid = pinned[0]
            request("/streams", "POST", {"application": "live", "name": name})
            keys = [request(stream_path + "/sources", "POST",
                            {"id": f"source{i}", "type": "srt"})["key"] for i in range(34)]

            def up_sources():
                return {source["id"] for source in request(stream_path, conn=owner)["sources"]
                        if source["evidence"] & 1}

            def pending_srt(port, key):
                nonlocal stopped_owner
                # libsrt can lose a shutdown delivered during its handshake.
                # Hold owner feedback until the real caller reports connected.
                os.kill(owner_pid, signal.SIGSTOP)
                stopped_owner = owner_pid
                try:
                    caller = Publishers(helper, port, [key], caller_log)
                    callers.append(caller)
                    return caller
                finally:
                    os.kill(owner_pid, signal.SIGCONT)
                    stopped_owner = None

            for index in range(32):
                caller = Publishers(helper, ports[index // 16], [keys[index]], caller_log)
                callers.append(caller)
                await_condition(f"owner accepted idle routed source {index}",
                                lambda index=index: f"source{index}" in up_sources())
                assert caller.command("alive") == "CONNECTED 1"
            assert len(up_sources()) == 32, "not all 32 owner slots were accepted"
            rejected = pending_srt(ports[2], keys[32])
            assert rejected.command("closed") == "CONNECTED 0", \
                "33rd routed publisher remained connected after owner capacity refusal"
            assert up_sources() == {f"source{i}" for i in range(32)}, \
                "rejected admission mutated accepted source health"
            print("32 idle routed SRT publishers accepted; 33rd publisher disconnected")

            callers[0].stop()
            await_condition("owner releases closed routed slot", lambda: "source0" not in up_sources())
            replacement = Publishers(helper, ports[2], [keys[33]], caller_log)
            callers.append(replacement)
            await_condition("closed owner slot is reused", lambda: "source33" in up_sources())
            assert replacement.command("alive") == "CONNECTED 1"
            assert len(up_sources()) == 32
            assert all(request(stream_path + f"/sources/source{i}/key")["key"] == keys[i]
                       for i in (0, 32, 33)), "capacity refusal/close changed provisioned credentials"
            print(f"close/reuse and provisioned credentials passed ({run})")

            # Current-cycle master copies keep the socketpair open after a
            # worker is killed.  Owner cleanup must use generation-local peer
            # identity, not wait for an EOF which those copies prevent.
            os.kill(ingress_pid, signal.SIGKILL)
            replica.close()
            replica = None
            survivors = {f"source{i}" for i in range(16, 32)} | {"source33"}
            await_condition("killed current-generation ingress releases its owner slots",
                            lambda: up_sources() == survivors)
            reuse = Publishers(helper, ports[2], [keys[0]], caller_log)
            callers.append(reuse)
            await_condition("killed ingress capacity is reused by surviving peer",
                            lambda: up_sources() == survivors | {"source0"})
            assert reuse.command("alive") == "CONNECTED 1"
            await_condition("nginx respawns killed ingress in the same generation", lambda:
                            len(re.findall(r"media: worker 0 routing ready", log_text())) == 2)
            respawn_pid = int(re.findall(r"\] (\d+)#\d+: media: worker 0 routing ready",
                                        log_text())[-1])
            assert respawn_pid != ingress_pid
            for _ in range(100):
                conn = http.client.HTTPConnection("127.0.0.1", api_port, timeout=3)
                if re.search(rf'nginx_media_worker_info\{{worker="0",pid="{respawn_pid}"\}}',
                             request("/metrics", conn=conn)):
                    replica = conn
                    break
                conn.close()
            assert replica is not None, "respawned replica API unavailable"
            print(f"current-generation SIGKILL released 15 owner slots; reused capacity; "
                  f"ingress PIDs {ingress_pid}->{respawn_pid}")

            for caller in callers:
                caller.stop()
            await_condition("all idle SRT routed slots close", lambda: not up_sources())

            def rtmp_connect(local=False):
                for _ in range(100):
                    client = RTMP(rtmp_port)
                    if accepted_by(client, owner_pid, rtmp_port) == local:
                        rtmp_callers.append(client)
                        return client
                    client.close()
                raise AssertionError("RTMP connections did not reach requested worker")

            # A respawn starts with an empty registry; replay the program from
            # the owner, then provision RTMP sources there in socket order.
            request("/streams", "POST", {"application": "live", "name": name}, conn=owner)
            rtmp_keys = [request(stream_path + "/sources", "POST",
                                 {"id": f"rtmp{i}", "type": "rtmp"}, conn=owner)["key"] for i in range(34)]
            await_condition("respawned replica has routed RTMP credentials",
                            lambda: has_source(stream_path, "rtmp33", replica))
            for index in range(32):
                client = rtmp_connect()
                client.publish(rtmp_keys[index])
                assert client.decision() == "accepted", f"routed RTMP source {index} refused"
                assert f"rtmp{index}" in up_sources(), "Publish.Start preceded owner acceptance"
            client = rtmp_connect()
            client.publish(rtmp_keys[32])
            assert client.decision() in ("rejected", "closed"), \
                "33rd routed RTMP publisher received Publish.Start"
            assert "rtmp32" not in up_sources(), "RTMP refusal mutated source health"
            rtmp_callers[0].close()
            await_condition("RTMP close releases owner slot", lambda: "rtmp0" not in up_sources())
            client = rtmp_connect()
            client.publish(rtmp_keys[33])
            assert client.decision() == "accepted"
            assert len(up_sources()) == 32
            print("32 idle routed RTMP publishers accepted; 33rd refused without Publish.Start; slot reused")
            for client in rtmp_callers:
                client.close()
            await_condition("all RTMP owner slots close", lambda: not up_sources())

            # An owner that cannot answer must not retain idle admission forever.
            client = rtmp_connect()
            os.kill(owner_pid, signal.SIGSTOP)
            stopped_owner = owner_pid
            try:
                client.publish(rtmp_keys[32])
                silent = Publishers(helper, ports[2], [keys[32]], caller_log)
                callers.append(silent)
                assert client.decision() == "closed", "missing RTMP feedback acknowledged publication"
                assert silent.command("closed") == "CONNECTED 0", "missing SRT feedback retained idle admission"
            finally:
                os.kill(owner_pid, signal.SIGCONT)
                stopped_owner = None
            silent.stop()
            await_condition("late OPEN/CLOSE leaves no owner source active", lambda: not up_sources())
            print("missing owner feedback expires RTMP and SRT admission without payload")

            # Drive real SOCK_SEQPACKET backpressure, then fill the finite
            # graph repair journal using one model object (no quota pressure).
            peer_pid = int(re.findall(r"\] (\d+)#\d+: media: worker 1 routing ready",
                                      log_text())[-1])
            for _ in range(100):
                conn = http.client.HTTPConnection("127.0.0.1", api_port, timeout=3)
                if re.search(rf'nginx_media_worker_info\{{worker="1",pid="{peer_pid}"\}}',
                             request("/metrics", conn=conn)):
                    repair_peer = conn
                    break
                conn.close()
            assert repair_peer is not None, "repair peer API unavailable"

            def desired_source(conn, source_id):
                stream = next(stream for stream in request("/desired", conn=conn)["streams"]
                              if stream["application"] == "live" and stream["name"] == name)
                source = next(source for source in stream["sources"] if source["id"] == source_id)
                return {key: source.get(key) for key in
                        ("id", "type", "priority", "path", "revision", "key_print", "enabled")}

            repair_path = "/" + "p" * 3990
            request("/desired", "PUT", {"streams": [{
                "application": "live", "name": name,
                "sources": [{"id": "rtmp32", "type": "rtmp", "path": repair_path}],
            }]}, conn=owner)
            await_condition("path metadata reaches peer before backpressure", lambda:
                            desired_source(repair_peer, "rtmp32")["path"] == repair_path)
            reader_directory = run / "queued-reader"
            reader_directory.mkdir()
            os.kill(peer_pid, signal.SIGSTOP)
            stopped_peer = peer_pid
            accepted_updates = 0
            previous = desired_source(owner, "rtmp32")
            try:
                for index in range(4096):
                    enabled = bool(index % 2)
                    verb = "enable" if enabled else "disable"
                    result = request(stream_path + "/sources/rtmp32/" + verb,
                                     "POST", {}, conn=owner, expected=(200, 503))
                    if "error" in result:
                        assert result["error"] == "graph_admission_failed", result
                        assert desired_source(owner, "rtmp32") == previous, \
                            "repair journal refusal mutated source priority/path/credentials"
                        break
                    accepted_updates += 1
                    previous = desired_source(owner, "rtmp32")
                    assert previous["enabled"] == enabled and previous["path"] == repair_path, \
                        "accepted graph operation lost enabled state or retained path"
                    if accepted_updates == 100:
                        reader_body = {"id": "queued-reader", "type": "hls_push",
                                       "priority": 901, "path": str(reader_directory)}
                        request(stream_path + "/sources", "POST", reader_body,
                                conn=owner, expected=(201,))
                        reader = desired_source(owner, "queued-reader")
                        assert reader["priority"] == 901 and reader["path"] == str(reader_directory), \
                            "accepted reader creation lost its local priority/path"
                else:
                    raise AssertionError("finite graph repair journal never refused saturated commands")
                assert accepted_updates > 0, "graph repair refused before socket backpressure"
                result = request(stream_path + "/sources", "POST",
                                 {"id": "journal-refused", "type": "rtmp"}, conn=owner, expected=(503,))
                assert result["error"] == "graph_admission_failed", result
                assert not has_source(stream_path, "journal-refused"), \
                    "journal refusal left a newly admitted source behind"
            finally:
                os.kill(peer_pid, signal.SIGCONT)
                stopped_peer = None
            await_condition("resumed peer receives final accepted graph update", lambda:
                            desired_source(repair_peer, "rtmp32") == previous, seconds=20)
            await_condition("repair capacity admits commands after resume", lambda:
                            "error" not in request(stream_path + "/sources", "POST",
                                                  {"id": "journal-refused", "type": "rtmp"},
                                                  conn=owner, expected=(200, 201, 503)), seconds=20)
            print(f"real socket/repair journal saturation accepted {accepted_updates} updates, "
                  "refused atomically, repaired and reused capacity")

            # Fill 256 real records within the hard per-generation model cap.
            # Keeping the old direct publishers across a same-count reload
            # gives the new generation room for the refused 257th directory
            # identity, without changing any production capacity constant.
            request(stream_path, "DELETE")
            names = [f"directory{i}" for i in range(2000)
                     if fnv(f"live/directory{i}") % 4 == 3][:257]
            directory_keys = []
            directory_callers = []
            for index, stream_name in enumerate(names[:256], 1):
                path = f"/streams/live/{stream_name}"
                created = request("/streams", "POST",
                                  {"application": "live", "name": stream_name}, conn=replica)
                credential = request(path + "/sources", "POST",
                                     {"id": "camera", "type": "rtmp"}, conn=replica)
                directory_keys.append(credential["key"])
                try:
                    await_condition(f"owner has provisioned directory credential {path} ({index}/256)",
                                    lambda path=path: has_source(path, "camera"))
                except AssertionError as error:
                    snapshots = {"path": path, "index": index, "created": created,
                                 "credential": {key: value for key, value in credential.items()
                                                if key != "key"}}
                    for label, conn in (("owner", owner), ("replica", replica)):
                        desired = request("/desired", conn=conn)["streams"]
                        snapshots[label] = {
                            "stream": request(path, conn=conn, expected=(200, 404)),
                            "desired_count": len(desired),
                            "desired_stream": next((stream for stream in desired
                                                    if stream["application"] == "live"
                                                    and stream["name"] == stream_name), None),
                            "prior_stream": next((stream for stream in desired
                                                  if stream["application"] == "live"
                                                  and stream["name"] == name), None),
                            "metrics": [line for line in request("/metrics", conn=conn).splitlines()
                                        if line.startswith(("nginx_media_worker_info{",
                                                            "nginx_media_streams_draining "))],
                        }
                    raise AssertionError(f"{error}; state={json.dumps(snapshots, sort_keys=True)}") from error
            for key in directory_keys:
                client = rtmp_connect(local=True)
                directory_callers.append(client)
                client.publish(key)
                assert client.decision() == "accepted", "directory refused before 256 owner records"

            old_owner_pid = owner_pid
            owner.close()
            replica.close()
            owner = replica = None
            reload_at = time.monotonic()
            nginx.send_signal(signal.SIGHUP)
            await_condition("same-count reload starts new generation", lambda:
                            len(re.findall(r"media: worker 3 routing ready", log_text())) == 2)
            new_owner_pid = int(re.findall(r"\] (\d+)#\d+: media: worker 3 routing ready",
                                          log_text())[-1])
            new_replica_pid = int(re.findall(r"\] (\d+)#\d+: media: worker 0 routing ready",
                                            log_text())[-1])
            assert new_owner_pid != old_owner_pid
            await_condition("new SRT listeners ready", lambda:
                            log_text().count("srt listener ready") == 6)
            pinned = {}
            for _ in range(100):
                conn = http.client.HTTPConnection("127.0.0.1", api_port, timeout=3)
                match = re.search(r'nginx_media_worker_info\{worker="(\d+)",pid="(\d+)"\}',
                                  request("/metrics", conn=conn))
                assert match
                worker, pid = map(int, match.groups())
                if ((worker, pid) in ((3, new_owner_pid), (0, new_replica_pid))
                        and worker not in pinned):
                    pinned[worker] = conn
                else:
                    conn.close()
                if len(pinned) == 2:
                    break
            assert len(pinned) == 2, "new-generation API connections unavailable"
            owner, replica, owner_pid = pinned[3], pinned[0], new_owner_pid
            overflow_path = f"/streams/live/{names[-1]}"
            request("/streams", "POST", {"application": "live", "name": names[-1]}, conn=replica)
            overflow_key = request(overflow_path + "/sources", "POST",
                                   {"id": "camera", "type": "rtmp"}, conn=replica)["key"]
            srt_key = request(overflow_path + "/sources", "POST",
                              {"id": "srt", "type": "srt"}, conn=replica)["key"]
            await_condition("new owner has both overflow credentials",
                            lambda: has_source(overflow_path, "camera") and has_source(overflow_path, "srt"))
            for local in (True, False):
                client = rtmp_connect(local=local)
                client.publish(overflow_key)
                assert client.decision() in ("rejected", "closed"), \
                    "publisher acknowledged after owner-directory exhaustion"
                client.close()
            refused = pending_srt(ports[2], srt_key)
            assert refused.command("closed") == "CONNECTED 0", "directory-full routed SRT admission leaked"
            refusal_elapsed = time.monotonic() - reload_at
            assert refusal_elapsed < 10, "directory refusal was not observed before the real stale deadline"
            assert not any(source["evidence"] & 1 for source in
                           request(overflow_path, conn=owner)["sources"]), \
                "owner claim refusal mutated source health"
            assert Path(f"/proc/{old_owner_pid}").exists(), "old publisher generation stopped draining"
            print(f"256 old-generation owner records: direct/routed RTMP and SRT refused after "
                  f"{refusal_elapsed:.3f}s; owner PIDs {old_owner_pid}->{owner_pid}")

            # Epoch fencing prevents old owners extending directory liveness.
            # Natural 10-second reclamation must recover admission, not a
            # forced old-generation release or a patched capacity constant.
            recovered = None
            deadline = reload_at + 18
            while time.monotonic() < deadline:
                client = rtmp_connect(local=True)
                client.publish(overflow_key)
                if client.decision() == "accepted":
                    recovered = client
                    break
                client.close()
                time.sleep(0.1)
            assert recovered is not None, "stale old-generation owner records were never reclaimed"
            recovery_elapsed = time.monotonic() - reload_at
            assert recovery_elapsed >= 8, "old-generation heartbeat/release bypassed epoch fencing"
            print(f"directory admission recovered by natural stale reclaim after {recovery_elapsed:.3f}s ({run})")

            def reclaimed_progress():
                local = request(overflow_path, conn=owner)
                remote = request(overflow_path, conn=replica)
                return local["generation"] > 0 and remote["generation"] == local["generation"]

            await_condition("reclaimed owner record publishes current-generation progress",
                            reclaimed_progress)
            for client in directory_callers:
                client.close()
            await_condition("old publisher owner finishes draining",
                            lambda: not Path(f"/proc/{old_owner_pid}").exists())
            assert reclaimed_progress(), "old owner exit removed the current owner record"
            assert any(source["id"] == "camera" and source["evidence"] & 1
                       for source in request(overflow_path, conn=owner)["sources"]), \
                "old publisher close changed the current publisher's health"
            print("old publisher generation closed; reclaimed current owner metadata survived")
        finally:
            if stopped_owner is not None:
                os.kill(stopped_owner, signal.SIGCONT)
            if stopped_peer is not None:
                os.kill(stopped_peer, signal.SIGCONT)
            if repair_peer is not None:
                repair_peer.close()
            for client in rtmp_callers:
                client.close()
            for caller in callers:
                caller.stop(check=False)
            if owner is not None:
                owner.close()
            if replica is not None:
                replica.close()
            if nginx.poll() is None:
                nginx.send_signal(signal.SIGTERM)
                try:
                    nginx.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    nginx.kill()
                    nginx.wait(timeout=2)


if __name__ == "__main__":
    main()

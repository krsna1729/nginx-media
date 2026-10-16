#!/usr/bin/env python3
"""SSH receiver transport. Paths and process identities belong to one fresh run."""
import argparse
import base64
import ctypes
import json
import os
from pathlib import Path
import shlex
import select
import signal
import subprocess
import sys
import threading
import time

# Each side waits longer than the work the other may legitimately do first.
READY_TIMEOUT, RELEASE_TIMEOUT, CAPTURE_FRAME_TIMEOUT = 30.0, 10.0, 30.0
COMMAND_TIMEOUT, REPLY_TIMEOUT, CALL_TIMEOUT = 60.0, 90.0, 300.0

# Match the native receiver control's Linux binding, including SIGKILL.
LIBC = ctypes.CDLL(None, use_errno=True)
LIBC.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                      ctypes.c_ulong, ctypes.c_ulong]
LIBC.prctl.restype = ctypes.c_int


def owner_binding(parent=None):
    parent = os.getpid() if parent is None else parent
    def bind():
        if LIBC.prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
            raise OSError(ctypes.get_errno(), "PR_SET_PDEATHSIG")
        if os.getppid() != parent:
            os.kill(os.getpid(), signal.SIGKILL)
    return bind


def stat_identity(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return {"pid": int(pid), "start": int(fields[19]), "pgid": os.getpgid(int(pid))}


def owned(path, token):
    identity = json.loads(Path(path).read_text())
    if identity["token"] != token:
        raise ValueError("receiver ownership token mismatch")
    actual = stat_identity(identity["pid"])
    if any(actual[key] != identity[key] for key in actual):
        raise ValueError("receiver PID/group was reused")
    if actual["pid"] != actual["pgid"]:
        raise ValueError("receiver does not own its process group")
    return identity


def publish(path, value):
    temporary = path + ".tmp"
    Path(temporary).write_text(json.dumps(value))
    os.replace(temporary, path)


def wait_file(path, timeout=10.0):
    deadline = time.monotonic() + timeout
    while not Path(path).is_file():
        if time.monotonic() >= deadline:
            raise ValueError(f"receiver acknowledgment timed out: {path}")
        time.sleep(0.001)
    return Path(path).read_bytes()


def signal_owned(path, token, signo):
    identity = owned(path, token)
    signo = signo.lstrip("-")
    value = 0 if signo == "0" else getattr(signal, "SIG" + signo)
    os.killpg(identity["pgid"], value)


def read_frame(stream, timeout=10.0):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([stream], [], [], remaining)[0]:
            raise ValueError("receiver capture handshake timed out")
        chunk = os.read(stream.fileno(), 65536)
        if not chunk:
            raise ValueError("receiver capture peer disconnected")
        data.extend(chunk)
        if b"\n" in chunk:
            return json.loads(data)


def release_capture(request):
    if request.get("prepared"):
        print(json.dumps({"ready": True}), flush=True)
        if read_frame(sys.stdin, CAPTURE_FRAME_TIMEOUT) != {"capture": True}:
            raise ValueError("invalid receiver capture release")
    # A controller barrier may have parked us; recheck ownership at release.
    owned(request["path"], request["token"])


def server(request):
    action = request["action"]
    path = request.get("path")
    if action == "start":
        if Path(path).exists():
            raise ValueError("receiver identity already exists")
        child = subprocess.Popen(request["argv"], start_new_session=True,
                                 stdin=subprocess.DEVNULL)
        try:
            identity = stat_identity(child.pid)
            identity["token"] = request["token"]
            publish(path, identity)
        except BaseException:
            child.kill()
            child.wait()
            raise
        def disconnect(_signo, _frame):
            try:
                owned(path, request["token"])
                os.killpg(child.pid, signal.SIGKILL)
            except (OSError, ValueError):
                pass
        for signo in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT):
            signal.signal(signo, disconnect)
        def watch_owner():
            while os.read(sys.stdin.fileno(), 1):
                pass
            disconnect(None, None)
        threading.Thread(target=watch_owner, daemon=True).start()
        try:
            return child.wait()
        finally:
            # A killed SSH client must not leave the receiver alive.
            disconnect(None, None)
    elif action == "signal":
        signal_owned(path, request["token"], request["signal"])
    elif action == "snapshot":
        owned(path, request["token"])
        quiesce = request.get("quiesce")
        if quiesce:
            owned(quiesce["path"], quiesce["token"])
            Path(quiesce["ack"]).unlink(missing_ok=True)
            signal_owned(quiesce["path"], quiesce["token"], "USR2")
            wait_file(quiesce["ack"])
        source = request["source"]
        output = Path(request["output"])
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + ".snapshot")
        release_capture(request)
        Path(source).unlink(missing_ok=True)
        signal_owned(path, request["token"], request["signal"])
        data = wait_file(source)
        temporary.write_bytes(data)
        os.replace(temporary, output)
        print(json.dumps({"data": base64.b64encode(data).decode()}), flush=True)
    elif action == "cpu-boundary":
        owned(path, request["token"])
        directory, phase = Path(request["directory"]), request["phase"]
        if phase not in ("before", "after"):
            raise ValueError("unknown CPU boundary")
        wait_file(directory / f"cpu.ready.{phase}")
        release_capture(request)
        (directory / f"cpu.release.{phase}").touch()
        data = wait_file(directory / f"cpu.captured.{phase}.json")
        print(json.dumps({"data": base64.b64encode(data).decode()}), flush=True)
    elif action == "command-capture":
        owned(path, request["token"])
        argv = request["argv"]
        if not argv or any(not isinstance(value, str) for value in argv):
            raise ValueError("receiver capture command requires argv")
        output = request["output"]
        release_capture(request)
        Path(output).unlink(missing_ok=True)
        result = subprocess.run(argv, stdout=subprocess.DEVNULL, timeout=COMMAND_TIMEOUT,
                                preexec_fn=owner_binding(), check=False)
        if result.returncode:
            raise ValueError(f"receiver capture command failed: {result.returncode}")
        data = wait_file(output)
        print(json.dumps({"data": base64.b64encode(data).decode()}), flush=True)
    elif action == "mkdir":
        Path(path).mkdir(parents=True, exist_ok=True)
    elif action == "remove":
        Path(path).unlink(missing_ok=True)
    elif action == "exists":
        return 0 if Path(path).is_file() else 1
    elif action == "read":
        sys.stdout.buffer.write(Path(path).read_bytes())
    elif action == "write":
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(base64.b64decode(request["data"]))
    elif action == "children":
        identity = owned(path, request["token"])
        children = subprocess.run(["pgrep", "-P", str(identity["pid"])], timeout=10,
                                  capture_output=True, text=True, check=False)
        if children.returncode != 0 or not children.stdout.strip():
            raise ValueError("receiver nginx has no worker")
        print(children.stdout.strip())
    else:
        raise ValueError(f"unknown receiver action {action}")
    return 0


class Transport:
    def __init__(self, config):
        self.config = config
        self.ssh = ["ssh", *config["ssh_options"], config["host"]]

    def mapped(self, value):
        # Loopback validation nests the receiver root below the sender root:
        # never translate a path that is already remote.
        for remote in (self.config["remote_workspace"], self.config["remote_run"],
                       self.config["remote_root"]):
            if value == remote or value.startswith(remote + "/"):
                return value
        for local, remote in ((self.config["local_run"], self.config["remote_run"]),
                              (self.config["local_root"], self.config["remote_root"])):
            if value == local or value.startswith(local + "/"):
                return remote + value[len(local):]
        return value

    def command(self, request):
        payload = base64.b64encode(json.dumps(request).encode()).decode()
        command = ["python3", self.config["remote_root"] +
                   "/tests/bench/capacity_receiver.py", "--serve", payload]
        return [*self.ssh, shlex.join(command)]

    def call(self, request, capture=False):
        return subprocess.run(self.command(request), timeout=CALL_TIMEOUT,
                              stdout=subprocess.PIPE if capture else None, check=False)

    def request(self, action, path, **kwargs):
        return self.call(dict(action=action, path=self.mapped(path), **kwargs))

    def fetch(self, path):
        result = self.call(dict(action="read", path=self.mapped(path)), capture=True)
        if result.returncode != 0:
            return result.returncode
        temporary = path + ".fetch"
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(temporary).write_bytes(result.stdout)
        os.replace(temporary, path)
        return 0

    def capture(self, request, output, ready_file=None, release_file=None):
        if bool(ready_file) != bool(release_file):
            raise ValueError("capture ready/release files must be supplied together")
        request = dict(request, prepared=True)
        process = subprocess.Popen(self.command(request), stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, preexec_fn=owner_binding())
        try:
            if read_frame(process.stdout, READY_TIMEOUT) != {"ready": True}:
                raise ValueError("invalid receiver capture readiness")
            if ready_file:
                publish(ready_file, {"ready": True})
                deadline = time.monotonic() + RELEASE_TIMEOUT
                while not Path(release_file).is_file():
                    if process.poll() is not None:
                        raise ValueError("receiver capture peer exited before release")
                    if time.monotonic() >= deadline:
                        raise ValueError("receiver capture release timed out")
                    time.sleep(0.001)
            # The bracket covers only token dispatch through the native reply.
            begin = time.monotonic_ns()
            process.stdin.write(b'{"capture":true}\n')
            process.stdin.flush()
            reply = read_frame(process.stdout, REPLY_TIMEOUT)
            end = time.monotonic_ns()
            process.stdin.close()
            status = process.wait(timeout=10)
            if status:
                return status
            data = base64.b64decode(reply["data"], validate=True)
            Path(output).parent.mkdir(parents=True, exist_ok=True)
            temporary = output + ".fetch"
            Path(temporary).write_bytes(data)
            os.replace(temporary, output)
            publish(output + ".request.json", {"request_ns": begin, "reply_ns": end})
            return 0
        finally:
            if not process.stdin.closed:
                process.stdin.close()
            process.stdout.close()
            stop(process)


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def client(args):
    transport = Transport(json.loads(Path(args.config).read_text()))
    action, rest = args.action, args.args
    if action in ("start", "tunnel"):
        # Bind to the harness PID, not our parent: the harness may already be gone.
        if args.owner_pid is None or args.owner_pid <= 0:
            raise ValueError(f"{action} requires a positive --owner-pid")
        owner_binding(args.owner_pid)()
    if action == "exec":
        return subprocess.call([*transport.ssh, shlex.join(
            [transport.mapped(value) for value in rest])])
    if action == "start":
        path, *argv = rest
        owner = subprocess.Popen(transport.command(dict(
            action="start", path=transport.mapped(path), token=path,
            argv=[transport.mapped(value) for value in argv])),
            stdin=subprocess.PIPE, preexec_fn=owner_binding())
        try:
            return owner.wait()
        finally:
            owner.stdin.close()
            stop(owner)
    if action == "signal":
        path, signo = rest
        return transport.request("signal", path, token=path, signal=signo).returncode
    if action in ("mkdir", "remove", "exists", "children"):
        return transport.request(action, rest[0], token=rest[0]).returncode
    if action == "path":
        print(transport.mapped(rest[0]))
        return 0
    if action == "fetch":
        return transport.fetch(rest[0])
    if action == "push":
        path = rest[0]
        return transport.request("write", path,
                                 data=base64.b64encode(Path(path).read_bytes()).decode()).returncode
    if action == "snapshot":
        identity, signo, source, output, *quiesce = rest
        request = dict(action=action, path=transport.mapped(identity), token=identity,
                       signal=signo, source=transport.mapped(source),
                       output=transport.mapped(output))
        if quiesce:
            sampler, acknowledgment = quiesce
            request["quiesce"] = dict(path=transport.mapped(sampler), token=sampler,
                                      ack=transport.mapped(acknowledgment))
        return transport.capture(request, output, args.capture_ready_file,
                                 args.capture_release_file)
    if action == "cpu-boundary":
        identity, directory, phase, output = rest
        request = dict(action=action, path=transport.mapped(identity), token=identity,
                       directory=transport.mapped(directory), phase=phase)
        return transport.capture(request, output, args.capture_ready_file,
                                 args.capture_release_file)
    if action == "command-capture":
        identity, output, *argv = rest
        request = dict(action=action, path=transport.mapped(identity), token=identity,
                       output=transport.mapped(output),
                       argv=[transport.mapped(value) for value in argv])
        return transport.capture(request, output, args.capture_ready_file,
                                 args.capture_release_file)
    if action == "collect":
        path = rest[0]
        # Named receiver outputs only: never overwrite local logs or tokens.
        for name in ("srt.csv", "receiver-intervals.csv", "hls-readers.csv",
                     "receiver.cpu.before", "receiver.cpu.after",
                     "receiver.host.before.json", "receiver.host.after.json"):
            target = str(Path(path) / name)
            if transport.request("exists", target).returncode == 0:
                status = transport.fetch(target)
                if status:
                    return status
        return 0
    if action == "tunnel":
        local, remote = rest
        os.execvp("ssh", [*transport.ssh[:-1], "-N", "-o",
                         "ExitOnForwardFailure=yes", "-L",
                         f"127.0.0.1:{local}:127.0.0.1:{remote}", transport.ssh[-1]])
    raise ValueError(f"unknown client action {action}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serve")
    parser.add_argument("--config")
    parser.add_argument("--owner-pid", type=int)
    parser.add_argument("--capture-ready-file")
    parser.add_argument("--capture-release-file")
    parser.add_argument("action", nargs="?")
    parser.add_argument("args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        if bool(args.capture_ready_file) != bool(args.capture_release_file):
            raise ValueError("capture ready/release files must be supplied together")
        status = server(json.loads(base64.b64decode(args.serve))) if args.serve else client(args)
        return 128 - status if status < 0 else status
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print(f"capacity_receiver: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

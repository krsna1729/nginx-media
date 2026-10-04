#!/usr/bin/env python3
"""SSH receiver transport. Paths and process identities belong to one fresh run."""
import argparse
import base64
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys


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


def server(request):
    action = request["action"]
    path = request.get("path")
    if action == "start":
        if Path(path).exists():
            raise ValueError("receiver identity already exists")
        child = subprocess.Popen(request["argv"], start_new_session=True)
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
        try:
            return child.wait()
        finally:
            # The owner waits for the group leader; a killed SSH client must not
            # leave the receiver alive. Signals below validate this identity.
            try:
                owned(path, request["token"])
                os.killpg(child.pid, signal.SIGKILL)
            except (OSError, ValueError):
                pass
    elif action == "signal":
        identity = owned(path, request["token"])
        signo = request["signal"].lstrip("-")
        value = 0 if signo == "0" else getattr(signal, "SIG" + signo)
        os.killpg(identity["pgid"], value)
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
        children = subprocess.run(["pgrep", "-P", str(identity["pid"])],
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
        # Run paths take precedence over the source prefix.
        for local, remote in ((self.config["local_run"], self.config["remote_run"]),
                              (self.config["local_root"], self.config["remote_root"])):
            value = value.replace(local + "/", remote + "/")
            if value == local:
                value = remote
        return value

    def call(self, request, capture=False):
        payload = base64.b64encode(json.dumps(request).encode()).decode()
        command = ["python3", self.config["remote_root"] +
                   "/tests/bench/capacity_receiver.py", "--serve", payload]
        return subprocess.run([*self.ssh, shlex.join(command)],
                              stdout=subprocess.PIPE if capture else None, check=False)

    def request(self, action, path, **kwargs):
        return self.call(dict(action=action, path=self.mapped(path), **kwargs))

    def fetch(self, path):
        result = self.call(dict(action="read", path=self.mapped(path)), capture=True)
        if result.returncode != 0:
            return result.returncode
        temporary = path + ".fetch"
        Path(temporary).write_bytes(result.stdout)
        os.replace(temporary, path)
        return 0


def client(args):
    transport = Transport(json.loads(Path(args.config).read_text()))
    action, rest = args.action, args.args
    if action == "exec":
        return subprocess.call([*transport.ssh, shlex.join(
            [transport.mapped(value) for value in rest])])
    if action == "start":
        path, *argv = rest
        return transport.request("start", path, token=path,
                                 argv=[transport.mapped(value) for value in argv]).returncode
    if action == "signal":
        path, signo = rest
        return transport.request("signal", path, token=path, signal=signo).returncode
    if action in ("mkdir", "remove", "exists", "children"):
        return transport.request(action, rest[0], token=rest[0]).returncode
    if action == "fetch":
        return transport.fetch(rest[0])
    if action == "push":
        path = rest[0]
        return transport.request("write", path,
                                 data=base64.b64encode(Path(path).read_bytes()).decode()).returncode
    if action == "collect":
        path = rest[0]
        # Receiver artifacts never overwrite local diagnostics, sender logs or
        # ownership tokens. Fetch the named receiver outputs only.
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
        return subprocess.call([*transport.ssh[:-1], "-N", "-o",
                                "ExitOnForwardFailure=yes", "-L",
                                f"127.0.0.1:{local}:127.0.0.1:{remote}",
                                transport.ssh[-1]])
    raise ValueError(f"unknown client action {action}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serve")
    parser.add_argument("--config")
    parser.add_argument("action", nargs="?")
    parser.add_argument("args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        status = server(json.loads(base64.b64decode(args.serve))) if args.serve else client(args)
        return 128 - status if status < 0 else status
    except (OSError, ValueError, KeyError) as error:
        print(f"capacity_receiver: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Receiver ownership regression and optional real SSH plumbing smoke.

python3 tests/bench/test_capacity_receiver.py
python3 tests/bench/test_capacity_receiver.py --ssh-config OUT/receiver-config.json
The SSH mode exercises remote Python only; media workloads use the harness.
"""
import base64
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("capacity_receiver", HERE / "capacity_receiver.py")
receiver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receiver)


def serve(request):
    payload = base64.b64encode(json.dumps(request).encode()).decode()
    return subprocess.Popen([sys.executable, str(HERE / "capacity_receiver.py"),
                             "--serve", payload], stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def wait_file(path, process, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if Path(path).is_file():
            return
        if process.poll() is not None:
            raise AssertionError(process.communicate())
        time.sleep(0.02)
    raise AssertionError(f"receiver did not publish {path}")


class Ownership(unittest.TestCase):
    def test_signal_rejects_reused_identity_and_wrong_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            identity = str(Path(directory) / "identity.json")
            stopped = str(Path(directory) / "stopped")
            ready = str(Path(directory) / "ready")
            child_code = ('import pathlib,signal,time; '
                          'signal.signal(signal.SIGTERM, lambda *_: '
                          f'(pathlib.Path({stopped!r}).write_text("stopped"), exit(0))); '
                          f'pathlib.Path({ready!r}).write_text("ready"); time.sleep(60)')
            owner = serve(dict(action="start", path=identity, token="owned",
                               argv=[sys.executable, "-c", child_code]))
            try:
                wait_file(identity, owner)
                wait_file(ready, owner)
                original = json.loads(Path(identity).read_text())
                self.assertNotEqual(original["pid"], owner.pid)
                self.assertEqual(original["pid"], original["pgid"])
                with self.assertRaises(ValueError):
                    receiver.server(dict(action="signal", path=identity,
                                         token="wrong", signal="TERM"))
                forged = dict(original, start=original["start"] + 1)
                Path(identity).write_text(json.dumps(forged))
                with self.assertRaises(ValueError):
                    receiver.server(dict(action="signal", path=identity,
                                         token="owned", signal="TERM"))
                self.assertFalse(Path(stopped).exists())
                Path(identity).write_text(json.dumps(original))
                receiver.server(dict(action="signal", path=identity,
                                     token="owned", signal="TERM"))
                owner.communicate(timeout=5)
                self.assertEqual(owner.returncode, 0)
                self.assertEqual(Path(stopped).read_text(), "stopped")
                with self.assertRaises(OSError):
                    receiver.owned(identity, "owned")
            finally:
                if owner.poll() is None:
                    owner.terminate()
                    owner.communicate(timeout=5)

    def test_existing_identity_cannot_start_another_receiver(self):
        with tempfile.TemporaryDirectory() as directory:
            identity = str(Path(directory) / "identity.json")
            marker = Path(directory) / "unexpected"
            Path(identity).write_text('{}')
            with self.assertRaises(ValueError):
                receiver.server(dict(action="start", path=identity, token="run",
                                     argv=[sys.executable, "-c",
                                           f'open({str(marker)!r}, "w").write("bad")']))
            self.assertFalse(marker.exists())

    def test_all_workloads_are_accepted_and_unknown_mix_rejected(self):
        driver = str(HERE / "capacity_distributed.sh")
        arguments = ["bash", driver, "--receiver", "user@localhost", "--addr", "127.0.0.1",
                     "--sender-addr", "127.0.0.1", "--dry-run"]
        valid = subprocess.run(arguments + ["--mixes", "all"], capture_output=True, text=True)
        self.assertEqual(valid.returncode, 0, valid.stderr)
        invalid = subprocess.run(arguments + ["--mixes", "pure-srt unknown"], capture_output=True, text=True)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertIn("unknown mix unknown", invalid.stderr)


def ssh_smoke(config_path):
    config = json.loads(Path(config_path).read_text())
    transport = receiver.Transport(config)
    run = Path(config["local_run"]) / ("ssh-smoke-" + str(time.monotonic_ns()))
    run.mkdir(parents=True)
    assert transport.request("mkdir", str(run)).returncode == 0
    identity, snapshot = str(run / "identity.json"), str(run / "snapshot")
    remote_snapshot = transport.mapped(snapshot)
    ready = str(run / "ready")
    remote_ready = transport.mapped(ready)
    code = ('import os,pathlib,signal,time; '
            'signal.signal(signal.SIGUSR1,lambda *_: '
            f'pathlib.Path({remote_snapshot!r}).write_text(str(os.getpid()))); '
            'signal.signal(signal.SIGTERM,lambda *_: exit(0)); '
            f'pathlib.Path({remote_ready!r}).write_text("ready"); time.sleep(60)')
    command = [sys.executable, str(HERE / "capacity_receiver.py"), "--config", config_path,
               "start", identity, "python3", "-c", code]
    owner = subprocess.Popen(command)
    try:
        deadline = time.monotonic() + 15
        while transport.request("exists", identity).returncode != 0:
            assert owner.poll() is None
            assert time.monotonic() < deadline
            time.sleep(0.05)
        assert transport.fetch(identity) == 0
        actual = json.loads(Path(identity).read_text())
        assert actual["pid"] == actual["pgid"]
        assert transport.request("signal", identity, token=identity, signal="0").returncode == 0
        while transport.request("exists", ready).returncode != 0:
            assert owner.poll() is None
            assert time.monotonic() < deadline
            time.sleep(0.05)
        assert transport.request("remove", snapshot).returncode == 0
        assert transport.request("exists", snapshot).returncode != 0
        assert transport.request("signal", identity, token=identity, signal="USR1").returncode == 0
        while transport.request("exists", snapshot).returncode != 0:
            assert time.monotonic() < deadline
        assert transport.fetch(snapshot) == 0
        assert int(Path(snapshot).read_text()) == actual["pid"]
        assert transport.request("signal", identity, token=identity, signal="TERM").returncode == 0
        assert owner.wait(timeout=10) == 0
        assert transport.request("signal", identity, token=identity, signal="0").returncode != 0
        print(json.dumps(dict(ssh_process_smoke="pass", receiver_pid=actual["pid"],
                              transport_pid=owner.pid, receiver_group=actual["pgid"],
                              second_physical_host_evidence=False)))
    finally:
        if owner.poll() is None:
            transport.request("signal", identity, token=identity, signal="KILL")
            owner.wait(timeout=10)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--ssh-config":
        ssh_smoke(sys.argv[2])
    else:
        unittest.main()

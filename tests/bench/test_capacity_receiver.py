#!/usr/bin/env python3
"""Receiver ownership regression and optional real SSH plumbing smoke.

python3 tests/bench/test_capacity_receiver.py
python3 tests/bench/test_capacity_receiver.py --ssh-config OUT/receiver-config.json
The SSH mode also kills real launchers and checks native/SSH/tunnel ownership;
media workloads use the harness.
"""
import base64
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
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
                             "--serve", payload], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def wait_file(path, process, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if Path(path).is_file():
            return
        if process.poll() is not None:
            raise AssertionError(process.communicate())
        time.sleep(0.02)
    raise AssertionError(f"receiver did not publish {path}")


def wait_stopped(pid, timeout=5):
    deadline = time.monotonic() + timeout
    while True:
        try:
            state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
        except (FileNotFoundError, ProcessLookupError):
            return
        if state == "Z":
            return
        if time.monotonic() >= deadline:
            raise AssertionError(f"owned process {pid} survived owner death")
        time.sleep(0.01)


def kill_process(pid):
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def launch_clients_owner(config_path, run, local_port, env=None):
    # A real launcher owns both long-lived clients. Its SIGKILL cannot execute
    # Python cleanup, and no signal RPC is sent on its behalf.
    command = [sys.executable, str(HERE / "capacity_receiver.py"),
               "--config", str(config_path)]
    code = (
        "import json,os,pathlib,subprocess,time\n"
        f"command={command!r}+['--owner-pid',str(os.getpid())]\n"
        f"client=subprocess.Popen(command+['start',{str(run / 'identity.json')!r},'sleep','60'])\n"
        f"tunnel=subprocess.Popen(command+['tunnel',{str(local_port)!r},'9'])\n"
        f"path=pathlib.Path({str(run / 'owners.json')!r})\n"
        "temporary=path.with_name(path.name+'.tmp')\n"
        "temporary.write_text(json.dumps({'client':client.pid,'tunnel':tunnel.pid}))\n"
        "os.replace(temporary,path)\n"
        "time.sleep(60)\n")
    return subprocess.Popen([sys.executable, "-c", code], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Ownership(unittest.TestCase):
    def test_nested_receiver_paths_are_mapped_once(self):
        config = {"host": "user@localhost", "ssh_options": [],
                  "local_root": "/sender tree", "local_run": "/sender tree/.build/run",
                  "remote_workspace": "/sender tree/.build/receiver",
                  "remote_root": "/sender tree/.build/receiver/source",
                  "remote_run": "/sender tree/.build/receiver/run"}
        transport = receiver.Transport(config)
        native = config["remote_root"] + "/sbin/nginx"
        self.assertEqual(transport.mapped(native), native)
        parent_run = config["remote_workspace"] + "/run/base-sink"
        self.assertEqual(transport.mapped(parent_run), parent_run)
        self.assertEqual(transport.mapped(config["local_run"] + "/sample.csv"),
                         config["remote_run"] + "/sample.csv")
        self.assertEqual(transport.mapped(config["local_root"] + "/tests/worker.py"),
                         config["remote_root"] + "/tests/worker.py")
        code = f"open({native!r}).read()"
        self.assertEqual(transport.mapped(code), code)

    def test_cpu_barriers_capture_after_release_and_keep_host_brackets(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            roles = run / "roles"
            roles.write_text("")
            command = [sys.executable, str(HERE / "proc_cpu_sampler.py"), "sample",
                       "--sleep", "0.05", "--roles", str(roles),
                       "--before", str(run / "cpu.before"), "--after", str(run / "cpu.after"),
                       "--extended-before", str(run / "host.before.json"),
                       "--extended-after", str(run / "host.after.json"),
                       "--boundary-dir", directory, str(os.getpid())]
            sampler = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                releases = {}
                for phase, snapshot in (("before", "cpu.before"), ("after", "cpu.after")):
                    wait_file(run / f"cpu.ready.{phase}", sampler)
                    self.assertFalse((run / snapshot).exists())
                    releases[phase] = time.monotonic_ns()
                    (run / f"cpu.release.{phase}").touch()
                _, stderr = sampler.communicate(timeout=10)
                self.assertEqual(sampler.returncode, 0, stderr)
                brackets = {}
                for phase in ("before", "after"):
                    header = (run / f"cpu.{phase}").read_text().splitlines()[0]
                    start, end = map(int, header.split("monotonic_ns=")[1].split())
                    self.assertGreaterEqual(start, releases[phase])
                    brackets[phase] = (start, end)
                    host = json.loads((run / f"host.{phase}.json").read_text())
                    self.assertLessEqual(host["capture_start_ns"], host["monotonic_ns"])
                    self.assertLessEqual(host["monotonic_ns"], host["capture_end_ns"])
                    if phase == "before":
                        self.assertLessEqual(host["capture_end_ns"], start)
                    else:
                        self.assertGreaterEqual(host["capture_start_ns"], end)
                self.assertGreater(brackets["after"][0], brackets["before"][1])
            finally:
                if sampler.poll() is None:
                    sampler.terminate()
                    sampler.communicate(timeout=5)

    def test_snapshot_rpc_replaces_stale_acknowledgment_with_native_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            identity, ready = str(run / "identity.json"), str(run / "ready")
            snapshot, copied = str(run / "snapshot.json"), str(run / "copied.json")
            child_code = (
                "import json,os,pathlib,signal,time\n"
                f"snapshot={snapshot!r}\n"
                "def capture(*_):\n"
                "    data=json.dumps({'native_ns':time.monotonic_ns(),'pid':os.getpid()})\n"
                "    pathlib.Path(snapshot+'.tmp').write_text(data)\n"
                "    os.replace(snapshot+'.tmp',snapshot)\n"
                "signal.signal(signal.SIGUSR1,capture)\n"
                f"pathlib.Path({ready!r}).touch()\n"
                "time.sleep(60)\n")
            owner = serve(dict(action="start", path=identity, token="owned",
                               argv=[sys.executable, "-c", child_code]))
            try:
                wait_file(identity, owner)
                wait_file(ready, owner)
                Path(snapshot).write_text('{"native_ns":0,"pid":0}')
                with self.assertRaises(ValueError):
                    receiver.server(dict(action="snapshot", path=identity, token="wrong",
                                         signal="USR1", source=snapshot, output=copied))
                self.assertEqual(json.loads(Path(snapshot).read_text())["native_ns"], 0)
                begin = time.monotonic_ns()
                request = serve(dict(action="snapshot", path=identity, token="owned",
                                     signal="USR1", source=snapshot, output=copied))
                stdout, stderr = request.communicate(timeout=10)
                end = time.monotonic_ns()
                self.assertEqual(request.returncode, 0, stderr)
                captured = json.loads(base64.b64decode(json.loads(stdout)["data"]))
                self.assertGreaterEqual(captured["native_ns"], begin)
                self.assertLessEqual(captured["native_ns"], end)
                self.assertEqual(captured["pid"], json.loads(Path(identity).read_text())["pid"])
            finally:
                if owner.poll() is None:
                    receiver.server(dict(action="signal", path=identity,
                                         token="owned", signal="KILL"))
                owner.communicate(timeout=5)

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
                owner.wait(timeout=5)
                owner.communicate(timeout=5)
                self.assertEqual(owner.returncode, 0)
                self.assertEqual(Path(stopped).read_text(), "stopped")
                with self.assertRaises(OSError):
                    receiver.owned(identity, "owned")
            finally:
                if owner.poll() is None:
                    owner.terminate()
                    owner.communicate(timeout=5)

    def test_disconnected_owner_reaps_receiver_group(self):
        with tempfile.TemporaryDirectory() as directory:
            identity = str(Path(directory) / "identity.json")
            owner = serve(dict(action="start", path=identity, token="owned",
                               argv=["sleep", "60"]))
            try:
                wait_file(identity, owner)
                actual = json.loads(Path(identity).read_text())
                owner.stdin.close()
                owner.stdin = None
                owner.communicate(timeout=5)
                self.assertEqual(owner.returncode, 137)
                with self.assertRaises(OSError):
                    receiver.stat_identity(actual["pid"])
            finally:
                if owner.poll() is None:
                    owner.terminate()
                    owner.communicate(timeout=5)

    def test_killed_launcher_stops_native_receiver_client_and_tunnel(self):
        for signo in (signal.SIGTERM, signal.SIGKILL):
            with self.subTest(signal=signo), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                binary = run / "bin"
                binary.mkdir()
                # SSH's local client and remote supervisor are separate processes.
                # Killing the transport must close stdin, not SIGKILL the server.
                ssh = binary / "ssh"
                ssh.write_text(
                    f"#!{sys.executable}\n"
                    "import os,pathlib,subprocess,sys\n"
                    "if '-N' in sys.argv:\n"
                    f"    pathlib.Path({str(run / 'tunnel.ready')!r}).touch()\n"
                    "    os.execvp('sleep',['sleep','60'])\n"
                    f"path=pathlib.Path({str(run / 'ssh.pid')!r})\n"
                    "temporary=path.with_name(path.name+'.tmp')\n"
                    "temporary.write_text(str(os.getpid()))\n"
                    "os.replace(temporary,path)\n"
                    "sys.exit(subprocess.call(['bash','-c',sys.argv[-1]],\n"
                    "    stdin=sys.stdin,start_new_session=True))\n")
                ssh.chmod(0o700)
                config = run / "config.json"
                config.write_text(json.dumps({
                    "host": "local-process-fixture", "ssh_options": [],
                    "local_root": str(HERE.parents[1]), "remote_root": str(HERE.parents[1]),
                    "local_run": directory, "remote_run": directory,
                    "remote_workspace": directory}))
                owner = launch_clients_owner(config, run, 1,
                    env=dict(os.environ, PATH=str(binary) + os.pathsep + os.environ["PATH"]))
                pids = []
                try:
                    for name in ("owners.json", "identity.json", "ssh.pid", "tunnel.ready"):
                        wait_file(run / name, owner)
                    clients = json.loads((run / "owners.json").read_text())
                    native = json.loads((run / "identity.json").read_text())
                    pids = [native["pid"], clients["client"], clients["tunnel"],
                            int((run / "ssh.pid").read_text())]
                    for pid in pids:
                        self.assertNotEqual(Path(f"/proc/{pid}/stat").read_text()
                                            .rsplit(")", 1)[1].split()[0], "Z")
                    owner.send_signal(signo)
                    owner.wait(timeout=5)
                    for pid in pids:
                        wait_stopped(pid)
                finally:
                    if owner.poll() is None:
                        owner.kill()
                        owner.wait(timeout=5)
                    if (run / "owners.json").exists():
                        pids.extend(json.loads((run / "owners.json").read_text()).values())
                    if (run / "identity.json").exists():
                        pids.append(json.loads((run / "identity.json").read_text())["pid"])
                    if (run / "ssh.pid").exists():
                        pids.append(int((run / "ssh.pid").read_text()))
                    for pid in pids:
                        kill_process(pid)

    def test_already_dead_launcher_cannot_acquire_a_new_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            config = run / "config.json"
            config.write_text(json.dumps({"host": "unused", "ssh_options": [],
                "local_root": directory, "remote_root": directory,
                "local_run": directory, "remote_run": directory,
                "remote_workspace": directory}))
            for action in (["start", str(run / "identity.json"), "sleep", "60"],
                           ["tunnel", "1", "9"]):
                with self.subTest(action=action[0]):
                    process = subprocess.Popen([
                        sys.executable, str(HERE / "capacity_receiver.py"),
                        "--config", str(config), "--owner-pid", "1", *action],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    _, stderr = process.communicate(timeout=5)
                    self.assertEqual(process.returncode, -signal.SIGKILL, stderr)
                    self.assertFalse((run / "identity.json").exists())

    def test_teardown_reaps_owners_even_when_signal_rpc_fails(self):
        source = (HERE / "ingest_egress_fanout.sh").read_text()
        functions = [re.search(rf"^{name}\(\) \{{.*?^\}}\n", source, re.M | re.S).group(0)
                     for name in ("cleanup", "stop_rtmp_sink_instance", "receiver_stop_owner")]
        script = "\n".join(functions) + r'''
set -uo pipefail
declare -A RECEIVER_IDENTITIES=() RECEIVER_CLIENTS=()
CAPACITY_RECEIVER_CONFIG=unreachable-receiver
PUBS=() SINKS=() OWNED=() RECEIVER_TUNNELS=() RTMP_REMOTE_MASTERS=()
STORM=0
receiver_kill() { return 42; }
capacity_quality_stop_monitors() { :; }
stop_instance() { :; }
trap 'kill -KILL "${OWNED[@]}" 2>/dev/null || true; wait || true' EXIT
for action in cleanup stop_rtmp_sink_instance; do
    sleep 60 &
    client=$!
    sleep 60 &
    tunnel=$!
    OWNED+=( "$client" "$tunnel" )
    RECEIVER_IDENTITIES[r1]=owned-identity
    RECEIVER_CLIENTS[r1]="$client"
    RTMP_REMOTE_MASTERS=( r1 )
    RECEIVER_TUNNELS=( "$tunnel" )
    "$action"
    for pid in "$client" "$tunnel"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "$action left owner $pid running after signal RPC failed" >&2
            exit 1
        fi
    done
done
'''
        process = subprocess.Popen(["bash", "-c", script], start_new_session=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, (stdout, stderr))
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate(timeout=5)

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


class PreparedCaptures(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.run = Path(self.temporary.name)
        self.identity = str(self.run / "identity.json")
        self.snapshot = str(self.run / "snapshot.json")
        self.output = str(self.run / "output.json")
        native_ready = str(self.run / "native.ready")
        code = (
            "import json,os,pathlib,signal,time\n"
            "def capture(*_):\n"
            "    data=json.dumps({'native_ns':time.monotonic_ns(),'pid':os.getpid()})\n"
            f"    path=pathlib.Path({self.snapshot!r})\n"
            "    temporary=path.with_suffix('.tmp')\n"
            "    temporary.write_text(data)\n"
            "    os.replace(temporary,path)\n"
            "signal.signal(signal.SIGUSR1,capture)\n"
            f"pathlib.Path({native_ready!r}).touch()\n"
            "time.sleep(60)\n")
        self.owner = serve(dict(action="start", path=self.identity, token=self.identity,
                                argv=[sys.executable, "-c", code]))
        self.addCleanup(self.stop_owner, self.owner, self.identity)
        wait_file(self.identity, self.owner)
        wait_file(native_ready, self.owner)
        Path(self.snapshot).write_text('{"native_ns":0}')
        binary = self.run / "bin"
        binary.mkdir()
        ssh = binary / "ssh"
        # A real remote server runs behind a delayed subprocess transport.
        # The separate session survives transport SIGKILL long enough to see EOF.
        ssh.write_text(
            f"#!{sys.executable}\n"
            "import json,os,pathlib,subprocess,sys,time\n"
            "time.sleep(0.6)\n"
            "child=subprocess.Popen(['bash','-c',sys.argv[-1]],\n"
            "    stdin=sys.stdin,start_new_session=True)\n"
            f"path=pathlib.Path({str(self.run / 'transport.json')!r})\n"
            "temporary=path.with_suffix('.tmp')\n"
            "temporary.write_text(json.dumps({'ssh':os.getpid(),'remote':child.pid}))\n"
            "os.replace(temporary,path)\n"
            "sys.exit(child.wait())\n")
        ssh.chmod(0o700)
        self.env = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ["PATH"])
        self.config = self.run / "config.json"
        self.config.write_text(json.dumps({
            "host": "local-process-fixture", "ssh_options": [],
            "local_root": str(HERE.parents[1]), "remote_root": str(HERE.parents[1]),
            "local_run": str(self.run), "remote_run": str(self.run),
            "remote_workspace": str(self.run)}))

    def stop_owner(self, owner, identity):
        if owner.poll() is None:
            receiver.signal_owned(identity, identity, "KILL")
        owner.communicate(timeout=5)

    def stop_request(self, process):
        if process.poll() is None:
            process.terminate()
        process.communicate(timeout=5)

    def request(self, **kwargs):
        return dict(action="snapshot", path=self.identity, token=self.identity,
                    signal="USR1", source=self.snapshot, output=self.output,
                    prepared=True, **kwargs)

    def client(self, *action, gated=True):
        command = [sys.executable, str(HERE / "capacity_receiver.py"),
                   "--config", str(self.config)]
        if gated:
            command += ["--capture-ready-file", str(self.run / "capture.ready"),
                        "--capture-release-file", str(self.run / "capture.release")]
        process = subprocess.Popen(command + list(action), env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop_request, process)
        return process

    def assert_bracket(self, native_start, native_end=None):
        bracket = json.loads(Path(self.output + ".request.json").read_text())
        self.assertLessEqual(bracket["request_ns"], native_start)
        self.assertLessEqual(native_end or native_start, bracket["reply_ns"])
        return bracket["reply_ns"] - bracket["request_ns"]

    def test_delayed_rpc_startup_is_outside_native_capture_bracket(self):
        started = time.monotonic_ns()
        process = self.client("snapshot", self.identity, "USR1", self.snapshot, self.output)
        wait_file(self.run / "capture.ready", process)
        self.assertGreater(time.monotonic_ns() - started, 500_000_000)
        self.assertEqual(json.loads(Path(self.snapshot).read_text()), {"native_ns": 0})
        self.assertFalse(Path(self.output).exists())
        release = time.monotonic_ns()
        (self.run / "capture.release").touch()
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        native = json.loads(Path(self.output).read_text())
        self.assertGreaterEqual(native["native_ns"], release)
        self.assertEqual(native["pid"], json.loads(Path(self.identity).read_text())["pid"])
        self.assertLess(self.assert_bracket(native["native_ns"]), 300_000_000)

    def test_one_shot_capture_releases_after_ready_without_local_gate(self):
        process = self.client("snapshot", self.identity, "USR1",
                              self.snapshot, self.output, gated=False)
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        native = json.loads(Path(self.output).read_text())
        self.assertEqual(native["pid"], json.loads(Path(self.identity).read_text())["pid"])
        self.assertLess(self.assert_bracket(native["native_ns"]), 300_000_000)

    def test_wrong_owner_rejected_before_ready_and_eof_aborts_capture(self):
        wrong = self.request()
        wrong["token"] = "wrong"
        rejected = serve(wrong)
        stdout, stderr = rejected.communicate(timeout=5)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertEqual(stdout, b"")
        self.assertIn(b"ownership token mismatch", stderr)
        self.assertEqual(json.loads(Path(self.snapshot).read_text()), {"native_ns": 0})
        parked = serve(self.request())
        self.addCleanup(self.stop_request, parked)
        self.assertEqual(receiver.read_frame(parked.stdout), {"ready": True})
        stdout, stderr = parked.communicate(timeout=5)
        self.assertNotEqual(parked.returncode, 0)
        self.assertIn(b"peer disconnected", stderr)
        self.assertEqual(json.loads(Path(self.snapshot).read_text()), {"native_ns": 0})
        self.assertFalse(Path(self.output).exists())

    def test_release_revalidates_identity_before_mutation(self):
        parked = serve(self.request())
        self.addCleanup(self.stop_request, parked)
        self.assertEqual(receiver.read_frame(parked.stdout), {"ready": True})
        original = json.loads(Path(self.identity).read_text())
        Path(self.identity).write_text(json.dumps(dict(original, start=original["start"] + 1)))
        try:
            stdout, stderr = parked.communicate(b'{"capture":true}\n', timeout=5)
            self.assertNotEqual(parked.returncode, 0)
            self.assertIn(b"PID/group was reused", stderr)
            self.assertEqual(json.loads(Path(self.snapshot).read_text()), {"native_ns": 0})
            self.assertFalse(Path(self.output).exists())
        finally:
            Path(self.identity).write_text(json.dumps(original))

    def test_owned_command_captures_real_helper_output_only_after_release(self):
        helper = self.run / "capture.py"
        helper.write_text(
            "import json,os,pathlib,sys,time\n"
            "pathlib.Path(sys.argv[1]).write_text(json.dumps(\n"
            "    {'native_ns':time.monotonic_ns(),'pid':os.getpid(),'value':137}))\n")
        process = self.client("command-capture", self.identity, self.output,
                              sys.executable, str(helper), self.output)
        wait_file(self.run / "capture.ready", process)
        self.assertFalse(Path(self.output).exists())
        release = time.monotonic_ns()
        (self.run / "capture.release").touch()
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        native = json.loads(Path(self.output).read_text())
        self.assertEqual(native["value"], 137)
        self.assertNotEqual(native["pid"], json.loads(Path(self.identity).read_text())["pid"])
        self.assertGreaterEqual(native["native_ns"], release)
        self.assertLess(self.assert_bracket(native["native_ns"]), 300_000_000)

    def test_cpu_rpc_prepares_sampler_and_captures_inside_release_bracket(self):
        identity = str(self.run / "cpu.identity.json")
        roles = self.run / "roles"
        roles.write_text("")
        argv = [sys.executable, str(HERE / "proc_cpu_sampler.py"), "sample",
                "--sleep", "0.05", "--roles", str(roles),
                "--before", str(self.run / "cpu.before"),
                "--after", str(self.run / "cpu.after"),
                "--boundary-dir", str(self.run), str(os.getpid())]
        sampler_owner = serve(dict(action="start", path=identity, token=identity, argv=argv))
        self.addCleanup(self.stop_owner, sampler_owner, identity)
        wait_file(identity, sampler_owner)
        process = self.client("cpu-boundary", identity, str(self.run), "before", self.output)
        wait_file(self.run / "capture.ready", process)
        self.assertTrue((self.run / "cpu.ready.before").is_file())
        self.assertFalse((self.run / "cpu.release.before").exists())
        self.assertFalse((self.run / "cpu.before").exists())
        (self.run / "capture.release").touch()
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        native = json.loads(Path(self.output).read_text())
        self.assertLess(self.assert_bracket(native["capture_start_ns"],
                                           native["capture_end_ns"]), 300_000_000)

    def test_killed_parked_client_closes_remote_rpc_without_capture(self):
        for signo in (signal.SIGTERM, signal.SIGKILL):
            with self.subTest(signal=signo):
                (self.run / "capture.ready").unlink(missing_ok=True)
                process = self.client("snapshot", self.identity, "USR1",
                                      self.snapshot, self.output)
                wait_file(self.run / "capture.ready", process)
                pids = json.loads((self.run / "transport.json").read_text())
                try:
                    process.send_signal(signo)
                    process.communicate(timeout=5)
                    wait_stopped(pids["ssh"])
                    wait_stopped(pids["remote"])
                    self.assertEqual(json.loads(Path(self.snapshot).read_text()), {"native_ns": 0})
                    self.assertFalse(Path(self.output).exists())
                finally:
                    for pid in pids.values():
                        kill_process(pid)


def ssh_owner_death(config_path, transport, run, signo):
    run.mkdir()
    assert transport.request("mkdir", str(run)).returncode == 0
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        local_port = listener.getsockname()[1]
    owner = launch_clients_owner(config_path, run, local_port)
    pids = []
    actual = None
    passed = False
    identity = str(run / "identity.json")
    try:
        wait_file(run / "owners.json", owner)
        clients = json.loads((run / "owners.json").read_text())
        pids = list(clients.values())
        deadline = time.monotonic() + 15
        while transport.request("exists", identity).returncode != 0:
            assert owner.poll() is None
            assert time.monotonic() < deadline
            time.sleep(0.05)
        assert transport.fetch(identity) == 0
        actual = json.loads(Path(identity).read_text())
        assert transport.request("signal", identity, token=identity, signal="0").returncode == 0
        children = Path(f"/proc/{clients['client']}/task/{clients['client']}/children")
        ssh_pids = list(map(int, children.read_text().split()))
        assert len(ssh_pids) == 1, f"unexpected receiver SSH owners: {ssh_pids}"
        pids.extend(ssh_pids)
        while True:
            try:
                with socket.create_connection(("127.0.0.1", local_port), timeout=0.2):
                    break
            except OSError:
                assert time.monotonic() < deadline, "receiver forwarding tunnel did not bind"
                assert owner.poll() is None
                time.sleep(0.02)
        owner.send_signal(signo)
        owner.wait(timeout=5)
        for pid in pids:
            wait_stopped(pid)
        # Successful SSH observation distinguishes native death from an
        # unreachable signal RPC. PID start validation rejects PID reuse.
        probe = (
            "import pathlib,time\n"
            f"path=pathlib.Path('/proc/{actual['pid']}/stat')\n"
            "deadline=time.monotonic()+5\n"
            "while path.exists():\n"
            "    try: fields=path.read_text().rsplit(')',1)[1].split()\n"
            "    except FileNotFoundError: break\n"
            f"    if int(fields[19])!={actual['start']} or fields[0]=='Z': break\n"
            "    if time.monotonic()>=deadline: raise SystemExit('native receiver survived owner death')\n"
            "    time.sleep(0.01)\n"
            "print('native receiver stopped')\n")
        observed = subprocess.run([*transport.ssh, shlex.join(["python3", "-c", probe])],
                                  capture_output=True, text=True, check=False)
        assert observed.returncode == 0, observed.stdout + observed.stderr
        passed = True
        print(json.dumps(dict(ssh_owner_death_smoke="pass", signal=signo.name,
                              receiver_pid=actual["pid"], client_pid=clients["client"],
                              ssh_pid=ssh_pids[0], tunnel_pid=clients["tunnel"],
                              second_physical_host_evidence=False)))
    finally:
        if owner.poll() is None:
            owner.kill()
            owner.wait(timeout=5)
        if (run / "owners.json").exists():
            pids.extend(json.loads((run / "owners.json").read_text()).values())
        for pid in pids:
            kill_process(pid)
        if not passed and actual is not None:
            transport.request("signal", identity, token=identity, signal="KILL")


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
               "--owner-pid", str(os.getpid()), "start", identity, "python3", "-c", code]
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
            owner.kill()
            owner.wait(timeout=10)
    for signo in (signal.SIGTERM, signal.SIGKILL):
        ssh_owner_death(config_path, transport, run / signo.name.lower(), signo)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--ssh-config":
        ssh_smoke(sys.argv[2])
    else:
        unittest.main()

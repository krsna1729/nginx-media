"""Processes, namespaces and deadlines.

Every child the harness starts is a Proc, every wait is wait_until, and every
HTTP request is curl with a deadline run in the namespace that can reach the
address. Only the network namespace differs between roles, so PIDs are host
PIDs and a Proc's pid is the media process itself (nsenter --no-fork execs).
"""

import contextlib
import os
import shlex
import signal
import subprocess
import time


class Failed(Exception):
    """A step failed for a stated reason; the rung records it."""


class Timeout(Failed):
    pass


SENDER, HARNESS, RECEIVER = "sender", "harness", "receiver"


def _prefix(name, default=""):
    return shlex.split(os.environ.get(name, default))


def prefix(ns):
    """argv prefix that runs a command in a role's network namespace."""
    if ns is None:
        return []
    receiver = _prefix("CAPACITY_RECEIVER_EXEC")
    return {SENDER: _prefix("CAPACITY_SENDER_EXEC"),
            RECEIVER: receiver,
            HARNESS: _prefix("CAPACITY_HARNESS_EXEC") or receiver}[ns]


def pin(cpus):
    return ["taskset", "-c", cpus] if cpus else []


def argv_for(argv, ns=None, cpus=None):
    return prefix(ns) + pin(cpus) + [str(a) for a in argv]


def _bounded(full, timeout, data=None, **kw):
    """Run in its own session; on timeout kill the whole session, so a
    grandchild holding the output pipe cannot keep us waiting."""
    popen = subprocess.Popen(full, stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             start_new_session=True, **kw)
    try:
        out, err = popen.communicate(data, timeout=timeout)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(popen.pid, signal.SIGKILL)
        with contextlib.suppress(subprocess.TimeoutExpired):
            popen.communicate(timeout=5)  # a setsid'd grandchild may keep the pipe
        raise Timeout("%s did not finish within %ss" % (" ".join(full[:3]), timeout))
    return subprocess.CompletedProcess(full, popen.returncode, out, err)


def run(argv, ns=None, cpus=None, timeout=60, check=True, input=None, **kw):
    """Run to completion within `timeout`; output captured as text."""
    done = _bounded(argv_for(argv, ns, cpus), timeout, input, **kw)
    if check and done.returncode != 0:
        raise Failed("%s exited %d: %s" % (argv[0], done.returncode,
                                           (done.stderr or done.stdout).strip()[-400:]))
    return done


def curl(url, ns, timeout=20, data=None, method=None, headers=(), check=True):
    """HTTP from inside `ns`. Returns (status, body); status 0 = no answer."""
    argv = ["curl", "-sS", "--connect-timeout", str(min(3, timeout)),
            "--max-time", str(timeout), "-w", "\n%{http_code}"]
    if method:
        argv += ["-X", method]
    for header in headers:
        argv += ["-H", header]
    if data is not None:
        argv += ["--data-binary", "@-"]
    argv.append(url)
    try:
        done = _bounded(argv_for(argv, ns), timeout + 5, data)
        body, _, code = done.stdout.rpartition("\n")
        status = int(code) if code.isdigit() else 0
    except Timeout:
        body, status = "", 0
    if check and not 200 <= status < 300:
        raise Failed("%s %s -> %s" % (method or "GET", url, status or "no answer"))
    return status, body


def wait_until(what, predicate, timeout, interval=0.05, alive=()):
    """Poll `predicate` until truthy; fail by the clock, or as soon as a
    process the wait depends on has exited."""
    deadline = time.monotonic() + timeout
    while True:
        value = predicate()
        if value:
            return value
        for proc in alive:
            if not proc.alive():
                raise Failed("%s: %s exited (%s) while waiting" % (what, proc.name, proc.returncode))
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Timeout("%s: not ready after %ss" % (what, timeout))
        time.sleep(min(interval, remaining))


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


class Proc:
    """A child in its own session, so stopping it reaches its descendants."""

    def __init__(self, name, argv, ns=None, cpus=None, log=None, env=None, cwd=None):
        self.name = name
        self.log = log
        out = open(log, "ab") if log else subprocess.DEVNULL
        try:
            self.popen = subprocess.Popen(argv_for(argv, ns, cpus), stdout=out,
                                          stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                          start_new_session=True, env=env, cwd=cwd)
        finally:
            if log:
                out.close()
        self.pid = self.popen.pid

    @property
    def returncode(self):
        return self.popen.poll()

    def alive(self):
        return self.popen.poll() is None

    def signal(self, sig):
        if self.alive():
            os.kill(self.pid, sig)

    def wait(self, timeout):
        try:
            return self.popen.wait(timeout)
        except subprocess.TimeoutExpired:
            raise Timeout("%s still running after %ss" % (self.name, timeout))

    def stop(self, grace=10):
        """TERM, then KILL the session after `grace`. Returns the exit status;
        a process that died by our TERM counts as a clean stop (0)."""
        if self.alive():
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self.pid, signal.SIGTERM)
            try:
                self.popen.wait(grace)
            except subprocess.TimeoutExpired:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(self.pid, signal.SIGKILL)
                self.popen.wait(5)
                return -signal.SIGKILL
            if self.popen.returncode == -signal.SIGTERM:
                return 0
        with contextlib.suppress(ProcessLookupError):
            os.killpg(self.pid, signal.SIGKILL)  # stragglers in its session
        return self.popen.returncode


def nginx_pid(prefix_dir):
    try:
        with open(os.path.join(prefix_dir, "logs", "nginx.pid")) as handle:
            return int(handle.read().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def stop_nginx(prefix_dir, grace=10):
    """Stop a leftover nginx from a crashed earlier run by its pidfile:
    the master and its direct children (workers), nothing else."""
    master = nginx_pid(prefix_dir)
    if not master or not pid_alive(master):
        return
    with open("/proc/%d/cmdline" % master, "rb") as handle:
        if b"nginx" not in handle.read():
            return  # pid reused by something else
    workers = [int(p) for p in run(["pgrep", "-P", str(master)], timeout=5, check=False).stdout.split()]
    for pid in [master] + workers:
        with contextlib.suppress(ProcessLookupError):
            os.kill(pid, signal.SIGTERM)
    with contextlib.suppress(Timeout):
        wait_until("nginx %s exit" % prefix_dir,
                   lambda: not any(pid_alive(p) for p in [master] + workers), grace)
    for pid in [master] + workers:
        with contextlib.suppress(ProcessLookupError):
            os.kill(pid, signal.SIGKILL)


class Scope(contextlib.ExitStack):
    """Owns what one rung or phase starts; closing stops it all, newest first.

    Monitors are processes whose failure invalidates the window; a sampler
    that finished on its own (status 0, e.g. paused at the end edge) is fine.
    stop_monitors names any that failed and how."""

    def __init__(self):
        super().__init__()
        self.monitors = []

    def spawn(self, name, argv, monitor=False, grace=10, **kw):
        proc = Proc(name, argv, **kw)
        self.callback(proc.stop, grace)
        if monitor:
            self.monitors.append(proc)
        return proc

    def stop_monitors(self, grace=10):
        failed = []
        for proc in self.monitors:
            status = proc.stop(grace)
            if status != 0:
                failed.append("%s (pid %d) exited %s" % (proc.name, proc.pid, status))
        self.monitors = []
        if failed:
            raise Failed("; ".join(failed))


def install_signal_handlers():
    """Turn TERM/INT/HUP into SystemExit so every Scope's cleanup runs."""
    def handler(signum, _frame):
        for name in ("SIGTERM", "SIGINT", "SIGHUP"):
            signal.signal(getattr(signal, name), signal.SIG_IGN)
        raise SystemExit(128 + signum)
    for name in ("SIGTERM", "SIGINT", "SIGHUP"):
        signal.signal(getattr(signal, name), handler)

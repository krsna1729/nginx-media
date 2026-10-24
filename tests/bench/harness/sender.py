"""The system under test: an nginx (or restream) sender in the SENDER namespace."""

import json
import os
import re
import shutil

from . import api, proc

_LOG_PID = re.compile(r".*\[[a-z]*\] (\d+)#")
# Order matters: the ingest thread wins over anything else in its stack.
_THREAD_MARKS = (("ngx_media_srt_thread", "ingest"),
                 ("ngx_media_srt_out_thread", "srt-send"),
                 ("ngx_media_hls_push_thread", "hls-push"),
                 ("ngx_worker_process_cycle", "worker"),
                 ("CRcvQueue::worker", "SRT:RcvQ"),
                 ("CSndQueue::worker", "SRT:SndQ"),
                 ("CUDT::tsbpd", "SRT:TsbPd"),
                 ("garbageCollect", "SRT:GC"))


def srt_ports(cfg, workers):
    return [cfg.base + 2 + i for i in range(workers)]


def write_config(cfg, workers, hls, rtmp):
    """nginx.conf for `workers` SRT endpoints; media on cfg.media_addr, the
    unauthenticated API only on loopback. Returns the SRT ports."""
    ports = srt_ports(cfg, workers)
    lines = ["worker_processes %d;" % workers,
             "error_log logs/error.log info;", "pid logs/nginx.pid;"]
    if cfg.srt_workers != "adaptive":
        lines.append("media_egress_workers srt %s;" % cfg.srt_workers)
    if cfg.hls_push_workers != "adaptive":
        lines.append("media_egress_workers hls_push %s;" % cfg.hls_push_workers)
    lines += ["", "events { worker_connections 8192; }", "",
              "media_ingest_secret %s;" % cfg.path("ingest.secret")]
    if hls:
        lines.append("media_hls %s;" % cfg.path("hls"))
    lines += ["media_srt_listen %s:%d;" % (cfg.media_addr, port) for port in ports]
    if rtmp:
        lines.append("media_rtmp_listen %s:%d;" % (cfg.media_addr, cfg.base + 1))
    lines += ["", "http {"]
    if hls:
        lines += ['    log_format media_pid "$pid $bytes_sent";',
                  "    access_log logs/access.log media_pid;"]
    else:
        lines.append("    access_log off;")
    lines += ["", "    server {",
              "        listen 127.0.0.1:%d reuseport;" % cfg.api_port, "",
              "        location /media/api/ { media_api; }",
              "        location /hls/       { alias %s/; }" % cfg.path("hls"),
              "    }"]
    if hls and cfg.sender_addr != "127.0.0.1":
        lines += ["    server {",
                  "        listen %s:%d;" % (cfg.sender_addr, cfg.api_port + 31),
                  "        location /hls/ { alias %s/; }" % cfg.path("hls"),
                  "        location / { }",
                  "    }"]
    lines.append("}")
    os.makedirs(cfg.path("conf"), exist_ok=True)
    with open(cfg.path("conf", "nginx.conf"), "w") as handle:
        handle.write("\n".join(lines) + "\n")
    return ports


def _children(pid):
    found = []
    for entry in os.listdir("/proc"):
        if entry.isdigit():
            try:
                with open("/proc/%s/stat" % entry) as handle:
                    if int(handle.read().rpartition(")")[2].split()[1]) == pid:
                        found.append(int(entry))
            except (OSError, IndexError, ValueError):
                pass
    return found


def _thread_roles(text):
    roles, tid, role = {}, None, ""
    for line in text.splitlines():
        match = re.match(r"Thread \d+ \(Thread .*?LWP (\d+)", line)
        if match:
            if tid is not None:
                roles[tid] = role or "unlabelled"
            tid, role = int(match.group(1)), ""
            continue
        for mark, name in _THREAD_MARKS:
            if mark in line and (not role or name == "ingest"):
                role = name
    if tid is not None:
        roles[tid] = role or "unlabelled"
    return roles


_unlabelled = []


def labelled():
    """False once any sender in this run fell back to comm names."""
    return not _unlabelled


def label_threads(pids):
    """Thread roles from gdb backtraces: ({tid: role}, every pid labelled)."""
    roles, ok = {}, True
    for pid in pids:
        try:
            done = proc.run(["sudo", "-n", "gdb", "-p", pid, "-batch",
                             "-ex", "thread apply all bt 14"], timeout=60, check=False)
            found = _thread_roles(done.stdout)
        except (proc.Timeout, OSError):
            found = {}
        roles.update(found)
        ok = ok and bool(found)
    if not ok:
        _unlabelled.append(pids)
    return roles, ok


class Instance:
    """A running sender. Every lookup reads the log or /proc afresh."""

    def __init__(self, cfg, workers):
        self.cfg = cfg
        self.workers = workers
        self.srt_ports = srt_ports(cfg, workers)
        self.http_port = cfg.api_port
        self.error_log = cfg.path("logs", "error.log")
        self.api = api.base(cfg)
        self.roles, self.labelled, self.slots, self.placement = {}, True, {}, ""
        self.proc = self.pipeline_id = self.cookie = None
        self.restream = cfg.engine == "restream"

    def master_pid(self):
        return self.proc.pid if self.restream else proc.nginx_pid(self.cfg.run)

    def worker_pids(self):
        """The sender's processes: nginx workers (children of the pidfile
        master, whatever the namespace prefix execs), or restream itself."""
        master = self.master_pid() if self.proc.alive() else None
        if not master:
            return []
        return [master] if self.restream else sorted(_children(master))

    def slot_name(self, pid):
        if self.restream:
            return "restream"
        return "w%d" % self.slots[pid] if pid in self.slots else "pid%d" % pid

    def _log(self):
        try:
            with open(self.error_log, errors="replace") as handle:
                return handle.read().splitlines()
        except FileNotFoundError:
            return []

    def pid_of(self, pattern):
        """The pid on the last error-log line matching `pattern`."""
        found = None
        for line in self._log():
            if re.search(pattern, line):
                match = _LOG_PID.search(line)
                found = int(match.group(1)) if match else found
        return found

    def _listener(self, port):
        return self.pid_of(r"srt listener ready on %s:%d$" % (re.escape(self.cfg.media_addr), port))

    def endpoint_worker(self, port):
        pid = self._listener(port)
        return self.slot_name(pid) if pid else "unknown"

    def ingest_totals(self):
        """Sum of every worker's last `srt ingest drained` counters."""
        fields = ("bytes", "chunks", "dropped_chunks", "sessions")
        last = {}
        for line in self._log():
            match = _LOG_PID.search(line)
            if match and "srt ingest drained" in line:
                last[int(match.group(1))] = line
        totals, seen = dict.fromkeys(fields, 0), False
        for pid in self.worker_pids():
            if pid not in last:
                continue
            seen = True
            for field in fields:
                value = re.search(r" %s=(\d+)" % field, last[pid])
                if not value:
                    raise proc.Failed("worker %d drained line has no %s" % (pid, field))
                totals[field] += int(value.group(1))
        if not seen:
            raise proc.Failed("no worker logged an srt ingest drained line")
        return totals

    def _count(self, text):
        return sum(text in line for line in self._log())


def start(scope, cfg, workers, hls=True, rtmp=False):
    """Start the sender (cfg.engine) as a child of `scope`, ready when every
    endpoint is bound; slots mapped and threads labelled."""
    if cfg.engine == "restream":
        return _start_restream(scope, cfg, workers)
    running = proc.nginx_pid(cfg.run)
    if running and proc.pid_alive(running):
        raise proc.Failed("a sender (pid %d) is still running in %s" % (running, cfg.run))
    for name in ("hls", "logs"):
        shutil.rmtree(cfg.path(name), ignore_errors=True)
        os.makedirs(cfg.path(name))
    write_config(cfg, workers, hls, rtmp)
    nginx = [cfg.nginx, "-p", cfg.run, "-c", "conf/nginx.conf"]
    done = proc.run(nginx + ["-t"], ns=proc.SENDER, timeout=30, check=False)
    with open(cfg.path("conf.log"), "w") as handle:
        handle.write(done.stdout + done.stderr)
    if done.returncode != 0:
        raise proc.Failed("configuration rejected: %s" % (done.stdout + done.stderr).strip()[-400:])
    inst = Instance(cfg, workers)
    inst.proc = scope.spawn("sender nginx", nginx + ["-g", "daemon off;"], ns=proc.SENDER,
                            cpus=cfg.nginx_cpus, log=cfg.path("logs", "nginx.stderr"))
    try:
        proc.wait_until("sender endpoints bound", lambda: inst._count("srt listener ready") >= workers,
                        timeout=20, alive=[inst.proc])
        if rtmp:
            proc.wait_until("sender rtmp listener", lambda: inst._count("rtmp listener ready"),
                            timeout=10, alive=[inst.proc])
    except proc.Failed as error:
        raise type(error)("%s; log tail: %s" % (error, " | ".join(inst._log()[-5:])))
    for index, port in enumerate(inst.srt_ports):
        pid = inst._listener(port)
        if pid:
            inst.slots[pid] = index
    inst.roles, inst.labelled = label_threads(inst.worker_pids())
    inst.placement = "".join(" %d->%s" % (port, inst.endpoint_worker(port)) for port in inst.srt_ports)
    return inst


def _restream_curl(inst, path, data, jar_flag):
    done = proc.run(["curl", "-sS", "--connect-timeout", "3", "--max-time", "20",
                     "-w", "\n%{http_code}", "-X", "POST",
                     "-H", "Content-Type: application/json", "--data-binary", data,
                     jar_flag, inst.cookie, inst.api + path],
                    ns=proc.SENDER, timeout=30, check=False)
    body, _, code = done.stdout.rpartition("\n")
    if not code.startswith("2"):
        raise proc.Failed("restream POST %s -> %s %s" % (path, code or "no answer", body[:200]))
    return body


def _start_restream(scope, cfg, workers):
    """restream as the sender: healthy by a deadline, logged in, one pipeline."""
    root = cfg.path("restream")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "db"))
    os.makedirs(os.path.join(root, "logs"))
    env = dict(os.environ,
               RESTREAM_HTTP_PORT=str(cfg.restream_http_port),
               RESTREAM_HTTP_BIND_ADDR=cfg.media_addr,
               RESTREAM_RTMP_PORT=str(cfg.restream_rtmp_port),
               RESTREAM_SRT_PORT=str(cfg.restream_srt_port),
               RESTREAM_INITIAL_ADMIN_PASSWORD="admin_password",
               RESTREAM_DB_PATH=os.path.join(root, "db", "restream.db"),
               RESTREAM_LOG_DIR=os.path.join(root, "logs"))
    inst = Instance(cfg, workers)
    root_url = "http://%s:%d" % (cfg.media_addr, cfg.restream_http_port)
    inst.api = root_url + "/api/v1"
    inst.cookie = os.path.join(root, "cookie.txt")
    inst.proc = scope.spawn("restream", [cfg.restream_bin], ns=proc.SENDER, cpus=cfg.nginx_cpus,
                            env=env, log=os.path.join(root, "logs", "stdout.log"), grace=5)
    try:
        proc.wait_until("restream healthz",
                        lambda: 200 <= proc.curl(root_url + "/healthz", proc.SENDER, timeout=2,
                                                 check=False)[0] < 300,
                        timeout=30, interval=0.1, alive=[inst.proc])
        _restream_curl(inst, "/auth/login", '{"password":"admin_password"}', "-c")
        body = _restream_curl(inst, "/pipelines", '{"name":"live","streamKey":"live"}', "-b")
        try:
            inst.pipeline_id = json.loads(body)["pipeline"]["id"]
        except (ValueError, KeyError, TypeError):
            raise proc.Failed("restream did not report a pipeline id: %s" % body[:200])
    except BaseException:
        inst.proc.stop(5)
        raise
    inst.roles, inst.labelled = label_threads(inst.worker_pids())
    return inst

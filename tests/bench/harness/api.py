"""The sender's control API (loopback of the SENDER namespace) and the
Prometheus text it serves."""

import json
import re
import time

from . import proc

_SERIES = re.compile(r"([A-Za-z_:][\w:]*)(?:\{(.*)\})?$")
_LABEL = re.compile(r'(\w+)="((?:[^"\\]|\\.)*)"')


def base(cfg):
    return "http://127.0.0.1:%d/media/api/v1" % cfg.api_port


def get(cfg, path, timeout=20):
    """(status, body) of GET base+path; status 0 = no answer."""
    return proc.curl(base(cfg) + path, proc.SENDER, timeout=timeout, check=False)


def _within(what, attempt, timeout, interval):
    """First non-None result of `attempt` by the deadline (at least one try), else None."""
    found = []

    def ready():
        value = attempt()
        if value is not None:
            found.append(value)
        return found
    try:
        return proc.wait_until(what, ready, timeout, interval)[0]
    except proc.Timeout:
        return None


def stream_field(cfg, name, field, timeout=20):
    """An integer field of the stream document, as the answering worker sees it."""
    status, body = get(cfg, "/streams/live/%s" % name, timeout)
    match = re.search(r'"%s":(\d+)' % re.escape(field), body) if 200 <= status < 300 else None
    return int(match.group(1)) if match else None


def field(cfg, name, field_name, timeout=2.4):
    """stream_field retried until a worker that has the stream answers."""
    return _within("%s %s" % (name, field_name),
                   lambda: stream_field(cfg, name, field_name, min(20, max(1, timeout))),
                   timeout, 0.2)


def owner_of(cfg, name, timeout=10):
    return field(cfg, name, "owner", timeout)


def owner_metrics(cfg, name, timeout=10):
    """The owner's stream document and metrics, fetched on one connection so
    both come from the worker that says observed_here; None if never reached."""
    if cfg.engine == "restream":
        return None
    urls = [base(cfg) + "/streams/live/%s" % name, base(cfg) + "/metrics"]
    request = min(20, max(1, timeout))

    def attempt():
        done = proc.run(["curl", "-fsS", "--connect-timeout", "3", "--max-time", request] + urls,
                        ns=proc.SENDER, timeout=request + 5, check=False)
        return done.stdout if done.returncode == 0 and '"observed_here":true' in done.stdout else None
    return _within("owner metrics of %s" % name, attempt, timeout, 0)


def owner_metric(cfg, name, series):
    text = owner_metrics(cfg, name)
    return None if text is None else metric(text, series, {"application": "live", "name": name})


def owner_percentile(cfg, name, p):
    text = owner_metrics(cfg, name)
    return None if text is None else percentile(text, name, p)


def post_owner(cfg, path, body):
    """POST until the owner accepts it. 409 not_owner and 404 (owner cannot
    see the stream yet) retry, immediately while misses are few: every
    attempt is a new connection, so reuseport re-rolls the worker."""
    budget = cfg.owner_retry_budget
    deadline = time.monotonic() + budget
    misses = 0
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise proc.Failed("POST %s never reached the owner within %ss" % (path, budget))
        status, answer = proc.curl(base(cfg) + path, proc.SENDER, timeout=max(1, min(20, remaining)),
                                   data=body, method="POST", check=False,
                                   headers=("Content-Type: application/json",))
        if 200 <= status < 300:
            return
        if status not in (404, 409):
            raise proc.Failed("POST %s answered %s %s" % (path, status or "nothing", answer[:200]))
        misses += 1
        if misses >= 2 * cfg.workers:
            time.sleep(min(0.05, max(0, deadline - time.monotonic())))


def _number(text):
    try:
        return int(text)
    except ValueError:
        return float(text)


def metric(text, series, labels=None):
    """First sample of `series`. labels None: the series token must equal
    `series` exactly (e.g. 'x' or 'x{a="b"}'); a dict: the metric name is
    `series` and these labels are among the sample's."""
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 2 or line.startswith("#"):
            continue
        if labels is None:
            if parts[0] == series:
                return _number(parts[1])
            continue
        match = _SERIES.match(parts[0])
        if match and match.group(1) == series:
            found = dict(_LABEL.findall(match.group(2) or ""))
            if all(found.get(key) == str(value) for key, value in labels.items()):
                return _number(parts[1])
    return None


def percentile(text, stream, p):
    return metric(text, "nginx_media_stream_fanout_delay_ms", {"name": stream, "percentile": p})


def json_field(text, path):
    """A dotted field of the stream document leading a stream+metrics snapshot.
    Histogram lists map their open bucket (null) to 60000 ms."""
    value = json.JSONDecoder().raw_decode(text.lstrip())[0]
    for key in path.split("."):
        value = value[key]
    return [60000 if item is None else item for item in value] if isinstance(value, list) else value


def worker_pid(text):
    match = re.search(r'^nginx_media_worker_info\{worker="[^"]*",pid="(\d+)"\} 1$', text, re.M)
    return int(match.group(1)) if match else None


def worker_metrics(cfg, inst, prefix, timeout=20):
    """One /metrics answer from every worker, written to prefix.<pid>."""
    if inst.restream:
        pids = inst.worker_pids()
        for pid in pids:
            open("%s.%d" % (prefix, pid), "a").close()
        return dict.fromkeys(pids, "")
    wanted = set(inst.worker_pids())
    if not wanted:
        raise proc.Failed("no sender workers to scrape")
    seen = {}

    def attempt():
        status, body = get(cfg, "/metrics", timeout=min(20, timeout))
        pid = worker_pid(body) if 200 <= status < 300 else None
        if pid in wanted and pid not in seen:
            with open("%s.%d" % (prefix, pid), "w") as handle:
                handle.write(body if body.endswith("\n") else body + "\n")
            seen[pid] = body
        return len(seen) == len(wanted)
    try:
        proc.wait_until("worker metrics", attempt, timeout, interval=0)
    except proc.Timeout:
        raise proc.Failed("only scraped %d of %d worker metrics within %ss"
                          % (len(seen), len(wanted), timeout))
    return seen


def owner_names(slot, workers, count, prefix):
    """`count` stream names whose FNV-1a owner hash (src/core/ngx_media_owner.c)
    picks worker `slot` of `workers`."""
    names = []
    for i in range(1, 20000):
        name = "%s%d" % (prefix, i)
        h = 14695981039346656037
        for byte in b"live/" + name.encode():
            h = ((h ^ byte) * 1099511628211) & ((1 << 64) - 1)
        if h % workers == slot:
            names.append(name)
            if len(names) == count:
                break
    return names

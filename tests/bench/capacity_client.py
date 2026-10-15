#!/usr/bin/env python3
"""The capacity harness's data plane: HTTP, JSON and the destination scheme.

The harness spends most of its code on things that are not orchestration --
building request bodies, retrying owner-routed calls, walking per-destination
loops -- and the failures that cost the most have all been in that half:

  * request bodies assembled by shell concatenation, then handed to python
    through a `'''$res'''` literal
  * a 0.2 s sleep on a retry path that runs three times per destination, which
    put 93% of a rung's wall time into setup nobody measured
  * a retry loop bounded by attempt count rather than by time

This module owns that half.  The shell keeps the other half, which it does
well: process lifecycle, ordering, cleanup traps, CPU pinning, namespace
entry, signal delivery.

Two rules govern the HTTP here, and both were learned the hard way.

**Owner-routed calls need a new connection per attempt.**  A stream is served
by exactly one worker, chosen by hashing the stream name, and a `reuseport`
listener hashes each *new connection* to pick the worker that accepts it.  A
POST that lands on a replica answers 409, and the only thing that can make the
next attempt land elsewhere is a different connection with a different source
port.  So `post_owner` deliberately does not reuse its connection across
attempts, while every other call does.

**Retries are bounded by time, not by count.**  A 404 means "the owner cannot
see this stream yet" as often as it means "wrong worker", so giving up after a
fixed number of immediate retries turns a busy owner into a reported setup
error.  The budget matches the 60 x 0.2 s window the old loop allowed, but a
healthy run finishes in about three round trips instead of twelve seconds.
"""

from __future__ import annotations

import argparse
import http.client
import json
import sys
import time
import urllib.parse

__all__ = [
    "ApiError",
    "Client",
    "HLS_PUSH",
    "Program",
    "RTMP",
    "SRT",
    "destination_id",
    "nginx_destination_body",
    "provision",
    "read_cookie_jar",
    "receiver_requests",
    "restream_output_url",
    "rtmp_sink_http_port",
    "rtmp_sink_port",
    "sink_port",
]

#: A retry budget in seconds, matching the window the old fixed-attempt loop
#: allowed, so a slow owner is still waited for.
OWNER_RETRY_BUDGET = 12.0

#: How long to pause once misses are no longer plausibly just bad hashing.
OWNER_RETRY_PAUSE = 0.05

#: Default socket timeout.  Loopback and a veth need nothing more.
TIMEOUT = 10.0

# -- destination kinds -----------------------------------------------------
#
# Three kinds, two engines, one scheme.  Each kind becomes either an nginx
# destination body or a restream output URL, and the harness used to build
# both by string concatenation in three separate loops -- so a change to one
# kind could silently skip the others.
SRT, RTMP, HLS_PUSH = "srt", "rtmp", "hls_push"


class ApiError(RuntimeError):
    """An API call that failed in a way the caller is expected to report."""

    def __init__(self, message, status=None, body=""):
        super().__init__(message)
        self.status = status
        self.body = body


def destination_id(program_index: int, destination_index: int,
                   program_count: int) -> str:
    """The single definition of the harness's destination naming scheme.

    The shell has an equivalent `capacity_destination_id`; `--self-test`
    checks that the two agree, because the receiver's slot mapping, the stall
    contract and every report all key off these strings.
    """
    if program_count == 1:
        return "d%04d" % destination_index
    return "p%04d-d%04d" % (program_index, destination_index)


def sink_port(base_port: int, program_index: int, attempt: int,
              destinations_per_program: int, listeners: int) -> int:
    """Which of a receiver's listeners a destination is sent to.

    Round-robins the listeners so that no single receiver socket carries every
    destination, which is what makes a per-listener failure visible.
    """
    return base_port + (program_index * destinations_per_program + attempt) % listeners


class Client:
    """A connection-pooling HTTP client for one API base URL.

    One instance serves many calls.  Reusing the connection is the point: a
    curl invocation per destination costs a process fork and a TCP handshake,
    and at a thousand destinations that is the difference between a rung that
    costs seconds and one that costs minutes.
    """

    def __init__(self, base, timeout=TIMEOUT, workers=1,
                 budget=OWNER_RETRY_BUDGET, debug=False):
        self.base = base.rstrip("/")
        self.timeout = timeout
        self.workers = max(1, int(workers))
        self.budget = float(budget)
        self.debug = debug
        self._parsed = urllib.parse.urlsplit(self.base)
        self._conn = None

    # -- plumbing ----------------------------------------------------------

    def _connection(self):
        if self._conn is None:
            if self._parsed.scheme == "https":
                self._conn = http.client.HTTPSConnection(
                    self._parsed.netloc, timeout=self.timeout)
            else:
                self._conn = http.client.HTTPConnection(
                    self._parsed.netloc, timeout=self.timeout)
        return self._conn

    def _drop(self):
        """Forget the connection so the next call opens a fresh one.

        This is what re-rolls the `reuseport` worker hash, so it is called on
        every owner miss and on every transport error, and nowhere else.
        """
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None

    def request(self, method, path, body=None, headers=None, fresh=False):
        """One request.  `fresh` forces a new connection, which re-rolls the
        accept-time worker hash."""
        if fresh:
            self._drop()

        payload = None
        head = dict(headers or {})
        if body is not None:
            if isinstance(body, (dict, list)):
                payload = json.dumps(body).encode()
                head.setdefault("Content-Type", "application/json")
            else:
                payload = body.encode() if isinstance(body, str) else body

        url = self._parsed.path.rstrip("/") + path
        try:
            conn = self._connection()
            if payload is not None:
                head["Content-Length"] = str(len(payload))
            conn.request(method, url, body=payload, headers=head)
            response = conn.getresponse()
            data = response.read()
            status = response.status
            # A draining keep-alive connection would otherwise fail the next
            # call; let the pool re-open it instead.
            if response.will_close:
                self._drop()
        except Exception as exc:
            self._drop()
            raise ApiError("%s %s: %s" % (method, path, exc)) from exc

        if self.debug:
            print("  %s %s -> %s" % (method, path, status), file=sys.stderr)

        return status, data

    def get(self, path, fresh=False):
        status, data = self.request("GET", path, fresh=fresh)
        if status >= 400:
            raise ApiError("GET %s answered %s" % (path, status),
                           status=status, body=data.decode("utf-8", "replace"))
        return data

    def post(self, path, body=None, headers=None, fresh=False):
        status, data = self.request("POST", path, body, headers, fresh=fresh)
        if status >= 400:
            raise ApiError("POST %s answered %s" % (path, status),
                           status=status, body=data.decode("utf-8", "replace"))
        return data

    def post_owner(self, path, body, budget=None):
        """POST to a stream-scoped endpoint, retrying until the owner answers.

        Misses are retried immediately while they are few, because each one
        costs a connection and the next connection is a fresh hash.  Once the
        misses exceed what bad hashing explains -- two per worker, so that a
        single unlucky streak does not trip it -- the owner is presumed busy
        and a short pause is inserted.  The loop ends at a deadline, never at
        an attempt count.
        """
        budget = self.budget if budget is None else float(budget)
        deadline = time.monotonic() + budget
        misses = 0
        last = None

        while time.monotonic() < deadline:
            # Always a new connection: a reused one is pinned to whichever
            # worker accepted it, so a retry on it could never succeed.
            status, data = self.request(
                "POST", path, body, fresh=True)
            if 200 <= status < 300:
                return data
            if status in (409, 404):
                last = status
                misses += 1
                if misses >= 2 * self.workers:
                    time.sleep(OWNER_RETRY_PAUSE)
                continue
            raise ApiError(
                "POST %s answered %s" % (path, status),
                status=status, body=data.decode("utf-8", "replace"))

        raise ApiError("POST %s never reached the owner within %gs "
                       "(last status %s)" % (path, budget, last),
                       status=last)

    def close(self):
        self._drop()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _client_from(args):
    return Client(args.api, timeout=args.timeout, workers=args.workers,
                  budget=args.retry_budget, debug=args.debug)


def _cmd_destination_id(args):
    for index in range(args.count):
        print(destination_id(args.program_index, args.first + index,
                             args.programs))
    return 0


def read_cookie_jar(path):
    """The `name=value` pairs curl would send for a Netscape cookie jar.

    The harvester logs in with curl and keeps the jar; these calls are made
    without curl, so the jar is read directly rather than shelling out to
    something that can parse it.

    curl marks an HttpOnly cookie by prefixing the *domain* field with
    `#HttpOnly_`, and restream's session cookie is HttpOnly.  A jar can
    therefore consist of one such line and nothing else, so treating every
    leading `#` as a comment would drop the only credential and send every
    request unauthenticated: a whole engine failing setup, with no error
    anyone sees.
    """
    pairs = []
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if line.startswith("#HttpOnly_"):
                line = line[len("#HttpOnly_"):]
            elif line.startswith("#") or not line.strip():
                continue
            fields = line.split("\t")
            if len(fields) >= 7:
                pairs.append("%s=%s" % (fields[5], fields[6]))
    return "; ".join(pairs)


def rtmp_sink_port(base, index):
    """The media port of RTMP sink `index`."""
    return base + 128 + index


def rtmp_sink_http_port(base, index, receiver_config=False):
    """The API port of RTMP sink `index`.

    With an external receiver configuration the sinks are separate instances
    reachable on the harness host, and their API ports live in a different
    range.
    """
    return base + (256 if receiver_config else 192) + index


def nginx_destination_body(kind, sink_id, receiver, port, stream, path=""):
    """The body of a `POST /streams/<stream>/destinations` for one kind."""
    body = {"id": sink_id, "type": kind}
    if kind == SRT:
        body.update(host=receiver, port=port,
                    streamid="#!::r=%s,m=publish,s=%s" % (stream, sink_id))
    elif kind == RTMP:
        body.update(host=receiver, port=port, streamid=sink_id)
    elif kind == HLS_PUSH:
        body.update(host="http://%s:%d/%s/" % (receiver, port, sink_id),
                    path=path)
    else:
        raise ApiError("unknown destination kind %r" % kind)
    return body


def restream_output_url(kind, sink_id, receiver, port, stream, path=""):
    """The URL restream should push one destination to."""
    if kind == SRT:
        return "srt://%s:%d?streamid=#!::m=publish,r=%s,s=%s" % (
            receiver, port, stream, sink_id)
    if kind == RTMP:
        return "rtmp://%s:%d/live/%s" % (receiver, port, sink_id)
    if kind == HLS_PUSH:
        return "http://%s:%d/%s/index.m3u8" % (receiver, port, sink_id)
    raise ApiError("unknown destination kind %r" % kind)


def receiver_requests(kind, sink_id):
    """The receiver-side calls an RTMP destination needs before it is wired.

    Returns (path, body) pairs.  SRT and HLS push need nothing: the sink
    already listens and the sender connects out.
    """
    if kind != RTMP:
        return []
    return [
        ("/streams", {"application": "live", "name": sink_id}),
        ("/streams/live/%s/sources" % sink_id,
         {"id": sink_id, "type": "rtmp", "key": sink_id}),
    ]


class Program:
    """One program's destinations, and the order they are provisioned in.

    The harness numbers a program's destinations across all kinds in one
    sequence -- the SRT ones first, then RTMP, then HLS push -- and several
    reports and the stall contract depend on that numbering, so it is
    reproduced here rather than reinvented.
    """

    def __init__(self, program_index, program_count, stream):
        self.program_index = program_index
        self.program_count = program_count
        self.stream = stream
        self.entries = []

    def add(self, kind, count, first=0):
        for offset in range(first, first + count):
            self.entries.append((kind, offset))
        return self

    def id_for(self, offset):
        return destination_id(self.program_index, offset, self.program_count)

    def plan(self, srt_base_port, srt_listeners, rtmp_base, rtmp_sinks,
             hls_push_port, receiver, receiver_config, hls_path):
        """Resolve each destination to (kind, id, port, receiver_api)."""
        srt_count = sum(1 for kind, _ in self.entries if kind == SRT)
        rtmp_count = sum(1 for kind, _ in self.entries if kind == RTMP)
        out = []
        for kind, offset in self.entries:
            sink_id = self.id_for(offset)
            if kind == SRT:
                port = sink_port(srt_base_port, self.program_index, offset,
                                 srt_count, srt_listeners)
                out.append((kind, sink_id, port, None))
            elif kind == RTMP:
                index = ((self.program_index * rtmp_count
                          + offset - srt_count) % rtmp_sinks)
                address = "127.0.0.1" if receiver_config else receiver
                api = "http://%s:%d/media/api/v1" % (
                    address, rtmp_sink_http_port(rtmp_base, index,
                                                 receiver_config))
                out.append((kind, sink_id, rtmp_sink_port(rtmp_base, index), api))
            else:
                out.append((kind, sink_id, hls_push_port, None))
        return out


def _receiver_sink(sinks, api, body):
    """One receiver-side call, against the sink that owns the address."""
    if sinks is None:
        return
    client = sinks.get(api)
    if client is None:
        client = sinks[api] = Client(api)
    client.post(body[0], body[1])


def provision(engine, client, program, plan, receiver, path="",
              pipeline_id="", cookie="", stall_id="", sinks=None):
    """Provision every destination of one program, on either engine.

    The nested order matters: the receiver side is prepared before the sender
    is told to use it, so a destination is never live at the sender before
    something exists to receive it.
    """
    headers = {"Cookie": cookie} if cookie else {}
    added = 0
    stall_seen = False

    for kind, sink_id, port, receiver_api in plan:
        if sink_id == stall_id:
            stall_seen = True

        if receiver_api:
            for body in receiver_requests(kind, sink_id):
                _receiver_sink(sinks, receiver_api, body)

        if engine == "restream":
            url = restream_output_url(kind, sink_id, receiver, port,
                                      program.stream, path)
            raw = client.post("/pipelines/%s/outputs" % pipeline_id, {
                "name": sink_id,
                "url": url,
                "config": {"video": {"mode": "source"},
                           "audio": {"mode": "all"}},
            }, headers=headers)
            output_id = _output_id(sink_id, raw)
            client.post(
                "/pipelines/%s/outputs/%s/start" % (pipeline_id, output_id),
                {}, headers=headers)
        else:
            client.post_owner(
                "/streams/%s/destinations" % program.stream,
                nginx_destination_body(kind, sink_id, receiver, port,
                                       program.stream, path))
        added += 1

    return added, stall_seen


def _output_id(sink_id, raw):
    """The id every restream output must have before it can be started.

    A missing id is an error, never a silent skip: an output that is created
    but not started is a destination the sender never uses.
    """
    try:
        parsed = json.loads(raw)
    except ValueError as exc:
        raise ApiError("restream returned no JSON for %s: %s"
                       % (sink_id, raw[:200])) from exc
    try:
        return parsed["output"]["id"]
    except (KeyError, TypeError) as exc:
        raise ApiError("restream returned no output id for %s: %s"
                       % (sink_id, raw[:200])) from exc


def _parse_spec(text):
    """`kind:count` -> (kind, count).  One spec per destination kind."""
    kind, _, count = text.partition(":")
    kind = kind.strip().lower().replace("-", "_")
    if kind not in (SRT, RTMP, HLS_PUSH):
        raise ApiError("unknown destination kind %r in --spec" % kind)
    try:
        return kind, int(count or 0)
    except ValueError as exc:
        raise ApiError("--spec %s wants an integer count" % text) from exc


def _cmd_provision(args):
    kinds = dict(_parse_spec(text) for text in args.spec)
    program = Program(args.program_index, args.programs, args.stream)
    # SRT first, then RTMP, then HLS push: the harness numbers a program's
    # destinations in this order and the reports depend on it.
    offset = 0
    for kind in (SRT, RTMP, HLS_PUSH):
        count = kinds.get(kind, 0)
        if count:
            program.add(kind, count, first=offset)
            offset += count

    plan = program.plan(
        srt_base_port=args.srt_base_port,
        srt_listeners=args.srt_listeners,
        rtmp_base=args.rtmp_base,
        rtmp_sinks=args.rtmp_sinks or 1,
        hls_push_port=args.hls_push_port,
        receiver=args.receiver_addr,
        receiver_config=args.receiver_config,
        hls_path=args.path,
    )

    cookie = read_cookie_jar(args.cookie_file) if args.cookie_file else ""
    sinks = {} if args.rtmp_sinks else None
    with _client_from(args) as client:
        added, stall_seen = provision(
            args.engine, client, program, plan, args.receiver_addr,
            path=args.path, pipeline_id=args.pipeline, cookie=cookie,
            stall_id=args.stall_id, sinks=sinks)
        for sink in (sinks or {}).values():
            sink.close()

    if stall_seen:
        print("stall-destination-seen")
    print("provisioned=%d" % added)
    return 0


def _cmd_post_owner(args):
    body = json.loads(args.body) if args.body else None
    with _client_from(args) as client:
        client.post_owner(args.path, body, budget=args.retry_budget)
    return 0


def _cmd_get(args):
    with _client_from(args) as client:
        sys.stdout.write(client.get(args.path).decode("utf-8", "replace"))
    return 0


def _cmd_post(args):
    body = json.loads(args.body) if args.body else None
    with _client_from(args) as client:
        sys.stdout.write(client.post(args.path, body).decode("utf-8", "replace"))
    return 0


HARNESS = "ingest_egress_fanout.sh"


def _harness_path():
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (os.path.join(here, HARNESS),
                      os.path.join(here, "..", "..", "tests", "bench", HARNESS)):
        if os.path.exists(candidate):
            return os.path.abspath(candidate)
    raise ApiError("cannot find %s next to this module" % HARNESS)


def _shell_destination_ids(cases):
    """Run the harness's own function over `cases`, via bash.

    The shell function is extracted from the script and asked for its answers
    rather than reimplemented here, so this compares two implementations
    instead of one against itself.
    """
    import re
    import subprocess

    source = open(_harness_path(), errors="ignore").read()
    match = re.search(
        r"^capacity_destination_id\(\)\s*\{.*?^\}", source,
        re.MULTILINE | re.DOTALL)
    if not match:
        raise ApiError("capacity_destination_id is gone from %s" % HARNESS)

    # The shell function takes <program index> <destination index> <program
    # count>, and its printf carries no newline, so each answer is wrapped.
    calls = "\n".join(
        'printf "%%s\\n" "$(capacity_destination_id %d %d %d)"'
        % (program_index, destination_index, programs)
        for programs, program_index, destination_index in cases)
    script = "%s\n%s\n" % (match.group(0), calls)
    result = subprocess.run(["bash", "-c", script], text=True,
                            capture_output=True)
    if result.returncode != 0:
        raise ApiError("the shell function failed: %s" % result.stderr.strip())
    return result.stdout.split()


def _cmd_self_test(_args):
    """Check this module's id scheme against the harness's shell function.

    The scheme is defined in two languages and every report, the receiver slot
    mapping and the stall contract key off these strings, so a divergence
    would silently corrupt results rather than fail loudly.
    """
    cases = [(programs, program_index, destination_index)
             for programs in (1, 4)
             for program_index in range(programs)
             for destination_index in (0, 1, 17, 1023, 2047)]
    theirs = _shell_destination_ids(cases)

    mismatches = [
        "programs=%d p=%d d=%d: python %r shell %r"
        % (programs, program_index, destination_index,
           destination_id(program_index, destination_index, programs), shell)
        for (programs, program_index, destination_index), shell
        in zip(cases, theirs)
        if destination_id(program_index, destination_index, programs) != shell
    ]
    if len(theirs) != len(cases):
        mismatches.append("shell returned %d answers for %d cases"
                          % (len(theirs), len(cases)))
    if mismatches:
        for line in mismatches:
            print(line, file=sys.stderr)
        return 1
    print("destination id scheme agrees with the shell across %d cases"
          % len(cases))
    return 0


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--api", default="http://127.0.0.1:19999/media/api/v1",
                        help="API base URL, as the shell's `api` composes it")
    parser.add_argument("--timeout", type=float, default=TIMEOUT)
    parser.add_argument("--workers", type=int, default=1,
                        help="sender worker count; sizes the owner-miss "
                             "tolerance")
    parser.add_argument("--retry-budget", type=float, default=OWNER_RETRY_BUDGET)
    parser.add_argument("--debug", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("destination-id",
                       help="print the destination ids a run would use")
    p.add_argument("--program-index", type=int, default=0)
    p.add_argument("--programs", type=int, default=1)
    p.add_argument("--first", type=int, default=0)
    p.add_argument("--count", type=int, required=True)
    p.set_defaults(run=_cmd_destination_id)

    p = sub.add_parser("provision",
                       help="provision a program's destinations in one process")
    p.add_argument("--engine", choices=("nginx", "restream"), default="nginx")
    p.add_argument("--stream", required=True,
                   help="the sender-side stream name, as live/<name>")
    p.add_argument("--spec", action="append", required=True,
                   metavar="KIND:COUNT",
                   help="one per kind; srt, rtmp or hls_push")
    p.add_argument("--program-index", type=int, default=0)
    p.add_argument("--programs", type=int, default=1)
    p.add_argument("--receiver-addr", required=True)
    p.add_argument("--srt-base-port", type=int, default=0)
    p.add_argument("--srt-listeners", type=int, default=16)
    p.add_argument("--rtmp-base", type=int, default=0)
    p.add_argument("--rtmp-sinks", type=int, default=0)
    p.add_argument("--receiver-config", action="store_true",
                   help="the receiver sinks are separate instances")
    p.add_argument("--hls-push-port", type=int, default=0)
    p.add_argument("--path", default="")
    p.add_argument("--pipeline", default="")
    p.add_argument("--cookie-file", default="")
    p.add_argument("--stall-id", default="")
    p.set_defaults(run=_cmd_provision)

    p = sub.add_parser("post-owner",
                       help="one owner-routed POST with the shared retry policy")
    p.add_argument("path")
    p.add_argument("--body", default="")
    p.set_defaults(run=_cmd_post_owner)

    p = sub.add_parser("get")
    p.add_argument("path")
    p.set_defaults(run=_cmd_get)

    p = sub.add_parser("post")
    p.add_argument("path")
    p.add_argument("--body", default="")
    p.set_defaults(run=_cmd_post)

    p = sub.add_parser("self-test",
                       help="check the id scheme against the shell's")
    p.set_defaults(run=_cmd_self_test)

    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.run(args)
    except ApiError as exc:
        print("capacity_client: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

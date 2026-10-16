#!/usr/bin/env python3
"""The capacity harness's data plane: HTTP, JSON and the destination scheme.

Owner-routed calls open a new connection per attempt: a `reuseport` listener
hashes each new connection to a worker, so only a fresh source port can reach
the stream's owner after a 409/404.  Retries are bounded by time, not count.
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

OWNER_RETRY_BUDGET = 12.0
OWNER_RETRY_PAUSE = 0.05
TIMEOUT = 10.0

SRT, RTMP, HLS_PUSH = "srt", "rtmp", "hls_push"


class ApiError(RuntimeError):
    """An API call that failed in a way the caller is expected to report."""

    def __init__(self, message, status=None, body=""):
        super().__init__(message)
        self.status = status
        self.body = body


def destination_id(program_index: int, destination_index: int,
                   program_count: int) -> str:
    """Must agree with the ids the harness's receivers expect (harness/receivers.py)."""
    if program_count == 1:
        return "d%04d" % destination_index
    return "p%04d-d%04d" % (program_index, destination_index)


def sink_port(base_port: int, program_index: int, attempt: int,
              destinations_per_program: int, listeners: int) -> int:
    """Round-robin a destination over the receiver's listeners."""
    return base_port + (program_index * destinations_per_program + attempt) % listeners


class Client:
    """A connection-reusing HTTP client for one API base URL."""

    def __init__(self, base, timeout=TIMEOUT, workers=1,
                 budget=OWNER_RETRY_BUDGET):
        self.base = base.rstrip("/")
        self.timeout = timeout
        self.workers = max(1, int(workers))
        self.budget = float(budget)
        self._parsed = urllib.parse.urlsplit(self.base)
        self._conn = None

    def _connection(self):
        if self._conn is None:
            cls = (http.client.HTTPSConnection if self._parsed.scheme == "https"
                   else http.client.HTTPConnection)
            self._conn = cls(self._parsed.netloc, timeout=self.timeout)
        return self._conn

    def _drop(self):
        if self._conn is not None:
            try:
                self._conn.close()
            except OSError:
                pass
            self._conn = None

    def request(self, method, path, body=None, headers=None, fresh=False,
                timeout=None):
        """One request.  `fresh` forces a new connection (re-rolls the
        accept-time worker hash)."""
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
        for attempt in (0, 1):
            reused = self._conn is not None
            try:
                conn = self._connection()
                if timeout is not None:
                    conn.timeout = timeout
                if payload is not None:
                    head["Content-Length"] = str(len(payload))
                try:
                    conn.request(method, url, body=payload, headers=head)
                except (BrokenPipeError, ConnectionResetError):
                    # The server closed an idle keep-alive connection; the
                    # request was never delivered, so one resend is safe.
                    if reused and attempt == 0:
                        self._drop()
                        continue
                    raise
                response = conn.getresponse()
                data = response.read()
                status = response.status
                if response.will_close:
                    self._drop()
            except Exception as exc:
                self._drop()
                raise ApiError("%s %s: %s" % (method, path, exc)) from exc
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
        """POST to a stream-scoped endpoint, retrying 409/404 until a deadline.

        Misses up to two per worker are retried at once (each is a fresh
        hash); past that the owner is presumed busy and retries are paced.
        """
        budget = self.budget if budget is None else float(budget)
        deadline = time.monotonic() + budget
        misses = 0
        last = None

        while (remaining := deadline - time.monotonic()) > 0:
            # Never let one attempt's socket timeout overrun the budget.
            status, data = self.request("POST", path, body, fresh=True,
                                        timeout=min(self.timeout, remaining))
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
                  budget=args.retry_budget)


def read_cookie_jar(path):
    """`name=value` pairs from a Netscape cookie jar.

    curl prefixes HttpOnly cookies (restream's session) with `#HttpOnly_`;
    they are credentials, not comments.
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


def rtmp_sink_http_port(base, index):
    """The API port of RTMP sink `index`."""
    return base + 192 + index


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
    """(path, body) receiver-side calls needed before an RTMP destination."""
    if kind != RTMP:
        return []
    return [
        ("/streams", {"application": "live", "name": sink_id}),
        ("/streams/live/%s/sources" % sink_id,
         {"id": sink_id, "type": "rtmp", "key": sink_id}),
    ]


class Program:
    """One program's destinations, numbered SRT, then RTMP, then HLS push."""

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
             hls_push_port, receiver, hls_path):
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
                api = "http://%s:%d/media/api/v1" % (
                    receiver, rtmp_sink_http_port(rtmp_base, index))
                out.append((kind, sink_id, rtmp_sink_port(rtmp_base, index), api))
            else:
                out.append((kind, sink_id, hls_push_port, None))
        return out


def provision(engine, client, program, plan, receiver, path="",
              pipeline_id="", cookie="", stall_id="", sinks=None):
    """Provision every destination of one program; the receiver side of each
    is prepared before the sender is told to use it."""
    headers = {"Cookie": cookie} if cookie else {}
    added = 0
    stall_seen = False

    for kind, sink_id, port, receiver_api in plan:
        if sink_id == stall_id:
            stall_seen = True

        if receiver_api and sinks is not None:
            sink = sinks.get(receiver_api)
            if sink is None:
                sink = sinks[receiver_api] = Client(receiver_api)
            for request_path, body in receiver_requests(kind, sink_id):
                sink.post(request_path, body)

        if engine == "restream":
            # A fresh connection per POST: restream closes idle keep-alive
            # connections, and a POST lost after the send cannot be resent.
            url = restream_output_url(kind, sink_id, receiver, port,
                                      program.stream, path)
            raw = client.post("/pipelines/%s/outputs" % pipeline_id, {
                "name": sink_id,
                "url": url,
                "config": {"video": {"mode": "source"},
                           "audio": {"mode": "all"}},
            }, headers=headers, fresh=True)
            output_id = _output_id(sink_id, raw)
            client.post(
                "/pipelines/%s/outputs/%s/start" % (pipeline_id, output_id),
                {}, headers=headers, fresh=True)
        else:
            client.post_owner(
                "/streams/%s/destinations" % program.stream,
                nginx_destination_body(kind, sink_id, receiver, port,
                                       program.stream, path))
        added += 1

    return added, stall_seen


def _output_id(sink_id, raw):
    """A restream output id; missing is an error (the output would never start)."""
    try:
        return json.loads(raw)["output"]["id"]
    except (ValueError, KeyError, TypeError) as exc:
        raise ApiError("restream returned no output id for %s: %s"
                       % (sink_id, raw[:200])) from exc


def _parse_spec(text):
    """`kind:count` -> (kind, count)."""
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
        hls_path=args.path,
    )

    cookie = read_cookie_jar(args.cookie_file) if args.cookie_file else ""
    sinks = {} if args.rtmp_sinks else None
    with _client_from(args) as client:
        try:
            added, stall_seen = provision(
                args.engine, client, program, plan, args.receiver_addr,
                path=args.path, pipeline_id=args.pipeline, cookie=cookie,
                stall_id=args.stall_id, sinks=sinks)
        finally:
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


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--api", default="http://127.0.0.1:19999/media/api/v1")
    parser.add_argument("--timeout", type=float, default=TIMEOUT)
    parser.add_argument("--workers", type=int, default=1,
                        help="sender worker count; sizes the owner-miss "
                             "tolerance")
    parser.add_argument("--retry-budget", type=float, default=OWNER_RETRY_BUDGET)
    sub = parser.add_subparsers(dest="command", required=True)

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

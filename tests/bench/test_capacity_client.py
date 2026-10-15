#!/usr/bin/env python3
"""Tests for the capacity data plane: the scheme, the bodies, and the retries.

The client exists because the harness's failures cluster in the half that
builds request bodies and talks HTTP.  The behaviours pinned here are the ones
that fail *silently* rather than loudly:

  * the destination id scheme, which the receiver slot mapping, the stall
    contract and every report all key off, and which exists in two languages
  * the request body of each of the three destination kinds, for each of the
    two engines -- a wrong field is a misroute, not an error
  * owner routing: a miss must be retried on a *new connection*, because a
    reused one is pinned to the worker that already refused it, and the loop
    must end at a deadline rather than after a fixed number of tries
  * a restream response with no output id, which used to leave an output
    created but never started, so a rung could be measured against
    destinations that were never in use

Run it with plain python3 - no pytest, no nginx:

    python3 tests/bench/test_capacity_client.py
"""

import http.server
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(HERE))

_client = importlib.util.spec_from_file_location(
    "capacity_client", os.path.join(HERE, "capacity_client.py"))
cc = importlib.util.module_from_spec(_client)
_client.loader.exec_module(cc)

FAILURES = []


def check(condition, label, detail=""):
    if condition:
        print("  ok   %s" % label)
    else:
        print("  FAIL %s %s" % (label, detail))
        FAILURES.append(label)


def raises(call, label, exception=cc.ApiError, contains=""):
    try:
        call()
    except exception as exc:
        if contains and contains not in str(exc):
            check(False, label, "message %r lacks %r" % (str(exc), contains))
        else:
            check(True, label)
        return
    except Exception as exc:                       # wrong type is a failure
        check(False, label, "raised %s: %s" % (type(exc).__name__, exc))
        return
    check(False, label, "did not raise")


class Stub(http.server.BaseHTTPRequestHandler):
    """An API whose first `misses` answers are refusals.

    Records the client port of every request, which is what makes it possible
    to tell a fresh connection from a reused one.
    """

    protocol_version = "HTTP/1.1"
    replies = []            # status codes, consumed in order
    seen = []               # (path, body, client_port)
    lock = threading.Lock()

    def do_POST(self):                              # noqa: N802
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode() if length else ""
        with self.lock:
            self.seen.append((self.path, body, self.client_address[1]))
            status = self.replies.pop(0) if self.replies else 200
        payload = b'{"ok":true}'
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):                               # noqa: N802
        with self.lock:
            self.seen.append((self.path, "", self.client_address[1]))
        payload = b'{"ok":true}'
        self.send_response(200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


class Server:
    def __init__(self):
        Stub.replies = []
        Stub.seen = []
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       daemon=True)
        self.thread.start()

    @property
    def url(self):
        return "http://127.0.0.1:%d" % self.port

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


# -- the destination scheme ------------------------------------------------

def test_destination_id_matches_shell():
    """The scheme is defined twice; a divergence corrupts results silently."""
    print("\ndestination id parity with the harness")
    result = subprocess.run(
        [sys.executable, os.path.join(HERE, "capacity_client.py"), "self-test"],
        text=True, capture_output=True)
    check(result.returncode == 0, "python agrees with the shell function",
          result.stderr.strip()[:200])

    # And the parity check is not vacuous: a one-character change must fail.
    source = open(os.path.join(HERE, "capacity_client.py")).read()
    mutated = source.replace('return "d%04d" % destination_index',
                             'return "d%05d" % destination_index')
    check(mutated != source, "the mutation applies")
    patched = os.path.join(HERE, ".capacity_client.mutant.py")
    try:
        with open(patched, "w") as handle:
            handle.write(mutated)
        mutant = subprocess.run(
            [sys.executable, patched, "self-test"],
            text=True, capture_output=True)
        check(mutant.returncode != 0,
              "a mutated scheme is rejected by the same check")
    finally:
        if os.path.exists(patched):
            os.remove(patched)


def test_sink_port():
    print("\nlistener mapping")
    check(cc.sink_port(9000, 0, 0, 64, 4) == 9000, "first destination uses the base")
    check(cc.sink_port(9000, 0, 4, 64, 4) == 9000, "round-robins to the base again")
    check(cc.sink_port(9000, 0, 3, 64, 4) == 9003, "last listener before wrapping")
    # A second program starts one stride past the first, so two programs do not
    # land on the same listener in lockstep.
    check(cc.sink_port(9000, 1, 0, 64, 8) == 9000,
          "a stride of 64 with 8 listeners wraps to the base")
    check(cc.sink_port(9000, 1, 1, 3, 8) == 9004,
          "the stride is the program's own destination count")


def test_rtmp_ports():
    print("\nrtmp sink port ranges")
    check(cc.rtmp_sink_port(20000, 0) == 20128, "media port base")
    check(cc.rtmp_sink_http_port(20000, 0) == 20192, "api port base")
    check(cc.rtmp_sink_http_port(20000, 3, True) == 20259,
          "an external receiver config uses the other api range")


# -- the request bodies ----------------------------------------------------

def test_bodies():
    print("\nrequest bodies")
    srt = cc.nginx_destination_body(cc.SRT, "d0007", "10.1.1.1", 9100,
                                    "live/a", "")
    check(srt["type"] == "srt", "srt type")
    check(srt["port"] == 9100, "srt port is a number, not a string")
    check(srt["streamid"] == "#!::r=live/a,m=publish,s=d0007",
          "srt streamid names the receiving stream and the source", srt["streamid"])

    rtmp = cc.nginx_destination_body(cc.RTMP, "d0008", "10.1.1.1", 20128,
                                     "live/a", "")
    check(rtmp["host"] == "10.1.1.1", "rtmp host is bare, without a scheme")
    check(rtmp["streamid"] == "d0008", "rtmp carries the key as the streamid")

    hls = cc.nginx_destination_body(cc.HLS_PUSH, "d0009", "10.1.1.1", 8080,
                                    "live/a", "/run/hls/live/a")
    check(hls["host"] == "http://10.1.1.1:8080/d0009/",
          "hls push carries a scheme and a path segment", hls["host"])
    check(hls["path"] == "/run/hls/live/a", "hls push names the local source path")

    raises(lambda: cc.nginx_destination_body("udp", "d1", "h", 1, "s"),
           "an unknown kind is refused rather than sent")

    # restream addresses the same three kinds as URLs.
    check(cc.restream_output_url(cc.SRT, "d1", "10.1.1.1", 9100, "live/a")
          == "srt://10.1.1.1:9100?streamid=#!::m=publish,r=live/a,s=d1",
          "restream srt url")
    check(cc.restream_output_url(cc.RTMP, "d1", "10.1.1.1", 20128, "live/a")
          == "rtmp://10.1.1.1:20128/live/d1", "restream rtmp url")
    check(cc.restream_output_url(cc.HLS_PUSH, "d1", "10.1.1.1", 8080, "live/a")
          == "http://10.1.1.1:8080/d1/index.m3u8", "restream hls push url")

    # Only RTMP needs the receiver prepared first.
    check(cc.receiver_requests(cc.SRT, "d1") == [],
          "an srt destination needs nothing from the receiver")
    check(cc.receiver_requests(cc.RTMP, "d1")[0] ==
          ("/streams", {"application": "live", "name": "d1"}),
          "an rtmp destination creates the receiver stream")
    check(len(cc.receiver_requests(cc.RTMP, "d1")) == 2,
          "and then the source that accepts the push")


def test_program_numbering():
    """SRT, then RTMP, then HLS push: the order every report depends on."""
    print("\nprogram destination numbering")
    program = cc.Program(0, 1, "live/a")
    program.add(cc.SRT, 3, first=0)
    program.add(cc.RTMP, 2, first=3)
    program.add(cc.HLS_PUSH, 1, first=5)
    plan = program.plan(srt_base_port=9000, srt_listeners=4, rtmp_base=20000,
                        rtmp_sinks=2, hls_push_port=8080,
                        receiver="10.0.0.9", receiver_config=False, hls_path="")
    check([entry[0] for entry in plan] ==
          [cc.SRT] * 3 + [cc.RTMP] * 2 + [cc.HLS_PUSH],
          "kinds appear in the harness's order")
    check([entry[1] for entry in plan] ==
          ["d0000", "d0001", "d0002", "d0003", "d0004", "d0005"],
          "ids are consecutive across the kinds, not per kind")
    # RTMP sink index counts from the start of the RTMP run, not from zero.
    check([e[2] for e in plan if e[0] == cc.RTMP] == [20128, 20129],
          "rtmp destinations walk the sinks from index 0")
    check(all(e[2] == 9000 + (i % 4) for i, e in enumerate(plan[:3])),
          "srt destinations round-robin the listeners")
    check(plan[3][3] == "http://10.0.0.9:20192/media/api/v1",
          "an rtmp destination carries its own receiver api", plan[3][3])

    multi = cc.Program(1, 2, "live/b")
    multi.add(cc.SRT, 2, first=0)
    plan = multi.plan(9000, 4, 20000, 1, 8080, "10.0.0.9", False, "")
    check([e[1] for e in plan] == ["p0001-d0000", "p0001-d0001"],
          "with more than one program the ids carry the program index")


# -- owner routing ---------------------------------------------------------

def test_owner_retry():
    """Immediate retries, a fresh connection each time, a deadline at the end."""
    print("\nowner routing")
    server = Server()
    try:
        # Two refusals, then success: the retry must not sleep 0.2s per miss.
        Stub.replies = [409, 409, 200]
        client = cc.Client(server.url, workers=4)
        started = time.monotonic()
        client.post_owner("/streams/live/a/destinations", {"id": "d1"})
        elapsed = time.monotonic() - started
        check(len(Stub.seen) == 3, "two refusals then success took three calls")
        # Two misses is below the 2*workers tolerance, so nothing slept.
        check(elapsed < 0.05,
              "misses below the tolerance are retried immediately",
              "took %.3fs" % elapsed)

        ports = [entry[2] for entry in Stub.seen]
        check(len(set(ports)) == len(ports),
              "every attempt used a new connection, which re-rolls the "
              "reuseport worker hash", ports)

        # A 404 is a miss too: the owner may not see the stream yet.
        Stub.replies = [404, 200]
        Stub.seen = []
        cc.Client(server.url, workers=4).post_owner("/x", {"id": "d1"})
        check(len(Stub.seen) == 2, "404 is retried like 409")

        # A status that means something else must not be retried.
        Stub.replies = [500, 200]
        Stub.seen = []
        raises(lambda: cc.Client(server.url, workers=4).post_owner("/x", {"id": "d"}),
               "a 500 is reported, not retried", contains="500")
        check(len(Stub.seen) == 1, "and it is not retried")

        # Past the tolerance, a pause is inserted rather than spinning.
        Stub.replies = [409] * 3 + [200]
        Stub.seen = []
        started = time.monotonic()
        cc.Client(server.url, workers=1).post_owner("/x", {"id": "d"})
        elapsed = time.monotonic() - started
        check(elapsed >= cc.OWNER_RETRY_PAUSE,
              "misses past 2*workers are paced", "took %.3fs" % elapsed)

        # The loop is bounded by time: a budget of zero fails at once.
        Stub.replies = [409] * 100
        Stub.seen = []
        raises(lambda: cc.Client(server.url, workers=1, budget=0.2)
               .post_owner("/x", {"id": "d"}),
               "an owner that never answers exhausts the budget",
               contains="never reached the owner")
        check(len(Stub.seen) >= 1, "and it did try")

        # A connection that is reused would never change workers, so the
        # plain (non-owner) calls must reuse one, and did above.
        Stub.replies = []
        Stub.seen = []
        reused = cc.Client(server.url, workers=1)
        reused.get("/a")
        reused.get("/b")
        check(len({entry[2] for entry in Stub.seen}) == 1,
              "ordinary calls keep one connection instead of one per call")
        reused.close()
    finally:
        server.stop()


def test_destination_idempotence():
    """An owner POST carries the id, so a retry cannot duplicate a destination."""
    print("\nrequest content")
    server = Server()
    try:
        Stub.replies = [409, 200]
        Stub.seen = []
        cc.Client(server.url, workers=4).post_owner(
            "/streams/live/a/destinations",
            cc.nginx_destination_body(cc.SRT, "d0042", "10.0.0.9", 9100,
                                      "live/a"))
        bodies = [entry[1] for entry in Stub.seen]
        check(len(bodies) == 2 and bodies[0] == bodies[1],
              "a retried attempt sends the same body, id included")
        check(json.loads(bodies[0])["id"] == "d0042", "with the caller's id")
    finally:
        server.stop()


# -- restream output ids ---------------------------------------------------

def test_output_id():
    print("\nrestream output ids")
    check(cc._output_id("d1", b'{"output":{"id":"abc"}}') == "abc",
          "the assigned id is read")
    # The old shell discarded these cases and carried on, leaving the output
    # created and never started.
    raises(lambda: cc._output_id("d1", b"not json"), "a non-JSON body is refused")
    raises(lambda: cc._output_id("d1", b'{"output":{}}'),
           "a body with no id is refused")
    raises(lambda: cc._output_id("d1", b'{"error":"nope"}'),
           "an error body is refused")


def test_cookie_jar():
    print("\ncookie jar")
    path = os.path.join(HERE, ".cookie.test.txt")
    try:
        with open(path, "w") as handle:
            handle.write("# Netscape HTTP Cookie File\n")
            handle.write("127.0.0.1\tFALSE\t/\tFALSE\t0\tsession\ts3cret\n")
            handle.write("127.0.0.1\tFALSE\t/\tFALSE\t0\tother\tv\n")
        value = cc.read_cookie_jar(path)
        check("session=s3cret" in value, "the session cookie is read")
        check("other=v" in value, "and every other cookie with it")
        check(value.startswith("session="), "comments are skipped", value)
    finally:
        if os.path.exists(path):
            os.remove(path)


def main():
    print("capacity_client: data plane, bodies and retries")
    test_destination_id_matches_shell()
    test_sink_port()
    test_rtmp_ports()
    test_bodies()
    test_program_numbering()
    test_owner_retry()
    test_destination_idempotence()
    test_output_id()
    test_cookie_jar()

    print()
    if FAILURES:
        print("FAILED: %d check(s): %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

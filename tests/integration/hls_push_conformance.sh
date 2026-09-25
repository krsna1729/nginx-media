#!/usr/bin/env bash
#
# HLS push conformance: what an hls_push destination sends, checked against
# the contract YouTube's HLS ingest and the DASH-IF Live Media Ingest
# specification (Interface-2) publish - see docs/hls-push-interop.md.
#
# Three destinations on one stream, each to its own recording sink (an
# HTTP/1.1 server that keeps connections open, logs every request and can be
# told to fail):
#
#   yt    the youtube_live profile, over https, to a YouTube-shaped endpoint
#         (http_upload_hls?cid=KEY&copy=0&file=NAME).  The profile has to
#         reach the wire: POST, 1-4 s segments, at most five in every
#         playlist, EXT-X-TARGETDURATION within 4, no DELETE, a playlist
#         after every segment and never before the segments it lists, and
#         the requests on a few kept connections rather than one each.
#   gen   a generic origin with a window of 3 and expiry: PUT, playlists of
#         three with the media sequence advancing, and a DELETE for each
#         segment once a playlist no longer lists it.
#   bad   an origin that fails: one segment is refused every time (503), so
#         it is retried within its budget, given up on, and listed in later
#         playlists as EXT-X-GAP; another is refused once and then accepted,
#         so it is retried and is not a gap.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/hls-push-conformance"
SRT_PORT=24670
HTTP_PORT=18500
YT_PORT=18501
GEN_PORT=18502
BAD_PORT=18503

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/yt" "$RUN/gen" "$RUN/bad"

PIDS=()

stop_instance() {
    local pid child

    [ -f "$RUN/logs/nginx.pid" ] || return 0
    pid="$(cat "$RUN/logs/nginx.pid")"
    kill -QUIT "$pid" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done
    for child in $(pgrep -P "$pid" 2>/dev/null); do
        kill -KILL "$child" 2>/dev/null
    done
    kill -KILL "$pid" 2>/dev/null
    return 0
}

cleanup() {
    local pid
    for pid in "${PIDS[@]}"; do
        kill -KILL "$pid" 2>/dev/null
    done
    stop_instance
    return 0
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    grep -a 'hls push\|hls segmentation' "$RUN/logs/error.log" | tail -8 >&2
    exit 1
}

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$RUN/key.pem" -out "$RUN/cert.pem" \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" \
    >"$RUN/openssl.log" 2>&1 || fail "certificate generation failed"

cat > "$RUN/sink.py" <<'PYEOF'
import http.server, json, os, ssl, sys, threading, time, urllib.parse

root, port, control = sys.argv[1], int(sys.argv[2]), sys.argv[3]
cert = sys.argv[4] if len(sys.argv) > 4 else None
key = sys.argv[5] if len(sys.argv) > 5 else None

lock = threading.Lock()
log = open(os.path.join(root, "requests.jsonl"), "a", buffering=1)
state = {"always": None, "once": None, "once_done": False}


def object_name(path):
    parts = urllib.parse.urlsplit(path)
    query = urllib.parse.parse_qs(parts.query)
    if "file" in query:
        return query["file"][-1]
    return os.path.basename(parts.path)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"          # keep-alive, as an origin would

    def answer(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def record(self, name, status, size):
        with lock:
            log.write(json.dumps({
                "t": time.time(), "conn": "%s:%d" % self.client_address,
                "method": self.command, "path": self.path, "name": name,
                "status": status, "bytes": size,
                "type": self.headers.get("Content-Type", ""),
                "agent": self.headers.get("User-Agent", "")}) + "\n")

    def upload(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        name = object_name(self.path)
        status = 201

        with lock:
            mode = open(control).read().strip() if os.path.exists(control) else ""
            if name.endswith(".ts"):
                if mode == "fail-always" and state["always"] is None:
                    state["always"] = name
                    open(os.path.join(root, "always"), "w").write(name)
                if mode == "fail-once" and state["once"] is None:
                    state["once"] = name
                    open(os.path.join(root, "once"), "w").write(name)
            if name == state["always"]:
                status = 503
            elif name == state["once"] and not state["once_done"]:
                state["once_done"] = True
                status = 503

        if status == 201:
            target = os.path.join(root, "objects", name)
            if name.endswith(".m3u8"):
                # every playlist as it arrived, in order
                seq = len(os.listdir(os.path.join(root, "playlists")))
                with open(os.path.join(root, "playlists", "%06d.m3u8" % seq),
                          "wb") as f:
                    f.write(body)
            existed = os.path.exists(target)
            with open(target, "wb") as f:
                f.write(body)
            status = 204 if existed else 201

        self.record(name, status, len(body))
        self.answer(status)

    do_PUT = upload
    do_POST = upload

    def do_DELETE(self):
        name = object_name(self.path)
        target = os.path.join(root, "objects", name)
        status = 404
        if os.path.exists(target):
            os.unlink(target)
            status = 200
        self.record(name, status, 0)
        self.answer(status)

    def log_message(self, *args):
        pass


os.makedirs(os.path.join(root, "objects"), exist_ok=True)
os.makedirs(os.path.join(root, "playlists"), exist_ok=True)
server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
if cert:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)
server.serve_forever()
PYEOF

python3 "$RUN/sink.py" "$RUN/yt" "$YT_PORT" "$RUN/yt.mode" \
    "$RUN/cert.pem" "$RUN/key.pem" &
PIDS+=($!)
python3 "$RUN/sink.py" "$RUN/gen" "$GEN_PORT" "$RUN/gen.mode" &
PIDS+=($!)
python3 "$RUN/sink.py" "$RUN/bad" "$BAD_PORT" "$RUN/bad.mode" &
PIDS+=($!)
sleep 0.5

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/hls;
media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;

http {
    access_log off;
    server {
        listen 127.0.0.1:$HTTP_PORT;
        location /media/api/ { media_api; }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 || fail "config rejected"
"$NGINX" -p "$RUN" -c conf/nginx.conf || fail "nginx did not start"
sleep 0.5

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"conf"}' "$API/streams" >/dev/null \
    || fail "stream not created"

add() {   # <id> <json>
    local status
    status="$(curl -sS -o "$RUN/$1.json" -w '%{http_code}' \
        -X POST -H 'Content-Type: application/json' -d "$2" \
        "$API/streams/live/conf/destinations")"
    cat "$RUN/$1.json"; echo
    [ "$status" = "201" ] || fail "destination $1: expected 201, got $status"
}

echo "== invalid generic settings are refused"
code="$(curl -sS -o "$RUN/inv.json" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d '{"id":"inv","type":"hls_push","host":"http://127.0.0.1:1/","path":"'"$RUN"'/hls/live/conf","playlist_window":64}' \
    "$API/streams/live/conf/destinations")"
[ "$code" = "400" ] && grep -q invalid_hls_push "$RUN/inv.json" \
    || fail "a window of 64 was not refused: $code $(cat "$RUN/inv.json")"
code="$(curl -sS -o "$RUN/inv.json" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d '{"id":"inv","type":"hls_push","host":"http://127.0.0.1:1/","path":"'"$RUN"'/hls/live/conf","method":"PATCH"}' \
    "$API/streams/live/conf/destinations")"
[ "$code" = "400" ] || fail "method PATCH was not refused: $code"

echo "== a generic destination asking for 4 s segments, then publishing"
add gen '{"id":"gen","type":"hls_push","host":"http://127.0.0.1:'"$GEN_PORT"'/live/conf/",
  "path":"'"$RUN"'/hls/live/conf","playlist_window":3,"segment_duration_ms":4000}'
for want in '"playlist_window":3' '"method":"PUT"' '"delete_expired":true' \
            '"segment_duration_ms":4000' '"segment_max_ms":8000'
do
    grep -qF "$want" "$RUN/gen.json" || fail "gen does not report $want"
done

timeout 90 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -t 44 -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/conf,m=publish,s=encoder-a" \
    >"$RUN/pub.log" 2>&1 &
PUBLISHER=$!
PIDS+=($PUBLISHER)

grep_wait() {   # <file> <pattern> <tenths>
    for _ in $(seq 1 "$3"); do
        grep -qa -- "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

grep_wait "$RUN/logs/error.log" 'hls segmentation for live/conf is now 4000-8000 ms, window 5' 300 \
    || fail "the segmenter did not follow the generic destination's 4000 ms"
echo "   segmentation followed gen: 4000-8000 ms"
sleep 6

echo "== the youtube_live profile joins: the segmenter tightens to it"
add yt '{"id":"yt","type":"hls_push","profile":"youtube_live",
  "host":"https://127.0.0.1:'"$YT_PORT"'/http_upload_hls?cid=SECRETKEY&copy=0&file=",
  "ca_file":"'"$RUN"'/cert.pem","path":"'"$RUN"'/hls/live/conf"}'
add bad '{"id":"bad","type":"hls_push","host":"http://127.0.0.1:'"$BAD_PORT"'/live/conf/",
  "path":"'"$RUN"'/hls/live/conf","delete_expired":false}'

for want in '"profile":"youtube_live"' '"segment_duration_ms":2000' \
            '"segment_max_ms":4000' '"playlist_window":5' '"method":"POST"' \
            '"delete_expired":false'
do
    grep -qF "$want" "$RUN/yt.json" || fail "yt does not report $want"
done
grep -qF 'SECRETKEY' "$RUN/yt.json" && fail "the endpoint's key was reported"

grep_wait "$RUN/logs/error.log" 'hls segmentation for live/conf is now 2000-4000 ms, window 5' 30 \
    || fail "the segmenter was not set to the youtube profile's 2000-4000 ms"
echo "   segmentation followed yt: 2000-4000 ms"

sleep 8
echo "== failures: one segment refused every time"
echo fail-always > "$RUN/bad.mode"
grep_wait "$RUN/bad/always" '.ts' 100 || fail "no segment was chosen to fail"
sleep 8
echo "== failures: one segment refused once"
echo fail-once > "$RUN/bad.mode"
grep_wait "$RUN/bad/once" '.ts' 100 || fail "no segment was chosen to fail once"

wait "$PUBLISHER"
sleep 6

echo "== checking what each endpoint received"
python3 - "$RUN" <<'PYEOF' || fail "conformance checks failed"
import json, os, re, sys

run = sys.argv[1]
ok = True

def check(cond, what):
    global ok
    print(("   ok   " if cond else "   FAIL ") + what)
    ok = ok and cond

def load(dest):
    return [json.loads(l) for l in open(os.path.join(run, dest, "requests.jsonl"))]

def playlists(dest):
    d = os.path.join(run, dest, "playlists")
    return [open(os.path.join(d, f)).read() for f in sorted(os.listdir(d))]

def entries(text):
    lines = text.splitlines()
    out, extinf, gap = [], None, False
    for l in lines:
        if l.startswith("#EXTINF:"):
            extinf = float(l[8:].split(",")[0])
        elif l == "#EXT-X-GAP":
            gap = True
        elif l and not l.startswith("#"):
            out.append((l, extinf, gap))
            extinf, gap = None, False
    return out

def tag(text, name):
    m = re.search(r"^#%s:(\d+)" % name, text, re.M)
    return int(m.group(1)) if m else None

def listed_before_upload(dest):
    reqs = load(dest)
    pls = playlists(dest)
    have, bad, pi = set(), 0, 0
    for r in reqs:
        if r["method"] not in ("PUT", "POST") or r["status"] not in (201, 204):
            continue
        if r["name"].endswith(".ts"):
            have.add(r["name"])
        elif r["name"].endswith(".m3u8"):
            for uri, _, gap in entries(pls[pi]):
                if not gap and uri not in have:
                    bad += 1
            pi += 1
    return bad

# --- yt: the youtube_live profile on the wire
reqs = load("yt")
uploads = [r for r in reqs if r["method"] != "DELETE"]
segs = [r for r in uploads if r["name"].endswith(".ts")]
pls = playlists("yt")
conns = {r["conn"] for r in reqs}
print("yt: %d requests, %d segments, %d playlists, %d connections"
      % (len(reqs), len(segs), len(pls), len(conns)))
check(len(segs) >= 12, "yt received at least 12 segments")
check(all(r["method"] == "POST" for r in reqs), "yt: every request is POST")
check(all(r["path"].startswith("/http_upload_hls?cid=SECRETKEY&copy=0&file=")
          for r in reqs), "yt: the object is named in the file= parameter")
check(all(r["agent"] == "nginx-media" for r in reqs), "yt: User-Agent names the source")
check(all(r["type"] == ("video/mp2t" if r["name"].endswith(".ts")
                        else "application/vnd.apple.mpegurl") for r in uploads),
      "yt: Content-Type by object")
check(len(pls) >= len(segs) - 1, "yt: a playlist after every segment")
check(all(len(entries(p)) <= 5 for p in pls), "yt: at most 5 segments in every playlist")
check(all(tag(p, "EXT-X-TARGETDURATION") <= 4 for p in pls), "yt: EXT-X-TARGETDURATION <= 4")
durs = {u: d for p in pls for u, d, _ in entries(p)}
last = entries(pls[-1])[-1][0]            # the end of stream may be short
steady = sorted(d for u, d in durs.items() if u != last)
check(all(d <= 4.0 for d in durs.values()), "yt: every segment <= 4 s (max %.3f)"
      % max(durs.values()))
check(all(d >= 1.0 for d in steady), "yt: every segment >= 1 s (min %.3f)" % steady[0])
check(abs(steady[len(steady) // 2] - 2.0) < 0.05,
      "yt: segments are the 2 s asked for (median %.3f)" % steady[len(steady) // 2])
check(listed_before_upload("yt") == 0, "yt: no playlist before the segments it lists")
check(len(conns) <= 3, "yt: requests on kept connections (%d for %d requests)"
      % (len(conns), len(reqs)))
check(not any(r["method"] == "DELETE" for r in reqs), "yt: no DELETE")

# --- gen: window 3 and expiry
reqs = load("gen")
pls = playlists("gen")
dels = [r for r in reqs if r["method"] == "DELETE"]
segs = [r for r in reqs if r["method"] == "PUT" and r["name"].endswith(".ts")]
print("gen: %d requests, %d segments, %d playlists, %d deletes, %d connections"
      % (len(reqs), len(segs), len(pls), len(dels), len({r["conn"] for r in reqs})))
check(all(r["method"] in ("PUT", "DELETE") for r in reqs), "gen: PUT and DELETE only")
check(all(len(entries(p)) <= 3 for p in pls), "gen: at most 3 segments in every playlist")
check(any(len(entries(p)) == 3 for p in pls), "gen: the window fills to 3")
seqs = [tag(p, "EXT-X-MEDIA-SEQUENCE") or 0 for p in pls]
check(all(b >= a for a, b in zip(seqs, seqs[1:])) and seqs[-1] > seqs[0],
      "gen: the media sequence only advances (%d -> %d)" % (seqs[0], seqs[-1]))
# the numbering a trimmed playlist keeps: first URI's number == media sequence
first = [(tag(p, "EXT-X-MEDIA-SEQUENCE") or 0, entries(p)[0][0]) for p in pls if entries(p)]
check(all(int(re.search(r"(\d+)\.ts$", u).group(1)) == s for s, u in first),
      "gen: every segment keeps its media sequence number")
check(len(dels) >= len(segs) - 5, "gen: expired segments are deleted (%d of %d)"
      % (len(dels), len(segs)))
check(all(r["status"] == 200 for r in dels), "gen: every DELETE named an object it had")
# a DELETE only after a playlist that no longer lists the segment
listed, bad = set(), 0
pi = 0
for r in reqs:
    if r["method"] == "PUT" and r["name"].endswith(".m3u8") and r["status"] in (201, 204):
        listed = {u for u, _, _ in entries(pls[pi])}
        pi += 1
    elif r["method"] == "DELETE" and r["name"] in listed:
        bad += 1
check(bad == 0, "gen: never deletes a segment the last playlist listed")
left = [f for f in os.listdir(os.path.join(run, "gen", "objects")) if f.endswith(".ts")]
check(len(left) <= 5, "gen: the origin holds %d segments, not the whole stream" % len(left))
check(listed_before_upload("gen") == 0, "gen: no playlist before the segments it lists")

# --- bad: retry, gap, recovery
reqs = load("bad")
pls = playlists("bad")
always = open(os.path.join(run, "bad", "always")).read()
once = open(os.path.join(run, "bad", "once")).read()
tries_always = [r for r in reqs if r["name"] == always]
tries_once = [r for r in reqs if r["name"] == once]
print("bad: %s tried %d times; %s tried %d times" % (always, len(tries_always),
                                                    once, len(tries_once)))
check(len(tries_always) >= 3, "bad: the refused segment was retried")
span = tries_always[-1]["t"] - tries_always[0]["t"]
check(span <= 2.5, "bad: retries stop within the segment budget (%.2f s)" % span)
gap_lists = [p for p in pls if "#EXT-X-GAP\n" + always + "\n" in p]
check(len(gap_lists) >= 1, "bad: later playlists list it as EXT-X-GAP (%d)" % len(gap_lists))
after = [r for r in reqs if r["name"].endswith(".ts") and r["status"] == 201
         and r["t"] > tries_always[-1]["t"]]
check(len(after) >= 3, "bad: later segments still arrive (%d)" % len(after))
check([r["status"] for r in tries_once][:2] == [503, 201],
      "bad: the segment refused once was sent again and accepted")
check(not any("#EXT-X-GAP\n" + once in p for p in pls), "bad: and it is not a gap")
check(not any(r["method"] == "DELETE" for r in reqs), "bad: delete_expired false sends no DELETE")
check(listed_before_upload("bad") == 0, "bad: no playlist before the segments it lists")

sys.exit(0 if ok else 1)
PYEOF

grep -qa "gave up on" "$RUN/logs/error.log" || fail "the give-up was not logged"
if grep -qaE '\[(alert|emerg)\]|signal [0-9]+|AddressSanitizer' "$RUN/logs/error.log"; then
    fail "worker reported an alert or crash"
fi

echo "== hls push conformance ok"

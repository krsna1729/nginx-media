#!/usr/bin/env bash
#
# HLS push source (normative revision, acceptance case 7): someone else PUTs
# segments to us and they become a source.
#
#   7. upload HLS into an HLS PUT source and make it eligible through the
#      normal health model
#
# The endpoint stores what it receives in a directory; a source of type
# hls_push watches that directory and demuxes what arrives.  Neither half
# knows about the other beyond the directory, so an upload arriving by any
# other means would work the same way.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/hls-ingest"
HTTP_PORT=18550
UPLOAD_PORT=18551

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/incoming"

# The master is stopped and waited for, and its children are then killed by
# parent: nginx rewrites a worker's argv to "nginx: worker process", so a
# pattern that matches the configuration path matches the master only, and a
# worker whose master was killed outright is reparented to init and keeps the
# ports the next run needs.
stop_instance() {
    local prefix="$1" pid child

    [ -f "$prefix/logs/nginx.pid" ] || return 0

    pid="$(cat "$prefix/logs/nginx.pid")"

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
    stop_instance "$RUN"
    return 0
}
trap cleanup EXIT

echo "== building segments to upload"
# real segments, cut at keyframes by ffmpeg's own segmenter: splitting a TS at
# arbitrary byte offsets produces fragments no demuxer can follow
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 12 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -f segment -segment_time 2 -segment_format mpegts \
    "$RUN/incoming/seg-%05d.ts" 2>/dev/null \
    || { echo "could not build the fixture" >&2; exit 1; }

ls -la "$RUN/incoming" | tail -4

[ "$(ls "$RUN/incoming"/*.ts 2>/dev/null | wc -l)" -ge 2 ] \
    || { echo "the segmenter produced too few segments" >&2; exit 1; }

ls -la "$RUN/incoming" | tail -4

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/hls;
media_ingest_secret $RUN/ingest.secret;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;
        location /media/api/ { media_api; }
    }

    server {
        listen 127.0.0.1:$UPLOAD_PORT;

        # the body is buffered to a file and renamed into the ingest
        # directory, so the reader never sees a partial segment
        client_body_temp_path $RUN/body;
        client_body_buffer_size 1k;

        location /ingest/ {
            access_log off;
            media_hls_ingest $RUN/uploaded;
        }

        # a second location, so a container with a stream type this build
        # cannot carry reaches a reader of its own instead of the one above
        location /ingest-odd/ {
            access_log off;
            media_hls_ingest $RUN/uploaded-odd;
        }
    }
}
EOF

mkdir -p "$RUN/uploaded" "$RUN/uploaded-odd" "$RUN/body"

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || {
    echo "configuration rejected" >&2
    exit 1
}

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
key_of() { python3 -c 'import json,sys; print(json.load(sys.stdin)["key"])'; }

assert_no_upload_bodies() {
    python3 - "$RUN/body" <<'PY' || exit 1
from pathlib import Path
import sys
files = [p for p in Path(sys.argv[1]).rglob("*") if p.is_file()]
if files:
    raise SystemExit(f"upload left temporary bodies: {files}")
PY
}

echo "== a stream and an hls push source"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"upload"}' "$API/streams" >/dev/null

STATUS="$(curl -sS -o "$RUN/src.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"uploader\",\"type\":\"hls_push\",\"path\":\"$RUN/uploaded/uploader\"}" \
    "$API/streams/live/upload/sources")"

python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); d.pop("key",None); print(json.dumps(d))' \
    "$RUN/src.json"
KEY="$(key_of < "$RUN/src.json")"

[ "$STATUS" = "201" ] || { echo "expected 201, got $STATUS" >&2; exit 1; }

curl -fsS "$API/streams/live/upload/sources" | grep -q '"id":"uploader"' \
    || { echo "the source is not registered" >&2; exit 1; }
CODE="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"unsafe\",\"type\":\"hls_push\",\"path\":\"$RUN/uploaded/unsafe\",\"key\":\"../outside\"}" \
    "$API/streams/live/upload/sources")"
[ "$CODE" = "400" ] || { echo "an unsafe HLS key got HTTP $CODE" >&2; exit 1; }
STATUS="$(curl -sS -o "$RUN/srt-src.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"id":"srt-key","type":"srt"}' "$API/streams/live/upload/sources")"
[ "$STATUS" = "201" ] || { echo "could not create SRT source: $STATUS" >&2; exit 1; }
SRT_KEY="$(key_of < "$RUN/srt-src.json")"
CODE="$(curl -sS -o /dev/null -w '%{http_code}' \
    -T "$RUN/incoming/seg-00000.ts" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest/$SRT_KEY/not-hls.ts")"
[ "$CODE" = "404" ] || { echo "an SRT key got HTTP $CODE at HLS ingest" >&2; exit 1; }
echo "== only the provisioned key can upload into this source"
CODE="$(curl -sS -o /dev/null -w '%{http_code}' \
    -T "$RUN/incoming/seg-00000.ts" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest/not-a-key/seg-00000.ts")"
[ "$CODE" = "404" ] || { echo "an unknown key got HTTP $CODE" >&2; exit 1; }
assert_no_upload_bodies

echo "== unknown keys are rejected before 100 Continue"
python3 - "$UPLOAD_PORT" <<'PY'
import socket, sys
with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10) as peer:
    peer.sendall(b"PUT /ingest/not-a-key/early.ts HTTP/1.1\r\n"
                 b"Host: localhost\r\nContent-Length: 16384\r\n"
                 b"Expect: 100-continue\r\nConnection: close\r\n\r\n")
    header = b""
    while b"\r\n\r\n" not in header:
        chunk = peer.recv(4096)
        if not chunk:
            raise SystemExit("upload admission returned no response")
        header += chunk
        if len(header) > 16384:
            raise SystemExit("upload admission returned oversized headers")
    status = header.split(b"\r\n", 1)[0].split()
    if len(status) < 2 or status[1] != b"404":
        raise SystemExit(f"expected immediate 404, got {header.splitlines()[0]!r}")
PY
[ "$?" = 0 ] || exit 1
assert_no_upload_bodies

echo "== rotation and client abort clean up an admitted body"
KEY="$(python3 - "$UPLOAD_PORT" "$HTTP_PORT" "$KEY" "$RUN" <<'PY'
import http.client, json, socket, sys, time
from pathlib import Path
upload_port, api_port = map(int, sys.argv[1:3])
old_key, root = sys.argv[3], Path(sys.argv[4])
body_dir = root / "body"
payload = b"x" * 16384

def wait_for_body(present):
    deadline = time.monotonic() + 10
    while bool([p for p in body_dir.rglob("*") if p.is_file()]) != present:
        if time.monotonic() >= deadline:
            raise RuntimeError(f"temporary-body presence did not become {present}")
        time.sleep(0.02)

def begin_upload(key, name):
    peer = socket.create_connection(("127.0.0.1", upload_port), timeout=10)
    headers = (f"PUT /ingest/{key}/{name} HTTP/1.1\r\nHost: localhost\r\n"
               f"Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n")
    peer.sendall(headers.encode() + payload[:4096])
    wait_for_body(True)
    return peer

with begin_upload(old_key, "rotated.ts") as peer:
    api = http.client.HTTPConnection("127.0.0.1", api_port, timeout=10)
    api.request("POST", "/media/api/v1/streams/live/upload/sources/uploader/rotate")
    response = api.getresponse()
    document = json.loads(response.read())
    if response.status != 200:
        raise RuntimeError(f"key rotation returned {response.status}")
    new_key = document["key"]
    if new_key == old_key:
        raise RuntimeError("key rotation reused the old key")
    api.close()
    peer.sendall(payload[4096:])
    response = http.client.HTTPResponse(peer)
    response.begin()
    response.read()
    if response.status != 404:
        raise RuntimeError(f"in-flight old-key upload returned {response.status}")
wait_for_body(False)
if (root / "uploaded/uploader/rotated.ts").exists():
    raise RuntimeError("in-flight old-key upload was published")

peer = begin_upload(new_key, "aborted.ts")
peer.close()
wait_for_body(False)
if (root / "uploaded/uploader/aborted.ts").exists():
    raise RuntimeError("incomplete upload was published")
print(new_key)
PY
)" || exit 1
assert_no_upload_bodies

echo "== rename failures do not retain temporary bodies"
mkdir -p "$RUN/uploaded/uploader/blocked.ts"
CODE="$(curl -sS -o /dev/null -w '%{http_code}' \
    -T "$RUN/incoming/seg-00000.ts" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest/$KEY/blocked.ts")"
[ "$CODE" = "500" ] || { echo "rename failure got HTTP $CODE" >&2; exit 1; }
rmdir "$RUN/uploaded/uploader/blocked.ts"
assert_no_upload_bodies

echo "== segments are uploaded"
UPLOADED=0

# Uploaded at roughly the rate they were produced, which is what a live
# uploader does.  Dumping twelve seconds of media in one burst is not a
# realistic test: the program feed is a bounded window, and a source that
# outruns every consumer simply overruns it.
for seg in "$RUN"/incoming/*.ts; do
    base="$(basename "$seg")"
    CODE="$(curl -sS -o /dev/null -w '%{http_code}' -T "$seg" \
        "http://127.0.0.1:$UPLOAD_PORT/ingest/$KEY/$base")"
    echo "   PUT $base -> $CODE"
    [ "$CODE" = "201" ] || { echo "upload of $base failed" >&2; exit 1; }
    UPLOADED=$(( UPLOADED + 1 ))
    sleep 2
done

echo "   stored: $(ls "$RUN/uploaded/uploader" | wc -l) of $UPLOADED segments"

[ "$(ls "$RUN/uploaded/uploader" | wc -l)" -eq "$UPLOADED" ] \
    || { echo "not every upload landed" >&2; exit 1; }
assert_no_upload_bodies

echo "== the source reads them through the normal health model"
for _ in $(seq 1 300); do
    curl -fsS "$API/streams/live/upload" \
        | grep -qE '"program_frames":[1-9]' && break
    sleep 0.1
done

FRAMES="$(curl -fsS "$API/streams/live/upload" \
    | grep -o '"program_frames":[0-9]*' | cut -d: -f2)"

echo "   program frames: ${FRAMES:-0}"

[ "${FRAMES:-0}" -gt 0 ] \
    || { echo "the uploaded segments produced no frames" >&2
         tail -5 "$RUN/logs/error.log" >&2; exit 1; }

SOURCES="$(curl -fsS "$API/streams/live/upload/sources")"
printf '%s\n' "$SOURCES"

printf '%s' "$SOURCES" | grep -q '"id":"uploader".*"healthy":true' \
    || { echo "the upload source is not healthy" >&2; exit 1; }

printf '%s' "$SOURCES" | grep -q '"id":"uploader".*"eligible":true' \
    || { echo "the upload source is not eligible" >&2; exit 1; }

echo "   the upload source is healthy and eligible"

for _ in $(seq 1 200); do
    [ -f "$RUN/hls/live/upload/index.m3u8" ] && break
    sleep 0.1
done

[ -f "$RUN/hls/live/upload/index.m3u8" ] \
    || { echo "the uploaded program produced no hls" >&2; exit 1; }

SEG="$(ls "$RUN"/hls/live/upload/*.ts 2>/dev/null | head -1)"
PROBE="$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries stream=codec_name,width,height -of csv=p=0 "$SEG" 2>&1)"

echo "   hls segment: $PROBE"

printf '%s' "$PROBE" | grep -q '^h264' \
    || { echo "the segment does not carry H.264" >&2; exit 1; }

# ---------------------------------------------------------------------------
# what an hls input may carry, and what happens when it carries something else
# ---------------------------------------------------------------------------

echo "== a segment in a stream type this build cannot carry is reported"

# MPEG-2 video in MPEG-TS: a real HLS segment, in a container this build
# demuxes, carrying a stream type it does not track
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 4 \
    -c:v mpeg2video -g 25 -f mpegts "$RUN/incoming/mpeg2.ts" 2>/dev/null \
    || { echo "could not build the unsupported fixture" >&2; exit 1; }

mkdir -p "$RUN/uploaded-odd"

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"odd"}' "$API/streams")"

[ "$STATUS" = "201" ] || [ "$STATUS" = "200" ] \
    || { echo "could not create the odd stream: $STATUS" >&2; exit 1; }

STATUS="$(curl -sS -o "$RUN/odd-src.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"odd-uploader\",\"type\":\"hls_push\",\"path\":\"$RUN/uploaded-odd/odd-uploader\"}" \
    "$API/streams/live/odd/sources")"

[ "$STATUS" = "201" ] \
    || { echo "could not create the odd source: $STATUS" >&2; exit 1; }
ODD_KEY="$(key_of < "$RUN/odd-src.json")"
CODE="$(curl -sS -o /dev/null -w '%{http_code}' \
    -T "$RUN/incoming/mpeg2.ts" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest/$ODD_KEY/mpeg2.ts")"
[ "$CODE" = "404" ] || { echo "a source outside this ingest root got HTTP $CODE" >&2; exit 1; }

CODE="$(curl -sS -o /dev/null -w '%{http_code}' -T "$RUN/incoming/mpeg2.ts" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest-odd/$ODD_KEY/mpeg2.ts")"

echo "   PUT mpeg2.ts -> $CODE"

[ "$CODE" = "201" ] || { echo "upload of the unsupported segment failed" >&2
                         exit 1; }

for _ in $(seq 1 200); do
    grep -q 'carries no stream type this build can carry' \
        "$RUN/logs/error.log" && break
    sleep 0.1
done

grep -q 'carries no stream type this build can carry' "$RUN/logs/error.log" \
    || { echo "an unsupported input was never reported" >&2
         tail -10 "$RUN/logs/error.log" >&2; exit 1; }

echo "   reported:"
grep -a 'carries no stream type this build can carry' \
    "$RUN/logs/error.log" | tail -1 | sed 's/^/   /'

# and it never carries media: the demux has nothing it can publish, so the
# source has no frames and never goes on air.  It reports as a source whose
# transport is up and which is not carrying media yet - the same state a
# publisher that connected and has not sent a keyframe is in - rather than as
# one that failed, because nothing about it failed: it is carrying something
# this build cannot read, and the line above is where that is said.
sleep 3

ODD="$(curl -fsS "$API/streams/live/odd/sources")"

printf '%s' "$ODD" | grep -q '"id":"odd-uploader".*"frames_in":0' \
    || { echo "an unsupported input published frames" >&2
         printf '%s\n' "$ODD" >&2; exit 1; }

printf '%s' "$ODD" | grep -q '"id":"odd-uploader".*"active":false' \
    || { echo "an unsupported input went on air" >&2
         printf '%s\n' "$ODD" >&2; exit 1; }

echo "   it published no frames and never went on air"

# ---------------------------------------------------------------------------
# a standard HLS push encoder: segment, then the media playlist naming it,
# every segment; unpadded numbered names; DELETE of expired segments.  This
# is the ingest shape of RFC 8216 publishing as YouTube's HLS ingest and the
# DASH-IF Live Media Ingest specification (Interface-2) describe it, and
# ffmpeg's HLS muxer speaks it with -method PUT.
# ---------------------------------------------------------------------------

echo "== a standard HLS push encoder (ffmpeg -f hls -method PUT)"

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"std"}' "$API/streams" >/dev/null
mkdir -p "$RUN/uploaded/std/live"
STATUS="$(curl -sS -o "$RUN/std-src.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"encoder\",\"type\":\"hls_push\",\"path\":\"$RUN/uploaded/std/live\"}" \
    "$API/streams/live/std/sources")"
STD_KEY="$(key_of < "$RUN/std-src.json")"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); d.pop("key",None); print(json.dumps(d))' \
    "$RUN/std-src.json"

# 16 two-second segments: the names cross index9 -> index10, which is where
# ordering by name as a string goes wrong
ffmpeg -hide_banner -loglevel warning -re -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -t 32 \
    -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p \
    -f hls -hls_time 2 -hls_list_size 5 \
    -hls_flags delete_segments -method PUT \
    "http://127.0.0.1:$UPLOAD_PORT/ingest/$STD_KEY/index.m3u8" \
    >"$RUN/std-ffmpeg.log" 2>&1
FFMPEG_STATUS=$?

grep -iE 'error|failed|40[0-9] |41[0-9] |50[0-9] ' "$RUN/std-ffmpeg.log" \
    && { echo "the encoder reported upload errors" >&2; exit 1; }
[ "$FFMPEG_STATUS" = 0 ] \
    || { echo "the encoder exited with $FFMPEG_STATUS" >&2
         cat "$RUN/std-ffmpeg.log" >&2; exit 1; }

STORED="$(grep -c "hls ingest source=encoder stored index[0-9]*\.ts" "$RUN/logs/error.log")"
PLAYLISTS="$(grep -c "hls ingest source=encoder stored index\.m3u8" "$RUN/logs/error.log")"
echo "   segments stored: $STORED, playlist uploads: $PLAYLISTS"
[ "$STORED" -ge 15 ] && [ "$PLAYLISTS" -ge "$STORED" ] \
    || { echo "expected a playlist upload per segment" >&2; exit 1; }

LEFT="$(ls "$RUN/uploaded/std/live"/*.ts 2>/dev/null | wc -l)"
echo "   segments left after the encoder's DELETEs: $LEFT"
[ "$LEFT" -le 7 ] \
    || { echo "expired segments were not deleted" >&2; exit 1; }

echo "== each key writes only to its source directory"
CODE="$(curl -sS -o /dev/null -w '%{http_code}' -T "$RUN/incoming/seg-00000.ts" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest/$STD_KEY/isolation.ts")"
[ "$CODE" = "201" ] || { echo "keyed source upload got HTTP $CODE" >&2; exit 1; }
[ -f "$RUN/uploaded/std/live/isolation.ts" ] \
    && [ ! -e "$RUN/uploaded/uploader/isolation.ts" ] \
    || { echo "the upload escaped its source directory" >&2; exit 1; }

STD="$(curl -fsS "$API/streams/live/std")"
STD_FRAMES="$(printf '%s' "$STD" | grep -o '"program_frames":[0-9]*' | cut -d: -f2)"
echo "   program frames: ${STD_FRAMES:-0}"
# 32 s at 25 fps is 800 frames; allow for the start and the last segments
[ "${STD_FRAMES:-0}" -ge 600 ] \
    || { echo "the pushed program is short of frames" >&2; exit 1; }
curl -fsS "$API/streams/live/std/sources" \
    | grep -q '"id":"encoder".*"healthy":true' \
    || { echo "the standard push source is not healthy" >&2; exit 1; }

echo "== the endpoint's answers"
put() {   # <path> -> status
    curl -sS -o /dev/null -w '%{http_code}' -X PUT --data-binary @"$SEG" \
        "http://127.0.0.1:$UPLOAD_PORT/ingest/$STD_KEY/$1"
}
expect() {   # <what> <expected> <got>
    echo "   $1 -> $3"
    [ "$3" = "$2" ] || { echo "$1: expected $2, got $3" >&2; exit 1; }
}
expect "PUT a new segment"          201 "$(put codes/a1.ts)"
expect "PUT it again"               204 "$(put codes/a1.ts)"
expect "PUT a playlist"             201 "$(put codes/a.m3u8)"
expect "PUT fMP4 (.m4s)"            415 "$(put codes/a1.m4s)"
expect "PUT a DASH manifest"        415 "$(put codes/a.mpd)"
expect "PUT an unknown type"        400 "$(put codes/a1.bin)"
expect "PUT a hidden name"          400 "$(put codes/.a1.ts)"
# The handler checks the original request target, before nginx can normalize
# dot segments; encoded paths are rejected rather than decoded ambiguously.
CODE="$(curl -sS -o /dev/null -w '%{http_code}' --path-as-is -X PUT \
    --data-binary @"$SEG" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest/$STD_KEY/codes/../../x.ts")"
echo "   PUT a traversal -> $CODE"
case "$CODE" in 4??) ;; *) echo "a traversal was not refused: $CODE" >&2; exit 1 ;; esac
CODE="$(curl -sS -o /dev/null -w '%{http_code}' --path-as-is -X PUT \
    --data-binary @"$SEG" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest/$STD_KEY/codes/%2e%2e/x.ts")"
echo "   PUT an encoded traversal -> $CODE"
case "$CODE" in 4??) ;; *) echo "an encoded traversal was not refused: $CODE" >&2; exit 1 ;; esac
expect "PUT with a query"             400 "$(curl -sS -o /dev/null -w '%{http_code}' -X PUT \
    --data-binary @"$SEG" \
    "http://127.0.0.1:$UPLOAD_PORT/ingest/$STD_KEY/query.ts?override=1")"
expect "GET"                        405 "$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$UPLOAD_PORT/ingest/$STD_KEY/codes/a1.ts")"
expect "DELETE a segment"           200 "$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "http://127.0.0.1:$UPLOAD_PORT/ingest/$STD_KEY/codes/a1.ts")"
expect "DELETE it again"            404 "$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "http://127.0.0.1:$UPLOAD_PORT/ingest/$STD_KEY/codes/a1.ts")"
[ ! -e "$RUN/x.ts" ] && [ ! -e "$RUN/uploaded/x.ts" ] \
    || { echo "a traversal wrote a file" >&2; exit 1; }
assert_no_upload_bodies

trap - EXIT
cleanup

echo "== hls ingest ok"

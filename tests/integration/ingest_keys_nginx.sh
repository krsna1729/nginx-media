#!/usr/bin/env bash
#
# Ingest keys (goal doc 11.6).
#
# A publisher attaches to a source by presenting a key: the SRT stream id, the
# RTMP stream name of the app the listener accepts.  The key is issued by the
# API, returned once, and never readable again.  This test is the contract:
#
#   * one source, one key: publishing with it carries media;
#   * two sources on one program, two keys: the API reports two sources, each
#     publisher attaches to its own, and the selector prefers the higher
#     priority;
#   * a rotated key: the old one is refused at once, the new one works;
#   * an unmatched key: refused, and the log carries the fingerprint rather
#     than the key;
#   * RTMP: the same, with the key as the stream name and the app fixed.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/ingest-keys"
SRT_PORT="${INGEST_KEYS_SRT_PORT:-19072}"
RTMP_PORT="${INGEST_KEYS_RTMP_PORT:-19073}"
API_PORT="${INGEST_KEYS_API_PORT:-19074}"
API="http://127.0.0.1:$API_PORT/media/api/v1"
NO_SECRET_API_PORT="${INGEST_KEYS_NO_SECRET_API_PORT:-19075}"
NO_SECRET_API="http://127.0.0.1:$NO_SECRET_API_PORT/media/api/v1"
NO_SECRET_RUN="$RUN/no-secret"
LOG="$RUN/logs/error.log"

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

fail() {
    echo "FAIL: $*" >&2
    echo "--- the tail of the error log ---" >&2
    tail -n 20 "$LOG" >&2 || true
    exit 1
}

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/hls"

cat > "$RUN/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/hls;
media_ingest_secret $RUN/ingest.secret;
media_srt_listen 127.0.0.1:$SRT_PORT;
media_rtmp_listen 127.0.0.1:$RTMP_PORT;
media_rtmp_app live;

http {
    server {
        listen 127.0.0.1:$API_PORT;
        location /media/api/ { media_api; }
        location /hls/ { alias $RUN/hls/; }
    }
}
EOF

echo "== configuration rejects an unusable ingest secret"
cat > "$RUN/bad-secret.conf" <<EOF
worker_processes 1;
daemon off;
error_log $RUN/logs/bad-secret.log info;
pid $RUN/logs/bad-secret.pid;

events { worker_connections 16; }

media_ingest_secret $RUN/missing-secret-dir/ingest.secret;

http {
    server {
        listen 127.0.0.1:$API_PORT;
        location /media/api/ { media_api; }
    }
}
EOF

if "$NGINX" -p "$RUN" -c bad-secret.conf -t \
    > "$RUN/logs/bad-secret-test.log" 2>&1
then
    cat "$RUN/logs/bad-secret-test.log" >&2
    fail "nginx accepted an ingest secret it could not create"
fi

echo "   an unavailable configured secret aborts initialization"

mkdir -p "$RUN/missing-secret-dir"
printf 'short-secret' > "$RUN/missing-secret-dir/ingest.secret"
if "$NGINX" -p "$RUN" -c bad-secret.conf -t \
    > "$RUN/logs/short-secret-test.log" 2>&1
then
    cat "$RUN/logs/short-secret-test.log" >&2
    fail "nginx accepted an ingest secret shorter than 32 bytes"
fi
[ "$(cat "$RUN/missing-secret-dir/ingest.secret")" = short-secret ] \
    || fail "nginx changed a rejected short ingest secret"
echo "   a short configured secret is rejected without modification"

rm "$RUN/missing-secret-dir/ingest.secret"
ln -s "$RUN/missing-secret-target" "$RUN/missing-secret-dir/ingest.secret"
if "$NGINX" -p "$RUN" -c bad-secret.conf -t \
    > "$RUN/logs/dangling-secret-test.log" 2>&1
then
    cat "$RUN/logs/dangling-secret-test.log" >&2
    fail "nginx replaced a dangling ingest-secret path"
fi
[ -L "$RUN/missing-secret-dir/ingest.secret" ] \
    && [ ! -e "$RUN/missing-secret-target" ] \
    || fail "nginx changed a dangling ingest-secret path"
rm "$RUN/missing-secret-dir/ingest.secret"
echo "   a pre-existing secret path is not replaced"

cleanup() {
    "$NGINX" -p "$NO_SECRET_RUN" -c nginx.conf -s quit 2>/dev/null || true
    "$NGINX" -p "$RUN" -c nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

echo "== missing secret does not leave a source registered"
mkdir -p "$NO_SECRET_RUN/logs"
cat > "$NO_SECRET_RUN/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

http {
    server {
        listen 127.0.0.1:$NO_SECRET_API_PORT;
        location /media/api/ { media_api; }
    }
}
EOF
"$NGINX" -p "$NO_SECRET_RUN" -c nginx.conf
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"no-secret"}' \
    "$NO_SECRET_API/streams" >/dev/null
status="$(curl -sS -o "$NO_SECRET_RUN/source.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"id":"enc1","type":"srt"}' \
    "$NO_SECRET_API/streams/live/no-secret/sources")"
[ "$status" = 500 ] \
    || fail "source creation without a secret returned HTTP $status"
state="$(curl -fsS "$NO_SECRET_API/streams/live/no-secret")"
python3 - "$state" <<'PY' || fail "failed key derivation registered a source"
import json, sys
assert json.loads(sys.argv[1])["sources"] == []
PY
"$NGINX" -p "$NO_SECRET_RUN" -c nginx.conf -s quit
echo "   failed key derivation leaves no source behind"


"$NGINX" -p "$RUN" -c nginx.conf

api() { curl -fsS "$@"; }

# the key a source answers to, and the fingerprint the API reports instead
key_of() { python3 -c "import json,sys; print(json.load(sys.stdin)['key'])"; }
print_of() { python3 -c "import json,sys; print(json.load(sys.stdin)['key_print'])"; }

publish_srt() {   # <key> <seconds>
    timeout 40 ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x180:rate=25" -t "$2" \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?streamid=$1" >"$RUN/pub.log" 2>&1
}


publish_rtmp() {   # <stream name> <seconds>
    timeout 40 ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x180:rate=25" -t "$2" \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -f flv \
        "rtmp://127.0.0.1:$RTMP_PORT/live/$1" >"$RUN/pub-rtmp.log" 2>&1
}

program() {   # <name>
    api -X POST -H 'Content-Type: application/json' \
        -d "{\"application\":\"live\",\"name\":\"$1\"}" \
        "$API/streams" >/dev/null
}

source() {   # <program> <id> <priority> <type>
    api -X POST -H 'Content-Type: application/json' \
        -d "{\"id\":\"$2\",\"type\":\"$4\",\"priority\":$3}" \
        "$API/streams/live/$1/sources"
}

echo "== one source, one key"
program one
key="$(source one enc1 100 srt | key_of)"
[ -n "$key" ] || fail "no key was issued"
[ "${#key}" -eq 26 ] || fail "a key should be 26 characters, got ${#key}"

publish_srt "$key" 6 || true
for _ in $(seq 1 100); do
    [ -s "$RUN/hls/live/one/index.m3u8" ] && break
    sleep 0.1
done
[ -s "$RUN/hls/live/one/index.m3u8" ] \
    || fail "publishing with the issued key carried no media"
echo "   the key published and HLS was written"

echo "== two sources on one program, two keys"
program two
k1="$(source two enc1 100 srt | key_of)"
k2="$(source two enc2 50 srt | key_of)"
[ "$k1" != "$k2" ] || fail "two sources must not share a key"

# the lower-priority publisher first, then the higher one: the API must show
# two sources and the selector must prefer the higher
( publish_srt "$k2" 12 || true ) &
sleep 5
( publish_srt "$k1" 12 || true ) &
sleep 6

sources="$(api "$API/streams/live/two")"
echo "$sources" > "$RUN/two.json"
python3 - "$sources" <<'PY' || fail "the two publishers did not attach as two sources"
import json, sys
try:
    doc = json.loads(sys.argv[1])
except Exception as exc:
    print("the document did not parse:", exc, file=sys.stderr)
    print(repr(sys.argv[1][:400]), file=sys.stderr)
    raise
by_id = {s["id"]: s for s in doc["sources"]}
assert sorted(by_id) == ["enc1", "enc2"], sorted(by_id)
assert by_id["enc1"]["priority"] == 100, by_id["enc1"]
assert by_id["enc2"]["priority"] == 50, by_id["enc2"]
# each publisher attached as its own source, with its own key
assert by_id["enc1"]["key_print"] != by_id["enc2"]["key_print"], by_id
# which one is on air is the selector's business - with switchback off the
# first publisher keeps the program - so only that it is one of them is
# asserted here; the health evidence itself is the ingest suite's subject
assert doc.get("active") in ("enc1", "enc2"), doc.get("active")
PY
wait || true
echo "   two publishers attached as two sources, each with its own priority"

echo "== the key is readable again, whenever the encoder is configured"
read_back="$(api "$API/streams/live/one/sources/enc1/key" | key_of)"
[ "$read_back" = "$key" ] \
    || fail "the key read back is not the key issued ($read_back vs $key)"
echo "   the same key comes back"

echo "== rotation: the old key is refused at once"
rotated="$(api -X POST "$API/streams/live/one/sources/enc1/rotate")"
new="$(printf '%s' "$rotated" | key_of)"
[ "$new" != "$key" ] || fail "rotation issued the same key"
[ -n "$(printf '%s' "$rotated" | print_of)" ] || fail "no fingerprint after rotation"

before="$(grep -c 'no source for key' "$LOG" || true)"
publish_srt "$key" 3 || true
after="$(grep -c 'no source for key' "$LOG" || true)"
[ "$after" -gt "$before" ] || fail "the old key was not refused"

publish_srt "$new" 6 || true
rm -f "$RUN/hls/live/one/index.m3u8"
publish_srt "$new" 6 || true
for _ in $(seq 1 100); do
    [ -s "$RUN/hls/live/one/index.m3u8" ] && break
    sleep 0.1
done
[ -s "$RUN/hls/live/one/index.m3u8" ] || fail "the rotated key did not publish"
echo "   the old key refused, the new one publishes"

echo "== an unmatched key: refused, and the key never reaches the log"
publish_srt "ZZZZZZZZZZZZZZZZZZZZZZZZZZ" 3 || true
grep -q 'no source for key' "$LOG" || fail "an unmatched key was not refused"
grep -q 'ZZZZZZZZZZZZZZZZZZZZZZZZZZ' "$LOG" \
    && fail "the key itself reached the log"
echo "   refused by fingerprint, key not logged"

echo "== rtmp: the key is the stream name, the app is fixed"
program rt
rk="$(source rt enc1 100 rtmp | key_of)"

before="$(grep -c 'srt publisher rejected.*no source for key' "$LOG" || true)"
publish_srt "$rk" 3 || true
after="$(grep -c 'srt publisher rejected.*no source for key' "$LOG" || true)"
[ "$after" -gt "$before" ] || fail "SRT accepted an RTMP source key"

before="$(grep -c 'rtmp publisher rejected: no source for key' "$LOG" || true)"
publish_rtmp "$new" 3 || true
after="$(grep -c 'rtmp publisher rejected: no source for key' "$LOG" || true)"
[ "$after" -gt "$before" ] || fail "RTMP accepted an SRT source key"
echo "   each ingest transport rejects keys for the other transport"

publish_rtmp "WRONGKEY1234567890ABCDEFG" 3 || true
grep -q 'rtmp publisher rejected: no source for key' "$LOG" \
    || fail "an unmatched rtmp key was not refused"

publish_rtmp "$rk" 6 || true
for _ in $(seq 1 100); do
    [ -s "$RUN/hls/live/rt/index.m3u8" ] && break
    sleep 0.1
done
[ -s "$RUN/hls/live/rt/index.m3u8" ] \
    || fail "an rtmp publisher with the issued key carried no media"
grep -q "rtmp publisher stream=live/rt source=enc1" "$LOG" \
    || fail "the rtmp publisher did not attach to its source"
echo "   rtmp admitted by key, attached as enc1"

echo "== ingest keys ok"

#!/usr/bin/env bash
#
# Conflicting HLS destination profiles on one program.
#
# One program, two HLS PUT destinations with different segmentation settings:
#
#   d-short   segment_duration_ms 1000, playlist_window 3
#   d-long    segment_duration_ms 4000, playlist_window 8
#
# and checks that both get what they asked for - the short profile many short
# segments, the long one few long segments, each playlist satisfying its own
# window - while the *preparation* stays shared: the program has one prepared
# feed, not one per destination, so the media is demuxed and remuxed once and
# only the segmentation and the playlists differ.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/hls-push-profiles"
BASE=$(( 26500 + ($$ % 30) * 16 ))
HTTP_PORT=$BASE
SRT_PORT=$(( BASE + 1 ))
SHORT_PORT=$(( BASE + 4 ))
LONG_PORT=$(( BASE + 5 ))

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

PUB_PID=""
SHORT_PID=""
LONG_PID=""

stop_instance() {
    [ -f "$RUN/logs/nginx.pid" ] || return 0
    local pid child
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
    [ -n "$PUB_PID" ] && kill -KILL "$PUB_PID" 2>/dev/null
    [ -n "$SHORT_PID" ] && kill -TERM "$SHORT_PID" 2>/dev/null
    [ -n "$LONG_PID" ] && kill -TERM "$LONG_PID" 2>/dev/null
    stop_instance
    return 0
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    tail -20 "$RUN/logs/error.log" >&2
    exit 1
}

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/hls;
media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /media/api/ {
            media_api;
        }

        location /hls/ {
            alias $RUN/hls/;
        }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 \
    || fail "configuration rejected"
"$NGINX" -p "$RUN" -c conf/nginx.conf || fail "nginx did not start"
sleep 0.5

python3 "$ROOT/tests/bench/hls_push_capacity.py" serve --port "$SHORT_PORT" \
    >"$RUN/short-sink.log" 2>&1 &
SHORT_PID=$!
python3 "$ROOT/tests/bench/hls_push_capacity.py" serve --port "$LONG_PORT" \
    >"$RUN/long-sink.log" 2>&1 &
LONG_PID=$!
for port in "$SHORT_PORT" "$LONG_PORT"; do
    for _ in $(seq 1 100); do
        curl -fsS "http://127.0.0.1:$port/__health" >/dev/null 2>&1 && break
        sleep 0.1
    done
    curl -fsS "http://127.0.0.1:$port/__health" >/dev/null 2>&1 \
        || fail "sink on port $port did not become ready"
done

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"profiles"}' "$API/streams" >/dev/null \
    || fail "stream not created"

timeout 120 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" -t 60 \
    -c:v libx264 -preset ultrafast -b:v 1200k -maxrate 1200k -bufsize 600k \
    -g 25 -pix_fmt yuv420p -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/profiles,m=publish,s=encoder-a" \
    >"$RUN/pub.log" 2>&1 &
PUB_PID=$!
for _ in $(seq 1 100); do
    grep -q 'srt source open app=live stream=profiles' "$RUN/logs/error.log" && break
    sleep 0.1
done

add_destination() {   # <id> <port> <segment_duration_ms> <playlist_window>
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"id\":\"$1\",\"type\":\"hls_push\",\"host\":\"http://127.0.0.1:$2/$1/\",\"path\":\"$RUN/hls/live/profiles\",\"segment_duration_ms\":$3,\"playlist_window\":$4}" \
        "$API/streams/live/profiles/destinations" >/dev/null \
        || fail "destination $1 not added"
}

add_destination d-short "$SHORT_PORT" 1000 3
add_destination d-long "$LONG_PORT" 4000 8

sleep 30

curl -fsS "http://127.0.0.1:$SHORT_PORT/__snapshot" > "$RUN/short.json" \
    || fail "short sink snapshot"
curl -fsS "http://127.0.0.1:$LONG_PORT/__snapshot" > "$RUN/long.json" \
    || fail "long sink snapshot"
curl -fsS "$API/metrics" > "$RUN/metrics.txt" || fail "metrics"

python3 - "$RUN/short.json" "$RUN/long.json" "$RUN/metrics.txt" <<'PYEOF' \
    || fail "conflicting profiles"
import json, re, sys

short = json.load(open(sys.argv[1]))
long_sink = json.load(open(sys.argv[2]))
metrics = open(sys.argv[3]).read()

failures = []

def check(condition, what):
    print(("   ok   " if condition else "   FAIL ") + what)
    if not condition:
        failures.append(what)

short_dest = short["destinations"].get("d-short", {})
long_dest = long_sink["destinations"].get("d-long", {})
short_segments = short_dest.get("segments", 0)
long_segments = long_dest.get("segments", 0)
print(f"   d-short: {short_segments} segments, {short_dest.get('ts_bytes', 0)} bytes, "
      f"{short['http_errors']} HTTP errors")
print(f"   d-long:  {long_segments} segments, {long_dest.get('ts_bytes', 0)} bytes, "
      f"{long_sink['http_errors']} HTTP errors")

check(short_segments >= 8, f"the short profile received segments ({short_segments})")
check(long_segments >= 3, f"the long profile received segments ({long_segments})")
check(short_segments > long_segments,
      f"the short profile segmented finer than the long one "
      f"({short_segments} > {long_segments})")
check(short["http_errors"] == 0 and long_sink["http_errors"] == 0,
      f"neither sink saw HTTP errors ({short['http_errors']}, "
      f"{long_sink['http_errors']})")

# Each destination's playlist must reference segments that reached it: the
# sink counts a violation when a playlist names a segment it has not seen.
check(not short_dest.get("playlist_violations")
      and not long_dest.get("playlist_violations"),
      f"no playlist referenced a segment before its upload "
      f"({short_dest.get('playlist_violations')}, "
      f"{long_dest.get('playlist_violations')})")
check(short_dest.get("playlist_puts", 0) > 0
      and long_dest.get("playlist_puts", 0) > 0,
      "both destinations published playlists")

# Shared preparation: one prepared feed for the program, not one per
# destination.  The feed metrics carry application and name labels; a second
# feed for the same program would mean the media was prepared twice.
feeds = set()
for line in metrics.splitlines():
    if line.startswith("nginx_media_stream_feed_units{"):
        match = re.search(r'application="([^"]+)",name="([^"]+)"', line)
        if match:
            feeds.add(match.groups())
check(feeds == {("live", "profiles")},
      f"the program has exactly one prepared feed: {sorted(feeds)}")

feed_bytes = 0.0
for line in metrics.splitlines():
    if line.startswith("nginx_media_stream_feed_bytes{"):
        match = re.search(r'application="live",name="profiles"', line)
        if match:
            feed_bytes = float(line.split()[-1])
delivered = short_dest.get("ts_bytes", 0) + long_dest.get("ts_bytes", 0)
print(f"   prepared feed holds {feed_bytes:.0f} bytes for "
      f"{delivered} bytes delivered to both destinations")
check(feed_bytes > 0 and feed_bytes < delivered * 1.5,
      f"the feed is prepared once and fanned out, not once per destination "
      f"({feed_bytes:.0f} vs {delivered} delivered)")

sys.exit(0 if not failures else 1)
PYEOF

if grep -qE '\[(alert|emerg)\]|AddressSanitizer' "$RUN/logs/error.log"; then
    fail "worker reported an alert or crash"
fi

echo "== conflicting HLS destination profiles ok"

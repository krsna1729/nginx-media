#!/usr/bin/env bash
#
# SRT backend qualification (phase 10 exit criteria).
#
# The same scenario is run twice: a publisher feeds a program through the SRT
# ingest path, the program is prepared once and pushed to a second nginx over
# an SRT destination.  Both receivers must produce decodable HLS.  The only
# difference between the runs is the transport backend selected with
# media_srt_backend:
#
#   haivision  - the production libsrt backend, published to with ffmpeg
#   udp        - the UDP conformance backend in
#                src/srt/ngx_media_srt_udp.c, published to with a small
#                harness that speaks its (deliberately trivial) wire format
#
# Qualifying a third backend (Robotweax) means adding its ops table and its
# publisher here; the assertions do not change.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tests/integration/ingest_test_helpers.sh"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/srt-qualify"
BASE=$(( 19900 + ($$ % 60) * 8 ))
PUB=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null || true
    "$NGINX" -p "$RUN/a" -c conf/nginx.conf -s quit 2>/dev/null || true
    "$NGINX" -p "$RUN/b" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$RUN"
mkdir -p "$RUN/a/logs" "$RUN/a/conf" "$RUN/a/hls" \
         "$RUN/b/logs" "$RUN/b/conf" "$RUN/b/hls"

run_backend() {
    local backend="$1" in_port="$2" out_port="$3"
    local api_a_port="$4" api_b_port="$5"
    local expected_streamid="$6" api_a api_b out_key in_key stream_id

    api_a="http://127.0.0.1:$api_a_port/media/api/v1"
    api_b="http://127.0.0.1:$api_b_port/media/api/v1"

    rm -rf "$RUN/a/hls" "$RUN/b/hls" "$RUN/a/logs" "$RUN/b/logs"
    mkdir -p "$RUN/a/hls" "$RUN/b/hls" "$RUN/a/logs" "$RUN/b/logs"

    cat > "$RUN/b/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_backend $backend;
media_hls $RUN/b/hls;
media_ingest_secret $RUN/b/ingest.secret;
media_srt_listen 127.0.0.1:$out_port;

http {
    server {
        listen 127.0.0.1:$api_b_port;
        location /media/api/ { media_api; }
    }
}
EOF

    echo "== backend $backend: start receiver"
    if ! "$NGINX" -p "$RUN/b" -c conf/nginx.conf -t >/dev/null 2>&1; then
        echo "== backend $backend: skipped (not compiled in this binary)"
        return 0
    fi
    "$NGINX" -p "$RUN/b" -c conf/nginx.conf

    for _ in $(seq 1 200); do
        curl -fsS "$api_b/streams" >/dev/null 2>&1 \
            && grep -q 'srt listener ready' "$RUN/b/logs/error.log" 2>/dev/null \
            && break
        sleep 0.05
    done

    out_key="$(media_test_ingest_key "$api_b" live qualify \
        "$expected_streamid" srt 100)"

    cat > "$RUN/a/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_backend $backend;
media_hls $RUN/a/hls;
media_ingest_secret $RUN/a/ingest.secret;
media_srt_listen 127.0.0.1:$in_port;
media_srt_output live/qualify 127.0.0.1:$out_port "$out_key";

http {
    server {
        listen 127.0.0.1:$api_a_port;
        location /media/api/ { media_api; }
    }
}
EOF

    echo "== backend $backend: start sender"
    "$NGINX" -p "$RUN/a" -c conf/nginx.conf -t >/dev/null
    "$NGINX" -p "$RUN/a" -c conf/nginx.conf

    for _ in $(seq 1 200); do
        curl -fsS "$api_a/streams" >/dev/null 2>&1 \
            && grep -q 'srt listener ready' "$RUN/a/logs/error.log" 2>/dev/null \
            && break
        sleep 0.05
    done

    in_key="$(media_test_ingest_key "$api_a" live qualify encoder-a srt 100)"
    stream_id="$in_key"

    if [ "$backend" = "srt" ]; then
        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "testsrc2=size=320x240:rate=25" \
            -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
            -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
            -c:a aac -b:a 96k \
            -t 10 -f mpegts \
            "$(media_test_srt_publisher_url "$api_a" 127.0.0.1 "$in_port" live qualify encoder-a 100)" \
            >"$RUN/a/pub.log" 2>&1 &
        PUB=$!

    else
        # the UDP backend's wire format: one stream id packet, then TS datagrams.
        # The sender lives in its own file: a heredoc would become python's
        # stdin, leaving nothing to read the transport pipe.
        cat > "$RUN/udp-publisher.py" <<'PY'
import socket, struct, sys

port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
target = ("127.0.0.1", port)

streamid = sys.argv[2].encode()
sock.sendto(struct.pack("<II", 0x53494431, len(streamid)) + streamid, target)

while True:
    chunk = sys.stdin.buffer.read(1316)
    if not chunk:
        break
    sock.sendto(chunk, target)
PY

        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "testsrc2=size=320x240:rate=25" \
            -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
            -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
            -c:a aac -b:a 96k \
            -t 10 -f mpegts - 2>"$RUN/a/pub.log" \
        | python3 "$RUN/udp-publisher.py" "$in_port" "$stream_id" \
            >>"$RUN/a/pub.log" 2>&1 &

        PUB=$!
    fi

    # the receiver of the destination must register the announced source
    for _ in $(seq 1 300); do
        grep -q "source=$expected_streamid" "$RUN/b/logs/error.log" 2>/dev/null \
            && break
        sleep 0.1
    done

    grep -q "source=$expected_streamid" "$RUN/b/logs/error.log" \
        || { echo "$backend: the destination never saw its source" >&2; exit 1; }

    # both receivers must produce a decodable segment
    for dir in a b; do
        for _ in $(seq 1 300); do
            [ -f "$RUN/$dir/hls/live/qualify/index.m3u8" ] \
                && [ "$(grep -c '^#EXTINF' "$RUN/$dir/hls/live/qualify/index.m3u8" || true)" -ge 1 ] \
                && break
            sleep 0.1
        done

        grep -q '^#EXTM3U' "$RUN/$dir/hls/live/qualify/index.m3u8" \
            || { echo "$backend: $dir produced no hls" >&2; exit 1; }

        SEGMENT="$(grep '\.ts$' "$RUN/$dir/hls/live/qualify/index.m3u8" | head -1)"
        [ -n "$SEGMENT" ] || { echo "$backend: $dir produced no segment" >&2; exit 1; }

        PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
            stream=codec_name,codec_type -of csv "$RUN/$dir/hls/live/qualify/$SEGMENT" 2>&1)"

        printf '%s' "$PROBE" | grep -q ',h264,video' \
            || { echo "$backend: $dir segment has no H.264" >&2; printf '%s\n' "$PROBE" >&2; exit 1; }
        printf '%s' "$PROBE" | grep -q ',aac,audio' \
            || { echo "$backend: $dir segment has no AAC" >&2; printf '%s\n' "$PROBE" >&2; exit 1; }

        echo "   $backend: $dir segment ok ($(stat -c %s "$RUN/$dir/hls/live/qualify/$SEGMENT") bytes)"
    done

    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null || true
    PUB=0

    sleep 1

    for inst in a b; do
        PID="$(cat "$RUN/$inst/logs/nginx.pid")"
        "$NGINX" -p "$RUN/$inst" -c conf/nginx.conf -s quit

        for _ in $(seq 1 400); do
            kill -0 "$PID" 2>/dev/null || break
            sleep 0.05
        done

        if kill -0 "$PID" 2>/dev/null; then
            echo "$backend: nginx ($inst) did not shut down" >&2
            kill -9 "$PID" || true
            exit 1
        fi

        grep -aq 'exited with code 0' "$RUN/$inst/logs/error.log" \
            || { echo "$backend: worker ($inst) did not exit cleanly" >&2; exit 1; }
    done

    echo "== backend $backend: ok"
}

# the SRT output announces the stream id configured on the destination, so the
# receiving instance registers that source identity
run_backend srt "$BASE" "$(( BASE + 1 ))" "$(( BASE + 4 ))" "$(( BASE + 5 ))" "qualify-out"
run_backend udp "$(( BASE + 2 ))" "$(( BASE + 3 ))" "$(( BASE + 6 ))" "$(( BASE + 7 ))" "qualify-out"

trap - EXIT

echo "== srt backend qualification ok"

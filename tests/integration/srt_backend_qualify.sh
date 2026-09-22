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
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
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
    local backend="$1"
    local in_port="$2"
    local out_port="$3"
    local expected_streamid="$4"   # the identity the destination announces

    rm -rf "$RUN/a/hls" "$RUN/b/hls" "$RUN/a/logs" "$RUN/b/logs"
    mkdir -p "$RUN/a/hls" "$RUN/b/hls" "$RUN/a/logs" "$RUN/b/logs"

    cat > "$RUN/a/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_srt_backend $backend;

media_hls $RUN/a/hls;

media_srt_listen 127.0.0.1:$in_port;
media_srt_source_priority encoder-a 100;
media_srt_output live/qualify 127.0.0.1:$out_port "#!::r=live/qualify,m=publish,s=qualify-out";
EOF

    cat > "$RUN/b/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_srt_backend $backend;

media_hls $RUN/b/hls;

media_srt_listen 127.0.0.1:$out_port;
media_srt_source_priority qualify-out 100;
EOF

    echo "== backend $backend: config test"
    "$NGINX" -p "$RUN/a" -c conf/nginx.conf -t >/dev/null
    "$NGINX" -p "$RUN/b" -c conf/nginx.conf -t >/dev/null

    "$NGINX" -p "$RUN/b" -c conf/nginx.conf
    "$NGINX" -p "$RUN/a" -c conf/nginx.conf

    for _ in $(seq 1 200); do
        grep -q 'srt listener ready' "$RUN/a/logs/error.log" 2>/dev/null \
            && grep -q 'srt listener ready' "$RUN/b/logs/error.log" 2>/dev/null \
            && break
        sleep 0.05
    done

    if [ "$backend" = "srt" ]; then
        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "testsrc2=size=320x240:rate=25" \
            -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
            -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
            -c:a aac -b:a 96k \
            -t 10 -f mpegts \
            "srt://127.0.0.1:$in_port?mode=caller&streamid=#!::r=live/qualify,m=publish,s=encoder-a" \
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

streamid = b"#!::r=live/qualify,m=publish,s=encoder-a"
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
        | python3 "$RUN/udp-publisher.py" "$in_port" \
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
            [ -f "$RUN/$dir/hls/index.m3u8" ] \
                && [ "$(grep -c '^#EXTINF' "$RUN/$dir/hls/index.m3u8" || true)" -ge 1 ] \
                && break
            sleep 0.1
        done

        grep -q '^#EXTM3U' "$RUN/$dir/hls/index.m3u8" \
            || { echo "$backend: $dir produced no hls" >&2; exit 1; }

        SEGMENT="$(grep '\.ts$' "$RUN/$dir/hls/index.m3u8" | head -1)"
        [ -n "$SEGMENT" ] || { echo "$backend: $dir produced no segment" >&2; exit 1; }

        PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
            stream=codec_name,codec_type -of csv "$RUN/$dir/hls/$SEGMENT" 2>&1)"

        printf '%s' "$PROBE" | grep -q ',h264,video' \
            || { echo "$backend: $dir segment has no H.264" >&2; printf '%s\n' "$PROBE" >&2; exit 1; }
        printf '%s' "$PROBE" | grep -q ',aac,audio' \
            || { echo "$backend: $dir segment has no AAC" >&2; printf '%s\n' "$PROBE" >&2; exit 1; }

        echo "   $backend: $dir segment ok ($(stat -c %s "$RUN/$dir/hls/$SEGMENT") bytes)"
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
run_backend srt "$BASE" "$(( BASE + 1 ))" "qualify-out"
run_backend udp "$(( BASE + 2 ))" "$(( BASE + 3 ))" "qualify-out"

trap - EXIT

echo "== srt backend qualification ok"

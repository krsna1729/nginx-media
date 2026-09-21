#!/usr/bin/env bash
#
# Soak: churn publishers while streaming, reload the configuration in the
# middle, and measure.  The point is not throughput but stability: the worker
# must survive repeated publisher turnover and two reloads without crashing,
# without leaking memory and while still producing output.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/soak"
BASE=$(( 19700 + ($$ % 80) * 4 ))
SRT_PORT="${SOAK_SRT_PORT:-$BASE}"
HTTP_PORT="${SOAK_HTTP_PORT:-$(( BASE + 1 ))}"
CYCLES="${SOAK_CYCLES:-6}"
PUB=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf" "$RUN/hls" "$RUN/rec"

write_conf() {
    local marker="$1"

    cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/hls;
media_record $RUN/rec/program.ts;

media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;
media_srt_source_priority encoder-b 90;
# $marker

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /media/api/ {
            media_api;
        }
    }
}
EOF
}

worker_pid() {
    # pipefail would report pgrep's SIGPIPE instead of the worker pid
    pgrep -f "nginx: worker process" 2>/dev/null | head -1 || true
}

rss_kb() {
    local pid="$1"

    awk '/VmRSS/ { print $2 }' "/proc/$pid/status" 2>/dev/null || echo 0
}

publish() {
    local source="$1" seconds="$2" pattern="$3"

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "$pattern" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$seconds" -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=%23!::r%3Dlive%2Fsoak%2Cm%3Dpublish%2Cs%3D$source" \
        >>"$RUN/pub.log" 2>&1 &
    PUB=$!
}

echo "== config test"
write_conf "initial"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

PID="$(worker_pid)"
[ -n "$PID" ] || { echo "no worker running" >&2; exit 1; }

RSS_START="$(rss_kb "$PID")"
echo "   worker $PID rss ${RSS_START} kB"

for cycle in $(seq 1 "$CYCLES"); do

    if [ $(( cycle % 2 )) -eq 0 ]; then
        source="encoder-a"
    else
        source="encoder-b"
    fi

    echo "== cycle $cycle: publishing as $source for 3s"
    publish "$source" 3 "testsrc2=size=320x240:rate=25"

    sleep 4

    kill -KILL "$PUB" 2>/dev/null || true
    PUB=0

    # a reload in the middle of the churn
    if [ "$cycle" = "2" ] || [ "$cycle" = "4" ]; then
        echo "== reload after cycle $cycle"
        write_conf "reload $cycle"
        "$NGINX" -p "$RUN" -c conf/nginx.conf -s reload

        for _ in $(seq 1 200); do
            grep -q "srt listener ready" "$RUN/logs/error.log" 2>/dev/null \
                && [ -n "$(worker_pid)" ] && break
            sleep 0.1
        done

        PID="$(worker_pid)"
        [ -n "$PID" ] || { echo "worker died across the reload" >&2; exit 1; }
    fi

    sleep 1
done

PID="$(worker_pid)"
[ -n "$PID" ] || { echo "no worker at the end of the soak" >&2; exit 1; }

RSS_END="$(rss_kb "$PID")"

echo "   worker rss ${RSS_START} kB -> ${RSS_END} kB"

# no crash, no abort, no core dump anywhere in the log
if grep -aqE 'signal [0-9]+ \(core dumped\)|worker process .* exited on signal' \
    "$RUN/logs/error.log"; then
    echo "the worker crashed during the soak" >&2
    grep -aE 'signal|alert' "$RUN/logs/error.log" | tail -5 >&2
    exit 1
fi

# memory must not run away: allow a generous but bounded margin
GROWTH=$(( RSS_END - RSS_START ))
echo "   rss growth ${GROWTH} kB"

if [ "$GROWTH" -gt 65536 ]; then
    echo "worker memory grew by ${GROWTH} kB during the soak" >&2
    exit 1
fi

# and the program still produced output after all that churn
SEGMENTS="$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" 2>/dev/null || echo 0)"
echo "   hls segments: $SEGMENTS"
[ "$SEGMENTS" -ge 1 ] || { echo "no hls output after the soak" >&2; exit 1; }

[ -s "$RUN/rec/program.ts" ] || { echo "no program recording after the soak" >&2; exit 1; }

echo "== stop"
PID="$(cat "$RUN/logs/nginx.pid")"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit
trap - EXIT

for _ in $(seq 1 400); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.05
done

if kill -0 "$PID" 2>/dev/null; then
    echo "nginx did not shut down" >&2
    kill -9 "$PID" || true
    exit 1
fi

grep -aq 'exited with code 0' "$RUN/logs/error.log" \
    || { echo "worker did not exit cleanly" >&2; exit 1; }

echo "== soak ok"

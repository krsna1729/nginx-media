#!/usr/bin/env bash
#
# Health, failover and switchback through nginx (phase 4 exit criteria).
#
# Two publishers feed live/news with configured priorities.  The test kills the
# active publisher, freezes it, lets it recover, and finally replaces it with a
# corrupted one, asserting that the program recovers deterministically every
# time.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/failover-nginx"
SRT_PORT="${FAILOVER_SRT_PORT:-19045}"
HTTP_PORT="${FAILOVER_HTTP_PORT:-18445}"
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
STREAMID='%23!::r%3Dlive%2Fnews%2Cm%3Dpublish%2Cs%3D'

PUB_A=0
PUB_B=0
PUB_C=0
PUB_PID=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_failover_failure_timeout 700;
media_failover_recovery_timeout 300;
media_failover_switchback auto;

media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;
media_srt_source_priority encoder-b 90;

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

cleanup() {
    [ "$PUB_A" != "0" ] && kill -KILL "$PUB_A" 2>/dev/null || true
    [ "$PUB_B" != "0" ] && kill -KILL "$PUB_B" 2>/dev/null || true
    [ "$PUB_C" != "0" ] && kill -KILL "$PUB_C" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

api() {
    curl -fsS "$API$1"
}

active_source() {
    api /streams/live/news | sed -n 's/.*"active":"\([^"]*\)".*/\1/p'
}

switches() {
    api /streams/live/news | sed -n 's/.*"switches":\([0-9]*\).*/\1/p'
}

program_frames() {
    api /streams/live/news | sed -n 's/.*"program_frames":\([0-9]*\).*/\1/p'
}

wait_for_active() {
    local want="$1" tries="${2:-80}"

    for _ in $(seq 1 "$tries"); do
        if [ "$(active_source)" = "$want" ]; then
            return 0
        fi
        sleep 0.1
    done

    echo "timed out waiting for active=$want" >&2
    api /streams/live/news >&2
    return 1
}

wait_for_healthy() {
    local id="$1" tries="${2:-80}"

    for _ in $(seq 1 "$tries"); do
        if api /streams/live/news/sources \
            | grep -q "\"id\":\"$id\".*\"healthy\":true"; then
            return 0
        fi
        sleep 0.1
    done

    echo "source $id never became healthy" >&2
    return 1
}

# starts ffmpeg in the background and reports its real pid through PUB_PID
start_publisher() {
    local pattern="$1" freq="$2" source="$3" out="$4" seconds="$5"

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "$pattern" \
        -f lavfi -i "sine=frequency=$freq:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$seconds" -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=$STREAMID$source" \
        >"$RUN/$out.log" 2>&1 &

    PUB_PID=$!
}

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

grep -q 'srt listener ready' "$RUN/logs/error.log" \
    || { echo "listener not ready" >&2; exit 1; }

echo "== two publishers start"
start_publisher "testsrc=size=320x240:rate=25" 440 "encoder-a" pub_a 30
PUB_A="$PUB_PID"
sleep 1
start_publisher "smptehdbars=size=320x240:rate=25" 880 "encoder-b" pub_b 30
PUB_B="$PUB_PID"

wait_for_active encoder-a
wait_for_healthy encoder-b

echo "== kill: the active publisher dies"
kill -KILL "$PUB_A"
PUB_A=0

wait_for_active encoder-b
SWITCHES_AFTER_KILL="$(switches)"
[ "$SWITCHES_AFTER_KILL" -ge 1 ] \
    || { echo "no switch counted after the kill" >&2; exit 1; }
FRAMES_AFTER_KILL="$(program_frames)"
[ "$FRAMES_AFTER_KILL" -gt 0 ] || { echo "no program frames" >&2; exit 1; }
echo "   active=encoder-b switches=$SWITCHES_AFTER_KILL frames=$FRAMES_AFTER_KILL"

echo "== switchback: the primary returns"
start_publisher "testsrc=size=320x240:rate=25" 440 "encoder-a" pub_a2 30
PUB_A="$PUB_PID"

wait_for_active encoder-a
SWITCHES_AFTER_BACK="$(switches)"
[ "$SWITCHES_AFTER_BACK" -gt "$SWITCHES_AFTER_KILL" ] \
    || { echo "switchback was not counted" >&2; exit 1; }
echo "   active=encoder-a switches=$SWITCHES_AFTER_BACK"

echo "== freeze: the active publisher stops making progress"
kill -STOP "$PUB_A"
wait_for_active encoder-b
echo "   active=encoder-b after freeze"

echo "== thaw: the frozen publisher recovers"
kill -CONT "$PUB_A"
wait_for_active encoder-a
echo "   active=encoder-a after thaw"

echo "== corrupt: the primary is replaced by a garbage publisher"
kill -KILL "$PUB_A"
PUB_A=0
wait_for_active encoder-b

head -c 200000 /dev/urandom > "$RUN/garbage.bin"

srt-live-transmit \
    "file://$RUN/garbage.bin" \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=$STREAMID""encoder-a" \
    >"$RUN/pub_c.log" 2>&1 &
PUB_C=$!

sleep 3

wait_for_active encoder-b

CORRUPT_VIEW="$(api /streams/live/news/sources)"
printf '%s\n' "$CORRUPT_VIEW"

printf '%s' "$CORRUPT_VIEW" | grep -q '"id":"encoder-a".*"healthy":false' \
    || { echo "the corrupt source was not marked unhealthy" >&2; exit 1; }

printf '%s' "$CORRUPT_VIEW" | grep -q '"id":"encoder-b".*"healthy":true' \
    || { echo "encoder-b is not healthy" >&2; exit 1; }

FRAMES_LATE="$(program_frames)"
[ "$FRAMES_LATE" -gt "$FRAMES_AFTER_KILL" ] \
    || { echo "the program stopped progressing" >&2; exit 1; }

echo "   active=encoder-b frames=$FRAMES_LATE (was $FRAMES_AFTER_KILL)"

echo "== stop"
kill -KILL "$PUB_B" 2>/dev/null || true
PUB_B=0
kill -KILL "$PUB_C" 2>/dev/null || true
PUB_C=0

echo "== worker view"
grep -E 'selector|program stream' "$RUN/logs/error.log" | tail -20 || true

PID="$(cat "$RUN/logs/nginx.pid")"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit
trap - EXIT

for _ in $(seq 1 100); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.05
done

if kill -0 "$PID" 2>/dev/null; then
    echo "nginx did not shut down" >&2
    kill -9 "$PID" || true
    exit 1
fi

grep -q 'exited with code 0' "$RUN/logs/error.log" \
    || { echo "worker did not exit cleanly" >&2; exit 1; }

echo "== failover ok"

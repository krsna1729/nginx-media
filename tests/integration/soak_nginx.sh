#!/usr/bin/env bash
#
# Soak: churn publishers while streaming, reload the configuration in the
# middle, and measure.  The point is not throughput but stability: the worker
# must survive repeated publisher turnover and two reloads without crashing,
# without leaking memory and while still producing output.
#
# The last phase adds the load the stability question is really about: many
# concurrent receivers pulling segments while the active source dies.  The
# failover is timed against the configured failure timeout, with the storm
# running and without it, because "the program recovers" is not the same claim
# as "the program recovers inside its SLA while under load" (goal doc 34 item
# 14).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/soak"
BASE=$(( 19700 + ($$ % 80) * 4 ))
SRT_PORT="${SOAK_SRT_PORT:-$BASE}"
HTTP_PORT="${SOAK_HTTP_PORT:-$(( BASE + 1 ))}"
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
CYCLES="${SOAK_CYCLES:-6}"
PUB=0
PUB_B=0
STORM_PIDS=""

# the SLA the config asks for, and the room allowed around it: the failure
# timeout is when health gives up, and the rest covers the 100ms selector tick
# plus the fact that every measurement here is an HTTP poll competing with the
# storm for the worker
FAILOVER_SLA_MS="${SOAK_FAILOVER_SLA_MS:-700}"
SLA_MARGIN_MS="${SOAK_SLA_MARGIN_MS:-400}"
SLA_BOUND_MS=$(( FAILOVER_SLA_MS + SLA_MARGIN_MS ))
STORM_CONSUMERS="${SOAK_STORM_CONSUMERS:-32}"
STORM_RATE="${SOAK_STORM_RATE:-500k}"

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

cleanup() {
    local p

    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null || true
    [ "$PUB_B" != "0" ] && kill -KILL "$PUB_B" 2>/dev/null || true

    for p in $STORM_PIDS; do
        kill -KILL "$p" 2>/dev/null || true
    done

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

media_failover_failure_timeout $FAILOVER_SLA_MS;
media_failover_recovery_timeout 300;
media_failover_switchback auto;

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

        location /hls/ {
            alias $RUN/hls/;
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

# starts a publisher and echoes its pid, so a phase can hold more than one
# (the failover phase needs the standby on air as well as the active source)
start_publisher() {
    local source="$1" seconds="$2" pattern="$3" log="${4:-pub}" pid

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "$pattern" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$seconds" -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/soak,m=publish,s=$source" \
        >>"$RUN/$log.log" 2>&1 &
    pid=$!

    echo "$pid"
}

publish() {
    PUB="$(start_publisher "$1" "$2" "$3")"
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
SEGMENTS="$(grep -c '^#EXTINF' "$RUN/hls/live/soak/index.m3u8" 2>/dev/null || echo 0)"
echo "   hls segments: $SEGMENTS"
[ "$SEGMENTS" -ge 1 ] || { echo "no hls output after the soak" >&2; exit 1; }

[ -s "$RUN/rec/program.ts" ] || { echo "no program recording after the soak" >&2; exit 1; }

# --- receiver storms and the failover SLA ---------------------------------

# Everything above is about a program nobody is watching.  This phase puts
# receivers on it -- concurrent, rate limited segment fetchers -- then kills
# the active source and times how long the program takes to report the
# standby.  The same failover is measured with the storm running and without
# it, and both are held against the failure timeout the config asks for, so a
# storm that delayed health or the selector past its SLA fails here
# (goal doc 34 item 14).
echo
echo "== receiver storm: failover against the ${FAILOVER_SLA_MS}ms SLA"
echo "   (bound ${SLA_BOUND_MS}ms = the configured failure timeout plus"
echo "    ${SLA_MARGIN_MS}ms for the selector tick and for polling a worker"
echo "    that is busy serving the storm)"

active_source() {
    curl -fsS "$API/streams/live/soak" 2>/dev/null \
        | sed -n 's/.*"active":"\([^"]*\)".*/\1/p'
}

program_frames() {
    curl -fsS "$API/streams/live/soak" 2>/dev/null \
        | sed -n 's/.*"program_frames":\([0-9]*\).*/\1/p' || true
}

# the poll is its own pace: every answer is one HTTP round trip through the
# worker that is also serving the storm, which is the position an operator
# checking the API during an incident is in
time_to_active() {
    local want="$1" t0 t1

    t0=$(date +%s%N)

    for _ in $(seq 1 4000); do
        if [ "$(active_source)" = "$want" ]; then
            t1=$(date +%s%N)
            echo $(( (t1 - t0) / 1000000 ))
            return 0
        fi

        if [ $(( ($(date +%s%N) - t0) / 1000000 )) -gt 5000 ]; then
            break
        fi
    done

    echo "$(( ($(date +%s%N) - t0) / 1000000 ))"
    return 1
}

wait_for_active() {
    local want="$1" ms

    ms="$(time_to_active "$want")" || {
        echo "the program never moved to $want (waited ${ms}ms)" >&2
        curl -fsS "$API/streams/live/soak" >&2 || true
        exit 1
    }
}

storm_start() {
    local i name

    : > "$RUN/fetches"

    for i in $(seq 1 "$STORM_CONSUMERS"); do
        (
            while :; do
                for name in $(grep -v '^#' "$RUN/hls/live/soak/index.m3u8" 2>/dev/null \
                              | grep '\.ts$' || true); do
                    # a rate limit keeps a fetcher on the wire: the storm is
                    # many slow receivers, not many fast ones
                    curl -fsS --limit-rate "$STORM_RATE" -o /dev/null \
                        "http://127.0.0.1:$HTTP_PORT/hls/live/soak/$name" 2>/dev/null \
                        && echo "$name" >> "$RUN/fetches"
                done
                sleep 0.1
            done
        ) &
        STORM_PIDS="$STORM_PIDS $!"
    done
}

storm_stop() {
    local p

    for p in $STORM_PIDS; do
        kill -KILL "$p" 2>/dev/null || true
    done

    STORM_PIDS=""
}

switches() {
    curl -fsS "$API/streams/live/soak" 2>/dev/null \
        | sed -n 's/.*"switches":\([0-9]*\).*/\1/p' || true
}

worker_metric() {
    curl -fsS "$API/metrics" 2>/dev/null \
        | sed -n "/^$1 /{s/^$1 //;p;q;}"
}

# the standby goes on air first, so that the program is verifiably carrying a
# source before anything is measured: an "active" that was already the value we
# are waiting for would time nothing at all
PUB_B="$(start_publisher encoder-b 120 "smptehdbars=size=320x240:rate=25" pub_b)"
wait_for_active encoder-b

for _ in $(seq 1 400); do
    have="$(grep -c '^#EXTINF' "$RUN/hls/live/soak/index.m3u8" 2>/dev/null || true)"
    [ "${have:-0}" -ge 1 ] && break
    sleep 0.1
done

# and the primary comes back, which is the switchback the config asks for
PUB="$(start_publisher encoder-a 120 "testsrc=size=320x240:rate=25")"
wait_for_active encoder-a

echo "   on air: encoder-a, with encoder-b healthy behind it"

echo "== failover with no receivers"
SWITCHES_BEFORE="$(switches)"
kill -KILL "$PUB" 2>/dev/null || true
PUB=0

QUIET_MS="$(time_to_active encoder-b)" \
    || { echo "the program did not fail over at all without a storm" >&2
         exit 1; }

echo "   measured: ${QUIET_MS} ms to report encoder-b"

QUIET_SWITCHES="$(switches)"

[ "${QUIET_SWITCHES:-0}" -gt "${SWITCHES_BEFORE:-0}" ] \
    || { echo "no switch was counted: the program was already reporting" \
              "encoder-b, so nothing was timed" >&2
         exit 1; }

# back on the primary, so the storm measurement starts from the same place
PUB="$(start_publisher encoder-a 120 "testsrc=size=320x240:rate=25")"
wait_for_active encoder-a

echo "== failover under the storm"
storm_start
sleep 3

FRAMES_BEFORE="$(program_frames)"
SWITCHES_BEFORE="$(switches)"

kill -KILL "$PUB" 2>/dev/null || true
PUB=0

STORM_MS="$(time_to_active encoder-b)" \
    || { echo "the program did not fail over under a receiver storm" >&2
         storm_stop
         exit 1; }

FRAMES_AFTER="$(program_frames)"
FETCHES="$(wc -l < "$RUN/fetches" 2>/dev/null || true)"
SWITCHES_AFTER="$(switches)"

storm_stop

echo "   measured: ${STORM_MS} ms to report encoder-b" \
     "(${FETCHES} segment fetches served while the source died)"
echo "   program frames across the storm: ${FRAMES_BEFORE} -> ${FRAMES_AFTER}"

[ "${SWITCHES_AFTER:-0}" -gt "${SWITCHES_BEFORE:-0}" ] \
    || { echo "no switch was counted under the storm: the program was" \
              "already reporting encoder-b, so nothing was timed" >&2
         exit 1; }

[ "${FETCHES:-0}" -ge 50 ] \
    || { echo "the storm served only ${FETCHES} segment fetches: the load" \
              "never happened" >&2
         exit 1; }

echo "   the storm was real: ${FETCHES} fetches"

[ "${FRAMES_AFTER:-0}" -gt "${FRAMES_BEFORE:-0}" ] \
    || { echo "the program stopped carrying frames while the storm ran" >&2
         exit 1; }

echo "   the program kept carrying frames through the storm"

[ "${QUIET_MS:-99999}" -le "$SLA_BOUND_MS" ] \
    || { echo "failover with no receivers took ${QUIET_MS}ms, over the" \
              "${SLA_BOUND_MS}ms bound (${FAILOVER_SLA_MS}ms SLA +" \
              "${SLA_MARGIN_MS}ms margin)" >&2
         exit 1; }

[ "${STORM_MS:-99999}" -le "$SLA_BOUND_MS" ] \
    || { echo "failover under a receiver storm took ${STORM_MS}ms, over the" \
              "${SLA_BOUND_MS}ms bound (${FAILOVER_SLA_MS}ms SLA +" \
              "${SLA_MARGIN_MS}ms margin): receivers delayed the failover" >&2
         exit 1; }

# the SLA bound above is the claim of record, and this is the same claim
# stated as the comparison it is about: adding receivers must not push the
# failover out by more than the margin the bound already allows
STORM_DELAY=$(( STORM_MS - QUIET_MS ))

[ "$STORM_DELAY" -le "$SLA_MARGIN_MS" ] \
    || { echo "a receiver storm added ${STORM_DELAY}ms to the failover" \
              "(${QUIET_MS}ms quiet, ${STORM_MS}ms under load): receivers" \
              "delayed the failover beyond the ${SLA_MARGIN_MS}ms margin" >&2
         exit 1; }

echo "   ${QUIET_MS}ms quiet and ${STORM_MS}ms under load: the storm added" \
     "${STORM_DELAY}ms, both within ${SLA_BOUND_MS}ms"

# the conditions the numbers hold under: a tick the storm delayed by more than
# its interval is the mechanism by which receivers would push a failover out
echo "   worker tick interval: worst" \
     "$(worker_metric nginx_media_worker_event_loop_max_delay_ms) ms" \
     "(asked for 100ms)," \
     "$(worker_metric nginx_media_worker_late_ticks_total) late ticks," \
     "worst visit $(worker_metric nginx_media_worker_max_service_ms) ms"

kill -KILL "$PUB_B" 2>/dev/null || true
PUB_B=0

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

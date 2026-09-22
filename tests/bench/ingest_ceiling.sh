#!/usr/bin/env bash
#
# Where SRT ingest saturates, and on whose thread.
#
# libsrt has its own runtime: it creates its own threads and, for a listener,
# the UDP receive and the demultiplexing of sessions happen there rather than
# on the thread that calls srt_recvmsg.  Which means the ceiling on ingest is
# not simply "one nginx worker" - it is whichever thread runs out first, and
# that is worth measuring rather than reasoning about.
#
# This ramps publishers onto one SRT endpoint and reports, at each step, how
# much CPU each *class* of thread is burning.  libsrt names its threads, so
# /proc/<pid>/task/*/comm separates them from nginx's workers, and the answer
# to "what saturates" is read off directly rather than inferred.
#
# Publishers stream a pre-encoded file with -c copy, so the measurement is the
# receiver's cost and not ffmpeg's encoder.
#
# Conditions are printed with the numbers.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/ingest-ceiling"
BASE=$(( 24000 + ($$ % 40) * 8 ))
HTTP_PORT="$BASE"
SRT_PORT="$(( BASE + 1 ))"
BITRATE="${BITRATE:-6M}"
STEPS="${STEPS:-2 4 8 12 16}"
WINDOW="${WINDOW:-6}"
WORKERS="${WORKERS:-1}"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/media"

PUB_PIDS=()

cleanup() {
    for p in "${PUB_PIDS[@]:-}"; do kill -KILL "$p" 2>/dev/null; done
    if [ -f "$RUN/logs/nginx.pid" ]; then
        kill -QUIT "$(cat "$RUN/logs/nginx.pid")" 2>/dev/null
        sleep 1
        kill -KILL "$(cat "$RUN/logs/nginx.pid")" 2>/dev/null
    fi
    return 0
}
trap cleanup EXIT

[ -x "$NGINX" ] || { echo "nginx is not built; run: make nginx" >&2; exit 1; }

echo "== encoding ${BITRATE} 720p source (publishers stream it with -c copy)"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=1280x720:rate=25" \
    -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p \
    -b:v "$BITRATE" -maxrate "$BITRATE" -bufsize 12M \
    -t 120 -f mpegts "$RUN/media/source.ts"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes $WORKERS;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 4096; }

media_hls $RUN/hls;
media_srt_listen 127.0.0.1:$SRT_PORT;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT reuseport;

        location /media/api/ { media_api; }
        location /hls/       { alias $RUN/hls/; }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 || {
    echo "configuration rejected" >&2; exit 1; }

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 1

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

# every thread the instance owns, by name, between two samples
thread_cpu() {
    local pid dir tid comm a b
    for dir in /proc/$(pgrep -f "nginx: master.*$RUN" | head -1) \
               $(pgrep -f "nginx: worker.*$RUN")
    do
        [ -d "$dir/task" ] || continue
        for tid in "$dir"/task/*; do
            [ -r "$tid/comm" ] || continue
            comm="$(cat "$tid/comm" 2>/dev/null)"
            a="$(awk '{print $14+$15}' "$tid/stat" 2>/dev/null)"
            echo "$comm ${a:-0}"
        done
    done
}

printf '%-8s %-10s %-12s %-12s %-10s %s\n' \
    publishers delivered worker-cpu srt-cpu total-cpu thread-hottest

for K in $STEPS; do

    # fresh instance per step, so nothing carries over
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s stop 2>/dev/null
    sleep 1
    : > "$RUN/logs/error.log"
    "$NGINX" -p "$RUN" -c conf/nginx.conf
    sleep 1

    for i in $(seq 1 "$K"); do
        curl -fsS -X POST -H 'Content-Type: application/json' \
            -d "{\"application\":\"live\",\"name\":\"c$i\"}" \
            "$API/streams" >/dev/null 2>&1
    done

    PUB_PIDS=()
    for i in $(seq 1 "$K"); do
        ffmpeg -hide_banner -loglevel error -re -stream_loop -1 \
            -i "$RUN/media/source.ts" -c copy -f mpegts \
            "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/c$i,m=publish,s=p$i" \
            >/dev/null 2>&1 &
        PUB_PIDS+=("$!")
    done

    sleep "$WINDOW"

    BEFORE="$(thread_cpu)"
    sleep "$WINDOW"
    AFTER="$(thread_cpu)"

    # aggregate by thread name, in hundredths of a second of CPU per second
    report="$(printf '%s\n' "$BEFORE" | while read -r n v; do
        a="$(printf '%s\n' "$AFTER" | awk -v n="$n" '$1 == n {print $2; exit}')"
        printf '%s %s\n' "$n" "$(( (${a:-0} - v) * 100 / (WINDOW * 100) ))"
    done | awk '{cpu[$1] += $2} END { for (n in cpu) printf "%s %d\n", n, cpu[n] }')"

    worker="$(printf '%s\n' "$report" | awk '/^nginx/ {s += $2} END {print s + 0}')"
    srt="$(printf '%s\n' "$report" | awk '/^SRT/ {s += $2} END {print s + 0}')"
    total=$(( worker + srt ))
    hottest="$(printf '%s\n' "$report" | sort -k2 -rn | head -1)"

    delivered=0
    for i in $(seq 1 "$K"); do
        n="$(curl -fsS "$API/streams/live/c$i" 2>/dev/null \
            | grep -o '"frames_in":[0-9]*' | head -1 | cut -d: -f2)"
        [ -n "$n" ] && delivered=$((delivered + 1))
    done

    printf '%-8s %-10s %-12s %-12s %-10s %s\n' \
        "$K" "$delivered/$K" "${worker}%" "${srt}%" "${total}%" "$hottest"

    for p in "${PUB_PIDS[@]}"; do kill -KILL "$p" 2>/dev/null; done
    PUB_PIDS=()
    sleep 1
done

trap - EXIT
cleanup

echo
echo "== conditions"
echo "   source:     $BITRATE 720p25, pre-encoded, publishers use -c copy"
echo "   workers:    $WORKERS, one SRT endpoint"
echo "   window:     ${WINDOW}s per sample"
echo "   cpu:        per thread class, percent of one core, sampled over the window"
echo "   host:       single host, $(nproc) cpus"
echo "== ingest ceiling done"

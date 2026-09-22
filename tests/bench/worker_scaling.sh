#!/usr/bin/env bash
#
# Worker scaling: what more workers buy, and what they do not.
#
# The architecture gives each logical program exactly one owner worker, and
# the tick that drives selection, fanout and outputs runs there and nowhere
# else.  That has a consequence worth measuring rather than assuming: one
# program's egress cannot be spread over workers, so fanout for a single
# program is bound by one worker no matter how many exist.  Ingest is the
# other half - publishers route to the owner - so what scales is the number
# of programs, not the size of one.
#
# This measures both, per worker count:
#
#   api     reads and writes seen by the worker that answers.  The graph is
#           per-worker state and the control API does not route to the owner,
#           so a request can land on a worker that has never heard of the
#           stream.  This is the number that says how usable the API is at a
#           given worker count.
#   single  one program carried to one consumer: frames out and fanout delay
#   many    four programs carried at once: total frames, which is what should
#           rise with workers
#
# Conditions are printed with the numbers, because a fanout figure without
# them means nothing.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/worker-scaling"
BASE=$(( 21000 + ($$ % 60) * 8 ))
HTTP_PORT="$BASE"
SRT_PORT="$(( BASE + 1 ))"
SOURCE_SECONDS="${SOURCE_SECONDS:-12}"
WORKER_COUNTS="${WORKER_COUNTS:-1 2 3 4}"
READS="${READS:-24}"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/media"

cleanup() {
    pkill -f "nginx: .*$RUN" 2>/dev/null
    return 0
}
trap cleanup EXIT

[ -x "$NGINX" ] || { echo "nginx is not built; run: make nginx" >&2; exit 1; }

echo "== generating ${SOURCE_SECONDS}s of source media"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 32k \
    -t "$SOURCE_SECONDS" -f mpegts "$RUN/media/source.ts"

printf '%-8s %-14s %-10s %-12s %-12s %s\n' \
    workers api-reads api-writes single-frames many-frames fanout-p99

for W in $WORKER_COUNTS; do

    pkill -f "nginx: .*$RUN" 2>/dev/null
    sleep 1

    cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes $W;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 1024; }

media_hls $RUN/hls;
media_srt_listen 127.0.0.1:$SRT_PORT;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /media/api/ { media_api; }
        location /hls/       { alias $RUN/hls/; }
    }
}
EOF

    "$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 || {
        echo "$W: configuration rejected" >&2; continue; }

    "$NGINX" -p "$RUN" -c conf/nginx.conf
    sleep 1

    API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

    api_reads() {
        local ok=0 miss=0 code
        for _ in $(seq 1 "$READS"); do
            code="$(curl -sS -o /dev/null -w '%{http_code}' \
                "$API/streams/live/scale" 2>/dev/null)"
            [ "$code" = "200" ] && ok=$((ok + 1)) || miss=$((miss + 1))
        done
        printf '%s/%s' "$ok" "$READS"
    }

    # writes: create four programs, then read each back once
    api_writes() {
        local ok=0 id
        for id in a b c d; do
            curl -fsS -X POST -H 'Content-Type: application/json' \
                -d "{\"application\":\"live\",\"name\":\"w-$id\"}" \
                "$API/streams" >/dev/null 2>&1
        done
        for id in a b c d; do
            [ "$(curl -sS -o /dev/null -w '%{http_code}' \
                 "$API/streams/live/w-$id" 2>/dev/null)" = "200" ] \
                && ok=$((ok + 1))
        done
        printf '%s/4' "$ok"
    }

    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d '{"application":"live","name":"scale"}' "$API/streams" >/dev/null 2>&1

    READS_RESULT="$(api_reads)"
    WRITES_RESULT="$(api_writes)"

    # one program, one source, measured while it carries media
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"id\":\"file1\",\"type\":\"file\",\"path\":\"$RUN/media/source.ts\"}" \
        "$API/streams/live/scale/sources" >/dev/null 2>&1

    # Read until a worker that actually holds the stream answers.  With more
    # than one worker a request can land on one that has never heard of it, so
    # reading once measures the API's consistency rather than the program.
    read_field() {
        local path="$1" field="$2" v
        for _ in $(seq 1 40); do
            v="$(curl -fsS "$API$path" 2>/dev/null \
                | grep -o "\"$field\":[0-9]*" | head -1 | cut -d: -f2)"
            [ -n "$v" ] && { printf '%s' "$v"; return; }
            sleep 0.25
        done
        printf '0'
    }

    sleep 6
    single="$(read_field /streams/live/scale program_frames)"

    # four programs at once: this is the half that should scale
    for id in a b c d; do
        curl -fsS -X POST -H 'Content-Type: application/json' \
            -d "{\"id\":\"f-$id\",\"type\":\"file\",\"path\":\"$RUN/media/source.ts\"}" \
            "$API/streams/live/w-$id/sources" >/dev/null 2>&1
    done

    sleep 6
    many=0
    for id in a b c d; do
        n="$(read_field "/streams/live/w-$id" program_frames)"
        many=$(( many + ${n:-0} ))
    done

    p99="$(curl -fsS "$API/metrics" 2>/dev/null \
        | grep 'nginx_media_stream_fanout_delay_ms' \
        | grep 'percentile="99"' | head -1 | awk '{print $2}')"

    printf '%-8s %-14s %-10s %-12s %-12s %s\n' \
        "$W" "$READS_RESULT" "$WRITES_RESULT" "${single:-0}" "$many" "${p99:-?}"

    pkill -f "nginx: .*$RUN" 2>/dev/null
    sleep 1
done

trap - EXIT
cleanup

echo
echo "== conditions"
echo "   source:   file source, ffmpeg testsrc2 320x240p25, ${SOURCE_SECONDS}s"
echo "   consumer: the HLS output, which is what drives the fanout"
echo "   api:      reads and writes answered by the worker that took them"
echo "   host:     single host, no netem, $(nproc) cpus"
echo "== worker scaling done"

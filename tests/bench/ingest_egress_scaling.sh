#!/usr/bin/env bash
#
# Ingest and egress scale differently, and this measures how.
#
# One logical program has exactly one owner worker.  That worker runs the tick
# that drives selection, fanout and outputs, and nobody else does - so the
# work of carrying *one* program is bound by one worker whatever the worker
# count.  What more workers buy is more programs: each program's owner is
# chosen independently, so N programs spread over min(N, programs) workers.
#
# That is the shape worth knowing before sizing a deployment, and it is not
# the shape "add workers until it holds" assumes.  This measures, per worker
# count, with a fixed number of programs:
#
#   programs   how many are carried
#   frames     total carried, which is the ingest+fanout work actually done
#   owners     distinct worker processes that carried any of it.  This is the
#              spread: with four programs and one worker it is 1, and with
#              four workers it should be 4.
#   worst      the largest tick service time any worker reported, which is the
#              headroom figure - it is what a worker spends serving every
#              program it owns, so it is the number that bounds capacity
#
# Conditions are printed with the numbers.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/ingest-egress"
BASE=$(( 22000 + ($$ % 60) * 8 ))
HTTP_PORT="$BASE"
SRT_PORT="$(( BASE + 1 ))"
PROGRAMS="${PROGRAMS:-4}"
WINDOW="${WINDOW:-8}"
WORKER_COUNTS="${WORKER_COUNTS:-1 2 3 4}"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/media"

cleanup() {
    pkill -f "nginx: .*$RUN" 2>/dev/null
    return 0
}
trap cleanup EXIT

[ -x "$NGINX" ] || { echo "nginx is not built; run: make nginx" >&2; exit 1; }

echo "== generating source media"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 32k -t 20 -f mpegts "$RUN/media/source.ts"

printf '%-8s %-10s %-10s %-8s %s\n' workers programs frames owners worst-service-ms

for W in $WORKER_COUNTS; do

    pkill -f "nginx: .*$RUN" 2>/dev/null
    sleep 1
    rm -rf "$RUN/hls"
    mkdir -p "$RUN/hls"

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

    for i in $(seq 1 "$PROGRAMS"); do
        curl -fsS -X POST -H 'Content-Type: application/json' \
            -d "{\"application\":\"live\",\"name\":\"p$i\"}" \
            "$API/streams" >/dev/null 2>&1

        curl -fsS -X POST -H 'Content-Type: application/json' \
            -d "{\"id\":\"f$i\",\"type\":\"file\",\"path\":\"$RUN/media/source.ts\"}" \
            "$API/streams/live/p$i/sources" >/dev/null 2>&1
    done

    sleep "$WINDOW"

    frames=0
    served=0
    owner_list=""

    for i in $(seq 1 "$PROGRAMS"); do
        # The graph is replicated, so any worker can answer; the document says
        # which worker owns the program, which is what the spread is.
        for _ in $(seq 1 20); do
            doc="$(curl -fsS "$API/streams/live/p$i" 2>/dev/null)"
            echo "$doc" | grep -q '"program_frames"' && break
            sleep 0.25
        done

        [ -n "${doc:-}" ] && served=$((served + 1))
        n="$(printf '%s' "$doc" | grep -o '"program_frames":[0-9]*' | head -1 | cut -d: -f2)"
        frames=$((frames + ${n:-0}))

        o="$(printf '%s' "$doc" | grep -o '"owner":[0-9]*' | head -1 | cut -d: -f2)"
        [ -n "$o" ] && owner_list="$owner_list $o"
    done

    owners="$(printf '%s\n' $owner_list | sort -u | wc -l)"

    worst="$(grep -aoE 'media: worker .* service=[0-9]+' "$RUN/logs/error.log" \
        2>/dev/null | grep -oE '[0-9]+$' | sort -n | tail -1)"

    printf '%-8s %-10s %-10s %-8s %s\n' \
        "$W" "$served/$PROGRAMS" "$frames" "${owners:-?}" "${worst:-0}"

    pkill -f "nginx: .*$RUN" 2>/dev/null
    sleep 1
done

trap - EXIT
cleanup

echo
echo "== conditions"
echo "   programs:  $PROGRAMS file sources, one per program, paced by the tick"
echo "   window:    ${WINDOW}s"
echo "   host:      single host, no netem, $(nproc) cpus"
echo "== ingest and egress scaling done"

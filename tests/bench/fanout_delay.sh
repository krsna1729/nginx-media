#!/usr/bin/env bash
#
# fanout_delay: what a unit of media waits between the program publishing it
# and a consumer taking it (goal doc 32, 34 item 20).
#
# A capacity number is only a measurement when its conditions travel with it,
# so this prints the percentiles from nginx_media_stream_fanout_delay_ms
# alongside the worker count, the source, the segment duration, the consumer
# count, the host and whether netem shaped the path -- and then the worker's
# own tick cost, which is the bound the fanout has to hold inside.
#
# It also asserts the things the numbers alone would let you miss:
#   * all four percentiles are reported, they are ordered, and the program
#     actually dispatched units: a percentile of nothing is not a measurement
#   * one scheduler visit stays bounded under the load, so the worst tick
#     service time is under the 100ms interval and no tick is late by more
#     than half of it -- measured both under a paced source and under a burst
#     source that leaves a multi-megabyte backlog in the ring for the visits
#     to walk (goal doc 34 item 13)
#   * seven more rtmp players do not cost the worker seven more copies of the
#     media: the memory they add is a bound, not a multiple of the payload
#     (goal doc 34 item 15)
#
# Environment: WORKERS (default 1), CONSUMERS (default 16), PUB_SECONDS (75).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/bench-fanout-delay"

# ports from the pid, so two runs on one host cannot collide
BASE=$(( 20100 + ($$ % 60) * 6 ))
SRT_PORT="${FANOUT_SRT_PORT:-$BASE}"
HTTP_PORT="${FANOUT_HTTP_PORT:-$(( BASE + 1 ))}"
RTMP_PORT="${FANOUT_RTMP_PORT:-$(( BASE + 2 ))}"

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
HLS_URL="http://127.0.0.1:$HTTP_PORT/hls"

WORKERS="${WORKERS:-1}"
CONSUMERS="${CONSUMERS:-16}"
PUB_SECONDS="${PUB_SECONDS:-75}"

# Added-memory bound for eight rtmp players over one.  What the unit test
# asserts precisely is pointer identity: every player's packet points at the
# program's one payload buffer and takes one reference on it (goal doc 34 item
# 15).  What this bounds is the resident cost of a connection, since that is
# what an operator sees.  The number is the marginal cost of seven more players
# measured in the same run, and part of it is the shared FLV ring filling over
# the same window rather than anything per player, so the bound is loose on
# purpose: it is set to catch a per-connection copy of the retained program
# window -- the multi-megabyte failure mode -- without tracking allocator
# noise.
PLAYER_MEMORY_BOUND_KB="${PLAYER_MEMORY_BOUND_KB:-8192}"

# one tick is 100ms; a visit that took a tenth of that would already be a
# stall, and under this load a visit is over in under a millisecond
TICK_SERVICE_BOUND_MS="${TICK_SERVICE_BOUND_MS:-10}"

# the backlog the burst phase has to build for the per-visit bound to mean
# anything.  The program feed retains at most 2048 units, and a burst faster
# than the reaper fills it: this is the backlog that says the visits really did
# have tens of 64-unit reads to do, not the handful a paced source leaves.
BACKLOG_BOUND_UNITS="${BACKLOG_BOUND_UNITS:-2000}"

PUB=0
STORM=0
PLAYERS=""

# kill -KILL 0 signals the whole process group, which is how a bench takes the
# shell that started it down with it: every signal here goes through this
kill_pid() {
    case "$1" in
        ""|0) return 0 ;;
    esac

    kill -KILL "$1" 2>/dev/null || true
}

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

cleanup() {
    kill_pid "$PUB"
    kill_pid "$STORM"

    for p in $PLAYERS; do
        kill_pid "$p"
    done

    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
    return 0
}
trap cleanup EXIT

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/rec" "$RUN/players"

# A run that was killed leaves its master holding this RUN's SRT and HTTP
# ports.  The config test would still pass and the next publisher would then
# never reach us, which looks like a broken streamer rather than a leftover
# process.  Clear this RUN's own master out and prove the ports are free.
leftover="$(pgrep -f "nginx: master process .* -p $RUN " 2>/dev/null || true)"

for p in $leftover; do
    echo "   stopping a leftover master $p from this RUN"
    kill -TERM "$p" 2>/dev/null || true
done

for _ in $(seq 1 100); do
    [ -z "$(pgrep -f "nginx: master process .* -p $RUN " 2>/dev/null || true)" ] \
        && break
    sleep 0.05
done

busy="$( { ss -ltn 2>/dev/null; ss -lun 2>/dev/null; } \
         | grep -E ":($SRT_PORT|$HTTP_PORT|$RTMP_PORT) " || true)"

[ -z "$busy" ] \
    || { echo "these ports are already bound:" >&2
         printf '%s\n' "$busy" >&2
         echo "another run holds this pid's port block; rerun" >&2
         exit 1; }

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes $WORKERS;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 4096;
}

media_hls $RUN/hls;
media_record $RUN/rec/program.ts;

media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;

media_rtmp_listen 127.0.0.1:$RTMP_PORT;

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

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >"$RUN/conf.log" 2>&1 \
    || { echo "configuration rejected" >&2; cat "$RUN/conf.log" >&2; exit 1; }

worker_pids() {
    # the workers of *this* run: the children of our own master.  Matching the
    # process title alone would also count a sibling test's nginx, and RSS
    # measured across two runs says nothing about either.
    local master

    master="$(cat "$RUN/logs/nginx.pid" 2>/dev/null || true)"
    [ -n "$master" ] || return 0

    pgrep -P "$master" 2>/dev/null || true
}

worker_rss_kb() {
    local pid total=0

    for pid in $(worker_pids); do
        total=$(( total + $(awk '/VmRSS/ { print $2 }' \
            "/proc/$pid/status" 2>/dev/null || echo 0) ))
    done

    echo "$total"
}

require_worker() {
    # a global "pkill nginx" from another run takes this bench's master with
    # it, and the symptoms -- a refused connection, a stalled publisher -- look
    # like a broken media path rather than a process that is simply gone
    [ -n "$(worker_pids)" ] \
        || { echo "the worker is gone after ${1}: something outside this" \
                  "bench killed this run's nginx" >&2
             exit 1; }
}

metric() {
    local out

    # the first matching sample; sed quits on the match so nothing gets a
    # SIGPIPE that pipefail would turn into a failure
    out="$(curl -fsS "$API/metrics" 2>/dev/null \
           | sed -n "/^$1 /{s/^$1 //;p;q;}")" \
        || { echo "the metrics endpoint did not answer: something outside" \
                  "this bench killed this run's nginx" >&2
             exit 1; }

    printf '%s' "$out"
}

percentile() {
    metric "nginx_media_stream_fanout_delay_ms{application=\"live\",name=\"bench\",percentile=\"$1\"}"
}

stream_field() {
    # greedy sed: the last occurrence of the field on the stream's one line
    curl -fsS "$API/streams/live/bench" 2>/dev/null \
        | sed -n "s/.*\"$1\":\([0-9]*\).*/\1/p" || true
}

netem_note() {
    local qdisc

    qdisc="$(tc qdisc show dev lo 2>/dev/null | grep netem || true)"

    if [ -n "$qdisc" ]; then
        echo "yes on lo: $qdisc"
    else
        echo "no (loopback unshaped)"
    fi
}

segment_duration() {
    local extinf

    extinf="$(grep -m1 '^#EXTINF:' "$RUN/hls/index.m3u8" 2>/dev/null || true)"
    extinf="${extinf#\#EXTINF:}"
    extinf="${extinf%%,*}"

    if [ -n "$extinf" ]; then
        echo "${extinf}s"
    else
        echo "(target 6s; no segment closed yet)"
    fi
}

echo "== config test and start"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

grep -q 'srt listener ready' "$RUN/logs/error.log" \
    || { echo "listener not ready" >&2; exit 1; }

[ -n "$(worker_pids)" ] || { echo "no worker process" >&2; exit 1; }

echo "== publishing for ${PUB_SECONDS}s (720p25, libx264 ultrafast, 2 Mbps)"
timeout "$PUB_SECONDS" ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=1280x720:rate=25" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset ultrafast -b:v 2M -g 50 -pix_fmt yuv420p \
    -c:a aac -b:a 96k \
    -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=%23!::r%3Dlive%2Fbench%2Cm%3Dpublish%2Cs%3Dencoder-a" \
    >"$RUN/pub.log" 2>&1 &
PUB=$!

for _ in $(seq 1 300); do
    grep -q 'srt source open' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

for _ in $(seq 1 400); do
    closed="$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" 2>/dev/null || true)"
    [ "${closed:-0}" -ge 2 ] && break
    sleep 0.1
done

SEGMENTS="$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" 2>/dev/null || true)"
[ "${SEGMENTS:-0}" -ge 2 ] \
    || { echo "the segmenter never closed two segments" >&2
         echo "   workers: $(worker_pids | wc -l) (master" \
              "$(cat "$RUN/logs/nginx.pid" 2>/dev/null || echo gone))" >&2
         echo "   publisher: $(kill -0 "$PUB" 2>/dev/null \
                                 && echo alive || echo gone)" >&2
         echo "   last error log lines:" >&2
         tail -3 "$RUN/logs/error.log" >&2
         echo "   last publisher lines:" >&2
         tail -3 "$RUN/pub.log" >&2
         exit 1; }

echo "   $SEGMENTS segments closed, program frames $(stream_field program_frames)"

echo "== receiver storm: $CONSUMERS concurrent hls fetches for 12s"

# one curl process holds the consumer count open: --parallel-max is the number
# of concurrent connections, and the loop re-reads the playlist so freshly
# closed segments are fetched too
storm() {
    local list="$RUN/urls.txt" name

    while :; do
        : > "$list"

        for name in $(grep -v '^#' "$RUN/hls/index.m3u8" 2>/dev/null \
                      | grep '\.ts$' || true); do
            printf 'url = "%s/%s"\noutput = "/dev/null"\n' \
                "$HLS_URL" "$name" >> "$list"
        done

        if [ ! -s "$list" ]; then
            sleep 0.2
            continue
        fi

        curl -fsS --parallel --parallel-max "$CONSUMERS" \
            --config "$list" >/dev/null 2>&1 || true

        sleep 0.05
    done
}

storm &
STORM=$!

sleep 12

require_worker "the storm"

# --- the published numbers and the conditions they hold under -------------

P50="$(percentile 50)"
P95="$(percentile 95)"
P99="$(percentile 99)"
P999="$(percentile 99.9)"
DISPATCHED="$(metric 'nginx_media_stream_dispatched_total{application="live",name="bench"}')"
PROGRAM_FRAMES="$(stream_field program_frames)"
FEED_UNITS="$(metric 'nginx_media_stream_feed_units{application="live",name="bench"}')"
FEED_BYTES="$(metric 'nginx_media_stream_feed_bytes{application="live",name="bench"}')"
SERVICE_MS="$(metric nginx_media_worker_service_ms)"
MAX_SERVICE_MS="$(metric nginx_media_worker_max_service_ms)"
LATE_TICKS="$(metric nginx_media_worker_late_ticks_total)"

echo
echo "== conditions"
echo "   host:             $(uname -srm), $(nproc) cpus"
echo "   workers:          $WORKERS ($(worker_pids | wc -l) worker processes)"
echo "   source:           srt publisher, ffmpeg testsrc2 1280x720p25 2 Mbps,"
echo "                     libx264 ultrafast, aac 96k, encoder-a priority 100"
echo "   segment duration: $(segment_duration)"
echo "   outputs:          hls segmenter + program recording"
echo "   consumers:        $CONSUMERS concurrent hls segment fetches over http"
echo "   netem:            $(netem_note)"
echo "   fanout:           nginx_media_stream_fanout_delay_ms ="
echo "                     dispatch time minus program publish time"

echo
echo "== fanout_delay at load (ms)"
printf '   p50 %s ms   p95 %s ms   p99 %s ms   p99.9 %s ms\n' \
    "$P50" "$P95" "$P99" "$P999"
echo "   dispatched units: $DISPATCHED"
echo "   program frames:   $PROGRAM_FRAMES"
echo "   feed retained:    $FEED_UNITS units, $FEED_BYTES bytes"

echo
echo "== worker tick cost (bound: <${TICK_SERVICE_BOUND_MS}ms service,"
echo "   no tick late by more than half the 100ms interval)"
echo "   last service:     $SERVICE_MS ms"
echo "   worst service:    $MAX_SERVICE_MS ms"
echo "   late ticks:       $LATE_TICKS"

for pair in "p50=$P50" "p95=$P95" "p99=$P99" "p99.9=$P999"; do
    [ -n "${pair#*=}" ] \
        || { echo "${pair%%=*} was not reported by /media/api/v1/metrics" >&2
             exit 1; }
done

echo "   all four percentiles reported"

[ "${DISPATCHED:-0}" -gt 0 ] \
    || { echo "nothing was dispatched, so the percentiles are empty" >&2
         exit 1; }

echo "   $DISPATCHED units dispatched by consumers during the storm"

[ "$P50" -le "$P95" ] && [ "$P95" -le "$P99" ] && [ "$P99" -le "$P999" ] \
    || { echo "the percentiles are not ordered: $P50 $P95 $P99 $P999" >&2
         exit 1; }

echo "   percentiles are ordered"

# a histogram that records nothing would report four zeroes; a realtime source
# through a 100ms tick cannot dispatch every unit in the same millisecond as
# it was published
[ $(( P50 + P95 + P99 + P999 )) -gt 0 ] \
    || { echo "every percentile is zero: the histogram is not recording" >&2
         exit 1; }

echo "   the delay histogram recorded the dispatch (p50 ${P50}ms)"

[ "${MAX_SERVICE_MS:-9999}" -lt "$TICK_SERVICE_BOUND_MS" ] \
    || { echo "a scheduler visit took ${MAX_SERVICE_MS}ms, over the" \
              "${TICK_SERVICE_BOUND_MS}ms bound: fanout work per visit is" \
              "not bounded" >&2
         exit 1; }

echo "   worst scheduler visit ${MAX_SERVICE_MS}ms < ${TICK_SERVICE_BOUND_MS}ms"

[ "${LATE_TICKS:-1}" -eq 0 ] \
    || { echo "$LATE_TICKS ticks were late by more than half the interval" >&2
         exit 1; }

echo "   no tick missed its interval by more than half"

# --- do rtmp players share the payloads or copy them per connection? ------

echo
echo "== rtmp players: does the worker's memory grow per connection?"
echo "   (bound: ${PLAYER_MEMORY_BOUND_KB} kB added by seven more players)"

# baseline with no players, media already flowing and the storm running
RSS_BASE="$(worker_rss_kb)"

start_player() {
    ffmpeg -hide_banner -loglevel error -re \
        -i "rtmp://127.0.0.1:$RTMP_PORT/live/bench" \
        -t 30 -c copy -f null - >"$RUN/players/$1.log" 2>&1 &
    PLAYERS="$PLAYERS $!"
}

start_player one
sleep 6

RSS_ONE="$(worker_rss_kb)"

for i in $(seq 2 8); do
    start_player "$i"
done

sleep 6

RSS_EIGHT="$(worker_rss_kb)"

require_worker "the rtmp players"

# a zero here would make the bound below pass by accident
[ "${RSS_EIGHT:-0}" -gt 0 ] \
    || { echo "could not read the worker's resident set" >&2; exit 1; }

PLAYER_SESSIONS="$(grep -c 'rtmp player stream=live/bench' \
    "$RUN/logs/error.log" 2>/dev/null || true)"

echo "   worker rss: ${RSS_BASE} kB idle, ${RSS_ONE} kB with one player," \
     "${RSS_EIGHT} kB with eight"
echo "   one player cost $(( RSS_ONE - RSS_BASE )) kB," \
     "seven more cost $(( RSS_EIGHT - RSS_ONE )) kB"
echo "   player sessions served: $PLAYER_SESSIONS"

[ "${PLAYER_SESSIONS:-0}" -ge 8 ] \
    || { echo "only $PLAYER_SESSIONS player sessions were served: the load" \
              "never happened" >&2
         exit 1; }

ADDED_KB=$(( RSS_EIGHT - RSS_ONE ))

[ "$ADDED_KB" -lt "$PLAYER_MEMORY_BOUND_KB" ] \
    || { echo "seven more rtmp players added ${ADDED_KB} kB, over the" \
              "${PLAYER_MEMORY_BOUND_KB} kB bound: players are copying the" \
              "media instead of sharing it" >&2
         exit 1; }

echo "   seven more players added ${ADDED_KB} kB" \
     "< ${PLAYER_MEMORY_BOUND_KB} kB bound: a connection costs a session" \
     "and its chunk headers, not a copy of the program's media"

# --- a deep backlog: what one scheduler visit costs -----------------------

echo
echo "== burst source: a real backlog, to price one scheduler visit"
echo "   (a visit takes the feed in 64-unit reads, so a backlog this deep"
echo "    needs several reads per visit -- and stays bounded regardless)"

for p in $PLAYERS; do
    kill_pid "$p"
done
PLAYERS=""

kill_pid "$PUB"
PUB=0

# no -re: the encoder publishes 60s of media in a couple of seconds, so the
# program feed fills to its 2048-unit ceiling and the visits have tens of
# 64-unit reads to do instead of the handful a paced source leaves behind
ffmpeg -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=1280x720:rate=25" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset ultrafast -b:v 2M -g 50 -pix_fmt yuv420p \
    -c:a aac -b:a 96k -t 60 \
    -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=%23!::r%3Dlive%2Fbench%2Cm%3Dpublish%2Cs%3Dencoder-a" \
    >"$RUN/burst.log" 2>&1 &
PUB=$!

wait "$PUB" 2>/dev/null || true
PUB=0

# let the remaining ticks walk what the burst left in the ring
sleep 3

require_worker "the burst"

BACKLOG_BYTES="$(metric 'nginx_media_stream_feed_bytes{application="live",name="bench"}')"
BACKLOG_UNITS="$(metric 'nginx_media_stream_feed_units{application="live",name="bench"}')"
BURST_SERVICE_MS="$(metric nginx_media_worker_max_service_ms)"
BURST_LATE="$(metric nginx_media_worker_late_ticks_total)"

echo "   backlog:          ${BACKLOG_UNITS} units, ${BACKLOG_BYTES} bytes retained"
echo "                     (paced source left ${FEED_BYTES} bytes)"
echo "   worst visit:      ${BURST_SERVICE_MS} ms"
echo "   late ticks:       ${BURST_LATE}"

[ "${BACKLOG_UNITS:-0}" -ge "$BACKLOG_BOUND_UNITS" ] \
    || { echo "the burst left only ${BACKLOG_UNITS} units retained, under" \
              "the ${BACKLOG_BOUND_UNITS} the per-visit bound is supposed to" \
              "hold against" >&2
         exit 1; }

echo "   the backlog is ${BACKLOG_UNITS} units deep, so visits really did" \
     "have tens of 64-unit reads to do"

[ "${BURST_SERVICE_MS:-9999}" -lt "$TICK_SERVICE_BOUND_MS" ] \
    || { echo "a scheduler visit took ${BURST_SERVICE_MS}ms with a" \
              "${BACKLOG_BYTES} byte backlog, over the" \
              "${TICK_SERVICE_BOUND_MS}ms bound: fanout work per visit is" \
              "not bounded" >&2
         exit 1; }

[ "${BURST_LATE:-1}" -eq 0 ] \
    || { echo "$BURST_LATE ticks were late by more than half the interval" \
              "with a deep backlog" >&2
         exit 1; }

echo "   every visit stayed under ${TICK_SERVICE_BOUND_MS}ms and on schedule"

echo
echo "== stop"
kill_pid "$STORM"
STORM=0

for p in $PLAYERS; do
    kill_pid "$p"
done
PLAYERS=""

kill_pid "$PUB"
PUB=0

PID="$(cat "$RUN/logs/nginx.pid")"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit
trap - EXIT

for _ in $(seq 1 200); do
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

echo "== fanout_delay bench ok"

#!/usr/bin/env bash
#
# Ingest and egress under extreme fanout: where each side saturates, on whose
# thread, and what placing a publisher on a worker that does not own its
# program costs.
#
# Four things, measured rather than reasoned about:
#
#   floor    a worker with no media at all, so the CPU a worker burns before
#            any publisher arrives is a number, not a target: after the
#            idle-wait fix the sender pool sleeps with every slot unused; the
#            pre-fix run recorded the defect as roughly one core per worker.
#
#   ingest   libsrt runs the receive path itself, one thread per bound UDP
#            port, and this module's ingest thread only copies out of the
#            session buffer.  So the ceiling on one endpoint should be one
#            core and not one worker - which means publishers scale by
#            *endpoint* and not by worker count.  This ramps publishers onto
#            an endpoint and attributes CPU per thread, because "the ceiling
#            is one core" is a claim about `SRT:RcvQ`, not about a process.
#
#   egress   one program has one owner worker.  HLS is served from the shared
#            segment store, so the kernel spreads readers over every worker
#            (`listen ... reuseport`), while live destinations are prepared
#            and fed by the owner in process and cannot be spread at all.
#            Both are measured per worker count, so "does adding workers
#            help" is a number for each rather than a principle.
#
#   placed   a publisher that lands on a worker which does not own its
#   vs       program has its frames carried to the owner over the bounded
#   routed   SOCK_SEQPACKET transport.  The same load is run both ways - on
#            its owners' endpoint and on another worker's - and the difference
#            in CPU, throughput and owner queue is reported, so the escape
#            hatch has a price instead of a description.  Ownership is
#            deterministic (FNV-1a over application/stream, hash % workers)
#            and the graph reports the owner, so streams can be named to own
#            a chosen slot and placed on a chosen endpoint.
#
#   topology a four-worker owner-balanced matrix: all-local, all-routed,
#            alternating, and one-program local/routed controls.  Every row
#            includes input/program frames, feed pressure, preroll causes and
#            route failures from the owning worker.
#   capacity a fixed-worker offered-load curve: add programs, bitrate, and an
#            exact single-program output ladder over SRT and RTMP, followed by
#            a stalled-reader isolation case, saturated 4x250 SRT fanout, and
#            sustained multi-program fairness.  Reports receiver bytes,
#            sender-shard counters/CPU, per-worker resources, and fairness.
#
#
# Publishers stream a pre-encoded file with -c copy, so what is measured is
# the receiver's cost and not ffmpeg's encoder.  `media_srt_listen` is given
# once per worker, so worker i binds the i-th endpoint, and a publisher is
# placed by choosing the endpoint it connects to.
#
# Thread identification: the fixed SRT egress shards name themselves
# `srt-egress-00` through `srt-egress-15`; libsrt names its threads
# `SRT:RcvQ:wN`, `SRT:SndQ:wN`, `SRT:TsbPd`, and `SRT:GC`.  The sampler reads
# the comm field from each TID's `stat` record in the same pass as CPU ticks.
# The worker's own threads are labelled once per instance from a stack
# backtrace, because a detached thread is otherwise indistinguishable from
# the worker's event loop; if that is unavailable the labels fall back to
# comm and the conditions say so.
#
# CPU is per thread from /proc/<pid>/task/<tid>/stat.  One sampler takes both
# snapshots; CPU percentages use the wall time between their sample midpoints.
#
# Conditions are printed with the numbers.  One host, no netem, nothing else
# running.
#
#   make bench-ingest-egress-fanout
#   PHASES=misplace make bench-ingest-egress-fanout     one phase
#   PHASES=topology make bench-ingest-egress-fanout    owner matrix
#   HI_BITRATE=45M PHASES=ingest ...                    more aggressive
#   make bench-capacity-curve
#   PHASES=capacity-saturated ...                  only 4x250 SRT fanout
#   PHASES=capacity-slow-reader ...               only SRT slow-reader case
#   PHASES=capacity-sustained ...                  only sustained case

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/ingest-egress-fanout"

# Each run owns a 256-wide port block: HTTP +0, RTMP sink +1, SRT endpoints
# +2..+17, SRT receiver +20, HLS push receiver +30, and RTMP sink API +40.
# CAPACITY_BASE may select another block when the PID-derived block is occupied.
BASE="${CAPACITY_BASE:-$(( 30000 + ($$ % 80) * 256 ))}"
HTTP_PORT="$BASE"
RTMP_PORT="$(( BASE + 1 ))"
RTMP_SINK_RUN="$RUN/rtmp-sink"
RTMP_SINK_HTTP_PORT="$(( BASE + 40 ))"
RTMP_SINK_WORKER_PID=""

PHASES="${PHASES:-floor ingest egress misplace}"
WINDOW="${WINDOW:-6}"
FLOOR_WINDOW="${FLOOR_WINDOW:-4}"

# ingest ceiling: sixteen publishers is the per-worker session ceiling, so the
# offered packet rate is what has to be raised to reach one core on one port.
# -muxrate pads each publisher's transport stream to the chosen rate, so the
# receive path sees a packet rate that is a parameter and not whatever the
# content happened to encode to.
HI_PUBS="${HI_PUBS:-16}"
HI_RATES="${HI_RATES:-20M 40M 80M 120M}"

# ingest, publisher growth on one endpoint
PUB_STEPS="${PUB_STEPS:-4 8 12 16}"

# ingest, endpoint scaling: the same per-publisher rate at every worker count,
# so the only thing that changes is how many endpoints carry the load and how
# many receive threads exist
LO_BITRATE="${LO_BITRATE:-6M}"
LO_PER_PORT="${LO_PER_PORT:-12}"
LO_WORKERS="${LO_WORKERS:-1 2 4}"

# egress
EGRESS_WORKERS="${EGRESS_WORKERS:-1 2 4}"
HLS_READERS="${HLS_READERS:-48}"
HLS_SECONDS="${HLS_SECONDS:-12}"
SRT_DESTS="${SRT_DESTS:-8}"
LIVE_SECONDS="${LIVE_SECONDS:-12}"

# placement
MP_WORKERS="${MP_WORKERS:-2}"
MP_PUBS="${MP_PUBS:-12}"
MP_BITRATE="${MP_BITRATE:-20M}"
# capacity curve (fixed worker count; each axis varies offered work)
CAPACITY_WORKERS="${CAPACITY_WORKERS:-4}"
CAPACITY_WINDOW="${CAPACITY_WINDOW:-8}"
CAPACITY_PROGRAM_STEPS="${CAPACITY_PROGRAM_STEPS:-1 2 4 8}"
CAPACITY_PROGRAM_RATE="${CAPACITY_PROGRAM_RATE:-6M}"
CAPACITY_PROGRAM_DESTS="${CAPACITY_PROGRAM_DESTS:-1}"
CAPACITY_BITRATE_STEPS="${CAPACITY_BITRATE_STEPS:-2M 6M 12M 20M}"
CAPACITY_BITRATE_PROGRAMS="${CAPACITY_BITRATE_PROGRAMS:-4}"
CAPACITY_BITRATE_DESTS="${CAPACITY_BITRATE_DESTS:-1}"
CAPACITY_DEST_STEPS="${CAPACITY_DEST_STEPS:-1 8 32 64 128 256 512 1000}"
CAPACITY_DEST_RATE="${CAPACITY_DEST_RATE:-6M}"
CAPACITY_SLOW_RATE="${CAPACITY_SLOW_RATE:-20M}"
CAPACITY_SLOW_SECONDS="${CAPACITY_SLOW_SECONDS:-30}"
CAPACITY_SUSTAINED_SECONDS="${CAPACITY_SUSTAINED_SECONDS:-60}"
CAPACITY_SUSTAINED_PROGRAMS="${CAPACITY_SUSTAINED_PROGRAMS:-4}"
CAPACITY_SUSTAINED_RATE="${CAPACITY_SUSTAINED_RATE:-6M}"
CAPACITY_SUSTAINED_DESTS="${CAPACITY_SUSTAINED_DESTS:-4}"
CAPACITY_SATURATED_SECONDS="${CAPACITY_SATURATED_SECONDS:-15}"
CAPACITY_QUALITY_RATE="${CAPACITY_QUALITY_RATE:-8M}"
CAPACITY_QUALITY_SECONDS="${CAPACITY_QUALITY_SECONDS:-120}"
CAPACITY_QUALITY_STEPS="${CAPACITY_QUALITY_STEPS:-1 2 4 8 16 32 64 128 256 512 1000}"
CAPACITY_FIXED_SRT_WORKERS="${CAPACITY_FIXED_SRT_WORKERS:-adaptive}"
CAPACITY_FIXED_HLS_PUSH_WORKERS="${CAPACITY_FIXED_HLS_PUSH_WORKERS:-adaptive}"
CAPACITY_QUALITY_REFERENCE_RTMP_BPS="${CAPACITY_QUALITY_REFERENCE_RTMP_BPS:-0}"
CAPACITY_QUALITY_REFERENCE_HLS_BPS="${CAPACITY_QUALITY_REFERENCE_HLS_BPS:-0}"
CAPACITY_QUALITY_REFERENCE_HLS_PUSH_BPS="${CAPACITY_QUALITY_REFERENCE_HLS_PUSH_BPS:-0}"
CAPACITY_QUALITY_REFERENCE_BPS="${CAPACITY_QUALITY_REFERENCE_BPS:-0}"
CAPACITY_QUALITY_MIXES="${CAPACITY_QUALITY_MIXES:-all}"
CAPACITY_QUALITY_MIN_DELIVERY_RATIO="${CAPACITY_QUALITY_MIN_DELIVERY_RATIO:-0.95}"
CAPACITY_QUALITY_INTERVAL_FLOOR="${CAPACITY_QUALITY_INTERVAL_FLOOR:-0.80}"
CAPACITY_QUALITY_MAX_LOW_SECONDS="${CAPACITY_QUALITY_MAX_LOW_SECONDS:-2}"
CAPACITY_QUALITY_QUEUE_PRESSURE="${CAPACITY_QUALITY_QUEUE_PRESSURE:-0.90}"
CAPACITY_QUALITY_PRESSURE_SAMPLES="${CAPACITY_QUALITY_PRESSURE_SAMPLES:-3}"
CAPACITY_QUALITY_BLOCKED_SAMPLES="${CAPACITY_QUALITY_BLOCKED_SAMPLES:-3}"
# -muxrate makes the offered rate a parameter, so the placed and routed runs
# carry the same media and the CPU comparison is per unit carried
MP_RATE="${MP_RATE:-20M}"

# live egress: the program's own rate.  The source file is encoded at this
# target, because the destinations carry the program and not the padding: a
# transport stream padded with null packets costs the receive path and is
# dropped by the demuxer, so it does not reach an output.
LIVE_BITRATE="${LIVE_BITRATE:-20M}"
LIVE_RATE="${LIVE_RATE:-}"

PUBS=()
SINKS=()
QUALITY_MONITORS=()
STORM=0

SRT_PORTS=()
declare -A ENDPOINT_INDEX=()
declare -A SLOT=()
declare -A ROLE=()
declare -A TID_PID=()
LABELLED=1
W_BEFORE=""
W_AFTER=""
W_DUR=0
W_CPU_BEFORE_MS=0
W_CPU_AFTER_MS=0

cleanup() {
    local p
    for p in ${QUALITY_MONITORS[@]+"${QUALITY_MONITORS[@]}"}; do
        kill -TERM "$p" 2>/dev/null
    done
    for p in ${QUALITY_MONITORS[@]+"${QUALITY_MONITORS[@]}"}; do
        wait "$p" 2>/dev/null || true
    done
    QUALITY_MONITORS=()


    for p in ${PUBS[@]+"${PUBS[@]}"} ${SINKS[@]+"${SINKS[@]}"}; do
        kill -KILL "$p" 2>/dev/null
    done

    [ "$STORM" != "0" ] && kill -KILL "$STORM" 2>/dev/null

    stop_instance
    stop_rtmp_sink_instance

    return 0
}
trap cleanup EXIT

[ -x "$NGINX" ] || { echo "nginx is not built; run: make nginx" >&2; exit 1; }

# --- process bookkeeping ----------------------------------------------------
#
# nginx overwrites the worker's argv with its process title ("nginx: worker
# process"), so `pgrep -f <prefix>` matches nothing - an earlier version of
# this bench reported zero CPU for exactly that reason.  The workers are the
# children of the master the pid file names, which survives the retitling.

master_pid() {
    cat "$RUN/logs/nginx.pid" 2>/dev/null || true
}

worker_pids() {
    local m

    m="$(master_pid)"
    [ -n "$m" ] || return 0

    pgrep -P "$m" 2>/dev/null || true
}

pid_of() {   # <log> <ere> → the pid on the last matching line
    grep -E "$2" "$1" 2>/dev/null \
        | sed -n 's/.*\[[a-z]*\] \([0-9][0-9]*\)#.*/\1/p' | tail -1
}

# worker i binds the i-th endpoint and the log line naming the endpoint
# carries the pid that bound it, so a worker's slot is a lookup
map_slots() {
    local p i

    SLOT=()

    for p in $(worker_pids); do
        for i in "${SRT_PORTS[@]}"; do
            [ "$(pid_of "$RUN/logs/error.log" \
                  "srt listener ready on 127.0.0.1:$i\$")" = "$p" ] \
                && SLOT["$p"]="${ENDPOINT_INDEX[$i]}"
        done
    done

    return 0
}

slot_name() {   # <pid>
    if [ -n "${SLOT[$1]:-}" ]; then
        printf 'w%s' "${SLOT[$1]}"
    else
        printf 'pid%s' "$1"
    fi
}

stop_instance() {
    local m p

    m="$(master_pid)"
    [ -n "$m" ] || return 0

    # workers first: killing the master outright orphans them, and an orphaned
    # worker still holds its SRT port and its routing sockets
    for p in $(pgrep -P "$m" 2>/dev/null); do
        kill -KILL "$p" 2>/dev/null
    done

    kill -QUIT "$m" 2>/dev/null
    sleep 1
    kill -KILL "$m" 2>/dev/null

    rm -f "$RUN/logs/nginx.pid"

    return 0
}

stop_rtmp_sink_instance() {
    local m p

    m="$(cat "$RTMP_SINK_RUN/logs/nginx.pid" 2>/dev/null || true)"
    [ -n "$m" ] || {
        RTMP_SINK_WORKER_PID=""
        return 0
    }

    for p in $(pgrep -P "$m" 2>/dev/null); do
        kill -KILL "$p" 2>/dev/null
    done

    kill -QUIT "$m" 2>/dev/null
    sleep 1
    kill -KILL "$m" 2>/dev/null
    rm -f "$RTMP_SINK_RUN/logs/nginx.pid"
    unset "ROLE[$RTMP_SINK_WORKER_PID]" "SLOT[$RTMP_SINK_WORKER_PID]"
    RTMP_SINK_WORKER_PID=""
}

start_rtmp_sink_instance() {
    local i m

    stop_rtmp_sink_instance
    rm -rf "$RTMP_SINK_RUN/logs"
    mkdir -p "$RTMP_SINK_RUN/conf" "$RTMP_SINK_RUN/logs"

    cat > "$RTMP_SINK_RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 8192; }

media_rtmp_listen 127.0.0.1:$RTMP_PORT;

http {
    server {
        listen 127.0.0.1:$RTMP_SINK_HTTP_PORT;
        location /media/api/ { media_api; }
    }
}
EOF

    "$NGINX" -p "$RTMP_SINK_RUN" -c conf/nginx.conf -t \
        >"$RTMP_SINK_RUN/conf.log" 2>&1 \
        || {
            cat "$RTMP_SINK_RUN/conf.log" >&2
            echo "RTMP sink configuration rejected" >&2
            return 1
        }

    "$NGINX" -p "$RTMP_SINK_RUN" -c conf/nginx.conf \
        || { echo "could not start RTMP sink" >&2; return 1; }

    for i in $(seq 1 200); do
        if curl -fsS "$(rtmp_sink_api)/metrics" >/dev/null 2>&1; then
            break
        fi
        sleep 0.05
    done

    if ! curl -fsS "$(rtmp_sink_api)/metrics" >/dev/null 2>&1; then
        echo "RTMP sink did not become ready" >&2
        stop_rtmp_sink_instance
        return 1
    fi

    m="$(cat "$RTMP_SINK_RUN/logs/nginx.pid" 2>/dev/null || true)"
    RTMP_SINK_WORKER_PID="$(pgrep -P "$m" 2>/dev/null | sed -n '1p')"
    if [ -z "$RTMP_SINK_WORKER_PID" ]; then
        echo "RTMP sink worker did not start" >&2
        stop_rtmp_sink_instance
        return 1
    fi

    ROLE["$RTMP_SINK_WORKER_PID"]="worker"
    SLOT["$RTMP_SINK_WORKER_PID"]="rtmp-sink"
}

# --- thread labelling -------------------------------------------------------

label_worker() {   # <pid> → 0 when a backtrace with symbols was read
    local pid="$1" out tid role

    out="$(sudo -n gdb -p "$pid" -batch -ex 'thread apply all bt 14' 2>/dev/null)"
    [ -n "$out" ] || return 1

    out="$(printf '%s\n' "$out" | awk '
        /^Thread [0-9]+ \(Thread/ {
            if (tid != "") { print tid, (role == "" ? "unlabelled" : role) }
            match($0, /LWP [0-9]+/)
            tid = substr($0, RSTART + 4, RLENGTH - 4)
            role = ""
            next
        }
        /ngx_media_srt_thread/       { role = "ingest" }
        /ngx_media_srt_out_thread/   { if (role == "") role = "srt-send" }
        /ngx_media_hls_push_thread/  { if (role == "") role = "hls-push" }
        /ngx_worker_process_cycle/   { if (role == "") role = "worker" }
        /CRcvQueue::worker/          { if (role == "") role = "SRT:RcvQ" }
        /CSndQueue::worker/          { if (role == "") role = "SRT:SndQ" }
        /CUDT::tsbpd/                { if (role == "") role = "SRT:TsbPd" }
        /garbageCollect/             { if (role == "") role = "SRT:GC" }
        END { if (tid != "") print tid, (role == "" ? "unlabelled" : role) }
    ')"

    [ -n "$out" ] || return 1

    while read -r tid role; do
        ROLE["$tid"]="$role"
    done <<< "$out"

    return 0
}

label_all() {
    local p

    LABELLED=1
    ROLE=()

    for p in $(worker_pids); do
        label_worker "$p" || LABELLED=0
    done
}

write_cpu_roles() {
    local tid

    : > "$RUN/roles" || return 1
    for tid in "${!ROLE[@]}"; do
        printf '%s\t%s\n' "$tid" "${ROLE[$tid]}" >> "$RUN/roles" \
            || return 1
    done
}

# One Python process takes both snapshots and enumerates every sampled task
# directory once per sample. Optional extra PIDs expose receiver-side CPU.
cpu_window() {   # <seconds> [extra PID ...]
    local result extra
    local -a pids=()

    W_BEFORE="$RUN/before"
    W_AFTER="$RUN/after"
    write_cpu_roles || return 1
    mapfile -t pids < <(worker_pids)
    if [ "$#" -gt 1 ]; then
        for extra in "${@:2}"; do
            [ -n "$extra" ] && pids+=( "$extra" )
        done
    fi
    [ "${#pids[@]}" -gt 0 ] \
        || { echo "no processes available for CPU sampling" >&2; return 1; }

    result="$(python3 "$ROOT/tests/bench/proc_cpu_sampler.py" \
        --sleep "$1" --roles "$RUN/roles" \
        --before "$W_BEFORE" --after "$W_AFTER" "${pids[@]}")" \
        || return 1
    read -r W_DUR W_CPU_BEFORE_MS W_CPU_AFTER_MS <<< "$result"
    [ -n "$W_DUR" ] && [ -n "$W_CPU_BEFORE_MS" ] \
        && [ -n "$W_CPU_AFTER_MS" ]
}

# per (worker, role) percent of one core between two samples
report_cpu() {   # <before> <after> <seconds>
    local pid role pct

    awk -v w="$3" '
        NR == FNR { a[$1" "$2] = $4; next }
        { cpu[$1" "$3] += $4 - a[$1" "$2] }
        END { for (k in cpu) print k, cpu[k] * 100 / (w * 100) }
    ' "$1" "$2" | sort -k2,2 -k1,1 | while read -r pid role pct; do
        printf '%s %s %s\n' "$(slot_name "$pid")" "$role" "$pct"
    done
}

# the roles worth a line of their own
detail_cpu() {   # <before> <after> <seconds>
    report_cpu "$1" "$2" "$3" | awk '
        $2 == "SRT:RcvQ" || $2 == "SRT:SndQ" || $2 == "SRT:TsbPd" \
        || $2 == "ingest" || $2 == "worker" || $2 == "srt-send" \
        || $2 == "SRT:GC" || $2 == "hls-push" || $2 ~ /^srt-egress-/ \
        { print; next }
        { other[$1] += $3 }
        END { for (w in other) printf "%s other %d\n", w, other[w] }
    ' | sort
}

cpu_total() {   # <before> <after> <seconds>
    report_cpu "$1" "$2" "$3" \
        | awk '{ c[$1] += $3 } END { for (w in c) printf "%s=%d%% ", w, c[w] }'
}

# --- API helpers ------------------------------------------------------------
#
# The control API answers from the worker that accepts the request.  With more
# than one worker a request can land on a replica, where a mutation is refused
# with not_owner and where the program's fanout histogram is empty - the feed
# is the owner's.  Two URLs in one curl invocation share the connection, so
# they land on the same worker: reading the stream document first says whether
# the worker that will answer the metrics is the owner.

api() { printf 'http://127.0.0.1:%s/media/api/v1' "$HTTP_PORT"; }

rtmp_sink_api() {
    printf 'http://127.0.0.1:%s/media/api/v1' "$RTMP_SINK_HTTP_PORT"
}

stream_field() {   # <name> <field>
    curl -fsS "$(api)/streams/live/$1" 2>/dev/null \
        | sed -n "s/.*\"$2\":\([0-9]*\).*/\1/p" | head -1
}

# A read can land on a worker that has no copy of a stream a publisher
# created, and answers 404.  Retrying is how the owner is reached; the value
# itself is the owner's either way, because program progress is published in
# the shared directory.
field_any() {   # <name> <field> [tries]
    local i v

    for i in $(seq 1 "${3:-12}"); do
        v="$(stream_field "$1" "$2")"
        [ -n "$v" ] && { printf '%s' "$v"; return 0; }
        sleep 0.2
    done

    return 1
}

owner_of() {   # <name> → the owner the graph reports
    local i v

    for i in $(seq 1 40); do
        v="$(stream_field "$1" owner)"
        [ -n "$v" ] && { printf '%s' "$v"; return 0; }
        sleep 0.25
    done

    return 1
}

owner_metrics() {   # <name> → the owner's own metrics, on one connection
    local i out

    for i in $(seq 1 40); do
        out="$(curl -fsS "$(api)/streams/live/$1" "$(api)/metrics" 2>/dev/null)" \
            || continue

        case "$out" in
            *'"observed_here":true'*) printf '%s' "$out"; return 0 ;;
        esac
    done

    return 1
}

owner_metric() {   # <name> <metric>
    owner_metrics "$1" 2>/dev/null \
        | grep "^$2{application=\"live\",name=\"$1\"" | awk '{print $2}' | head -1
}
blob_label_metric() {   # <metrics> <metric> <stream>
    printf '%s\n' "$1" \
        | grep "^$2{application=\"live\",name=\"$3\"" \
        | awk '{print $2}' | head -1 || true
}

blob_source_metric() {   # <metrics> <metric> <stream>
    printf '%s\n' "$1" \
        | grep "^$2{application=\"live\",name=\"$3\",source=\"enc-$3\"" \
        | awk '{print $2}' | head -1 || true
}

blob_global_metric() {   # <metrics> <metric>
    printf '%s\n' "$1" \
        | sed -n "/^$2 /{s/^$2 //;p;q;}" || true
}

endpoint_worker() {   # <port> → the worker that logged the listener
    local pid

    pid="$(pid_of "$RUN/logs/error.log" \
        "srt listener ready on 127.0.0.1:$1\$")"

    if [ -n "$pid" ]; then
        slot_name "$pid"
    else
        printf 'unknown'
    fi
}


owner_percentile() {   # <name> <percentile>
    owner_metrics "$1" 2>/dev/null \
        | grep "fanout_delay_ms{application=\"live\",name=\"$1\"" \
        | grep "percentile=\"$2\"" | awk '{print $2}' | head -1
}

post_owner() {   # <path> <json> ; retried until the owner answers it
    local i code

    for i in $(seq 1 60); do
        code="$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
            -H 'Content-Type: application/json' -d "$2" "$(api)$1" 2>/dev/null)"

        case "$code" in
            2*) return 0 ;;
            # 409 not_owner and 404 both mean "this worker cannot answer for
            # that stream": with more than one worker the request lands on a
            # replica, so the next attempt is the point, not an error
            409|404) sleep 0.2 ;;
            *)  echo "   POST $1 answered $code" >&2; return 1 ;;
        esac
    done

    echo "   POST $1 never reached the owner" >&2
    return 1
}

# --- the owner hash ---------------------------------------------------------
#
# FNV-1a over application/stream, the arithmetic in src/core/ngx_media_owner.c,
# so streams whose owner is a chosen slot can be named on purpose.

owner_names() {   # <slot> <workers> <count> <prefix> → names, one per line
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import sys

want, workers, count, prefix = (int(sys.argv[1]), int(sys.argv[2]),
                                int(sys.argv[3]), sys.argv[4])
found = 0

for i in range(1, 20000):
    name = "%s%d" % (prefix, i)
    h = 14695981039346656037
    for b in b"live/" + name.encode():
        h ^= b
        h = (h * 1099511628211) & ((1 << 64) - 1)
    if h % workers == want:
        print(name)
        found += 1
        if found == count:
            break
PY
}

# --- the instance -----------------------------------------------------------

make_source() {   # <path> <bitrate>
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=1280x720:rate=25" \
        -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p \
        -b:v "$2" -maxrate "$2" -minrate "$2" -bufsize 60M \
        -t 30 -f mpegts "$1" 2>/dev/null

    [ -s "$1" ] || { echo "could not build the $2 source" >&2; exit 1; }

    printf '   source %s: %s KiB over 30s (%.1f Mbit/s)\n' \
        "$2" "$(( $(stat -c %s "$1") / 1024 ))" \
        "$(awk -v b="$(stat -c %s "$1")" 'BEGIN { printf "%.1f", b * 8 / 30 / 1000000 }')"
}

write_config() {   # <workers> <hls yes|no> <rtmp yes|no>
    local w="$1" i

    SRT_PORTS=()
    ENDPOINT_INDEX=()

    for i in $(seq 0 $(( w - 1 ))); do
        SRT_PORTS+=( "$(( BASE + 2 + i ))" )
        ENDPOINT_INDEX[$(( BASE + 2 + i ))]="$i"
    done

    {
        echo "worker_processes $w;"
        echo "daemon on;"
        echo "error_log logs/error.log info;"
        echo "pid logs/nginx.pid;"
        [ "$CAPACITY_FIXED_SRT_WORKERS" = adaptive ] \
            || echo "media_egress_workers srt $CAPACITY_FIXED_SRT_WORKERS;"
        [ "$CAPACITY_FIXED_HLS_PUSH_WORKERS" = adaptive ] \
            || echo "media_egress_workers hls_push $CAPACITY_FIXED_HLS_PUSH_WORKERS;"
        echo
        echo "events { worker_connections 8192; }"
        echo

        [ "$2" = yes ] && echo "media_hls $RUN/hls;"

        for i in "${SRT_PORTS[@]}"; do
            echo "media_srt_listen 127.0.0.1:$i;"
        done

        [ "$3" = yes ] && echo "media_rtmp_listen 127.0.0.1:$RTMP_PORT;"

        echo
        echo "http {"

        if [ "$2" = yes ]; then
            echo "    log_format media_pid \"\$pid \$bytes_sent\";"
            echo "    access_log logs/access.log media_pid;"
        else
            echo "    access_log off;"
        fi

        echo
        echo "    server {"
        echo "        listen 127.0.0.1:$HTTP_PORT reuseport;"
        echo
        echo "        location /media/api/ { media_api; }"
        echo "        location /hls/       { alias $RUN/hls/; }"
        echo "    }"
        echo "}"
    } > "$RUN/conf/nginx.conf"
}

start_instance() {   # <workers> <hls> <rtmp>
    local i want

    stop_instance
    rm -rf "$RUN/hls" "$RUN/logs"
    mkdir -p "$RUN/logs" "$RUN/hls"

    write_config "$1" "$2" "$3"

    "$NGINX" -p "$RUN" -c conf/nginx.conf -t >"$RUN/conf.log" 2>&1 \
        || { cat "$RUN/conf.log" >&2; echo "configuration rejected" >&2; exit 1; }

    "$NGINX" -p "$RUN" -c conf/nginx.conf

    for i in $(seq 1 400); do
        want="$(grep -c 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null || true)"
        [ "${want:-0}" -ge "$1" ] && break
        sleep 0.05
    done

    want="$(grep -c 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null || true)"
    [ "${want:-0}" -ge "$1" ] \
        || { echo "not every endpoint was bound ($want of $1)" >&2
             tail -5 "$RUN/logs/error.log" >&2; exit 1; }

    if [ "$3" = yes ]; then
        for i in $(seq 1 200); do
            grep -q 'rtmp listener ready' "$RUN/logs/error.log" 2>/dev/null && break
            sleep 0.05
        done
    fi

    map_slots
    label_all

    # the placement every number below depends on: which worker bound which
    # endpoint, read back from the log
    PLACEMENT=""
    for i in $(seq 0 $(( $1 - 1 ))); do
        PLACEMENT="$PLACEMENT ${SRT_PORTS[$i]}->$(slot_name "$(pid_of "$RUN/logs/error.log" "srt listener ready on 127.0.0.1:${SRT_PORTS[$i]}\$")")"
    done

    return 0
}

publish() {   # <endpoint port> <name> <source> [muxrate]
    local rate=()

    # -muxrate pads the transport stream to a chosen rate, so the packet rate
    # a publisher offers is a parameter rather than whatever the content
    # happens to encode to
    [ -n "${4:-}" ] && rate=( -muxrate "$4" )

    ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$3" -c copy \
        "${rate[@]}" -f mpegts \
        "srt://127.0.0.1:$1?mode=caller&streamid=#!::r=live/$2,m=publish,s=enc-$2" \
        >/dev/null 2>&1 &
}

kill_pubs() {
    local p

    for p in ${PUBS[@]+"${PUBS[@]}"}; do
        kill -KILL "$p" 2>/dev/null
    done

    # reaped, so the shell does not print a "Killed" line per publisher
    for p in ${PUBS[@]+"${PUBS[@]}"}; do
        wait "$p" 2>/dev/null
    done

    PUBS=()
    return 0
}

kill_one() {   # <pid> ; the same, for a single publisher or receiver
    [ -n "${1:-}" ] || return 0
    kill -KILL "$1" 2>/dev/null
    wait "$1" 2>/dev/null
    return 0
}

# --- ingest throughput, per worker ------------------------------------------
#
# Every worker's own main thread logs what its ingest thread drained, so the
# line is per-worker evidence of how much arrived and how much of it the raw
# queue dropped.  The counters are cumulative, so the window is a delta.

drained_last() {   # <pid> <field>
    grep "^.*\[[a-z]*\] $1#" "$RUN/logs/error.log" 2>/dev/null \
        | grep 'srt ingest drained' | tail -1 \
        | sed -n "s/.* $2=\([0-9]*\).*/\1/p"
}

ingest_totals() {   # sums the last drained line of every worker
    local p b c d s any=0

    BYTES=0; CHUNKS=0; DROPPED=0; SESSIONS=0

    for p in $(worker_pids); do
        b="$(drained_last "$p" bytes)"
        [ -n "$b" ] || continue

        any=1
        c="$(drained_last "$p" chunks)"
        d="$(drained_last "$p" dropped_chunks)"
        s="$(drained_last "$p" sessions)"

        BYTES=$(( BYTES + b )); CHUNKS=$(( CHUNKS + c ))
        DROPPED=$(( DROPPED + d )); SESSIONS=$(( SESSIONS + s ))
    done

    [ "$any" = 1 ]
}

row() {   # <workers> <publishers> <delivered> <chunks/s> <MiB/s>
    printf '%-8s %-6s %-11s %-11s %-9s %s\n' "$1" "$2" "$3" "$4" "$5" \
        "$(cpu_total "$W_BEFORE" "$W_AFTER" "$W_DUR")"
}

detail_rows() {   # <publishers>
    detail_cpu "$W_BEFORE" "$W_AFTER" "$W_DUR" \
        | awk -v p="$1" '{ printf "   %-4s %-10s %-6s %s%%\n", p, $2, $1, $3 }'
}

mibps() {   # <bytes> ; bytes/s over the window, in MiB
    awk -v b="$1" -v w="$W_DUR" 'BEGIN { printf "%.1f", b / 1048576 / w }'
}

# --- phases -----------------------------------------------------------------

phase_floor() {
    local w

    echo
    echo "== floor: what a worker burns with no publisher and no destination"
    echo
    printf '%-8s %-10s %s\n' workers 'idle CPU' 'per-thread'

    for w in $LO_WORKERS; do

        start_instance "$w" no no
        cpu_window "$FLOOR_WINDOW"

        printf '%-8s %-10s %s\n' "$w" \
            "$(cpu_total "$W_BEFORE" "$W_AFTER" "$W_DUR")" \
            "$(report_cpu "$W_BEFORE" "$W_AFTER" "$W_DUR" \
               | awk '$2 == "srt-send" { printf "%s=%d%% ", $1, $3 }')"
    done

    echo "   (no media has been published at this point in the run)"
    return 0
}

phase_ingest_high() {
    local rate i delivered b0 c0 d0 db dc dd late noroom

    echo
    echo "== ingest: $HI_PUBS publishers on one endpoint, one worker, with the"
    echo "   offered rate raised ($HI_RATES per publisher, -muxrate)"
    echo "   $HI_PUBS is the per-worker session ceiling, so the only thing that"
    echo "   rises here is the packet rate one port has to carry"
    echo
    printf '%-8s %-10s %-11s %-9s %-8s %s\n' rate chunks/s MiB/s dropped \
        seqno-late 'CPU per worker (total)'

    make_source "$RUN/media/hi.ts" "$LO_BITRATE"

    for rate in $HI_RATES; do

        start_instance 1 no no

        PUBS=()
        for i in $(seq 1 "$HI_PUBS"); do
            publish "${SRT_PORTS[0]}" "hi$i" "$RUN/media/hi.ts" "$rate"
            PUBS+=( $! )
        done

        sleep "$WINDOW"

        delivered=0
        for i in $(seq 1 "$HI_PUBS"); do
            [ -n "$(field_any "hi$i" program_frames 6)" ] \
                && delivered=$(( delivered + 1 ))
        done

        # the drained counters are sampled around the window only: reading
        # them across the delivered loop above would divide a longer interval
        # by the window and report a rate nobody carried
        ingest_totals; b0=$BYTES; c0=$CHUNKS; d0=$DROPPED
        cpu_window "$WINDOW"
        ingest_totals || true

        db=$(( BYTES - b0 )); dc=$(( CHUNKS - c0 )); dd=$(( DROPPED - d0 ))

        # the library's own two drop sites, which are not module counters
        late="$(grep -c 'RCV-DROPPED' "$RUN/logs/error.log" 2>/dev/null)"
        noroom="$(grep -c 'No room to store incoming packet' \
                  "$RUN/logs/error.log" 2>/dev/null)"
        late="${late:-0}"
        noroom="${noroom:-0}"

        printf '%-8s %-10s %-11s %-9s %-8s %s\n' "$rate" \
            "$(awk -v c="$dc" -v w="$W_DUR" 'BEGIN { printf "%d", c / w }')" \
            "$(mibps "$db")" "$dd" "$(( late + noroom ))" \
            "$(cpu_total "$W_BEFORE" "$W_AFTER" "$W_DUR")"

        printf '   %-4s sessions %s  fanout-delay drops %s  receive-buffer drops %s\n' \
            "$rate" "$delivered/$HI_PUBS" "$late" "$noroom"

        detail_rows "$rate"

        kill_pubs
        sleep 1
    done

    return 0
}

phase_ingest_publishers() {
    local k i delivered b0 c0 d0 db dc dd

    echo
    echo "== ingest: publishers grown on one endpoint at ~$LO_BITRATE each"
    echo
    printf '%-8s %-6s %-11s %-11s %-9s %s\n' workers publ. delivered \
        chunks/s MiB/s 'CPU per worker (total)'

    for k in $PUB_STEPS; do

        start_instance 1 no no

        PUBS=()
        for i in $(seq 1 "$k"); do
            publish "${SRT_PORTS[0]}" "pu$i" "$RUN/media/lo.ts"
            PUBS+=( $! )
        done

        sleep "$WINDOW"

        delivered=0
        for i in $(seq 1 "$k"); do
            [ -n "$(field_any "pu$i" program_frames 6)" ] \
                && delivered=$(( delivered + 1 ))
        done

        ingest_totals; b0=$BYTES; c0=$CHUNKS; d0=$DROPPED
        cpu_window "$WINDOW"
        ingest_totals || true

        db=$(( BYTES - b0 )); dc=$(( CHUNKS - c0 )); dd=$(( DROPPED - d0 ))

        row 1 "$k" "$delivered/$k" \
            "$(awk -v c="$dc" -v w="$W_DUR" 'BEGIN { printf "%d", c / w }')" \
            "$(mibps "$db")"
        detail_rows "$k"

        [ "$dd" -gt 0 ] && echo "   dropped chunks in the window: $dd"

        kill_pubs
        sleep 1
    done

    return 0
}

phase_ingest_endpoints() {
    local w n i delivered b0 c0 db dc

    echo
    echo "== ingest: the same publishers per endpoint, across worker counts"
    echo "   ~$LO_BITRATE publishers, $LO_PER_PORT per endpoint, spread"
    echo "   round-robin over the endpoints the instance binds"
    echo
    printf '%-8s %-6s %-11s %-11s %-9s %s\n' workers publ. delivered \
        chunks/s MiB/s 'CPU per worker (total)'

    for w in $LO_WORKERS; do
        n=$(( w * LO_PER_PORT ))

        start_instance "$w" no no

        echo "   endpoints:$PLACEMENT"

        PUBS=()
        for i in $(seq 1 "$n"); do
            publish "${SRT_PORTS[$(( (i - 1) % w ))]}" "lo$i" "$RUN/media/lo.ts"
            PUBS+=( $! )
        done

        sleep "$WINDOW"

        delivered=0
        for i in $(seq 1 "$n"); do
            [ -n "$(field_any "lo$i" program_frames 6)" ] \
                && delivered=$(( delivered + 1 ))
        done

        ingest_totals; b0=$BYTES; c0=$CHUNKS
        cpu_window "$WINDOW"
        ingest_totals || true

        db=$(( BYTES - b0 )); dc=$(( CHUNKS - c0 ))

        row "$w" "$n" "$delivered/$n" \
            "$(awk -v c="$dc" -v w="$W_DUR" 'BEGIN { printf "%d", c / w }')" \
            "$(mibps "$db")"
        detail_rows "$n"

        kill_pubs
        sleep 1
    done

    return 0
}

phase_ingest_one_port() {
    local w n i delivered b0 c0 db dc

    echo
    echo "== ingest: $LO_PER_PORT publishers on one endpoint, from a single"
    echo "   worker and from an instance with four workers bound (three idle)"
    echo
    printf '%-8s %-6s %-11s %-11s %-9s %s\n' workers publ. delivered \
        chunks/s MiB/s 'CPU per worker (total)'

    for w in 1 4; do
        n="$LO_PER_PORT"

        start_instance "$w" no no

        PUBS=()
        for i in $(seq 1 "$n"); do
            publish "${SRT_PORTS[0]}" "op$i" "$RUN/media/lo.ts"
            PUBS+=( $! )
        done

        sleep "$WINDOW"

        delivered=0
        for i in $(seq 1 "$n"); do
            [ -n "$(field_any "op$i" program_frames 6)" ] \
                && delivered=$(( delivered + 1 ))
        done

        ingest_totals; b0=$BYTES; c0=$CHUNKS
        cpu_window "$WINDOW"
        ingest_totals || true

        db=$(( BYTES - b0 )); dc=$(( CHUNKS - c0 ))

        row "$w" "$n" "$delivered/$n" \
            "$(awk -v c="$dc" -v w="$W_DUR" 'BEGIN { printf "%d", c / w }')" \
            "$(mibps "$db")"
        detail_rows "$n"

        kill_pubs
        sleep 1
    done

    return 0
}

# --- egress -----------------------------------------------------------------

segments_closed() {   # the playlist's closed segments, 0 when there is none
    local n

    n="$(grep -c '^#EXTINF' "$RUN/hls/live/eg/index.m3u8" 2>/dev/null)"
    printf '%s' "${n:-0}"
}

hls_storm() {
    local list="$RUN/urls.txt" name
    while :; do
        : > "$list"

        for name in $(grep -v '^#' "$RUN/hls/live/eg/index.m3u8" 2>/dev/null \
                      | grep '\.ts$' || true); do
            printf 'url = "http://127.0.0.1:%s/hls/live/eg/%s"\noutput = "/dev/null"\n' \
                "$HTTP_PORT" "$name" >> "$list"
        done

        [ -s "$list" ] || { sleep 0.2; continue; }

        curl -fsS --parallel --parallel-max "$HLS_READERS" \
            --config "$list" >/dev/null 2>&1 || true

        sleep 0.02
    done
}

phase_egress_hls() {
    local w i served mib rps pub

    echo
    echo "== egress: $HLS_READERS concurrent HLS readers against one program"
    echo "   the segmenter runs on the owner; each reader is served by"
    echo "   whichever worker the kernel hashed it to"
    echo
    printf '%-8s %-12s %-11s %-9s %s\n' workers requests/s MiB/s \
        segments 'CPU per worker (total)'

    for w in $EGRESS_WORKERS; do

        start_instance "$w" yes no
        : > "$RUN/logs/access.log"

        publish "${SRT_PORTS[0]}" "eg" "$RUN/media/lo.ts"
        pub=$!

        for i in $(seq 1 400); do
            [ "$(segments_closed)" -ge 3 ] && break
            sleep 0.1
        done

        sleep 4

        hls_storm &
        STORM=$!

        cpu_window "$HLS_SECONDS"

        kill -KILL "$STORM" 2>/dev/null
        STORM=0
        sleep 0.5

        served="$(wc -l < "$RUN/logs/access.log" 2>/dev/null || echo 0)"
        mib="$(awk -v s="$W_DUR" \
               '{ b += $2 } END { printf "%.1f", b / 1048576 / s }' \
               "$RUN/logs/access.log" 2>/dev/null || echo 0)"
        rps="$(awk -v n="$served" -v s="$W_DUR" 'BEGIN { printf "%d", n / s }')"

        printf '%-8s %-12s %-11s %-9s %s\n' "$w" "$rps" "$mib" \
            "$(segments_closed)" \
            "$(cpu_total "$W_BEFORE" "$W_AFTER" "$W_DUR")"

        echo "   requests per worker (the access log's \$pid):"
        awk '{ c[$1]++ } END { for (p in c) print p, c[p] }' \
            "$RUN/logs/access.log" 2>/dev/null \
            | while read -r p n; do
                  printf '     %-6s %s requests\n' "$(slot_name "$p")" "$n"
              done

        detail_rows hls

        kill_one "$pub"
        sleep 1
    done

    return 0
}

phase_egress_live() {
    local w d port pub started owner dests f0 f1

    echo
    echo "== egress: $SRT_DESTS live SRT destinations on one program"
    echo "   each is pushed to an ffmpeg receiver of its own, prepared and fed"
    echo "   by the owner worker in process"
    echo
    printf '%-8s %-13s %-11s %s\n' workers started frames/s \
        'CPU per worker (total)'

    make_source "$RUN/media/lv.ts" "$LIVE_BITRATE"

    for w in $EGRESS_WORKERS; do

        start_instance "$w" no yes

        publish "${SRT_PORTS[0]}" "lv" "$RUN/media/lv.ts" "$LIVE_RATE"
        pub=$!

        for d in $(seq 1 200); do
            [ -n "$(field_any lv program_frames 8)" ] && break
            sleep 0.1
        done

        owner="$(owner_of lv)"

        # the receivers have to be listening before the destinations connect
        SINKS=()
        for d in $(seq 1 "$SRT_DESTS"); do
            port=$(( BASE + 20 + d ))
            ffmpeg -hide_banner -loglevel error \
                -i "srt://127.0.0.1:$port?mode=listener" -c copy -f null - \
                >"$RUN/sink-$d.log" 2>&1 &
            SINKS+=( $! )
        done

        sleep 2

        for d in $(seq 1 "$SRT_DESTS"); do
            post_owner "/streams/live/lv/destinations" \
                "{\"id\":\"sink$d\",\"type\":\"srt\",\"host\":\"127.0.0.1\",\"port\":$(( BASE + 20 + d )),\"streamid\":\"#!::r=live/lv,m=publish,s=sink$d\"}" \
                || true
        done

        sleep 5

        dests="$(grep -cE 'media: srt destination sink[0-9]+ started' \
                  "$RUN/logs/error.log" 2>/dev/null || true)"
        dests="${dests:-0}"
        f0="$(field_any lv program_frames 8)"; f0="${f0:-0}"

        cpu_window "$LIVE_SECONDS"

        f1="$(field_any lv program_frames 8)"; f1="${f1:-0}"

        printf '%-8s %-13s %-11s %s\n' "$w" "$dests/$SRT_DESTS" \
            "$(awk -v f="$(( f1 - f0 ))" -v s="$W_DUR" \
               'BEGIN { printf "%.1f", f / s }')" \
            "$(cpu_total "$W_BEFORE" "$W_AFTER" "$W_DUR")"

        echo "   owner reported: ${owner:-?}; destinations started: $dests"

        detail_rows live

        for d in ${SINKS[@]+"${SINKS[@]}"}; do kill_one "$d"; done
        SINKS=()
        kill_one "$pub"
        sleep 1
    done

    return 0
}

# --- placed versus routed ---------------------------------------------------

phase_misplace() {
    local w="$MP_WORKERS" i n="$MP_PUBS"
    local placed routed placed_owner routed_owner routed_log placed_routed
    local f0 f1 frames units backlog p50 p99 span t0 t1

    echo
    echo "== placement: $n publishers on their owner's endpoint, and the same"
    echo "   $n on another worker's endpoint, where every one is routed"
    echo

    make_source "$RUN/media/mp.ts" "$MP_BITRATE"

    mapfile -t PLACED < <(owner_names 0 "$w" "$n" p)
    mapfile -t ROUTED < <(owner_names 0 "$w" "$n" r)

    for i in "${!PLACED[@]}"; do
        [ -n "${PLACED[$i]}" ] && [ -n "${ROUTED[$i]}" ] \
            || { echo "could not name $n streams with owner 0" >&2; exit 1; }
    done

    for case in placed routed; do

        # HLS is on so the program has a consumer: the fanout delay histogram
        # only records a dispatch when something takes a unit out of the feed.
        # The segmenter runs on the owner in both cases, so it is common to
        # both and cancels in the comparison.
        start_instance "$w" yes no

        echo "   endpoints:$PLACEMENT"

        PUBS=()
        if [ "$case" = placed ]; then
            # accepted by the owner that drives the program: no transport
            for i in "${!PLACED[@]}"; do
                publish "${SRT_PORTS[0]}" "${PLACED[$i]}" "$RUN/media/mp.ts" \
                    "$MP_RATE"
                PUBS+=( $! )
            done
        else
            # accepted by worker 1 and routed to the owner on worker 0
            for i in "${!ROUTED[@]}"; do
                publish "${SRT_PORTS[1]}" "${ROUTED[$i]}" "$RUN/media/mp.ts" \
                    "$MP_RATE"
                PUBS+=( $! )
            done
        fi

        sleep 6

        # Everything is sampled between t0 and t1, and the rates are divided
        # by that interval rather than by the sleep: reading 12 programs'
        # counters takes long enough to matter, and it is the interval the
        # deltas actually cover.
        t0="$(date +%s.%N)"
        ingest_totals; local b0=$BYTES c0=$CHUNKS d0=$DROPPED

        f0=0
        for i in "${!PLACED[@]}"; do
            local v
            if [ "$case" = placed ]; then
                v="$(field_any "${PLACED[$i]}" program_frames 8)"
            else
                v="$(field_any "${ROUTED[$i]}" program_frames 8)"
            fi
            f0=$(( f0 + ${v:-0} ))
        done

        cpu_window "$WINDOW"

        f1=0
        for i in "${!PLACED[@]}"; do
            local v
            if [ "$case" = placed ]; then
                v="$(field_any "${PLACED[$i]}" program_frames 8)"
            else
                v="$(field_any "${ROUTED[$i]}" program_frames 8)"
            fi
            f1=$(( f1 + ${v:-0} ))
        done

        ingest_totals || true
        t1="$(date +%s.%N)"
        span="$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f", b - a }')"
        frames=$(( f1 - f0 ))

        # the owner's queue and its own dispatch delay, for the first program
        if [ "$case" = placed ]; then
            p50="$(owner_percentile "${PLACED[0]}" 50)"
            p99="$(owner_percentile "${PLACED[0]}" 99)"
        else
            p50="$(owner_percentile "${ROUTED[0]}" 50)"
            p99="$(owner_percentile "${ROUTED[0]}" 99)"
        fi

        backlog=0
        for i in "${!PLACED[@]}"; do
            local u
            if [ "$case" = placed ]; then
                u="$(owner_metric "${PLACED[$i]}" nginx_media_stream_feed_units)"
            else
                u="$(owner_metric "${ROUTED[$i]}" nginx_media_stream_feed_units)"
            fi
            backlog=$(( backlog + ${u:-0} ))
        done

        printf '   %-7s programs %s  frames/s %s  owner backlog %s units\n' \
            "$case" "$n" \
            "$(awk -v f="$frames" -v s="$span" 'BEGIN { printf "%.1f", f / s }')" \
            "$backlog"
        printf '   %-7s drained %s chunks/s  %s MiB/s  dropped %s  fanout p50 %sms p99 %sms\n' \
            "$case" \
            "$(awk -v c="$(( CHUNKS - c0 ))" -v s="$span" 'BEGIN { printf "%d", c / s }')" \
            "$(awk -v b="$(( BYTES - b0 ))" -v s="$span" 'BEGIN { printf "%.1f", b / 1048576 / s }')" \
            "$(( DROPPED - d0 ))" "${p50:-?}" "${p99:-?}"

        detail_rows "$case"

        if [ "$case" = placed ]; then
            placed_owner="$(owner_of "${PLACED[0]}")"
            placed_routed="$(grep -c 'srt publisher routed to the owner stream=live/p' \
                             "$RUN/logs/error.log" 2>/dev/null || true)"
        else
            routed_owner="$(owner_of "${ROUTED[0]}")"
            routed_log="$(grep -c 'srt publisher routed to the owner stream=live/r' \
                          "$RUN/logs/error.log" 2>/dev/null || true)"
        fi

        kill_pubs
        sleep 1
    done

    echo
    echo "   owner from the graph: placed=$placed_owner routed=$routed_owner"
    echo "   routed sessions logged: ${routed_log:-0} of $n in the routed case," \
         "${placed_routed:-0} of $n in the placed case"
    echo "   placed: endpoint ${SRT_PORTS[0]} is worker 0's, so every publisher"
    echo "           is accepted by the worker that owns its program"
    echo "   routed: endpoint ${SRT_PORTS[1]} is worker 1's, so every frame"
    echo "           crosses the SOCK_SEQPACKET transport to worker 0"

    return 0
}

# --- deterministic topology matrix ------------------------------------------
#
# This phase removes ownership luck from the worker-scaling question.  One
# stream is deliberately named for each owner slot, then the same balanced
# programs are run with all publishers local, all routed, alternating local
# and routed, and one-program local/routed controls.  Each row reports the
# source input, program/feed output, feed pressure, preroll causes and route
# failures from the program owner's own metrics response.

phase_topology() {
    local w="${TOPOLOGY_WORKERS:-4}"
    local window="${TOPOLOGY_WINDOW:-8}"
    local i case_idx name owner endpoint landing path metrics
    local input program feed_units feed_bytes feed_high_units feed_high_bytes
    local evictions overruns
    local preroll_overflows unit_overflows byte_overflows route_identity
    local route_slots route_no_slot route_no_payload route_reassembly
    local route_publish route_fail
    local -a names case_names case_endpoints found

    [ "$w" -ge 2 ] || {
        echo "topology needs at least two workers" >&2
        return 1
    }

    mapfile -t names < <(owner_names 0 "$w" 1 topology-w)
    for i in $(seq 1 $(( w - 1 ))); do
        mapfile -t found < <(owner_names "$i" "$w" 1 topology-w)
        names[$i]="${found[0]:-}"
    done

    for i in $(seq 0 $(( w - 1 ))); do
        [ -n "${names[$i]:-}" ] \
            || { echo "could not select a stream for owner $i" >&2; return 1; }
    done

    echo
    echo "== deterministic worker topology matrix"
    echo "   workers: $w, source: pre-encoded mpegts, window: ${window}s"
    echo "   names are selected by the 64-bit FNV-1a owner hash"
    for i in $(seq 0 $(( w - 1 ))); do
        echo "   owner w$i: ${names[$i]}"
    done

    for case_idx in all-local all-routed alternating one-local one-routed; do
        start_instance "$w" yes no
        case_names=()
        case_endpoints=()

        case "$case_idx" in
            all-local)
                for i in $(seq 0 $(( w - 1 ))); do
                    case_names+=("${names[$i]}")
                    case_endpoints+=("$i")
                done
                ;;
            all-routed)
                for i in $(seq 0 $(( w - 1 ))); do
                    case_names+=("${names[$i]}")
                    case_endpoints+=("$(( (i + 1) % w ))")
                done
                ;;
            alternating)
                for i in $(seq 0 $(( w - 1 ))); do
                    case_names+=("${names[$i]}")
                    if [ $(( i % 2 )) -eq 0 ]; then
                        case_endpoints+=("$i")
                    else
                        case_endpoints+=("$(( (i + 1) % w ))")
                    fi
                done
                ;;
            one-local)
                case_names=("${names[0]}")
                case_endpoints=(0)
                ;;
            one-routed)
                case_names=("${names[0]}")
                case_endpoints=(1)
                ;;
        esac

        echo
        echo "== topology case: $case_idx"
        PUBS=()
        for i in "${!case_names[@]}"; do
            publish "${SRT_PORTS[${case_endpoints[$i]}]}" \
                "${case_names[$i]}" "$RUN/media/lo.ts"
            PUBS+=( $! )
        done

        for name in "${case_names[@]}"; do
            field_any "$name" program_frames 80 >/dev/null 2>&1 || true
        done
        sleep "$window"

        printf '   %-16s %s\n' case "$case_idx"
        for i in "${!case_names[@]}"; do
            name="${case_names[$i]}"
            endpoint="${case_endpoints[$i]}"
            metrics="$(owner_metrics "$name" 2>/dev/null || true)"
            owner="$(owner_of "$name" 2>/dev/null || printf '?')"
            landing="$(endpoint_worker "${SRT_PORTS[$endpoint]}")"

            if [ "$owner" = "$endpoint" ]; then
                path=local
            else
                path=routed
            fi

            input="$(blob_source_metric "$metrics" \
                nginx_media_source_frames_in "$name")"
            program="$(blob_label_metric "$metrics" \
                nginx_media_stream_program_frames "$name")"
            feed_units="$(blob_label_metric "$metrics" \
                nginx_media_stream_feed_units "$name")"
            feed_bytes="$(blob_label_metric "$metrics" \
                nginx_media_stream_feed_bytes "$name")"
            feed_high_units="$(blob_label_metric "$metrics" \
                nginx_media_stream_feed_high_water_units "$name")"
            feed_high_bytes="$(blob_label_metric "$metrics" \
                nginx_media_stream_feed_high_water_bytes "$name")"
            evictions="$(blob_label_metric "$metrics" \
                nginx_media_stream_feed_evictions_total "$name")"
            overruns="$(blob_label_metric "$metrics" \
                nginx_media_stream_feed_overruns_total "$name")"
            generation_mismatches="$(blob_label_metric "$metrics" \
                nginx_media_stream_feed_generation_mismatches_total "$name")"
            publish_errors="$(blob_label_metric "$metrics" \
                nginx_media_stream_feed_publish_errors_total "$name")"
            preroll_units="$(blob_source_metric "$metrics" \
                nginx_media_source_preroll_high_water_units "$name")"
            preroll_bytes="$(blob_source_metric "$metrics" \
                nginx_media_source_preroll_high_water_bytes "$name")"
            preroll_overflows="$(blob_source_metric "$metrics" \
                nginx_media_source_preroll_overflows_total "$name")"
            unit_overflows="$(blob_source_metric "$metrics" \
                nginx_media_source_preroll_unit_overflows_total "$name")"
            byte_overflows="$(blob_source_metric "$metrics" \
                nginx_media_source_preroll_byte_overflows_total "$name")"
            route_identity="$(blob_global_metric "$metrics" \
                nginx_media_runtime_routed_identity_mismatches_total)"
            route_slots="$(blob_global_metric "$metrics" \
                nginx_media_runtime_routed_slot_overflows_total)"
            route_no_slot="$(blob_global_metric "$metrics" \
                nginx_media_runtime_routed_no_slot_total)"
            route_no_payload="$(blob_global_metric "$metrics" \
                nginx_media_runtime_routed_no_payload_total)"
            route_reassembly="$(blob_global_metric "$metrics" \
                nginx_media_runtime_routed_reassembly_errors_total)"
            route_publish="$(blob_global_metric "$metrics" \
                nginx_media_runtime_routed_publish_errors_total)"

            input="${input:-0}"
            program="${program:-0}"
            route_identity="${route_identity:-0}"
            route_slots="${route_slots:-0}"
            route_no_slot="${route_no_slot:-0}"
            route_no_payload="${route_no_payload:-0}"
            route_reassembly="${route_reassembly:-0}"
            route_publish="${route_publish:-0}"
            route_fail=$(( route_identity + route_slots + route_no_slot
                         + route_no_payload + route_reassembly + route_publish ))

            echo "   $name owner=$owner landing=$landing path=$path" \
                 "input_frames=$input program_frames=$program" \
                 "feed_retained=${feed_units:-0}u/${feed_bytes:-0}B" \
                 "feed_high_water=${feed_high_units:-0}u/${feed_high_bytes:-0}B" \
                 "feed_evictions=${evictions:-0}" \
                 "feed_overruns=${overruns:-0}" \
                 "feed_generation_mismatches=${generation_mismatches:-0}" \
                 "feed_publish_errors=${publish_errors:-0}" \
                 "preroll_high_water=${preroll_units:-0}u/${preroll_bytes:-0}B" \
                 "preroll_overflows=${preroll_overflows:-0}" \
                 "preroll_unit_overflows=${unit_overflows:-0}" \
                 "preroll_byte_overflows=${byte_overflows:-0}" \
                 "route_failures=$route_fail" \
                 "route_identity=$route_identity" \
                 "route_slots=$route_slots" \
                 "route_no_slot=$route_no_slot" \
                 "route_no_payload=$route_no_payload" \
                 "route_reassembly=$route_reassembly" \
                 "route_publish=$route_publish"
        done

        kill_pubs
        sleep 1
    done

    return 0
}

capacity_json_field() {   # <stream snapshot> <dot-separated field>
    python3 - "$1" "$2" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    value = json.loads(source.read().split("# HELP", 1)[0])
for field in sys.argv[2].split("."):
    value = value[field]
if isinstance(value, list):
    print(" ".join("60000" if item is None else str(item) for item in value))
else:
    print(value)
PY
}

capacity_metric() {   # <metrics snapshot> <metric>
    awk -v metric="$2" '$1 == metric { print $2; exit }' "$1"
}

capacity_rtmp_receiver_ready() {   # <metrics file> <programs> <destinations> [offset]
    python3 - "$1" "$2" "$3" "${4:-0}" <<'PY'
import re
import sys

programs = int(sys.argv[2])
per_program = int(sys.argv[3])
offset = int(sys.argv[4])
expected = {
    (f"d{destination:04d}" if programs == 1
     else f"p{program:04d}-d{destination:04d}")
    for program in range(programs)
    for destination in range(offset, offset + per_program)
}
received = {}
with open(sys.argv[1], encoding="utf-8") as source:
    for line in source:
        match = re.match(
            r"^nginx_media_source_payload_bytes_in_total\{(.*?)\}\s+([0-9]+)\s*$",
            line,
        )
        if match is None:
            continue
        labels = dict(re.findall(
            r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:\\.|[^"])*)"',
            match.group(1),
        ))
        name = labels.get("name")
        if (labels.get("application") == "live"
                and name in expected and labels.get("source") == name):
            received[name] = int(match.group(2))
raise SystemExit(
    0 if received.keys() >= expected and all(received[name] > 0 for name in expected)
    else 1
)
PY
}

capacity_rtmp_payload_report() {   # <before prefix> <after prefix> <programs> <destinations> [offset]
    python3 - "$1" "$2" "$3" "$4" "${5:-0}" <<'PY'
import glob
import re
import sys

programs = int(sys.argv[3])
per_program = int(sys.argv[4])
offset = int(sys.argv[5])
expected = {
    (f"d{destination:04d}" if programs == 1
     else f"p{program:04d}-d{destination:04d}")
    for program in range(programs)
    for destination in range(offset, offset + per_program)
}

def read(prefix):
    values = {}
    for path in glob.glob(prefix + ".*"):
        with open(path, encoding="utf-8") as source:
            for line in source:
                match = re.match(
                    r"^nginx_media_source_payload_bytes_in_total\{(.*?)\}\s+([0-9]+)\s*$",
                    line,
                )
                if match is None:
                    continue
                labels = dict(re.findall(
                    r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:\\.|[^"])*)"',
                    match.group(1),
                ))
                name = labels.get("name")
                if (labels.get("application") == "live"
                        and name in expected and labels.get("source") == name):
                    values[name] = values.get(name, 0) + int(match.group(2))
    return values

before = read(sys.argv[1])
after = read(sys.argv[2])
for label, values in (("before", before), ("after", after)):
    missing = sorted(expected - values.keys())
    if missing:
        raise SystemExit(
            f"RTMP {label} metrics missing destination streams: {missing[:4]}"
        )
deltas = {name: after[name] - before[name] for name in expected}
undelivered = sorted(name for name, value in deltas.items() if value <= 0)
if undelivered:
    raise SystemExit(
        f"RTMP destinations received no measured payload: {undelivered[:4]}"
    )
print(f"receiver_delivered_rtmp_payload_bytes={sum(deltas.values())}")
print(f"rtmp_receiver_count={len(deltas)}")
for name in sorted(deltas):
    print(f"destination_{name} receiver_delivered_rtmp_payload_bytes={deltas[name]}")
PY
}


capacity_destination_id() {   # <program index> <destination index> <program count>
    if [ "$3" -eq 1 ]; then
        printf 'd%04d' "$2"
    else
        printf 'p%04d-d%04d' "$1" "$2"
    fi
}

capacity_percentile() {   # <metrics snapshot> <stream> <percentile>
    awk -v name="$2" -v percentile="$3" '
        index($1, "nginx_media_stream_fanout_delay_ms{") == 1 &&
        index($1, "name=\"" name "\"") > 0 &&
        index($1, "percentile=\"" percentile "\"") > 0 { print $2; exit }
    ' "$1"
}

capacity_worker_pid() {
    sed -n \
        's/^nginx_media_worker_info{worker="[^"]*",pid="\([0-9][0-9]*\)"} 1$/\1/p'
}

capacity_worker_metrics() {   # <file prefix>; one response from every worker
    local prefix="$1" required valid blob pid attempt
    local -A seen=()

    required="$(worker_pids | awk 'NF { n++ } END { print n + 0 }')"
    valid=" $(worker_pids | tr '\n' ' ') "

    for attempt in $(seq 1 "$(( required * 50 ))"); do
        blob="$(curl -fsS "$(api)/metrics" 2>/dev/null || true)"
        pid="$(printf '%s\n' "$blob" | capacity_worker_pid | head -1)"

        case "$valid" in
            *" $pid "*)
                if [ -z "${seen[$pid]:-}" ]; then
                    printf '%s\n' "$blob" > "$prefix.$pid"
                    seen[$pid]=1
                fi
                ;;
        esac

        [ "${#seen[@]}" -ge "$required" ] && break
    done

    if [ "${#seen[@]}" -ne "$required" ]; then
        echo "   only scraped ${#seen[@]} of $required worker metrics" >&2
        return 1
    fi
}

capacity_rtmp_sink_metrics() {   # <file prefix>
    local prefix="$1"

    [ -n "$RTMP_SINK_WORKER_PID" ] || return 0
    curl -fsS "$(rtmp_sink_api)/metrics" \
        > "$prefix.$RTMP_SINK_WORKER_PID" \
        || { echo "could not scrape RTMP sink worker metrics" >&2; return 1; }
}

capacity_socket_snapshot() {   # <file prefix>
    local prefix="$1" pids

    pids="$(worker_pids | tr '\n' ' ')"
    ss -t -n -a -m -p > "$prefix.tcp" || return 1
    ss -u -n -a -m -p > "$prefix.udp" || return 1
    cp /proc/net/sockstat "$prefix.sockstat" || return 1

    python3 - "$prefix" "$pids" > "$prefix" <<'PY'
import re
import sys

prefix = sys.argv[1]
pids = [pid for pid in sys.argv[2].split() if pid]
stats = {
    pid: {"tcp_sockets": 0, "tcp_bytes": 0,
          "udp_sockets": 0, "udp_bytes": 0}
    for pid in pids
}

for proto in ("tcp", "udp"):
    with open(f"{prefix}.{proto}", encoding="utf-8") as source:
        lines = source.readlines()

    for index, line in enumerate(lines):
        match = re.search(r"skmem:\(([^)]*)\)", line)
        if match is None:
            continue

        chunk = "".join(lines[max(0, index - 2):index + 1])
        owners = set(re.findall(r"pid=(\d+)", chunk)).intersection(stats)
        if not owners:
            continue

        fields = dict((key, int(value))
                      for key, value in re.findall(
                          r"([a-z]+)(\d+)", match.group(1)))
        memory = sum(fields.get(key, 0)
                     for key in ("r", "t", "f", "w", "o", "bl"))
        for pid in owners:
            stats[pid][f"{proto}_sockets"] += 1
            stats[pid][f"{proto}_bytes"] += memory

for pid in sorted(stats, key=int):
    row = stats[pid]
    sockets = row["tcp_sockets"] + row["udp_sockets"]
    memory = row["tcp_bytes"] + row["udp_bytes"]
    print(pid, row["tcp_sockets"], row["tcp_bytes"],
          row["udp_sockets"], row["udp_bytes"], sockets, memory,
          sep="\t")
PY
}

capacity_sockstat_value() {   # <snapshot> <protocol:> <field>
    awk -v proto="$2" -v field="$3" '
        $1 == proto {
            for (i = 2; i < NF; i += 2) {
                if ($i == field) {
                    print $(i + 1)
                    exit
                }
            }
        }
    ' "$1"
}

capacity_rate_bps() {
    python3 - "$1" <<'PY'
import re
import sys

match = re.fullmatch(r"(\d+(?:\.\d+)?)([kKmMgG]?)", sys.argv[1])
if match is None:
    raise SystemExit(f"invalid bitrate: {sys.argv[1]}")
scale = {"": 1, "k": 1_000, "m": 1_000_000, "g": 1_000_000_000}
bps = round(float(match.group(1)) * scale[match.group(2).lower()])
if bps <= 0 or bps > 60_000_000:
    raise SystemExit("bitrate must be positive and no higher than 60M")
print(bps)
PY
}
capacity_build_srt_sink() {
    local source="$ROOT/tests/bench/srt_fanout_sink.c"
    local binary="$RUN/srt_fanout_sink"
    local -a cflags=() libs=()

    [ -x "$binary" ] && [ "$binary" -nt "$source" ] && return 0
    command -v cc >/dev/null && command -v pkg-config >/dev/null \
        && pkg-config --exists srt \
        || { echo "cc, pkg-config, and libsrt are required for SRT capacity cases" \
                 >&2; return 1; }
    [ -f "$source" ] \
        || { echo "missing shared SRT receiver helper: $source" >&2; return 1; }
    read -r -a cflags <<< "$(pkg-config --cflags srt)"
    read -r -a libs <<< "$(pkg-config --libs srt)"
    cc -O2 -g -Wall -Wextra -Werror -std=c11 "${cflags[@]}" \
        "$source" -o "$binary" "${libs[@]}" \
        || { echo "could not build the shared SRT receiver helper" >&2; return 1; }
}

capacity_srt_csv_report() {   # <receiver CSV> <programs> <destinations> <stall ID> <before snapshot> <after snapshot>
    python3 - "$1" "$2" "$3" "${4:-}" "$5" "$6" <<'PY'
import csv
import math
import re
import sys

programs = int(sys.argv[2])
per_program = int(sys.argv[3])
expected = programs * per_program
expected_stall = sys.argv[4]
expected_ids = {
    (f"d{destination:04d}" if programs == 1
     else f"p{program:04d}-d{destination:04d}")
    for program in range(programs)
    for destination in range(per_program)
}

def snapshot(path):
    values = {}
    with open(path, newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        required = {"destination_id", "bytes_received"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise SystemExit(f"SRT snapshot {path} does not match its schema")
        for row in reader:
            destination = row["destination_id"]
            try:
                received = int(row["bytes_received"])
            except (TypeError, ValueError):
                raise SystemExit(f"invalid snapshot byte count for {destination}")
            if received < 0 or destination in values:
                raise SystemExit(f"invalid or duplicate snapshot row for {destination}")
            values[destination] = received
    if set(values) != expected_ids:
        missing = sorted(expected_ids - set(values))
        unexpected = sorted(set(values) - expected_ids)
        raise SystemExit(
            f"SRT snapshot IDs mismatch; missing={missing[:4]} "
            f"unexpected={unexpected[:4]}"
        )
    return values

before = snapshot(sys.argv[5])
after = snapshot(sys.argv[6])
if any(after[name] < before[name] for name in expected_ids):
    raise SystemExit("SRT receiver byte counter decreased between snapshots")

rows = []
with open(sys.argv[1], newline="", encoding="utf-8") as source:
    reader = csv.DictReader(source)
    required = {"destination_id", "bytes_received", "first_ms", "stalled"}
    if reader.fieldnames is None or not required.issubset(reader.fieldnames):
        raise SystemExit("SRT receiver CSV does not match its documented schema")
    for row in reader:
        destination = row["destination_id"]
        try:
            total_received = int(row["bytes_received"])
        except (TypeError, ValueError):
            raise SystemExit(f"invalid receiver byte count for {destination}")
        if total_received < 0:
            raise SystemExit(f"negative receiver byte count for {destination}")
        try:
            first_ms = float(row["first_ms"]) if row["first_ms"] else 0.0
        except ValueError:
            raise SystemExit(f"invalid first_ms for {destination}")
        if not math.isfinite(first_ms):
            raise SystemExit(f"invalid first_ms for {destination}")
        stalled = row["stalled"].lower() in ("1", "true", "yes")
        match = re.search(r"(?:^|-)p([0-9]+)-d[0-9]+$", destination)
        program = int(match.group(1)) if match else 0
        rows.append((destination, after[destination] - before[destination],
                     total_received, first_ms, stalled, program))

if len(rows) != expected:
    raise SystemExit(
        f"SRT receiver CSV has {len(rows)} destinations; expected {expected}"
    )
destinations = [row[0] for row in rows]
if len(set(destinations)) != len(destinations):
    raise SystemExit("SRT receiver CSV contains duplicate destination IDs")
if set(destinations) != expected_ids:
    missing = sorted(expected_ids - set(destinations))
    unexpected = sorted(set(destinations) - expected_ids)
    raise SystemExit(
        f"SRT receiver IDs mismatch; missing={missing[:4]} "
        f"unexpected={unexpected[:4]}"
    )
undelivered = [row[0] for row in rows if not row[4] and row[1] == 0]
if undelivered:
    raise SystemExit(
        f"healthy SRT destinations received no payload in the measured window: "
        f"{undelivered[:4]}"
    )
if expected_stall:
    stalled_rows = [row for row in rows if row[0] == expected_stall]
    if len(stalled_rows) != 1 or not stalled_rows[0][4]:
        raise SystemExit(
            f"slow-reader destination {expected_stall} was not reported stalled"
        )
    if stalled_rows[0][1] != 0:
        raise SystemExit("the deliberately stalled SRT reader was consumed")
    if sum(1 for row in rows if row[4]) != 1:
        raise SystemExit("slow-reader case must report exactly one stalled destination")
    if not any(row[1] > 0 and not row[4] for row in rows):
        raise SystemExit("healthy SRT destinations received no payload in the measured window")

def jain(values):
    total = sum(values)
    squares = sum(value * value for value in values)
    return total * total / (len(values) * squares) if squares else 0.0

totals = {}
for _, received, _, _, _, program in rows:
    totals[program] = totals.get(program, 0) + received
stalled = [row for row in rows if row[4]]
healthy = [row for row in rows if not row[4]]
stalled_bytes = sum(row[1] for row in stalled)
healthy_bytes = sum(row[1] for row in healthy)
spread = max(row[3] for row in rows)
print(f"receiver_delivered_bytes={sum(row[1] for row in rows)}")
print(f"receiver_total_bytes={sum(row[2] for row in rows)}")
print(f"destination_fairness_jain={jain([row[1] for row in rows]):.4f}")
print(f"healthy_destination_fairness_jain={jain([row[1] for row in healthy]):.4f}")
print(f"healthy_receiver_delivered_bytes={healthy_bytes}")
print(f"healthy_destination_count={len(healthy)}")
print(f"stalled_destination_count={len(stalled)}")
print(f"stalled_receiver_delivered_bytes={stalled_bytes}")
print(f"first_byte_spread_ms={spread:.3f}")
print(f"program_fairness_jain={jain(list(totals.values())):.4f}")
for program, received in sorted(totals.items()):
    print(f"program_p{program:04d}_receiver_delivered_bytes={received}")
for destination, received, total_received, first_ms, stalled, _ in rows:
    print(f"destination_{destination} receiver_delivered_bytes={received} "
          f"receiver_total_bytes={total_received} first_ms={first_ms:.3f} "
          f"stalled={'yes' if stalled else 'no'}")
PY
}

capacity_srt_receiver_snapshot() {   # <receiver PID> <snapshot path> <copy path>
    local pid="$1" snapshot="$2" output="$3" attempt

    rm -f "$snapshot" "$output" || return 1
    kill -USR1 "$pid" 2>/dev/null \
        || { echo "could not request SRT receiver snapshot" >&2; return 1; }
    for attempt in $(seq 1 100); do
        [ -s "$snapshot" ] && break
        kill -0 "$pid" 2>/dev/null \
            || { echo "SRT receiver exited before writing a snapshot" >&2; return 1; }
        sleep 0.01
    done
    [ -s "$snapshot" ] \
        || { echo "SRT receiver snapshot was not written" >&2; return 1; }
    cp "$snapshot" "$output"
}

capacity_srt_measurement_snapshot() {   # <receiver PID> <snapshot> <copy> <start|stop>
    local pid="$1" snapshot="$2" output="$3" action="$4" attempt

    rm -f "$snapshot" "$output" || return 1
    kill -USR2 "$pid" 2>/dev/null \
        || { echo "could not toggle SRT receiver measurement $action" >&2; return 1; }
    for attempt in $(seq 1 1000); do
        [ -s "$snapshot" ] && break
        kill -0 "$pid" 2>/dev/null \
            || { echo "SRT receiver exited before $action snapshot" >&2; return 1; }
        sleep 0.01
    done
    [ -s "$snapshot" ] \
        || { echo "SRT receiver $action snapshot was not written" >&2; return 1; }
    cp "$snapshot" "$output"
}

capacity_quality_stop_monitors() {
    local pid status=0

    for pid in ${QUALITY_MONITORS[@]+"${QUALITY_MONITORS[@]}"}; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in ${QUALITY_MONITORS[@]+"${QUALITY_MONITORS[@]}"}; do
        wait "$pid" 2>/dev/null || status=1
    done
    QUALITY_MONITORS=()
    return "$status"
}

capacity_srt_incomplete_report() {   # <receiver snapshot> <programs> <destinations>
    python3 - "$1" "$2" "$3" <<'PY'
import csv
import sys

programs = int(sys.argv[2])
per_program = int(sys.argv[3])
expected_ids = {
    (f"d{destination:04d}" if programs == 1
     else f"p{program:04d}-d{destination:04d}")
    for program in range(programs)
    for destination in range(per_program)
}
with open(sys.argv[1], newline="", encoding="utf-8") as source:
    reader = csv.DictReader(source)
    if reader.fieldnames is None or "destination_id" not in reader.fieldnames:
        raise SystemExit(f"SRT snapshot {sys.argv[1]} does not match its schema")
    accepted = {row["destination_id"] for row in reader}
missing = sorted(expected_ids - accepted)
unexpected = sorted(accepted - expected_ids)
print(f"SRT receiver accepted {len(accepted)} of {len(expected_ids)} peers; "
      f"missing={missing[:16]} unexpected={unexpected[:4]}")
PY
}


capacity_srt_metrics_report() {   # <before worker-metrics prefix> <after prefix>
    python3 - "$1" "$2" <<'PY'
import glob
import math
import os
import re
import sys

metrics = (
    "nginx_media_srt_egress_shard_destinations",
    "nginx_media_srt_egress_shard_feed_queue_units",
    "nginx_media_srt_egress_shard_feed_queue_bytes",
    "nginx_media_srt_egress_shard_feed_queue_dropped_total",
    "nginx_media_srt_egress_shard_output_queue_units",
    "nginx_media_srt_egress_shard_output_queue_bytes",
    "nginx_media_srt_egress_shard_output_dropped_total",
    "nginx_media_srt_egress_shard_sent_bytes_total",
    "nginx_media_srt_egress_shard_sent_bursts_total",
    "nginx_media_srt_egress_shard_blocked_sends_total",
    "nginx_media_srt_egress_shard_retransmitted_packets_total",
)

def read_snapshot(prefix):
    values = {}
    for path in glob.glob(prefix + ".*"):
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as source:
            for line in source:
                match = re.match(
                    r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{(.*?)\})?\s+"
                    r"([-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)",
                    line,
                )
                if not match or match.group(1) not in metrics:
                    continue
                labels = dict(re.findall(
                    r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:\\.|[^"])*)"',
                    match.group(2) or "",
                ))
                if "worker" not in labels or "shard" not in labels:
                    continue
                try:
                    value = float(match.group(3))
                except ValueError:
                    continue
                if math.isfinite(value):
                    values[(labels["worker"], labels["shard"],
                            match.group(1))] = value
    return values


def active_workers(prefix):
    values = []
    for path in glob.glob(prefix + ".*"):
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as source:
            for line in source:
                if not line.startswith("nginx_media_egress_active_workers{"):
                    continue
                labels_text, _, value_text = line.partition("}")
                labels = dict(re.findall(
                    r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:\\.|[^"])*)"',
                    labels_text,
                ))
                if labels.get("engine") == "srt_shard":
                    values.append(int(float(value_text.strip())))
    return max(values) if values else "unavailable"


before_active = active_workers(sys.argv[1])
after_active = active_workers(sys.argv[2])
print(f"srt_active_senders_before={before_active}")
print(f"srt_active_senders_after={after_active}")
before = read_snapshot(sys.argv[1])
after = read_snapshot(sys.argv[2])
all_keys = {(worker, shard) for worker, shard, _ in set(before) | set(after)}
keys = sorted(
    (key for key in all_keys
     if after.get((key[0], key[1], metrics[0]), 0.0) > 0
     or before.get((key[0], key[1], metrics[0]), 0.0) > 0),
    key=lambda key: (key[0], key[1]),
)
if not all_keys:
    print("srt_shard_metrics=unavailable")
    raise SystemExit(0)
if not keys:
    print("srt_shard_metrics=no_active_destinations")
    raise SystemExit(0)

sent = []
for worker, shard in keys:
    def value(snapshot, metric):
        return snapshot.get((worker, shard, metric), 0.0)
    deltas = {
        "sent_bytes_delta": value(after, metrics[7]) - value(before, metrics[7]),
        "sent_bursts_delta": value(after, metrics[8]) - value(before, metrics[8]),
        "blocked_sends_delta": value(after, metrics[9]) - value(before, metrics[9]),
        "retransmitted_packets_delta": value(after, metrics[10]) - value(before, metrics[10]),
        "feed_drops_delta": value(after, metrics[3]) - value(before, metrics[3]),
        "output_drops_delta": value(after, metrics[6]) - value(before, metrics[6]),
    }
    sent.append(deltas["sent_bytes_delta"])
    gauge = {
        "destinations": value(after, metrics[0]),
        "feed_queue_units": value(after, metrics[1]),
        "feed_queue_bytes": value(after, metrics[2]),
        "output_queue_units": value(after, metrics[4]),
        "output_queue_bytes": value(after, metrics[5]),
    }
    fields = []
    for key, number in (*gauge.items(), *deltas.items()):
        formatted = str(int(number)) if number.is_integer() else f"{number:g}"
        fields.append(f"{key}={formatted}")
    print(f"shard worker={worker} shard={shard} " + " ".join(fields))
total = sum(sent)
squares = sum(value * value for value in sent)
jain = total * total / (len(sent) * squares) if squares else 0.0
print(f"shard_sent_bytes_spread={min(sent):.0f}..{max(sent):.0f} "
      f"range={(max(sent) - min(sent)):.0f} jain={jain:.4f}")
PY
}

capacity_memory_snapshot() {   # <file>
    local output="$1" pid
    local -a pids=()

    : > "$output" || return 1
    mapfile -t pids < <(worker_pids)
    [ -n "$RTMP_SINK_WORKER_PID" ] \
        && pids+=( "$RTMP_SINK_WORKER_PID" )

    for pid in "${pids[@]}"; do
        awk -v pid="$pid" '
            $1 == "Rss:" { rss = $2 }
            $1 == "Pss:" { pss = $2 }
            END {
                if (rss == "" || pss == "") exit 1
                print pid, rss, pss
            }
        ' "/proc/$pid/smaps_rollup" >> "$output" \
            || { echo "could not read worker $pid smaps_rollup" >&2; return 1; }
    done
}

capacity_memory_report() {   # <before file> <after file>
    awk '
        NR == FNR { rss[$1] = $2; pss[$1] = $3; next }
        {
            br = rss[$1] + 0
            bp = pss[$1] + 0
            printf "      pid=%s rss_kb=%d->%d delta_kb=%+d pss_kb=%d->%d delta_kb=%+d\n",
                   $1, br, $2, $2 - br, bp, $3, $3 - bp
        }
    ' "$1" "$2"
}


capacity_worker_cpu() {   # <before> <after> <seconds> <slot>
    report_cpu "$1" "$2" "$3" \
        | awk -v slot="$4" '$1 == slot { total += $3 }
            END { printf "%.1f", total }'
}

capacity_case() {   # <label> <programs> <bitrate> <destinations> <seconds> [srt|rtmp|hls|hls-push|rtmp-hls-push] [stall destination ID] [yes|no quality] [SRT share percent] [HLS push share percent]
    local label="$1" programs="$2" rate="$3" destinations="$4" seconds="$5"
    local protocol="${6:-srt}" stall_id="${7:-}" quality_mode="${8:-no}"
    local srt_share="${9:-0}"
    local hls_push_share="${10:-0}"
    local quality_failed=0 quality_status quality_report_file
    local hls_push_quality_report hls_push_reference_bps
    local quality_reference_bps rtmp_reference_bps hls_reference_bps
    local rtmp_report_file hls_report_file
    local receiver_sampler_pid queue_sampler_pid
    local workers="$CAPACITY_WORKERS" case_id source rate_bps offered_in offered_out
    local primary_destinations=0 srt_destinations=0 rtmp_destinations=0
    local hls_readers=0 hls_push_destinations=0 hls_enabled=yes
    local total_dest total_srt_dest total_rtmp_dest total_hls_push_dest
    local started_srt started_rtmp started_hls_push sink_id output_id owner name slot index depth owner_streams
    local connect_wait connect_attempts sink_port sink_ready sink_csv
    local sink_snapshot_before sink_snapshot_after stall_seen rtmp_ready
    local sink_pid="" publisher_pid="" receiver_pid hls_pid="" hls_ready hls_log hls_playlist
    local hls_push_sink_pid="" hls_push_sink_port hls_push_ready ready_status
    local hls_push_progress_file hls_push_progress_received
    local hls_push_progress_remaining hls_push_progress_elapsed
    local before_frames after_frames before_dispatched after_dispatched
    local fanout_max p50 p95 p99 p999 frame_rate frame_delta fairness
    local measure_start measure_end measure_seconds
    local metrics_before metrics_after feed_units feed_bytes feed_high_units
    local feed_high_bytes feed_evictions feed_evictions_before
    local feed_evictions_after feed_overruns feed_overruns_before
    local feed_overruns_after source_frames source_frames_before source_frames_after
    local worker_pid worker_slot worker_before worker_after cpu_pct worker_cpu_total
    local budget_before budget_after visits_before visits_after
    local late_before late_after max_service event_delay
    local socket_before socket_after sock_tcp_before sock_tcp_after
    local sock_udp_before sock_udp_after tcp_inuse_before tcp_inuse_after
    local udp_inuse_before udp_inuse_after socket_row slow_drops
    local -a owner_pool=() one_owner=() names=() owners=()
    local -a counts_before=() counts_after=() bounds=()
    local -a progress_frames=() quality_worker_pids=()
    local bucket_total=0 dispatched_delta=0 bin_label bins_text=""
    local low high value
    [[ "$programs" =~ ^[1-9][0-9]*$ \
       && "$destinations" =~ ^[1-9][0-9]*$ \
       && "$workers" =~ ^[1-9][0-9]*$ \
       && "$seconds" =~ ^[1-9][0-9]*$ ]] \
        || { echo "invalid capacity case: $label" >&2; return 1; }
    case "$protocol" in
        srt|rtmp|hls|hls-push|rtmp-hls-push) ;;
        *) echo "invalid capacity protocol: $protocol" >&2; return 1 ;;
    esac
    case "$quality_mode" in
        yes|no) ;;
        *) echo "invalid quality mode: $quality_mode" >&2; return 1 ;;
    esac
    [[ "$srt_share" =~ ^(0|[1-9][0-9]*)$ ]] \
        && [ "$srt_share" -le 100 ] \
        || { echo "SRT share must be an integer from 0 to 100" >&2; return 1; }
    [[ "$hls_push_share" =~ ^(0|[1-9][0-9]*)$ ]] \
        && [ "$hls_push_share" -le 100 ] \
        || { echo "HLS push share must be an integer from 0 to 100" \
                 >&2; return 1; }
    if [ "$protocol" = rtmp-hls-push ]; then
        [ "$srt_share" -gt 0 ] && [ "$hls_push_share" -gt 0 ] \
            && [ "$(( srt_share + hls_push_share ))" -lt 100 ] \
            || { echo "three-way mixes need positive SRT, HLS push, and RTMP shares" \
                     >&2; return 1; }
    elif [ "$hls_push_share" -ne 0 ]; then
        echo "HLS push share is valid only for three-way mixes" >&2
        return 1
    fi
    if [ "$protocol" = srt ]; then
        [ "$srt_share" -eq 0 ] \
            || { echo "pure SRT cases do not take a secondary SRT share" >&2; return 1; }
        srt_destinations="$destinations"
    else
        srt_destinations=$(( (destinations * srt_share + 50) / 100 ))
    fi
    primary_destinations=$(( destinations - srt_destinations ))
    case "$protocol" in
        rtmp) rtmp_destinations="$primary_destinations" ;;
        hls) hls_readers="$primary_destinations" ;;
        hls-push) hls_push_destinations="$primary_destinations" ;;
        rtmp-hls-push)
            hls_push_destinations=$(( (destinations * hls_push_share + 50) / 100 ))
            rtmp_destinations=$(( primary_destinations - hls_push_destinations ))
            ;;
    esac
    if [ "$protocol" != srt ] && [ "$primary_destinations" -eq 0 ]; then
        echo "mixed cases require at least one $protocol destination" >&2
        return 1
    fi
    if [ "$quality_mode" = yes ]; then
        [ "$programs" -eq 1 ] && [ -z "$stall_id" ] \
            || { echo "quality cases require one un-stalled program" \
                     >&2; return 1; }
    fi
    if [ "$hls_readers" -gt 0 ] && [ "$quality_mode" != yes ]; then
        echo "HLS capacity cases require quality mode" >&2
        return 1
    fi
    if [ "$hls_push_destinations" -gt 0 ] && [ "$quality_mode" != yes ]; then
        echo "HLS push capacity cases require quality mode" >&2
        return 1
    fi
    if [ "$rtmp_destinations" -gt 0 ]; then
        [ -z "$stall_id" ] \
            || { echo "RTMP capacity cases do not support a stalled SRT peer" \
                     >&2; return 1; }
        # Receiver metrics are worker-local, so keep this complete RTMP
        # quality sample on one worker.
        workers=1
    fi
    if [ -n "$stall_id" ] && [ "$protocol" != srt ]; then
        echo "only pure SRT cases support a stalled destination" >&2
        return 1
    fi
    [ "$seconds" -le 600 ] \
        || { echo "capacity window must be at most 600 seconds" \
                 >&2; return 1; }
    [ "$workers" -le 16 ] && [ "$programs" -le 32 ] \
        && [ "$destinations" -le 1000 ] \
        && [ "$(( programs * destinations ))" -le 1000 ] \
        || { echo "capacity case exceeds bounded worker/output limits" \
                 >&2; return 1; }
    owner_streams=$(( (programs + workers - 1) / workers ))
    [ "$(( owner_streams * destinations ))" -le 1000 ] \
        || { echo "capacity case exceeds per-owner output limit" >&2; return 1; }
    command -v ss >/dev/null \
        || { echo "ss is required for socket memory snapshots" >&2; return 1; }
    if [ "$srt_destinations" -gt 0 ]; then
        capacity_build_srt_sink || return 1
    fi

    rate_bps="$(capacity_rate_bps "$rate")" || return 1
    [ "$(( rate_bps * programs * destinations ))" -le 8000000000 ] \
        || { echo "capacity case nominal egress exceeds 8000 Mbit/s" \
                 >&2; return 1; }
    offered_in="$(awk -v b="$(( programs * rate_bps ))" \
        'BEGIN { printf "%.1f", b / 1000000 }')"
    offered_out="$(awk -v b="$(( programs * rate_bps * destinations ))" \
        'BEGIN { printf "%.1f", b / 1000000 }')"
    total_dest=$(( programs * destinations ))
    total_srt_dest=$(( programs * srt_destinations ))
    total_rtmp_dest=$(( programs * rtmp_destinations ))
    total_hls_push_dest=$(( programs * hls_push_destinations ))
    CAPACITY_CASE_ID=$(( ${CAPACITY_CASE_ID:-0} + 1 ))
    case_id="$CAPACITY_CASE_ID"
    source="$RUN/media/capacity-$rate.ts"
    if [ ! -s "$source" ]; then
        make_source "$source" "$rate"
    fi

    mkdir -p "$RUN/capacity"
    local case_dir="$RUN/capacity/$label"
    rm -rf "$case_dir"
    mkdir -p "$case_dir"

    for (( slot = 0; slot < workers; slot++ )); do
        mapfile -t one_owner < <(
            owner_names "$slot" "$workers" "$programs" "cc${case_id}-"
        )
        [ "${#one_owner[@]}" -eq "$programs" ] \
            || { echo "could not name $programs streams for worker $slot" \
                     >&2; return 1; }
        for name in "${one_owner[@]}"; do
            owner_pool+=( "$slot:$name" )
        done
    done

    depth=0
    while [ "${#names[@]}" -lt "$programs" ]; do
        for (( slot = 0; slot < workers; slot++ )); do
            index=$(( slot * programs + depth ))
            [ "$index" -lt "${#owner_pool[@]}" ] || continue
            IFS=: read -r owner name <<< "${owner_pool[$index]}"
            owners+=( "$owner" )
            names+=( "$name" )
            [ "${#names[@]}" -ge "$programs" ] && break
        done
        depth=$(( depth + 1 ))
    done

    echo
    echo "== capacity $protocol $label: workers=$workers programs=$programs"
    echo "   video_rate=$rate offered_ingress=${offered_in}Mbit/s"
    echo "   destinations_per_program=$destinations total_destinations=$total_dest"
    echo "   per_program_outputs: SRT=$srt_destinations RTMP=$rtmp_destinations HLS_readers=$hls_readers HLS_push=$hls_push_destinations"
    echo "   egress_workers: SRT=$CAPACITY_FIXED_SRT_WORKERS HLS_push=$CAPACITY_FIXED_HLS_PUSH_WORKERS"
    echo "   offered_egress=${offered_out}Mbit/s window=${seconds}s"

    # RTMP-only receivers need to accept and drain the stream.  SRT cases
    # keep HLS enabled for their shared transport-stream output.
    if [ "$rtmp_destinations" -gt 0 ] \
        && [ "$srt_destinations" -eq 0 ] \
        && [ "$hls_readers" -eq 0 ] \
        && [ "$hls_push_destinations" -eq 0 ]; then
        hls_enabled=no
    fi
    start_instance "$workers" "$hls_enabled" no
    if [ "$rtmp_destinations" -gt 0 ]; then
        start_rtmp_sink_instance || return 1
    fi
    PUBS=()
    SINKS=()
    sink_port=$(( BASE + 20 ))
    if [ "$srt_destinations" -gt 0 ]; then
        sink_ready="$case_dir/srt.ready"
        sink_csv="$case_dir/srt.csv"
        sink_snapshot_before="$case_dir/srt.before.csv"
        sink_snapshot_after="$case_dir/srt.after.csv"
        rm -f "$sink_ready" "$sink_csv" "$sink_csv.snapshot" \
            "$sink_snapshot_before" "$sink_snapshot_after"
        if [ "$quality_mode" = yes ]; then
            "$RUN/srt_fanout_sink" "$sink_port" "$total_srt_dest" \
                "$sink_ready" "$sink_csv" quality \
                >"$case_dir/srt-receiver.log" 2>&1 &
        elif [ -n "$stall_id" ]; then
            "$RUN/srt_fanout_sink" "$sink_port" "$total_srt_dest" \
                "$sink_ready" "$sink_csv" "$stall_id" \
                >"$case_dir/srt-receiver.log" 2>&1 &
        else
            "$RUN/srt_fanout_sink" "$sink_port" "$total_srt_dest" \
                "$sink_ready" "$sink_csv" \
                >"$case_dir/srt-receiver.log" 2>&1 &
        fi
        sink_pid="$!"
        SINKS+=( "$sink_pid" )
    fi
    if [ "$hls_push_destinations" -gt 0 ]; then
        hls_push_sink_port=$(( BASE + 30 ))
        python3 "$ROOT/tests/bench/hls_push_capacity.py" serve \
            --port "$hls_push_sink_port" >"$case_dir/hls-push-sink.log" 2>&1 &
        hls_push_sink_pid="$!"
        SINKS+=( "$hls_push_sink_pid" )
        hls_push_ready=0
        for attempt in $(seq 1 100); do
            if curl -fsS "http://127.0.0.1:$hls_push_sink_port/__health" \
                >/dev/null 2>&1
            then
                hls_push_ready=1
                break
            fi
            kill -0 "$hls_push_sink_pid" 2>/dev/null \
                || { cat "$case_dir/hls-push-sink.log" >&2
                     echo "HLS push sink exited during startup" >&2; return 1; }
            sleep 0.1
        done
        [ "$hls_push_ready" -eq 1 ] \
            || { echo "HLS push sink did not become ready" >&2; return 1; }
    fi

    for name in "${names[@]}"; do
        curl -fsS -X POST -H 'Content-Type: application/json' \
            -d "{\"application\":\"live\",\"name\":\"$name\"}" \
            "$(api)/streams" >/dev/null \
            || { echo "could not create capacity stream $name" >&2; return 1; }
    done

    for (( index = 0; index < programs; index++ )); do
        name="${names[$index]}"
        slot="${owners[$index]}"
        owner="$(owner_of "$name")" || {
            echo "could not read owner for $name" >&2; return 1;
        }
        [ "$owner" = "$slot" ] \
            || { echo "$name owner $owner did not match expected worker $slot" \
                     >&2; return 1; }

        for (( attempt = 0; attempt < srt_destinations; attempt++ )); do
            sink_id="$(capacity_destination_id "$index" "$attempt" "$programs")"
            post_owner "/streams/live/$name/destinations" \
                "{\"id\":\"$sink_id\",\"type\":\"srt\",\"host\":\"127.0.0.1\",\"port\":$sink_port,\"streamid\":\"#!::r=live/$name,m=publish,s=$sink_id\"}" \
                || return 1
            [ "$sink_id" = "$stall_id" ] && stall_seen=1
        done
        if [ "$rtmp_destinations" -gt 0 ]; then
            for (( attempt = srt_destinations;
                  attempt < srt_destinations + rtmp_destinations;
                  attempt++ )); do
                output_id="$(capacity_destination_id "$index" "$attempt" "$programs")"
                post_owner "/streams/live/$name/destinations" \
                    "{\"id\":\"$output_id\",\"type\":\"rtmp\",\"host\":\"127.0.0.1\",\"port\":$RTMP_PORT,\"streamid\":\"$output_id\"}" \
                    || return 1
            done
        fi
        if [ "$hls_push_destinations" -gt 0 ]; then
            for (( attempt = srt_destinations + rtmp_destinations;
                  attempt < srt_destinations + rtmp_destinations + hls_push_destinations;
                  attempt++ )); do
                output_id="$(capacity_destination_id "$index" "$attempt" "$programs")"
                post_owner "/streams/live/$name/destinations" \
                    "{\"id\":\"$output_id\",\"type\":\"hls_push\",\"host\":\"http://127.0.0.1:$hls_push_sink_port/$output_id/\",\"path\":\"$RUN/hls/live/$name\"}" \
                    || return 1
            done
        fi
    done
    if [ -n "$stall_id" ] && [ "${stall_seen:-0}" -ne 1 ]; then
        echo "slow-reader destination ID $stall_id was not configured" >&2
        return 1
    fi

    connect_wait=$(( 30 + total_dest / 10 ))
    connect_attempts=$(( connect_wait * 10 ))
    for attempt in $(seq 1 "$connect_attempts"); do
        started_srt="$(grep -cE "media: srt destination .* started" \
            "$RUN/logs/error.log" 2>/dev/null || true)"
        started_rtmp="$(grep -cE "media: rtmp destination .* started" \
            "$RUN/logs/error.log" 2>/dev/null || true)"
        started_hls_push="$(grep -cE "media: hls push destination .* started" \
            "$RUN/logs/error.log" 2>/dev/null || true)"
        if [ "$started_srt" -ge "$total_srt_dest" ] \
            && [ "$started_rtmp" -ge "$total_rtmp_dest" ] \
            && [ "$started_hls_push" -ge "$total_hls_push_dest" ]; then
            break
        fi
        sleep 0.1
    done
    [ "$started_srt" -ge "$total_srt_dest" ] \
        && [ "$started_rtmp" -ge "$total_rtmp_dest" ] \
        && [ "$started_hls_push" -ge "$total_hls_push_dest" ] \
        || { echo "only ${started_srt:-0} of $total_srt_dest SRT, ${started_rtmp:-0} of $total_rtmp_dest RTMP and ${started_hls_push:-0} of $total_hls_push_dest HLS push destinations started" \
                 >&2; return 1; }
    # Start media while nonblocking SRT handshakes settle; send errors trigger the
    # output's normal retry path for a connection that did not establish.
    if [ "$hls_push_destinations" -gt 0 ]; then
        curl -fsS "http://127.0.0.1:$hls_push_sink_port/__mark" \
            >"$case_dir/hls-push.mark.json" \
            || { echo "could not mark HLS push first-segment timing" >&2; return 1; }
    fi
    for (( index = 0; index < programs; index++ )); do
        publish "${SRT_PORTS[${owners[$index]}]}" "${names[$index]}" "$source"
        PUBS+=( "$!" )
    done
    if [ "$srt_destinations" -gt 0 ]; then
        for attempt in $(seq 1 "$connect_attempts"); do
            [ -f "$sink_ready" ] && break
            kill -0 "$sink_pid" 2>/dev/null \
                || { echo "shared SRT receiver exited before accepting all peers" \
                     >&2; return 1; }
            sleep 0.1
        done
        if [ ! -f "$sink_ready" ]; then
            if capacity_srt_receiver_snapshot "$sink_pid" \
                "$sink_csv.snapshot" "$case_dir/srt.incomplete.csv"; then
                capacity_srt_incomplete_report "$case_dir/srt.incomplete.csv" \
                    "$programs" "$srt_destinations" || return 1
            else
                echo "could not capture incomplete SRT receiver snapshot" >&2
            fi
            echo "shared SRT receiver accepted fewer than $total_srt_dest peers" >&2
            return 1
        fi
    fi


    # Exclude publisher connection and the first output burst from measurement.
    for attempt in $(seq 1 120); do
        progress_ready=1
        for name in "${names[@]}"; do
            source_frames="$(owner_metric "$name" \
                nginx_media_stream_program_frames)"
            dispatched_delta="$(owner_metric "$name" \
                nginx_media_stream_dispatched_total)"
            if [ "${source_frames:-0}" -le 0 ] \
                || [ "${dispatched_delta:-0}" -le 0 ]; then
                progress_ready=0
                break
            fi
        done
        [ "$progress_ready" -eq 1 ] && break
        sleep 0.1
    done
    [ "$progress_ready" -eq 1 ] \
        || { echo "capacity streams never reached program/output progress" \
                 >&2; return 1; }
    if [ "$rtmp_destinations" -gt 0 ]; then
        rtmp_ready=0
        for attempt in $(seq 1 "$connect_attempts"); do
            curl -fsS "$(rtmp_sink_api)/metrics" \
                > "$case_dir/rtmp-warmup.metrics" \
                || { echo "could not scrape RTMP receiver metrics" >&2; return 1; }
            if capacity_rtmp_receiver_ready "$case_dir/rtmp-warmup.metrics" \
                "$programs" "$rtmp_destinations" "$srt_destinations"
            then
                rtmp_ready=1
                break
            fi
            sleep 0.1
        done
        [ "$rtmp_ready" -eq 1 ] \
            || { echo "RTMP capacity receivers did not all accept media" \
                 >&2; return 1; }
    fi
    if [ "$hls_readers" -gt 0 ]; then
        hls_playlist="$RUN/hls/live/${names[0]}/index.m3u8"
        echo "   waiting for first completed HLS segment"
        for attempt in $(seq 1 "$connect_attempts"); do
            [ -s "$hls_playlist" ] && break
            sleep 0.1
        done
        [ -s "$hls_playlist" ] \
            || { echo "HLS segmenter did not publish $hls_playlist" >&2; return 1; }
        hls_ready="$case_dir/hls.ready"
        hls_report_file="$case_dir/hls-readers.csv"
        hls_log="$case_dir/hls-reader.log"
        rm -f "$hls_ready" "$hls_report_file"
        python3 "$ROOT/tests/bench/hls_capacity_readers.py" \
            --url "http://127.0.0.1:$HTTP_PORT/hls/live/${names[0]}/index.m3u8" \
            --readers "$hls_readers" --duration "$(( seconds + 60 ))" \
            --report "$hls_report_file" --ready-file "$hls_ready" \
            --reference-bps "${CAPACITY_QUALITY_REFERENCE_HLS_BPS:-0}" \
            --min-delivery-ratio "$CAPACITY_QUALITY_MIN_DELIVERY_RATIO" \
            >"$hls_log" 2>&1 &
        hls_pid="$!"
        QUALITY_MONITORS+=( "$hls_pid" )
        for attempt in $(seq 1 "$connect_attempts"); do
            [ -f "$hls_ready" ] && break
            kill -0 "$hls_pid" 2>/dev/null \
                || { cat "$hls_log" >&2; echo "HLS readers exited during warmup" >&2; return 1; }
            sleep 0.1
        done
        [ -f "$hls_ready" ] \
            || { cat "$hls_log" >&2; echo "HLS readers did not become ready" >&2; return 1; }
    fi
    if [ "$hls_push_destinations" -gt 0 ]; then
        hls_push_ready=0
        hls_push_progress_file="$case_dir/hls-push-readiness.json"
        : > "$case_dir/hls-push-readiness.csv"
        echo "elapsed_s,received,expected,remaining" \
            > "$case_dir/hls-push-readiness.csv"
        for attempt in $(seq 1 "$connect_attempts"); do
            ready_status="$(curl -sS -o /dev/null -w '%{http_code}' \
                "http://127.0.0.1:$hls_push_sink_port/__ready?destinations=$hls_push_destinations&offset=$((srt_destinations + rtmp_destinations))" \
                2>/dev/null || true)"
            if [ $(( attempt % 10 )) -eq 0 ] || [ "$ready_status" = 200 ]; then
                curl -fsS \
                    "http://127.0.0.1:$hls_push_sink_port/__progress?destinations=$hls_push_destinations&offset=$((srt_destinations + rtmp_destinations))" \
                    > "$hls_push_progress_file" \
                    || { echo "could not query HLS push readiness progress" \
                             >&2; return 1; }
                read -r hls_push_progress_elapsed hls_push_progress_received \
                    hls_push_progress_remaining < <(python3 - \
                        "$hls_push_progress_file" "$hls_push_destinations" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    progress = json.load(source)
print(
    f"{progress['elapsed_s'] or 0:.3f}",
    progress["received"],
    len(progress["missing_ids"]),
)
PY
                )
                printf '%s,%s,%s,%s\n' "$hls_push_progress_elapsed" \
                    "$hls_push_progress_received" "$hls_push_destinations" \
                    "$hls_push_progress_remaining" \
                    >> "$case_dir/hls-push-readiness.csv"
                if [ "$hls_push_progress_received" != \
                     "${hls_push_last_progress_received:-}" ]; then
                    echo "   HLS push first-segment readiness: elapsed=${hls_push_progress_elapsed}s received=$hls_push_progress_received/$hls_push_destinations remaining=$hls_push_progress_remaining"
                    hls_push_last_progress_received="$hls_push_progress_received"
                fi
            fi
            if [ "$ready_status" = 200 ]; then
                hls_push_ready=1
                break
            fi
            kill -0 "$hls_push_sink_pid" 2>/dev/null \
                || { cat "$case_dir/hls-push-sink.log" >&2
                     echo "HLS push sink exited before all destinations received a segment" \
                         >&2; return 1; }
            sleep 0.1
        done
        if [ "$hls_push_ready" -ne 1 ]; then
            curl -fsS \
                "http://127.0.0.1:$hls_push_sink_port/__progress?destinations=$hls_push_destinations&offset=$((srt_destinations + rtmp_destinations))" \
                > "$hls_push_progress_file" || true
            python3 - "$hls_push_progress_file" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    progress = json.load(source)
print(
    "HLS push readiness timed out: "
    f"received={progress['received']}/{progress['expected']} "
    "remaining_ids=" + ",".join(progress["missing_ids"])
)
PY
            if cpu_window 3 "$hls_push_sink_pid"; then
                capacity_worker_metrics "$case_dir/workers.readiness" || true
                curl -fsS \
                    "http://127.0.0.1:$hls_push_sink_port/__progress?destinations=$hls_push_destinations&offset=$((srt_destinations + rtmp_destinations))" \
                    > "$hls_push_progress_file" || true
                curl -fsS "http://127.0.0.1:$hls_push_sink_port/__snapshot" \
                    > "$case_dir/hls-push.readiness.json" || true
                python3 "$ROOT/tests/bench/hls_push_capacity.py" \
                    readiness-report \
                    --metrics-prefix "$case_dir/workers.readiness" \
                    --stream "${names[0]}" \
                    --destinations "$hls_push_destinations" \
                    --destination-offset "$((srt_destinations + rtmp_destinations))" \
                    --sink-snapshot "$case_dir/hls-push.readiness.json" \
                    --progress "$hls_push_progress_file" \
                    > "$case_dir/hls-push-readiness-report.txt" || true
                cat "$case_dir/hls-push-readiness-report.txt"
                echo "   HLS push sink CPU during timeout diagnostics:"
                report_cpu "$W_BEFORE" "$W_AFTER" "$W_DUR" \
                    | awk -v pid="pid$hls_push_sink_pid" '
                        $1 == pid {
                            printf "      thread=%s cpu=%.1f%%\n", $2, $3
                            total += $3
                            count++
                        }
                        END {
                            if (!count) print "      no HLS push sink CPU rows"
                            else printf "      aggregate_hls_push_sink_cpu=%.1f%%\n", total
                        }'
            else
                echo "could not collect timeout CPU diagnostics" >&2
            fi
            echo "HLS push sink did not receive a TS segment from all" \
                 "$hls_push_destinations destinations before timeout" \
                 "(HTTP $ready_status)" >&2
            echo "readiness_progress=$case_dir/hls-push-readiness.csv" >&2
            return 1
        fi
        python3 - "$hls_push_progress_file" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    progress = json.load(source)
print(
    f"   HLS push all-destination first-segment latency="
    f"{progress['elapsed_s']:.3f}s ({progress['received']}/{progress['expected']})"
)
PY
        echo "   HLS push readiness progress=$case_dir/hls-push-readiness.csv"
    fi

    for name in "${names[@]}"; do
        owner_metrics "$name" > "$case_dir/stream.$name.before" \
            || { echo "could not capture baseline for $name" >&2; return 1; }
    done
    capacity_socket_snapshot "$case_dir/before" || return 1
    capacity_memory_snapshot "$case_dir/memory.before" || return 1
    capacity_worker_metrics "$case_dir/workers.before" || return 1
    capacity_rtmp_sink_metrics "$case_dir/rtmp-receiver.before" || return 1
    if [ "$srt_destinations" -gt 0 ]; then
        if [ "$quality_mode" = yes ]; then
            capacity_srt_measurement_snapshot "$sink_pid" "$sink_csv.snapshot" \
                "$sink_snapshot_before" start || return 1
            python3 "$ROOT/tests/bench/srt_capacity_quality.py" \
                sample-receivers "$sink_pid" "$sink_csv.snapshot" \
                "$sink_snapshot_before" "$case_dir/receiver-intervals.csv" \
                --interval 1 \
                >"$case_dir/receiver-sampler.log" 2>&1 &
            receiver_sampler_pid="$!"
            QUALITY_MONITORS+=( "$receiver_sampler_pid" )
            mapfile -t quality_worker_pids < <(worker_pids)
            python3 "$ROOT/tests/bench/srt_capacity_quality.py" \
                sample-queues "$HTTP_PORT" "$case_dir/queue-samples.csv" \
                "${quality_worker_pids[@]}" --interval 1 \
                >"$case_dir/queue-sampler.log" 2>&1 &
            queue_sampler_pid="$!"
            QUALITY_MONITORS+=( "$queue_sampler_pid" )
        else
            capacity_srt_receiver_snapshot "$sink_pid" "$sink_csv.snapshot" \
                "$sink_snapshot_before" || return 1
        fi
    fi
    if [ "$hls_push_destinations" -gt 0 ]; then
        curl -fsS "http://127.0.0.1:$hls_push_sink_port/__snapshot" \
            >"$case_dir/hls-push.before.json" \
            || { echo "could not capture HLS push receiver baseline" >&2; return 1; }
    fi
    measure_start="$(date +%s.%N)"
    if [ -n "$hls_pid" ]; then
        kill -USR1 "$hls_pid" \
            || { echo "could not start HLS reader measurement" >&2; return 1; }
    fi
    cpu_window "$seconds" "$sink_pid" "$hls_pid" "$hls_push_sink_pid" \
        "$RTMP_SINK_WORKER_PID"
    if [ "$hls_push_destinations" -gt 0 ]; then
        curl -fsS "http://127.0.0.1:$hls_push_sink_port/__snapshot" \
            >"$case_dir/hls-push.after.json" \
            || { echo "could not capture HLS push receiver final snapshot" >&2; return 1; }
    fi
    if [ "$quality_mode" = yes ]; then
        for publisher_pid in "${PUBS[@]}"; do
            kill -0 "$publisher_pid" 2>/dev/null \
                || { echo "a capacity publisher exited during the window" \
                     >&2; return 1; }
        done
        kill_pubs
    fi
    if [ "$quality_mode" = yes ]; then
        capacity_quality_stop_monitors \
            || { echo "a quality sampler failed during the measurement" \
                 >&2; return 1; }
    fi
    if [ "$srt_destinations" -gt 0 ]; then
        if [ "$quality_mode" = yes ]; then
            capacity_srt_measurement_snapshot "$sink_pid" "$sink_csv.snapshot" \
                "$sink_snapshot_after" stop || return 1
        else
            capacity_srt_receiver_snapshot "$sink_pid" "$sink_csv.snapshot" \
                "$sink_snapshot_after" || return 1
        fi
    fi

    for publisher_pid in "${PUBS[@]}"; do
        kill -0 "$publisher_pid" 2>/dev/null \
            || { echo "a capacity publisher exited during the window" \
                 >&2; return 1; }
    done
    for name in "${names[@]}"; do
        owner_metrics "$name" > "$case_dir/stream.$name.after" \
            || { echo "could not capture final metrics for $name" >&2; return 1; }
    done
    measure_end="$(date +%s.%N)"
    measure_seconds="$(awk -v a="$measure_start" -v b="$measure_end" \
        'BEGIN { printf "%.2f", b - a }')"
    capacity_worker_metrics "$case_dir/workers.after" || return 1
    capacity_rtmp_sink_metrics "$case_dir/rtmp-receiver.after" || return 1
    capacity_socket_snapshot "$case_dir/after" || return 1
    capacity_memory_snapshot "$case_dir/memory.after" || return 1

    echo "   measurement_s=$measure_seconds requested_window_s=$seconds" \
         "media_measurement_window_s=$measure_seconds cpu_window_s=$W_DUR" \
         "cpu_sampling_before_ms=$W_CPU_BEFORE_MS" \
         "cpu_sampling_after_ms=$W_CPU_AFTER_MS"

    for (( index = 0; index < programs; index++ )); do
        name="${names[$index]}"
        slot="${owners[$index]}"
        before_frames="$(capacity_json_field "$case_dir/stream.$name.before" \
            program_frames)"
        after_frames="$(capacity_json_field "$case_dir/stream.$name.after" \
            program_frames)"
        before_dispatched="$(capacity_json_field \
            "$case_dir/stream.$name.before" dispatched)"
        after_dispatched="$(capacity_json_field \
            "$case_dir/stream.$name.after" dispatched)"
        dispatched_delta=$(( after_dispatched - before_dispatched ))

        read -r -a counts_before <<< "$(
            capacity_json_field "$case_dir/stream.$name.before" \
                fanout_ms.bucket_counts
        )"
        read -r -a counts_after <<< "$(
            capacity_json_field "$case_dir/stream.$name.after" \
                fanout_ms.bucket_counts
        )"
        read -r -a bounds <<< "$(
            capacity_json_field "$case_dir/stream.$name.after" \
                fanout_ms.bucket_upper_ms
        )"
        if (( ${#counts_before[@]} != 16 ||
              ${#counts_after[@]} != 16 ||
              ${#bounds[@]} != 16 )); then
            echo "$name returned an invalid fanout histogram" >&2
            return 1
        fi

        bucket_total=0
        bins_text=""
        for (( attempt = 0; attempt < 16; attempt++ )); do
            value=$(( counts_after[attempt] - counts_before[attempt] ))
            [ "$value" -ge 0 ] \
                || { echo "$name fanout bucket counter regressed" \
                         >&2; return 1; }
            bucket_total=$(( bucket_total + value ))
            high="${bounds[$attempt]}"
            if [ "$attempt" -eq 0 ]; then
                low=0
            else
                low=$(( bounds[attempt - 1] + 1 ))
            fi
            if [ "$low" -eq "$high" ]; then
                bin_label="$low"
            else
                bin_label="$low-$high"
            fi
            [ -z "$bins_text" ] || bins_text+=","
            bins_text+="$bin_label:$value"
        done
        [ "$bucket_total" -eq "$dispatched_delta" ] \
            || { echo "$name fanout histogram has $bucket_total samples, " \
                     "but dispatched delta is $dispatched_delta" >&2; return 1; }
        [ "$dispatched_delta" -gt 0 ] \
            || { echo "$name produced no fanout samples" >&2; return 1; }

        fanout_max="$(capacity_json_field "$case_dir/stream.$name.after" \
            fanout_ms.max)"
        metrics_before="$(<"$case_dir/stream.$name.before")"
        metrics_after="$(<"$case_dir/stream.$name.after")"
        p50="$(capacity_percentile "$case_dir/stream.$name.after" "$name" 50)"
        p95="$(capacity_percentile "$case_dir/stream.$name.after" "$name" 95)"
        p99="$(capacity_percentile "$case_dir/stream.$name.after" "$name" 99)"
        p999="$(capacity_percentile "$case_dir/stream.$name.after" "$name" 99.9)"
        feed_units="$(blob_label_metric "$metrics_after" \
            nginx_media_stream_feed_units "$name")"
        feed_bytes="$(blob_label_metric "$metrics_after" \
            nginx_media_stream_feed_bytes "$name")"
        feed_high_units="$(blob_label_metric "$metrics_after" \
            nginx_media_stream_feed_high_water_units "$name")"
        feed_high_bytes="$(blob_label_metric "$metrics_after" \
            nginx_media_stream_feed_high_water_bytes "$name")"
        feed_overruns_before="$(blob_label_metric "$metrics_before" \
            nginx_media_stream_feed_overruns_total "$name")"
        feed_overruns_after="$(blob_label_metric "$metrics_after" \
            nginx_media_stream_feed_overruns_total "$name")"
        feed_overruns=$(( ${feed_overruns_after:-0} \
                        - ${feed_overruns_before:-0} ))
        feed_evictions_before="$(blob_label_metric "$metrics_before" \
            nginx_media_stream_feed_evictions_total "$name")"
        feed_evictions_after="$(blob_label_metric "$metrics_after" \
            nginx_media_stream_feed_evictions_total "$name")"
        feed_evictions=$(( ${feed_evictions_after:-0} \
                         - ${feed_evictions_before:-0} ))
        source_frames_before="$(blob_source_metric "$metrics_before" \
            nginx_media_source_frames_in "$name")"
        source_frames_after="$(blob_source_metric "$metrics_after" \
            nginx_media_source_frames_in "$name")"
        source_frames=$(( ${source_frames_after:-0} \
                        - ${source_frames_before:-0} ))
        frame_delta=$(( after_frames - before_frames ))
        [ "$frame_delta" -gt 0 ] \
            || { echo "$name made no program progress" >&2; return 1; }
        progress_frames+=( "$frame_delta" )
        frame_rate="$(awk -v f="$frame_delta" -v s="$measure_seconds" \
            'BEGIN { printf "%.1f", f / s }')"

        echo "   $name owner=w$slot source_frames=$source_frames"
        echo "      program_frames=$frame_delta"
        echo "      progress_fps=$frame_rate fanout_samples=$dispatched_delta"
        echo "      fanout_p50/p95/p99/p99.9=${p50:-0}/${p95:-0}/${p99:-0}/${p999:-0}ms"
        echo "      fanout.max=$fanout_max ms buckets_ms=[$bins_text]"
        echo "      feed_retained=${feed_units:-0}u/${feed_bytes:-0}B"
        echo "      feed_high_water=${feed_high_units:-0}u/${feed_high_bytes:-0}B"
        echo "      feed_evictions_delta=$feed_evictions"
        echo "      feed_overruns_delta=$feed_overruns"
    done

    fairness="$(python3 - "${progress_frames[@]}" <<'PY'
import sys

frames = [int(value) for value in sys.argv[1:]]
total = sum(frames)
squares = sum(value * value for value in frames)
print(f"{total * total / (len(frames) * squares):.4f}" if squares else "0")
PY
)"
    echo "   program_frame_fairness_jain=$fairness (1.0 is equal progress)"
    if [ "$srt_destinations" -gt 0 ]; then
        if kill -0 "$sink_pid" 2>/dev/null; then
            kill -TERM "$sink_pid" 2>/dev/null || true
        fi
        if ! wait "$sink_pid"; then
            echo "shared SRT receiver did not stop cleanly" >&2
            return 1
        fi
        SINKS=()
        [ -n "$hls_push_sink_pid" ] && SINKS+=( "$hls_push_sink_pid" )
        [ -s "$sink_csv" ] \
            || { echo "shared SRT receiver produced no result CSV" >&2; return 1; }
        if [ "$quality_mode" = yes ]; then
            echo "   per-destination rates, MPEG-TS counters, and queue quality:"
        else
            capacity_srt_csv_report "$sink_csv" "$programs" "$srt_destinations" \
                "$stall_id" "$sink_snapshot_before" "$sink_snapshot_after" \
                > "$case_dir/receiver-report.txt" || return 1
            echo "   receiver-delivered SRT payload bytes in measured window, per destination:"
            cat "$case_dir/receiver-report.txt"
        fi
        if [ "$quality_mode" = yes ]; then
            quality_report_file="$case_dir/quality-report.txt"
            quality_reference_bps="${CAPACITY_QUALITY_REFERENCE_BPS:-0}"
            python3 "$ROOT/tests/bench/srt_capacity_quality.py" report \
                --baseline "$sink_snapshot_before" \
                --final "$sink_snapshot_after" \
                --intervals "$case_dir/receiver-intervals.csv" \
                --queues "$case_dir/queue-samples.csv" \
                --receiver-results "$sink_csv" \
                --metrics-before "$case_dir/workers.before" \
                --metrics-after "$case_dir/workers.after" \
                --stream-before "$case_dir/stream.$name.before" \
                --stream-after "$case_dir/stream.$name.after" \
                --destination-report "$case_dir/destination-quality.csv" \
                --interval-report "$case_dir/interval-throughput.csv" \
                --destinations "$srt_destinations" \
                --prepared-source "$source" \
                --source-duration-s 30 \
                --reference-bps "$quality_reference_bps" \
                --min-delivery-ratio "$CAPACITY_QUALITY_MIN_DELIVERY_RATIO" \
                --interval-floor "$CAPACITY_QUALITY_INTERVAL_FLOOR" \
                --max-low-s "$CAPACITY_QUALITY_MAX_LOW_SECONDS" \
                --max-interval-s 2 \
                --queue-pressure "$CAPACITY_QUALITY_QUEUE_PRESSURE" \
                --pressure-samples "$CAPACITY_QUALITY_PRESSURE_SAMPLES" \
                --blocked-samples "$CAPACITY_QUALITY_BLOCKED_SAMPLES" \
                > "$quality_report_file" || return 1
            cat "$quality_report_file"
            echo "   destination_quality_csv=$case_dir/destination-quality.csv"
            echo "   interval_throughput_csv=$case_dir/interval-throughput.csv"
            quality_status="$(awk -F= '$1 == "quality_pass" { print $2; exit }' \
                "$quality_report_file")"
            case "$quality_status" in
                yes|no) ;;
                *) echo "quality analyzer returned no status" >&2; return 1 ;;
            esac
            if [ "$quality_reference_bps" = 0 ]; then
                CAPACITY_QUALITY_REFERENCE_BPS="$(awk -F= \
                    '$1 == "quality_reference_payload_bps" { print $2; exit }' \
                    "$quality_report_file")"
                [[ "$CAPACITY_QUALITY_REFERENCE_BPS" =~ ^[0-9]+([.][0-9]+)?$ ]] \
                    || { echo "quality analyzer returned an invalid reference rate" \
                         >&2; return 1; }
                echo "   calibrated_quality_reference_bps=$CAPACITY_QUALITY_REFERENCE_BPS"
            fi
            [ "$quality_status" = yes ] || quality_failed=1
        fi
    fi
    if [ "$rtmp_destinations" -gt 0 ]; then
        if [ "$quality_mode" = yes ]; then
            rtmp_reference_bps="${CAPACITY_QUALITY_REFERENCE_RTMP_BPS:-0}"
            rtmp_report_file="$case_dir/rtmp-quality.txt"
            python3 "$ROOT/tests/bench/rtmp_capacity_quality.py" \
                --before-prefix "$case_dir/rtmp-receiver.before" \
                --after-prefix "$case_dir/rtmp-receiver.after" \
                --queue-before-prefix "$case_dir/workers.before" \
                --queue-after-prefix "$case_dir/workers.after" \
                --programs "$programs" \
                --destinations "$rtmp_destinations" \
                --destination-offset "$srt_destinations" \
                --measurement-s "$measure_seconds" \
                --reference-bps "$rtmp_reference_bps" \
                --min-delivery-ratio "$CAPACITY_QUALITY_MIN_DELIVERY_RATIO" \
                --report "$case_dir/rtmp-quality.csv" \
                > "$rtmp_report_file" || return 1
            cat "$rtmp_report_file"
            echo "   rtmp_destination_quality_csv=$case_dir/rtmp-quality.csv"
            quality_status="$(awk -F= '$1 == "quality_pass" { print $2; exit }' \
                "$rtmp_report_file")"
            case "$quality_status" in
                yes|no) ;;
                *) echo "RTMP quality analyzer returned no status" >&2; return 1 ;;
            esac
            if [ "$rtmp_reference_bps" = 0 ]; then
                CAPACITY_QUALITY_REFERENCE_RTMP_BPS="$(awk -F= \
                    '$1 == "quality_reference_payload_bps" { print $2; exit }' \
                    "$rtmp_report_file")"
                [[ "$CAPACITY_QUALITY_REFERENCE_RTMP_BPS" =~ ^[0-9]+([.][0-9]+)?$ ]] \
                    || { echo "RTMP quality analyzer returned an invalid reference rate" \
                         >&2; return 1; }
                echo "   calibrated_rtmp_quality_reference_bps=$CAPACITY_QUALITY_REFERENCE_RTMP_BPS"
            fi
            [ "$quality_status" = yes ] || quality_failed=1
        else
            capacity_rtmp_payload_report "$case_dir/rtmp-receiver.before" \
                "$case_dir/rtmp-receiver.after" "$programs" "$rtmp_destinations" \
                "$srt_destinations" > "$case_dir/rtmp-receiver-report.txt" \
                || return 1
            echo "   receiver-delivered RTMP payload bytes (per destination):"
            cat "$case_dir/rtmp-receiver-report.txt"
        fi
    fi
    if [ "$hls_readers" -gt 0 ]; then
        [ -s "$hls_report_file" ] \
            || { cat "$hls_log" >&2; echo "HLS reader quality report is missing" >&2; return 1; }
        cat "$hls_log"
        echo "   hls_destination_quality_csv=$hls_report_file"
        quality_status="$(awk -F= '$1 == "quality_pass" { print $2; exit }' \
            "$hls_log")"
        case "$quality_status" in
            yes|no) ;;
            *) echo "HLS quality analyzer returned no status" >&2; return 1 ;;
        esac
        hls_reference_bps="${CAPACITY_QUALITY_REFERENCE_HLS_BPS:-0}"
        if [ "$hls_reference_bps" = 0 ]; then
            CAPACITY_QUALITY_REFERENCE_HLS_BPS="$(awk -F= \
                '$1 == "reference_bps" { print $2; exit }' "$hls_log")"
            [[ "$CAPACITY_QUALITY_REFERENCE_HLS_BPS" =~ ^[0-9]+([.][0-9]+)?$ ]] \
                && awk -v b="$CAPACITY_QUALITY_REFERENCE_HLS_BPS" \
                    'BEGIN { exit !(b > 0) }' \
                || { echo "HLS quality analyzer returned an invalid reference rate" \
                     >&2; return 1; }
            echo "   calibrated_hls_quality_reference_bps=$CAPACITY_QUALITY_REFERENCE_HLS_BPS"
        fi
        [ "$quality_status" = yes ] || quality_failed=1
    fi
    if [ "$hls_push_destinations" -gt 0 ]; then
        hls_push_quality_report="$case_dir/hls-push-quality.txt"
        hls_push_reference_bps="${CAPACITY_QUALITY_REFERENCE_HLS_PUSH_BPS:-0}"
        python3 "$ROOT/tests/bench/hls_push_capacity.py" report \
            --before-prefix "$case_dir/workers.before" \
            --after-prefix "$case_dir/workers.after" \
            --stream "${names[0]}" \
            --destination-offset "$((srt_destinations + rtmp_destinations))" \
            --destinations "$hls_push_destinations" \
            --sink-before "$case_dir/hls-push.before.json" \
            --sink-after "$case_dir/hls-push.after.json" \
            --reference-bps "$hls_push_reference_bps" \
            --min-delivery-ratio "$CAPACITY_QUALITY_MIN_DELIVERY_RATIO" \
            --report "$case_dir/hls-push-quality.csv" \
            >"$hls_push_quality_report" || return 1
        cat "$hls_push_quality_report"
        echo "   hls_push_destination_quality_csv=$case_dir/hls-push-quality.csv"
        quality_status="$(awk -F= '$1 == "quality_pass" { print $2; exit }' \
            "$hls_push_quality_report")"
        case "$quality_status" in
            yes|no) ;;
            *) echo "HLS push quality analyzer returned no status" >&2; return 1 ;;
        esac
        if [ "$hls_push_reference_bps" = 0 ]; then
            CAPACITY_QUALITY_REFERENCE_HLS_PUSH_BPS="$(awk -F= \
                '$1 == "quality_reference_payload_bps" { print $2; exit }' \
                "$hls_push_quality_report")"
            [[ "$CAPACITY_QUALITY_REFERENCE_HLS_PUSH_BPS" =~ ^[0-9]+([.][0-9]+)?$ ]] \
                && awk -v b="$CAPACITY_QUALITY_REFERENCE_HLS_PUSH_BPS" \
                    'BEGIN { exit !(b > 0) }' \
                || { echo "HLS push quality analyzer returned an invalid reference rate" \
                     >&2; return 1; }
            echo "   calibrated_hls_push_quality_reference_bps=$CAPACITY_QUALITY_REFERENCE_HLS_PUSH_BPS"
        fi
        [ "$quality_status" = yes ] || quality_failed=1
    fi

    if [ "$srt_destinations" -gt 0 ]; then
        echo "   SRT sender-shard counters and queue snapshots:"
        capacity_srt_metrics_report "$case_dir/workers.before" \
            "$case_dir/workers.after" > "$case_dir/srt-shards.txt" \
            || return 1
        cat "$case_dir/srt-shards.txt"
        if [ -n "$stall_id" ]; then
            slow_drops="$(awk '
                {
                    for (i = 1; i <= NF; i++) {
                        if ($i ~ /^output_drops_delta=/) {
                            split($i, field, "=")
                            total += field[2]
                        }
                    }
                }
                END { print total + 0 }
            ' "$case_dir/srt-shards.txt")"
            [ "$slow_drops" -gt 0 ] \
                || { echo "slow SRT reader caused no destination-queue drops" \
                         >&2; return 1; }
            echo "   slow-reader destination queue drops=$slow_drops"
        fi
        echo "   sender shard CPU from /proc thread comm srt-egress-*:"
        report_cpu "$W_BEFORE" "$W_AFTER" "$W_DUR" \
            | awk '$2 ~ /^srt-egress-/ {
                       printf "      worker=%s thread=%s cpu=%.1f%%\n", $1, $2, $3
                       worker[$1] += $3
                       total += $3
                       count++
                   }
                   END {
                       if (!count) print "      no srt-egress-* thread rows"
                       for (slot in worker) {
                           printf "      worker=%s aggregate_egress_shard_cpu=%.1f%%\n",
                                  slot, worker[slot]
                       }
                       printf "      aggregate_egress_shard_cpu=%.1f%%\n", total
                   }'
        echo "   libsrt SRT:SndQ CPU:"
        report_cpu "$W_BEFORE" "$W_AFTER" "$W_DUR" \
            | awk '$2 == "SRT:SndQ" {
                       printf "      worker=%s cpu=%.1f%%\n", $1, $3
                       worker[$1] += $3
                       total += $3
                       count++
                   }
                   END {
                       if (!count) print "      no SRT:SndQ thread rows"
                       for (slot in worker) {
                           printf "      worker=%s aggregate_libsrt_sndq_cpu=%.1f%%\n",
                                  slot, worker[slot]
                       }
                       printf "      aggregate_libsrt_sndq_cpu=%.1f%%\n", total
                   }'
        echo "   shared SRT receiver thread CPU:"
        report_cpu "$W_BEFORE" "$W_AFTER" "$W_DUR" \
            | awk -v pid="pid$sink_pid" '
                $1 == pid {
                    printf "      pid=%s thread=%s cpu=%.1f%%\n", pid, $2, $3
                    total += $3
                    count++
                }
                END {
                    if (!count) print "      no receiver thread rows"
                    else printf "      aggregate_receiver_cpu=%.1f%%\n", total
                }'
    fi
    if [ "$hls_readers" -gt 0 ]; then
        echo "   HLS reader process CPU:"
        report_cpu "$W_BEFORE" "$W_AFTER" "$W_DUR" \
            | awk -v pid="pid$hls_pid" '
                $1 == pid {
                    printf "      pid=%s thread=%s cpu=%.1f%%\n", pid, $2, $3
                    total += $3
                    count++
                }
                END {
                    if (!count) print "      no HLS reader thread rows"
                    else printf "      aggregate_hls_reader_cpu=%.1f%%\n", total
                }'
    fi
    if [ -n "$hls_push_sink_pid" ]; then
        echo "   HLS push sink process CPU:"
        report_cpu "$W_BEFORE" "$W_AFTER" "$W_DUR" \
            | awk -v pid="pid$hls_push_sink_pid" '
                $1 == pid {
                    printf "      pid=%s thread=%s cpu=%.1f%%\n", pid, $2, $3
                    total += $3
                    count++
                }
                END {
                    if (!count) print "      no HLS push sink process rows"
                    else printf "      aggregate_hls_push_sink_cpu=%.1f%%\n", total
                }'
    fi
    echo "   worker process RSS/PSS from smaps_rollup:"
    capacity_memory_report "$case_dir/memory.before" "$case_dir/memory.after"


    echo "   worker visit and scheduling metrics:"
    worker_cpu_total=0
    for worker_pid in $(worker_pids); do
        worker_slot="$(slot_name "$worker_pid")"
        worker_before="$case_dir/workers.before.$worker_pid"
        worker_after="$case_dir/workers.after.$worker_pid"
        cpu_pct="$(capacity_worker_cpu "$W_BEFORE" "$W_AFTER" "$W_DUR" \
            "$worker_slot")"
        worker_cpu_total="$(awk -v a="$worker_cpu_total" -v b="$cpu_pct" \
            'BEGIN { printf "%.1f", a + b }')"
        budget_before="$(capacity_metric "$worker_before" \
            nginx_media_worker_budget_reposts_total)"
        budget_after="$(capacity_metric "$worker_after" \
            nginx_media_worker_budget_reposts_total)"
        visits_before="$(capacity_metric "$worker_before" \
            nginx_media_worker_periodic_visits_total)"
        visits_after="$(capacity_metric "$worker_after" \
            nginx_media_worker_periodic_visits_total)"
        late_before="$(capacity_metric "$worker_before" \
            nginx_media_worker_late_ticks_total)"
        late_after="$(capacity_metric "$worker_after" \
            nginx_media_worker_late_ticks_total)"
        max_service="$(capacity_metric "$worker_after" \
            nginx_media_worker_max_service_ms)"
        event_delay="$(capacity_metric "$worker_after" \
            nginx_media_worker_event_loop_max_delay_ms)"
        echo "      $worker_slot pid=$worker_pid cpu=${cpu_pct}%"
        echo "         visit_max=${max_service:-0}ms event_loop_max_delay=${event_delay:-0}ms"
        echo "         periodic_visits_delta=$(( ${visits_after:-0} - ${visits_before:-0} ))"
        echo "         budget_reposts_delta=$(( ${budget_after:-0} - ${budget_before:-0} ))"
        echo "         late_ticks_delta=$(( ${late_after:-0} - ${late_before:-0} ))"
    done
    echo "      aggregate_worker_cpu=${worker_cpu_total}% of one core"
    if [ -n "$RTMP_SINK_WORKER_PID" ]; then
        echo "      RTMP sink CPU=$(capacity_worker_cpu "$W_BEFORE" "$W_AFTER" \
            "$W_DUR" wrtmp-sink)% of one core (separate receiver worker)"
    fi

    echo "   NGINX socket skmem (ss -m):"
    for worker_pid in $(worker_pids); do
        worker_slot="$(slot_name "$worker_pid")"
        socket_before="$(awk -v pid="$worker_pid" '$1 == pid { print $7; exit }' \
            "$case_dir/before")"
        socket_after="$(awk -v pid="$worker_pid" '$1 == pid { print $7; exit }' \
            "$case_dir/after")"
        [ -n "$socket_before" ] || socket_before=0
        [ -n "$socket_after" ] || socket_after=0
        socket_row="$(awk -v pid="$worker_pid" '$1 == pid { print $6; exit }' \
            "$case_dir/after")"
        [ -n "$socket_row" ] || socket_row=0
        echo "      $worker_slot sockets=$socket_row skmem_bytes=$socket_after delta_bytes=$(( socket_after - socket_before ))"
    done
    sock_tcp_before="$(capacity_sockstat_value "$case_dir/before.sockstat" TCP: mem)"
    sock_tcp_after="$(capacity_sockstat_value "$case_dir/after.sockstat" TCP: mem)"
    sock_udp_before="$(capacity_sockstat_value "$case_dir/before.sockstat" UDP: mem)"
    sock_udp_after="$(capacity_sockstat_value "$case_dir/after.sockstat" UDP: mem)"
    tcp_inuse_before="$(capacity_sockstat_value "$case_dir/before.sockstat" TCP: inuse)"
    tcp_inuse_after="$(capacity_sockstat_value "$case_dir/after.sockstat" TCP: inuse)"
    udp_inuse_before="$(capacity_sockstat_value "$case_dir/before.sockstat" UDP: inuse)"
    udp_inuse_after="$(capacity_sockstat_value "$case_dir/after.sockstat" UDP: inuse)"
    echo "   host /proc/net/sockstat (global, not process-attributed):"
    echo "      TCP mem_pages=${sock_tcp_before:-0}->${sock_tcp_after:-0}"
    echo "      UDP mem_pages=${sock_udp_before:-0}->${sock_udp_after:-0}"
    echo "      TCP inuse=${tcp_inuse_before:-0}->${tcp_inuse_after:-0}"
    echo "      UDP inuse=${udp_inuse_before:-0}->${udp_inuse_after:-0}"

    kill_pubs
    for receiver_pid in "${SINKS[@]}"; do kill_one "$receiver_pid"; done
    SINKS=()
    stop_instance
    stop_rtmp_sink_instance
    if [ "$quality_mode" = yes ] && [ "$quality_failed" -ne 0 ]; then
        return 2
    fi
}

phase_capacity() {
    local programs rate destinations

    echo
    echo "== offered-load capacity curve (fixed worker count)"
    echo "   program and bitrate cases use one shared SRT receiver per case."
    echo "   The destination ladder runs every exact rung over SRT and RTMP;"
    echo "   RTMP source uses one worker and a separate no-HLS receiver."
    echo "   Publishers and outputs warm up before the timed baseline."

    for programs in $CAPACITY_PROGRAM_STEPS; do
        capacity_case "programs-$programs" "$programs" \
            "$CAPACITY_PROGRAM_RATE" "$CAPACITY_PROGRAM_DESTS" \
            "$CAPACITY_WINDOW" || return 1
    done

    for rate in $CAPACITY_BITRATE_STEPS; do
        capacity_case "bitrate-$rate" "$CAPACITY_BITRATE_PROGRAMS" "$rate" \
            "$CAPACITY_BITRATE_DESTS" "$CAPACITY_WINDOW" || return 1
    done

    for destinations in $CAPACITY_DEST_STEPS; do
        capacity_case "destinations-$destinations-srt" \
            1 "$CAPACITY_DEST_RATE" "$destinations" \
            "$CAPACITY_WINDOW" srt || return 1
        capacity_case "destinations-$destinations-rtmp" \
            1 "$CAPACITY_DEST_RATE" "$destinations" \
            "$CAPACITY_WINDOW" rtmp || return 1
    done

    echo "   slow-reader: rate=$CAPACITY_SLOW_RATE window=${CAPACITY_SLOW_SECONDS}s"
    capacity_case "slow-reader-isolation" 1 "$CAPACITY_SLOW_RATE" 4 \
        "$CAPACITY_SLOW_SECONDS" srt d0000 || return 1
    capacity_case "saturated-multiprogram" 4 6M 250 \
        "$CAPACITY_SATURATED_SECONDS" srt || return 1
    capacity_case "sustained" "$CAPACITY_SUSTAINED_PROGRAMS" \
        "$CAPACITY_SUSTAINED_RATE" "$CAPACITY_SUSTAINED_DESTS" \
        "$CAPACITY_SUSTAINED_SECONDS"
}

phase_capacity_saturated() {
    capacity_case "saturated-multiprogram" 4 6M 250 \
        "$CAPACITY_SATURATED_SECONDS" srt
}

phase_capacity_slow_reader() {
    echo "   slow-reader: rate=$CAPACITY_SLOW_RATE window=${CAPACITY_SLOW_SECONDS}s"
    capacity_case "slow-reader-isolation" 1 "$CAPACITY_SLOW_RATE" 4 \
        "$CAPACITY_SLOW_SECONDS" srt d0000
}

phase_capacity_sustained() {
    capacity_case "sustained" "$CAPACITY_SUSTAINED_PROGRAMS" \
        "$CAPACITY_SUSTAINED_RATE" "$CAPACITY_SUSTAINED_DESTS" \
        "$CAPACITY_SUSTAINED_SECONDS"
}

phase_capacity_srt_ladder() {
    local destinations

    echo
    echo "== single-program SRT destination ladder"
    for destinations in $CAPACITY_DEST_STEPS; do
        capacity_case "destinations-$destinations-srt" \
            1 "$CAPACITY_DEST_RATE" "$destinations" \
            "$CAPACITY_WINDOW" srt || return 1
    done
}

capacity_quality_ladder() {   # <mix> <primary protocol> <SRT share> [HLS push share]
    local mix="$1" protocol="$2" srt_share="$3" hls_push_share="${4:-0}"
    local destinations previous=0 status failures=0 first_failure="" failed_rungs=""
    local srt_count primary_count rtmp_count hls_count hls_push_count
    local actual_srt_share actual_rtmp_share actual_hls_push_share

    echo
    echo "== $mix ${CAPACITY_QUALITY_RATE} receiver-quality ladder"
    echo "   duration=${CAPACITY_QUALITY_SECONDS}s per rung; reference=one destination per protocol"
    echo "   SRT counts use nearest-integer share rounding; actual counts shown per rung."
    echo "   pass: per-destination delivery >=${CAPACITY_QUALITY_MIN_DELIVERY_RATIO} of its protocol reference"
    if [ "$protocol" = srt ]; then
        echo "   SRT quality also requires interval floor=${CAPACITY_QUALITY_INTERVAL_FLOOR},"
        echo "   lag limit=${CAPACITY_QUALITY_MAX_LOW_SECONDS}s, queue pressure limit="
        echo "   ${CAPACITY_QUALITY_QUEUE_PRESSURE} for ${CAPACITY_QUALITY_PRESSURE_SAMPLES} samples,"
        echo "   and zero MPEG-TS errors/application drops."
    fi
    for destinations in $CAPACITY_QUALITY_STEPS; do
        [[ "$destinations" =~ ^[1-9][0-9]*$ ]] \
            && [ "$destinations" -le 1000 ] \
            && [ "$destinations" -gt "$previous" ] \
            || { echo "quality ladder must be strictly increasing within 1..1000" \
                     >&2; return 1; }
        if [ "$previous" -eq 0 ] && [ "$destinations" -ne 1 ]; then
            echo "quality ladder must begin with one destination for calibration" >&2
            return 1
        fi
        previous="$destinations"
        if [ "$protocol" = srt ]; then
            srt_count="$destinations"
        else
            srt_count=$(( (destinations * srt_share + 50) / 100 ))
        fi
        primary_count=$(( destinations - srt_count ))
        rtmp_count=0
        hls_count=0
        hls_push_count=0
        case "$protocol" in
            rtmp) rtmp_count="$primary_count" ;;
            hls) hls_count="$primary_count" ;;
            hls-push) hls_push_count="$primary_count" ;;
            rtmp-hls-push)
                hls_push_count=$(( (destinations * hls_push_share + 50) / 100 ))
                rtmp_count=$(( primary_count - hls_push_count ))
                ;;
        esac
        actual_srt_share="$(awk -v s="$srt_count" -v d="$destinations" \
            'BEGIN { printf "%.2f", 100 * s / d }')"
        actual_rtmp_share="$(awk -v s="$rtmp_count" -v d="$destinations" \
            'BEGIN { printf "%.2f", 100 * s / d }')"
        actual_hls_push_share="$(awk -v s="$hls_push_count" -v d="$destinations" \
            'BEGIN { printf "%.2f", 100 * s / d }')"
        echo "quality_rung_start mix=$mix destinations=$destinations srt=$srt_count rtmp=$rtmp_count hls=$hls_count hls_push=$hls_push_count actual_shares_srt=${actual_srt_share}%_rtmp=${actual_rtmp_share}%_hls_push=${actual_hls_push_share}%"
        if capacity_case "quality-$mix-$destinations" 1 \
            "$CAPACITY_QUALITY_RATE" "$destinations" \
            "$CAPACITY_QUALITY_SECONDS" "$protocol" "" yes \
            "$srt_share" "$hls_push_share"
        then
            echo "quality_rung_result mix=$mix destinations=$destinations srt=$srt_count rtmp=$rtmp_count hls=$hls_count hls_push=$hls_push_count result=pass"
        else
            status="$?"
            if [ "$status" -ne 2 ]; then
                echo "quality_rung_result mix=$mix destinations=$destinations result=setup-error status=$status"
                echo "mix=$mix first_failing_destination_rung=unmeasured"
                echo "quality_ladder_aborted_at=$destinations"
                return "$status"
            fi
            echo "quality_rung_result mix=$mix destinations=$destinations srt=$srt_count rtmp=$rtmp_count hls=$hls_count hls_push=$hls_push_count result=fail"
            failures=$(( failures + 1 ))
            [ -n "$first_failure" ] || first_failure="$destinations"
            [ -z "$failed_rungs" ] || failed_rungs+=" "
            failed_rungs+="$destinations"
        fi
    done
    if [ "$failures" -gt 0 ]; then
        echo "mix=$mix first_failing_destination_rung=$first_failure"
        echo "mix=$mix quality_failed_rungs=$failed_rungs"
        return 2
    fi
    echo "mix=$mix first_failing_destination_rung=none"
    echo "mix=$mix quality_failed_rungs=none"
}

phase_capacity_quality_ladder() {
    local entry mix protocol srt_share hls_push_share status failures=0 failed_mixes=""
    local requested found
    local -a mix_specs=(
        pure-srt:srt:0
        pure-rtmp:rtmp:0
        pure-hls:hls:0
        pure-hls-push:hls-push:0
        rtmp-95-srt-5:rtmp:5
        hls-push-95-srt-5:hls-push:5
        rtmp-50-hls-push-45-srt-5:rtmp-hls-push:5:45
    )
    local -a requested_mixes=() selected_mix_specs=()

    [ -n "$CAPACITY_QUALITY_STEPS" ] \
        || { echo "CAPACITY_QUALITY_STEPS must not be empty" >&2; return 1; }
    if [ "$CAPACITY_QUALITY_MIXES" != all ]; then
        read -r -a requested_mixes <<< "$CAPACITY_QUALITY_MIXES"
        [ "${#requested_mixes[@]}" -gt 0 ] \
            || { echo "CAPACITY_QUALITY_MIXES must not be empty" >&2; return 1; }
        for requested in "${requested_mixes[@]}"; do
            case "$requested" in
                pure-srt|pure-rtmp|pure-hls|pure-hls-push|rtmp-95-srt-5|hls-push-95-srt-5|rtmp-50-hls-push-45-srt-5)
                    ;;
                *)
                    echo "unknown capacity quality mix: $requested" >&2
                    return 1
                    ;;
            esac
            found=0
            for entry in "${mix_specs[@]}"; do
                if [ "${entry%%:*}" = "$requested" ]; then
                    selected_mix_specs+=( "$entry" )
                    found=1
                    break
                fi
            done
            [ "$found" -eq 1 ] \
                || { echo "unknown capacity quality mix: $requested" >&2; return 1; }
        done
        mix_specs=( "${selected_mix_specs[@]}" )
    fi

    if [ "$CAPACITY_QUALITY_MIXES" = all ]; then
        echo
        echo "== seven 8 Mbit/s delivery-quality ladders"
        echo "   each rung uses one program; per-protocol one-destination baselines calibrate before scaling."
        echo "   mix order: pure SRT, pure RTMP, pure HLS readers, pure HLS push, 95% RTMP/5% SRT, 95% HLS push/5% SRT, and RTMP/HLS push/SRT."
    else
        echo
        echo "== selected 8 Mbit/s delivery-quality ladders"
        echo "   selected mixes: $CAPACITY_QUALITY_MIXES"
        echo "   each rung uses one program; per-protocol one-destination baselines calibrate before scaling."
    fi
    for entry in "${mix_specs[@]}"; do
        IFS=: read -r mix protocol srt_share hls_push_share <<< "$entry"
        if capacity_quality_ladder "$mix" "$protocol" "$srt_share" "$hls_push_share"; then
            :
        else
            status="$?"
            if [ "$status" -ne 2 ]; then
                return "$status"
            fi
            failures=$(( failures + 1 ))
            [ -n "$failed_mixes" ] || failed_mixes="$mix"
            [[ " $failed_mixes " == *" $mix "* ]] \
                || failed_mixes+=" $mix"
        fi
    done
    if [ "$failures" -gt 0 ]; then
        echo "quality_ladders_with_failures=$failed_mixes"
        return 1
    fi
    echo "quality_ladders_with_failures=none"
}

# --- run --------------------------------------------------------------------

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/media"

for p in $(pgrep -f "nginx: master process .* -p $RUN" 2>/dev/null || true); do
    echo "   stopping a leftover master $p from this RUN"
    for c in $(pgrep -P "$p" 2>/dev/null); do kill -KILL "$c" 2>/dev/null; done
    kill -KILL "$p" 2>/dev/null
done

echo "== host"
echo "   $(uname -srm), $(nproc) cpus"
echo "   net.core.rmem_max $(sysctl -n net.core.rmem_max 2>/dev/null || echo '?')"
echo "   nginx $(basename "$NGINX") built $(stat -c %y "$NGINX" | cut -d. -f1)"
echo

needs_lo_source=0
for phase in $PHASES; do
    case "$phase" in
        capacity-quality-ladder) ;;
        *) needs_lo_source=1; break ;;
    esac
done
if [ "$needs_lo_source" -eq 1 ]; then
    make_source "$RUN/media/lo.ts" "$LO_BITRATE"
fi

for phase in $PHASES; do
    case "$phase" in
        floor)    phase_floor ;;
        ingest)
            phase_ingest_high
            phase_ingest_publishers
            phase_ingest_endpoints
            phase_ingest_one_port
            ;;
        egress)
            phase_egress_hls
            phase_egress_live
            ;;
        misplace) phase_misplace ;;
        topology) phase_topology ;;
        capacity) phase_capacity || exit 1 ;;
        capacity-srt-ladder) phase_capacity_srt_ladder || exit 1 ;;
        capacity-quality-ladder) phase_capacity_quality_ladder || exit 1 ;;
        capacity-saturated) phase_capacity_saturated || exit 1 ;;
        capacity-slow-reader) phase_capacity_slow_reader || exit 1 ;;
        capacity-sustained) phase_capacity_sustained || exit 1 ;;
    esac
done

trap - EXIT
cleanup

echo
echo "== conditions"
echo "   source:     pre-encoded 720p25 mpegts, publishers use -c copy"
echo "   endpoints:  one media_srt_listen per worker, worker i binds entry i"
echo "   api:        listen ... reuseport; mutations retried until the owner"
echo "               answers, metrics read on the owner's own connection"
echo "   throughput: chunks/s and bytes/s are the workers' own drained totals"
echo "   cpu:        per thread, percent of one core, over the measured window"
if [[ " $PHASES " == *" capacity "* \
   || " $PHASES " == *" capacity-srt-ladder "* \
   || " $PHASES " == *" capacity-quality-ladder "* \
   || " $PHASES " == *" capacity-saturated "* \
   || " $PHASES " == *" capacity-slow-reader "* \
   || " $PHASES " == *" capacity-sustained "* ]]; then
    echo "   receivers:  separate SRT/RTMP sinks, HLS readers and HLS PUT sink"
    echo "   receiver CPU: sampled for external SRT, RTMP and HLS sink workers"
fi
echo "   labels:     SRT:* from comm; this module's threads from a stack" \
     "backtrace ($([ "$LABELLED" = 1 ] && echo 'attached and labelled' \
                                || echo 'NOT AVAILABLE: comm names only'))"
echo "   host:       single host, no netem, $(nproc) cpus"
echo "== ingest/egress fanout done"

#!/usr/bin/env bash
#
# RTMP ingest is accepted by every worker, not funnelled through worker 0.
#
# The listener is opened by every worker with SO_REUSEPORT, so the kernel
# hashes each arriving connection onto one of them (goal doc 22: transport
# socket ownership may differ from program ownership, and internal routing is
# the escape hatch, not the path).  What that buys is visible per publisher:
# the worker that accepted a connection is the pid on the log line recording
# the publish, and with four workers the probes below must land on more than
# one of them.  Before the socket was shared every one of those lines carried
# worker 0's pid, because every publisher connected to worker 0 and worker 0
# carried all of RTMP.
#
# Two more things are asserted on the same run, because sharing the listener is
# only an improvement if it costs nothing:
#
#   * a publisher that lands on a worker which does not own its program is
#     still routed to the owner, the owner opens the routed source, and the
#     program the owner reports is fed by it;
#   * a reload binds the new workers' listeners alongside the old ones instead
#     of waiting for the port, and every worker still exits cleanly.
#
# The distribution half is the regression guard: revert the listener to worker
# 0 alone and the first assertion fails, because every publisher is accepted by
# worker 0 again.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/rtmp-workers"
BASE=$(( 19900 + ($$ % 80) * 4 ))
RTMP_PORT="${RW_RTMP_PORT:-$BASE}"
HTTP_PORT="${RW_HTTP_PORT:-$(( BASE + 1 ))}"
WORKERS="${RW_WORKERS:-4}"
# Enough connections that an unlucky hash cannot fake "one worker": at four
# workers, every probe landing on the same one has probability 4^-15.
PROBES="${RW_PROBES:-16}"
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
LOG="$RUN/logs/error.log"
PUB=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes $WORKERS;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_rtmp_listen 127.0.0.1:$RTMP_PORT;

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
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

fail() { echo "$*" >&2; exit 1; }

log_count() { grep -c "$1" "$LOG" 2>/dev/null || true; }

wait_for_log() {  # <regex> <count> <description>
    local i count
    for i in $(seq 1 300); do
        count="$(log_count "$1")"
        [ "${count:-0}" -ge "$2" ] && return 0
        sleep 0.1
    done
    fail "$3"
}

# the line number of the nth line matching <regex>, or the empty string
log_line() {  # <regex> <n>
    grep -n "$1" "$LOG" 2>/dev/null | sed -n "$2p" | cut -d: -f1 || true
}

# nginx writes the pid in front of every error_log line, so a pid identifies
# the worker that wrote it.
line_pid() {
    sed -nE 's@.*\] ([0-9]+)#[0-9]+: .*@\1@p' <<<"$1"
}

# The line that records a publish, and with it the worker that accepted the
# publisher: the owner itself when it owns the program, and the accepting
# worker when the publisher had to be routed.  The routed form ends at the
# stream name, the local form continues with the source count, so the match
# ends at a space or the end of the line rather than assuming one.
publish_line() {  # <name>
    grep -m1 -E "media: rtmp publisher (routed to the owner )?stream=live/$1( |$)" \
        "$LOG" 2>/dev/null || true
}

worker_pid() {  # <index>
    sed -nE "s@.*\] ([0-9]+)#[0-9]+: media: worker $1 routing ready.*@\1@p" \
        "$LOG" | head -1
}

# worker index -> pid, or -1 when the slot did not report itself
worker_index_of_pid() {  # <pid>
    local i pid
    for i in $(seq 0 $(( WORKERS - 1 ))); do
        pid="$(worker_pid "$i")"
        [ "$pid" = "$1" ] && { printf '%s' "$i"; return; }
    done
    printf '%s' "-1"
}

# one publisher, one connection: ffmpeg completes the handshake, sends
# connect/createStream/publish and then audio, which is all the log line and
# the program need.  -re paces it so the connection stays up for <seconds>.
publish() {  # <name> <seconds>
    if timeout 30 ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "sine=frequency=440:sample_rate=48000" \
        -c:a aac -b:a 32k \
        -t "$2" -f flv "rtmp://127.0.0.1:$RTMP_PORT/live/$1" \
        >"$RUN/pub-$1.log" 2>&1
    then
        return 0
    fi

    # A few of the older test scripts on this host clean up with a global
    # `pkill -f 'nginx: '`, which is a silent SIGKILL of every nginx on the
    # box - including this one, if they run at the same time.  That is not a
    # listener failure and the log says nothing about it, so say it here.
    if [ -f "$RUN/logs/nginx.pid" ] \
        && ! kill -0 "$(cat "$RUN/logs/nginx.pid")" 2>/dev/null
    then
        fail "nginx was killed while live/$1 was published: another process \
on this host killed it, which is not a failure of the listener"
    fi

    fail "ffmpeg could not publish live/$1; see $RUN/pub-$1.log"
}

publish_bg() {  # <name> <seconds>
    publish "$1" "$2" &
    PUB=$!
}

started_pids() {  # <count>: the first <count> worker pids the master forked
    sed -nE 's@.*\] [0-9]+#[0-9]+: start worker process ([0-9]+).*@\1@p' \
        "$LOG" | head -n "$1"
}

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx with $WORKERS workers"
"$NGINX" -p "$RUN" -c conf/nginx.conf

wait_for_log 'media: rtmp listener ready' 1 \
    "no worker opened the rtmp listener"
wait_for_log "media: routing socket pairs created for $WORKERS workers" 1 \
    "the routing pairs were not created"
wait_for_log "media: worker $(( WORKERS - 1 )) routing ready" 1 \
    "the last worker did not adopt its routing endpoints"

# --- the listener is shared: publishers land on more than one worker --------

echo "== $PROBES publishers, one connection each"
ACCEPTED=""
for i in $(seq -w 1 "$PROBES"); do
    publish "p$i" 1

    LINE="$(publish_line "p$i")"
    [ -n "$LINE" ] || fail "live/p$i was published but no worker recorded it"

    ACCEPTED="$ACCEPTED $(line_pid "$LINE")"
done

DISTRIBUTION="$(printf '%s\n' $ACCEPTED | sort | uniq -c | sort -k2 -n)"

echo "   publishers accepted per worker pid:"
printf '%s\n' "$DISTRIBUTION" | while read -r n pid; do
    echo "     worker $(worker_index_of_pid "$pid") (pid $pid): $n"
done

DISTINCT="$(printf '%s\n' $ACCEPTED | sort -u | grep -c . || true)"
OWNED="$(printf '%s\n' $ACCEPTED | sed -n '1p')"
[ -n "$OWNED" ] || fail "no publisher was recorded at all"

[ "$DISTINCT" -ge 2 ] \
    || fail "all $PROBES publishers were accepted by one worker (pid $OWNED): the \
rtmp listener is not shared, so worker 0 carries every RTMP publisher"

echo "   accepted by $DISTINCT of $WORKERS workers"

# --- a publisher away from its owner is routed to it -----------------------

echo "== a publisher that lands away from its program's owner"
ROUTED=""
for attempt in $(seq 1 "$(( PROBES + 8 ))"); do
    NAME="route$attempt"
    publish_bg "$NAME" 6

    LINE=""
    for _ in $(seq 1 100); do
        LINE="$(publish_line "$NAME")"
        [ -n "$LINE" ] && break
        sleep 0.1
    done

    [ -n "$LINE" ] || fail "live/$NAME was published but no worker recorded it"

    if grep -q "routed to the owner" <<<"$LINE"; then
        ROUTED="$NAME"
        break
    fi

    # this one landed on the owner: it proves nothing about routing, so try
    # again on a fresh connection (and a fresh name, so the lines do not mix)
    kill -KILL "$PUB" 2>/dev/null || true
    wait "$PUB" 2>/dev/null || true
    PUB=0
done

[ -n "$ROUTED" ] \
    || fail "no publisher landed away from its program's owner: publisher \
placement is not spread over the workers"

ACCEPT_PID="$(line_pid "$(publish_line "$ROUTED")")"

# the OPEN crosses the inter-worker transport, so the owner's line is written a
# moment after the accepting worker's, by another process
OWNER_LINE=""
for _ in $(seq 1 100); do
    OWNER_LINE="$(grep -m1 "media: routed source opened stream=live/$ROUTED" \
        "$LOG" || true)"
    [ -n "$OWNER_LINE" ] && break
    sleep 0.1
done

[ -n "$OWNER_LINE" ] \
    || fail "live/$ROUTED was routed but the owner never opened the routed source"
OWNER_PID="$(line_pid "$OWNER_LINE")"

[ "$ACCEPT_PID" != "$OWNER_PID" ] \
    || fail "live/$ROUTED was recorded as routed but accepted by the owner anyway"

OWNER_INDEX="$(worker_index_of_pid "$OWNER_PID")"
[ "$OWNER_INDEX" != "-1" ] \
    || fail "the routed source was opened by pid $OWNER_PID, not a worker"

echo "   live/$ROUTED accepted by worker $(worker_index_of_pid "$ACCEPT_PID") \
(pid $ACCEPT_PID), opened as a routed source by worker $OWNER_INDEX (pid $OWNER_PID)"

# the owner's program is fed by the routed publisher, not by a local one: the
# API reports the owner's own numbers wherever the read lands
FRAMES=0
for _ in $(seq 1 100); do
    FRAMES="$(curl -fsS "$API/streams/live/$ROUTED" 2>/dev/null \
        | grep -o '"program_frames":[0-9]*' | head -1 | cut -d: -f2 \
        || true)"
    [ "${FRAMES:-0}" -gt 0 ] && break
    sleep 0.1
done

[ "${FRAMES:-0}" -gt 0 ] \
    || fail "live/$ROUTED was routed to the owner but its program carried no frames"

REPORTED_OWNER="$(curl -fsS "$API/streams/live/$ROUTED" 2>/dev/null \
    | grep -o '"owner":[0-9]*' | head -1 | cut -d: -f2 || true)"
[ "${REPORTED_OWNER:-none}" = "$OWNER_INDEX" ] \
    || fail "the graph reports owner $REPORTED_OWNER for live/$ROUTED, but the \
routed source was opened by worker $OWNER_INDEX"

echo "   the owner's program carried $FRAMES frames from the routed publisher"

kill -KILL "$PUB" 2>/dev/null || true
wait "$PUB" 2>/dev/null || true
PUB=0

# --- reload and shutdown with one listener per worker ----------------------

echo "== reload"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s reload

wait_for_log 'start worker process' "$(( WORKERS * 2 ))" \
    "the reload did not start a second generation of workers"
wait_for_log 'media: rtmp listener ready' "$(( WORKERS * 2 ))" \
    "the new workers did not open the rtmp listener"
wait_for_log 'media: rtmp listener .* is shared with every worker' \
    "$(( WORKERS * 2 ))" \
    "the new workers did not share the rtmp listener"

grep -q 'media: rtmp listener .* is still bound; retrying' "$LOG" \
    && fail "a worker had to wait for the port on reload: the listener is not \
being bound alongside the one it replaces"

# The overlap itself, not just its effect: the new generation was listening
# before any replaced worker released its copy of the socket.  nginx starts the
# new workers before it signals the old ones, so this ordering is what a shared
# listener produces and a port-contended one cannot.
READY_LINE="$(log_line 'media: rtmp listener ready' "$(( WORKERS * 2 ))")"

RELEASE_LINE=""
for _ in $(seq 1 200); do
    RELEASE_LINE="$(log_line 'media: releasing the rtmp listener' 1)"
    [ -n "$RELEASE_LINE" ] && break
    sleep 0.05
done

if [ -z "$RELEASE_LINE" ]; then
    fail "no worker released its listener during the reload"
fi

[ "$READY_LINE" -lt "$RELEASE_LINE" ] \
    || fail "the new workers were still binding when the replaced workers let \
go of the port: the listener was not shared across the reload"

# A publisher that arrives while a leaving worker still has its socket bound
# can be hashed onto it and reset when it closes - nginx has the same window on
# reload, and closing the listener before bind would only move it - so the
# publisher below waits for the replaced generation to be gone instead of
# measuring that race.
GONE=0
for _ in $(seq 1 200); do
    GONE=1
    for pid in $(started_pids "$WORKERS"); do
        kill -0 "$pid" 2>/dev/null && GONE=0
    done
    [ "$GONE" = "1" ] && break
    sleep 0.05
done

[ "$GONE" = "1" ] || fail "the replaced workers were still running after the \
reload"

publish "reloaded" 1

RL_PID="$(line_pid "$(publish_line "reloaded")")"
[ -n "$RL_PID" ] || fail "no worker recorded the publisher after the reload"

started_pids "$(( WORKERS * 2 ))" | grep -qx "$RL_PID" \
    || fail "the publisher after the reload was accepted by pid $RL_PID, which \
is not one of the workers the reload started"

NEW_GENERATION="$(started_pids "$(( WORKERS * 2 ))" | tail -n "$WORKERS" \
    | tr '\n' ' ')"

echo "   a publisher after the reload was accepted by pid $RL_PID, and the \
generation the reload started is: $NEW_GENERATION"

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

EXITED="$(grep -c 'worker process .* exited with code 0' "$LOG" || true)"
[ "$EXITED" -ge "$(( WORKERS * 2 ))" ] \
    || fail "only $EXITED workers exited cleanly, expected $(( WORKERS * 2 ))"

echo "   every worker exited cleanly"

echo "== rtmp workers ok"

#!/usr/bin/env bash
#
# One SRT ingest endpoint, shared by every worker.
#
# media_srt_listen_shared takes one endpoint and every worker binds it - each
# on a UDP socket of its own, created with SO_REUSEPORT and handed to libsrt
# with srt_bind_acquire() - so the kernel spreads publishers across the
# workers by 4-tuple instead of funnelling them all through the worker that
# bound the port.  It is a third mode beside the other two, neither of which
# changes meaning: one media_srt_listen is still worker 0's endpoint, and N
# entries are still "worker i binds entry i".
#
# What is proved here, and against which observable:
#
#   every worker binds the one endpoint
#     four workers, one directive, four "srt listener ready on <endpoint>"
#     lines from four distinct pids, each of them a worker of this instance.
#     The single-endpoint media_srt_listen shape is one line from worker 0,
#     and srt_worker_ports_nginx.sh asserts that shape still holds.
#
#   the mode is announced where an operator will see it
#     the process that reads the configuration logs one line naming the
#     endpoint and saying what a reload does to live publishers, before any
#     worker starts - once, not once per worker.
#
#   publishers spread over the workers, and every one of them carries media
#     eight publishers on the one port are accepted by more than one worker
#     (the split is printed), every stream's program_frames grows on its
#     owner, and a publisher accepted by a worker that does not own its
#     program is still routed over the internal transport.
#
#   what a reload does to live sessions, measured
#     publishers carrying media at the moment of a reload, a reload, and the
#     answer: each one of them fails, in the same second.  This is the
#     constraint the mode carries, and the reason it is not the default.
#     The same measurement is taken on a media_srt_listen endpoint, so the
#     comparison in docs/operations.md is a measurement rather than a claim.
#
#   the one bind failure the mode can report
#     changing media_srt_listen into media_srt_listen_shared and reloading
#     leaves the old generation holding the port without SO_REUSEPORT, which
#     the new workers retry until it exits - so the operator sees the retry
#     line and then a shared port, and a publisher is accepted afterwards.
#     A foreign process holding the port is the same failure, measured from
#     the other side: the worker line names the address, the reason, and what
#     it is doing about it, no worker claims the listener, and all of them take
#     the port when the holder exits - with a publisher accepted afterwards.
#
#   the configuration refuses what cannot work
#     media_srt_listen_shared beside media_srt_listen, beside
#     media_srt_listen_bond, and twice, are each refused with a message that
#     names what is wrong.
#
# Which worker accepted a session is read from the pid in the log line
# (nginx's error log prefix), mapped through the "worker N routing ready" line
# each worker writes with its own pid.
#
# Ports are derived from the pid, in a band of their own, so this script does
# not collide with the other SRT suites; the instance is stopped through its
# own prefix's pid file, never by name.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/srt-shared-port"
SHARED="$RUN/shared"
CONTROL="$RUN/control"
MIGRATE="$RUN/migrate"
HOLD="$RUN/hold"
ERRORS="$RUN/errors"

BASE=$(( 21000 + ($$ % 100) * 8 ))
SHR_SRT_PORT="$BASE"
SHR_HTTP_PORT="$(( BASE + 1 ))"
CTL_SRT_PORT="$(( BASE + 2 ))"
CTL_HTTP_PORT="$(( BASE + 3 ))"
MIG_SRT_PORT="$(( BASE + 4 ))"
MIG_HTTP_PORT="$(( BASE + 5 ))"
HOLD_SRT_PORT="$(( BASE + 6 ))"
HOLD_HTTP_PORT="$(( BASE + 7 ))"

HOLDER_PIDFILE="$RUN/holder.pid"

WORKERS=4
STREAMS=8
PUBS=()

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

fail() {
    echo "$*" >&2
    exit 1
}

stop_nginx() {
    # only this prefix's instance, through its own pid file: a graceful
    # signal first so the master reaps its workers, then what is left is
    # killed outright, so a worker stuck in transport teardown cannot hold
    # the ports.
    local run="$1" pid child

    [ -f "$run/logs/nginx.pid" ] || return 0

    pid="$(cat "$run/logs/nginx.pid" 2>/dev/null || true)"

    [ -n "$pid" ] || return 0

    kill -QUIT "$pid" 2>/dev/null || true

    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done

    for child in $(pgrep -P "$pid" 2>/dev/null || true); do
        kill -KILL "$child" 2>/dev/null || true
    done

    kill -KILL "$pid" 2>/dev/null || true

    return 0
}

cleanup() {
    local pid

    for pid in ${PUBS[@]+"${PUBS[@]}"}; do
        kill -KILL "$pid" 2>/dev/null || true
    done

    if [ -n "${HOLDER:-}" ]; then
        kill -KILL "$HOLDER" 2>/dev/null || true
    fi

    stop_nginx "$SHARED"
    stop_nginx "$CONTROL"
    stop_nginx "$MIGRATE"
    stop_nginx "$HOLD"

    return 0
}
trap cleanup EXIT

pids_of() {   # <log> <ere> → one pid per matching line
    grep -E "$2" "$1" 2>/dev/null \
        | sed -n 's/.*\[[a-z]*\] \([0-9][0-9]*\)#.*/\1/p' || true
}

pid_of() {    # <log> <ere> → the pid of the last matching line
    pids_of "$1" "$2" | tail -1
}

worker_pid() {  # <log> <worker index>
    pid_of "$1" "media: worker $2 routing ready"
}

wait_for() {  # <log> <ere> <count>
    local i

    for i in $(seq 1 400); do
        if [ "$(grep -cE "$2" "$1" 2>/dev/null || true)" -ge "$3" ]; then
            return 0
        fi
        sleep 0.05
    done

    return 1
}

# The owner hash is FNV-1a over application/stream and the owner slot is
# hash % workers: the same arithmetic every worker does
# (src/core/ngx_media_owner.c).  Asking for a name whose owner is a given slot
# is how a publisher is placed on a worker that does not own its program.
owner_name() {  # <wanted slot> <worker count> <prefix> → a name with that owner
    python3 - "$1" "$2" "$3" <<'PY'
import sys

want, workers, prefix = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]

for i in range(1, 2000):
    name = "%s%d" % (prefix, i)
    h = 2166136261
    for b in b"live/" + name.encode():
        h ^= b
        h = (h * 16777619) & 0xffffffff
    if h % workers == want:
        print(name)
        break
PY
}

# Every publisher is a real encoder over SRT, kept running past the window of
# the run so that "it stopped" can only mean its session ended.  The pid is
# remembered so the reload measurement can ask the process whether it is still
# there, and so cleanup can only ever kill what this script started.
publish() {  # <port> <stream name> <seconds> <tag>
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$3" -f mpegts \
        "srt://127.0.0.1:$1?mode=caller&streamid=#!::r=live/$2,m=publish,s=$4" \
        >"$RUN/pub-$4.log" 2>&1 &

    PUBS+=( $! )
}

pub_running() {  # <pid> → 0 while the process is still there
    local state

    state="$(ps -o stat= -p "$1" 2>/dev/null || true)"

    [ -n "$state" ] && [ "${state:0:1}" != "Z" ]
}

running_pubs() {  # <pids...> → the ones still running
    local pid

    for pid in "$@"; do
        pub_running "$pid" && printf '%s\n' "$pid"
    done

    return 0
}

drain_pubs() {
    if [ "${#PUBS[@]}" -gt 0 ]; then
        wait "${PUBS[@]}" 2>/dev/null || true
    fi

    PUBS=()
}

api_field() {  # <api base> <stream name> <json field>
    local v i

    for i in $(seq 1 60); do
        v="$(curl -fsS "$1/streams/live/$2" 2>/dev/null \
             | grep -o "\"$3\":[0-9]*" | head -1 | cut -d: -f2 || true)"

        if [ -n "$v" ]; then
            printf '%s' "$v"
            return 0
        fi

        sleep 0.25
    done

    return 1
}

api_owner() {  # <api base> <stream name>
    api_field "$1" "$2" owner
}

api_frames() {  # <api base> <stream name>
    api_field "$1" "$2" program_frames
}

wait_frames() {  # <api base> <stream name> <count>
    local i

    for i in $(seq 1 120); do
        if [ "$(api_frames "$1" "$2" || true)" -ge "$3" ] 2>/dev/null; then
            return 0
        fi

        sleep 0.25
    done

    return 1
}

rm -rf "$RUN"
mkdir -p "$SHARED/logs" "$SHARED/conf" "$CONTROL/logs" "$CONTROL/conf" \
         "$MIGRATE/logs" "$MIGRATE/conf" "$HOLD/logs" "$HOLD/conf" \
         "$ERRORS/logs" "$ERRORS/conf"

###############################################################################
echo "== configuration errors"
###############################################################################

# the shared mode is one endpoint for the whole instance: beside the per-worker
# directive there would be an entry order that says nothing about a worker's
# second listener, and beside a bond address it cannot work at all
cat > "$ERRORS/conf/both.conf" <<EOF
worker_processes 4;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen 127.0.0.1:$SHR_SRT_PORT;
media_srt_listen_shared 127.0.0.1:$(( SHR_SRT_PORT + 100 ));
EOF

cat > "$ERRORS/conf/bond.conf" <<EOF
worker_processes 4;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen_shared 127.0.0.1:$SHR_SRT_PORT;
media_srt_listen_bond 127.0.0.2;
EOF

cat > "$ERRORS/conf/duplicate.conf" <<EOF
worker_processes 4;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen_shared 127.0.0.1:$SHR_SRT_PORT;
media_srt_listen_shared 127.0.0.1:$(( SHR_SRT_PORT + 100 ));
EOF

config_error() {  # <conf name> <message that must be there>
    if "$NGINX" -p "$ERRORS" -c "conf/$1.conf" -t >"$ERRORS/$1.out" 2>&1; then
        fail "conf/$1.conf was accepted; it must be refused"
    fi

    grep -qF "$2" "$ERRORS/$1.out" \
        || { cat "$ERRORS/$1.out" >&2
             fail "the refusal of conf/$1.conf does not say: $2"; }
}

config_error both "cannot be combined with media_srt_listen"
echo "   refused: media_srt_listen_shared with media_srt_listen"

config_error bond "cannot be combined with media_srt_listen_bond"
echo "   refused: media_srt_listen_shared with media_srt_listen_bond"

config_error duplicate "is duplicate"
echo "   refused: media_srt_listen_shared twice"

###############################################################################
echo "== four workers, one endpoint"
###############################################################################

cat > "$SHARED/conf/nginx.conf" <<EOF
worker_processes $WORKERS;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen_shared 127.0.0.1:$SHR_SRT_PORT;

http {
    access_log off;

    server {
        listen 127.0.0.1:$SHR_HTTP_PORT;

        location /media/api/ { media_api; }
    }
}
EOF

SHR_LOG="$SHARED/logs/error.log"
SHR_API="http://127.0.0.1:$SHR_HTTP_PORT/media/api/v1"

"$NGINX" -p "$SHARED" -e stderr -c conf/nginx.conf -t
"$NGINX" -p "$SHARED" -c conf/nginx.conf

i=0
while [ "$i" -lt "$WORKERS" ]; do
    wait_for "$SHR_LOG" "media: worker $i routing ready" 1 \
        || fail "worker $i did not adopt its routing endpoints"
    i=$(( i + 1 ))
done

wait_for "$SHR_LOG" "srt listener ready on 127.0.0.1:$SHR_SRT_PORT" "$WORKERS" \
    || fail "the shared endpoint was bound $(grep -c "srt listener ready" "$SHR_LOG" || true) time(s), not by all $WORKERS workers"

BOUND="$(pids_of "$SHR_LOG" "srt listener ready on 127.0.0.1:$SHR_SRT_PORT" | sort -u)"
BOUND_N="$(printf '%s\n' "$BOUND" | grep -c . || true)"
[ "$BOUND_N" -eq "$WORKERS" ] \
    || fail "$BOUND_N distinct workers bound the shared endpoint, not $WORKERS"

i=0
while [ "$i" -lt "$WORKERS" ]; do
    WP="$(worker_pid "$SHR_LOG" "$i")"

    printf '%s\n' "$BOUND" | grep -qx "$WP" \
        || fail "worker $i ($WP) did not bind the shared endpoint"

    i=$(( i + 1 ))
done

echo "   every worker bound 127.0.0.1:$SHR_SRT_PORT:" \
     "$(printf '%s' "$BOUND" | tr '\n' ' ')"

# the mode is loud about itself: one line per configuration read, naming the
# endpoint, saying what a reload does, and naming where the constraint is
# written down.  The configuration test is run with -e stderr so that it does
# not write its own copy into the instance's log, and the assertion counts
# distinct pids: the announcement is a property of the mode, not of a worker.
WARN_ERE="media_srt_listen_shared 127.0.0.1:$SHR_SRT_PORT is bound by every worker"

wait_for "$SHR_LOG" "$WARN_ERE" 1 \
    || fail "the shared mode was not announced at all"

WARN_PIDS="$(pids_of "$SHR_LOG" "$WARN_ERE" | sort -u)"
WARN_N="$(printf '%s\n' "$WARN_PIDS" | grep -c . || true)"
[ "$WARN_N" -eq 1 ] \
    || fail "the shared mode was announced by $WARN_N processes, not once:$WARN_PIDS"

i=0
while [ "$i" -lt "$WORKERS" ]; do
    WP="$(worker_pid "$SHR_LOG" "$i")"

    [ "$(printf '%s\n' "$WARN_PIDS" | grep -cx "$WP" || true)" -eq 0 ] \
        || fail "worker $i announced the shared mode too: it is a property of the mode, not of a worker"

    i=$(( i + 1 ))
done

grep -qF "a reload ends every live publisher's session" "$SHR_LOG" \
    || fail "the shared mode was announced without saying what a reload does"

grep -qF "docs/operations.md" "$SHR_LOG" \
    || fail "the announcement does not say where the constraint is documented"

echo "   announced once per configuration read, naming the reload and the" \
     "documentation"

###############################################################################
echo "== publishers spread over the workers, and carry media"
###############################################################################

# Every publisher names a stream owned by the *next* worker, so a publisher
# accepted by its own owner's worker still has to be routed - and which
# publisher that is depends on where the kernel put it, which is the point of
# the mode.
NAMES=()
OWNERS=()
i=0
while [ "$i" -lt "$STREAMS" ]; do
    OWNER=$(( (i + 1) % WORKERS ))
    NAME="$(owner_name "$OWNER" "$WORKERS" "sh$i")"

    [ -n "$NAME" ] || fail "no stream name has owner $OWNER"

    NAMES+=( "$NAME" )
    OWNERS+=( "$OWNER" )
    publish "$SHR_SRT_PORT" "$NAME" 90 "sh$i"

    i=$(( i + 1 ))
done

i=0
while [ "$i" -lt "$STREAMS" ]; do
    wait_for "$SHR_LOG" "srt source open app=live stream=${NAMES[$i]} " 1 \
        || fail "publisher ${NAMES[$i]} was never accepted"

    # media reached the program, which lives on the stream's owner
    wait_frames "$SHR_API" "${NAMES[$i]}" 1 \
        || fail "${NAMES[$i]} carried no frames to its owner"

    i=$(( i + 1 ))
done

ACCEPTED_PIDS="$(pids_of "$SHR_LOG" 'srt source open app=live' | sort -u)"
ACCEPTED_N="$(printf '%s\n' "$ACCEPTED_PIDS" | grep -c . || true)"

[ "$ACCEPTED_N" -ge 2 ] \
    || fail "all $STREAMS publishers were accepted by one worker ($ACCEPTED_PIDS): the port was not shared"

echo "   $STREAMS publishers accepted by $ACCEPTED_N workers:" \
     "$(printf '%s' "$ACCEPTED_PIDS" | tr '\n' ' ')"

# at least one publisher must have landed on a worker that does not own its
# program, and that one is the routing case
ROUTED=0
i=0
while [ "$i" -lt "$STREAMS" ]; do
    OWNER="${OWNERS[$i]}"
    NAME="${NAMES[$i]}"
    ACC="$(pid_of "$SHR_LOG" "srt source open app=live stream=$NAME ")"
    OWNPID="$(worker_pid "$SHR_LOG" "$OWNER")"

    REPORTED="$(api_owner "$SHR_API" "$NAME")" \
        || fail "the graph reports no owner for $NAME"
    [ "$REPORTED" = "$OWNER" ] \
        || fail "the graph reports owner $REPORTED for $NAME, not $OWNER"

    if [ "$ACC" != "$OWNPID" ]; then
        wait_for "$SHR_LOG" "srt publisher routed to the owner stream=live/$NAME" 1 \
            || fail "$NAME was accepted by pid $ACC, not worker $OWNER ($OWNPID), and was not routed"

        ROUTED_PID="$(pid_of "$SHR_LOG" "routed source opened stream=live/$NAME")"
        [ "$ROUTED_PID" = "$OWNPID" ] \
            || fail "routed source $NAME opened on pid $ROUTED_PID, not worker $OWNER ($OWNPID)"

        ROUTED=$(( ROUTED + 1 ))
    fi

    i=$(( i + 1 ))
done

[ "$ROUTED" -gt 0 ] \
    || fail "no publisher landed on a worker that does not own its program, so routing was never exercised"

echo "   $ROUTED of $STREAMS publishers were accepted by a non-owner and routed" \
     "to the owner the graph reports"

###############################################################################
echo "== what a reload does to live publishers (shared endpoint)"
###############################################################################

i=0
while [ "$i" -lt "$STREAMS" ]; do
    pub_running "${PUBS[$i]}" \
        || fail "publisher ${NAMES[$i]} stopped before the reload: its exit cannot be attributed to it"

    i=$(( i + 1 ))
done

RELOAD_AT=$SECONDS
"$NGINX" -p "$SHARED" -c conf/nginx.conf -s reload

# A publisher still running after this window was not touched by the reload.
DEADLINE=$(( SECONDS + 15 ))
while :; do
    STILL="$(running_pubs "${PUBS[@]}")"
    [ -z "$STILL" ] && break
    [ "$SECONDS" -ge "$DEADLINE" ] && break
    sleep 0.1
done

ELAPSED=$(( SECONDS - RELOAD_AT ))
STILL_N="$(printf '%s\n' "$STILL" | grep -c . || true)"
DEAD_N=$(( STREAMS - STILL_N ))

[ "$STILL_N" -eq 0 ] \
    || fail "$STILL_N of $STREAMS publishers were still running $ELAPSED s after the reload," \
            " which contradicts what the mode announces and what docs/operations.md says"

# the status of each one, now that they have all exited: a publisher that was
# cut off fails (ffmpeg exits non-zero), it does not finish
i=0
while [ "$i" -lt "$STREAMS" ]; do
    set +e
    wait "${PUBS[$i]}" 2>/dev/null
    STATUS=$?
    set -e

    [ "$STATUS" -ne 0 ] \
        || fail "publisher ${NAMES[$i]} exited cleanly across the reload"

    i=$(( i + 1 ))
done

PUBS=()

echo "   all $STREAMS live publishers failed within ${ELAPSED}s of the reload"
echo "   a publisher's own log says: $(grep -m1 'Input/output error' "$RUN/pub-sh0.log" | tr -s ' ' || echo '(nothing)')"

# the port is still an ingest port afterwards: the new generation bound it
wait_for "$SHR_LOG" "srt listener ready on 127.0.0.1:$SHR_SRT_PORT" "$(( WORKERS * 2 ))" \
    || fail "the reloaded generation did not bind the shared endpoint"

SURVIVOR="$(owner_name 0 "$WORKERS" "after")"
publish "$SHR_SRT_PORT" "$SURVIVOR" 4 "after"
wait_for "$SHR_LOG" "srt source open app=live stream=$SURVIVOR " 1 \
    || fail "a publisher after the reload was not accepted on the shared endpoint"

wait_frames "$SHR_API" "$SURVIVOR" 1 \
    || fail "the publisher after the reload carried no frames"

drain_pubs

echo "   a publisher after the reload was accepted and carried media"

stop_nginx "$SHARED"

###############################################################################
echo "== the same reload on a per-worker endpoint (control)"
###############################################################################

cat > "$CONTROL/conf/nginx.conf" <<EOF
worker_processes $WORKERS;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen 127.0.0.1:$CTL_SRT_PORT;

http {
    access_log off;

    server {
        listen 127.0.0.1:$CTL_HTTP_PORT;

        location /media/api/ { media_api; }
    }
}
EOF

CTL_LOG="$CONTROL/logs/error.log"
CTL_API="http://127.0.0.1:$CTL_HTTP_PORT/media/api/v1"

"$NGINX" -p "$CONTROL" -e stderr -c conf/nginx.conf -t
"$NGINX" -p "$CONTROL" -c conf/nginx.conf

wait_for "$CTL_LOG" "srt listener ready on 127.0.0.1:$CTL_SRT_PORT" 1 \
    || fail "the control instance never bound its endpoint"

CTL_NAME="$(owner_name 1 "$WORKERS" "ctl")"
publish "$CTL_SRT_PORT" "$CTL_NAME" 90 "ctl"

wait_for "$CTL_LOG" "srt source open app=live stream=$CTL_NAME " 1 \
    || fail "the control publisher was never accepted"

wait_frames "$CTL_API" "$CTL_NAME" 1 \
    || fail "the control publisher carried no frames"

CTL_AT=$SECONDS
"$NGINX" -p "$CONTROL" -c conf/nginx.conf -s reload

CTL_STILL_CTL=""
DEADLINE=$(( SECONDS + 15 ))
while :; do
    CTL_STILL_CTL="$(running_pubs "${PUBS[@]}")"
    [ -z "$CTL_STILL_CTL" ] && break
    [ "$SECONDS" -ge "$DEADLINE" ] && break
    sleep 0.1
done

CTL_ELAPSED=$(( SECONDS - CTL_AT ))
CTL_STILL="$(printf '%s\n' "$CTL_STILL_CTL" | grep -c . || true)"

if [ "$CTL_STILL" -eq 0 ]; then
    echo "   a reload ends a live publisher's session here too (${CTL_ELAPSED}s)"
else
    echo "   a live publisher survived this reload (still running after ${CTL_ELAPSED}s)"
fi

drain_pubs

stop_nginx "$CONTROL"

###############################################################################
echo "== a reload into the shared mode retries the port"
###############################################################################

cat > "$MIGRATE/conf/nginx.conf" <<EOF
worker_processes $WORKERS;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen 127.0.0.1:$MIG_SRT_PORT;

http {
    access_log off;

    server {
        listen 127.0.0.1:$MIG_HTTP_PORT;

        location /media/api/ { media_api; }
    }
}
EOF

MIG_LOG="$MIGRATE/logs/error.log"

"$NGINX" -p "$MIGRATE" -e stderr -c conf/nginx.conf -t
"$NGINX" -p "$MIGRATE" -c conf/nginx.conf

wait_for "$MIG_LOG" "srt listener ready on 127.0.0.1:$MIG_SRT_PORT" 1 \
    || fail "the per-worker listener was never bound before the change"

# the same port, now shared: the old generation holds it without SO_REUSEPORT,
# so the new workers can only wait for it
MIG_OLD_BOUND="$(pids_of "$MIG_LOG" "srt listener ready on 127.0.0.1:$MIG_SRT_PORT" | sort -u)"

python3 - "$MIGRATE/conf/nginx.conf" "$MIG_SRT_PORT" <<'PY'
import sys

path, port = sys.argv[1], sys.argv[2]

with open(path) as fh:
    text = fh.read()

text = text.replace("media_srt_listen 127.0.0.1:%s;" % port,
                    "media_srt_listen_shared 127.0.0.1:%s;" % port)

with open(path, "w") as fh:
    fh.write(text)
PY

"$NGINX" -p "$MIGRATE" -e stderr -c conf/nginx.conf -t
"$NGINX" -p "$MIGRATE" -c conf/nginx.conf -s reload

# The old generation holds the port without SO_REUSEPORT, so the new workers
# can only retry until it exits.  Whether one of them logged a failed attempt
# depends on how quickly the old generation let the port go, so it is reported
# rather than required; the port ending up shared is what is asserted.
RETRIED=no

if wait_for "$MIG_LOG" "is not available yet" 1; then
    RETRIED=yes
fi

wait_for "$MIG_LOG" "srt listener ready on 127.0.0.1:$MIG_SRT_PORT" "$(( WORKERS + 1 ))" \
    || fail "the port was not released into the shared mode"

MIG_NEW_BOUND="$(comm -13 \
    <(printf '%s\n' "$MIG_OLD_BOUND" | sort) \
    <(pids_of "$MIG_LOG" "srt listener ready on 127.0.0.1:$MIG_SRT_PORT" | sort -u) \
    | grep -c . || true)"

[ "$MIG_NEW_BOUND" -eq "$WORKERS" ] \
    || fail "$MIG_NEW_BOUND workers of the reloaded generation held the port, not $WORKERS"

MIG_NAME="$(owner_name 2 "$WORKERS" "mig")"
publish "$MIG_SRT_PORT" "$MIG_NAME" 4 "mig"

wait_for "$MIG_LOG" "srt source open app=live stream=$MIG_NAME " 1 \
    || fail "a publisher after the mode change was not accepted on the shared endpoint"

wait_frames "http://127.0.0.1:$MIG_HTTP_PORT/media/api/v1" "$MIG_NAME" 1 \
    || fail "the publisher after the mode change carried no frames"

drain_pubs

echo "   the port was released into the shared mode (retry line: $RETRIED), all" \
     "$MIG_NEW_BOUND workers of the new generation bound it, and a publisher" \
     "was accepted"

stop_nginx "$MIGRATE"

###############################################################################
echo "== the one bind failure the mode can report"
###############################################################################

# SO_REUSEPORT is what makes the shared bind work, so the one way it still
# fails with an address in use is a holder that does not share: a foreign
# process, or the generation a reload is replacing when the directive changes.
# The worker must say that in full - the address, the reason, and what it does
# about it - and then take the port when the holder goes, rather than starting
# an instance that accepts nothing.
cat > "$HOLD/conf/nginx.conf" <<EOF
worker_processes $WORKERS;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen_shared 127.0.0.1:$HOLD_SRT_PORT;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HOLD_HTTP_PORT;

        location /media/api/ { media_api; }
    }
}
EOF

HOLD_LOG="$HOLD/logs/error.log"

python3 - "$HOLD_SRT_PORT" "$HOLDER_PIDFILE" <<'PY' &
import socket, sys, time

port, pidfile = int(sys.argv[1]), sys.argv[2]

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", port))

with open(pidfile, "w") as fh:
    fh.write("%d\n" % __import__("os").getpid())

time.sleep(60)
PY
HOLDER=$!

for _ in $(seq 1 100); do
    [ -s "$HOLDER_PIDFILE" ] && break
    sleep 0.05
done

[ -s "$HOLDER_PIDFILE" ] \
    || fail "the process holding the port never bound it"

"$NGINX" -p "$HOLD" -c conf/nginx.conf

wait_for "$HOLD_LOG" "is not available yet" 1 \
    || fail "the shared bind against a holder that does not share did not say so"

grep -qF "the port is held by something without SO_REUSEPORT" "$HOLD_LOG" \
    || { grep 'is not available yet' "$HOLD_LOG" >&2
         fail "the refusal does not say why the port cannot be shared"; }

grep -qF "Address already in use" "$HOLD_LOG" \
    || fail "the refusal does not name the address failure"

[ "$(grep -c 'srt listener ready' "$HOLD_LOG" || true)" -eq 0 ] \
    || fail "a worker claimed the listener while the port was held"

echo "   while the port was held: $(grep -m1 -o 'is not available yet.*' "$HOLD_LOG" | cut -c1-140)"

# the holder goes: the workers take the port, with nothing else restarted
kill -KILL "$HOLDER" 2>/dev/null || true
wait "$HOLDER" 2>/dev/null || true

wait_for "$HOLD_LOG" "srt listener ready on 127.0.0.1:$HOLD_SRT_PORT" "$WORKERS" \
    || fail "the port was never taken after the holder exited"

HOLD_NAME="$(owner_name 3 "$WORKERS" "held")"
publish "$HOLD_SRT_PORT" "$HOLD_NAME" 4 "held"

wait_for "$HOLD_LOG" "srt source open app=live stream=$HOLD_NAME " 1 \
    || fail "a publisher was not accepted after the port was taken"

wait_frames "http://127.0.0.1:$HOLD_HTTP_PORT/media/api/v1" "$HOLD_NAME" 1 \
    || fail "the publisher after the port was taken carried no frames"

drain_pubs

echo "   when the holder exited, all $WORKERS workers bound it and a publisher" \
     "was accepted"

stop_nginx "$HOLD"

trap - EXIT
cleanup

echo "== srt shared port ok"

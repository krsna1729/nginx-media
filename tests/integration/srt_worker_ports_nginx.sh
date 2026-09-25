#!/usr/bin/env bash
#
# One SRT ingest endpoint per worker.
#
# media_srt_listen may be given once per worker, and worker i binds the i-th
# entry: a publisher placed on a worker's endpoint is accepted by that worker,
# and its frames cross to the program's owner only when the owner is another
# worker.  Before the directive accumulated, only worker 0 had a listener, so
# a four-worker instance had one ingest endpoint, every publisher arrived at
# worker 0, and routing to the owner was the main path rather than the escape
# hatch goal doc 22 describes it as.
#
# What is proved here, and against which observable:
#
#   four workers, four endpoints
#     all four workers bind, worker i binds entry i, and one publisher per
#     endpoint is accepted by four distinct workers.  On the old behaviour
#     one worker binds, and three of the four publishers have nowhere to
#     connect at all.
#
#   routing still works
#     every publisher here names a stream owned by a *different* worker, so
#     each one has to be routed and has to arrive: the accepting worker logs
#     the route, the owner logs the source it opened, and the control API
#     reports the owner the frames reached.
#
#   one endpoint, four workers
#     the single-listener shape is unchanged: worker 0 binds the only
#     endpoint, every publisher still lands there, and a publisher whose
#     program is owned elsewhere is still routed from there.
#
#   configuration errors
#     the same endpoint twice is refused with a message that names what is
#     wrong; more endpoints than workers is accepted with a warning, because
#     the worker count is not always in the configuration; fewer endpoints
#     than workers is accepted, because those workers own programs without
#     accepting publishers.
#
# Which worker accepted a session is read from the pid in the log line
# (nginx's error log prefix), mapped through the "worker N routing ready" line
# each worker writes with its own pid.
#
# Ports are derived from the pid so two runs of this script, or this script
# beside another, do not collide; the instance is stopped through its own
# prefix's pid file, never by name.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/srt-worker-ports"
ONE="$RUN/one"
MANY="$RUN/many"
ERRORS="$RUN/errors"

BASE=$(( 20000 + ($$ % 100) * 8 ))
ONE_SRT_PORT="$BASE"
ONE_HTTP_PORT="$(( BASE + 1 ))"
SRT_PORTS=( "$(( BASE + 2 ))" "$(( BASE + 3 ))" "$(( BASE + 4 ))" "$(( BASE + 5 ))" )
MANY_HTTP_PORT="$(( BASE + 6 ))"

WORKERS=4
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

    stop_nginx "$MANY"
    stop_nginx "$ONE"

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
owner_name() {  # <wanted slot> <worker count> → a stream name with that owner
    python3 - "$1" "$2" <<'PY'
import sys

want, workers = int(sys.argv[1]), int(sys.argv[2])

for i in range(1, 500):
    name = "wp%d" % i
    h = 14695981039346656037
    for b in b"live/" + name.encode():
        h ^= b
        h = (h * 1099511628211) & ((1 << 64) - 1)
    if h % workers == want:
        print(name)
        break
PY
}

publish() {  # <port> <stream name> <seconds>
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$3" -f mpegts \
        "srt://127.0.0.1:$1?mode=caller&streamid=#!::r=live/$2,m=publish,s=encoder" \
        >"$RUN/pub-$1.log" 2>&1 &

    PUBS+=( $! )
}

drain_pubs() {
    wait ${PUBS[@]+"${PUBS[@]}"} || true
    PUBS=()
}

api_owner() {  # <api base> <stream name> → the owner the graph reports
    local v i

    for i in $(seq 1 60); do
        v="$(curl -fsS "$1/streams/live/$2" 2>/dev/null \
             | grep -o '"owner":[0-9]*' | head -1 | cut -d: -f2 || true)"

        if [ -n "$v" ]; then
            printf '%s' "$v"
            return 0
        fi

        sleep 0.25
    done

    return 1
}

rm -rf "$RUN"
mkdir -p "$ONE/logs" "$ONE/conf" "$MANY/logs" "$MANY/conf" \
         "$ERRORS/logs" "$ERRORS/conf"

###############################################################################
echo "== configuration errors"
###############################################################################

# one endpoint per worker: the same endpoint twice is refused, five endpoints
# with four workers is accepted with a warning (the worker count comes from the
# environment in a container, so scaling down must not stop the instance), and
# fewer endpoints than workers is legal - those workers own programs without
# accepting publishers
cat > "$ERRORS/conf/too-many.conf" <<EOF
worker_processes 4;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen 127.0.0.1:$(( BASE + 10 ));
media_srt_listen 127.0.0.1:$(( BASE + 11 ));
media_srt_listen 127.0.0.1:$(( BASE + 12 ));
media_srt_listen 127.0.0.1:$(( BASE + 13 ));
media_srt_listen 127.0.0.1:$(( BASE + 14 ));
EOF

cat > "$ERRORS/conf/duplicate.conf" <<EOF
worker_processes 4;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen 127.0.0.1:$(( BASE + 10 ));
media_srt_listen 127.0.0.1:$(( BASE + 10 ));
EOF

cat > "$ERRORS/conf/fewer.conf" <<EOF
worker_processes 4;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen 127.0.0.1:$(( BASE + 10 ));
media_srt_listen 127.0.0.1:$(( BASE + 11 ));
EOF

config_error() {  # <conf name> <message that must be there>
    local name="$1" want="$2" out="$ERRORS/$1.out"

    if "$NGINX" -p "$ERRORS" -c "conf/$name.conf" -t >"$out" 2>&1; then
        fail "$name was accepted; expected it refused: $want"
    fi

    if ! grep -qF "$want" "$out" 2>/dev/null \
       && ! grep -qF "$want" "$ERRORS/logs/error.log" 2>/dev/null; then
        cat "$out" >&2
        tail -3 "$ERRORS/logs/error.log" >&2
        fail "$name was refused without saying: $want"
    fi

    echo "   refused: $want"
}

config_error duplicate "is duplicate: one endpoint per worker"

# more endpoints than workers: accepted, with a warning naming the waste.
# Refusing would make the configuration depend on the worker count, and the
# container takes that from the environment.
if ! "$NGINX" -p "$ERRORS" -c conf/too-many.conf -t >"$ERRORS/too-many.out" 2>&1
then
    cat "$ERRORS/too-many.out" >&2
    fail "five endpoints with four workers must be accepted, with a warning"
fi

grep -qF "endpoint(s) and there are 4 worker(s)" "$ERRORS/too-many.out" \
    || { cat "$ERRORS/too-many.out" >&2
         fail "the extra endpoints were accepted without saying they are unused"; }

echo "   accepted with a warning: more endpoints than workers"

"$NGINX" -p "$ERRORS" -c conf/fewer.conf -t >"$ERRORS/fewer.out" 2>&1 \
    || { cat "$ERRORS/fewer.out" >&2
         fail "two endpoints with four workers must be legal"; }
echo "   accepted: fewer endpoints than workers"

###############################################################################
echo "== one endpoint, four workers"
###############################################################################

cat > "$ONE/conf/nginx.conf" <<EOF
worker_processes $WORKERS;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_srt_listen 127.0.0.1:$ONE_SRT_PORT;

http {
    access_log off;

    server {
        listen 127.0.0.1:$ONE_HTTP_PORT;

        location /media/api/ { media_api; }
    }
}
EOF

ONE_LOG="$ONE/logs/error.log"

"$NGINX" -p "$ONE" -c conf/nginx.conf -t
"$NGINX" -p "$ONE" -c conf/nginx.conf

wait_for "$ONE_LOG" 'media: worker 3 routing ready' 1 \
    || fail "worker 3 did not adopt its routing endpoints"
wait_for "$ONE_LOG" 'srt listener ready' 1 \
    || fail "the single endpoint was never bound"

LISTENED="$(grep -cE 'srt listener ready' "$ONE_LOG" || true)"
[ "$LISTENED" -eq 1 ] \
    || fail "one endpoint must mean one listener, saw $LISTENED"

ONE_W0="$(worker_pid "$ONE_LOG" 0)"
[ -n "$ONE_W0" ] || fail "worker 0 wrote no routing line" \
                     " (its pid cannot be resolved from the log)"

ONE_LISTENER="$(pid_of "$ONE_LOG" "srt listener ready on 127.0.0.1:$ONE_SRT_PORT")"
[ "$ONE_LISTENER" = "$ONE_W0" ] \
    || fail "with one endpoint worker 0 ($ONE_W0) binds it, not pid $ONE_LISTENER"

ONE_OWNER=2
ONE_NAME="$(owner_name "$ONE_OWNER" "$WORKERS")"
[ -n "$ONE_NAME" ] || fail "no stream name has owner $ONE_OWNER"

publish "$ONE_SRT_PORT" "$ONE_NAME" 3

wait_for "$ONE_LOG" "srt publisher routed to the owner stream=live/$ONE_NAME" 1 \
    || fail "the publisher to the single endpoint was not routed"

ONE_ACCEPTED="$(pid_of "$ONE_LOG" "srt source open app=live stream=$ONE_NAME ")"
[ "$ONE_ACCEPTED" = "$ONE_W0" ] \
    || fail "the publisher was accepted by pid $ONE_ACCEPTED, not worker 0"

wait_for "$ONE_LOG" "routed source opened stream=live/$ONE_NAME" 1 \
    || fail "worker $ONE_OWNER did not open the routed source"

ONE_ROUTED="$(pid_of "$ONE_LOG" "routed source opened stream=live/$ONE_NAME")"
[ "$ONE_ROUTED" = "$(worker_pid "$ONE_LOG" "$ONE_OWNER")" ] \
    || fail "the routed source opened on pid $ONE_ROUTED, not worker $ONE_OWNER"

drain_pubs
stop_nginx "$ONE"

echo "   worker 0 accepted it on $ONE_SRT_PORT and routed it to worker $ONE_OWNER"

###############################################################################
echo "== four workers, four endpoints"
###############################################################################

{
    echo "worker_processes $WORKERS;"
    echo "daemon on;"
    echo "error_log logs/error.log info;"
    echo "pid logs/nginx.pid;"
    echo
    echo "events { worker_connections 256; }"
    echo

    for port in "${SRT_PORTS[@]}"; do
        echo "media_srt_listen 127.0.0.1:$port;"
    done

    cat <<EOF

http {
    access_log off;

    server {
        listen 127.0.0.1:$MANY_HTTP_PORT;

        location /media/api/ { media_api; }
    }
}
EOF
} > "$MANY/conf/nginx.conf"

MANY_LOG="$MANY/logs/error.log"
MANY_API="http://127.0.0.1:$MANY_HTTP_PORT/media/api/v1"

"$NGINX" -p "$MANY" -c conf/nginx.conf -t
"$NGINX" -p "$MANY" -c conf/nginx.conf

i=0
while [ "$i" -lt "$WORKERS" ]; do
    wait_for "$MANY_LOG" "media: worker $i routing ready" 1 \
        || fail "worker $i did not adopt its routing endpoints"
    i=$(( i + 1 ))
done

wait_for "$MANY_LOG" 'srt listener ready' "$WORKERS" \
    || fail "not every endpoint was bound: $(grep -c 'srt listener ready' "$MANY_LOG" || true) of $WORKERS"

i=0
while [ "$i" -lt "$WORKERS" ]; do
    WP="$(worker_pid "$MANY_LOG" "$i")"
    LP="$(pid_of "$MANY_LOG" "srt listener ready on 127.0.0.1:${SRT_PORTS[$i]}")"

    [ -n "$WP" ] || fail "worker $i wrote no routing line"
    [ -n "$LP" ] || fail "endpoint ${SRT_PORTS[$i]} was never bound"
    [ "$LP" = "$WP" ] \
        || fail "endpoint ${SRT_PORTS[$i]} was bound by pid $LP, not worker $i ($WP)"

    i=$(( i + 1 ))
done

echo "   worker i bound entry i: ${SRT_PORTS[*]}"

# every publisher names a stream owned by the *next* worker, so all four have
# to be routed and all four have to arrive
NAMES=()
i=0
while [ "$i" -lt "$WORKERS" ]; do
    OWNER=$(( (i + 1) % WORKERS ))
    NAME="$(owner_name "$OWNER" "$WORKERS")"

    [ -n "$NAME" ] || fail "no stream name has owner $OWNER"

    NAMES+=( "$NAME" )
    publish "${SRT_PORTS[$i]}" "$NAME" 4

    i=$(( i + 1 ))
done

i=0
while [ "$i" -lt "$WORKERS" ]; do
    wait_for "$MANY_LOG" "srt publisher routed to the owner stream=live/${NAMES[$i]}" 1 \
        || fail "the publisher on ${SRT_PORTS[$i]} was not routed to its owner"

    wait_for "$MANY_LOG" "routed source opened stream=live/${NAMES[$i]}" 1 \
        || fail "worker $(( (i + 1) % WORKERS )) did not open the routed source ${NAMES[$i]}"

    i=$(( i + 1 ))
done

ACCEPTED="$(pids_of "$MANY_LOG" 'srt source open app=live' | sort -u | wc -l)"
[ "$ACCEPTED" -eq "$WORKERS" ] \
    || fail "the four publishers were accepted by $ACCEPTED distinct workers, not $WORKERS"

i=0
while [ "$i" -lt "$WORKERS" ]; do
    OWNER=$(( (i + 1) % WORKERS ))
    WP="$(worker_pid "$MANY_LOG" "$i")"
    OP="$(worker_pid "$MANY_LOG" "$OWNER")"

    ACCEPTED_PID="$(pid_of "$MANY_LOG" "srt source open app=live stream=${NAMES[$i]} ")"
    [ "$ACCEPTED_PID" = "$WP" ] \
        || fail "the publisher on ${SRT_PORTS[$i]} was accepted by pid $ACCEPTED_PID, not worker $i ($WP)"

    ROUTED_PID="$(pid_of "$MANY_LOG" "routed source opened stream=live/${NAMES[$i]}")"
    [ "$ROUTED_PID" = "$OP" ] \
        || fail "routed source ${NAMES[$i]} opened on pid $ROUTED_PID, not worker $OWNER ($OP)"

    REPORTED="$(api_owner "$MANY_API" "${NAMES[$i]}")" \
        || fail "the graph reports no owner for ${NAMES[$i]}"
    [ "$REPORTED" = "$OWNER" ] \
        || fail "the graph reports owner $REPORTED for ${NAMES[$i]}, not $OWNER"

    i=$(( i + 1 ))
done

drain_pubs

echo "   four publishers on four endpoints, accepted by workers" \
     "$(pids_of "$MANY_LOG" 'srt source open app=live' | sort -u | tr '\n' ' ')"
echo "   each routed to the owner the graph reports"

stop_nginx "$MANY"

trap - EXIT
cleanup

echo "== srt worker ports ok"

#!/usr/bin/env bash
#
# Two workers writing one stream at the same time converge on one state.
#
# Revisions are one sequence for every worker, so two mutations that race are
# comparable and every replica resolves them the same way: the higher revision
# is the newer state and the lower one is dropped.  With a counter per worker
# the two mutations take the same number, each replica applies the other's
# operation (it is not older, so it is not stale) and the workers end up
# holding each other's value - which is what this case asserts cannot happen.
#
# What it would catch: a revision assigned locally instead of from the shared
# sequence, a replica that applies an operation it has already moved past, and
# a mutation that reports a revision it did not store.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/graph-conflict"
HTTP_PORT=18595
ROUNDS=12
READS=6

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

cleanup() {
    for inst in "$RUN"; do
        [ -f "$inst/logs/nginx.pid" ] || continue

        pid="$(cat "$inst/logs/nginx.pid")"
        kill -QUIT "$pid" 2>/dev/null

        for _ in $(seq 1 100); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.05
        done

        kill -KILL "$pid" 2>/dev/null
    done

    return 0
}
trap cleanup EXIT

# reuseport: the kernel picks the worker per connection, which is how one port
# reaches all of them - the same way a real client's requests land wherever the
# socket lands.  Three of them, because the resurrection case needs two
# different originators: with two, an operation and the delete that races it
# travel the same channel and arrive in the order they were sent.
cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 3;
daemon on;
error_log logs/error.log notice;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/hls;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT reuseport;

        location /media/api/ { media_api; }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null \
    || { echo "configuration rejected" >&2; exit 1; }

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

echo "== a stream on two workers"
STATUS="$(curl -sS -o "$RUN/create.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"news"}' "$API/streams")"

[ "$STATUS" = "201" ] || { echo "create failed: $STATUS" >&2
                           cat "$RUN/create.json" >&2; exit 1; }

# the case is about two workers disagreeing; if every connection lands on one
# worker there is nothing to disagree about and the result means nothing
SEEN_OWNER=0
SEEN_REPLICA=0

field() {  # field <json> <name>
    printf '%s' "$1" | grep -o "\"$2\":[^,}]*" | head -1 | cut -d: -f2
}

echo "== $ROUNDS rounds of two conflicting mutations at once"
for round in $(seq 1 "$ROUNDS"); do
    A=$((1000 + round))
    B=$((2000 + round))

    curl -sS -o "$RUN/patch-a.json" -w '%{http_code}' \
        -X PATCH -H 'Content-Type: application/json' \
        -d "{\"failure_timeout_ms\":$A}" \
        "$API/streams/live/news" >"$RUN/code-a" &
    PA=$!

    curl -sS -o "$RUN/patch-b.json" -w '%{http_code}' \
        -X PATCH -H 'Content-Type: application/json' \
        -d "{\"failure_timeout_ms\":$B}" \
        "$API/streams/live/news" >"$RUN/code-b" &
    PB=$!

    wait "$PA" "$PB"

    RA="$(field "$(cat "$RUN/patch-a.json")" revision)"
    RB="$(field "$(cat "$RUN/patch-b.json")" revision)"

    [ -n "$RA" ] && [ -n "$RB" ] \
        || { echo "round $round: no revision in a patch response" >&2
             cat "$RUN/patch-a.json" "$RUN/patch-b.json" >&2; exit 1; }

    # the higher revision is the newer state, and it is what every worker has
    # to be holding once the round settles
    if [ "$RA" -gt "$RB" ]; then WINNER="$RA"; else WINNER="$RB"; fi

    for _ in $(seq 1 100); do
        CONVERGED=1
        for _ in $(seq 1 "$READS"); do
            curl -fsS "$API/streams/live/news" >>"$RUN/reads.json"
            printf '\n' >>"$RUN/reads.json"
        done

        while read -r line; do
            [ -n "$line" ] || continue

            R="$(field "$line" revision)"
            V="$(field "$line" failure_timeout_ms)"
            O="$(field "$line" observed_here)"

            [ "$O" = "true" ] && SEEN_OWNER=1
            [ "$O" = "false" ] && SEEN_REPLICA=1

            [ "$R" = "$WINNER" ] || CONVERGED=0
            [ "$V" = "$A" ] || [ "$V" = "$B" ] || CONVERGED=0
        done <"$RUN/reads.json"

        rm -f "$RUN/reads.json"

        [ "$CONVERGED" = "1" ] && break
        sleep 0.1
    done

    [ "$CONVERGED" = "1" ] \
        || { echo "round $round: the workers did not converge on revision" \
                "$WINNER (patches were $RA and $RB)" >&2
             for _ in $(seq 1 "$READS"); do
                 curl -fsS "$API/streams/live/news" | head -c 300 >&2
                 printf '\n' >&2
             done
             exit 1; }

    # the winning value is the one the winner revision carried
    VALUE="$(field "$(curl -fsS "$API/streams/live/news")" failure_timeout_ms)"

    if [ "$WINNER" = "$RA" ]; then EXPECT="$A"; else EXPECT="$B"; fi

    [ "$VALUE" = "$EXPECT" ] \
        || { echo "round $round: revision $WINNER carried $EXPECT but the" \
                "stream holds $VALUE" >&2; exit 1; }
done

echo "   $ROUNDS rounds converged on the newer revision"

[ "$SEEN_OWNER" = "1" ] && [ "$SEEN_REPLICA" = "1" ] \
    || { echo "the reads never reached both workers (owner=$SEEN_OWNER," \
            "replica=$SEEN_REPLICA): the case proves nothing" >&2; exit 1; }

echo "== both workers were read, and agreed"

grep -q 'stale' "$RUN/logs/error.log" && echo "   stale operations were dropped"

echo "== an operation in flight cannot resurrect a deleted stream"
for round in $(seq 1 "$ROUNDS"); do
    STATUS="$(curl -sS -o /dev/null -w '%{http_code}' \
        -X POST -H 'Content-Type: application/json' \
        -d '{"application":"live","name":"news"}' "$API/streams")"

    # 200 when the previous round's stream is still there, 201 when it is new:
    # a create is idempotent either way
    [ "$STATUS" = "201" ] || [ "$STATUS" = "200" ] \
        || { echo "round $round: create failed: $STATUS" >&2; exit 1; }

    # one worker adds a source while another deletes the stream: the delete can
    # land first on the other worker, and the source operation - which carries
    # the stream implicitly - must not create it again there
    curl -sS -o /dev/null -X POST -H 'Content-Type: application/json' \
        -d '{"id":"racer","type":"srt","priority":10}' \
        "$API/streams/live/news/sources" &
    PS=$!

    curl -sS -o "$RUN/delete.json" -X DELETE "$API/streams/live/news" &
    PD=$!

    wait "$PS" "$PD"

    DELETED_REV="$(field "$(cat "$RUN/delete.json")" revision)"

    [ -n "$DELETED_REV" ] \
        || { echo "round $round: the delete reported no revision" >&2
             cat "$RUN/delete.json" >&2; exit 1; }

    # give every tick and every in-flight operation time to land
    sleep 0.4

    rm -f "$RUN/after.json"

    for _ in $(seq 1 "$READS"); do
        curl -sS -o "$RUN/read.json" -w '%{http_code}\n' \
            "$API/streams/live/news" >>"$RUN/after.json"

        if [ "$(head -1 "$RUN/after.json")" = "200" ]; then
            printf '%s\n' "$(cat "$RUN/read.json")" >>"$RUN/alive.json"
        fi

        # rewrite the codes without the body curl just wrote
        tail -n +2 "$RUN/after.json" >"$RUN/after.tmp"
        mv "$RUN/after.tmp" "$RUN/after.json"
    done

    while read -r code; do
        [ -n "$code" ] || continue

        [ "$code" = "404" ] || [ "$code" = "200" ] \
            || { echo "round $round: unexpected read status $code" >&2; exit 1; }
    done <"$RUN/after.json"

    # A stream may survive a delete only when a *newer* operation superseded
    # it - the source that raced the delete and was applied after it.  A
    # stream at or below the revision the delete moved past is the bug this
    # case is for: the deletion reached one worker and not the other.
    if [ -f "$RUN/alive.json" ]; then
        while read -r line; do
            [ -n "$line" ] || continue

            R="$(field "$line" revision)"

            [ -n "$R" ] && [ "$R" -gt "$DELETED_REV" ] \
                || { echo "round $round: a stream at revision ${R:-?} came" \
                        "back after a delete at $DELETED_REV" >&2
                     printf '%s\n' "$line" | head -c 300 >&2
                     printf '\n' >&2
                     exit 1; }
        done <"$RUN/alive.json"

        echo "   round $round: the delete was superseded by a newer write"
    fi

    rm -f "$RUN/after.json" "$RUN/alive.json"
done

echo "   $ROUNDS rounds: no worker kept a stream its delete had moved past"

trap - EXIT
cleanup

echo "== graph conflict ok"

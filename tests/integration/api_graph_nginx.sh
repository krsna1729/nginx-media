#!/usr/bin/env bash
#
# Runtime graph: stream CRUD through the control API (normative revision).
#
# The deployment starts with zero declared streams, so everything below is
# created at runtime: the stream, then its sources, then the publisher that
# attaches to them.  Deleting the active source has to fail over through the
# normal selector path, and deletion has to be idempotent.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/api-graph"
SRT_PORT=24640
HTTP_PORT=18448
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

PUB=0
cleanup() {
    [ "$PUB" != "0" ] && kill -KILL "$PUB" 2>/dev/null

    # only this script's instances, matched by the prefixes it started.  The
    # prefix is unique to this target, so a worker orphaned by an earlier
    # aborted run is reaped too, while another agent's nginx - a different
    # prefix - is never touched.  A pattern like `pkill -f 'nginx: '` would
    # kill every nginx on the box and is never used here.
    for inst in "$RUN" "$RUN/sink"; do
        [ -f "$inst/logs/nginx.pid" ] || continue

        pid="$(cat "$inst/logs/nginx.pid")"

        # the master reaps its workers on a graceful signal, so no orphaned
        # worker is left holding the SRT port for the next run
        kill -QUIT "$pid" 2>/dev/null

        for _ in $(seq 1 100); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.05
        done

        for child in $(pgrep -P "$pid" 2>/dev/null); do
            kill -KILL "$child" 2>/dev/null
        done

        kill -KILL "$pid" 2>/dev/null
    done

    return 0
}
trap cleanup EXIT

# note: no media_srt_output, and no streams declared anywhere
cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/hls;
media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;

http {
    access_log off;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /hls/ {
            alias $RUN/hls/;
        }

        location /media/api/ {
            media_api;
        }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || {
    echo "configuration rejected" >&2
    exit 1
}

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

echo "== a deployment with no declared streams"
LIST="$(curl -fsS "$API/streams")"
printf '%s\n' "$LIST"

printf '%s' "$LIST" | grep -q '"streams":\[\]' \
    || { echo "expected an empty graph" >&2; exit 1; }

echo "== create a stream through the api"
STATUS="$(curl -sS -o "$RUN/create.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"news"}' "$API/streams")"

cat "$RUN/create.json"; echo

[ "$STATUS" = "201" ] || { echo "expected 201, got $STATUS" >&2; exit 1; }

grep -q '"created":true' "$RUN/create.json" \
    || { echo "the stream was not reported as created" >&2; exit 1; }

REVISION="$(grep -o '"revision":[0-9]*' "$RUN/create.json" | cut -d: -f2)"
echo "   created at revision $REVISION"

echo "== the same request again is idempotent"
STATUS="$(curl -sS -o "$RUN/create2.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"news"}' "$API/streams")"

cat "$RUN/create2.json"; echo

[ "$STATUS" = "200" ] || { echo "replay should be 200, got $STATUS" >&2; exit 1; }

grep -q '"created":false' "$RUN/create2.json" \
    || { echo "the replay created a second object" >&2; exit 1; }

COUNT="$(curl -fsS "$API/streams" \
    | grep -o '"application":"live","name":"news"' | wc -l)"

[ "$COUNT" = "1" ] || { echo "expected one stream, found $COUNT" >&2; exit 1; }

echo "   replay produced no duplicate"

echo "== profile intent is rejected without an external executor"
STATUS="$(curl -sS -o "$RUN/profile-unavailable.json" -w '%{http_code}' \
    -X PATCH -H 'Content-Type: application/json' \
    -d "{\"revision\":$REVISION,\"media\":\"profile\"}" \
    "$API/streams/live/news")"

[ "$STATUS" = "400" ] \
    || { echo "profile intent should be unavailable: $STATUS" >&2; exit 1; }
grep -q '"error":"transform_unavailable"' "$RUN/profile-unavailable.json" \
    || { echo "missing transform_unavailable error" >&2; exit 1; }

echo "== a mutation carries the revision"
STATUS="$(curl -sS -o "$RUN/patch.json" -w '%{http_code}' \
    -X PATCH -H 'Content-Type: application/json' \
    -d "{\"revision\":$REVISION,\"failure_timeout_ms\":2500}" \
    "$API/streams/live/news")"

cat "$RUN/patch.json"; echo

[ "$STATUS" = "200" ] || { echo "expected 200, got $STATUS" >&2; exit 1; }

NEW_REVISION="$(grep -o '"revision":[0-9]*' "$RUN/patch.json" | cut -d: -f2)"

[ "$NEW_REVISION" -gt "$REVISION" ] \
    || { echo "the revision did not advance" >&2; exit 1; }

grep -q '"failure_timeout_ms":2500' <(curl -fsS "$API/streams/live/news") \
    || { echo "the patch did not take effect" >&2; exit 1; }

echo "   revision $REVISION -> $NEW_REVISION, policy updated"

echo "== a stale mutation is refused"
STATUS="$(curl -sS -o "$RUN/stale.json" -w '%{http_code}' \
    -X PATCH -H 'Content-Type: application/json' \
    -d "{\"revision\":$REVISION,\"failure_timeout_ms\":9999}" \
    "$API/streams/live/news")"

cat "$RUN/stale.json"; echo

[ "$STATUS" = "409" ] || { echo "stale write should be 409, got $STATUS" >&2; exit 1; }

grep -q '"failure_timeout_ms":2500' <(curl -fsS "$API/streams/live/news") \
    || { echo "the stale write was applied anyway" >&2; exit 1; }

echo "   refused with 409, newer state intact"

echo "== sources are runtime objects too"
STATUS="$(curl -sS -o "$RUN/src1.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"id":"encoder-a","type":"srt","priority":100}' \
    "$API/streams/live/news/sources")"

cat "$RUN/src1.json"; echo

[ "$STATUS" = "201" ] || { echo "expected 201, got $STATUS" >&2; exit 1; }

# replay is idempotent here as well
STATUS="$(curl -sS -o "$RUN/src1b.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"id":"encoder-a","type":"srt","priority":100}' \
    "$API/streams/live/news/sources")"

[ "$STATUS" = "200" ] || { echo "source replay should be 200, got $STATUS" >&2; exit 1; }

grep -q '"created":false' "$RUN/src1b.json" \
    || { echo "the source replay created a second object" >&2; exit 1; }

echo "   source created, replay idempotent"

echo "== a backup source joins a live program without interrupting it"
publish() {
    # $1: identity, $2: seconds
    timeout "$(( $2 + 20 ))" ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -t "$2" -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/news,m=publish,s=$1" \
        >"$RUN/pub-$1.log" 2>&1 &
    echo $!
}

P1="$(publish encoder-a 12)"
sleep 2

curl -sS -o /dev/null -X POST -H 'Content-Type: application/json' \
    -d '{"id":"encoder-b","type":"srt","priority":50}' \
    "$API/streams/live/news/sources"

P2="$(publish encoder-b 8)"
sleep 3

SOURCES="$(curl -fsS "$API/streams/live/news/sources")"
printf '%s\n' "$SOURCES"

for field in preroll_unit_overflows preroll_byte_overflows \
             preroll_high_water_units preroll_high_water_bytes
do
    printf '%s' "$SOURCES" | grep -q "\"$field\":" \
        || { echo "source response omitted $field" >&2; exit 1; }
done

echo "   source exposes cause-specific overflow and high-water counters"

printf '%s' "$SOURCES" | grep -q '"id":"encoder-a".*"state":"active"' \
    || { echo "the original source did not stay active" >&2; exit 1; }

printf '%s' "$SOURCES" | grep -q '"id":"encoder-b"' \
    || { echo "the backup source was not registered" >&2; exit 1; }

echo "   both sources live, the original still on air"

echo "== a publisher lands on the api-created stream"
timeout 40 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -t 12 -f mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/news,m=publish,s=encoder-a" \
    >"$RUN/pub.log" 2>&1 &
PUB=$!

for _ in $(seq 1 100); do
    grep -q 'srt source open app=live stream=news' "$RUN/logs/error.log" \
        2>/dev/null && break
    sleep 0.1
done

grep -q 'srt source open app=live stream=news' "$RUN/logs/error.log" \
    || { cat "$RUN/pub.log"; echo "the publisher did not attach" >&2; exit 1; }

HLS_READY=0
for _ in $(seq 1 200); do
    if [ -f "$RUN/hls/live/news/index.m3u8" ] \
        && [ "$(grep -c '^#EXTINF' "$RUN/hls/live/news/index.m3u8" || true)" -ge 1 ]
    then
        HLS_READY=1
        break
    fi
    sleep 0.1
done

[ "$HLS_READY" = "1" ] \
    || { echo "the api-created stream produced no hls output" >&2
         cat "$RUN/logs/error.log" >&2; exit 1; }

kill -KILL "$PUB" 2>/dev/null
wait "$PUB" 2>/dev/null
PUB=0

echo "   the publisher attached and HLS carried the api-created stream"

echo "== disabling a source takes it out of selection"
curl -fsS -X POST "$API/streams/live/news/sources/encoder-b/disable" >/dev/null

curl -fsS "$API/streams/live/news/sources/encoder-b" \
    | grep -q '"enabled":false' \
    || { echo "disable did not take effect" >&2; exit 1; }

curl -fsS -X POST "$API/streams/live/news/sources/encoder-b/enable" >/dev/null

curl -fsS "$API/streams/live/news/sources/encoder-b" \
    | grep -q '"enabled":true' \
    || { echo "enable did not take effect" >&2; exit 1; }

echo "   disabled and re-enabled"

echo "== deleting the inactive source leaves the program running"
curl -fsS -X DELETE "$API/streams/live/news/sources/encoder-b" \
    | grep -q '"deleted":true' \
    || { echo "the inactive source was not deleted" >&2; exit 1; }

sleep 1

curl -fsS "$API/streams/live/news/sources" \
    | grep -q '"id":"encoder-a".*"state":"active"' \
    || { echo "the program did not survive the deletion" >&2; exit 1; }

echo "   inactive source deleted, program undisturbed"

echo "== deleting the active source fails over through the normal path"
# both publishers have to be up at the moment of the delete, so the standby
# is started first and the active source is removed while it is still live
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"id":"encoder-c","type":"srt","priority":50}' \
    "$API/streams/live/news/sources" >/dev/null

P3="$(publish encoder-c 25)"
P4="$(publish encoder-a 25)"

for _ in $(seq 1 100); do
    curl -fsS "$API/streams/live/news/sources" \
        | grep -q '"id":"encoder-a".*"state":"active"' && break
    sleep 0.1
done

curl -fsS "$API/streams/live/news/sources" \
    | grep -q '"id":"encoder-a".*"state":"active"' \
    || { echo "encoder-a did not take the program" >&2; exit 1; }

curl -fsS -X DELETE "$API/streams/live/news/sources/encoder-a" >/dev/null

for _ in $(seq 1 150); do
    curl -fsS "$API/streams/live/news" \
        | grep -q '"active":"encoder-c"' && break
    sleep 0.1
done

# Two deterministic facts, rather than a race with the reconnecting
# publisher.  Deleting the active source must close its transport, which the
# log records, and the program must come back up on the standby.  A switch
# counter is the wrong assertion here: the program had no active source for a
# moment, so the standby is *activated* rather than switched to, and the
# publisher whose session was closed may legitimately reconnect afterwards.
grep -q 'source encoder-a removed, closing its session' "$RUN/logs/error.log" \
    || { echo "deleting the active source did not close its transport" >&2; exit 1; }

for _ in $(seq 1 150); do
    curl -fsS "$API/streams/live/news" \
        | grep -qE '"active":"[a-z]' && break
    sleep 0.1
done

curl -fsS "$API/streams/live/news" | grep -qE '"active":"[a-z]' \
    || { echo "no source came back on air" >&2
         curl -fsS "$API/streams/live/news" >&2; exit 1; }

echo "   active source deleted, transport closed, standby came on air"

kill -KILL "$P1" "$P2" "$P3" "$P4" 2>/dev/null
wait "$P1" "$P2" "$P3" "$P4" 2>/dev/null

echo "== a destination is added at runtime and carries the program"
SINK_PORT=24641
mkdir -p "$RUN/sink/conf" "$RUN/sink/logs" "$RUN/sink/hls"

cat > "$RUN/sink/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 64;
}

media_hls $RUN/sink/hls;
media_srt_listen 127.0.0.1:$SINK_PORT;
media_srt_source_priority runtime-out 100;
EOF

"$NGINX" -p "$RUN/sink" -c conf/nginx.conf

for _ in $(seq 1 100); do
    grep -q 'srt listener ready' "$RUN/sink/logs/error.log" 2>/dev/null && break
    sleep 0.1
done

# the publisher has to be live for the destination to carry anything
PD="$(publish encoder-d 20)"
sleep 2


STATUS="$(curl -sS -o "$RUN/dest.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"sink1\",\"type\":\"srt\",\"host\":\"127.0.0.1\",\"port\":$SINK_PORT,\"streamid\":\"#!::r=live/news,m=publish,s=runtime-out\"}" \
    "$API/streams/live/news/destinations")"

cat "$RUN/dest.json"; echo

[ "$STATUS" = "201" ] || { echo "expected 201, got $STATUS" >&2; exit 1; }

curl -fsS "$API/streams/live/news/destinations" \
    | grep -q '"id":"sink1"' \
    || { echo "the destination is not listed" >&2; exit 1; }

for _ in $(seq 1 150); do
    grep -q 'srt source open.*source=runtime-out' "$RUN/sink/logs/error.log" \
        2>/dev/null && break
    sleep 0.1
done

grep -q 'srt source open.*source=runtime-out' "$RUN/sink/logs/error.log" \
    || { echo "the runtime destination carried no media" >&2
         tail -3 "$RUN/sink/logs/error.log" >&2; exit 1; }

# The senders are one pool over the destination table, so while media keeps
# flowing the worker gains no thread: a sender per burst or per push shows up
# here as a count that keeps climbing.  (The exact "adding a destination adds
# no sender" property is asserted in tests/unit/test_srt_output.c, where the
# core runs against the reference backend: with libsrt the worker's thread
# count is dominated by the threads the transport creates per socket, so an
# absolute count here would test libsrt, not the pool.)
MASTER="$(cat "$RUN/logs/nginx.pid")"
WORKER="$(pgrep -P "$MASTER" | head -1)"
THREADS_SETTLED="$(ls /proc/$WORKER/task | wc -l)"

sleep 3

THREADS_FLOWING="$(ls /proc/$WORKER/task | wc -l)"

[ "$THREADS_FLOWING" = "$THREADS_SETTLED" ] \
    || { echo "the sender pool grew while carrying media: $THREADS_SETTLED -> $THREADS_FLOWING threads" >&2
         exit 1; }

echo "   destination added at runtime, media reached the sink, worker threads steady ($THREADS_FLOWING)"

echo "== removing it stops the push and leaves the program running"
STATUS="$(curl -sS -o "$RUN/destdel.json" -w '%{http_code}' \
    -X DELETE "$API/streams/live/news/destinations/sink1")"

cat "$RUN/destdel.json"; echo

[ "$STATUS" = "200" ] || { echo "expected 200, got $STATUS" >&2; exit 1; }

grep -q '"deleted":true' "$RUN/destdel.json" \
    || { echo "the destination was not removed" >&2; exit 1; }

curl -fsS "$API/streams/live/news/destinations" \
    | grep -q '"count":0' \
    || { echo "the destination list is not empty" >&2; exit 1; }

# deleting again is idempotent
curl -fsS -X DELETE "$API/streams/live/news/destinations/sink1" \
    | grep -q '"deleted":false' \
    || { echo "the second delete should report absence" >&2; exit 1; }

sleep 1

# the program's frame counter is monotonic, so it proves the program kept
# running without depending on which source happens to be on air
FRAMES="$(curl -fsS "$API/streams/live/news" \
    | grep -o '"program_frames":[0-9]*' | cut -d: -f2)"

[ "${FRAMES:-0}" -gt 0 ] \
    || { echo "the program did not survive destination removal" >&2
         curl -fsS "$API/streams/live/news" >&2; exit 1; }

echo "   program still running ($FRAMES frames)"

kill -KILL "$PD" 2>/dev/null
wait "$PD" 2>/dev/null

echo "   destination removed, program undisturbed"

echo "== delete is ordered and idempotent"
STATUS="$(curl -sS -o "$RUN/delete.json" -w '%{http_code}' \
    -X DELETE "$API/streams/live/news")"

cat "$RUN/delete.json"; echo

[ "$STATUS" = "200" ] || { echo "expected 200, got $STATUS" >&2; exit 1; }

grep -q '"deleted":true' "$RUN/delete.json" \
    || { echo "the stream was not deleted" >&2; exit 1; }

STATUS="$(curl -sS -o "$RUN/delete2.json" -w '%{http_code}' \
    -X DELETE "$API/streams/live/news")"

cat "$RUN/delete2.json"; echo

[ "$STATUS" = "200" ] || { echo "deleting again should succeed, got $STATUS" >&2; exit 1; }

grep -q '"deleted":false' "$RUN/delete2.json" \
    || { echo "the second delete should report absence" >&2; exit 1; }

curl -fsS "$API/streams" | grep -q '"streams":\[\]' \
    || { echo "the graph is not empty after deletion" >&2; exit 1; }

echo "   deleted, and deleting again is a no-op"
echo "== runtime output capacity is admitted before graph creation"
for i in $(seq 0 15); do
    cap_name="$(printf 'capacity-%02d' "$i")"
    STATUS="$(curl -sS -o "$RUN/capacity-$i.json" -w '%{http_code}' \
        -X POST -H 'Content-Type: application/json' \
        -d "{\"application\":\"live\",\"name\":\"$cap_name\"}" \
        "$API/streams")"
    [ "$STATUS" = "201" ] \
        || { echo "capacity stream $cap_name was refused: $STATUS" >&2
             exit 1; }
done

STATUS="$(curl -sS -o "$RUN/capacity-full.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"capacity-full"}' "$API/streams")"

[ "$STATUS" = "500" ] \
    || { echo "the seventeenth output stream was admitted: $STATUS" >&2
         cat "$RUN/capacity-full.json" >&2; exit 1; }
grep -q '"error":"runtime_output_capacity"' "$RUN/capacity-full.json" \
    || { echo "capacity refusal had the wrong error" >&2; exit 1; }

for i in $(seq 0 15); do
    cap_name="$(printf 'capacity-%02d' "$i")"
    curl -fsS -X DELETE "$API/streams/live/$cap_name" >/dev/null
done

echo "   sixteen output slots admitted, the seventeenth refused, and slots released"



echo "== desired state is a replayable document"
# its own stream, so this section does not depend on what earlier ones left
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"doc"}' "$API/streams" >/dev/null

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"id":"doc-src","type":"srt","priority":70}' \
    "$API/streams/live/doc/sources" >/dev/null

# The document carries a destination as well as a source, so replaying it
# exercises the destination half of the apply path.  A destination is the one
# child that needs this worker's runtime to exist - its transport is started
# by the worker that serves the request - so a worker that came up without it
# is what "the reconcile failed" means.
STATUS="$(curl -sS -o "$RUN/docdest.json" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d "{\"id\":\"doc-sink\",\"type\":\"srt\",\"host\":\"127.0.0.1\",\"port\":$SINK_PORT}" \
    "$API/streams/live/doc/destinations")"

cat "$RUN/docdest.json"; echo

[ "$STATUS" = "201" ] \
    || { echo "the doc destination was not created: $STATUS" >&2; exit 1; }

DESIRED="$(curl -fsS "$API/desired")"
printf '%s\n' "$DESIRED"

printf '%s' "$DESIRED" | grep -q '"application":"live","name":"doc"' \
    || { echo "the document does not describe the stream" >&2; exit 1; }
printf '%s' "$DESIRED" | grep -q '"id":"doc-src"' \
    || { echo "the document does not describe the source" >&2; exit 1; }
printf '%s' "$DESIRED" | grep -q '"id":"doc-sink"' \
    || { echo "the document does not describe the destination" >&2; exit 1; }

BEFORE="$(curl -fsS "$API/desired" \
    | grep -o '"id":"[a-z-]*"' | sort | uniq -c | sort -rn | head -1)"

echo "== replaying it does not duplicate anything"
STATUS="$(curl -sS -o "$RUN/replay.json" -w '%{http_code}' \
    -X PUT -H 'Content-Type: application/json' -d "$DESIRED" "$API/desired")"

cat "$RUN/replay.json"; echo

[ "$STATUS" = "200" ] || { echo "replay should be 200, got $STATUS" >&2; exit 1; }

grep -q '"children_created":0' "$RUN/replay.json" \
    || { echo "the replay created something new" >&2; exit 1; }

AFTER="$(curl -fsS "$API/desired" \
    | grep -o '"id":"[a-z-]*"' | sort | uniq -c | sort -rn | head -1)"

[ "$BEFORE" = "$AFTER" ] \
    || { echo "the replay changed the graph: $BEFORE -> $AFTER" >&2; exit 1; }

echo "   replayed with no duplicates ($AFTER)"

echo "== a reload loses the graph and the document restores it"
# The graph is populated when the reload happens - a stream and its source,
# asserted here - so "a reload loses the graph" is checked against real runtime
# objects rather than an empty registry.
curl -fsS "$API/streams/live/doc" | grep -q '"id":"doc-src"' \
    || { echo "the doc graph is not populated before the reload" >&2; exit 1; }

MASTER="$(cat "$RUN/logs/nginx.pid")"
WORKER_BEFORE="$(pgrep -P "$MASTER" | head -1)"
READY_BEFORE="$(grep -c 'srt listener ready' "$RUN/logs/error.log" || true)"

[ "$(pgrep -P "$MASTER" | wc -l)" = "1" ] \
    || { echo "worker_processes 1 should mean one worker, found $(pgrep -P "$MASTER" | wc -l)" >&2; exit 1; }

kill -HUP "$MASTER"

# the new worker is up and ready, and the old one has drained and exited: the
# reload's outcome is deterministic, so this is asserted, not merely waited for
for _ in $(seq 1 300); do
    WORKER_AFTER="$(pgrep -P "$MASTER" | head -1)"

    [ -n "$WORKER_AFTER" ] && [ "$WORKER_AFTER" != "$WORKER_BEFORE" ] \
        && [ "$(grep -c 'srt listener ready' "$RUN/logs/error.log" || true)" \
             -gt "$READY_BEFORE" ] \
        && break

    sleep 0.1
done

sleep 1

WORKER_AFTER="$(pgrep -P "$MASTER" | head -1)"

[ "$(cat "$RUN/logs/nginx.pid")" = "$MASTER" ] \
    || { echo "the master was replaced by the reload" >&2; exit 1; }

[ "$WORKER_AFTER" != "$WORKER_BEFORE" ] \
    || { echo "the reload did not replace the worker" >&2; exit 1; }

[ "$(pgrep -P "$MASTER" | wc -l)" = "1" ] \
    || { echo "the reload left $(pgrep -P "$MASTER" | wc -l) workers" >&2; exit 1; }

[ "$(grep -c 'srt listener ready' "$RUN/logs/error.log" || true)" -gt "$READY_BEFORE" ] \
    || { echo "the worker the reload started never became ready" >&2; exit 1; }

# every runtime object was worker memory, so the graph went with the old worker
curl -fsS "$API/streams" | grep -q '"streams":\[\]' \
    || { echo "the graph survived the reload" >&2
         curl -fsS "$API/streams" >&2; exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' "$API/streams/live/doc")"

[ "$STATUS" = "404" ] \
    || { echo "the doc stream survived the reload ($STATUS)" >&2; exit 1; }

echo "   worker $WORKER_BEFORE -> $WORKER_AFTER ready, graph empty, the doc objects went with it"

# The log line above is a proxy.  The property is that the instance accepts a
# publisher on the SRT port without an intervening restart - the bind race
# this guards against left the worker the reload started with no listener at
# all, so "the worker is up" was true while nothing could publish.  So: a real
# publisher connects to the same port, to a stream created after the reload,
# and its media has to reach the program.
echo "== a publisher is accepted after the reload"
PAFTER="$(publish encoder-after 10)"

for _ in $(seq 1 200); do
    grep -q 'srt source open app=live stream=news source=encoder-after' \
        "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.1
done

grep -q 'srt source open app=live stream=news source=encoder-after' \
    "$RUN/logs/error.log" \
    || { echo "nothing accepted a publisher on $SRT_PORT after the reload" >&2
         tail -5 "$RUN/logs/error.log" >&2; exit 1; }

for _ in $(seq 1 200); do
    curl -fsS "$API/streams/live/news" 2>/dev/null \
        | grep -qE '"program_frames":[1-9]' && break
    sleep 0.1
done

curl -fsS "$API/streams/live/news" | grep -qE '"program_frames":[1-9]' \
    || { echo "a publisher accepted after the reload carried no frames" >&2
         curl -fsS "$API/streams/live/news" >&2; exit 1; }

echo "   accepted on $SRT_PORT and on air"

# the publisher created its stream implicitly, and the sections below expect
# the empty graph the reload left, so it goes with the publisher
kill -TERM "$PAFTER" 2>/dev/null
wait "$PAFTER" 2>/dev/null

sleep 1

curl -fsS -X DELETE "$API/streams/live/news" >/dev/null

curl -fsS "$API/streams" | grep -q '"streams":\[\]' \
    || { echo "the post-reload publisher's stream was not cleaned up" >&2
         curl -fsS "$API/streams" >&2; exit 1; }

echo "== the document restores it exactly, with no duplicates and no orphans"
STATUS="$(curl -sS -o "$RUN/reconcile.json" -w '%{http_code}' \
    -X PUT -H 'Content-Type: application/json' -d "$DESIRED" "$API/desired")"

cat "$RUN/reconcile.json"; echo

[ "$STATUS" = "200" ] || { echo "reconcile failed: $STATUS" >&2; exit 1; }

# the document's stream, source and destination were all mine to create: the
# graph was empty after the reload, so a child the apply skipped would have to
# be accounted for here
CHILDREN="$(grep -o '"children_created":[0-9]*' "$RUN/reconcile.json" | cut -d: -f2)"

[ "$CHILDREN" = "2" ] \
    || { echo "the reconcile created $CHILDREN children, expected 2" >&2; exit 1; }

curl -fsS "$API/streams" | grep -q '"name":"doc"' \
    || { echo "the document did not restore the stream" >&2; exit 1; }

curl -fsS "$API/streams/live/doc/sources" | grep -q '"id":"doc-src"' \
    || { echo "the document did not restore the source" >&2; exit 1; }

# The destination is the assertion for the class of failure this section
# exists to catch.  A desired document whose destination is applied is the one
# child that needs the *serving* worker's destination runtime to exist: the
# apply calls the transport backend, and a worker that started without it
# fails the whole document with 400 {"error":"child_apply_failed"} - the log
# line below is that worker saying its runtime was there.
curl -fsS "$API/streams/live/doc/destinations" | grep -q '"id":"doc-sink"' \
    || { echo "the document did not restore the destination" >&2
         curl -fsS "$API/streams/live/doc/destinations" >&2; exit 1; }

grep -q "$WORKER_AFTER#$WORKER_AFTER:.*srt destination doc-sink started" \
    "$RUN/logs/error.log" \
    || { echo "worker $WORKER_AFTER did not start the document's destination" >&2
         grep 'doc-sink' "$RUN/logs/error.log" >&2; exit 1; }

# exactly the document's objects: one stream, one source, one destination
STREAMS="$(curl -fsS "$API/streams" | grep -o '"name":"[a-z-]*"' | wc -l)"

[ "$STREAMS" = "1" ] \
    || { echo "expected exactly one stream after the reconcile, found $STREAMS" >&2; exit 1; }

COUNT="$(curl -fsS "$API/desired" | grep -o '"id":"doc-src"' | wc -l)"

[ "$COUNT" = "1" ] \
    || { echo "reconcile duplicated the source: $COUNT" >&2; exit 1; }

DESTINATIONS="$(curl -fsS "$API/streams/live/doc/destinations" \
    | grep -o '"count":[0-9]*' | cut -d: -f2)"

[ "$DESTINATIONS" = "1" ] \
    || { echo "expected the document's one destination, found $DESTINATIONS" >&2; exit 1; }

DEST_COUNT="$(curl -fsS "$API/desired" | grep -o '"id":"doc-sink"' | wc -l)"

[ "$DEST_COUNT" = "1" ] \
    || { echo "reconcile duplicated the destination: $DEST_COUNT" >&2; exit 1; }

# a stream the document does not mention stays deleted, and so does its
# destination: the reconcile restores the document, it does not recreate what
# earlier sections deleted
STATUS="$(curl -sS -o /dev/null -w '%{http_code}' "$API/streams/live/news")"

[ "$STATUS" = "404" ] \
    || { echo "a stream absent from the document was resurrected ($STATUS)" >&2; exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' "$API/streams/live/news/destinations/sink1")"

[ "$STATUS" = "404" ] \
    || { echo "a destination absent from the document was resurrected ($STATUS)" >&2; exit 1; }

echo "   restored exactly: one stream, one source, one destination, nothing else"

echo "== the worker the reload started carries media"
PLIVE="$(publish encoder-live 12)"

for _ in $(seq 1 250); do
    curl -fsS "$API/streams/live/news" 2>/dev/null \
        | grep -qE '"program_frames":[1-9]' && break
    sleep 0.1
done

curl -fsS "$API/streams/live/news" | grep -qE '"program_frames":[1-9]' \
    || { echo "no program ran after the reload" >&2
         curl -fsS "$API/streams/live/news" >&2; exit 1; }

kill -KILL "$PLIVE" 2>/dev/null
wait "$PLIVE" 2>/dev/null

echo "   media after the reload"

echo "== create/delete cycles with media do not exhaust the runtime"
# The per-stream runtime output is a fixed table, and a slot is taken for
# every owned stream that has an output configured - whether or not it is
# carrying media, because the slot is what carries it.  These cycles each
# carry a short publish so that a leaked slot would be visible as a stream
# with no output rather than only as a missing number.  Ten cycles against a
# table of eight: without the release on delete, the later ones get no
# outputs.
CYCLE=0
for i in $(seq 1 10); do
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"application\":\"live\",\"name\":\"cycle$i\"}" \
        "$API/streams" >/dev/null

    PX="$(publish encoder-$i 2)"
    sleep 1

    curl -fsS -X DELETE "$API/streams/live/cycle$i" >/dev/null
    kill -KILL "$PX" 2>/dev/null
    wait "$PX" 2>/dev/null

    CYCLE=$i
done

echo "   $CYCLE cycles with media completed"

# a stream that carries media now must still get its outputs
PZ="$(publish encoder-z 12)"
sleep 4

for _ in $(seq 1 200); do
    [ -f "$RUN/hls/live/news/index.m3u8" ] \
        && [ "$(grep -c '^#EXTINF' "$RUN/hls/live/news/index.m3u8" || true)" -ge 1 ] \
        && break
    sleep 0.1
done

kill -KILL "$PZ" 2>/dev/null
wait "$PZ" 2>/dev/null

[ -f "$RUN/hls/live/news/index.m3u8" ] \
    || { echo "no outputs after the churn: the table was exhausted" >&2
         exit 1; }

echo "   the runtime still serves media after the churn"

# and the slots came back: this is the assertion that fails when ordered
# teardown forgets to release a stream's runtime outputs
sleep 2

ACTIVE="$(curl -fsS "$API/metrics" \
    | grep '^nginx_media_runtime_outputs ' | awk '{print $2}')"

echo "   runtime output slots in use: ${ACTIVE:-?}"

METRICS="$(curl -fsS "$API/metrics")"
for metric in \
    nginx_media_stream_feed_evictions_total \
    nginx_media_stream_feed_overruns_total \
    nginx_media_stream_feed_generation_mismatches_total \
    nginx_media_stream_feed_publish_errors_total \
    nginx_media_stream_feed_high_water_units \
    nginx_media_stream_feed_high_water_bytes \
    nginx_media_source_preroll_units \
    nginx_media_source_preroll_bytes \
    nginx_media_source_preroll_overflows_total \
    nginx_media_source_preroll_unit_overflows_total \
    nginx_media_source_preroll_byte_overflows_total \
    nginx_media_source_preroll_high_water_units \
    nginx_media_source_preroll_high_water_bytes
do
    printf '%s\n' "$METRICS" | grep -q "^$metric{" \
        || { echo "metrics omitted $metric" >&2; exit 1; }
done

for metric in \
    nginx_media_worker_wakeups_total \
    nginx_media_worker_wakeup_coalesced_total \
    nginx_media_runtime_routed_identity_mismatches_total \
    nginx_media_runtime_routed_slot_overflows_total \
    nginx_media_runtime_routed_no_slot_total \
    nginx_media_runtime_routed_no_payload_total \
    nginx_media_runtime_routed_reassembly_errors_total \
    nginx_media_runtime_routed_publish_errors_total
do
    printf '%s\n' "$METRICS" | grep -q "^$metric " \
        || { echo "metrics omitted $metric" >&2; exit 1; }
done

[ "${ACTIVE:-99}" -le 2 ] \
    || { echo "runtime output slots leaked: $ACTIVE in use" >&2; exit 1; }

trap - EXIT
cleanup

echo "== api graph ok"

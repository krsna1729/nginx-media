#!/usr/bin/env bash
#
# Health, failover and switchback through nginx (phase 4 exit criteria).
#
# Two publishers feed live/news with configured priorities.  The test kills the
# active publisher, freezes it, lets it recover, and finally replaces it with a
# corrupted one, asserting that the program recovers deterministically every
# time.
#
# The two encoders carry video of different sizes, and every promotion the
# selector takes is measured with one set of observables -- generation and
# switch counters, the discontinuity the playlist announces, the sync boundary
# the new generation starts on, and whether the demoted writer really left the
# program.  The same automatic promotion must look the same whichever trigger
# caused it, and a manual switch to a source that is already on air must not
# open a generation: both entry points are one promotion path.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/failover-nginx"
SRT_PORT="${FAILOVER_SRT_PORT:-19045}"
HTTP_PORT="${FAILOVER_HTTP_PORT:-18445}"
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
STREAMID='#!::r=live/news,m=publish,s='

WIDTH_A=320
WIDTH_B=640

PUB_A=0
PUB_B=0
PUB_C=0
PUB_PID=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf" "$RUN/hls"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_failover_failure_timeout 700;
media_failover_recovery_timeout 300;
media_failover_switchback auto;

media_hls $RUN/hls;
media_srt_listen 127.0.0.1:$SRT_PORT;
media_srt_source_priority encoder-a 100;
media_srt_source_priority encoder-b 90;

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

cleanup() {
    [ "$PUB_A" != "0" ] && kill -KILL "$PUB_A" 2>/dev/null || true
    [ "$PUB_B" != "0" ] && kill -KILL "$PUB_B" 2>/dev/null || true
    [ "$PUB_C" != "0" ] && kill -KILL "$PUB_C" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

api() {
    curl -fsS "$API$1"
}

active_source() {
    api /streams/live/news | sed -n 's/.*"active":"\([^"]*\)".*/\1/p'
}

switches() {
    api /streams/live/news | sed -n 's/.*"switches":\([0-9]*\).*/\1/p'
}

program_frames() {
    api /streams/live/news | sed -n 's/.*"program_frames":\([0-9]*\).*/\1/p'
}

wait_for_active() {
    local want="$1" tries="${2:-80}"

    for _ in $(seq 1 "$tries"); do
        if [ "$(active_source)" = "$want" ]; then
            return 0
        fi
        sleep 0.1
    done

    echo "timed out waiting for active=$want" >&2
    api /streams/live/news >&2
    return 1
}

wait_for_healthy() {
    local id="$1" tries="${2:-80}"

    for _ in $(seq 1 "$tries"); do
        if api /streams/live/news/sources \
            | grep -q "\"id\":\"$id\".*\"healthy\":true"; then
            return 0
        fi
        sleep 0.1
    done

    echo "source $id never became healthy" >&2
    return 1
}

# starts ffmpeg in the background and reports its real pid through PUB_PID
start_publisher() {
    local pattern="$1" freq="$2" source="$3" out="$4" seconds="$5"

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "$pattern" \
        -f lavfi -i "sine=frequency=$freq:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$seconds" -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=$STREAMID$source" \
        >"$RUN/$out.log" 2>&1 &

    PUB_PID=$!
}

# <field> of the stream detail, as plain text
stream_field() {
    api /streams/live/news | sed -n "s/.*\"$1\":\([0-9]*\).*/\1/p"
}

# the JSON object of one source, so that fields cannot be read from its
# neighbour
source_json() {
    api /streams/live/news/sources \
        | sed -n "s/.*\({\"id\":\"$1\"[^}]*}\).*/\1/p"
}

# <source-json> <field>
source_field() {
    printf '%s' "$1" | sed -n "s/.*\"$2\":\([^,}]*\).*/\1/p"
}

wait_for_writers_drained() {
    local id="$1" json

    for _ in $(seq 1 100); do
        json="$(source_json "$id")"

        # a source the worker has already detached holds no writer
        [ -z "$json" ] && return 0
        [ "$(source_field "$json" writers)" = "0" ] && return 0

        sleep 0.05
    done

    echo "$id still holds a writer after the promotion" >&2
    return 1
}

discontinuities() {
    grep -c '^#EXT-X-DISCONTINUITY$' "$RUN/hls/live/news/index.m3u8" 2>/dev/null || true
}

# the first segment of the newest generation: the one that follows the last
# discontinuity, which the playlist only carries once that segment is closed
new_generation_segment() {
    awk '/^#EXT-X-DISCONTINUITY$/ { found = 1; name = ""; next }
         /\.ts$/ { if (found) name = $0 }
         END { print name }' "$RUN/hls/live/news/index.m3u8"
}

wait_for_new_generation_segment() {
    local before="$1" name

    for _ in $(seq 1 300); do
        if [ "$(discontinuities)" -gt "$before" ]; then
            name="$(new_generation_segment)"

            if [ -n "$name" ] && [ -s "$RUN/hls/live/news/$name" ]; then
                printf '%s' "$name"
                return 0
            fi
        fi

        sleep 0.1
    done

    echo "no segment started after the promotion" >&2
    return 1
}

# video widths present in a segment, sorted and space separated
segment_widths() {
    ffprobe -hide_banner -loglevel error -select_streams v:0 \
        -show_entries frame=width -of csv=p=0 "$RUN/hls/live/news/$1" 2>/dev/null \
        | tr -d ',' | sort -u | tr '\n' ' '
}

segment_first_is_keyframe() {
    [ "$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
        -show_entries frame=key_frame -of csv=p=0 "$RUN/hls/live/news/$1" 2>/dev/null \
        | sed -n 1p | tr -d ',')" = "1" ]
}

# Snapshot the counters a promotion has to move, taken before it happens.
open_phase() {
    PHASE_GEN0="$(stream_field generation)"
    PHASE_SWITCHES0="$(stream_field switches)"
    PHASE_EMERGENCY0="$(stream_field emergency_switches)"
    PHASE_DISC0="$(discontinuities)"
}

# Measure the completed promotion of <promoted> over <demoted> and print one
# line of observable outcomes.  Every promotion in this test is measured with
# the same function, so the automatic ones can be compared as a whole instead
# of each being trusted on its own.
close_phase() {
    local label="$1" promoted="$2" demoted="$3" width="$4"
    local gen switches emergency disc segment demoted_json
    local demoted_before promoted_before demoted_after promoted_after

    wait_for_active "$promoted"
    wait_for_writers_drained "$demoted"

    # The new generation starts on a sync boundary and carries the selected
    # source only: a writer left over from the demoted source would splice its
    # own resolution into this segment.  The playlist only carries the segment
    # once it is closed, so this doubles as waiting for the promotion to be
    # announced.
    segment="$(wait_for_new_generation_segment "$PHASE_DISC0")"

    gen="$(stream_field generation)"
    switches="$(stream_field switches)"
    emergency="$(stream_field emergency_switches)"
    disc="$(discontinuities)"

    [ "$gen" = "$((PHASE_GEN0 + 1))" ] \
        || { echo "$label: generation went $PHASE_GEN0 -> $gen, not +1" >&2; return 1; }
    [ "$switches" = "$((PHASE_SWITCHES0 + 1))" ] \
        || { echo "$label: switches went $PHASE_SWITCHES0 -> $switches, not +1" >&2; return 1; }
    [ "$emergency" = "$PHASE_EMERGENCY0" ] \
        || { echo "$label: the promotion used the emergency path" >&2; return 1; }
    [ "$disc" -gt "$PHASE_DISC0" ] \
        || { echo "$label: the promotion was not announced to the playlist" >&2; return 1; }

    segment_first_is_keyframe "$segment" \
        || { echo "$label: $segment does not begin on a keyframe" >&2; return 1; }

    [ "$(segment_widths "$segment")" = "$width " ] \
        || { echo "$label: $segment carries $(segment_widths "$segment"), not ${width}px video only" >&2;
             return 1; }

    [ "$(api /streams/live/news/sources | grep -o '"active":true' | wc -l)" = "1" ] \
        || { echo "$label: not exactly one source is active" >&2; return 1; }

    demoted_json="$(source_json "$demoted")"

    if [ -n "$demoted_json" ]; then
        [ "$(source_field "$demoted_json" state)" = '"standby"' ] \
            || { echo "$label: the demoted source is not a standby" >&2; return 1; }
        [ "$(source_field "$demoted_json" active)" = "false" ] \
            || { echo "$label: the demoted source is still marked active" >&2; return 1; }
    fi

    demoted_before="$(source_field "$demoted_json" frames_out)"
    promoted_before="$(source_field "$(source_json "$promoted")" frames_out)"

    sleep 1

    demoted_after="$(source_field "$(source_json "$demoted")" frames_out)"
    promoted_after="$(source_field "$(source_json "$promoted")" frames_out)"

    [ "$demoted_after" = "$demoted_before" ] \
        || { echo "$label: the demoted source wrote $((demoted_after - demoted_before)) more frames to the program" >&2;
             return 1; }
    [ "$promoted_after" -gt "$promoted_before" ] \
        || { echo "$label: the selected source stopped feeding the program" >&2; return 1; }

    printf 'gen+%s switches+%s emergency+%s discontinuity=%s sync_boundary=keyframe only_selected=yes old_writer_gone=yes selected_feeding=yes\n' \
        "$((gen - PHASE_GEN0))" "$((switches - PHASE_SWITCHES0))" \
        "$((emergency - PHASE_EMERGENCY0))" "$((disc - PHASE_DISC0))"
}

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.05
done

grep -q 'srt listener ready' "$RUN/logs/error.log" \
    || { echo "listener not ready" >&2; exit 1; }

echo "== two publishers start"
start_publisher "testsrc=size=${WIDTH_A}x240:rate=25" 440 "encoder-a" pub_a 60
PUB_A="$PUB_PID"
sleep 1
start_publisher "smptehdbars=size=${WIDTH_B}x360:rate=25" 880 "encoder-b" pub_b 60
PUB_B="$PUB_PID"

wait_for_active encoder-a
wait_for_healthy encoder-b

echo "== kill: the active publisher dies"
open_phase

kill -KILL "$PUB_A"
PUB_A=0

OUTCOME_KILL="$(close_phase kill encoder-b encoder-a "$WIDTH_B")"
echo "   kill:              $OUTCOME_KILL"

SWITCHES_AFTER_KILL="$(switches)"
[ "$SWITCHES_AFTER_KILL" -ge 1 ] \
    || { echo "no switch counted after the kill" >&2; exit 1; }
FRAMES_AFTER_KILL="$(program_frames)"
[ "$FRAMES_AFTER_KILL" -gt 0 ] || { echo "no program frames" >&2; exit 1; }
echo "   active=encoder-b switches=$SWITCHES_AFTER_KILL frames=$FRAMES_AFTER_KILL"

echo "== no-op: switching to the source that is already on air"
NOOP_GEN="$(stream_field generation)"
NOOP_SWITCHES="$(stream_field switches)"
NOOP_DISC="$(discontinuities)"

NOOP="$(curl -fsS -X POST "$API/streams/live/news/switch?source=encoder-b")"
printf '%s\n' "$NOOP"

printf '%s' "$NOOP" | grep -q '"active":"encoder-b"' \
    || { echo "the no-op switch changed the active source" >&2; exit 1; }

[ "$(stream_field generation)" = "$NOOP_GEN" ] \
    || { echo "a switch to the source already on air opened a new generation" >&2; exit 1; }
[ "$(stream_field switches)" = "$NOOP_SWITCHES" ] \
    || { echo "a switch to the source already on air was counted as a switch" >&2; exit 1; }

sleep 1

[ "$(discontinuities)" = "$NOOP_DISC" ] \
    || { echo "a switch to the source already on air split the output" >&2; exit 1; }

echo "   the promotion path stood down without a generation change"

echo "== switchback: the primary returns"
open_phase

start_publisher "testsrc=size=${WIDTH_A}x240:rate=25" 440 "encoder-a" pub_a2 60
PUB_A="$PUB_PID"

OUTCOME_SWITCHBACK="$(close_phase switchback encoder-a encoder-b "$WIDTH_A")"
echo "   switchback:        $OUTCOME_SWITCHBACK"

SWITCHES_AFTER_BACK="$(switches)"
[ "$SWITCHES_AFTER_BACK" -gt "$SWITCHES_AFTER_KILL" ] \
    || { echo "switchback was not counted" >&2; exit 1; }
echo "   active=encoder-a switches=$SWITCHES_AFTER_BACK"

echo "== freeze: the active publisher stops making progress"
open_phase

kill -STOP "$PUB_A"

OUTCOME_FREEZE="$(close_phase freeze encoder-b encoder-a "$WIDTH_B")"
echo "   freeze:            $OUTCOME_FREEZE"

# The trigger differs -- a death, a recovery and a stall -- but the promotion
# is one code path, so it has to look the same every time.
[ "$OUTCOME_KILL" = "$OUTCOME_SWITCHBACK" ] \
    || { echo "the kill and the switchback are not the same promotion:" >&2
         echo "   kill:       $OUTCOME_KILL" >&2
         echo "   switchback: $OUTCOME_SWITCHBACK" >&2
         exit 1; }

[ "$OUTCOME_KILL" = "$OUTCOME_FREEZE" ] \
    || { echo "the kill and the freeze are not the same promotion:" >&2
         echo "   kill:   $OUTCOME_KILL" >&2
         echo "   freeze: $OUTCOME_FREEZE" >&2
         exit 1; }

echo "   kill, switchback and freeze took the same promotion: $OUTCOME_KILL"

echo "== thaw: the frozen publisher recovers"
kill -CONT "$PUB_A"
wait_for_active encoder-a
echo "   active=encoder-a after thaw"

echo "== corrupt: the primary is replaced by a garbage publisher"
kill -KILL "$PUB_A"
PUB_A=0
wait_for_active encoder-b

head -c 200000 /dev/urandom > "$RUN/garbage.bin"

srt-live-transmit \
    "file://$RUN/garbage.bin" \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=$STREAMID""encoder-a" \
    >"$RUN/pub_c.log" 2>&1 &
PUB_C=$!

sleep 3

wait_for_active encoder-b

CORRUPT_VIEW="$(api /streams/live/news/sources)"
printf '%s\n' "$CORRUPT_VIEW"

printf '%s' "$CORRUPT_VIEW" | grep -q '"id":"encoder-a".*"healthy":false' \
    || { echo "the corrupt source was not marked unhealthy" >&2; exit 1; }

printf '%s' "$CORRUPT_VIEW" | grep -q '"id":"encoder-b".*"healthy":true' \
    || { echo "encoder-b is not healthy" >&2; exit 1; }

FRAMES_LATE="$(program_frames)"
[ "$FRAMES_LATE" -gt "$FRAMES_AFTER_KILL" ] \
    || { echo "the program stopped progressing" >&2; exit 1; }

echo "   active=encoder-b frames=$FRAMES_LATE (was $FRAMES_AFTER_KILL)"

echo "== stop"
kill -KILL "$PUB_B" 2>/dev/null || true
PUB_B=0
kill -KILL "$PUB_C" 2>/dev/null || true
PUB_C=0

echo "== worker view"
grep -E 'selector|program stream' "$RUN/logs/error.log" | tail -20 || true

PID="$(cat "$RUN/logs/nginx.pid")"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit
trap - EXIT

for _ in $(seq 1 100); do
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

echo "== failover ok"

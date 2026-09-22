#!/usr/bin/env bash
#
# Control API, manual switch and automatic failover through nginx (phase 3 exit
# criteria).
#
# Two publishers feed the same logical stream (live/news) as encoder-a and
# encoder-b.  The API lists the stream, its sources and switches the program
# between them while both stay connected.  The two encoders carry video of
# different sizes so that the program output says which one is on air.
#
# The same promotion is then measured twice: once requested by hand through
# POST .../switch, once taken by the selector when the active source stops
# making progress.  Both must produce the same observable outcome, because
# both are the one promotion path.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/api-nginx"
SRT_PORT="${API_SRT_PORT:-19043}"
HTTP_PORT="${API_HTTP_PORT:-18443}"
LOG="$RUN/logs/error.log"
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

WIDTH_A=320
WIDTH_B=640

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf" "$RUN/html" "$RUN/hls"

# The playlist and the segments are written by the worker, and the worker is
# whoever the suite runs as - unless that is root, where nginx setuids it to
# the user it was compiled with, `nobody`.  The segmenter counts a segment it
# could not write rather than failing loudly, so a worker that cannot write
# this tree leaves the HLS output empty and the test sees no playlist at all.
# The image states its worker user and chowns its writable directories to
# match; the portable equivalent here is to leave the runtime tree writable by
# whichever user the worker turns out to be.
chmod -R a+rwX "$RUN"

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

PUB_A=0
PUB_B=0
PUB_PID=0

cleanup() {
    [ "$PUB_A" != "0" ] && kill "$PUB_A" 2>/dev/null || true
    # a publisher the failover phase froze must be resumed before it can die
    [ "$PUB_B" != "0" ] && kill -CONT "$PUB_B" 2>/dev/null || true
    [ "$PUB_B" != "0" ] && kill "$PUB_B" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== starting nginx"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 200); do
    grep -q 'srt listener ready' "$LOG" 2>/dev/null && break
    sleep 0.05
done

grep -q 'srt listener ready' "$LOG" || { echo "listener not ready" >&2; cat "$LOG" >&2; exit 1; }

api() {
    curl -fsS "$API$1"
}

# <field> of the stream detail, as plain text
stream_field() {
    api /streams/live/news | sed -n "s/.*\"$1\":\([0-9]*\).*/\1/p"
}

active_source() {
    api /streams/live/news | sed -n 's/.*"active":"\([^"]*\)".*/\1/p'
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

wait_for_active() {
    local want="$1"

    for _ in $(seq 1 300); do
        [ "$(active_source)" = "$want" ] && return 0
        sleep 0.1
    done

    echo "the program never moved to $want" >&2
    exit 1
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

    echo "$id still holds a writer after the switch" >&2
    exit 1
}

discontinuities() {
    local file="$RUN/hls/index.m3u8"

    # The playlist only appears once the first segment closes, and that can be
    # after this count is first taken.  `grep -c` on a file that is not there
    # prints nothing at all, which is not a number any caller can compare - and
    # a count of zero is what a playlist that does not exist yet means.
    if [ ! -f "$file" ]; then
        printf '0'
        return
    fi

    grep -c '^#EXT-X-DISCONTINUITY$' "$file" || true
}

# the first segment of the newest generation: the one that follows the last
# discontinuity, which the playlist only carries once that segment is closed
new_generation_segment() {
    awk '/^#EXT-X-DISCONTINUITY$/ { found = 1; name = ""; next }
         /\.ts$/ { if (found) name = $0 }
         END { print name }' "$RUN/hls/index.m3u8"
}

wait_for_new_generation_segment() {
    local before="$1" name

    for _ in $(seq 1 300); do
        if [ "$(discontinuities)" -gt "$before" ]; then
            name="$(new_generation_segment)"

            if [ -n "$name" ] && [ -s "$RUN/hls/$name" ]; then
                printf '%s' "$name"
                return 0
            fi
        fi

        sleep 0.1
    done

    echo "no segment started after the switch" >&2
    exit 1
}

# video widths present in a segment, sorted and space separated
segment_widths() {
    ffprobe -hide_banner -loglevel error -select_streams v:0 \
        -show_entries frame=width -of csv=p=0 "$RUN/hls/$1" 2>/dev/null \
        | tr -d ',' | sort -u | tr '\n' ' '
}

segment_first_is_keyframe() {
    [ "$(ffprobe -hide_banner -loglevel error -select_streams v:0 \
        -show_entries frame=key_frame -of csv=p=0 "$RUN/hls/$1" 2>/dev/null \
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
# line of observable outcomes.  The same measurements are taken for the manual
# switch and for the automatic failover, so that the two can be compared as a
# whole instead of each being trusted on its own.
close_phase() {
    local label="$1" promoted="$2" demoted="$3" width="$4"
    local gen switches emergency disc segment demoted_json demoted_after_json
    local demoted_before promoted_before demoted_after promoted_after

    wait_for_active "$promoted"
    wait_for_writers_drained "$demoted"

    # The demoted source is only observable for as long as its transport is
    # up, and the failover phase freezes its publisher: SRT drops a publisher
    # that has gone silent after its peer idle timeout - five seconds by
    # default, and nothing this test can configure.  So the window that proves
    # the demoted source stopped feeding the program is measured first and the
    # segment is waited for afterwards, which is what keeps the two phases
    # measuring the same thing on a slow machine.
    demoted_json="$(source_json "$demoted")"

    # a source the worker has already detached holds no writer either, and a
    # source that is gone cannot feed the program: detachment is the strongest
    # form of the property this phase measures
    if [ -n "$demoted_json" ]; then
        [ "$(source_field "$demoted_json" state)" = '"standby"' ] \
            || { echo "$label: the demoted source is not a standby" >&2; exit 1; }
        [ "$(source_field "$demoted_json" active)" = "false" ] \
            || { echo "$label: the demoted source is still marked active" >&2; exit 1; }
    fi

    [ "$(api /streams/live/news/sources | grep -o '"active":true' | wc -l)" = "1" ] \
        || { echo "$label: not exactly one source is active" >&2; exit 1; }

    demoted_before="$(source_field "$demoted_json" frames_out)"
    promoted_before="$(source_field "$(source_json "$promoted")" frames_out)"

    sleep 1

    demoted_after_json="$(source_json "$demoted")"
    demoted_after="$(source_field "$demoted_after_json" frames_out)"
    promoted_after="$(source_field "$(source_json "$promoted")" frames_out)"

    # Both ends of the window have to have been observable for the comparison
    # to mean anything; a source that vanished during it wrote nothing more,
    # because its writer went with it - the same reading the writer drain above
    # takes.
    if [ -n "$demoted_json" ] && [ -n "$demoted_after_json" ]; then
        [ "$demoted_after" = "$demoted_before" ] \
            || { echo "$label: the demoted source wrote $((demoted_after - demoted_before)) more frames to the program" >&2;
                 exit 1; }
    fi

    [ "$promoted_after" -gt "$promoted_before" ] \
        || { echo "$label: the selected source stopped feeding the program" >&2; exit 1; }

    # The new generation starts on a sync boundary and carries the selected
    # source only: a writer left over from the demoted source would splice its
    # own resolution into this segment.  The playlist only carries the segment
    # once it is closed, so this doubles as waiting for the switch to be
    # announced.
    #
    # The wait runs in a subshell, so its own exit only ends that subshell:
    # without this `||` the phase carries on with an empty segment name and
    # reports a second failure - the generation check below - for whatever
    # happened to the stream while it was waiting, which buries the one failure
    # that says what actually went wrong.
    segment="$(wait_for_new_generation_segment "$PHASE_DISC0")" || exit 1

    gen="$(stream_field generation)"
    switches="$(stream_field switches)"
    emergency="$(stream_field emergency_switches)"
    disc="$(discontinuities)"

    [ "$gen" = "$((PHASE_GEN0 + 1))" ] \
        || { echo "$label: generation went $PHASE_GEN0 -> $gen, not +1" >&2; exit 1; }
    [ "$switches" = "$((PHASE_SWITCHES0 + 1))" ] \
        || { echo "$label: switches went $PHASE_SWITCHES0 -> $switches, not +1" >&2; exit 1; }
    [ "$emergency" = "$PHASE_EMERGENCY0" ] \
        || { echo "$label: the promotion used the emergency path" >&2; exit 1; }
    [ "$disc" -gt "$PHASE_DISC0" ] \
        || { echo "$label: the switch was not announced to the playlist" >&2; exit 1; }

    segment_first_is_keyframe "$segment" \
        || { echo "$label: $segment does not begin on a keyframe" >&2; exit 1; }

    [ "$(segment_widths "$segment")" = "$width " ] \
        || { echo "$label: $segment carries $(segment_widths "$segment"), not ${width}px video only" >&2;
             exit 1; }

    printf 'gen+%s switches+%s emergency+%s discontinuity=%s sync_boundary=keyframe only_selected=yes old_writer_gone=yes selected_feeding=yes\n' \
        "$((gen - PHASE_GEN0))" "$((switches - PHASE_SWITCHES0))" \
        "$((emergency - PHASE_EMERGENCY0))" "$((disc - PHASE_DISC0))"
}

# publish <video-filter> <tone-hz> <source> <logname>
# starts ffmpeg in the background and reports its real pid through PUB_PID, so
# that the failover phase can freeze the publisher rather than the shell that
# spawned it
publish() {
    local pattern="$1" freq="$2" source="$3" out="$4"

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "$pattern" \
        -f lavfi -i "sine=frequency=$freq:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t 30 -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/news,m=publish,s=$source" \
        >"$RUN/$out.log" 2>&1 &

    PUB_PID=$!
}

echo "== starting two publishers on live/news"
publish "testsrc=size=${WIDTH_A}x240:rate=25" 440 "encoder-a" pub_a
PUB_A="$PUB_PID"
sleep 1
publish "smptehdbars=size=${WIDTH_B}x360:rate=25" 880 "encoder-b" pub_b
PUB_B="$PUB_PID"

for _ in $(seq 1 200); do
    if grep -q 'srt source open.*source=encoder-b' "$LOG" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

sleep 2

echo "== listing streams"
LIST="$(curl -fsS "$API/streams")"
printf '%s\n' "$LIST"

printf '%s' "$LIST" | grep -q '"application":"live","name":"news"' \
    || { echo "stream not listed" >&2; exit 1; }
printf '%s' "$LIST" | grep -q '"active":"encoder-a"' \
    || { echo "encoder-a is not the active source" >&2; exit 1; }
printf '%s' "$LIST" | grep -q '"id":"encoder-b"' \
    || { echo "encoder-b not registered" >&2; exit 1; }

echo "== sources endpoint"
SOURCES="$(curl -fsS "$API/streams/live/news/sources")"
printf '%s\n' "$SOURCES"
printf '%s' "$SOURCES" | grep -q '"id":"encoder-a".*"state":"active"' \
    || { echo "encoder-a state wrong" >&2; exit 1; }
printf '%s' "$SOURCES" | grep -q '"id":"encoder-b".*"state":"standby"' \
    || { echo "encoder-b is not a hot standby" >&2; exit 1; }

STANDBY_PREROLL="$(printf '%s' "$SOURCES" | sed -n 's/.*"id":"encoder-b".*"preroll_units":\([0-9]*\).*/\1/p')"
[ -n "$STANDBY_PREROLL" ] && [ "$STANDBY_PREROLL" -gt 0 ] \
    || { echo "the standby has no cached GOP" >&2; exit 1; }

echo "== switching to encoder-b"
open_phase

SWITCH="$(curl -fsS -X POST "$API/streams/live/news/switch?source=encoder-b")"
printf '%s\n' "$SWITCH"

printf '%s' "$SWITCH" | grep -q '"active":"encoder-b"' \
    || { echo "switch did not take effect" >&2; exit 1; }
printf '%s' "$SWITCH" | grep -q '"generation":2' \
    || { echo "generation was not bumped" >&2; exit 1; }
printf '%s' "$SWITCH" | grep -q '"switches":1' \
    || { echo "switch was not counted" >&2; exit 1; }

OUTCOME_MANUAL="$(close_phase manual encoder-b encoder-a "$WIDTH_B")"
echo "   manual switch:     $OUTCOME_MANUAL"

METRICS="$(curl -fsS "$API/metrics")"

printf '%s' "$METRICS" \
    | grep -q 'nginx_media_stream_switches{application="live",name="news"} 1' \
    || { echo "metrics did not report the switch" >&2; printf '%s\n' "$METRICS" >&2; exit 1; }
printf '%s' "$METRICS" \
    | grep -q 'nginx_media_stream_generation{application="live",name="news"} 2' \
    || { echo "metrics did not report the generation" >&2; exit 1; }
printf '%s' "$METRICS" \
    | grep -q 'nginx_media_source_active{application="live",name="news",source="encoder-b"} 1' \
    || { echo "metrics did not report the active source" >&2; exit 1; }
printf '%s' "$METRICS" \
    | grep -q 'nginx_media_source_active{application="live",name="news",source="encoder-a"} 0' \
    || { echo "metrics did not report the demoted source" >&2; exit 1; }

echo "   metrics ok (switches, generation, active source)"

DETAIL="$(curl -fsS "$API/streams/live/news")"
printf '%s\n' "$DETAIL"
printf '%s' "$DETAIL" | grep -q '"active":"encoder-b"' \
    || { echo "detail does not show the new active source" >&2; exit 1; }
printf '%s' "$DETAIL" | grep -q '"id":"encoder-a".*"state":"standby"' \
    || { echo "the demoted source is not a standby" >&2; exit 1; }

echo "== error paths"
STATUS="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$API/streams/live/news/switch?source=nobody")"
[ "$STATUS" = "404" ] || { echo "unknown source should be 404, got $STATUS" >&2; exit 1; }

STATUS="$(curl -s -o /dev/null -w '%{http_code}' "$API/streams/live/news/switch?source=encoder-a")"
[ "$STATUS" = "405" ] || { echo "GET on switch should be 405, got $STATUS" >&2; exit 1; }

STATUS="$(curl -s -o /dev/null -w '%{http_code}' "$API/streams/live/missing")"
[ "$STATUS" = "404" ] || { echo "unknown stream should be 404, got $STATUS" >&2; exit 1; }

echo "== failover: the active source stops making progress"
open_phase

# encoder-b freezes but stays connected, so the selector has to fail over to
# encoder-a on its own -- the same promotion, taken by the selector instead of
# by the API
kill -STOP "$PUB_B"

OUTCOME_AUTO="$(close_phase failover encoder-a encoder-b "$WIDTH_A")"
echo "   automatic failover: $OUTCOME_AUTO"

kill -CONT "$PUB_B"

# The two paths are one implementation: they must not just both work, they
# must be observably the same promotion.
[ "$OUTCOME_MANUAL" = "$OUTCOME_AUTO" ] \
    || { echo "manual switch and automatic failover differ:" >&2
         echo "   manual:    $OUTCOME_MANUAL" >&2
         echo "   automatic: $OUTCOME_AUTO" >&2
         exit 1; }

echo "   manual and automatic promotion are the same path"

kill -KILL "$PUB_A" 2>/dev/null || true
PUB_A=0
kill -KILL "$PUB_B" 2>/dev/null || true
PUB_B=0

wait 2>/dev/null || true

echo "== worker view"
grep -E 'srt program|srt source open|api switch' "$LOG" || true

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

grep -q 'exited with code 0' "$LOG" \
    || { echo "worker did not exit cleanly" >&2; exit 1; }

echo "== api switch ok"

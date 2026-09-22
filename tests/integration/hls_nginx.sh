#!/usr/bin/env bash
#
# HLS and recording through nginx (phase 5 exit criteria).
#
# Two publishers feed live/news over SRT as encoder-a and encoder-b, with
# deliberately distinguishable video (320x240 vs 640x360) so that the recorded
# PROGRAM can be told apart from the uncut source it was taken from.  The worker
# muxes the program into bursts and produces an HLS playlist with segments, a
# PROGRAM recording, a RAW transport recording and an ISO recording of one
# named source.
#
# The program is moved between the two connected sources by hand while both are
# healthy, so the recording must follow the selection and not the preferred
# standby; the demoted publisher is then killed and restarted, and the program
# is moved back by hand.  The playlist must announce each switch and the
# PROGRAM recording must contain exactly the media that went to air, in that
# order, with a timeline that never restarts.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/hls-nginx"
SRT_PORT="${HLS_SRT_PORT:-19046}"
HTTP_PORT="${HLS_HTTP_PORT:-18446}"
API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
STREAMID='#!::r=live/news,m=publish,s='
PUB_A=0
PUB_B=0
PUB_PID=0

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

rm -rf "$RUN"
mkdir -p "$RUN/logs" "$RUN/conf" "$RUN/hls" "$RUN/rec"

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

# Manual switchback on purpose: the program must be able to sit on the
# non-preferred source long enough for the recording to show that it follows
# the selection and not the highest-priority source.  The automatic switchback
# path is exercised by failover_nginx.sh.
media_failover_switchback manual;

media_hls $RUN/hls;
media_record $RUN/rec/program.ts;
media_record_raw $RUN/rec/raw.ts;
media_record_iso encoder-a $RUN/rec/iso-a.ts;

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

        location /rec/ {
            alias $RUN/rec/;
        }
    }
}
EOF

cleanup() {
    [ "$PUB_A" != "0" ] && kill -KILL "$PUB_A" 2>/dev/null || true
    [ "$PUB_B" != "0" ] && kill -KILL "$PUB_B" 2>/dev/null || true
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit 2>/dev/null || true
}
trap cleanup EXIT

# start_publisher <seconds> <source> <video-filter> <tone-hz> <logfile>
# reports the ffmpeg pid through PUB_PID
start_publisher() {
    local seconds="$1" source="$2" video="$3" tone="$4" log="$5"

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "$video" \
        -f lavfi -i "sine=frequency=$tone:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$seconds" -f mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=$STREAMID$source" \
        >"$log" 2>&1 &

    PUB_PID=$!
}

# the two encoders carry video that can be told apart frame by frame
VIDEO_A="testsrc2=size=320x240:rate=25"
VIDEO_B="smptehdbars=size=640x360:rate=25"
WIDTH_A=320
WIDTH_B=640

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

echo "== encoder-a on air, encoder-b joins as the hot standby"
start_publisher 22 encoder-a "$VIDEO_A" 440 "$RUN/pub-a.log"
PUB_A="$PUB_PID"

sleep 3

start_publisher 22 encoder-b "$VIDEO_B" 880 "$RUN/pub-b.log"
PUB_B="$PUB_PID"

for _ in $(seq 1 200); do
    curl -fsS "$API/streams/live/news" 2>/dev/null \
        | grep -q '"id":"encoder-b"' && break
    sleep 0.05
done

SOURCES="$(curl -fsS "$API/streams/live/news/sources")"
printf '%s\n' "$SOURCES"

printf '%s' "$SOURCES" | grep -q '"id":"encoder-a".*"state":"active"' \
    || { echo "encoder-a is not on air" >&2; exit 1; }
printf '%s' "$SOURCES" | grep -q '"id":"encoder-b".*"state":"standby"' \
    || { echo "encoder-b did not join as a standby" >&2; exit 1; }

echo "== switching the program to encoder-b by hand (both sources healthy)"
sleep 2

SWITCH="$(curl -fsS -X POST "$API/streams/live/news/switch?source=encoder-b")"
printf '%s\n' "$SWITCH"

printf '%s' "$SWITCH" | grep -q '"active":"encoder-b"' \
    || { echo "the manual switch did not take effect" >&2; exit 1; }
printf '%s' "$SWITCH" | grep -q '"generation":2' \
    || { echo "the manual switch did not open a generation" >&2; exit 1; }
printf '%s' "$SWITCH" | grep -q '"switches":1' \
    || { echo "the manual switch was not counted" >&2; exit 1; }

# the switch must be announced in the playlist
for _ in $(seq 1 200); do
    grep -q '^#EXT-X-DISCONTINUITY' "$RUN/hls/index.m3u8" 2>/dev/null && break
    sleep 0.1
done

# and at least two segments must be closed by now
for _ in $(seq 1 100); do
    [ "$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" 2>/dev/null || true)" -ge 2 ] && break
    sleep 0.1
done

echo "== playlist"
cat "$RUN/hls/index.m3u8"

# check_playlist <expected-switches> <label>
#
# The playlist is the contract with the player, so it is checked as one after
# every source change rather than by grepping for a tag: it must stay well
# formed, list contiguous sequences from the media sequence it announces, and
# every segment it references must exist, be served and decode.
#
# The number of switches the playlist has announced is the discontinuity
# sequence plus the discontinuity tags still inside the window: a tag whose
# segment has been evicted is accounted for by the sequence, so the count does
# not drift as the window slides.  A validator that only asked "does the tag
# exist" would pass a playlist with no media sequence, a segment that was
# never written, or a switch that was never announced.
PLAYLIST_SEQ=""
PLAYLIST_FIRST_URI=""

check_playlist() {
    local expected="$1" label="$2"
    local playlist="$RUN/hls/index.m3u8"
    local target disc dseq seq first_uri first_num uri uris cur prev n tries

    # a switch only shows up once the segment that follows it has closed, so
    # wait for the announcement instead of racing the segmenter
    tries=0
    while [ "$tries" -lt 300 ]; do
        if [ -f "$playlist" ]; then
            dseq="$(sed -n 's/^#EXT-X-DISCONTINUITY-SEQUENCE:\([0-9]*\).*/\1/p' \
                "$playlist" | head -1)"
            disc="$(grep -c '^#EXT-X-DISCONTINUITY$' "$playlist" || true)"

            [ "$(( ${dseq:-0} + disc ))" = "$expected" ] && break
        fi

        sleep 0.1
        tries=$((tries + 1))
    done

    [ -f "$playlist" ] || { echo "$label: no playlist" >&2; return 1; }

    dseq="$(sed -n 's/^#EXT-X-DISCONTINUITY-SEQUENCE:\([0-9]*\).*/\1/p' \
        "$playlist" | head -1)"
    disc="$(grep -c '^#EXT-X-DISCONTINUITY$' "$playlist" || true)"

    grep -q '^#EXTM3U' "$playlist" \
        || { echo "$label: no playlist header" >&2; return 1; }
    grep -q '^#EXT-X-TARGETDURATION:' "$playlist" \
        || { echo "$label: no target duration" >&2; return 1; }
    grep -q '^#EXT-X-MEDIA-SEQUENCE:' "$playlist" \
        || { echo "$label: no media sequence" >&2; return 1; }

    # one discontinuity per switch, including the ones the window has passed
    [ "$(( ${dseq:-0} + disc ))" = "$expected" ] \
        || { echo "$label: announced $(( ${dseq:-0} + disc )) discontinuities for $expected switch(es)" >&2
             return 1; }

    target="$(sed -n 's/^#EXT-X-TARGETDURATION:\([0-9]*\).*/\1/p' "$playlist" | head -1)"
    seq="$(sed -n 's/^#EXT-X-MEDIA-SEQUENCE:\([0-9]*\).*/\1/p' "$playlist" | head -1)"

    uris="$(grep -E '\.ts$' "$playlist")"
    n="$(printf '%s\n' "$uris" | grep -c . || true)"
    [ "$n" -ge 1 ] || { echo "$label: the playlist references no segment" >&2; return 1; }

    first_uri="$(printf '%s\n' "$uris" | head -1)"
    first_num="${first_uri#*seg-}"
    first_num="${first_num%.ts}"

    # the announced media sequence is the sequence of the first listed segment
    [ -n "$first_num" ] && [ "$((10#$seq))" -eq "$((10#$first_num))" ] \
        || { echo "$label: media sequence $seq does not match $first_uri" >&2; return 1; }

    # the window never rewinds, and never loses a sequence it announced
    if [ -n "$PLAYLIST_FIRST_URI" ]; then
        prev="${PLAYLIST_FIRST_URI#*seg-}"
        prev="${prev%.ts}"

        [ "$((10#$first_num))" -ge "$((10#$prev))" ] \
            || { echo "$label: the window moved back to $first_uri" >&2; return 1; }
    fi

    PLAYLIST_SEQ="$seq"
    PLAYLIST_FIRST_URI="$first_uri"

    # listed sequences are contiguous: a gap means a segment went missing
    prev=""

    while read -r uri; do
        [ -n "$uri" ] || continue

        cur="${uri#*seg-}"
        cur="${cur%.ts}"

        if [ -n "$prev" ]; then
            [ "$((10#$cur))" -eq "$(( $((10#$prev)) + 1 ))" ] \
                || { echo "$label: the segment list has a gap: $prev -> $cur" >&2
                     return 1; }
        fi

        prev="$cur"
    done <<< "$uris"

    # every referenced segment exists, is served, and decodes
    while read -r uri; do
        [ -n "$uri" ] || continue

        [ -s "$RUN/hls/$uri" ] \
            || { echo "$label: $uri is referenced but missing on disk" >&2; return 1; }

        curl -fsS "http://127.0.0.1:$HTTP_PORT/hls/$uri" -o "$RUN/check.ts" \
            || { echo "$label: $uri is not fetchable" >&2; return 1; }

        cmp -s "$RUN/hls/$uri" "$RUN/check.ts" \
            || { echo "$label: $uri is served differently than it is on disk" >&2
                 return 1; }

        ffprobe -hide_banner -loglevel error -show_entries \
            stream=codec_name,codec_type -of csv "$RUN/check.ts" 2>/dev/null \
            | grep -q ',h264,video' \
            || { echo "$label: $uri does not decode as H.264 video" >&2; return 1; }
    done <<< "$uris"

    # no EXTINF may exceed the target duration the playlist announces
    awk -v target="$target" '
        /^#EXTINF:/ {
            d = $0; sub(/^#EXTINF:/, "", d); sub(/,.*/, "", d);
            if (d + 0 > target + 0) {
                printf "duration %s exceeds target %s\n", d, target > "/dev/stderr";
                bad = 1;
            }
        }
        END { exit bad }' "$playlist" \
        || { echo "$label: a segment exceeds EXT-X-TARGETDURATION" >&2; return 1; }

    # every tag sits between two segments, never before the first or after the
    # last, because a discontinuity with no segment after it is meaningless
    awk -v disc="$disc" '
        /^#EXT-X-DISCONTINUITY$/ { pending = 1; next }
        /^#EXTINF:/ { if (pending) { pending = 0; placed++ } }
        END {
            if (pending) {
                print "a discontinuity is not followed by a segment" > "/dev/stderr";
                exit 1;
            }
            if (placed != disc) {
                printf "only %d of %d discontinuities precede a segment\n",
                       placed, disc > "/dev/stderr";
                exit 1;
            }
            exit 0;
        }' "$playlist" \
        || { echo "$label: a discontinuity is not placed at a segment boundary" >&2
             return 1; }
}

grep -q '^#EXTM3U' "$RUN/hls/index.m3u8" || { echo "no playlist header" >&2; exit 1; }

SEGMENTS="$(grep -c '^#EXTINF' "$RUN/hls/index.m3u8" || true)"
[ "$SEGMENTS" -ge 2 ] || { echo "too few segments: $SEGMENTS" >&2; exit 1; }

check_playlist 1 "after the first switch"

SEGMENT_NAME="$(grep '\.ts$' "$RUN/hls/index.m3u8" | tail -1)"

echo "== serving $SEGMENT_NAME over HTTP"
curl -fsS "http://127.0.0.1:$HTTP_PORT/hls/$SEGMENT_NAME" -o "$RUN/fetched.ts"

SHA_DISK="$(sha256sum "$RUN/hls/$SEGMENT_NAME" | cut -d' ' -f1)"
SHA_HTTP="$(sha256sum "$RUN/fetched.ts" | cut -d' ' -f1)"
[ "$SHA_DISK" = "$SHA_HTTP" ] \
    || { echo "served segment differs from the file on disk" >&2; exit 1; }

echo "== probing the served segment"
PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
    stream=codec_name,codec_type -show_entries format=duration -of csv \
    "$RUN/fetched.ts" 2>&1)"

printf '%s\n' "$PROBE"

printf '%s' "$PROBE" | grep -q ',h264,video' || { echo "segment has no H.264" >&2; exit 1; }
printf '%s' "$PROBE" | grep -q ',aac,audio' || { echo "segment has no AAC" >&2; exit 1; }

echo "== killing the demoted publisher"
kill -KILL "$PUB_A"
PUB_A=0
sleep 1

AFTER_KILL="$(curl -fsS "$API/streams/live/news")"
printf '%s\n' "$AFTER_KILL"

printf '%s' "$AFTER_KILL" | grep -q '"active":"encoder-b"' \
    || { echo "the program left the selected source when the standby died" >&2; exit 1; }
printf '%s' "$AFTER_KILL" | grep -q '"switches":1' \
    || { echo "a dead standby was counted as a switch" >&2; exit 1; }

echo "== encoder-a returns, but the selection holds the program on encoder-b"
start_publisher 8 encoder-a "$VIDEO_A" 440 "$RUN/pub-a2.log"
PUB_A="$PUB_PID"

for _ in $(seq 1 200); do
    curl -fsS "$API/streams/live/news" 2>/dev/null \
        | grep -q '"id":"encoder-a"' && break
    sleep 0.05
done

sleep 2

AFTER_RETURN="$(curl -fsS "$API/streams/live/news")"

printf '%s' "$AFTER_RETURN" | grep -q '"active":"encoder-b"' \
    || { echo "a returning higher-priority source displaced the selection" >&2; exit 1; }
printf '%s' "$AFTER_RETURN" | grep -q '"switches":1' \
    || { echo "the returning source was promoted while switchback is manual" >&2; exit 1; }

echo "== switching the program back to encoder-a by hand"
SWITCH_BACK="$(curl -fsS -X POST "$API/streams/live/news/switch?source=encoder-a")"
printf '%s\n' "$SWITCH_BACK"

printf '%s' "$SWITCH_BACK" | grep -q '"active":"encoder-a"' \
    || { echo "the switch back did not take effect" >&2; exit 1; }
printf '%s' "$SWITCH_BACK" | grep -q '"generation":3' \
    || { echo "the switch back did not open a generation" >&2; exit 1; }
printf '%s' "$SWITCH_BACK" | grep -q '"switches":2' \
    || { echo "the switch back was not counted" >&2; exit 1; }

sleep 3

# the playlist must have announced the switch back as well, and it must still
# reference segments that exist, are served and decode
echo "== playlist after the switch back"
cat "$RUN/hls/index.m3u8"
check_playlist 2 "after the switch back"

echo "== recordings"
for f in "$RUN/rec/program.ts" "$RUN/rec/iso-a.ts"; do
    [ -s "$f" ] || { echo "$f is missing" >&2; exit 1; }

    REC_PROBE="$(ffprobe -hide_banner -loglevel error -show_entries \
        stream=codec_name,codec_type -of csv "$f" 2>&1)"

    printf '%s\n' "$REC_PROBE"

    printf '%s' "$REC_PROBE" | grep -q ',h264,video' \
        || { echo "$f has no H.264" >&2; exit 1; }

    printf '%s' "$REC_PROBE" | grep -q ',aac,audio' \
        || { echo "$f has no AAC" >&2; exit 1; }
done

[ -s "$RUN/rec/raw.ts" ] || { echo "raw recording is missing" >&2; exit 1; }

RAW_SIZE="$(stat -c %s "$RUN/rec/raw.ts")"
[ "$RAW_SIZE" -gt 100000 ] || { echo "raw recording too small: $RAW_SIZE" >&2; exit 1; }

PROGRAM_SIZE="$(stat -c %s "$RUN/rec/program.ts")"
echo "   program=$PROGRAM_SIZE bytes raw=$RAW_SIZE bytes segments=$SEGMENTS"

echo "== the PROGRAM recording must hold exactly what went to air"
# Frame level, because the width of each recorded access unit is what says
# which source it came from.  The publisher drops frames when the machine is
# busy, so the decoder logs errors about the gaps; only the CSV matters here.
ffprobe -hide_banner -loglevel error -select_streams v:0 \
    -show_entries frame=pkt_dts_time,key_frame,width -of csv=p=0 \
    "$RUN/rec/program.ts" > "$RUN/program.frames" 2>/dev/null

FRAMES="$(wc -l < "$RUN/program.frames")"
[ "$FRAMES" -gt 0 ] || { echo "the program recording has no video frames" >&2; exit 1; }

# The recorded video must show each selected source, in the order it was on
# air: encoder-a, then encoder-b after the manual switch, then encoder-a again
# after the switch back.  A tap that followed the standby instead of the
# selection, a demoted writer that kept feeding the program, or a recorder that
# restarted at the switch changes this sequence.
WIDTH_RUNS="$(awk -F, '
    { if ($3 != w) { if (w != "") runs = runs " " w; w = $3 } }
    END { if (w != "") runs = runs " " w; print runs }' "$RUN/program.frames")"

WIDTH_RUNS_EXPECTED=" $WIDTH_A $WIDTH_B $WIDTH_A"

echo "   recorded widths:$WIDTH_RUNS (expected:$WIDTH_RUNS_EXPECTED)"

[ "$WIDTH_RUNS" = "$WIDTH_RUNS_EXPECTED" ] \
    || { echo "the program recording does not follow the selected source: $WIDTH_RUNS" >&2;
         exit 1; }

for w in "$WIDTH_A" "$WIDTH_B"; do
    WIDTH_FRAMES="$(awk -F, -v w="$w" '$3 == w' "$RUN/program.frames" | wc -l)"
    [ "$WIDTH_FRAMES" -ge 10 ] \
        || { echo "only $WIDTH_FRAMES recorded frames of ${w}px video" >&2; exit 1; }
done

# Every switch replays from a decodable point, so the first frame of each new
# run of content is a keyframe.
TRANSITIONS="$(awk -F, '
    { if ($3 != w) { if (w != "") { n++; if ($1 != 1) bad++ } w = $3 } }
    END { printf "%d %d", n, bad + 0 }' "$RUN/program.frames")"

[ "$TRANSITIONS" = "2 0" ] \
    || { echo "content transitions=$TRANSITIONS; expected 2, none mid-GOP" >&2; exit 1; }

# The timeline runs through the switches instead of restarting: no frame is
# ever stamped before its predecessor, and the file spans the whole run rather
# than only the content that followed the last switch.
REGRESSIONS="$(awk -F, 'NR > 1 && $2 < prev { bad++ } { prev = $2 } END { print bad + 0 }' \
    "$RUN/program.frames")"

[ "$REGRESSIONS" = "0" ] \
    || { echo "the program timeline went backwards $REGRESSIONS time(s)" >&2; exit 1; }

SPAN="$(awk -F, 'NR == 1 { first = $2 } { last = $2 } END { printf "%.3f", last - first }' \
    "$RUN/program.frames")"

echo "   program timeline spans ${SPAN}s over $FRAMES recorded frames"

awk -v span="$SPAN" 'BEGIN { exit !(span >= 10) }' \
    || { echo "the recording only spans ${SPAN}s; the switch restarted the timeline" >&2; exit 1; }

echo "== stop"
kill -KILL "$PUB_A" 2>/dev/null || true
PUB_A=0
kill -KILL "$PUB_B" 2>/dev/null || true
PUB_B=0

sleep 1

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

if grep -q 'open socket' "$RUN/logs/error.log"; then
    echo "worker left a socket registered at shutdown" >&2
    exit 1
fi

echo "== hls and recording ok"

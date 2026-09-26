#!/usr/bin/env bash
#
# ffmpeg interoperability, both directions, across the protocol spread.
#
# ffmpeg is the encoder and the player most deployments meet first, so every
# way media enters nginx-media is driven by ffmpeg, and every way it leaves is
# read back by ffmpeg.  Each case checks what ffmpeg itself decodes: the codecs
# it finds, that it decodes frames, and that decode timestamps never go
# backwards.
#
#   in  (ffmpeg -> nginx-media, read back through the HLS origin)
#     srt caller        H.264+AAC, HEVC+AAC, H.264 video only
#     rtmp publish      H.264+AAC, HEVC+AAC (enhanced RTMP), H.264 video only
#     hls push ingest   ffmpeg's HLS muxer with -method PUT, and with POST
#     hls pull          an HLS presentation ffmpeg wrote, served over HTTP
#     file              an MPEG-TS file ffmpeg wrote
#
#   out (nginx-media -> ffmpeg)
#     hls origin        ffmpeg reads the playlist over HTTP   (H.264, HEVC)
#     rtmp play         ffmpeg plays the program              (H.264, HEVC)
#     srt destination   ffmpeg is the SRT listener             (H.264, HEVC)
#     rtmp destination  ffmpeg is the RTMP server (-listen 1)  (H.264)
#     hls push          nginx-media pushes into an ingest endpoint and ffmpeg
#                       reads the playlist that arrived        (H.264)
#
# Audio-only programs are not in the matrix: HLS segments start at video
# keyframes, so a program without video is carried by SRT and RTMP but not
# segmented (docs/hls-push-interop.md, "Not accepted yet").

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/ffmpeg-interop"
BASE=$(( 23000 + ($$ % 40) * 16 ))
HTTP_PORT=$BASE
SRT_PORT=$(( BASE + 1 ))
RTMP_PORT=$(( BASE + 2 ))
OUT_PORT=$(( BASE + 3 ))

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls" "$RUN/ingest" "$RUN/pull" \
         "$RUN/media" "$RUN/body" "$RUN/out"

PIDS=()
RESULTS=()
FAILED=0

stop_instance() {
    local pid child

    [ -f "$RUN/logs/nginx.pid" ] || return 0
    pid="$(cat "$RUN/logs/nginx.pid")"
    kill -QUIT "$pid" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done
    for child in $(pgrep -P "$pid" 2>/dev/null); do
        kill -KILL "$child" 2>/dev/null
    done
    kill -KILL "$pid" 2>/dev/null
    return 0
}

reap() {
    local pid
    for pid in ${PIDS[@]+"${PIDS[@]}"}; do
        kill -KILL "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    PIDS=()
}

cleanup() {
    reap
    stop_instance
    return 0
}
trap cleanup EXIT

die() {
    echo "FAIL: $*" >&2
    tail -20 "$RUN/logs/error.log" >&2
    exit 1
}

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 512; }

media_hls $RUN/hls;
media_srt_listen 127.0.0.1:$SRT_PORT;
media_rtmp_listen 127.0.0.1:$RTMP_PORT;

http {
    access_log off;
    client_body_temp_path $RUN/body;
    client_max_body_size 16m;

    server {
        listen 127.0.0.1:$HTTP_PORT;
        location /media/api/ { media_api; }
        location /hls/ { alias $RUN/hls/; }
        location /pull/ { alias $RUN/pull/; }
        location /ingest/ { media_hls_ingest $RUN/ingest; }
        location /stored/ { alias $RUN/ingest/; }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 || die "config rejected"
"$NGINX" -p "$RUN" -c conf/nginx.conf || die "nginx did not start"
sleep 0.5

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"
HTTP="http://127.0.0.1:$HTTP_PORT"

# --- encoders ----------------------------------------------------------------

# encode <codecs> <seconds> <format> <url> [extra output options...]
#   codecs: h264+aac | hevc+aac | h264
encode() {
    local codecs="$1" seconds="$2" format="$3" url="$4"
    shift 4
    local -a in=(-f lavfi -i "testsrc2=size=320x240:rate=25")
    local -a enc=()

    case "$codecs" in
        h264*) enc+=(-c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p) ;;
        hevc*) enc+=(-c:v libx265 -preset ultrafast -x265-params log-level=none
                     -g 25 -pix_fmt yuv420p) ;;
    esac
    case "$codecs" in
        *+aac)
            in+=(-f lavfi -i "sine=frequency=440:sample_rate=48000")
            enc+=(-ac 2 -c:a aac -b:a 96k)
            ;;
    esac

    timeout $(( seconds + 30 )) ffmpeg -hide_banner -loglevel error -re \
        "${in[@]}" "${enc[@]}" -t "$seconds" "$@" -f "$format" "$url" \
        >>"$RUN/encoders.log" 2>&1 &
    PIDS+=($!)
}

# --- what ffmpeg decodes -------------------------------------------------------

# check <label> <input> <codecs> [ffprobe input options...]
#   the input's streams must be exactly the codecs expected, video must decode
#   at least 25 frames, and packet dts must never decrease within a stream
check() {
    local label="$1" input="$2" codecs="$3"
    shift 3
    local want_v want_a report

    want_v="${codecs%%+*}"
    want_a=""
    [[ "$codecs" == *+aac ]] && want_a=aac

    report="$(timeout 60 python3 - "$input" "$want_v" "$want_a" "$@" <<'PYEOF'
import json, subprocess, sys

src, want_v, want_a, extra = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
cmd = ["ffprobe", "-v", "error", *extra, "-read_intervals", "%+8", "-count_frames",
       "-show_entries", "stream=index,codec_type,codec_name,nb_read_frames",
       "-show_entries", "packet=stream_index,dts",
       "-of", "json", src]
try:
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=50)
except subprocess.TimeoutExpired:
    print("ffprobe timed out"); sys.exit(1)
if out.returncode != 0:
    print("ffprobe failed: " + out.stderr.strip()[:200]); sys.exit(1)
data = json.loads(out.stdout or "{}")
streams = {s["codec_type"]: s for s in data.get("streams", [])}
problems = []
video = streams.get("video")
if (video or {}).get("codec_name") != want_v:
    problems.append(f"video {video and video.get('codec_name')} != {want_v}")
elif int(video.get("nb_read_frames") or 0) < 25:
    problems.append(f"video decoded {video.get('nb_read_frames')} frames")
audio = streams.get("audio")
if want_a and (audio or {}).get("codec_name") != want_a:
    problems.append(f"audio {audio and audio.get('codec_name')} != {want_a}")
if not want_a and audio:
    problems.append(f"unexpected audio {audio.get('codec_name')}")
last, backwards = {}, 0
for p in data.get("packets", []):
    if p.get("dts") in (None, "N/A"):
        continue
    i, d = p["stream_index"], int(p["dts"])
    if i in last and d < last[i]:
        backwards += 1
    last[i] = d
if backwards:
    problems.append(f"{backwards} dts went backwards")
if problems:
    print("; ".join(problems)); sys.exit(1)
v = int(video["nb_read_frames"])
a = int(audio["nb_read_frames"]) if audio else 0
print(f"{want_v} {v} frames" + (f", aac {a} frames" if audio else ""))
PYEOF
)"
    if [ "$?" -eq 0 ]; then
        RESULTS+=("ok    $label: $report")
    else
        RESULTS+=("FAIL  $label: $report")
        FAILED=$(( FAILED + 1 ))
    fi
    echo "${RESULTS[-1]}"
}

stream() {   # <name>
    curl -fsS -X POST -H 'Content-Type: application/json' \
        -d "{\"application\":\"live\",\"name\":\"$1\"}" "$API/streams" >/dev/null
}

source_add() {   # <stream> <json>
    curl -fsS -X POST -H 'Content-Type: application/json' -d "$2" \
        "$API/streams/live/$1/sources" >/dev/null
}

destination_add() {   # <stream> <json>
    curl -fsS -X POST -H 'Content-Type: application/json' -d "$2" \
        "$API/streams/live/$1/destinations" >/dev/null
}

# the HLS origin has at least n segments of the stream
wait_segments() {   # <stream> <n>
    local playlist="$RUN/hls/live/$1/index.m3u8"
    for _ in $(seq 1 300); do
        [ "$(grep -c '\.ts$' "$playlist" 2>/dev/null || echo 0)" -ge "$2" ] \
            && return 0
        sleep 0.1
    done
    return 1
}

origin() {   # <label> <stream> <codecs>
    if wait_segments "$2" 3; then
        check "$1" "$HTTP/hls/live/$2/index.m3u8" "$3"
    else
        RESULTS+=("FAIL  $1: the HLS origin never had 3 segments")
        FAILED=$(( FAILED + 1 ))
        echo "${RESULTS[-1]}"
    fi
}

# --- in ---------------------------------------------------------------------

echo "== ffmpeg -> nginx-media"

for codecs in h264+aac hevc+aac h264; do
    name="srt-${codecs//+/-}"
    encode "$codecs" 20 mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/$name,m=publish,s=enc"
    origin "in  srt $codecs -> hls origin" "$name" "$codecs"
    reap
done

for codecs in h264+aac hevc+aac h264; do
    name="rtmp-${codecs//+/-}"
    encode "$codecs" 20 flv "rtmp://127.0.0.1:$RTMP_PORT/live/$name"
    origin "in  rtmp $codecs -> hls origin" "$name" "$codecs"
    reap
done

for method in PUT POST; do
    name="push-$method"
    stream "$name" || die "stream $name"
    source_add "$name" "{\"id\":\"enc\",\"type\":\"hls_push\",\"path\":\"$RUN/ingest/live/$name\"}" \
        || die "hls_push source for $name"
    encode h264+aac 24 hls "$HTTP/ingest/live/$name/index.m3u8" \
        -hls_time 2 -hls_list_size 5 -hls_flags delete_segments -method "$method"
    origin "in  hls push ($method) h264+aac -> hls origin" "$name" h264+aac
    reap
done

name="pull"
mkdir -p "$RUN/pull/$name"
encode h264+aac 30 hls "$RUN/pull/$name/index.m3u8" \
    -hls_time 2 -hls_list_size 6 -hls_flags delete_segments
for _ in $(seq 1 100); do
    [ -s "$RUN/pull/$name/index.m3u8" ] && break
    sleep 0.1
done
stream "$name" || die "stream $name"
source_add "$name" "{\"id\":\"origin\",\"type\":\"hls_pull\",\"path\":\"$HTTP/pull/$name/index.m3u8\"}" \
    || die "hls_pull source"
origin "in  hls pull h264+aac -> hls origin" "$name" h264+aac
reap

name="file"
ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 20 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -ac 2 -c:a aac \
    -f mpegts "$RUN/media/file.ts" || die "could not write the file"
stream "$name" || die "stream $name"
source_add "$name" "{\"id\":\"file1\",\"type\":\"file\",\"path\":\"$RUN/media/file.ts\"}" \
    || die "file source"
origin "in  file h264+aac -> hls origin" "$name" h264+aac

# --- out --------------------------------------------------------------------

echo "== nginx-media -> ffmpeg"

for codecs in h264+aac hevc+aac; do
    name="out-${codecs//+/-}"
    encode "$codecs" 40 mpegts \
        "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/$name,m=publish,s=enc"
    wait_segments "$name" 2 || die "$name never reached the HLS origin"

    # rtmp play: ffmpeg is the player
    timeout 30 ffmpeg -hide_banner -loglevel error -y \
        -i "rtmp://127.0.0.1:$RTMP_PORT/live/$name" -t 6 -c copy \
        "$RUN/out/$name-play.flv" >>"$RUN/players.log" 2>&1
    check "out rtmp play $codecs" "$RUN/out/$name-play.flv" "$codecs"

    # srt destination: ffmpeg is the listener nginx-media connects to
    timeout 30 ffmpeg -hide_banner -loglevel error -y \
        -i "srt://127.0.0.1:$OUT_PORT?mode=listener" -t 6 -c copy \
        -f mpegts "$RUN/out/$name-srt.ts" >>"$RUN/players.log" 2>&1 &
    listener=$!
    sleep 1
    destination_add "$name" "{\"id\":\"srt-out\",\"type\":\"srt\",\"host\":\"127.0.0.1\",\"port\":$OUT_PORT}" \
        || die "srt destination for $name"
    wait "$listener"
    check "out srt destination $codecs" "$RUN/out/$name-srt.ts" "$codecs"
    curl -fsS -X DELETE "$API/streams/live/$name/destinations/srt-out" >/dev/null

    check "out hls origin $codecs" "$HTTP/hls/live/$name/index.m3u8" "$codecs"
    reap
done

name="out-rtmp-dest"
encode h264+aac 40 mpegts \
    "srt://127.0.0.1:$SRT_PORT?mode=caller&streamid=#!::r=live/$name,m=publish,s=enc"
wait_segments "$name" 2 || die "$name never reached the HLS origin"
timeout 30 ffmpeg -hide_banner -loglevel error -y -listen 1 \
    -i "rtmp://127.0.0.1:$OUT_PORT/live/$name" -t 6 -c copy \
    "$RUN/out/$name.flv" >>"$RUN/players.log" 2>&1 &
listener=$!
sleep 1
destination_add "$name" "{\"id\":\"rtmp-out\",\"type\":\"rtmp\",\"host\":\"127.0.0.1\",\"port\":$OUT_PORT}" \
    || die "rtmp destination"
wait "$listener"
check "out rtmp destination h264+aac" "$RUN/out/$name.flv" h264+aac

# hls push: into this server's own ingest endpoint, read back by ffmpeg
destination_add "$name" "{\"id\":\"push\",\"type\":\"hls_push\",\"host\":\"$HTTP/ingest/pushed/$name/\",\"path\":\"$RUN/hls/live/$name\"}" \
    || die "hls push destination"
for _ in $(seq 1 200); do
    [ "$(grep -c '\.ts$' "$RUN/ingest/pushed/$name/index.m3u8" 2>/dev/null || echo 0)" -ge 3 ] \
        && break
    sleep 0.1
done
check "out hls push h264+aac (read back from the endpoint)" \
    "$HTTP/stored/pushed/$name/index.m3u8" h264+aac
reap

echo
echo "== ffmpeg interop matrix"
printf '   %s\n' "${RESULTS[@]}"

if grep -qE '\[(alert|emerg)\]|signal [0-9]+|AddressSanitizer' "$RUN/logs/error.log"; then
    die "worker reported an alert or crash"
fi

[ "$FAILED" -eq 0 ] || die "$FAILED of ${#RESULTS[@]} interop cases failed"
echo "== ffmpeg interop ok (${#RESULTS[@]} cases)"

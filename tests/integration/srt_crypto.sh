#!/usr/bin/env bash
#
# SRT encryption (goal doc 11, phase 11).
#
# Three cases, all against the real library:
#
#   1. publisher and listener agree on the passphrase  -> media flows
#   2. they disagree                                   -> the publisher is
#      rejected, nothing is registered, nothing is served
#   3. AES-GCM is requested                            -> the listener refuses
#      with the reason, instead of silently falling back to AES-CTR
#
# Case 3 is library dependent: Haivision/srt 1.5.6 as packaged here is built
# without the AEAD API preview, so it has no AES-GCM.  The test asserts the
# honest outcome either way: it must either work or say why it cannot.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="${NGINX_BIN:-$ROOT/.build/nginx-install/sbin/nginx}"
RUN="$ROOT/.build/srt-crypto"
PASS="correct-horse-battery"
WRONG="wrong-horse-battery"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

# The master is stopped and waited for, and its children are then killed by
# parent: nginx rewrites a worker's argv to "nginx: worker process", so a
# pattern that matches the configuration path matches the master only, and a
# worker whose master was killed outright is reparented to init and keeps the
# ports the next case needs.
stop_instance() {
    local prefix="$1" pid child

    [ -f "$prefix/logs/nginx.pid" ] || return 0

    pid="$(cat "$prefix/logs/nginx.pid")"

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

cleanup() {
    stop_instance "$RUN"
    stop_instance "$RUN/a"
    stop_instance "$RUN/b"
    return 0
}

trap cleanup EXIT

write_conf() {
    cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/hls;

$1

media_srt_listen 127.0.0.1:$PORT;
media_srt_source_priority encoder-a 100;
EOF
}

# starts an instance and waits until its listener is actually up: a fixed
# sleep turns a slow start into a confusing "publisher rejected"
start_nginx() {
    local dir="$1" pattern="$2" log

    log="$dir/logs/error.log"
    rm -f "$log"
    "$NGINX" -p "$dir" -c conf/nginx.conf

    for _ in $(seq 1 100); do
        grep -q "$pattern" "$log" 2>/dev/null && return 0
        grep -q 'listener failed' "$log" 2>/dev/null && {
            echo "FAIL: listener did not start"
            tail -2 "$log"
            return 1
        }
        sleep 0.1
    done

    echo "FAIL: listener never became ready"
    return 1
}

# Killing only the master leaves its workers holding the port, which shows up
# as an inexplicable "listener failed" in the next case.  Every instance here
# belongs to this test, so tear the whole set down the way the other
# integration tests do.
stop_nginx() {
    stop_instance "$1"
    sleep 0.4
    return 0
}

publish() {
    # $1: passphrase, $2: log file, $3: seconds
    timeout "$(( $3 + 15 ))" ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
        -c:a aac -b:a 96k \
        -t "$3" -f mpegts \
        "srt://127.0.0.1:$PORT?mode=caller&passphrase=$1&streamid=#!::r=live/crypto,m=publish,s=encoder-a" \
        >"$2" 2>&1
}

# ---------------------------------------------------------------------------
# case 1: matching passphrase
# ---------------------------------------------------------------------------

PORT=24610
write_conf "media_srt_crypto \"$PASS\";"

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || {
    echo "FAIL: configuration with media_srt_crypto was rejected"
    exit 1
}

start_nginx "$RUN" 'srt listener ready' || exit 1

publish "$PASS" "$RUN/case1.log" 6 &
PUB=$!

for _ in $(seq 1 120); do
    grep -q 'srt source open' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.1
done

wait "$PUB" 2>/dev/null
sleep 1

if ! grep -q 'srt source open app=live stream=crypto source=encoder-a' \
        "$RUN/logs/error.log"; then
    echo "FAIL: the encrypted publisher was not registered"
    tail -5 "$RUN/logs/error.log"
    exit 1
fi

SEGMENTS=$(ls "$RUN/hls/live/crypto"/*.ts 2>/dev/null | wc -l)

if [ "$SEGMENTS" -eq 0 ]; then
    echo "FAIL: encrypted ingest produced no HLS segments"
    exit 1
fi

# The newest part is still being written and the first one may open mid
# access unit, so the assertion is that the program carried media: at least
# one closed segment has to decode without errors.
GOOD=""

for SEG in "$RUN"/hls/live/crypto/*.ts; do
    if [ -z "$(ffmpeg -hide_banner -v error -i "$SEG" -f null - 2>&1)" ]; then
        GOOD="$SEG"
        break
    fi
done

if [ -z "$GOOD" ]; then
    echo "FAIL: no segment decodes cleanly ($SEGMENTS produced)"
    exit 1
fi

echo "   encrypted ingest ok ($(basename "$GOOD"), $(stat -c%s "$GOOD") bytes, $SEGMENTS segments)"

stop_nginx "$RUN"

# ---------------------------------------------------------------------------
# case 2: wrong passphrase
# ---------------------------------------------------------------------------

rm -f "$RUN/hls/live/crypto"/*.ts "$RUN/hls/live/crypto"/*.m3u8
start_nginx "$RUN" 'srt listener ready' || exit 1

publish "$WRONG" "$RUN/case2.log" 4 &
PUB=$!

for _ in $(seq 1 60); do
    grep -q 'srt source open' "$RUN/logs/error.log" 2>/dev/null && break
    sleep 0.1
done

wait "$PUB" 2>/dev/null
sleep 0.5

if grep -q 'srt source open' "$RUN/logs/error.log"; then
    echo "FAIL: a publisher with the wrong passphrase was accepted"
    exit 1
fi

if ls "$RUN/hls/live/crypto"/*.ts >/dev/null 2>&1; then
    echo "FAIL: a rejected publisher still produced media"
    exit 1
fi

echo "   wrong passphrase rejected (no source, no media)"

stop_nginx "$RUN"

# ---------------------------------------------------------------------------
# case 3: AES-GCM either works or says why it cannot
# ---------------------------------------------------------------------------

PORT=24611
write_conf "media_srt_crypto \"$PASS\" gcm;"

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || {
    echo "FAIL: media_srt_crypto ... gcm was rejected by the configuration"
    exit 1
}

rm -f "$RUN/logs/error.log"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 50); do
    grep -q 'srt listener ready\|listener failed' "$RUN/logs/error.log" \
        2>/dev/null && break
    sleep 0.1
done

if grep -q 'srt listener ready' "$RUN/logs/error.log"; then
    echo "   aes-gcm: accepted by this SRT library"

    publish "$PASS" "$RUN/case3.log" 4 &
    PUB=$!
    wait "$PUB" 2>/dev/null

    if grep -q 'srt source open' "$RUN/logs/error.log"; then
        echo "   aes-gcm ingest ok"

    else
        echo "FAIL: gcm listener accepted the socket but carried no media"
        exit 1
    fi

else
    if ! grep -q 'AES-GCM is not available' "$RUN/logs/error.log"; then
        echo "FAIL: gcm failed without naming the cause"
        tail -3 "$RUN/logs/error.log"
        exit 1
    fi

    echo "   aes-gcm refused with the reason (library has no AEAD)"
fi

# ---------------------------------------------------------------------------
# case 4: scope -- the global passphrase guards the listener, a per-stream
# passphrase guards that stream's destination
# ---------------------------------------------------------------------------

stop_nginx "$RUN"

rm -rf "$RUN/a" "$RUN/b"
mkdir -p "$RUN/a/conf" "$RUN/a/logs" "$RUN/a/hls" \
         "$RUN/b/conf" "$RUN/b/logs" "$RUN/b/hls"

IN=24612
OUT=24613
GLOBAL="global-secret-1234"
STREAM="stream-secret-5678"

cat > "$RUN/a/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/a/hls;

media_srt_crypto "$GLOBAL";
media_srt_crypto_stream live/crypto "$STREAM";

media_srt_listen 127.0.0.1:$IN;
media_srt_source_priority encoder-a 100;
media_srt_output live/crypto 127.0.0.1:$OUT "#!::r=live/crypto,m=publish,s=qualify-out";
EOF

cat > "$RUN/b/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 256;
}

media_hls $RUN/b/hls;

media_srt_crypto "$STREAM";

media_srt_listen 127.0.0.1:$OUT;
media_srt_source_priority qualify-out 100;
EOF

"$NGINX" -p "$RUN/a" -c conf/nginx.conf -t >/dev/null || {
    echo "FAIL: the scoped configuration was rejected"
    exit 1
}
"$NGINX" -p "$RUN/b" -c conf/nginx.conf -t >/dev/null || {
    echo "FAIL: the destination configuration was rejected"
    exit 1
}

start_nginx "$RUN/b" 'srt listener ready' || exit 1
start_nginx "$RUN/a" 'srt listener ready' || exit 1

PORT=$IN
publish "$GLOBAL" "$RUN/case4.log" 8 &
PUB=$!
wait "$PUB" 2>/dev/null
sleep 2

if ! grep -q 'srt source open' "$RUN/a/logs/error.log"; then
    echo "FAIL: the global passphrase did not admit the publisher"
    exit 1
fi

if ! grep -q 'srt source open app=live stream=crypto source=qualify-out' \
        "$RUN/b/logs/error.log"; then
    echo "FAIL: the destination did not connect with the per-stream passphrase"
    tail -3 "$RUN/b/logs/error.log"
    exit 1
fi

GOOD=""
for SEG in "$RUN"/b/hls/live/crypto/*.ts; do
    if [ -z "$(ffmpeg -hide_banner -v error -i "$SEG" -f null - 2>&1)" ]; then
        GOOD="$SEG"
        break
    fi
done

if [ -z "$GOOD" ]; then
    echo "FAIL: the encrypted destination carried no decodable media"
    exit 1
fi

echo "   scope ok (listener=global, destination=live/crypto override)"

stop_nginx "$RUN/a"
stop_nginx "$RUN/b"

trap - EXIT
cleanup

echo "== srt crypto ok"

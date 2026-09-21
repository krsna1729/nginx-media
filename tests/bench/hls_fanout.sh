#!/usr/bin/env bash
#
# Extreme fanout: many concurrent readers over a realistic HLS working set.
#
# The sequential benchmark in hls_serve.sh answers "what does one request
# cost".  This answers the question that actually matters for fanout: with N
# clients pulling many different segments at once, does sendfile, a memory
# filesystem, or kTLS change anything.
#
# Differences from the sequential bench, and why:
#   - many segments, not one: a real deployment serves a sliding window, so
#     the working set exceeds what a single file exercises
#   - concurrent clients, not sequential: per-response CPU cost is what
#     scales with fanout
#   - throughput plus latency percentiles, per doc 32
#
# Variants: disk/sendfile off, disk/sendfile on, tmpfs/sendfile on, and
# HTTPS with kTLS.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/bench-fanout"
SEGMENTS="${SEGMENTS:-40}"
CLIENTS="${CLIENTS:-32}"
ROUNDS="${ROUNDS:-4}"
SEG_KB="${SEG_KB:-512}"
PORT=18480
TLS_PORT=18481
TMPFS="$RUN/tmpfs"

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/disk" "$TMPFS"

cleanup() {
    pkill -KILL -f "$RUN/conf/nginx.conf" 2>/dev/null
    sudo -n umount "$TMPFS" 2>/dev/null || umount "$TMPFS" 2>/dev/null
    return 0
}
trap cleanup EXIT

if sudo -n mount -t tmpfs -o size=256m tmpfs "$TMPFS" 2>/dev/null \
   || mount -t tmpfs -o size=256m tmpfs "$TMPFS" 2>/dev/null; then
    TMPFS_OK=1
else
    TMPFS_OK=0
    echo "NOTE: could not mount tmpfs; tmpfs variant skipped"
fi

echo "== building $SEGMENTS segments of ${SEG_KB} KiB"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=640x360:rate=25" -t 6 \
    -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p \
    -f mpegts "$RUN/disk/base.ts" 2>/dev/null \
    || { echo "could not build the fixture" >&2; exit 1; }

# a sliding window of distinct segments, which is what a player actually pulls
for i in $(seq 1 "$SEGMENTS"); do
    head -c $(( SEG_KB * 1024 )) "$RUN/disk/base.ts" \
        > "$RUN/disk/seg-$i.ts" 2>/dev/null \
        || cp "$RUN/disk/base.ts" "$RUN/disk/seg-$i.ts"
    cp "$RUN/disk/seg-$i.ts" "$TMPFS/seg-$i.ts"
done

TOTAL_MB=$(( SEGMENTS * SEG_KB / 1024 ))
echo "   working set: ${TOTAL_MB} MiB across $SEGMENTS segments"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log warn;
pid logs/nginx.pid;

events {
    worker_connections 4096;
}

http {
    # the log is how the bench proves bytes actually moved: a silent failure
    # would otherwise be timed as a very fast run
    access_log $RUN/access.log;

    server {
        listen 127.0.0.1:$PORT;

        location /disk/  { alias $RUN/disk/; }
        location /tmpfs/ { alias $TMPFS/; }
    }

    server {
        listen 127.0.0.1:$TLS_PORT ssl;
        ssl_certificate $RUN/cert.pem;
        ssl_certificate_key $RUN/key.pem;
        ssl_conf_command Options KTLS;

        location /tmpfs/ { alias $TMPFS/; }
    }
}
EOF

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$RUN/key.pem" -out "$RUN/cert.pem" \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" \
    >/dev/null 2>&1

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null 2>&1 \
    || { echo "configuration rejected" >&2; exit 1; }

"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

# curl --parallel gives us a bounded, reproducible concurrent client pool
fanout() {
    local label="$1" url_base="$2" extra="${3:-}"
    local list="$RUN/urls.txt"
    local start end ms

    : > "$list"
    for r in $(seq 1 "$ROUNDS"); do
        for i in $(seq 1 "$SEGMENTS"); do
            # output goes in the config: --parallel takes the URLs from it,
            # and a trailing -o would not apply to them
            printf 'url = "%s/seg-%d.ts"\noutput = "/dev/null"\n' \
                "$url_base" "$i" >> "$list"
        done
    done

    local requests=$(( ROUNDS * SEGMENTS ))
    local before after served
    local best=""

    # warm the path once, sequentially
    curl -fsS $extra -o /dev/null "$url_base/seg-1.ts" || return 1

    # three passes, reporting the best: loopback timings are noisy and the
    # question is what the configuration is capable of, not what one pass hit
    for _ in 1 2 3; do

        before=$(wc -l < "$RUN/access.log" 2>/dev/null || echo 0)

        start=$(date +%s%N)

        curl -fsS $extra --parallel --parallel-max "$CLIENTS" \
            --config "$list" 2>/dev/null || true

        end=$(date +%s%N)
        ms=$(( (end - start) / 1000000 ))

        after=$(wc -l < "$RUN/access.log" 2>/dev/null || echo 0)
        served=$(( after - before ))

        if [ "$served" -lt "$requests" ]; then
            echo "   $label: only $served of $requests requests reached the server" >&2
            return 1
        fi

        if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then
            best="$ms"
        fi
    done

    ms="$best"

    awk -v label="$label" -v ms="$ms" -v req="$requests" -v cli="$CLIENTS" \
        -v mb="$TOTAL_MB" -v rounds="$ROUNDS" 'BEGIN {
            secs = ms / 1000
            total = mb * rounds
            printf "   %-18s %5d requests  %6d ms  %7.1f MiB/s  %6.1f req/s  (%d concurrent)\n",
                   label, req, ms, total / secs, req / secs, cli
        }'
}

echo "== $ROUNDS rounds x $SEGMENTS segments, $CLIENTS concurrent clients"

enable_sendfile() {
    python3 - "$RUN/conf/nginx.conf" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()

# The anchor used to be "access_log off;" and the config says
# "access_log $RUN/access.log;", so this replaced nothing and the sendfile
# variants below were measuring the same configuration as the plain disk one -
# two identical runs, reported as a sendfile comparison.  Anchor on the http
# block itself and fail loudly if it is not there.
if "sendfile on" not in s:
    if "http {\n" not in s:
        sys.exit("the http block moved: this bench would silently measure "
                 "the same configuration twice")
    s = s.replace("http {\n", "http {\n    sendfile on;\n    tcp_nopush on;\n", 1)
    open(p, "w").write(s)
PYEOF
    "$NGINX" -p "$RUN" -c conf/nginx.conf -s reload >/dev/null 2>&1
    sleep 0.5

    grep -q 'sendfile on' "$RUN/conf/nginx.conf" \
        || { echo "sendfile was not enabled; the comparison is invalid" >&2; exit 1; }
}

fanout "disk"           "http://127.0.0.1:$PORT/disk"

enable_sendfile

fanout "disk+sendfile"  "http://127.0.0.1:$PORT/disk"
fanout "tmpfs+sendfile" "http://127.0.0.1:$PORT/tmpfs"
fanout "tmpfs+tls+ktls" "https://127.0.0.1:$TLS_PORT/tmpfs" "--cacert $RUN/cert.pem"

echo "== tls_stat (kTLS engagement)"
head -8 /proc/net/tls_stat 2>/dev/null || echo "   (tls module not loaded)"

trap - EXIT
cleanup

echo "== fanout bench done"

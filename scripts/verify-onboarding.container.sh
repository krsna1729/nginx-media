#!/usr/bin/env bash
#
# The container half of the onboarding check: run by
# scripts/verify-onboarding.sh inside a clean image, with the repository
# mounted read-only at /src.  It is a separate file rather than a heredoc
# because a script read from stdin cannot also use heredocs for its own
# configuration files.
#
# Every command here is one of the documented ones, in the documented order:
# the prerequisites from docs/deployment.md, the four commands from
# docs/quickstart.md, and the publish/read path the README shows.
set -euo pipefail

DEBIAN_FRONTEND=noninteractive
export DEBIAN_FRONTEND

echo "== prerequisites (docs/deployment.md)"
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential pkg-config curl ca-certificates python3 \
    libpcre2-dev zlib1g-dev libssl-dev libsrt-openssl-dev \
    ffmpeg >/dev/null

mkdir -p /work
cp -a /src/. /work/
cd /work

echo "== make nginx"
make nginx >/tmp/nginx.log 2>&1 || { tail -20 /tmp/nginx.log; exit 1; }
./.build/nginx-install/sbin/nginx -V 2>&1 | head -1

echo "== make unit"
make unit >/tmp/unit.log 2>&1 || { cat /tmp/unit.log; exit 1; }
tail -1 /tmp/unit.log

echo "== make smoke"
make smoke >/tmp/smoke.log 2>&1 || { tail -20 /tmp/smoke.log; exit 1; }
tail -1 /tmp/smoke.log

# --- the quickstart, verbatim: if the documentation drifts, this fails.
echo "== the quickstart: a configuration with nothing declared"
# The workers need to write the HLS root, and a root master drops them to
# `nobody` unless the configuration says otherwise - the trap docs/deployment.md
# names, and the reason the unit runs as a service user.  Here: same shape,
# smaller.
id media 2>/dev/null || useradd --system --no-create-home media
mkdir -p /run/media/logs /var/lib/nginx/media/hls
chown -R media: /var/lib/nginx/media /run/media
cat > /run/media/nginx.conf <<'CONF'
user media;
worker_processes 1;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls /var/lib/nginx/media/hls;
media_ingest_secret /var/lib/nginx/media/ingest.secret;
media_srt_listen 127.0.0.1:9000;

http {
    server {
        listen 8080;

        location /media/api/ { media_api; }

        location /hls/ { alias /var/lib/nginx/media/hls/; }
    }
}
CONF
./.build/nginx-install/sbin/nginx -p /run/media -c nginx.conf

empty="$(curl -fsS http://127.0.0.1:8080/media/api/v1/streams)"
case "$empty" in
    *'"count":0'*) echo "   an empty list, as documented: $empty" ;;
    *) echo "expected no streams yet, got: $empty" >&2; exit 1 ;;
esac

echo "== a program"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"demo"}' \
    http://127.0.0.1:8080/media/api/v1/streams >/dev/null

echo "== a live encoder instead (SRT publish)"
# the quickstart: a source is provisioned and the API issues the key
KEY="$(curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"id":"enc1","type":"srt","priority":100}' \
    http://127.0.0.1:8080/media/api/v1/streams/live/demo/sources \
    | python3 -c "import json,sys; print(json.load(sys.stdin)['key'])")"
[ -n "$KEY" ] || { echo "the API issued no key" >&2; exit 1; }
echo "   issued key: $KEY"

timeout 30 ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x180:rate=25" -t 6 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -f mpegts "srt://127.0.0.1:9000?streamid=$KEY" \
    >/tmp/pub.log 2>&1 || true

echo "== watch it come out"
for _ in $(seq 1 100); do
    curl -fsS http://127.0.0.1:8080/hls/live/demo/index.m3u8 \
        >/tmp/playlist 2>/dev/null && [ -s /tmp/playlist ] && break
    sleep 0.1
done
[ -s /tmp/playlist ] \
    || { echo "no playlist at /hls/live/demo/index.m3u8" >&2
         cat /tmp/pub.log >&2; exit 1; }
grep -q '#EXTM3U' /tmp/playlist \
    || { echo "the playlist is not a playlist:" >&2; head -3 /tmp/playlist >&2; exit 1; }
echo "   playlist: $(head -1 /tmp/playlist)"

view="$(curl -fsS http://127.0.0.1:8080/media/api/v1/streams/live/demo)"
case "$view" in
    *fanout_ms*) echo "   the program's own view carries fanout_ms" ;;
    *) echo "the program view has no fanout_ms: $view" >&2; exit 1 ;;
esac

./.build/nginx-install/sbin/nginx -p /run/media -c nginx.conf -s quit
echo "== onboarding ok"

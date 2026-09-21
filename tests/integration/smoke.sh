#!/usr/bin/env bash
#
# Smoke test: start the nginx built with the nginx-media module, serve one
# request, shut it down cleanly.  Proves the module links and the module
# lifecycle integrates with a real nginx process.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/smoke"
PORT=18432

if [ ! -x "$NGINX" ]; then
    echo "nginx is not built; run: make nginx" >&2
    exit 1
fi

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/html"
printf 'ok\n' > "$RUN/html/index.html"

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 64;
}

http {
    access_log off;

    server {
        listen 127.0.0.1:$PORT;
        root html;
        index index.html;
    }
}
EOF

echo "== config test"
"$NGINX" -p "$RUN" -c conf/nginx.conf -t

echo "== start"
"$NGINX" -p "$RUN" -c conf/nginx.conf

for _ in $(seq 1 50); do
    [ -s "$RUN/logs/nginx.pid" ] && break
    sleep 0.1
done

PID="$(cat "$RUN/logs/nginx.pid")"

if ! kill -0 "$PID" 2>/dev/null; then
    echo "nginx master did not stay up" >&2
    cat "$RUN/logs/error.log" >&2
    exit 1
fi

BODY="$(curl -fsS "http://127.0.0.1:$PORT/" || true)"

case "$BODY" in
    *ok*)
        echo "== response ok"
        ;;
    *)
        echo "unexpected response: '$BODY'" >&2
        "$NGINX" -p "$RUN" -c conf/nginx.conf -s quit || true
        exit 1
        ;;
esac

echo "== stop"
"$NGINX" -p "$RUN" -c conf/nginx.conf -s quit

for _ in $(seq 1 50); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.1
done

if kill -0 "$PID" 2>/dev/null; then
    echo "nginx did not shut down" >&2
    kill -9 "$PID" || true
    exit 1
fi

echo "== smoke ok"

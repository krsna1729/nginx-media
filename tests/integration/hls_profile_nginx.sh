#!/usr/bin/env bash
#
# HLS destination profiles (normative revision): youtube_live, and the
# redaction that goes with it.
#
# A profile is validation and defaults layered on the generic HLS publisher,
# not a special path.  The platform's rules are checked at configuration time,
# where an operator can act on them, rather than on the wire at three in the
# morning.  Nothing here talks to YouTube: the contract is tested as a
# contract, which is what the boundary allows.
#
#   - https is required, http is refused
#   - segment duration and playlist window are validated, not clamped
#   - defaults are filled in when unset
#   - an unknown profile is refused
#   - the endpoint's credential never reaches a log

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX="$ROOT/.build/nginx-install/sbin/nginx"
RUN="$ROOT/.build/hls-profile"
HTTP_PORT=18570

rm -rf "$RUN"
mkdir -p "$RUN/conf" "$RUN/logs" "$RUN/hls"

cleanup() {
    pkill -KILL -f 'nginx: ' 2>/dev/null
    return 0
}
trap cleanup EXIT

cat > "$RUN/conf/nginx.conf" <<EOF
worker_processes 1;
daemon on;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls $RUN/hls;

http {
    access_log off;
    server {
        listen 127.0.0.1:$HTTP_PORT;
        location /media/api/ { media_api; }
    }
}
EOF

"$NGINX" -p "$RUN" -c conf/nginx.conf -t >/dev/null || exit 1
"$NGINX" -p "$RUN" -c conf/nginx.conf
sleep 0.5

API="http://127.0.0.1:$HTTP_PORT/media/api/v1"

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"yt"}' "$API/streams" >/dev/null

add() {
    # $1: id, $2: json body tail
    curl -sS -o "$RUN/last.json" -w '%{http_code}' \
        -X POST -H 'Content-Type: application/json' \
        -d "$2" "$API/streams/live/yt/destinations"
}

echo "== http is refused by a profile that requires https"
CODE="$(add bad1 '{"id":"bad1","type":"hls_push","profile":"youtube_live","host":"http://origin.example/live/","path":"'"$RUN"'/hls"}')"
cat "$RUN/last.json"; echo

[ "$CODE" = "400" ] || { echo "expected 400, got $CODE" >&2; exit 1; }

grep -q 'requires an https endpoint' "$RUN/last.json" \
    || { echo "the refusal did not name the reason" >&2; exit 1; }

echo "== a segment duration outside the contract is refused, not clamped"
CODE="$(add bad2 '{"id":"bad2","type":"hls_push","profile":"youtube_live","host":"https://origin.example/live/","path":"'"$RUN"'/hls","segment_duration_ms":9000}')"
cat "$RUN/last.json"; echo

[ "$CODE" = "400" ] || { echo "expected 400, got $CODE" >&2; exit 1; }

CODE="$(add bad3 '{"id":"bad3","type":"hls_push","profile":"youtube_live","host":"https://origin.example/live/","path":"'"$RUN"'/hls","segment_duration_ms":100}')"

[ "$CODE" = "400" ] || { echo "a too-short duration should be refused, got $CODE" >&2; exit 1; }

echo "== a playlist window beyond the contract is refused"
CODE="$(add bad4 '{"id":"bad4","type":"hls_push","profile":"youtube_live","host":"https://origin.example/live/","path":"'"$RUN"'/hls","playlist_window":12}')"
cat "$RUN/last.json"; echo

[ "$CODE" = "400" ] || { echo "expected 400, got $CODE" >&2; exit 1; }

echo "== an unknown profile is refused"
CODE="$(add bad5 '{"id":"bad5","type":"hls_push","profile":"nosuchplatform","host":"https://origin.example/live/","path":"'"$RUN"'/hls"}')"
cat "$RUN/last.json"; echo

[ "$CODE" = "400" ] || { echo "expected 400, got $CODE" >&2; exit 1; }

echo "== a conforming destination is accepted with the profile's defaults"
SECRET="SECRET-KEY-DO-NOT-LOG"
CODE="$(add yt1 '{"id":"yt1","type":"hls_push","profile":"youtube_live","host":"https://origin.example/live/?key='"$SECRET"'","path":"'"$RUN"'/hls"}')"
cat "$RUN/last.json"; echo

[ "$CODE" = "201" ] || { echo "expected 201, got $CODE" >&2; exit 1; }

DEST="$(curl -fsS "$API/streams/live/yt/destinations/yt1")"
printf '%s\n' "$DEST"

echo "== and the API read reports the endpoint without the credential"
case "$DEST" in
    *"$SECRET"*)
        echo "FAIL: the credential appears in an API read" >&2
        exit 1
        ;;
esac

case "$DEST" in
    *"https://origin.example/live/"*)
        ;;
    *)
        echo "FAIL: the API read lost the endpoint itself" >&2
        exit 1
        ;;
esac

LIST="$(curl -fsS "$API/streams/live/yt")"

case "$LIST" in
    *"$SECRET"*)
        echo "FAIL: the credential appears in the stream listing" >&2
        exit 1
        ;;
esac

echo "   the API read and the listing keep the origin, not the key"

echo "== the credential never reaches a log"
# the endpoint carries its key in the URL, so the reporting path redacts it
if grep -q "$SECRET" "$RUN/logs/error.log"; then
    echo "FAIL: the credential appears in the error log" >&2
    grep -a "$SECRET" "$RUN/logs/error.log" | head -2 >&2
    exit 1
fi

grep -a 'hls push destination yt1 started' "$RUN/logs/error.log" \
    | tail -1

[ "$(grep -ac 'hls push destination yt1 started' "$RUN/logs/error.log")" -ge 1 ] \
    || { echo "the destination was not reported at all" >&2; exit 1; }

echo "   no credential in the log, and the destination was reported"

trap - EXIT
cleanup

echo "== hls profile ok"

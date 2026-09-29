#!/usr/bin/env bash
#
# Build the statically linked nginx, and report what it actually is.
#
#   scripts/build-static.sh            # build, extract to .build/static/nginx
#   scripts/build-static.sh --report   # print what was built, from the image
#
# The artifact is one file with no library dependencies: nginx, the module,
# libsrt (at the pin in scripts/srt-pins.sh), OpenSSL, PCRE2 and zlib are all
# linked in.  That is what makes it releasable without a dependency matrix -
# and it is also its cost, because a libsrt or OpenSSL fix means a new
# artifact rather than a new system library.  Both facts are in
# docs/development.md.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$root/scripts/srt-pins.sh"

# docker needs root here; the Makefile's convention is the same one.
docker_cmd=("${DOCKER:-docker}")
docker info >/dev/null 2>&1 || docker_cmd=(sudo -n docker)

image="${STATIC_IMAGE:-nginx-media-static}"
out="${STATIC_OUT:-$root/.build/static}"
nginx_version="${NGINX_VERSION:-1.30.5}"

"${docker_cmd[@]}" build -f "$root/Containerfile.static" \
    --target build \
    --build-arg "SRT_REF=$SRT_REF" \
    --build-arg "SRT_REPO=$SRT_REPO" \
    --build-arg "NGINX_VERSION=$nginx_version" \
    -t "$image" "$root"

mkdir -p "$out"
id="$("${docker_cmd[@]}" create "$image")"
trap '"${docker_cmd[@]}" rm -f "$id" >/dev/null 2>&1 || true' EXIT
"${docker_cmd[@]}" cp "$id:/opt/nginx/sbin/nginx" "$out/nginx"
# docker writes as root: the artifact belongs to whoever ran this.
sudo -n chown "$(id -u):$(id -g)" "$out/nginx" 2>/dev/null || true
chmod +x "$out/nginx"

echo
echo "== $out/nginx"
ls -l "$out/nginx" | awk '{ printf "   %s bytes\n", $5 }'
# `ldd` exits nonzero for a static binary, so the answer is read from the
# dynamic section: a static executable has no NEEDED entries at all.
if command -v readelf >/dev/null; then
    needed="$(readelf -d "$out/nginx" 2>/dev/null | grep -c NEEDED || true)"
    if [ "${needed:-0}" -eq 0 ]; then
        echo "   linkage: static (no NEEDED entries)"
    else
        echo "   linkage: DYNAMIC - this is not the artifact we wanted:" >&2
        readelf -d "$out/nginx" | grep NEEDED >&2
        exit 1
    fi
fi
"$out/nginx" -V 2>&1 | sed -n '1,2p' | sed 's/^/   /'
echo "   libsrt: $SRT_REF (Haivision/srt, static)"

#!/usr/bin/env bash
#
# Fetch and build the pinned NGINX version with the nginx-media module.
# Everything lands in .build/ and never touches the system nginx.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${NGINX_VERSION:-1.30.5}"
BUILD="${BUILD_DIR:-$ROOT/.build}"
SRC="$BUILD/nginx-$VERSION"
# The prefix is compiled into the binary: nginx resolves its temp paths, its
# default error log and its prefix-relative directories against it at run
# time.  A container needs a real installed layout rather than a path inside
# the build tree, so this is overridable.
PREFIX="${NGINX_PREFIX:-$BUILD/nginx-install}"

mkdir -p "$BUILD"

if [ ! -d "$SRC" ]; then
    echo "== fetching nginx $VERSION"
    curl -fsSL "https://nginx.org/download/nginx-$VERSION.tar.gz" \
        -o "$BUILD/nginx-$VERSION.tar.gz"
    tar -xzf "$BUILD/nginx-$VERSION.tar.gz" -C "$BUILD"
fi

cd "$SRC"

# The backend selection changes which sources are linked, so it has to
# invalidate the configure output the same way an edited config does.  So does
# this script itself: adding a configure flag would otherwise be ignored on a
# tree that already has an objs/Makefile.
SRT_BACKEND="${MEDIA_SRT_BACKEND:-srt}"
SRT_STAMP="$BUILD/.srt-backend"
# The prefix is compiled into the binary, so changing it has to reconfigure
# just as an edited config does.  Without it in the stamp, a tree already
# configured for one prefix silently keeps it and installs somewhere else.
PREFIX_STAMP="$BUILD/.prefix"

if [ ! -f objs/Makefile ] || [ "$ROOT/config" -nt objs/Makefile ] \
   || [ "${BASH_SOURCE[0]}" -nt objs/Makefile ] \
   || [ ! -f "$SRT_STAMP" ] || [ "$(cat "$SRT_STAMP" 2>/dev/null)" != "$SRT_BACKEND" ] \
   || [ ! -f "$PREFIX_STAMP" ] || [ "$(cat "$PREFIX_STAMP" 2>/dev/null)" != "$PREFIX" ]; then
    echo "== configuring nginx $VERSION with the nginx-media module"
    if ! ./configure \
            --prefix="$PREFIX" \
            --add-module="$ROOT" \
            --with-threads \
            --with-http_ssl_module \
            --without-http_rewrite_module \
            --without-http_gzip_module \
            >"$BUILD/configure.log" 2>&1
    then
        echo "configure failed; last lines:" >&2
        tail -n 40 "$BUILD/configure.log" >&2
        exit 1
    fi

    echo "$SRT_BACKEND" > "$SRT_STAMP"
    echo "$PREFIX" > "$PREFIX_STAMP"
fi

echo "== building nginx"
if ! make -j"$(nproc)" >"$BUILD/build.log" 2>&1; then
    echo "build failed; last lines:" >&2
    tail -n 60 "$BUILD/build.log" >&2
    exit 1
fi

echo "== installing to $PREFIX"
make install >"$BUILD/install.log" 2>&1

test -x "$PREFIX/sbin/nginx"
echo "== ok: $PREFIX/sbin/nginx"

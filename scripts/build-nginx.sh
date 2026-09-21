#!/usr/bin/env bash
#
# Fetch and build the pinned NGINX version with the nginx-media module.
# Everything lands in .build/ and never touches the system nginx.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${NGINX_VERSION:-1.30.5}"
BUILD="${BUILD_DIR:-$ROOT/.build}"
SRC="$BUILD/nginx-$VERSION"
PREFIX="$BUILD/nginx-install"

mkdir -p "$BUILD"

if [ ! -d "$SRC" ]; then
    echo "== fetching nginx $VERSION"
    curl -fsSL "https://nginx.org/download/nginx-$VERSION.tar.gz" \
        -o "$BUILD/nginx-$VERSION.tar.gz"
    tar -xzf "$BUILD/nginx-$VERSION.tar.gz" -C "$BUILD"
fi

cd "$SRC"

if [ ! -f objs/Makefile ]; then
    echo "== configuring nginx $VERSION with the nginx-media module"
    if ! ./configure \
            --prefix="$PREFIX" \
            --add-module="$ROOT" \
            --with-threads \
            --without-http_rewrite_module \
            --without-http_gzip_module \
            >"$BUILD/configure.log" 2>&1
    then
        echo "configure failed; last lines:" >&2
        tail -n 40 "$BUILD/configure.log" >&2
        exit 1
    fi
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

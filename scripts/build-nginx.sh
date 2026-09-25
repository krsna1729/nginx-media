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
# Which SRT library is linked is part of the same selection: the package name
# and SRT_DIR change the link line exactly as the backend list does.
SRT_SELECTION="$SRT_BACKEND|${MEDIA_SRT_PKG:-srt}|${SRT_DIR:-}"
# The prefix is compiled into the binary, so changing it has to reconfigure
# just as an edited config does.  Without it in the stamp, a tree already
# configured for one prefix silently keeps it and installs somewhere else.
PREFIX_STAMP="$BUILD/.prefix"

# Extra compiler and linker flags, appended to nginx's own.  A sanitizer build
# needs both halves, and the flags have to be stamped like everything else that
# changes the binary: without the stamp a tree configured for a plain build
# would keep it and quietly ignore them.
NGINX_CC_OPT="${NGINX_CC_OPT:-}"
NGINX_LD_OPT="${NGINX_LD_OPT:-}"
OPT_STAMP="$BUILD/.cc-opt"
OPT_VALUE="$NGINX_CC_OPT|$NGINX_LD_OPT"

# One argv element each: nginx takes the whole flag string as one argument, so
# a value with spaces in it must not be word split on the way in.
CONFIGURE_OPTS=()

if [ -n "$NGINX_CC_OPT" ]; then
    CONFIGURE_OPTS+=("--with-cc-opt=$NGINX_CC_OPT")
fi

if [ -n "$NGINX_LD_OPT" ]; then
    CONFIGURE_OPTS+=("--with-ld-opt=$NGINX_LD_OPT")
fi

if [ ! -f objs/Makefile ] || [ "$ROOT/config" -nt objs/Makefile ] \
   || [ "${BASH_SOURCE[0]}" -nt objs/Makefile ] \
   || [ ! -f "$SRT_STAMP" ] || [ "$(cat "$SRT_STAMP" 2>/dev/null)" != "$SRT_SELECTION" ] \
   || [ ! -f "$PREFIX_STAMP" ] || [ "$(cat "$PREFIX_STAMP" 2>/dev/null)" != "$PREFIX" ] \
   || [ ! -f "$OPT_STAMP" ] || [ "$(cat "$OPT_STAMP" 2>/dev/null)" != "$OPT_VALUE" ]; then
    echo "== configuring nginx $VERSION with the nginx-media module"
    if ! ./configure \
            --prefix="$PREFIX" \
            --add-module="$ROOT" \
            --with-threads \
            --with-http_ssl_module \
            --without-http_rewrite_module \
            --without-http_gzip_module \
            "${CONFIGURE_OPTS[@]}" \
            >"$BUILD/configure.log" 2>&1
    then
        echo "configure failed; last lines:" >&2
        tail -n 40 "$BUILD/configure.log" >&2
        exit 1
    fi

    echo "$SRT_SELECTION" > "$SRT_STAMP"
    echo "$PREFIX" > "$PREFIX_STAMP"
    echo "$OPT_VALUE" > "$OPT_STAMP"
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

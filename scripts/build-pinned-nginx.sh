#!/usr/bin/env bash
#
# Build an nginx with the module linked against a pinned SRT library.
#
#   build-pinned-nginx.sh haivision        # the reference library, v1.5.7
#   build-pinned-nginx.sh robotweax        # the clean-slate implementation
#   build-pinned-nginx.sh <name> <repo> <ref> <pkg>
#
# Local and CI run the same script with the same pins from
# scripts/srt-pins.sh, so "it passed in CI" and "it passed here" mean the
# same binary.  The result is .build/nginx-<name>/install/sbin/nginx, which
# the capacity harness takes through CAPACITY_NGINX.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$root/scripts/srt-pins.sh"

name="${1:?usage: build-pinned-nginx.sh <haivision|robotweax|name> [repo ref pkg]}"
case "$name" in
    haivision)
        repo="$SRT_REPO"; ref="$SRT_REF"; pkg="$SRT_PKG" ;;
    robotweax)
        repo="$ROBOTWEAX_REPO"; ref="$ROBOTWEAX_REF"; pkg="$ROBOTWEAX_PKG" ;;
    *)
        repo="${2:?}"; ref="${3:?}"; pkg="${4:?}" ;;
esac

prefix="$root/.build/srt-$name"
build="$root/.build/nginx-$name"

"$root/scripts/build-srt.sh" "$repo" "$ref" "$prefix" "$pkg"

# The nginx tree is copied rather than shared: the module's build stamps the
# library selection, and two selections must not fight over one objs/.
source_dir="$root/.build/nginx-1.30.5"
[ -d "$source_dir" ] \
    || { echo "no $source_dir; run: make nginx" >&2; exit 1; }
rm -rf "$build"
mkdir -p "$build"
cp -r "$source_dir" "$build/"
rm -rf "$build/nginx-1.30.5/objs"

# The pinned library carries the same soname as the system one, so the binary
# records where to find its own: an rpath, not an environment variable that
# every future invocation has to remember.
BUILD_DIR="$build" \
NGINX_PREFIX="$build/install" \
SRT_DIR="$prefix" MEDIA_SRT_PKG="$pkg" \
NGINX_LD_OPT="-Wl,-rpath,$prefix/lib" \
    "$root/scripts/build-nginx.sh"

echo "pinned nginx: $build/install/sbin/nginx ($pkg at $ref)"

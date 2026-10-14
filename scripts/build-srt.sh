#!/usr/bin/env bash
#
# Build an SRT library from source at a pinned ref.
#
#   build-srt.sh <repo url> <ref> <install prefix> [package name]
#
# The capacity tiers link a pinned library instead of whatever the runner's
# distribution ships, so a number that moves between weeks moved because the
# code moved.  The distribution packages still matter - they are what users
# have, and the ci matrix deliberately spans three of them - but a capacity
# claim is about one library at one version.
#
# The ref is a commit, not a tag: a tag can be moved, and the whole point of
# the pin is that it cannot.
set -euo pipefail

repo="${1:?usage: build-srt.sh <repo> <ref> <prefix> [pkg]}"
ref="${2:?}"
prefix="${3:?}"
pkg="${4:-srt}"
work="${SRT_BUILD_DIR:-$(mktemp -d)}"

# Reuse only content-bound metadata from a completed build. A pkg-config
# Version containing the configured ref is not proof of the installed source.
pc="$prefix/lib/pkgconfig/$pkg.pc"
metadata="$prefix/lib/$pkg.provenance.json"
if [ -f "$pc" ] && python3 - "$metadata" "$repo" "$ref" "${SRT_SHARED:-yes}" <<'PY'
import hashlib, json, pathlib, sys
try:
    evidence = json.loads(pathlib.Path(sys.argv[1]).read_text())
    library = pathlib.Path(evidence["artifact"]["path"])
    valid = (evidence["schema"] == "nginx-media.transport-build/1"
             and evidence["source"]["repository"] == sys.argv[2]
             and evidence["source"]["commit"] == sys.argv[3]
             and evidence["linkage"] == ("static" if sys.argv[4] == "no" else "shared")
             and hashlib.sha256(library.read_bytes()).hexdigest()
                 == evidence["artifact"]["sha256"]
             and json.loads(pathlib.Path(str(library) + ".provenance.json").read_text())
                 == evidence)
except (OSError, ValueError, KeyError, TypeError):
    valid = False
sys.exit(0 if valid else 1)
PY
then
    echo "$pkg already built at $ref in $prefix"
    exit 0
fi

echo "building $repo at $ref -> $prefix ($pkg)"

rm -rf "$work/src" "$work/build"
git init -q "$work/src"
git -C "$work/src" fetch -q --depth 1 "$repo" "$ref"
git -C "$work/src" checkout -q FETCH_HEAD
head="$(git -C "$work/src" rev-parse HEAD)"
[ "$head" = "$ref" ] \
    || { echo "$repo: wanted $ref, got $head" >&2; exit 1; }

# Two null dereferences in robotweax/srt's compat layer warn under GCC 16,
# which the project's own toolchain does not.  Warnings are errors upstream;
# they are not here, because a warning in a compat shim is not this project's
# to fix.
warnings_off=()
case "$repo" in
    *robotweax*) warnings_off=( -DROBOTWEAX_SRT_WARNINGS_AS_ERRORS=OFF ) ;;
esac

cmake -S "$work/src" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_SHARED_LIBS="$([ "${SRT_SHARED:-yes}" = no ] && echo OFF || echo ON)" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_INCLUDEDIR=include \
    "${warnings_off[@]+"${warnings_off[@]}"}"
ninja -C "$work/build" install

# Which library landed, and is it shared?  A pinned build that installed only
# a static archive would leave the linker to find -lsrt somewhere else - the
# system's library - and the "pinned" binary would quietly be the old one.
# The name comes from what is on disk, never from an assumption.
if [ "${SRT_SHARED:-yes}" = no ]; then
    # A static artifact wants the archive, and it must be the archive: a
    # static build that silently linked the system's shared library would be
    # no more portable than the dynamic one.
    archive=""
    for candidate in "$prefix"/lib/libsrt.a "$prefix"/lib/lib*srt.a; do
        [ -e "$candidate" ] || continue
        archive="$(basename "$candidate")"
        break
    done
    [ -n "$archive" ] \
        || { echo "$repo at $ref installed no static archive under $prefix/lib" \
                  "(only: $(ls "$prefix"/lib 2>/dev/null | tr '\n' ' '))" >&2
             exit 1; }
    libname="${archive#lib}"
    libname="${libname%.a}"
else
    shared=""
    for candidate in "$prefix"/lib/libsrt.so "$prefix"/lib/lib*srt.so; do
        [ -e "$candidate" ] || continue
        shared="$(basename "$candidate")"
        break
    done
    [ -n "$shared" ] \
        || { echo "$repo at $ref installed no shared library under $prefix/lib" \
                  "(only: $(ls "$prefix"/lib 2>/dev/null | tr '\n' ' '))" >&2
             exit 1; }
    libname="${shared#lib}"
    libname="${libname%.so}"
fi

# The module finds the library through pkg-config, so the pinned build needs
# a .pc of its own: the package name is what the build stamps and what the
# diagnostics report.  Its own .pc is left alone - this one is separate.
mkdir -p "$(dirname "$pc")"
# Capture the installed library before publishing the selection .pc. The
# sidecar lives beside the resolved artifact, not beside a movable symlink.
python3 - "$prefix/lib/${archive:-${shared:-}}" "$metadata" "$repo" "$head" \
    "$work/build" "$pkg" "${SRT_SHARED:-yes}" <<'PY'
import hashlib, json, pathlib, subprocess, sys
library = pathlib.Path(sys.argv[1]).resolve(strict=True)
cache = {}
for line in (pathlib.Path(sys.argv[5]) / "CMakeCache.txt").read_text().splitlines():
    if "=" in line and not line.startswith(("//", "#")):
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value
compiler = cache.get("CMAKE_CXX_COMPILER") or cache.get("CMAKE_C_COMPILER")
try:
    version = subprocess.run([compiler, "--version"], check=True,
                             capture_output=True, text=True).stdout.strip()
except (OSError, subprocess.SubprocessError, TypeError):
    version = None
evidence = {
    "schema": "nginx-media.transport-build/1",
    "source": {"repository": sys.argv[3], "commit": sys.argv[4]},
    "package": sys.argv[6],
    "linkage": "static" if sys.argv[7] == "no" else "shared",
    "artifact": {"path": str(library),
                 "sha256": hashlib.sha256(library.read_bytes()).hexdigest()},
    "compiler": {"path": compiler, "version": version},
    "cmake": {key: value for key, value in cache.items()
              if key in ("CMAKE_BUILD_TYPE", "BUILD_SHARED_LIBS")
              or key.startswith(("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS",
                                 "CMAKE_SHARED_LINKER_FLAGS", "CMAKE_STATIC_LINKER_FLAGS"))},
}
# compile_commands carries per-target flags that are not all in CMakeCache.
commands = pathlib.Path(sys.argv[5]) / "compile_commands.json"
evidence["compile_commands"] = json.loads(commands.read_text()) if commands.exists() else None
for path in (pathlib.Path(str(library) + ".provenance.json"), pathlib.Path(sys.argv[2])):
    temporary = pathlib.Path(str(path) + ".tmp")
    temporary.write_text(json.dumps(evidence, indent=1) + "\n")
    temporary.replace(path)
PY
cat > "$pc" <<EOF
prefix=$prefix
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: $pkg
Description: SRT library built from $repo at $ref
Version: $ref
Libs: -L\${libdir} -l$libname
Cflags: -I\${includedir}
EOF

echo "pinned $pkg at $head in $prefix"

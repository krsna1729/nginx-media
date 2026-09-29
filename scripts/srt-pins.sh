#!/usr/bin/env bash
#
# The SRT libraries the capacity tiers link, in one place.
#
#   . scripts/srt-pins.sh          # sets SRT_* and ROBOTWEAX_*
#   scripts/srt-pins.sh --print    # one "name=value" per line
#
# The refs are commits, not tags: a tag can be moved, and the point of a pin
# is that it cannot.  The values are read by scripts/build-pinned-nginx.sh,
# so a local run and a CI run link exactly the same library.

# Haivision/srt - the reference implementation, latest release v1.5.7.
SRT_REPO="https://github.com/Haivision/srt"
SRT_REF="899348d8318eb9a3c5a5b6ec43c4a1114288773a"
# Not "srt": the pinned build writes its own pkg-config file, and a distinct
# package name is what makes the build prefer it to the system's libsrt.
SRT_PKG="srt-pinned"

# robotweax/srt - the clean-slate implementation of the same C API, latest
# release v0.2.6.  Its own pkg-config file is called robotweax-srt; ours is
# robotweax-srt-pinned so the pinned library is never confused with an
# installed one.
ROBOTWEAX_REPO="https://github.com/robotweax/srt"
ROBOTWEAX_REF="50cba37bc89f423ee7c0a292c3c3eae71b5fc7f9"
ROBOTWEAX_PKG="robotweax-srt-pinned"

if [ "${1:-}" = "--print" ]; then
    printf 'SRT_REPO=%s\nSRT_REF=%s\nSRT_PKG=%s\n' \
        "$SRT_REPO" "$SRT_REF" "$SRT_PKG"
    printf 'ROBOTWEAX_REPO=%s\nROBOTWEAX_REF=%s\nROBOTWEAX_PKG=%s\n' \
        "$ROBOTWEAX_REPO" "$ROBOTWEAX_REF" "$ROBOTWEAX_PKG"
fi

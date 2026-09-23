#!/usr/bin/env bash
#
# Sweep the MPEG-TS preparation buffer against real H.264/AAC media.
#
# The runtime's burst is a single bounded backing allocation.  A capacity that
# is too small for one access unit causes the muxer to roll the frame back and
# the retry path must preserve every frame.  This sweep reports the actual
# burst count, byte range and frame rollovers for each capacity instead of
# choosing a number from a bitrate-only estimate.
#
# Environment:
#   CAPACITIES  space-separated byte capacities (default below)
#   FIXTURE    existing MPEG-TS fixture to use instead of regenerating one

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/.build/ts_fixture"
FIXTURE="${FIXTURE:-$ROOT/.build/fixtures/h264_aac.ts}"
CAPACITIES="${CAPACITIES:-1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576}"

if [ ! -x "$BIN" ] || [ ! -f "$FIXTURE" ]; then
    echo "== building the real-media fixture"
    make -C "$ROOT" ts-fixture >/dev/null
fi

[ -x "$BIN" ] || { echo "ts fixture harness was not built" >&2; exit 1; }
[ -f "$FIXTURE" ] || { echo "fixture is missing: $FIXTURE" >&2; exit 1; }

tmp="${TMPDIR:-/tmp}/nginx-media-burst-sizing.$$"
mkdir -p "$tmp"
cleanup() {
    rm -rf "$tmp"
}
trap cleanup EXIT

field() {
    local key="$1"
    local text="$2"
    local token

    for token in $text; do
        case "$token" in
            "$key"=*)
                printf '%s\n' "${token#*=}"
                return 0
                ;;
        esac
    done

    printf '0\n'
}

printf '%-10s %-7s %-8s %-8s %-8s %-8s %-8s %s\n' \
       capacity status frames checked bursts min max rollovers

safe_count=0
first_safe=""
rows=0

for capacity in $CAPACITIES; do
    rows=$((rows + 1))
    output="$tmp/$capacity.log"

    set +e
    "$BIN" "$FIXTURE" "$capacity" >"$output" 2>&1
    rc=$?
    set -e

    remux="$(awk '/^REMUX / { line = $0 } END { print line }' "$output")"
    [ -n "$remux" ] || {
        cat "$output" >&2
        echo "no REMUX measurement for capacity $capacity" >&2
        exit 1
    }

    frames="$(field frames "$remux")"
    checked="$(field checked "$remux")"
    mismatched="$(field mismatched "$remux")"
    matched="$(field matched "$remux")"
    bursts="$(field bursts "$remux")"
    min_burst="$(field min_burst "$remux")"
    max_burst="$(field max_burst "$remux")"
    rollovers="$(field frame_rollovers "$remux")"
    continuity="$(field continuity_errors "$remux")"
    pes="$(field pes_errors "$remux")"
    status="loss"

    if [ "$rc" -eq 0 ] && [ "$mismatched" -eq 0 ] \
       && [ "$checked" -eq "$frames" ] && [ "$matched" -eq "$frames" ] \
       && [ "$continuity" -eq 0 ] && [ "$pes" -eq 0 ]; then
        status="lossless"
        safe_count=$((safe_count + 1))
        [ -n "$first_safe" ] || first_safe="$capacity"

        [ "$max_burst" -le "$capacity" ] || {
            echo "burst exceeded capacity at $capacity: $max_burst" >&2
            exit 1
        }
    fi

    printf '%-10s %-7s %-8s %-8s %-8s %-8s %-8s %s\n' \
           "$capacity" "$status" "$frames" "$checked" "$bursts" \
           "$min_burst" "$max_burst" "$rollovers"
done

[ "$rows" -ge 2 ] || { echo "sweep needs at least two capacities" >&2; exit 1; }
[ "$safe_count" -gt 0 ] || { echo "no lossless burst capacity found" >&2; exit 1; }

echo
echo "== burst sizing conditions"
echo "   fixture:       $FIXTURE"
echo "   capacities:    $CAPACITIES"
echo "   first safe:    $first_safe bytes"
echo "   lossless rows: $safe_count/$rows"
echo "== burst sizing sweep done"

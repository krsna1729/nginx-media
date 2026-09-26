#!/usr/bin/env bash
#
# Capacity benchmarks at CI tiers.  Each tier is a fixed, versioned recipe so
# a number from one night is comparable with the next:
#
#   pr       minutes.  Pure SRT and pure RTMP at small fanout.  Gates only on
#            setup failures and on quality failures at rungs every supported
#            host must carry; capacity is recorded, not gated.
#   branch   the pr recipe plus RTMP 95%/SRT 5%, after a merge to main.
#   nightly  all seven workloads in four parallel configurations, 30 s
#            rungs up to the host's boundary.
#   weekly   the nightly ladders at 120 s, the fixed-vs-adaptive SRT sender
#            comparison, HLS preparation on/off, and the transport library
#            comparison (libsrt vs robotweax/srt) when ROBOTWEAX_NGINX names a
#            binary built against it.
#
# Nothing here is sized for a particular host: the ladders run until the
# rungs fail and the results record where that was, together with the host's
# CPU count, so a faster runner simply produces a higher boundary.  The number
# that is comparable across hosts is CPU per delivered Gbit/s, which every
# rung's diagnostics.json carries.
#
#   scripts/bench-ci.sh pr|branch|nightly|weekly [output dir]

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TIER="${1:-pr}"
OUT="${2:-$ROOT/.build/bench-results/$TIER}"
RUN="$ROOT/.build/ingest-egress-fanout"

rm -rf "$OUT"
mkdir -p "$OUT"
# absolute: the bundles are copied from inside the harness's run directory
OUT="$(cd "$OUT" && pwd)"

# BENCH_CONFIGS restricts a tier to some of its configurations, so CI can
# run a tier's configurations as parallel jobs and merge their results.
BENCH_CONFIGS="${BENCH_CONFIGS:-}"

ALL_MIXES="pure-srt pure-rtmp pure-hls pure-hls-push rtmp-95-srt-5 hls-push-95-srt-5 rtmp-50-hls-push-45-srt-5"

# config name, then environment assignments for the harness
run_config() {
    local name="$1" status arg mixes="" steps=""
    shift

    if [ -n "$BENCH_CONFIGS" ] && [[ " $BENCH_CONFIGS " != *" $name "* ]]; then
        return 0
    fi
    for arg in "$@"; do
        case "$arg" in
            CAPACITY_QUALITY_MIXES=*) mixes="${arg#*=}" ;;
            CAPACITY_QUALITY_STEPS=*) steps="${arg#*=}" ;;
        esac
    done
    [ "$mixes" = all ] && mixes="$ALL_MIXES"
    mkdir -p "$OUT/$name"
    # What this configuration must account for, written before it runs: the
    # gate compares it with what the run produced, so a harness that dies
    # half way is an incomplete run, not a short green one.
    python3 - "$OUT/$name/expected.json" "$mixes" "$steps" <<'PYEOF'
import json, sys
path, mixes, steps = sys.argv[1:]
json.dump({"mixes": mixes.split(), "steps": [int(s) for s in steps.split()]},
          open(path, "w"))
PYEOF
    echo "== bench-ci $TIER/$name"
    env "$@" PHASES=capacity-quality-ladder \
        CAPACITY_QUALITY_STOP_AFTER_FAILURES="${BENCH_STOP_AFTER_FAILURES:-2}" \
        "$ROOT/tests/bench/ingest_egress_fanout.sh" > "$OUT/$name.log" 2>&1
    status=$?
    [ -f "$RUN/capacity-matrix.json" ] \
        && cp "$RUN/capacity-matrix.json" "$OUT/$name/"
    if [ -d "$RUN/capacity" ]; then
        # the bundles and the small per-rung reports, not the media
        (cd "$RUN" && find capacity -maxdepth 2 -type f \
            \( -name 'diagnostics.json' -o -name '*quality*.txt' \
               -o -name '*.csv' -o -name 'srt-shards.txt' \) \
            -print0 | xargs -0 -r cp --parents -t "$OUT/$name/")
    fi
    grep -E '^(quality_rung_result|matrix |mix=)' "$OUT/$name.log" \
        | sed "s/^/   /"
    echo "$status" > "$OUT/$name.status"
    return 0
}

# The pr rungs are a subset of the branch rungs, so a pull request's CPU per
# Gbit/s can be compared with the history main has recorded.
case "$TIER" in
    pr)
        run_config srt CAPACITY_QUALITY_MIXES=pure-srt \
            CAPACITY_QUALITY_STEPS="1 16 32" CAPACITY_QUALITY_SECONDS=10
        run_config rtmp CAPACITY_QUALITY_MIXES=pure-rtmp \
            CAPACITY_QUALITY_STEPS="1 16 32" CAPACITY_QUALITY_SECONDS=10
        ;;
    branch)
        for mix in srt:pure-srt rtmp:pure-rtmp rtmp-srt:rtmp-95-srt-5; do
            run_config "${mix%%:*}" CAPACITY_QUALITY_MIXES="${mix#*:}" \
                CAPACITY_QUALITY_STEPS="1 16 32 64 128" \
                CAPACITY_QUALITY_SECONDS=15
        done
        ;;
    nightly)
        # Four configurations, run as parallel jobs (nightly.yml passes them
        # as the matrix), so the tier's wall time is the slowest group rather
        # than the sum of all seven workloads.  The publish job collects every
        # job's results and writes one record with these four config names.
        steps="${BENCH_STEPS:-1 16 64 128 256 512}"
        run_config srt-rtmp CAPACITY_QUALITY_MIXES="pure-srt pure-rtmp" \
            CAPACITY_QUALITY_STEPS="$steps" CAPACITY_QUALITY_SECONDS=30
        run_config hls CAPACITY_QUALITY_MIXES="pure-hls pure-hls-push" \
            CAPACITY_QUALITY_STEPS="$steps" CAPACITY_QUALITY_SECONDS=30
        run_config mix-srt-share \
            CAPACITY_QUALITY_MIXES="rtmp-95-srt-5 hls-push-95-srt-5" \
            CAPACITY_QUALITY_STEPS="$steps" CAPACITY_QUALITY_SECONDS=30
        run_config mix-all-protocols \
            CAPACITY_QUALITY_MIXES="rtmp-50-hls-push-45-srt-5" \
            CAPACITY_QUALITY_STEPS="$steps" CAPACITY_QUALITY_SECONDS=30
        ;;
    weekly)
        steps="${BENCH_STEPS:-1 32 64 128 192 256 384 512 768 1000}"
        run_config all CAPACITY_QUALITY_MIXES=all \
            CAPACITY_QUALITY_STEPS="$steps" CAPACITY_QUALITY_SECONDS=120
        for senders in 1 2 4 adaptive; do
            run_config "srt-senders-$senders" CAPACITY_QUALITY_MIXES=pure-srt \
                CAPACITY_QUALITY_STEPS="$steps" CAPACITY_QUALITY_SECONDS=30 \
                CAPACITY_FIXED_SRT_WORKERS="$senders"
        done
        run_config srt-no-hls CAPACITY_QUALITY_MIXES=pure-srt \
            CAPACITY_QUALITY_STEPS="$steps" CAPACITY_QUALITY_SECONDS=30 \
            CAPACITY_SRT_HLS=no
        if [ -n "${ROBOTWEAX_NGINX:-}" ] && [ -x "$ROBOTWEAX_NGINX" ]; then
            run_config srt-robotweax CAPACITY_QUALITY_MIXES=pure-srt \
                CAPACITY_QUALITY_STEPS="$steps" CAPACITY_QUALITY_SECONDS=30 \
                CAPACITY_NGINX="$ROBOTWEAX_NGINX"
        fi
        ;;
    *)
        echo "unknown tier: $TIER (pr, branch, nightly, weekly)" >&2
        exit 2
        ;;
esac

python3 "$ROOT/tests/bench/bench_history.py" summarize "$OUT" \
    --tier "$TIER" --out "$OUT/summary.json" || exit 1

# A setup failure is always a failure: the benchmark could not measure.  So
# is an incomplete run - a configuration whose harness did not finish, or a
# rung it expected that was neither measured nor deliberately skipped after
# the capacity boundary.  Quality failures are gated only where the tier says
# every host must pass.
python3 "$ROOT/tests/bench/bench_history.py" gate "$OUT/summary.json" \
    --tier "$TIER" ${BENCH_HISTORY:+--history "$BENCH_HISTORY"}

#!/usr/bin/env bash
set -uo pipefail

STEPS="${SWEEP_STEPS:-1 16 32 64 128 256 512 1024}"
RUNG_SECS="${SWEEP_SECONDS:-15}"
BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

runs=(
    "haivision_p_nonisolated:--cores p:$BASE_DIR/.build/nginx-haivision/install/sbin/nginx"
    "haivision_p_isolated:--cores p --isolate:$BASE_DIR/.build/nginx-haivision/install/sbin/nginx"
    "haivision_e_cores:--cores e:$BASE_DIR/.build/nginx-haivision/install/sbin/nginx"
    "robotweax_p_nonisolated:--cores p:$BASE_DIR/.build/nginx-robotweax/install/sbin/nginx"
    "robotweax_p_isolated:--cores p --isolate:$BASE_DIR/.build/nginx-robotweax/install/sbin/nginx"
    "robotweax_e_cores:--cores e:$BASE_DIR/.build/nginx-robotweax/install/sbin/nginx"
)

mkdir -p "$BASE_DIR/.build/bench-matrix-6"

for run in "${runs[@]}"; do
    IFS=: read -r name core_args nginx_bin <<< "$run"
    out="$BASE_DIR/.build/bench-matrix-6/$name"
    echo ""
    echo "=========================================================="
    echo "STARTING: $name ($core_args) with $nginx_bin"
    echo "=========================================================="
    rm -rf "$out"
    mkdir -p "$out"

    # shellcheck disable=SC2086
    omarchy-benchmark $core_args -- bash -c "
        CAPACITY_PREFLIGHT=no \
        CAPACITY_NGINX='$nginx_bin' \
        CAPACITY_QUALITY_MIXES=pure-srt \
        CAPACITY_QUALITY_STEPS='$STEPS' \
        CAPACITY_QUALITY_SECONDS=$RUNG_SECS \
        CAPACITY_QUALITY_STOP_AFTER_FAILURES=1 \
        CAPACITY_RUN='$out' \
        PHASES=capacity-quality-ladder \
        '$BASE_DIR/tests/bench/ingest_egress_fanout.sh'
    " || echo "==> $name stopped at capacity boundary."

    if [ -f "$out/capacity-matrix.json" ]; then
        echo "==> Result for $name:"
        cat "$out/capacity-matrix.json"
        echo ""
    fi
done

echo "All 6 combinations complete."

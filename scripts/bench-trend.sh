#!/usr/bin/env bash
# Benchmark a bounded commit sample sequentially on this job's runner.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TIER="${1:?usage: bench-trend.sh branch|nightly|weekly}"
OUT="$ROOT/.build/bench-trend"
RUN_ID="${GITHUB_RUN_ID:-local}-attempt-${GITHUB_RUN_ATTEMPT:-1}"
HEAD_SHA="${GITHUB_SHA:-$(git -C "$ROOT" rev-parse HEAD)}"
TREND_STEPS="${BENCH_TREND_STEPS:-1 32 128}"
TREND_SECONDS="${BENCH_TREND_SECONDS:-10}"

case "$TIER" in
    branch)
        configs='["srt","rtmp","rtmp-srt"]'
        config_names="srt rtmp rtmp-srt" ;;
    nightly)
        configs='["srt-rtmp","hls","mix-srt-share","mix-all-protocols"]'
        config_names="srt-rtmp hls mix-srt-share mix-all-protocols" ;;
    weekly)
        configs='["all"]'
        config_names="all" ;;
    *) echo "unsupported trend tier: $TIER" >&2; exit 2 ;;
esac

if [ ! -d "$ROOT/.build/nginx-1.30.5" ] || [ ! -d "$ROOT/.build/srt-haivision" ]; then
    echo "bench-trend prerequisites missing in $ROOT/.build (nginx-1.30.5 and srt-haivision required; run make nginx)" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"
python3 "$ROOT/tests/bench/bench_commit_selection.py" "$TIER" \
    --head "$HEAD_SHA" > "$OUT/commits.tsv"

git -C "$ROOT" worktree prune
cleanup() {
    local tree
    for tree in "$OUT"/worktree-*; do
        [ -e "$tree" ] || continue
        git -C "$ROOT" worktree remove --force "$tree" >/dev/null 2>&1 || true
    done
    git -C "$ROOT" worktree prune >/dev/null 2>&1 || true
}
trap cleanup EXIT

stub_summary() {
    local sha="$1" commit_time="$2" out="$3"
    python3 - "$sha" "$commit_time" "$out" "$TIER" "$configs" "$RUN_ID" <<'PY'
import datetime, json, os, sys
sha, commit_time, path, tier, configs, group = sys.argv[1:]
names = json.loads(configs)
record = {
    "schema": "nginx-media.bench-history/3", "tier": tier,
    "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    "commit_timestamp": commit_time, "sha": sha, "ref": "main",
    "run": os.environ.get("GITHUB_RUN_ID"), "comparison_group": group,
    "expected_configs": names, "missing_configs": names,
    "unexpected_configs": [], "complete": False,
    "configs": {name: {"harness_status": None, "mixes": {}, "complete": False,
                        "finished": False, "missing": ["historical revision did not build"],
                        "stopped": {}} for name in names},
}
with open(path, "w", encoding="utf-8") as out:
    json.dump(record, out, separators=(",", ":"))
PY
}

append_sample() {
    python3 - "$1" "$OUT/samples.jsonl" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    record = json.load(source)
with open(sys.argv[2], "a", encoding="utf-8") as output:
    json.dump(record, output, separators=(",", ":"))
    output.write("\n")
PY
}

: > "$OUT/samples.jsonl"
while IFS=$'\t' read -r -u 3 sha commit_time; do
    [ -n "$sha" ] || continue
    tree="$OUT/worktree-$sha"
    result="$OUT/$sha"
    mkdir -p "$result"
    echo "== same-runner trend $TIER $sha ($commit_time)"
    if ! git -C "$ROOT" worktree add --quiet --detach "$tree" "$sha"; then
        echo "could not create worktree for $sha" >&2
        stub_summary "$sha" "$commit_time" "$result/summary.json"
        append_sample "$result/summary.json"
        continue
    fi

    build="$tree/.build/nginx-haivision"
    mkdir -p "$tree/.build" "$build"
    cp -a "$ROOT/.build/nginx-1.30.5" "$build/"
    rm -rf "$build/nginx-1.30.5/objs"
    if ! BUILD_DIR="$build" NGINX_PREFIX="$build/install" \
         SRT_DIR="$ROOT/.build/srt-haivision" MEDIA_SRT_PKG=srt-pinned \
         NGINX_LD_OPT="-Wl,-rpath,$ROOT/.build/srt-haivision/lib" \
         "$tree/scripts/build-nginx.sh" > "$OUT/$sha-build.log" 2>&1; then
        echo "historical build failed for $sha; recording an incomplete sample"
        stub_summary "$sha" "$commit_time" "$result/summary.json"
    else
        BENCH_TREND=1 BENCH_CONFIGS="$config_names" \
        BENCH_STEPS="$TREND_STEPS" BENCH_SECONDS="$TREND_SECONDS" \
        BENCH_COMMIT_TIMESTAMP="$commit_time" BENCH_COMPARISON_GROUP="$RUN_ID" \
        GITHUB_SHA="$sha" GITHUB_REF_NAME=main \
        CAPACITY_NGINX="$build/install/sbin/nginx" \
            "$ROOT/scripts/bench-ci.sh" "$TIER" "$result" \
            > "$OUT/$sha-driver.log" 2>&1 || true
        if [ ! -s "$result/summary.json" ]; then
            stub_summary "$sha" "$commit_time" "$result/summary.json"
        fi
    fi
    append_sample "$result/summary.json"
    if [ "$sha" = "$HEAD_SHA" ]; then
        cp "$result/summary.json" "$OUT/current.json"
    fi
    git -C "$ROOT" worktree remove --force "$tree" >/dev/null 2>&1 || true
done 3< "$OUT/commits.tsv"

if [ ! -s "$OUT/current.json" ]; then
    echo "trend sample did not contain workflow HEAD $HEAD_SHA" >&2
    exit 1
fi
python3 - "$OUT/samples.jsonl" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    rows = [json.loads(line) for line in source if line.strip()]
print(f"same_runner_samples={len(rows)} commits=" + ",".join(r["sha"][:8] for r in rows))
PY

#!/usr/bin/env python3
"""Run a fair, apples-to-apples three-way capacity comparison between:
1. nginx-media (robotweax-srt v0.2.9)
2. nginx-media (haivision-libsrt v1.5.7)
3. restream (v1.5.7)

Using our unified benchmark harness (tests/bench/ingest_egress_fanout.sh),
identical CPU pinning, identical ingress publisher (8M H.264),
identical receiver sinks (srt_fanout_sink and RTMP sink in auto mode),
and identical quality gates (delivery ratio >= 0.95, interval floor >= 0.80).
"""

import os
import sys
import json
import time
import subprocess
from pathlib import Path

PROJECT_ROOT = Path("/home/krsna1729/Projects/nginx-media")
OUTPUT_DIR = PROJECT_ROOT / "docs/capacity-evidence/three-way-comparison"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

ENGINES = [
    {
        "id": "nginx-robotweax",
        "name": "nginx-media (robotweax-srt v0.2.9)",
        "engine_env": "nginx",
        "binary": str(PROJECT_ROOT / ".build/nginx-robotweax/install/sbin/nginx"),
    },
    {
        "id": "nginx-haivision",
        "name": "nginx-media (haivision-libsrt v1.5.7)",
        "engine_env": "nginx",
        "binary": str(PROJECT_ROOT / ".build/nginx-haivision/install/sbin/nginx"),
    },
    {
        "id": "restream",
        "name": "restream (v1.5.7)",
        "engine_env": "restream",
        "binary": "/home/krsna1729/Projects/restream/target/release/restream",
    },
]

# Workloads: Pure SRT, Pure RTMP, and Mixed RTMP/SRT (95/5)
WORKLOADS = ["pure-srt", "pure-rtmp", "rtmp-95-srt-5"]
STEPS = "1 16 64 128 256 512 1024"

def cleanup():
    subprocess.run(["killall", "-9", "restream", "nginx", "srt_fanout_sink", "ffmpeg"],
                   stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    subprocess.run(["omarchy-benchmark", "--reset", "--force"],
                   stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    time.sleep(1.5)

def run_engine_ladder(engine, mix):
    print(f"\n=================================================================")
    print(f"Running Engine: {engine['name']} | Workload: {mix}")
    print(f"=================================================================\n", flush=True)
    cleanup()

    run_dir = OUTPUT_DIR / f"{engine['id']}_{mix}"
    run_dir.mkdir(parents=True, exist_ok=True)

    cmd = (
        f"omarchy-benchmark --cores all --no-offline-siblings -- bash -c '"
        f"export CAPACITY_PREFLIGHT=no; "
        f"export CAPACITY_ENGINE={engine['engine_env']}; "
        f"export CAPACITY_NGINX=\"{engine['binary']}\"; "
        f"export RESTREAM_BIN=\"{engine['binary']}\"; "
        f"export CAPACITY_NGINX_CPUS=\"0,2,4,6\"; "
        f"export CAPACITY_RECEIVER_CPUS=\"12,13,14,15,16,17,18,19\"; "
        f"export CAPACITY_PUBLISHER_CPUS=\"12,13\"; "
        f"export CAPACITY_QUALITY_SECONDS=10; "
        f"export CAPACITY_QUALITY_STEPS=\"{STEPS}\"; "
        f"export CAPACITY_QUALITY_STOP_AFTER_FAILURES=1; "
        f"export CAPACITY_SINK_MODE=auto; "
        f"export PHASES=\"capacity-quality-ladder\"; "
        f"export CAPACITY_MIXES=\"{mix}\"; "
        f"export CAPACITY_RUN=\"{run_dir}\"; "
        f"tests/bench/ingest_egress_fanout.sh"
        f"'"
    )

    t0 = time.time()
    proc = subprocess.run(cmd, shell=True, cwd=str(PROJECT_ROOT),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    elapsed = time.time() - t0

    # Write log
    with open(run_dir / "bench.log", "w") as f:
        f.write(proc.stdout)

    # Parse matrix
    matrix_file = run_dir / "capacity-matrix.json"
    result = {"engine": engine["id"], "mix": mix, "wall_time_s": elapsed}
    if matrix_file.is_file():
        try:
            with open(matrix_file) as f:
                data = json.load(f)
            mix_data = data.get("mixes", {}).get(mix, {})
            result.update({
                "highest_passing": mix_data.get("highest_passing"),
                "highest_passing_correctness": mix_data.get("highest_passing_correctness"),
                "highest_passing_fast": mix_data.get("highest_passing_fast"),
                "first_quality_failure": mix_data.get("first_quality_failure"),
                "rungs": mix_data.get("rungs", []),
            })
        except Exception as e:
            result["parse_error"] = str(e)
    else:
        result["status"] = "NO_MATRIX"

    print(f"Finished {engine['name']} {mix}: Highest Passing = {result.get('highest_passing')}, First Failure = {result.get('first_quality_failure')}")
    return result

def main():
    all_results = []
    for mix in WORKLOADS:
        for engine in ENGINES:
            res = run_engine_ladder(engine, mix)
            all_results.append(res)
            # Save partial progress
            with open(OUTPUT_DIR / "summary.json", "w") as f:
                json.dump(all_results, f, indent=2)

    print("\n\nAll benchmarks completed. Summary written to:", OUTPUT_DIR / "summary.json")

if __name__ == "__main__":
    main()

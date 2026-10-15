#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import time

PROJECT_ROOT = "/home/krsna1729/Projects/nginx-media"
OUTPUT_DIR = os.path.join(PROJECT_ROOT, "docs/capacity-evidence/cpu-sweep")
os.makedirs(OUTPUT_DIR, exist_ok=True)

CPU_CONFIGS = [
    (1, "0"),
    (2, "0,2"),
    (3, "0,2,4"),
    (4, "0,2,4,6"),
    (5, "0,2,4,6,8"),
    (6, "0,2,4,6,8,10")
]
STEPS = "1 16 32 64 128 256 512 1024"
RECEIVER_CPUS = "12,13,14,15,16,17,18,19"
PUBLISHER_CPUS = "12,13"
NGINX_BIN = os.path.join(PROJECT_ROOT, ".build/nginx-robotweax/install/sbin/nginx")
def cleanup():
    subprocess.run(["killall", "-9", "nginx", "srt_fanout_sink", "ffmpeg"],
                   stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "tests/bench"],
                   stderr=subprocess.DEVNULL)
    subprocess.run(["omarchy-benchmark", "--reset", "--force"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)

def run_cpu_test(n_cpus, cpu_list, mix="pure-srt"):
    print(f"\n=======================================================", flush=True)
    print(f"Running CPU sweep: {n_cpus} P-cores (affinity: {cpu_list}) for {mix}", flush=True)
    print(f"=======================================================", flush=True)

    run_dir = os.path.join(PROJECT_ROOT, f".build/bench-cpu-sweep/cpu_{n_cpus}_{mix}")
    os.makedirs(run_dir, exist_ok=True)
    summary_file = os.path.join(run_dir, "capacity-matrix.json")
    if os.path.exists(summary_file):
        print(f"Found existing summary file in {summary_file}, loading directly...", flush=True)
        with open(summary_file) as f:
            data = json.load(f)
            mix_data = data.get("mixes", {}).get(mix, {})
            parsed_result = {
                "cpus": n_cpus,
                "affinity": cpu_list,
                "mix": mix,
                "highest_passing": mix_data.get("highest_passing"),
                "first_quality_failure": mix_data.get("first_quality_failure"),
                "setup_limited": mix_data.get("setup_limited", []),
                "rungs": mix_data.get("rungs", [])
            }
            print(f"Result for {n_cpus} CPUs: Highest Passing = {parsed_result.get('highest_passing')}, First Failure = {parsed_result.get('first_quality_failure')}", flush=True)
            return parsed_result

    cleanup()
    cmd = [
        "omarchy-benchmark", "--cores", "all", "--no-offline-siblings", "--",
        "bash", "-c",
        f"""
        CAPACITY_PREFLIGHT=no \\
        CAPACITY_NGINX={NGINX_BIN} \\
        CAPACITY_NGINX_CPUS="{cpu_list}" \\
        CAPACITY_RECEIVER_CPUS="{RECEIVER_CPUS}" \\
        CAPACITY_PUBLISHER_CPUS="{PUBLISHER_CPUS}" \\
        PHASES=capacity-quality-ladder \\
        CAPACITY_QUALITY_MIXES="{mix}" \\
        CAPACITY_QUALITY_STEPS="{STEPS}" \\
        CAPACITY_QUALITY_SECONDS=10 \\
        CAPACITY_QUALITY_STOP_AFTER_FAILURES=1 \\
        CAPACITY_RUN="{run_dir}" \\
        tests/bench/ingest_egress_fanout.sh
        """
    ]

    t0 = time.time()
    res = subprocess.run(cmd, cwd=PROJECT_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    duration = time.time() - t0

    log_path = os.path.join(OUTPUT_DIR, f"cpu_{n_cpus}_{mix}.log")
    with open(log_path, "w") as f:
        f.write(res.stdout)

    summary_file = os.path.join(run_dir, "capacity-matrix.json")
    parsed_result = {"cpus": n_cpus, "affinity": cpu_list, "mix": mix, "wall_s": duration}

    if os.path.exists(summary_file):
        with open(summary_file) as f:
            data = json.load(f)
            parsed_result["matrix"] = data
            mix_data = data.get("mixes", {}).get(mix, {})
            parsed_result["highest_passing"] = mix_data.get("highest_passing")
            parsed_result["first_quality_failure"] = mix_data.get("first_quality_failure")
            parsed_result["setup_limited"] = mix_data.get("setup_limited", [])
            parsed_result["rungs"] = mix_data.get("rungs", [])
    else:
        # Parse from stdout lines
        print(f"No capacity-matrix.json found in {run_dir}, inspecting stdout...", flush=True)
        # Look for quality_rung_result lines
        highest = None
        first_fail = None
        for line in res.stdout.splitlines():
            if "quality_rung_result" in line and f"mix={mix}" in line:
                # e.g. quality_rung_result mix=pure-srt destinations=16 ... result=pass
                parts = dict(p.split("=", 1) for p in line.split() if "=" in p)
                d = int(parts.get("destinations", 0))
                r = parts.get("result")
                if r == "pass":
                    highest = d
                elif r == "quality-failure" and first_fail is None:
                    first_fail = d
            if "mix=" + mix in line and "setup_failure_rung=" in line:
                parsed_result["setup_limited"] = line.split("setup_failure_rung=")[1].split()[0]
        parsed_result["highest_passing"] = highest
        parsed_result["first_quality_failure"] = first_fail

    print(f"Result for {n_cpus} CPUs: Highest Passing = {parsed_result.get('highest_passing')}, First Failure = {parsed_result.get('first_quality_failure')}", flush=True)
    return parsed_result

def main():
    results = []
    # Sweep CPUs 1 through 6 for pure-srt
    for n_cpus, cpu_list in CPU_CONFIGS:
        r = run_cpu_test(n_cpus, cpu_list, mix="pure-srt")
        results.append(r)
        with open(os.path.join(OUTPUT_DIR, "pure_srt_cpu_scaling.json"), "w") as f:
            json.dump(results, f, indent=2)

    print("\n\nAll CPU sweep runs completed. Final Summary:")
    print(json.dumps(results, indent=2))

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Smoke test across 3 engines x 7 protocol mixes over multi-queue veth topology."""

import os
import sys
import json
import time
import subprocess
from pathlib import Path

PROJECT_ROOT = Path("/home/krsna1729/Projects/nginx-media")
OUTPUT_DIR = PROJECT_ROOT / "docs/capacity-evidence/smoke-test"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

ENGINES = [
    {
        "id": "nginx-robotweax",
        "name": "nginx-media (robotweax-srt)",
        "engine_env": "nginx",
        "binary": str(PROJECT_ROOT / ".build/nginx-robotweax/install/sbin/nginx"),
    },
    {
        "id": "nginx-haivision",
        "name": "nginx-media (haivision-libsrt)",
        "engine_env": "nginx",
        "binary": str(PROJECT_ROOT / ".build/nginx-haivision/install/sbin/nginx"),
    },
    {
        "id": "restream",
        "name": "restream (srt-rs)",
        "engine_env": "restream",
        "binary": "/home/krsna1729/Projects/restream/target/release/restream",
    },
]

MIXES = [
    "pure-srt",
    "pure-rtmp",
    "pure-hls",
    "pure-hls-push",
    "rtmp-95-srt-5",
    "hls-push-95-srt-5",
    "rtmp-50-hls-push-45-srt-5",
]

def cleanup():
    """Stop only what this harness starts.

    A bare `killall nginx` or `killall python3` reaches every process the user
    owns on the host: language servers, other benchmark runs, anything else
    with the same name.  The media participants are launched under the run
    directory, so scoping the match to that path stops this harness's trees
    and nothing else.
    """
    for name in ("restream", "nginx", "srt_fanout_sink", "ffmpeg"):
        subprocess.run(["pkill", "-9", "-f", f"^{PROJECT_ROOT}.*{name}"],
                       stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        subprocess.run(["pkill", "-9", "-f", f"CAPACITY_RUN=.*{OUTPUT_DIR.name}.*{name}"],
                       stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    # The harness also starts participants whose paths live under the run
    # directory, which is the one predicate that covers every one of them.
    subprocess.run(["pkill", "-9", "-f", f"/{OUTPUT_DIR.name}/"],
                   stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    subprocess.run(["omarchy-benchmark", "--reset", "--force"],
                   stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    time.sleep(1.0)

def ensure_veth():
    cmd = ["tests/bench/capacity_veth.sh", "up"]
    out = subprocess.check_output(cmd, cwd=str(PROJECT_ROOT), text=True)
    env_vars = {}
    for line in out.strip().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env_vars[k.strip()] = v.strip().strip("'\"")
    return env_vars

def topology_environments():
    """Both topologies the harness must work in.

    Namespace mode is the one the capacity runs use; loopback is the one every
    developer and the CI container use.  Testing only one of them is how a
    routing change lands with half the matrix untested, which has happened
    here.
    """
    veth = ensure_veth()
    namespace = " ".join(
        f"export {key}=\"{value}\";"
        for key, value in (
            ("CAPACITY_SENDER_EXEC", veth["CAPACITY_SENDER_EXEC"]),
            ("CAPACITY_RECEIVER_EXEC", veth["CAPACITY_RECEIVER_EXEC"]),
            ("CAPACITY_RECEIVER_ADDR", veth["CAPACITY_RECEIVER_ADDR"]),
            ("CAPACITY_SENDER_ADDR", veth["CAPACITY_SENDER_ADDR"]),
        ))
    return [
        {"name": "loopback", "env": ""},
        {"name": "namespace", "env": namespace},
    ]


def main():
    print("Initializing multi-queue veth topology...", flush=True)
    topologies = topology_environments()
    print("Topologies:", [t["name"] for t in topologies], flush=True)

    results = []

    for topology in topologies:
        print(f"\n########## topology: {topology['name']} ##########", flush=True)
        for engine in ENGINES:
            for mix in MIXES:
                print(f"\n=======================================================", flush=True)
                print(f"SMOKE TEST: {engine['name']} | Mix: {mix}", flush=True)
                print(f"=======================================================", flush=True)
                cleanup()

                run_dir = OUTPUT_DIR / f"{topology['name']}_{engine['id']}_{mix}"
                run_dir.mkdir(parents=True, exist_ok=True)

                cmd = (
                    f"omarchy-benchmark --cores all --no-offline-siblings -- bash -c '"
                    f"export CAPACITY_PREFLIGHT=no; "
                    f"export CAPACITY_ENGINE={engine['engine_env']}; "
                    f"export CAPACITY_NGINX=\"{engine['binary']}\"; "
                    f"export RESTREAM_BIN=\"{engine['binary']}\"; "
                    f"export CAPACITY_RTMP_SINK_NGINX=\"{PROJECT_ROOT}/.build/nginx-haivision/install/sbin/nginx\"; "
                    f"{topology['env']} "
                    f"export CAPACITY_QUALITY_SECONDS=10; "
                    f"export CAPACITY_NGINX_CPUS=\"0,2\"; "
                    f"export CAPACITY_RECEIVER_CPUS=\"12,13,14,15,16,17,18,19\"; "
                    f"export CAPACITY_PUBLISHER_CPUS=\"12,13\"; "
                    f"export CAPACITY_QUALITY_STEPS=\"1\"; "
                    f"export CAPACITY_QUALITY_STOP_AFTER_FAILURES=1; "
                    f"export CAPACITY_SINK_MODE=correctness; "
                    f"export PHASES=\"capacity-quality-ladder\"; "
                    f"export CAPACITY_QUALITY_MIXES=\"{mix}\"; "
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

                # Check outcome
                diag_path = run_dir / f"capacity/quality-{mix}-1/diagnostics.json"
                outcome = "unknown"
                if diag_path.is_file():
                    try:
                        with open(diag_path) as f:
                            diag = json.load(f)
                        outcome = diag.get("outcome", "unknown")
                    except Exception as e:
                        outcome = f"error: {e}"

                status_str = "PASS" if outcome == "pass" else f"FAIL ({outcome})"
                print(f"RESULT: {engine['id']} | {mix} -> {status_str} ({elapsed:.1f}s)", flush=True)
                results.append({
                    "topology": topology["name"],
                    "engine": engine["id"],
                    "mix": mix,
                    "outcome": outcome,
                    "elapsed_s": elapsed
                })

    print("\n\n==================== SMOKE TEST SUMMARY ====================")
    for r in results:
        print(f"{r['topology']:10} {r['engine']:20} {r['mix']:28} -> "
              f"{r['outcome']:15} ({r['elapsed_s']:.1f}s)")

    with open(OUTPUT_DIR / "smoke_summary.json", "w") as f:
        json.dump(results, f, indent=2)

if __name__ == "__main__":
    main()

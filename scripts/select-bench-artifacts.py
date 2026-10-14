#!/usr/bin/env python3
"""Select the newest visible job artifacts from one immutable GitHub run.

A partial re-run leaves successful jobs in earlier attempts. Retain each
artifact's source attempt instead of relabelling it as the publisher attempt.
"""
import argparse
import json
from pathlib import Path
import re


def select(pages, tier, attempt, configurations, current_jobs):
    if not isinstance(pages, list):
        raise ValueError("artifact pages must be an array")
    pattern = re.compile(r"^bench-" + re.escape(tier) + r"-attempt-([1-9][0-9]*)-([0-9]+)$")
    trend_pattern = re.compile(r"^bench-trend-" + re.escape(tier) + r"-attempt-([1-9][0-9]*)$")
    jobs, trend = {}, None
    required = set()
    require_trend = False
    for page in current_jobs:
        for job in page["jobs"]:
            name = job["name"]
            for index, configuration in enumerate(configurations):
                target = "Benchmark " + configuration
                if name == target or name.endswith(" / " + target):
                    required.add(index)
            if name == "Trend history" or name.endswith(" / Trend history"):
                require_trend = True
    for page in pages:
        for artifact in page["artifacts"]:
            if artifact["expired"]:
                continue
            matched = pattern.fullmatch(artifact["name"])
            trend_match = trend_pattern.fullmatch(artifact["name"])
            if not matched and not trend_match:
                continue
            source_attempt = int((matched or trend_match)[1])
            if source_attempt > attempt:
                continue
            identifier = artifact["id"]
            if not isinstance(identifier, int) or isinstance(identifier, bool) or identifier <= 0:
                raise ValueError("invalid artifact id")
            record = {"artifact_id": identifier, "name": artifact["name"],
                      "source_attempt": source_attempt,
                      "created_at": artifact["created_at"]}
            if matched:
                index = int(matched[2])
                if index >= len(configurations):
                    raise ValueError("benchmark artifact has an unknown job index")
                record.update(job_index=index, configurations=configurations[index].split())
                old = jobs.get(index)
                if old is None or (source_attempt, identifier) > (old["source_attempt"], old["artifact_id"]):
                    jobs[index] = record
            elif trend is None or (source_attempt, identifier) > (trend["source_attempt"], trend["artifact_id"]):
                trend = record
    # A currently re-run job must not be replaced by an old visible artifact
    # while the new upload is pending.
    for index in required:
        if index in jobs and jobs[index]["source_attempt"] != attempt:
            del jobs[index]
    if require_trend and trend and trend["source_attempt"] != attempt:
        trend = None
    missing = sorted(set(range(len(configurations))) - set(jobs))
    return {"publisher_attempt": attempt, "jobs": [jobs[index] for index in sorted(jobs)],
            "trend": trend, "missing_job_indexes": missing, "configurations_complete": not missing,
            "complete": not missing and (not require_trend or trend is not None),
            "current_job_indexes": sorted(required), "current_trend_job": require_trend,
            "collection_status": "observed"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pages")
    parser.add_argument("--tier", required=True)
    parser.add_argument("--attempt", required=True, type=int)
    parser.add_argument("--configurations", required=True)
    parser.add_argument("--current-jobs", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--github-output")
    args = parser.parse_args()
    configurations = json.loads(args.configurations)
    if (args.attempt < 1 or not isinstance(configurations, list) or not configurations
            or not all(isinstance(config, str) and config.split() for config in configurations)):
        parser.error("requires a positive attempt and nonempty configuration job list")
    try:
        pages = json.loads(Path(args.pages).read_text())
        current_jobs = json.loads(Path(args.current_jobs).read_text())
        result = select(pages, args.tier, args.attempt, configurations, current_jobs)
    except (OSError, ValueError, KeyError, TypeError) as error:
        result = {"publisher_attempt": args.attempt, "jobs": [], "trend": None,
                  "missing_job_indexes": list(range(len(configurations))), "complete": False,
                  "configurations_complete": False,
                  "collection_status": "unavailable", "collection_error": str(error)}
    Path(args.out).write_text(json.dumps(result, indent=2) + "\n")
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as out:
            out.write("artifact_ids=" + ",".join(str(job["artifact_id"]) for job in result["jobs"]) + "\n")
            out.write("trend_artifact_id=" + (str(result["trend"]["artifact_id"]) if result["trend"] else "") + "\n")
    print(f"source_artifacts={len(result['jobs'])}/{len(configurations)}")
    return 0 if result["complete"] else 2


if __name__ == "__main__":
    raise SystemExit(main())

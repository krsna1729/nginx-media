#!/usr/bin/env python3
"""Immutable publication evidence, using the reporting suite's plain-Python conventions."""
import argparse
import copy
import json
import os
import shutil
import subprocess
import tempfile

from test_reporting import HISTORY, HERE, load_module, write_json


def smoke(work):
    history = load_module(HISTORY, "publication_history")
    pages = os.path.join(work, "pages")
    summary = os.path.join(work, "summary.json")
    args = argparse.Namespace(summary=summary, pages=pages, samples=None)
    first = {"tier": "weekly", "run": "42", "run_attempt": "1",
             "sha": "same-commit", "timestamp": "2026-10-01T00:00:00Z",
             "complete": False, "missing_configs": ["srt"], "configs": {}}

    def publish(record):
        write_json(summary, record)
        assert history.publish(args) == 0

    def rows():
        with open(os.path.join(args.pages, "data", "weekly.jsonl"), encoding="utf-8") as source:
            return [json.loads(line) for line in source]

    publish(first)
    retry = dict(first, timestamp="2026-10-01T00:03:00Z")
    publish(retry)
    assert rows() == [dict(first, record_id=history.publication_id(first))]
    second = dict(first, run_attempt="2", complete=True, missing_configs=[])
    publish(second)
    # A late collection cannot erase the earlier incomplete evidence of an attempt.
    late = dict(first, complete=True, missing_configs=[])
    publish(late)
    assert [(r["run_attempt"], r["complete"]) for r in rows()] == [
        ("1", False), ("2", True), ("1", True)]
    assert len({r["record_id"] for r in rows()}) == 3

    # A competing publisher replays its own input on the newest pages checkout.
    other_pages = os.path.join(work, "competing")
    shutil.copytree(pages, other_pages)
    other = dict(second, run="43")
    args.pages = other_pages
    publish(other)
    args.pages = pages
    publish(dict(second, run="44"))
    shutil.rmtree(other_pages)
    shutil.copytree(pages, other_pages)
    args.pages = other_pages
    publish(other)
    publish(other)
    assert [r["run"] for r in rows()] == ["42", "42", "42", "44", "43"]

    before = copy.deepcopy(rows())
    samples = os.path.join(work, "invalid-samples.jsonl")
    with open(samples, "w", encoding="utf-8") as output:
        output.write(json.dumps({"tier": "nightly", "comparison_group": "wrong"}) + "\n")
    args.samples = samples
    try:
        publish(dict(second, run="45"))
    except ValueError:
        pass
    else:
        raise AssertionError("invalid sample cohorts must reject publication")
    assert rows() == before

    selector = load_module(os.path.join(HERE, "../../scripts/select-bench-artifacts.py"),
                           "source_artifact_selector")
    configurations = ["srt", "rtmp"]
    def artifact(attempt, index, identifier, expired=False):
        return {"name": f"bench-branch-attempt-{attempt}-{index}", "id": identifier,
                "expired": expired, "created_at": f"2026-10-12T00:0{attempt}:00Z"}
    visible = [{"artifacts": [artifact(1, 0, 10), artifact(1, 1, 11),
                               artifact(2, 1, 20), artifact(3, 0, 30)]}]
    partial_jobs = [{"jobs": [{"name": "benchmark / Benchmark rtmp"}]}]
    selected = selector.select(visible, "branch", 2, configurations, partial_jobs)
    assert selected["complete"]
    assert [(job["job_index"], job["source_attempt"]) for job in selected["jobs"]] == [(0, 1), (1, 2)]
    pending = [{"artifacts": [artifact(1, 0, 10), artifact(1, 1, 11)]}]
    selected = selector.select(pending, "branch", 2, configurations, partial_jobs)
    assert not selected["complete"] and selected["missing_job_indexes"] == [1]
    assert [job["artifact_id"] for job in selected["jobs"]] == [10]
    all_jobs = [{"jobs": [{"name": "Benchmark srt"}, {"name": "Benchmark rtmp"}]}]
    selected = selector.select(visible, "branch", 2, configurations, all_jobs)
    assert not selected["complete"] and selected["missing_job_indexes"] == [0]
    selected = selector.select(visible, "branch", 2, configurations,
                               [{"jobs": [{"name": "publish"}]}])
    assert selected["complete"] and [job["source_attempt"] for job in selected["jobs"]] == [1, 2]
    expired = [{"artifacts": [artifact(1, 0, 10), artifact(2, 1, 20, expired=True)]}]
    assert not selector.select(expired, "branch", 2, configurations, partial_jobs)["complete"]
    trend = {"name": "bench-trend-branch-attempt-1", "id": 40, "expired": False,
             "created_at": "2026-10-12T00:01:00Z"}
    selected = selector.select([{"artifacts": visible[0]["artifacts"] + [trend]}],
                               "branch", 2, configurations,
                               [{"jobs": [{"name": "Trend history"}]}])
    assert selected["trend"] is None


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="nginx-media-publication-") as work:
        smoke(work)
    subprocess.run(["node", os.path.join(HERE, "test_history_viewer.js")], check=True)
    print("test_publication.py: immutable attempts, retries, competing replay and viewer isolation passed")

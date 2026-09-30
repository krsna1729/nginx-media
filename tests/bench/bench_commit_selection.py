#!/usr/bin/env python3
"""Choose a bounded set of mainline commits for one-runner trend probes."""

import argparse
import datetime as dt
import subprocess
import sys


def parse_history(lines):
    commits = []
    for line in lines:
        sha, separator, epoch = line.strip().partition("\t")
        if separator:
            commits.append((sha, dt.datetime.fromtimestamp(int(epoch), UTC)))
    return commits


def select(tier, commits, head, now):
    """Return unique (sha, committer-time) samples in chronological order."""
    by_sha = {sha: timestamp for sha, timestamp in commits}
    if head not in by_sha:
        raise ValueError(f"HEAD {head} is missing from fetched history")
    head_time = by_sha[head]
    picked = {}

    if tier == "branch":
        for sha, timestamp in commits[:10]:
            picked[sha] = timestamp
    elif tier == "nightly":
        for offset in range(7, 0, -1):
            day = now.date() - dt.timedelta(days=offset)
            on_day = [(sha, timestamp) for sha, timestamp in commits
                      if timestamp.date() == day]
            if on_day:
                sha, timestamp = on_day[0]
                picked[sha] = timestamp
    elif tier == "weekly":
        this_week = now.date() - dt.timedelta(days=now.weekday())
        for weeks_ago in range(4, 0, -1):
            start = this_week - dt.timedelta(days=7 * weeks_ago)
            week_commits = [(sha, timestamp) for sha, timestamp in commits
                            if start <= timestamp.date() < start + dt.timedelta(days=7)]
            for target_day in (start + dt.timedelta(days=2),
                               start + dt.timedelta(days=5)):
                if not week_commits:
                    continue
                sha, timestamp = min(week_commits,
                                     key=lambda item: (abs((item[1].date() - target_day).days),
                                                       item[1]))
                picked[sha] = timestamp
                week_commits = [(other_sha, other_time)
                                for other_sha, other_time in week_commits
                                if other_sha != sha]
    else:
        raise ValueError(f"unknown tier: {tier}")

    picked[head] = head_time
    return sorted(picked.items(), key=lambda item: (item[1], item[0]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tier", choices=("branch", "nightly", "weekly"))
    parser.add_argument("--head", required=True)
    parser.add_argument("--ref", default="HEAD")
    parser.add_argument("--now", help="UTC time for deterministic selection (ISO-8601)")
    args = parser.parse_args()
    now = (dt.datetime.fromisoformat(args.now.replace("Z", "+00:00"))
           if args.now else dt.datetime.now(UTC))
    if now.tzinfo is None:
        parser.error("--now must include a timezone")
    history = subprocess.run(
        ["git", "log", "--first-parent", "--format=%H%x09%ct", args.ref],
        check=True, capture_output=True, text=True).stdout.splitlines()
    try:
        for sha, timestamp in select(args.tier, parse_history(history), args.head,
                                     now.astimezone(UTC)):
            print(f"{sha}\t{timestamp.isoformat(timespec='seconds')}")
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    sys.exit(main())

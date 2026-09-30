#!/usr/bin/env python3
"""Small deterministic checks for tier sampling windows."""

import datetime as dt
import importlib.util
import os

path = os.path.join(os.path.dirname(__file__), "bench_commit_selection.py")
spec = importlib.util.spec_from_file_location("bench_commit_selection", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
UTC = dt.timezone.utc


def commits(start, days, per_day=1):
    result = []
    for day in range(days):
        for run in range(per_day):
            timestamp = dt.datetime(2026, 9, 1, 12 + run, tzinfo=UTC) + dt.timedelta(days=day)
            result.append((f"c{day}-{run}", timestamp))
    return list(reversed(result))


def check(value, message):
    if not value:
        raise AssertionError(message)


now = dt.datetime(2026, 9, 30, 12, tzinfo=UTC)
branch_history = commits(None, 20)
branch = module.select("branch", branch_history, branch_history[0][0], now)
check(len(branch) == 10, f"branch samples ten commits, got {len(branch)}")
check(branch[-1][0] == branch_history[0][0], "branch includes current HEAD")

nightly_history = commits(None, 35, per_day=2)
nightly_head = nightly_history[0][0]
nightly = module.select("nightly", nightly_history, nightly_head, now)
check(len(nightly) == 8, f"nightly samples seven days plus HEAD, got {len(nightly)}")
check(len({sha for sha, _ in nightly}) == len(nightly), "nightly samples are unique")
check(nightly[-1][0] == nightly_head, "nightly includes current HEAD")

weekly_history = commits(None, 42, per_day=3)
weekly_head = weekly_history[0][0]
weekly = module.select("weekly", weekly_history, weekly_head, now)
check(len(weekly) == 9, f"weekly samples up to eight dates plus HEAD, got {len(weekly)}")
check(len({sha for sha, _ in weekly}) == len(weekly), "weekly samples are unique")
check(weekly[-1][0] == weekly_head, "weekly includes current HEAD")
print("test_commit_selection.py: branch=10 nightly=8 weekly=9 (including HEAD)")

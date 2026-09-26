#!/usr/bin/env python3
"""Refuses a reusable workflow that asks for more than its caller grants.

GitHub starts a called workflow with at most the permissions of the job that
calls it; a called job asking for more makes the whole run a startup failure
with no jobs and no log, which is how this was found.  actionlint does not
check it.  For every job that `uses: ./.github/workflows/<file>`, each
permission any job (or the top level) of the called workflow asks for must be
no more than the calling job grants (or its workflow's top level, when the
job grants nothing itself).

    scripts/check-workflow-permissions.py [.github/workflows]
"""

import os
import sys

import yaml

LEVEL = {"none": 0, "read": 1, "write": 2}


def scopes(perms):
    """a permissions block as {scope: level}; None when it is not stated"""
    if perms is None:
        return None
    if isinstance(perms, str):
        # read-all / write-all
        level = LEVEL["write" if perms == "write-all" else "read"]
        return {"*": level}
    return {k: LEVEL.get(str(v), 0) for k, v in perms.items()}


def granted(grant, scope):
    if grant is None:
        return None
    if scope in grant:
        return grant[scope]
    return grant.get("*", 0)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else ".github/workflows"
    flows = {}
    for name in sorted(os.listdir(root)):
        if name.endswith((".yml", ".yaml")):
            with open(os.path.join(root, name)) as f:
                flows[name] = yaml.safe_load(f)

    problems = []
    for name, flow in flows.items():
        top = scopes(flow.get("permissions"))
        for job_id, job in (flow.get("jobs") or {}).items():
            uses = job.get("uses", "")
            if not uses.startswith("./.github/workflows/"):
                continue
            called = flows.get(os.path.basename(uses))
            if called is None:
                problems.append("%s: %s calls %s, which does not exist"
                                % (name, job_id, uses))
                continue
            grant = scopes(job.get("permissions"))
            if grant is None:
                grant = top
            if grant is None:
                # the repository default applies; nothing to compare with
                continue
            asks = [("top level", scopes(called.get("permissions")))]
            asks += [("job " + k, scopes(v.get("permissions")))
                     for k, v in (called.get("jobs") or {}).items()]
            for where, ask in asks:
                for scope, level in (ask or {}).items():
                    have = granted(grant, scope)
                    if have is not None and level > have:
                        problems.append(
                            "%s: job %s grants %s: %s, but %s (%s) asks for %s"
                            % (name, job_id, scope,
                               [k for k, v in LEVEL.items() if v == have][0],
                               os.path.basename(uses), where,
                               [k for k, v in LEVEL.items() if v == level][0]))

    for p in problems:
        print("error: " + p)
    if not problems:
        print("reusable workflow permissions: ok (%d workflows)" % len(flows))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

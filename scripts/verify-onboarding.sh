#!/usr/bin/env bash
#
# Onboarding check: does a clean machine build, install and run this?
#
#   scripts/verify-onboarding.sh [image]
#
# Runs the documented path - the prerequisites from docs/deployment.md, the
# commands from docs/quickstart.md, the publish-and-read path from the README -
# in a container of the given image (ubuntu:24.04 by default).  The repository
# is mounted read-only; nothing here touches the host.
#
# A local run and CI run the same script, which is the point: a newcomer's
# first ten minutes are a test, not a hope.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="${1:-ubuntu:24.04}"
docker_cmd=("${DOCKER:-docker}")
docker info >/dev/null 2>&1 || docker_cmd=(sudo -n docker)

echo "== onboarding in $image"
"${docker_cmd[@]}" run --rm \
    -v "$root:/src:ro" \
    "$image" bash /src/scripts/verify-onboarding.container.sh

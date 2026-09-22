#!/bin/sh
#
# Renders the one setting that comes from the environment, then starts nginx.
#
# It writes conf.d/workers.conf and nothing else.  nginx.conf itself is left
# alone on purpose: it is the file people mount to change behaviour, and an
# entrypoint that rewrote it would silently discard whatever they mounted.

set -eu

# Workers.  One by default, and the default is a correctness decision rather
# than a conservative one: the media graph is per-worker state and the control
# API does not route an operation to the worker that owns a stream, so a
# program created through the API is visible to whichever worker answered and
# invisible to the rest.  Measured with two workers: 4 of 20 requests for a
# stream that had just been created returned stream_not_found.  Publishing is
# routed between workers; the API is not.  Scale by running more containers.
: "${NGINX_WORKER_PROCESSES:=1}"
export NGINX_WORKER_PROCESSES

if [ "$NGINX_WORKER_PROCESSES" != "1" ]; then
    echo "nginx-media: NGINX_WORKER_PROCESSES=$NGINX_WORKER_PROCESSES, but the" \
         "control API is not worker-aware: programs created through it will" \
         "be visible to some requests and not others." >&2
fi

# Which configuration to run.  The default is the image's own; point it at a
# mounted directory to keep a configuration outside the image:
#
#   -v "$PWD/container:/config:ro" -e NGINX_CONFIG=/config/nginx.conf
#
# A directory mount rather than a file mount, because a single-file bind mount
# is pinned to the inode and an editor that replaces the file would leave the
# container reading the old one.
: "${NGINX_CONFIG:=/etc/nginx/nginx.conf}"
export NGINX_CONFIG

if [ ! -f "$NGINX_CONFIG" ]; then
    echo "nginx-media: NGINX_CONFIG=$NGINX_CONFIG does not exist" >&2
    exit 1
fi

mkdir -p /etc/nginx/conf.d

envsubst '${NGINX_WORKER_PROCESSES}' \
    < /etc/nginx/conf.d/workers.conf.template \
    > /etc/nginx/conf.d/workers.conf

exec "$@"

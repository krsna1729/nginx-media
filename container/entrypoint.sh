#!/bin/sh
#
# Renders the one setting that comes from the environment, then starts nginx.
#
# It writes conf.d/workers.conf and nothing else.  nginx.conf itself is left
# alone on purpose: it is the file people mount to change behaviour, and an
# entrypoint that rewrote it would silently discard whatever they mounted.

set -eu

# Workers.  The graph is replicated across them, so any worker answers any
# request; what more workers buy is room for more programs, not more fanout for
# one.  Each program is owned by a single worker - whichever created it - and
# that worker does all of the fanout for it, so one program's cost does not
# spread however many workers there are.
#
# One is the default because it is the predictable one: with a single worker
# every program is carried by the same process, ownership is not a question,
# and the numbers in the documentation were measured that way.  Raise it for
# program count, and read "How many workers, and what they buy" in
# docs/operations.md first.
: "${NGINX_WORKER_PROCESSES:=1}"
export NGINX_WORKER_PROCESSES

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

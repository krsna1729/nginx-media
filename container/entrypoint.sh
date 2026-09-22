#!/bin/sh
#
# Renders the configuration template and starts nginx.
#
# The template is the input and /etc/nginx/nginx.conf is the output, which is
# the way round that makes a custom configuration work: mount your file over
# /etc/nginx/nginx.conf.template and it is what nginx ends up reading.  Only
# ${NGINX_WORKER_PROCESSES} is substituted, so an nginx variable in your
# config ($host, $remote_addr) is left alone - substituting every $VAR is how
# that goes wrong.

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

envsubst '${NGINX_WORKER_PROCESSES}' \
    < /etc/nginx/nginx.conf.template \
    > /etc/nginx/nginx.conf

exec "$@"

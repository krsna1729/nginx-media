# nginx-media container image.
#
# An NGINX media subsystem: redundant sources selected into one logical
# program and carried to consumers.  The image starts with no streams
# declared, because the media graph is runtime state rather than
# configuration - you build the program you want through the control API.
#
# Run it:
#
#   docker run -d --name media \
#     -p 8080:8080 \
#     -p 1935:1935 \
#     -p 9000:9000/udp \
#     -v media:/var/lib/nginx/media \
#     -v logs:/var/log/nginx \
#     ghcr.io/krsna1729/nginx-media:latest
#
# Then create a program and give it something to carry:
#
#   curl -X POST -H 'Content-Type: application/json' \
#     -d '{"application":"live","name":"demo"}' \
#     http://127.0.0.1:8080/media/api/v1/streams
#
#   curl -X POST -H 'Content-Type: application/json' \
#     -d '{"id":"file1","type":"file","path":"/media/source.ts"}' \
#     http://127.0.0.1:8080/media/api/v1/streams/live/demo/sources
#
# HLS comes out at http://127.0.0.1:8080/hls/index.m3u8.  Or publish live:
#
#   srt://host:9000?streamid=#!::r=live/demo,m=publish
#   rtmp://host:1935/live/demo
#
# The configuration can be changed without rebuilding.  container/nginx.conf
# is a real file and the entrypoint leaves it alone, so mounting it is live:
# an edit on the host is visible inside the running container immediately, and
# nginx acts on it when it is reloaded.
#
#   -v "$PWD/container:/config:ro" -e NGINX_CONFIG=/config/nginx.conf
#   docker exec media nginx -s reload
#
# Mount the directory rather than the file: a single-file bind mount is pinned
# to the inode, so `sed -i` or an editor that replaces the file leaves the
# container reading the old one.  A directory mount sees the replacement.
#
# The entrypoint writes exactly one thing, /etc/nginx/conf.d/workers.conf,
# from the environment:
#
#   -e NGINX_WORKER_PROCESSES=4
#
# It defaults to 1, and raising it is not free - the control API is not
# worker-aware.  The entrypoint says so and warns when it is raised.
#
# Ready-made configurations for different scenarios are in
# container/examples/ and are copied into the image at /etc/nginx/examples/:
# API only, SRT ingest, RTMP ingest, a relay, an SRT sink, recording, and a
# full one for testing.
#
# Three stages.  The build stage has the toolchain, the pinned nginx source
# and the module.  The runtime stage has the runtime libraries and the
# installed prefix and nothing else: no compiler, no source tree, no headers.
# The test stage is the build stage plus what the integration suite needs, and
# it is what runs the suite in CI so that a local run and a CI run are the
# same run.  ffmpeg is deliberately absent from the runtime stage - the media
# core does not decode, and an image that carries a decoder invites a
# deployment that uses one.

# The base is an argument so CI can run the same suite against a different
# Debian without editing this file: trixie is the shipped image, sid is the
# canary for the newest libraries.
ARG BASE=debian:trixie

FROM ${BASE} AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential pkg-config ca-certificates curl \
        libpcre2-dev zlib1g-dev libssl-dev libsrt-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# The build reads the tree, the config and the pinned version; copy those
# first so a source edit does not invalidate the dependency layer above.
COPY scripts/build-nginx.sh /src/scripts/build-nginx.sh
COPY config /src/config
COPY src /src/src

# A real installed layout rather than a path inside the build tree, because
# nginx compiles its prefix into the binary and resolves its temp paths and
# its default error log against it at run time.
ENV BUILD_DIR=/src/.build
ENV NGINX_PREFIX=/usr/local/nginx
RUN bash /src/scripts/build-nginx.sh


# The integration suite's environment.  This is the same stage CI runs the
# suite in, so a failure here is reproducible locally with
# `make test-in-container` and not only on a runner.
FROM build AS test

ENV DEBIAN_FRONTEND=noninteractive

# ffmpeg is the publisher the suite drives, iproute2 is what the namespace
# topology uses, and the rest is what the scripts assume is present.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ffmpeg iproute2 procps ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# The suite drives the nginx this stage builds, and it looks for it under
# .build/nginx-install rather than the /usr/local/nginx the runtime image
# uses.  The build script treats the prefix as part of what invalidates a
# configure, so this rebuilds rather than silently reusing the other layout.
ENV NGINX_PREFIX=
RUN bash /src/scripts/build-nginx.sh

COPY Makefile /src/Makefile
COPY tests /src/tests
COPY container /src/container

WORKDIR /src

# Overridden by the caller with the targets to run.
CMD ["make", "unit"]


FROM ${BASE} AS runtime

# Provenance, in the OCI standard labels rather than in a tag per build.  A
# tag per commit is a tag explosion that gets worse the longer the project
# runs, and these are readable from `docker inspect`, from `docker images`,
# and from every registry UI - and they travel with the image when it is
# copied or mirrored, which a tag does not.
#
# The workflow fills all four in; the defaults are for a plain local build.
ARG IMAGE_VERSION=dev
ARG IMAGE_REVISION=unknown
ARG IMAGE_CREATED=unknown
ARG IMAGE_SOURCE=https://github.com/krsna1729/nginx-media

LABEL org.opencontainers.image.title="nginx-media" \
      org.opencontainers.image.description="An NGINX media subsystem: redundant sources selected into one logical program and carried to consumers" \
      org.opencontainers.image.source="${IMAGE_SOURCE}" \
      org.opencontainers.image.version="${IMAGE_VERSION}" \
      org.opencontainers.image.revision="${IMAGE_REVISION}" \
      org.opencontainers.image.created="${IMAGE_CREATED}"

ENV DEBIAN_FRONTEND=noninteractive

# Runtime libraries only, and three of them are load-bearing:
#
#   libsrt          the SRT transport links against it
#   ca-certificates an HLS destination pulls and pushes over HTTPS, and with
#                   no trust store every TLS fetch fails to verify - the
#                   youtube_live profile requires https, so an image without
#                   this could not do the thing the profile exists for
#   curl            the healthcheck below, which asks the control API rather
#                   than asking nginx whether its own config parses
#   gettext-base    envsubst, which the entrypoint uses to render the template
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libpcre2-8-0 zlib1g libssl3t64 libsrt1.5-openssl ca-certificates \
        curl gettext-base \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/nginx /usr/local/nginx
RUN ln -s /usr/local/nginx/sbin/nginx /usr/local/sbin/nginx

COPY container/nginx.conf /etc/nginx/nginx.conf
COPY container/workers.conf.template /etc/nginx/conf.d/workers.conf.template
COPY container/examples/ /etc/nginx/examples/
COPY container/entrypoint.sh /usr/local/bin/entrypoint.sh

# Rendered once here as well, so the image is inspectable and the config is
# valid without starting it.  The entrypoint re-renders on every start.
RUN NGINX_WORKER_PROCESSES=1 \
        envsubst '${NGINX_WORKER_PROCESSES}' \
        < /etc/nginx/conf.d/workers.conf.template \
        > /etc/nginx/conf.d/workers.conf

# The master starts as root so it can bind and setuid; the workers run as
# www-data, so everything they write has to belong to it.  Without this the
# image builds, starts, serves the API and then fails the moment a program
# carries media, because the HLS directory is root-owned and the worker
# cannot write a segment into it.
RUN mkdir -p /var/lib/nginx/media/hls \
             /var/lib/nginx/media/record \
             /var/lib/nginx/media/ingest \
             /var/log/nginx \
    && chown -R www-data:www-data /var/lib/nginx /var/log/nginx

# SRT ingest: publishers connect here.
EXPOSE 9000/udp
# RTMP ingest.
EXPOSE 1935
# The control API, and the local HLS output.
EXPOSE 8080

# The media directory is the state worth keeping: HLS output, recordings and
# the ingest drop box.  Mount it or the output dies with the container.
VOLUME ["/var/lib/nginx/media"]
# Logs, so they survive a restart and can be shipped somewhere else.
VOLUME ["/var/log/nginx"]

STOPSIGNAL SIGQUIT

# Asks the control API, which only answers when the worker is up and serving.
# `nginx -t` would pass with no worker running at all, which is not health.
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s \
    CMD curl -fsS http://127.0.0.1:8080/media/api/v1/streams >/dev/null \
        || exit 1

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

# -e because the error log path is compiled into the binary and points at the
# build tree; nginx opens it before it reads any configuration.  The config
# path comes from NGINX_CONFIG so that a mounted configuration is used, which
# is why this goes through a shell rather than naming a file directly.
CMD ["/bin/sh", "-c", "exec nginx -e /var/log/nginx/error.log -g 'daemon off;' -c \"${NGINX_CONFIG:-/etc/nginx/nginx.conf}\""]

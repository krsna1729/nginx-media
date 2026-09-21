# nginx-media container image.
#
# Two stages, because a media server has no business shipping a compiler.  The
# build stage fetches the pinned nginx source, builds it with the module
# against the distribution's libsrt, and the runtime stage keeps only what the
# result needs to run.
#
# The image is usable as it stands: it starts an SRT listener, an RTMP
# listener, a control API and a local HLS output, so `docker run -p ...` gives
# something to publish to without writing a config first.

FROM ubuntu:24.04 AS build

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

ENV BUILD_DIR=/src/.build
ENV NGINX_PREFIX=/usr/local/nginx
RUN bash /src/scripts/build-nginx.sh

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# The runtime libraries only.  libsrt is what the SRT transport links against,
# and ffmpeg is deliberately absent: the media core does not decode, and an
# image that carries a decoder invites a deployment that uses one.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libpcre2-8-0 zlib1g libssl3t64 libsrt1.5-openssl \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /var/log/nginx /var/lib/nginx/media/hls \
                /var/lib/nginx/media/record /var/lib/nginx/media/ingest

COPY --from=build /usr/local/nginx /usr/local/nginx
RUN ln -s /usr/local/nginx/sbin/nginx /usr/local/sbin/nginx \
    && mkdir -p /usr/local/nginx/logs
COPY container/nginx.conf /etc/nginx/nginx.conf

EXPOSE 1935 8080 9000/udp

STOPSIGNAL SIGQUIT

# A media server that cannot write its HLS directory is not a media server.
VOLUME ["/var/lib/nginx/media"]

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s \
    CMD /usr/local/sbin/nginx -t -c /etc/nginx/nginx.conf || exit 1

# -e because the error log path is compiled into the binary and points at the
# build tree; nginx opens it before it reads any configuration, so without
# this the very first thing it does is print an alert.
CMD ["/usr/local/sbin/nginx", "-e", "/var/log/nginx/error.log", \
     "-g", "daemon off;", "-c", "/etc/nginx/nginx.conf"]

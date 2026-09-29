# Deployment

Three ways to run this, in order of how much you want to own.  The first one
is also the onboarding path: nothing here is different for a first-time
reader, and `make onboarding` runs the whole of it in a container so the
instructions are tested rather than believed.

## 1. From source

Prerequisites (Debian/Ubuntu; on Arch, `base-devel pkgconf pcre2 zlib openssl
srt ffmpeg`):

```sh
sudo apt-get install -y build-essential pkg-config curl ca-certificates \
    python3 libpcre2-dev zlib1g-dev libssl-dev libsrt-openssl-dev ffmpeg
```

`ffmpeg` is what the examples publish with; `python3` is what they parse the
API's JSON with.  Neither is needed to build or run the server.

Then the four commands, in a checkout:

```sh
make nginx      # fetch and build the pinned NGINX with this module
make unit       # the core suites, under ASan and UBSan
make smoke      # start it, serve a request, stop it
make install    # PREFIX=/usr/local/nginx-media by default
```

`make nginx` builds into `.build/` and never touches a system nginx.
`make install` puts the binary, a starting configuration
(`container/nginx.conf`), the README and the service unit under the prefix,
and prints the two service commands.  `docs/quickstart.md` walks the same path
with the output each command gives.

### Which library it links

`make nginx` links the distribution's libsrt, which is what a user of that
distribution would get.  To link the library the capacity numbers were taken
against, at the commit in `scripts/srt-pins.sh`:

```sh
make srt-pinned       # Haivision/srt v1.5.7 -> .build/nginx-haivision/install/sbin/nginx
make nginx-robotweax  # robotweax/srt v0.2.6, the second implementation
```

Both build the library from source and record an rpath into it, so the binary
carries the version it was built against.  The distribution's library is the
default because a distribution's security update reaches it; the pinned one is
what a capacity claim names.

### Run it as a service

```sh
sudo useradd --system --home /usr/local/nginx-media \
    --shell /usr/sbin/nologin nginx-media
sudo chown -R nginx-media: /usr/local/nginx-media/logs \
    /usr/local/nginx-media/hls /usr/local/nginx-media/record
sudo cp /usr/local/nginx-media/share/nginx-media.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nginx-media
systemctl status nginx-media
```

The unit runs the master unprivileged, with `NoNewPrivileges`, a system-call
filter and a `ReadWritePaths` list limited to the directories media is written
to.  The example configuration binds ports above 1024; to bind 80 or 443
either add `CAP_NET_BIND_SERVICE` to the unit or put another nginx in front
(`docs/security.md`).

### Verify the deployment

```sh
curl -s localhost:8080/media/api/v1/streams      # {"streams":[],"count":0}
```

One trap worth knowing before the first publish: a root master drops its
workers to `nobody` unless the configuration sets `user`, so the HLS and
recording roots have to be writable by that user.  The unit below runs the
whole server as a service user, which is why it does not have this problem;
`make onboarding` runs this page's path in a container and would fail on it.

Then publish something and read it back, exactly as in the quickstart:

```sh
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"demo"}' \
    localhost:8080/media/api/v1/streams

ffmpeg -re -f lavfi -i "testsrc2=size=320x180:rate=25" -t 10 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -f mpegts \
    "srt://127.0.0.1:9000?streamid=CW3AB274M5NCZQX4896JH86PR7"

curl -s localhost:8080/hls/live/demo/index.m3u8
```

If the playlist appears, ingest, the program and HLS egress are all working.
`docs/operations.md` is what to watch afterwards: `fanout_ms` first, then the
worker's event-loop delay and the feed backlog.

## 2. The container

```sh
docker run -d --name media \
    -p 8080:8080 -p 1935:1935 -p 9000:9000/udp \
    -v "$PWD/container:/config:ro" -e NGINX_CONFIG=/config/nginx.conf \
    -v media:/var/lib/nginx/media \
    ghcr.io/krsna1729/nginx-media:latest
```

The header of `container/nginx.conf` is the full recipe, including why the
config directory is mounted rather than the file; `container/examples/` has
scenarios (SRT or RTMP ingest, recording, an onward relay).  The image is
built by `.github/workflows/container.yml` and tested by the same integration
suite as the source build.

## 3. The static artifact

A release carries `nginx-<version>-linux-x86_64-static.tar.gz` beside the
dynamic binary: one file with no library dependencies at all, built from the
same tag.  Unpack it and run it the same way (`nginx -p <dir> -c nginx.conf`);
the unit above works unchanged with its `ExecStart` pointing at the file.

Its trade is in `docs/development.md`: nothing to install and nothing to
drift, but a libsrt or OpenSSL fix is a new download rather than a system
update.

## Sizing

`docs/capacity-results.md` states what was measured, with the host and the
library each rung was taken on; the number that travels between hosts is
sender CPU per delivered Gbit/s.  `make bench-preflight` judges a host against
an offered load before a run spends its window on it.

## Upgrading

Streams, sources and destinations are runtime state, not configuration
(`docs/api.md`), so an upgrade is not a config migration:

```sh
sudo systemctl restart nginx-media     # new binary
sudo systemctl reload nginx-media      # configuration change only
```

A restart re-establishes destinations from the desired-state document a
controller replays; nothing in the configuration file describes what is on
air.

# Deployment

Three ways to run this, in order of how much you want to own.  The first one
is also the onboarding path: nothing here is different for a first-time
reader, and `make onboarding` runs the whole of it in a container so the
instructions are tested rather than believed.

## 1. From source

Prerequisites (Debian/Ubuntu; on Arch, `base-devel pkgconf pcre2 zlib openssl
c-ares srt ffmpeg`):

```sh
sudo apt-get install -y build-essential pkg-config curl ca-certificates \
    python3 libpcre2-dev zlib1g-dev libssl-dev libc-ares-dev libsrt-openssl-dev ffmpeg
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

### The ingest secret

```nginx
media_ingest_secret /var/lib/nginx-media/ingest.secret;
```

Ingest keys are derived from this secret.  The file must contain at least 32
bytes of unpredictable data; the server uses the first 32 bytes and creates a
32-byte secret with mode 0600 on first start if the file is absent.  Every
worker inherits it.  Back it up with the configuration — without it, existing
keys cannot be re-derived (they can still be rotated, which issues new ones).

For an existing installation that used `/usr/local/nginx-media/ingest.secret`,
copy that same file to `/var/lib/nginx-media/ingest.secret` and update the
directive before starting the service.  A secret shorter than 32 bytes is now
rejected; replacing it changes API-issued keys, so reconfigure or rotate those
publishers.

### Run it as a service

Before starting, adapt the installed copy of `container/nginx.conf` for the
service: replace the container worker include with `worker_processes 1;`,
remove `user www-data;` (the unit runs all processes as `nginx-media`), set
`pid logs/nginx.pid;` and `error_log logs/error.log info;`, and change the HLS
directive and public HLS alias to `/usr/local/nginx-media/hls`. Set
`media_ingest_secret /var/lib/nginx-media/ingest.secret;` explicitly. If
recording is enabled, use `/usr/local/nginx-media/record` for its directive and
alias. Keep the separate loopback admin listener and public refusal locations.
Use the same prefix in these paths and the unit if installing elsewhere.

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
to.  `StateDirectory=nginx-media` creates `/var/lib/nginx-media` for the
deployment secret and grants the service write access there under
`ProtectSystem=strict`.  The example configuration binds ports above 1024; to
bind 80 or 443 either add `CAP_NET_BIND_SERVICE` to the unit or put another
nginx in front (`docs/security.md`).

### Verify the deployment

```sh
curl -fsS http://127.0.0.1:8081/media/api/v1/streams # {"streams":[],"count":0}
curl -sS -o /dev/null -w '%{http_code}\n' \
    http://127.0.0.1:8080/media/api/v1/streams       # 403, even from localhost
```

One trap worth knowing before the first publish: a root master drops its
workers to `nobody` unless the configuration sets `user`, so the HLS and
recording roots have to be writable by that user.  The unit below runs the
whole server as a service user, which is why it does not have this problem;
`make onboarding` runs this page's path in a container and would fail on it.

Then publish something and read it back, exactly as in the quickstart:

```sh
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"demo"}' \
    http://127.0.0.1:8081/media/api/v1/streams
KEY=$(curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"id":"encoder","type":"srt","priority":100}' \
    http://127.0.0.1:8081/media/api/v1/streams/live/demo/sources \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["key"])')

ffmpeg -re -f lavfi -i "testsrc2=size=320x180:rate=25" -t 10 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -f mpegts \
    "srt://127.0.0.1:9000?streamid=$KEY"

curl -s localhost:8080/hls/live/demo/index.m3u8
```

If the playlist appears, ingest, the program and HLS egress are all working.
`docs/operations.md` is what to watch afterwards: `fanout_ms` first, then the
worker's event-loop delay and the feed backlog.

## 2. The container

```sh
docker run -d --name media \
    --cpus=2 --memory=1g --memory-swap=1g --ulimit nofile=8192:8192 \
    -e NGINX_WORKER_PROCESSES=1 \
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

### Administrative boundary

The default and every shipped container example put administrative routes
only on `127.0.0.1:8081`, with nginx `allow 127.0.0.1; deny all;`. Public
HTTP on 8080 uses `deny all;` in the `/media/api` and `/media/api/…`
locations, producing 403 in nginx's access phase. The admin listener also
denies paths outside its API location. This needs only the native access
module, not the rewrite module (the source build deliberately omits it).
HLS, recording locations where configured, and SRT/RTMP publisher listeners
stay on their media paths and ports. HLS ingest, when added, stays on its
publisher location with its existing source-key authorization.

For a native service, administer from the service host using the loopback
URL above. For a container, the authorized path is inside its network
namespace, using the image's existing `curl`:

```sh
docker exec media curl -fsS http://127.0.0.1:8081/media/api/v1/streams
docker exec media curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"demo"}' \
    http://127.0.0.1:8081/media/api/v1/streams
```

Do not publish 8081, add a public API alias, or proxy public HTTP to admin.
Loopback grants authority to processes in that network namespace, not to a
named user; restrict shell/container-exec access accordingly. Host-network
containers share the host's loopback trust boundary. An off-host controller
can use SSH to run the same local command. If a separate remote admin
listener is deliberately configured, restrict its network and use native
nginx TLS client-certificate or `auth_basic` protection; see
`docs/security.md`. The module has no administrative credential framework.

The entrypoint only renders `/etc/nginx/conf.d/workers.conf`; it never
rewrites or changes ownership of a mounted nginx configuration. The operator
owns its API boundary and resource settings. Source-key examples parse JSON
with host-side `python3`, not a new runtime-container dependency.

### Executable boundary checks

For a built native binary:

```sh
NGINX_BIN="$PWD/.build/nginx-install/sbin/nginx" \
    python3 tests/integration/deployment_surface_nginx.py
```

This starts real nginx instances from the default and all seven shipped
configs, changing only test-local paths, worker identity and ports. It checks
public GET/POST/DELETE refusal (also with an admin-looking Host header),
loopback stream create/list, and public HLS/recording file serving where
configured. It does not measure media performance. Default smoke HTTP ports
are 18080/18081; `PUBLIC_PORT` and `ADMIN_PORT` override them.
`make onboarding` additionally provisions an SRT source through loopback,
publishes real media and fetches the resulting public HLS playlist.
The image workflow runs public refusal, inside-container create/list and
public HLS-serving checks against the built image.


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

### Explicit resource budgets

The default config uses `worker_connections 4096;` per nginx worker and
`media_egress_workers srt 1; media_egress_workers hls_push 1;` in the main
context. The examples state their own connection limits (256–8192) and the
same fixed sender counts. Fixed counts disable adaptive pool sizing; they
are a predictable starting budget, not a measured capacity recommendation.
Sender pools are per nginx worker, so increasing `worker_processes` also
multiplies their thread budget. RTMP egress stays on the event loop.

The Docker command above limits the container to two CPU equivalents, 1 GiB
memory with no additional swap, and 8192 open FDs per process. For a native
service, an equivalent operator-selected systemd drop-in is:

```ini
# sudo systemctl edit nginx-media
[Service]
CPUQuota=200%
MemoryMax=1G
MemorySwapMax=0
LimitNOFILE=8192
```

Then run `sudo systemctl daemon-reload` and restart the service. The shipped
unit's `LimitNOFILE=1048576` alone is a ceiling, not a reserved allocation and
not a CPU/memory budget. Choose budgets from the actual workload: nginx
connection slots include HTTP clients and nginx-managed upstreams, while
SRT sockets, recordings, logs and runtime files also need process FDs.
Leave headroom above `worker_connections` for those files; a config with
8192 connection slots needs a larger FD limit than the example 8192 ceiling.
CPU quota is a shared scheduling limit, not CPU affinity or a latency promise.

Memory limits cover workers, replicated graph state, transport queues and
external FFmpeg transforms in the same cgroup. Exhausting the limit can kill
processes; it does not produce a graceful API capacity response. Recording
also needs an independently provisioned disk/retention budget. These
process/container controls are **not** R6 object quotas: there are no claimed
per-tenant/per-object stream, source or destination quotas here. Restrict
admin access and bound offered workloads operationally. No new sandbox
settings or throughput claims are implied by these examples.


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

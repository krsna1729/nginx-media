# nginx-media

An NGINX media subsystem.  Redundant input sources are selected into **one
logical program**, and that program is carried to consumers over SRT, RTMP,
HLS and to disk — bounded, deadline-aware, and sharing payloads rather than
copying them per receiver.

```text
INPUTS                    PROGRAM                        CONSUMERS
SRT / bonded SRT   ──┐     identity ─ probe ─ health   ┌── HLS
RTMP               ──┤  →  gate ─ selector ─ timeline →├── recording (PROGRAM/ISO/RAW)
file               ──┤     one program, one timeline  ├── SRT
HLS pull           ──┤                                 ├── RTMP
HLS PUT ingest     ──┘                                 └── HLS push
```

A logical stream is **not** a publisher.  One program may have several
independent sources; source health is not source selection; a standby source
is held hot rather than reconnected on failure; and fanout does not copy or
queue media per subscriber in steady state.

The media graph is **runtime state, not configuration** — streams, their
sources and their destinations are created, changed and deleted through a
control API with revisions, idempotent replay and a desired-state document.

## Try it

```sh
make nginx      # fetch and build the pinned NGINX with this module
make unit       # the core suites, under ASan and UBSan
make smoke      # start it, serve a request, stop it
```

Then, with `.build/nginx-install/sbin/nginx -p . -c <your config>` running:

```sh
# a program that carries media, built entirely through the API
curl -X POST -H 'Content-Type: application/json' \
  -d '{"application":"live","name":"demo"}' \
  http://127.0.0.1:8080/media/api/v1/streams

# publish to it over SRT, RTMP, or from a file
ffmpeg -re -f lavfi -i "testsrc2=size=1280x720:rate=25" \
  -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p \
  -f mpegts "srt://127.0.0.1:9000?streamid=#!::r=live/demo,m=publish"

curl http://127.0.0.1:8080/hls/live/demo/index.m3u8
```

`docs/quickstart.md` walks through this with the output each command gives.

## What is verified

Every claim below is produced by a command in this repository.

| | |
|---|---|
| Core suites | 25 unit binaries, clean under ASan/UBSan, concurrency suites clean under ThreadSanitizer |
| Integration | 24 targets: ingest, demux, selection, failover, HLS, recording, RTMP, SRT output, transport security, churn, soak, fault injection, and a network-namespace topology with `tc netem` |
| Capacity | `fanout_delay` percentiles (p50/p95/p99/p99.9) with conditions, a receiver storm against a 700 ms failover SLA, `bench-burst-sizing`'s real-media TS sweep, and `bench-capacity-curve`'s offered-load/fairness curve |
| Scaling | `make bench-worker-scaling`, `make bench-ingest-egress`, and `make bench-ingest-egress-fanout` (where each side saturates, per thread) |
| Static analysis | CodeQL on every pull request, on every push to main, and weekly |

```sh
make bench-worker-scaling      # what worker count buys
make bench-capacity-quality   # receiver-verified ladders, seven workloads
make bench-ingest-egress       # ingest versus egress, and the program spread
make bench-ingest-egress-fanout # ingest and egress under fanout, and the price of routing
make bench-capacity-curve     # offered-load capacity curve and fairness
make bench-fanout-delay        # fanout_delay percentiles with conditions
make bench-hls-fanout          # HLS serving: disk, sendfile, tmpfs, kTLS
make bench-burst-sizing        # MPEG-TS burst capacity sweep
```

## How the pieces fit

- **Protocol-neutral core.**  No decoding in the normal path.  Payloads are
  reference-counted and shared; a receiver gets a reference, not a copy.
- **Transports are a seam.**  SRT, RTMP and the rest sit behind adapters, and
  the SRT backend is already swappable at build time.  `make srt-qualify`
  exercises both.
- **One owner per program.**  The graph is replicated across workers so any
  worker answers any request; the *program* is not — one worker owns it and
  runs the tick that drives selection, fanout and outputs.  More workers buy
  room for more programs, not more fanout for one.
- **Bounded everywhere.**  Every ring, queue and cache has a ceiling, and
  crossing one drops and counts rather than growing.
- **Failure is a first-class state.**  Health is layered evidence, not a
  blended score, and a switch is decided by policy, announced as a
  discontinuity, and lands on a keyframe.

## Documentation

| | |
|---|---|
| [SPEC.md](SPEC.md) | the specification this is built to: the invariants, the phases, and the definition of done |
| [docs/architecture.md](docs/architecture.md) | how it is put together and why |
| [docs/configuration.md](docs/configuration.md) | every directive |
| [docs/api.md](docs/api.md) | every control API route, with the JSON each returns |
| [docs/operations.md](docs/operations.md) | running it: what to watch, failure modes, scaling, what is bounded |
| [docs/development.md](docs/development.md) | building, testing, the conventions, and the traps that have bitten this codebase |
| [docs/quickstart.md](docs/quickstart.md) | a runnable path from nothing to a program carrying media |
| [docs/zig-analysis.md](docs/zig-analysis.md) | what moving parts of this to Zig would and would not buy |

## Container

```sh
docker run -d --name media \
  -p 8080:8080 -p 1935:1935 -p 9000:9000/udp \
  -v media:/var/lib/nginx/media \
  ghcr.io/krsna1729/nginx-media:latest
```

The image starts with no streams declared and the control API on 8080.
`container/examples/` has ready-made configurations — API only, SRT ingest,
RTMP ingest, a relay, an SRT sink, recording, and a full one for testing.
`container/nginx.conf` is mountable and the mount is live:
`-v "$PWD/container:/config:ro" -e NGINX_CONFIG=/config/nginx.conf`.

## Contributing

```sh
make unit && make -C tests/unit tsan    # fast
make test-in-container                  # the whole suite in CI's container
make api-graph && make churn && make netns
```

`docs/development.md` covers the conventions, how to add a source or
destination type, and the mistakes this repository has already made — an
`ngx_str_t` is a pointer and a length whose data is not NUL-terminated, and
that one has bitten three separate times.

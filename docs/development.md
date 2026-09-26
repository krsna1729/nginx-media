# Development

`architecture.md` explains what the pieces are and which thread owns what, and
`configuration.md` and `api.md` describe the interfaces.  This document is the
working half: how to build the module, how to run its tests, the conventions
the code follows without being told to, the procedure for adding an input or an
output, and the mistakes that have already cost this codebase time.

## Build

### The module build

`make nginx` runs `scripts/build-nginx.sh`.  It fetches the pinned nginx
(`NGINX_VERSION`, default `1.30.5`) into `.build/nginx-<version>`, configures
it with `--add-module` pointing at the repository root, builds it, and installs
it.  Nothing on this path touches a system nginx.

```sh
make nginx    # .build/nginx-install/sbin/nginx
make unit     # the portable core suites, ASan + UBSan (see Tests)
```

The configure line is deliberately small: `--with-threads` because the ingest,
recording and push paths each own a thread, `--with-http_ssl_module` because
HLS pull and push speak TLS, and the rewrite and gzip modules are left out
because this module neither rewrites nor compresses.

Everything the script produces lands under `.build/`:

| Path | What it is |
|---|---|
| `.build/nginx-<version>/` | the unpacked source and its `objs/` tree |
| `.build/nginx-install/` | the installed prefix: `sbin/nginx` and its libraries |
| `.build/configure.log` | the last `./configure` output |
| `.build/build.log` | the last `make` output |
| `.build/install.log` | the last `make install` output (nginx's own generated Makefile) |
| `.build/.srt-backend` | which transport backends the last configure selected |

`BUILD_DIR` moves `.build` and `NGINX_PREFIX` moves the installed prefix.
`NGINX_PREFIX` exists because nginx compiles its prefix into the binary and
resolves its temp paths, its default error log and its prefix-relative
directories against it at run time — a container needs a real installed layout,
not a path inside a build tree.

`config` in the repository root is the module description nginx reads.  It has
two blocks: an `EVENT` module block for the core, SRT and RTMP modules, and an
`HTTP` block for the control API.  The core modules are declared as `EVENT`
rather than `CORE` on purpose — they register external eventfds in the event
loop, which is only possible after nginx's event module has initialised, and
the module type is what orders their `init_process` hooks.

Three things invalidate a cached configure: an edited `config`, an edited
`build-nginx.sh`, and a change to `MEDIA_SRT_BACKEND`.  The backend selection
is tracked with a stamp file because it changes which sources are linked, and
the script itself is tracked because adding a configure flag was otherwise
ignored on a tree that already had an `objs/Makefile`.

`MEDIA_SRT_BACKEND` selects which implementations of the module's own
transport contract are compiled in: `srt` (the default and the only production
configuration), `udp`, or `both`.  It is documented in `configuration.md`; for
development, `udp` is the one that builds with no libsrt at all, which is what
lets the rest of the module be built and tested on a machine without the
library.  The library is found through `pkg-config`; `SRT_DIR` selects an
installation that ships no `.pc` file, and `MEDIA_SRT_PKG` a package with a
different name (`robotweax-srt`, see `configuration.md`).

### The container

`docker build -f Containerfile -t nginx-media .` builds the same module in two
stages: a build stage with the toolchain and the distribution's libsrt, and a
runtime stage that keeps only the libraries the result needs.  The image sets
`NGINX_PREFIX=/usr/local/nginx`, installs `container/nginx.conf`, and starts
nginx with `-e /var/log/nginx/error.log -g 'daemon off;'` — the error log
explicitly, because nginx opens it before it reads any configuration and the
compiled-in prefix points at the build tree.  ffmpeg is deliberately absent:
the media core does not decode, and an image that carries a decoder invites a
deployment that uses one.  `.github/workflows/container.yml` builds the image
on pull requests and pushes it to GHCR on a version tag.

## Tests

There are three layers, and they answer different questions.  The unit suites
answer "does this function do what its comment says" against a stand-in for
nginx.  The integration scripts answer "does a real nginx carrying this module
behave" with real publishers and real players.  The benchmarks answer "what
does this cost", and report a number rather than a verdict.

### Unit suites: `tests/unit`

The portable core is compiled against `tests/unit/shim/ngx_shim.h` and
`shim/ngx_shim.c` instead of nginx.  The shim supplies the nginx types the core
uses (`ngx_str_t`, `ngx_pool_t`, `ngx_queue_t` and its macros, the atomic
macros, the status codes) plus a pool that tracks every allocation so a leak is
visible and frees everything on destroy.  Unlike `ngx_palloc()`, the shim pool
zeroes memory, which can only make a test stricter than production.  Two hooks
exist beyond that: `ngx_media_test_fail_alloc` makes every allocation fail so
callers' NULL handling is exercised, and `NGX_MEDIA_TEST_POISON=1` fills
allocations with `0xAA` and a poisoned tail, so a buffer handed to a syscall
without a terminator reads as a visibly wrong filename instead of a plausible
one.

Each suite is a standalone binary (`tests/unit/test_*.c`) using the macros in
`tests/unit/ngx_media_test.h`: `TEST_ASSERT*`, `TEST_LEAKS()` at the end of a
run, and `TEST_MAIN_END()` to report the check and failure counts.

```sh
make unit                                    # build and run all of them
make -C tests/unit build/test_buffer         # build one
./tests/unit/build/test_buffer               # run one
make -C tests/unit tsan                      # threaded suites under TSan
```

`make unit` is `make -C tests/unit test`: it builds the binaries into
`tests/unit/build/` and runs the 25 suites in the Makefile's `TESTS` list, with
AddressSanitizer and UndefinedBehaviorSanitizer on by default (`SAN`).  There
is no per-suite make target; the pattern rule `build/%: %.c` means one suite is
built by naming its binary, and run directly.  The suites that drive threads
(`test_record`, `test_lifecycle`, `test_feed`, `test_stream`, `test_owner`,
`test_srt_output`) are rebuilt into `build/tsan/` under ThreadSanitizer, which
is a separate directory so a normal build never mixes instrumentation.
`tests/unit/test_fuzz.c` scales its work with `NGX_MEDIA_FUZZ_SCALE` (the
nightly workflow runs it at 25).  The scale multiplies the number of inputs,
never the length of one: a generator's depth comes from the buffer that holds
it, and `FUZZ_BUF_WRITE` fails the suite if a hand-assembled write would pass
the end, so a larger nightly scale cannot overflow the harness itself.

`make bench-reporting` runs `tests/bench/test_reporting.py`: the tests for the
capacity reporting pipeline (observed versus configured delivery ratios, which
bytes count as delivered, and the benchmark completeness gate).  Plain
`python3`, no nginx, no network, a few seconds.

### Integration scripts: `tests/integration`

These start a real nginx built by `make nginx`, drive it with ffmpeg, curl and
the control API, and assert on what came out.  Each script owns a directory
under `.build/<name>/`, writes its own `conf/nginx.conf` there, starts
`.build/nginx-install/sbin/nginx -p <run> -c conf/nginx.conf`, and cleans up at
exit.  When one fails, the first place to look is
`.build/<name>/logs/error.log`; that is what CI uploads on a failure.

```sh
make nginx
make smoke          # start, serve, stop — the fastest check that the module links
make api-graph      # the runtime graph over the control API
make hls-pull       # a pull source over HTTPS with a private CA
./tests/integration/source_switch.sh   # equivalent: run the script directly
```

Each script binds fixed ports and several of them clean up with `pkill -KILL -f
'nginx: '`, so they are run serially, not concurrently.  They need `make nginx`
first, and in practice ffmpeg, curl and libsrt; the TLS cases generate their own
certificates with openssl.

A script that has to prove something about memory — a delete that must not free
a pool under a reader thread, a teardown that must not touch a destination an
uploader is inside — is worth running against a sanitized nginx.  The build
script takes the flags and stamps them like every other input, so a second tree
can hold a second build without disturbing the plain one:

```sh
BUILD_DIR=$PWD/.build-asan NGINX_PREFIX=$PWD/.build-asan/nginx-install \
NGINX_CC_OPT='-fsanitize=address -fno-omit-frame-pointer -g' \
NGINX_LD_OPT='-fsanitize=address' scripts/build-nginx.sh

# ASan writes to the process's stderr, which a daemonized worker does not have:
# log_path puts the report somewhere the test can read it.
ASAN_OPTIONS='detect_leaks=0:log_path=/tmp/asan-report' \
NGINX_BIN=$PWD/.build-asan/nginx-install/sbin/nginx \
tests/integration/stream_delete_nginx.sh
```

`detect_leaks=0` because a worker's exit is not a leak-free path by design.  The
scripts take the binary through `NGINX_BIN` and not `NGINX`, which nginx reads
itself and takes for a socket.  `make stream-delete` is the case built for this:
it deletes a stream while a file reader, an ingest reader thread and an
in-flight upload are all looking at it, and asserts the draining gauge returns
to zero — under ASan, an unguarded free shows up as a report instead of as a
silent corruption.

There is no umbrella `make all`.  The Makefile exposes one target per script
(`make srt-ingest`, `make rtmp`, `make srt-output`… — the full list is the
Makefile's `.PHONY` line), and the authoritative "everything" is `make
test-in-container`, which runs the Makefile's `TEST_TARGETS` in the shipped
image's environment: `.github/workflows/ci.yml` runs it (beside the fast subset
it names individually) on a pull request, and `.github/workflows/master.yml`
runs the individual targets — ingest and fixture, selection and switching, the
HLS directions (with hls-push-conformance), the ffmpeg interop matrix,
RTMP and RTMPS, srt-output, srt-output-mux, srt-lane-isolation
and srt-crypto, multi-worker, soak
and fault — before anything is tagged.  A new suite target belongs in
`TEST_TARGETS` for that reason; `srt-worker-ports`, the per-worker SRT ingest
endpoints, is the most recent one.  `make srt-qualify` is separate: it rebuilds
nginx with `MEDIA_SRT_BACKEND=both` and runs `srt_backend_qualify.sh`, which
drives one scenario against the SRT library and against the UDP test double.

### Benchmarks: `tests/bench`

```sh
make bench-hls            # serving HLS: disk vs tmpfs, sendfile, kTLS
make bench-hls-fanout     # many concurrent readers over a sliding window
make bench-push-fanout    # the uploader's copy path, 8 destinations
make bench-burst-sizing   # sweep TS backing capacity against real media
make bench-capacity-curve     # fixed-worker offered-load sweep and fairness
make bench-capacity-quality  # per-destination delivery checks across egress mixes
```

These measure rather than assert, and their numbers are what the HLS and push
sections of `architecture.md` and the README quote.  `bench-hls-fanout` wants a
tmpfs mount and says so and skips that variant when it cannot have one.
`bench-push-fanout` is a comparison between two builds of the same code with
one function changed, so it takes the two nginx binaries as its arguments
(defaulting to `/tmp/nginx-sendfile` and `/tmp/nginx-buffered`); the target
runs it with those defaults, which is only meaningful if you put them there.

`bench-capacity-curve` holds worker count constant (four by default).  It steps
program counts `1 2 4 8`, source bitrates `2M 6M 12M 20M`, then the exact
single-program SRT and RTMP destination ladder `1 8 32 64 128 256 512 1000`.
The remaining SRT cases isolate one stalled reader, saturate four programs with
250 destinations each, and run a sustained four-program / four-destination
fairness window.

Each case starts a fresh NGINX instance with HLS's shared transport mux active;
HLS segmentation and file I/O are part of the measured configuration.  The
receivers and destination objects are installed first.  SRT publishers then
start while nonblocking output handshakes settle, and the harness waits for all
expected peers and for program-frame/output-feed progress before taking the
measurement baseline.  The SRT receiver snapshots delivered payload bytes;
the RTMP receiver reports delivered payload-byte counter deltas.
Reports include overall `destination_fairness_jain` and
`healthy_destination_fairness_jain` (excludes the deliberately stalled reader),
per-program Jain fairness, first-byte spread, shard send/retransmission/queue
counters, sender-thread CPU, worker RSS/PSS, event-loop delay, per-worker
`ss -m` socket memory, and host-global `/proc/net/sockstat` values.

`CAPACITY_WORKERS` chooses the fixed worker count.  `CAPACITY_WINDOW`,
`CAPACITY_PROGRAM_STEPS`, `CAPACITY_BITRATE_STEPS`, and `CAPACITY_DEST_STEPS`
bound the shorter sweeps; `CAPACITY_SLOW_SECONDS`,
`CAPACITY_SATURATED_SECONDS`, and `CAPACITY_SUSTAINED_SECONDS` control the
scenario windows.  RTMP ladder cases use one worker.  Per case the harness
caps a source at 60 Mbit/s, nominal egress at 8000 Mbit/s, total destinations
at 1000, outputs per owner at 1000, and each measurement window at 600 seconds.
The focused `PHASES=capacity-srt-ladder` run executes only the single-program
SRT destination ladder. `capacity-slow-reader`, `capacity-saturated`, and
`capacity-sustained` each run one scenario without repeating the ladder.
This is an offered-load curve, not a worker-count sweep.

`bench-capacity-quality` runs seven calibrated delivery-quality ladders,
including the pairwise RTMP/SRT and HLS-push/SRT mixes and a three-way
50% RTMP / 45% HLS-push / 5% SRT mix.  The three-way case places all outputs
on the same program owner and validates each protocol against its calibrated
single-destination reference. `CAPACITY_QUALITY_MIXES` selects a subset;
`CAPACITY_QUALITY_STEPS` chooses the strictly increasing destination counts
(starting at one), and `CAPACITY_QUALITY_SECONDS` sets each measurement window.

#### Per-rung diagnostics and outcomes

Every capacity rung leaves `diagnostics.json` in its case directory
(`.build/ingest-egress-fanout/capacity/<label>/`), written by
`tests/bench/capacity_diagnostics.py`.  Every field is
`{"value", "unit", "status"}`; a measurement that could not be collected is
`"status": "unavailable"` with a reason, never a zero.  It holds:

- **cpu** — per worker: event loop, each `srt-egress-NN` sender and their sum,
  libsrt `SndQ`/`RcvQ`/`TsbPd`, HLS uploaders; the SRT/RTMP/HLS receivers and
  the publishers; all as percent of one core over the CPU window, and the
  sampled total as a share of the permitted CPUs.  Each process's own
  counter is the authoritative total - it keeps the CPU of threads that
  exited during the window, which no thread snapshot sees - and thread deltas
  only attribute it; what they cannot attribute is the process's
  `(unattributed)` role (`thread_accounting.processes`).  TID reuse is
  detected by start time, ticks use the kernel's `SC_CLK_TCK`, and a negative
  delta is rejected rather than summed.
- **cpu_environment** — online and permitted CPUs, every thread's affinity,
  cgroup quota and throttling, CPU frequency where the platform exposes it,
  host busy/steal/softirq fractions, threads per process and involuntary
  context switches.
- **network** — UDP/TCP counters from `/proc/net/snmp` and `netstat`,
  softnet drops, and UDP socket drops *per process* from `/proc/net/udp`, so
  a receive-buffer overflow is attributed to the side that overflowed.
- **srt / rtmp / hls_push** — queue timelines, per-lane service rates,
  enqueue-to-send lag, blocked sends, retransmissions, drops; RTMP scheduler
  visit counters; HLS uploader and segment accounting.
- **delivery** — each protocol's quality report and the short-interval ratios.
  The two are never mixed up: a *configured* acceptance threshold is reported
  under `delivery_ratio_threshold` and the HLS reader log's original
  `min_delivery_ratio` keeps that meaning, while the measured minimum is
  `observed_min_delivery_ratio` (with p5/p50/p95) for HLS origin and
  `quality_average_delivery_ratio_min` for the other three.  HLS origin's
  observed values come from `hls-readers.csv`, the per-reader record, so a
  reader whose row is unreadable is counted as unreadable rather than as a
  zero; the entry carries `observed_ratio_basis=reader-counted-bytes`.
- **efficiency** — delivered Gbit/s, and sender and receiver CPU per delivered
  Gbit/s.  Delivered means counted by the receivers (never a sender counter
  such as bytes queued, which keeps counting what a failing receiver never
  got), over each protocol's own receiver-side window.  All four protocols
  count, HLS origin included (`receiver_bytes_total` over
  `receiver_measurement_s`); a protocol whose only byte number is sender-side
  contributes nothing instead of contributing a cheap number.  The boundary
  rung depends on the host far more than this ratio does, which makes it the
  number to compare across commits and SRT libraries - within one host class,
  since CPU model and frequency, kernel, library version, compiler and NIC
  topology still move it.
- **saturation_notes** — plain statements of which side ran out first.

A rung has one of three outcomes: `setup-failure` (receivers, publishers or
destinations never became ready — quality was not measured, and the ladder
for that workload stops there without stopping the other workloads),
`quality-failure` (measured, and a delivery, continuity, error or lag
criterion failed), or `pass`.  A setup timeout is never turned into a
throughput number.  After the ladders, `capacity-matrix.json` in the run
directory lists, per workload, the highest passing rung, the first quality
failure and the setup-limited rungs.  Each rung there carries
`observed_delivery_ratio`, `delivery_ratio_threshold` and
`delivery_ratio_basis` (`observed`, `observed-for-<workloads>` when some
reported workload had no measurement, `threshold-only` when none did), so a
rung whose only delivery number is a gate says so.

`bench_history.py summarize` records the same three fields per rung, and its
gate fails a run that did not finish: no exit status recorded, an exit status
that is neither 0 nor 1 (a timeout or a kill), no diagnostics bundle for a
configuration, an expected workload or rung that was neither measured nor
skipped after the capacity boundary, a setup failure, or a quality failure at
or below the rung the tier requires every host to carry.  A ladder that stops
on purpose after two consecutive quality failures is finished, not aborted:
its remaining rungs are recorded as unmeasured.  `make bench-reporting` runs
the tests for all of this in seconds, with no nginx and no network.

The measuring side is kept from becoming the measured limit:

- **The receivers can live somewhere other than this host's loopback.**
  `CAPACITY_RECEIVER_EXEC` is the command prefix that starts a receiver
  program where it belongs and `CAPACITY_RECEIVER_ADDR` is the address the
  sender dials; unset, both keep the receivers on `127.0.0.1`, which is what
  every shared-runner tier uses.  Two other topologies are wired:
  `ip netns exec <ns>` / `nsenter --net=/run/netns/<ns> --no-fork` with the
  namespace's own address puts the traffic over a veth pair, and
  `ssh <host>` puts the receivers on another machine.  The namespace
  topology is one command - `eval "$(tests/bench/capacity_veth.sh up)"` -
  and it is the topology to use when the question is whether the software
  depends on loopback; it is not a NIC, so a number from it is a number
  about the software, not about a 25 Gbit/s link.
- **The preflight decides whether a rung can be measured at all.**  Before a
  rung at or above `CAPACITY_PREFLIGHT_MIN_DESTINATIONS` (128 by default,
  `CAPACITY_PREFLIGHT=auto|yes|no`), the harness fingerprints the sender and
  the receiver host (CPU model, permitted set, cgroup quota, frequency and
  throttling, NUMA, kernel, memory, every interface's speed/driver/offloads/
  MTU, the kernel's UDP limits, the transport library the binary carries) and
  measures the path between them with a UDP probe whose receiver counts what
  arrives.  `capacity_preflight.py judge` compares that with the offered
  load - payload times an explicit 1.20 overhead factor - and returns `ok`,
  `insufficient-evidence` (a core count or an unoffered load is not a
  capacity) or `infrastructure-limited` with the side and the number that
  was short.  An infrastructure-limited rung is recorded as such and stops
  that workload's ladder; it is never a quality failure and never a
  statement about nginx.  The bundle keeps the whole preflight under
  `preflight`, and the matrix and history list the rung separately.
- **Two hosts.**  `tests/bench/capacity_distributed.sh --receiver user@host
  --addr 10.10.0.2` mirrors the repository to the receiver host at the same
  absolute path, runs the preflight on both sides, then runs the ladder with
  the receivers started over ssh.  Artifacts come back through
  `CAPACITY_RECEIVER_FILE_TEST`/`_FETCH`/`_COLLECT` (a no-op when the
  receivers share this filesystem), so the same harness and the same
  diagnostics work for both.  The link between the hosts is the measurement:
  a 1000 x 8 Mbit/s rung needs about 9.6 Gbit/s of path including overhead,
  and on anything smaller the preflight says so instead of the run
  pretending.
- The SRT receiver listens on several ports (`PORT:N`) so no single receiver
  UDP socket and libsrt receive thread carries more than
  `CAPACITY_SRT_RECEIVER_PEERS` destinations (16 by default).
- `CAPACITY_NGINX_CPUS`, `CAPACITY_RECEIVER_CPUS` and
  `CAPACITY_PUBLISHER_CPUS` take `taskset` lists.  Unset means no pinning; on
  a host where sender and receivers share cores, pinning them apart turns
  "the host saturated" into an attributable result.
- `CAPACITY_NGINX` runs a different nginx binary against the same harness,
  e.g. a baseline build or one linked against robotweax/srt.
- `CAPACITY_SRT_HLS=no` measures pure SRT without HLS preparation (the
  default keeps it on as the regression baseline), and
  `CAPACITY_FIXED_SRT_WORKERS=1|2|4|…|adaptive` pins the SRT sender count.
- `CAPACITY_QUALITY_STOP_AFTER_FAILURES=N` ends a workload's ladder after N
  consecutive quality failures; the rungs above are reported as unmeasured,
  never as failures.  The CI tiers use 2.
- Each run takes a 256-port block from 10240-17919, chosen from its PID
  (`CAPACITY_BASE` overrides): below the kernel's ephemeral range, where an
  outgoing socket of the previous run can briefly hold a listener's port, and
  clear of the integration suites' fixed ports.

Run the suites and the benchmarks as an ordinary user.  Started as root,
nginx drops its workers to `nobody`, which cannot create HLS directories
under a root-owned build tree: HLS writes then fail, and suites that check
HLS output fail with them.

RTMP delivery divides by the interval between two receiver scrapes stamped
with the monotonic clock around each request, not by a shell-measured window,
and a 1 s receiver sampler provides the short-interval floor and the longest
stall.  HLS push is judged on identified segments: the segments first
delivered inside the window, each checked per destination for arrival,
exactly-once upload, size, lag behind the first destination, and a playlist
that referenced it before its upload completed.

`bench-burst-sizing` runs the in-process demux/mux/remux fixture at each
capacity in `CAPACITIES` (bytes).  It reports burst count, observed byte range,
frame rollovers and whether the remux remained lossless; the first lossless
row is a measurement for the fixture's access-unit distribution, not a
universal bitrate-derived setting.

#### CI tiers and history

`scripts/bench-ci.sh pr|branch|nightly|weekly` runs a versioned recipe per
tier (`.github/workflows/bench.yml` calls it): `pr` on every pull request,
`branch` after a merge to main, `nightly` all seven workloads, `weekly` long
rungs plus the fixed-vs-adaptive SRT sender comparison, HLS preparation off,
and libsrt vs robotweax/srt (one pinned commit, `ROBOTWEAX_REF` in
`bench.yml`, recorded in each run so a shift is ours and not the library's).

The gate (`bench_history.py gate`) fails the job on:

- a setup failure - the rung was never measured;
- an incomplete run - before a configuration runs, `bench-ci.sh` writes the
  mixes and rungs it expects (`expected.json`), and every one of them must end
  in a diagnostics bundle or in the harness's own list of rungs skipped after
  consecutive failures at the capacity boundary; a harness that did not reach
  its end marker, a mix never started, or a ladder cut short by a crash is
  incomplete, never a short green run;
- a quality failure at a rung the tier requires every host to carry.

The boundary itself is recorded, never gated, and a CPU-per-Gbit/s regression
against the recent history of the same tier is a warning (shared runners are
too noisy to gate on it).

`bench.yml` only measures, with a read-only token.  Publishing is
`bench-publish.yml`, the one job that writes to the repository, which
`master.yml`, `nightly.yml` and `weekly.yml` call after the benchmark: a
called workflow can never hold more permission than its caller grants, and a
pull request's caller grants `contents: read`.
`scripts/check-workflow-permissions.py` (the `workflows` job in `ci.yml`)
refuses any reusable workflow that asks for more than a caller grants, which
GitHub otherwise reports only as a startup failure with no jobs and no log.

`branch`, `nightly` and `weekly` results are appended to `data/<tier>.jsonl`
on the `gh-pages` branch, whose `index.html` (`tests/bench/history/index.html`)
charts them; enable GitHub Pages on that branch to browse it.


### ffmpeg interoperability

`make ffmpeg-interop` drives every way media enters with ffmpeg and reads
every way it leaves with ffmpeg, and judges each case on what ffmpeg itself
decodes - the codecs it finds, at least 25 decoded video frames, and decode
timestamps that never go backwards:

| Direction | Protocol | Codecs |
|---|---|---|
| in | SRT caller (MPEG-TS) | H.264+AAC, HEVC+AAC, H.264 only |
| in | RTMP publish (FLV, enhanced RTMP for HEVC) | H.264+AAC, HEVC+AAC, H.264 only |
| in | HLS push into `media_hls_ingest` (`-f hls -method PUT`, and `POST`) | H.264+AAC |
| in | HLS pull from an HTTP origin | H.264+AAC |
| in | file (MPEG-TS) | H.264+AAC |
| out | HLS origin | H.264+AAC, HEVC+AAC |
| out | RTMP play | H.264+AAC, HEVC+AAC |
| out | SRT destination to an ffmpeg listener | H.264+AAC, HEVC+AAC |
| out | RTMP destination to `ffmpeg -listen 1` | H.264+AAC |
| out | HLS push to an ingest endpoint, read back by ffmpeg | H.264+AAC |

The first run found two defects, both fixed with it: an RTMP publisher that
sends audio produced HLS segments without SPS/PPS (RTMP carries parameter
sets once, in the sequence header; keyframes now carry them in-band), and an
`hls_push` source could not be created before its encoder's first upload
created the directory.  Programs without video are not in the matrix: HLS
segments start at video keyframes.

## Conventions

These are not style preferences; each one is load-bearing, and the next section
shows what happens when one is missed.

**Allocation is pool allocation.**  Long-lived objects — a stream, a source, a
destination, its copied string fields — are allocated from the owning object's
pool (`ngx_pcalloc`, `ngx_pnalloc`) and live until the pool is destroyed.  There
is no free path for them and no reference counting; that is what `ordered
teardown` means here.  `ngx_media_destination_strdup()` is the pattern for a
string field: it copies into the pool and returns NULL for an empty source
string, so "unset" stays representable.  Hot paths allocate nothing: nothing in
the media path allocates per packet.

**Payload bytes are shared, never copied.**  `ngx_media_buf_t` is an immutable,
reference-counted payload.  A buffer is allocated, written by its owner, then
frozen with `ngx_media_buf_freeze()`; after that its bytes are shared by
reference across demux, the program feed, HLS, RTMP and SRT, and mutating them
is a bug.  The data area lives in the same allocation as the header, so
`ngx_media_buf_ref()`/`ngx_media_buf_unref()` are the only copying involved.
An `ngx_media_frame_t` owns exactly one reference to its payload: the case
matters because a frame is the unit that travels through the feed and each
consumer takes its own reference.  A burst prepared once for HLS, recording and
every destination therefore costs one reference per consumer, not one copy.

**An `ngx_str_t` is a slice, not a C string.**  It is a pointer and a length
into something larger — a configuration buffer, a request body, another
string — and it has no terminator.  Anything that expects a NUL (`inet_pton`,
`opendir`, `strrchr`) needs an explicit terminated copy, which is why the SRT
endpoint parser allocates `host->len + 1` and sets the last byte.  nginx's own
`ngx_strlchr()` is the bounded forward search and `ngx_media_strrlchr()` in
`ngx_media_compat.h` is the bounded reverse one; do not reach for `strchr` or
`strrchr`.  Comparing two `ngx_str_t`s means comparing lengths first, then
`ngx_memcmp` over `len` bytes, as `ngx_media_destination_find()` does.

**The core model does not log.**  The portable core files take a `ngx_log_t *`
where a caller has one to pass, but they do not call `ngx_log_error`; reporting
belongs to the caller.  The reason is mechanical: the unit build and the
`source_switch` harness compile the core without nginx's logging, so a log call
in the core breaks them.  Output belongs to the modules, the runtime and the
API, which have real loggers.

**Seams are ops tables, and they are registered once.**  Two contracts carry
"more than one implementation" in this codebase.  The SRT transport is
`ngx_media_srt_ops_t` in `src/srt/ngx_media_srt_transport.h`: the listener,
session, poll and send entry points, implemented by Haivision/srt and by a UDP
test double, with the backend symbols declared `weak` and selected by
`media_srt_backend` at runtime.  Destinations are `ngx_media_destination_ops_t`
in `src/core/ngx_media_destination.h`: the core owns the model and the list,
and a backend registers itself once with `ngx_media_destination_register()` and
is called for every destination of its type.  Sources have no such seam: the
`ngx_media_source_ops_t` forward declaration and the source's `ops` field are
vestigial, and a source type is a reader plus an API branch, not a registered
backend.

**Logging levels say what kind of event it is.**
`NGX_LOG_EMERG` is startup refusing to continue — a configuration error or a
subsystem that could not be created; the process does not come up.
`NGX_LOG_ERR` is a syscall or an operation that failed and is being reported.
`NGX_LOG_WARN` is something recoverable and abnormal: a refused request, a
dropped unit, a stalled remote, a fallback.  `NGX_LOG_NOTICE` is the lifecycle
and state narration an operator reads at `info`: a source opened, a destination
started, a switch, an upload path chosen, startup and shutdown.  `NGX_LOG_INFO`
is the per-request or per-session diagnostic.  The convention that matters in
practice is that NOTICE is where the "what happened" lives — every integration
test that asserts on the outside of the process can also be cross-checked
against it.

**Bounded queues drop and count; nothing blocks a worker.**  Every queue in the
data path has hard unit and byte ceilings, and a consumer that cannot keep up
loses its own units and counts them.  Nothing in the media path sleeps: the
file source is paced by the runtime tick, the ingest and push threads hand work
back through eventfds, and blocking I/O happens on threads that are not the
event loop.

**The graph is desired state, and mutations are revisions.**  Every object
carries a `revision` bumped by every mutation of it or of a child
(`ngx_media_destination_touch()`, `ngx_media_source_touch()`), a create that
names something existing returns it rather than failing, and a delete of
something absent succeeds.  A change to the graph model should keep all three
properties; they are what make a controller restart safe.

## Adding a destination backend

A destination is a runtime object; the transport behind it is a backend.  The
SRT backend (`src/srt/ngx_media_srt_module.c`), the RTMP backend
(`src/rtmp/ngx_media_rtmp_module.c`) and the HLS push backend
(`src/core/ngx_media_hls_push.c`) are the three worked examples; the steps are:

1. Give the type a number in `src/core/ngx_media_destination.h`, beside
   `NGX_MEDIA_DEST_SRT`, `_RTMP`, `_HLS_PUSH` and `_RECORD`.  The backend
   registry has `NGX_MEDIA_DESTINATION_MAX_BACKENDS` slots, one per type.
2. Teach the control API the name, in `ngx_media_api_dest_type()` in
   `src/api/ngx_media_api_module.c`, and extend the numeric range check in
   `ngx_media_api_dest_type_value()` if the new type is above the current last
   one.  A name the API does not know is refused with `unknown_destination_type`
   before anything is created.
3. Write the two functions the contract needs — `add()` starts the transport for
   an enabled destination and `remove()` stops it and releases `impl` — and fill
   in an `ngx_media_destination_ops_t`.  `impl` is the backend's own handle and
   is opaque to the core; the SRT backend stores its slot index there so removal
   does not have to search, and it does not allocate a thread per destination
   because its senders walk the table and pick up a new slot.
4. Register once with `ngx_media_destination_register()` from an init hook that
   always runs, not from a path that depends on other configuration.  Do not put
   the call behind a conditional early return: a deployment whose destinations
   all point outward and which therefore has no listener still has to be able to
   create one.
5. Read the fields the backend needs from `ngx_media_destination_t` — `host`,
   `port`, `streamid`, `path`, `ca_file`, `profile` — and validate them in
   `add()`, because that is where "the request was well formed but this build
   cannot carry it" is turned into a refused create.  A type with no backend is
   accepted by the API and then `ngx_media_destination_start()` fails and the
   create answers `500 destination_start_failed`, which is honest about what the
   build can do.
6. Take media from a shared preparation, never from the feed yourself.  The SRT
   backend registers the runtime's prepared transport bursts
   (`ngx_media_runtime_set_sink`) and fans them out to per-destination queues;
   the RTMP backend reads the stream's shared FLV preparation on the owning
   worker event loop.  The HLS push backend receives segmenter notifications
   after segment and playlist atomic rename, opens each sealed inode once, and
   queues its descriptor and metadata; it never scans the output directory.
   Sender-thread state and queue entries must be heap-owned.  Background
   threads must not touch `ngx_connection_t`, NGINX pools, timers, or handlers.
   Register with the worker-local egress manager to report queue/transport
   state and adapt SRT/HLS sender concurrency under the shared CPU budget; keep
   RTMP on its owning event loop.  The burst sink is a single global slot, so a
   second backend that wants prepared bursts either shares that registration or
   the seam has to be widened — it cannot simply register over the top.

The teardown order is not optional: `ngx_media_destination_remove()` stops the
transport before it unlinks the object, so a sender is never left writing into
a destination that no longer exists.

## Adding a source type

Sources are the other side of the same idea, but there is no ops table to
register with.  A source type is a reader plus an API branch:

1. Give the type a number in `src/core/ngx_media.h` beside
   `NGX_MEDIA_SOURCE_SRT`, `_RTMP`, `_FILE`, `_HLS_PULL` and `_HLS_PUSH`.
2. Teach the API the name in `ngx_media_api_source_type()` and the numeric range
   in `ngx_media_api_source_type_value()`, both in
   `src/api/ngx_media_api_module.c`.
3. Create the object with `ngx_media_stream_source_add(stream, id, type,
   priority, log)`, which registers it immediately — the source appears in the
   control API and in health before any media arrives, which is what makes a
   file slate a normal source rather than a special case.
4. If the type owns a reader, open it from the create branch instead of only
   registering a label — in `ngx_media_graph_source_open()`, which is where the
   graph decides whether this worker is the one that materialises it.
   `file`, `hls_push` and `hls_pull` do this; when the reader cannot be opened
   here the create fails with `400` and a named error (`source_open_failed`)
   rather than leaving a source that never produces.  A `hls_pull` source is
   the exception to that: its reader does the first fetch on its own thread, so
   an unreachable playlist is counted and the source stays unproductive until
   the origin answers.
5. Publish through the gate.  A reader calls `ngx_media_health_media()` with the
   frame's DTS and the current time, then `ngx_media_stream_publish()` — the
   same path a publisher's frames take — so selection, health and compatibility
   need to know nothing about where the bytes came from.  A trackset goes in
   through `ngx_media_source_tracks_set()` and marks the tracks evidence.  If
   the reader runs on the event loop rather than a thread, pace it from the
   runtime tick the way `ngx_media_file_advance_all()` does, never with a sleep.
6. Close the reader as part of the delete.  Removing a source has to close its
   transport or reader, not just drop the object: the publisher that is still
   attached otherwise re-creates the source on its next event, and the delete
   visibly does nothing.

Test both sides: a create that must fail, and the source carrying media into a
program.  `tests/integration/api_graph_nginx.sh` is the model — it builds a
graph from zero declared streams, attaches a real publisher, and asserts on the
program that comes out.

## Traps

Every one of these happened in this repository, and the lesson is usually not
the bug.

**An `ngx_str_t` with no terminator.**  The single most expensive mistake in
this codebase, made three times, all the same shape: an unbounded search on a
slice.  The SRT listener host worked only when the byte after it happened to be
NUL; `opendir()` on the ingest directory reported `ENOENT` for a directory that
plainly existed; `strrchr()` on a request URI produced a segment name of `1.1`.
Each failed in a way that pointed somewhere else entirely.  `ngx_media_strrlchr()`
exists because of this, and the comment above it is there to be read.

**nginx's generated Makefile does not track an addon's headers.**  Adding
`ca_file` to `ngx_media_destination_t` and building incrementally left two
translation units disagreeing about the struct's layout, so `ngx_queue_data()`
computed a wrong address and adding a second destination segfaulted in
`ngx_media_destination_find`.  From outside it looked like list corruption in
new code; the core dump showed an object whose `id.data` was `0x2`, whose
`host.data` was `0x17`, and whose `impl` held the tail of a path string.  It
vanished on a clean build, every time — which is the trap.  `config` now
declares `ngx_module_deps` as a glob over `src/*/*.h`, so touching a header
rebuilds the module.

**A stale binary and a swallowed exit code.**  "26 binaries, 0 failing" was
wrong: the unit build had in fact broken (the shim was missing an `ngx_inline`
the module headers use), and because the build's stderr went to `/dev/null` and
the old binaries were still sitting in `build/`, the stale result was read as a
pass.  The real count is 25.  A test result is worth nothing unless the build
that produced it succeeded: check the exit code, and build from clean when a
number changes unexpectedly.

**Registration behind an early return.**  The SRT destination backend was
registered inside the listener path, below a `return` for "no
`media_srt_listen` configured".  A deployment with no listener — one whose
destinations all point outward — got a `500` for every SRT destination it
created.  It looked like the create route was broken; it was the init order.
The sender pool and the registration now happen unconditionally, only the
listener is conditional.

**A pool that ended on an empty slot.**  The SRT sender returned when it met an
unused slot, which was harmless while every slot was declared up front and
fatal the moment slots could be empty: the pool exited immediately and nothing
was ever sent.  Idle slots are skipped, and only an explicit stopping flag ends
a sender.  The general form: a loop whose termination condition was written
against a table that was always full.

**A lock held across a network transfer.**  The HLS push upload ran while
holding the destination-list mutex; a stalled remote blocked the list, and the
runtime tick's scan takes the same lock, so the worker stopped serving media.
Uploads now dequeue under the lock and transfer with none held.  From outside
it looked like a producer that had gone quiet whenever a push destination was
slow.

**The write event that made RTMPS hang.**  `ngx_ssl_create_connection()` can
leave the write side disabled, and after the TLS handshake only the read event
was re-armed.  A publisher still worked — its traffic arrives from the peer and
the replies are small enough to leave immediately — but a player streams
continuously server-to-client and was never driven, so playback stalled
forever.  Both events are armed now, and every ffmpeg in that test has a hard
bound so a stall fails instead of hanging.  A hang in one direction only is
worth suspecting events for.

**A NULL logger.**  `ngx_media_http_tls_start()` dereferenced a NULL logger
because the pull passed one, and the process crashed.  The client now guards
every diagnostic: a library that crashes on a missing logger is a library with
a landmine, and the pull keeps the logger it was given.

**A leak only a counter could see.**  Deleting a stream never released its
per-stream runtime output slot (HLS and recording state is a fixed table), so
create/delete cycles filled the table and a new stream silently got no outputs
at all.  The first churn test passed with the fix disabled, because empty
streams never take an output slot; carrying media through each cycle still
passed, because a destroyed stream's pool is often reused at the same address
and the stale entry then matched the new stream.  What had teeth was a counter:
`GET /media/api/v1/metrics` reports the slots in use, and the test asserts they
come back.  When a leak is invisible to the obvious test, count the resource.

**A conclusion that was the test's fault.**  "A stream created through the API
produces no HLS" was reported as a defect with a page of reasoning.  It was the
test publishing before it created the source, so the ingest auto-created the
source from the stream id and the API create was a correct replay.  The fix was
to instrument the path and watch the bursts reach the segmenter before
concluding anything about the segmenter.

**Smaller ones, same species.**  The H.265 sequence-header builder accepted an
SPS shorter than 13 bytes and then read `level_idc` out of byte 12 — the garbage
level is what a player rejects, which is why RTMP playback of an H.265 program
failed to open.  The RTMP publisher's close log printed `frames=0` for every
session because `publisher_destroy` ran first and zeroed the struct.  The API's
JSON field reader was the scalar one, so an array body was silently truncated
while the route still answered `200` and applied nothing.  The configuration
prefix is compiled into the binary, so a build inside `.build/` had nginx
looking for its temp paths and default error log inside a tree that a container
does not have.  A test that dumped twelve seconds of media in one burst
produced nothing at all — correct behaviour, since the program feed is a
bounded window and a source that outruns every consumer overruns it, and an
unrealistic test rather than a bug.

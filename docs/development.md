# Development

`architecture.md` explains what the pieces are and which thread owns what, and
`configuration.md` and `api.md` describe the interfaces.  This document is the
working half: how to build the module, how to run its tests, the conventions
the code follows without being told to, the procedure for adding an input or an
output, and the mistakes that have already cost this codebase time.

The [codebase audit and implementation backlog](codebase-audit.md) records
the remaining correctness, test, benchmark and hardening work.

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
HTTP pull/push additionally require c-ares (`pkg-config libcares`): DNS runs in
the operation's poll loop, so the total deadline and cancellation also cover
name resolution. Debian/Ubuntu use `libc-ares-dev`; Arch uses `c-ares`.


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

The module and unit core use C23 (`gnu2x` for nginx's GNU extensions,
`c2x` for the portable unit build).  The build script keeps the compiler mode
and flags in its configure stamp, so switching options cannot reuse stale
objects.  Production builds use `-O2`, `_FORTIFY_SOURCE=2`,
`-fstack-protector-strong`, and linker RELRO, immediate binding, and a
non-executable stack.  Security-sensitive key and secret results are marked
`[[nodiscard]]`; an ignored failure is rejected by the warning-as-error
builds.  CI also builds and runs the unit suites with Clang.

`make srt-pinned` and `make nginx-robotweax` build the module against an SRT
library built from source at the pins in `scripts/srt-pins.sh` - Haivision/srt
v1.5.7 and robotweax/srt v0.2.6 - and put the binaries in
`.build/nginx-haivision/install/sbin/nginx` and
`.build/nginx-robotweax/install/sbin/nginx`.  The capacity tiers run the same
two targets, so a number taken here and a number taken in CI come from the same
library at the same commit; the binary records an rpath into its own library,
since the pinned one carries the system's soname.  The distribution's libsrt
still matters - it is what users have, and the ci matrix spans three of them -
but a capacity claim is about one library at one version.

Pinned builds also write content-bound `.provenance.json` beside their transport
artifacts and nginx binary. `source_commit` is the actual checked-out HEAD, not
the configured tag/ref. Fingerprinting verifies hashes and the binary's actual
linked-library resolution before using this metadata; system libraries and
stale sidecars report an unknown source commit. Native receiver builds retain
the executed compiler command, flags/version, source/binary hashes, actual
package/runtime library versions and receiver tool versions.

`MEDIA_SRT_BACKEND` selects which implementations of the module's own
transport contract are compiled in: `srt` (the default and the only production
configuration), `udp`, or `both`.  It is documented in `configuration.md`; for
development, `udp` is the one that builds with no libsrt at all, which is what
lets the rest of the module be built and tested on a machine without the
library.  The library is found through `pkg-config`; `SRT_DIR` selects an
installation that ships no `.pc` file, and `MEDIA_SRT_PKG` a package with a
different name (`robotweax-srt`, see `configuration.md`).

### A statically linked artifact

`scripts/build-static.sh` builds one file with no library dependencies at all:
nginx, the module, libsrt (at the pin in `scripts/srt-pins.sh`), OpenSSL, c-ares,
PCRE2 and zlib linked into a single static executable. It builds in an Alpine/musl
container - glibc warns loudly about static linking (NSS, `dlopen`,
`getaddrinfo`) and OpenSSL's providers are loadable modules there, while musl
is built for it and Alpine ships static OpenSSL, PCRE2 and zlib - and leaves
the binary in `.build/static/nginx`.

Measured here: **28.6 MB**, `readelf -d` reports no `NEEDED` entries at all,
and it passes the module's own integration suites unchanged - `smoke`,
`srt-ingest-nginx`, `rtmp` and `hls-push` - run with
`NGINX_BIN=.build/static/nginx`.  The dynamic build of the same source is
6.0 MB.

The trade is explicit.  A static artifact has no dependency matrix and cannot
drift from the library it was measured against; it also cannot be fixed by a
distribution's security update, so a libsrt or OpenSSL advisory means a new
artifact rather than a new system package.  That is why it is an *additional*
release asset, not a replacement for the dynamic build.

The release workflow ships both from the same tag: the dynamic binary against
the pinned Haivision/srt, and `nginx-<version>-linux-x86_64-static.tar.gz`
beside it, with the release notes naming the library versions and commits and
saying that the static artifact's upgrades are re-downloads.  The suite in
that workflow runs against the pinned binary (`NGINX_BIN` is exported), so the
artifact that is published is the artifact that was tested.

One product change came out of building it: `config` now names the C++ runtime
after libsrt (`-lstdc++`), because a static link of a C++ library needs it and
the module's library order is where it belongs.  With a shared libsrt the flag
is redundant, since the library records its own dependency.

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
visible and frees everything on destroy. Like nginx, `ngx_palloc()` and
`ngx_pnalloc()` leave requested memory uninitialized, while `ngx_pcalloc()`
zeroes exactly the requested bytes. Two hooks exist:
`ngx_media_test_fail_alloc` makes every allocation fail so callers' NULL
handling is exercised, and `NGX_MEDIA_TEST_POISON=1` fills direct and pool
allocations with `0xAA` and a poisoned tail. `ngx_pcalloc()` clears its requested
memory but leaves the poisoned tail intact.

Each suite is a standalone binary (`tests/unit/test_*.c`) using the macros in
`tests/unit/ngx_media_test.h`: `TEST_ASSERT*`, `TEST_LEAKS()` at the end of a
run, and `TEST_MAIN_END()` to report the check and failure counts.

```sh
make unit                                    # build and run all of them
NGX_MEDIA_TEST_POISON=1 make unit             # rerun every suite with poison
make -C tests/unit build/test_buffer         # build one
./tests/unit/build/test_buffer               # run one
make -C tests/unit tsan                      # threaded suites under TSan
```

`make unit` is `make -C tests/unit test`: it builds the binaries into
`tests/unit/build/` and discovers every `test_*.c` suite (35 at this revision).
AddressSanitizer and UndefinedBehaviorSanitizer are enabled by default (`SAN`);
sanitizer errors are fatal and frame pointers are retained. Source/shim headers
are build dependencies, so a header-only change rebuilds the affected suites.
There is no per-suite make target; name its `build/test_*` binary to build it.
The existing TSan subset (`test_record`, `test_lifecycle`, `test_feed`,
`test_stream`, `test_owner`, `test_srt_output`, `test_hls_ingest`) is rebuilt into
`build/tsan/`, separate from the normal ASan/UBSan binaries. TSan is additional
coverage, not a replacement for a suite's normal sanitizer run.
`tests/unit/test_fuzz.c` scales its work with `NGX_MEDIA_FUZZ_SCALE` (the
nightly workflow runs it at 25).  The scale multiplies the number of inputs,
never the length of one: a generator's depth comes from the buffer that holds
it, and `FUZZ_BUF_WRITE` fails the suite if a hand-assembled write would pass
the end, so a larger nightly scale cannot overflow the harness itself.

`make bench-reporting` runs reporting/commit selection, owned receiver lifecycle,
independent receiver controls, native provenance, artifact publication and viewer
behavior. Python3/Node plus isolated native fixtures; no nginx service is required.

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
RTMP and RTMPS, srt-output, srt-output-mux, srt-lane-isolation,
srt-lane-isolation-fanout and srt-crypto, multi-worker, soak
and fault — before anything is tagged.  A new suite target belongs in
`TEST_TARGETS` for that reason; `srt-worker-ports`, the per-worker SRT ingest
endpoints, is the most recent one.  `make srt-qualify` is separate: it rebuilds
nginx with `MEDIA_SRT_BACKEND=both` and runs `srt_backend_qualify.sh`, which
drives one scenario against the SRT library and against the UDP test double.

The release workflow runs `make test-integration` against the release build.
That target shares `TEST_TARGETS` with `test-in-container` but excludes unit
tests, which the workflow runs separately.  It then runs `make netns` and
`make srt-qualify`; `netns` stays separate because it needs host networking
privileges and is not part of the container suite.


### Benchmarks: `tests/bench`

```sh
make bench-hls            # serving HLS: disk vs tmpfs, sendfile, kTLS
make bench-hls-fanout     # many concurrent readers over a sliding window
make bench-push-fanout    # the uploader's copy path, 8 destinations
make bench-burst-sizing   # sweep TS backing capacity against real media
make bench-capacity-quality  # per-destination delivery checks across egress mixes
```

These measure rather than assert, and their numbers are what the HLS and push
sections of `architecture.md` and the README quote.  `bench-hls-fanout` wants a
tmpfs mount and says so and skips that variant when it cannot have one.
`bench-push-fanout` is a comparison between two builds of the same code with
one function changed, so it takes the two nginx binaries as its arguments
(defaulting to `/tmp/nginx-sendfile` and `/tmp/nginx-buffered`); the target
runs it with those defaults, which is only meaningful if you put them there.

Every capacity and developer benchmark runs from one entry,
`python3 tests/bench/capacity_harness.py` (package `tests/bench/harness/`).
`PHASES` selects `capacity-quality-ladder` (`make bench-capacity-quality`) or
the developer phases `floor ingest egress misplace topology`; the default is
`floor ingest egress misplace` (`make bench-ingest-egress-fanout`), and
`make bench-worker-topology` runs `topology`.  Every knob is an environment
variable, read in `tests/bench/harness/config.py`; `CAPACITY_RUN` is the run
directory (default `.build/ingest-egress-fanout`).  Exit status 0 means every
ladder passed, 1 that a ladder recorded failures, 3 a configuration or setup
error.

`bench-capacity-quality` runs the delivery-quality ladders listed in
`tests/bench/capacity-mixes.conf` (the one list the harness and
`scripts/bench-ci.sh` read): seven calibrated ladders, including the pairwise
RTMP/SRT and HLS-push/SRT mixes and a three-way
50% RTMP / 45% HLS-push / 5% SRT mix, and two SRT/RTMP/HLS-push contention
ladders.  The three-way case places all outputs
on the same program owner and validates each protocol against its calibrated
single-destination reference, which is stored only from a calibration rung that
passed its own checks. `CAPACITY_QUALITY_MIXES` selects a subset;

`CAPACITY_QUALITY_STEPS` chooses the strictly increasing destination counts
(starting at one), and `CAPACITY_QUALITY_SECONDS` sets each measurement window.

`make bench-core-critical-paths` rebuilds the configured native nginx recipe in
an isolated temporary tree and measures buffer references, feed publish/read,
TS mux/demux, RTMP chunk/AMF parsing, IPC reassembly, registry/owner operations,
and egress reports alone or during a 64-record metrics visit. A real TS fixture
is required (`make ts-fixture`). For a pinned collection:

```sh
python3 tests/bench/core_critical_paths.py --samples 10000 --cpu 8 \
    --visitor-cpu 10 --output .build/core-critical-pinned
```

Raw JSON/CSV retain three repetitions, sorted latency samples, p50/p95/p99/p99.9,
throughput and compiler/fixture/source provenance. Uninstrumented native timing
and linker-wrapped resource observations are separate builds. Allocation/copy
counts cover observable calls only; inlined operations and shared-library
internals remain explicitly unavailable, not zero. These are critical-path
observations, not a production capacity ceiling or a blocking regression gate.
Visit counters are deltas for that measured row. A visitor restricted to the
same CPU can remain unscheduled during a short row; zero completed visits do
not establish contention. The explicit visitor CPU above supplies a separate
core, and the provenance records both affinities.



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
  The SRT report carries two verdicts: `quality_pass` is the ladder's 0.95
  compatibility gate, and `quality_strict_full_rate` is the stricter one -
  every destination within `--strict-ratio` (0.999) of the reference, no
  interval below `--strict-interval-floor` (0.90) of it, zero MPEG-TS sync or
  continuity errors and no feed or output queue drops - with its components
  (`quality_strict_ratio_min`, `quality_strict_interval_min`,
  `quality_strict_ts_errors`).  A rung can pass the gate and fail the strict
  result; history records both (`strict_full_rate`,
  `strict_full_rate_pass`, `None` when the run never reported it).
  RTMP and SRT judges also emit `quality_measurement_valid`. Missing receiver
  destinations, malformed or non-reconciling counters, raw sampling gaps or
  out-of-window intervals invalidate the measurement before quality is judged.
  SRT additionally requires complete active-shard sender metrics and queue
  rounds covering the window. Invalid evidence emits `quality_pass=no`,
  `quality_measurement_valid=no` and `quality_setup_failure`, exits nonzero,
  and cannot qualify for strict full rate. Complete counters showing zero
  delivery still produce a measured quality failure. The harness joins its
  samplers and checks their exit statuses before taking closing receiver
  snapshots, while publishers are still running.
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
  CPU/Gbit comparisons additionally require proven boundary alignment:
  `efficiency.window_alignment` records each protocol's start/end brackets,
  overlap coverage and maximum endpoint skew. Both endpoint displacement
  (including capture uncertainty) and duration error must be at most **2% of
  the CPU window**. Equally long shifted windows do not pass. Missing old
  timestamps or missing remote-clock mapping leave CPU/Gbit unavailable while
  receiver-counted throughput remains visible; no shell-duration fallback is
  used. Remote CPU/process/network accounting stays under `receiver_host`,
  separate from local PID identities. Controller-side request brackets map
  remote boundaries; native remote monotonic clocks are never assumed shared.
  `receiver_host_fingerprint` and `receiver_nginx_fingerprint` retain native
  receiver architecture, boot identity, binary and transport-library provenance
  from collected receiver artifacts, including below preflight thresholds.
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
  every shared-runner tier uses.  The other wired topology is
  `ip netns exec <ns>` / `nsenter --net=/run/netns/<ns> --no-fork` with the
  namespace's own address, which puts the traffic over a veth pair. The namespace
  topology is one command - `eval "$(tests/bench/capacity_topology.sh namespace up)"` -
  and it is the topology to use when the question is whether the software
  depends on loopback; it is not a NIC, so a number from it is a number
  about the software, not about a 25 Gbit/s link.  `capacity_sweep.py` runs
  both topologies (`--topology namespace|loopback|both`) as separate cells,
  because the same engine at the same core count means different things on a
  shared stack and between namespaces; the cell name carries the topology
  (`nginx-haivision-4c`, `nginx-haivision-4c-loopback`).
- **Both topologies are tuned, and every tunable is a measured need.**
  `capacity_topology.sh` applies the settings and reads each one back, so a
  setting that did not take effect fails the run instead of quietly changing
  what the numbers mean.  Two settings exist for one reason: a packet arriving
  on a software device is processed on the CPU that transmitted it unless
  receive steering moves it.  On this machine, for one 1.7 Mpps flow into a
  receiver pinned to an E-core, the sender's CPU spent 20.7% (namespace) and
  33.5% (loopback, `lo`) of a core in softirq while the mask was zero, and the
  sender managed 1.67 M and 1.39 M sends; with the mask set to the receiver
  CPUs the softirq work moved to an E-core and the sender managed 2.71 M and
  1.75 M sends.  Without the mask a capacity run reads as a sender limit that
  is really the receivers' ingress.  Both modes also require 64 MiB socket
  buffers (`net.core.rmem_max`/`wmem_max`, checked, never set here: the write
  belongs to the host) and the namespace mode sets `tcp_tw_reuse` per
  namespace.  In loopback mode one device carries both directions, so the
  single mask is the receiver CPUs, which also moves the sender's own ingress
  off its cores; that asymmetry is the reason the namespace mode is the
  qualification topology and loopback is the single-host reference.  RFS would
  attribute per flow instead and remove the asymmetry, at the cost of two more
  host-global knobs (`rps_sock_flow_entries`, one `rps_flow_cnt` per queue)
  for a reverse direction that is acknowledgements and requests against a
  fanout of segments, so it is not used.  Loopback mode sets
  `net.ipv4.tcp_tw_reuse` (the namespace mode sets it per namespace) because
  every destination of a rung shares one address, and it restores whatever the
  together with the mask, when it goes down.  The shared address is
  loopback's own ceiling, and the arithmetic is worth stating: one destination
  pair offers `net.ipv4.ip_local_port_range` ports (28,232 here), so a single
  rung of a thousand connections cannot exhaust it, and only accumulation
  across successive rungs can.  Distinct addresses out of `127.0.0.0/8` would
  give each destination pair its own port space, and the kernel already routes
  that whole range to `lo`, so a sink can bind `127.0.0.7` with no
  configuration at all.  That is the remedy to reach for if a loopback rung
  ever fails on ports, and it is not applied yet: the diagnosis that put
  earlier RTMP rungs at that ceiling is unverified, so the spread would be
  complexity spent on an unmeasured limit.
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
- Each run takes a 512-port block from 10240-17919, chosen from its PID
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
stall. RTMP also checks every receiver and calibration against prepared
elementary media: `prepared-media.json` records ffprobe packet payload sizes
divided by the source's presentation span, not nominal bitrate or TS overhead.
HLS production is observed independently on the sender: playlist-identified
final segment sizes and filesystem last-write cut timestamps mapped to the
local monotonic clock are retained in `hls-production.json`. At least two cuts
inside the measurement interval are required; the production rate excludes the
first cut's bytes and divides subsequent bytes by the first-to-last cut span.
HLS production must itself meet the prepared elementary rate, so a common-mode
half-rate producer/reference is not calibration evidence. HLS push and readers
then check the exact produced segment set, with trailing grace of twice the
observed cut interval. Push checks arrival, exactly-once upload, size, relative
destination lag and playlist ordering; readers retain fetched identities for
local post-collection judging. Identified-window bytes and end-minus-grace
coverage are separate from raw receiver snapshot bytes and their full window;
only the latter enter efficiency. Relative and absolute gates both apply.
References are reset for every mix and persisted only after their own checks
pass. Offered/prepared rates and provenance travel into diagnostics and history.

HLS-reader acceptance excludes cuts before the reader's acknowledged actual
start, not just before the CPU snapshot. Missing in-window segments still fail
delivery.

CPU accounting opens only after sampler/receiver readiness and closes before
sampler joins or artifact fetches. Every capture first validates ownership and
prepares its native operation, including sampler quiescence, then acknowledges
READY while parked. The controller releases CPU accounting and all capture
tokens together. Process startup is outside request/reply brackets; signal
dispatch through native reply is inside. Receiver replies retain
native capture times separately from controller brackets. Failed preparation
aborts the boundary and reaps parked captures instead of opening accounting.
Extended host snapshots run outside CPU accounting and retain their own interval.
PSI shares use that host interval, not the stream or CPU denominator.
`stream.measurement.window` and `workers.measurement.window` retain independent
counter-collection brackets; frame FPS uses the stream-counter midpoint interval.
HLS readers use SIGUSR2 to seal end-of-window receipts but remain alive for the
closing CPU snapshot. SIGTERM releases report generation and exit afterward.
Warmup anchors refresh before start; a playlist completed across start is
consumed without skipping its in-window segments.



For an independent receiver control, use a valid MPEG-TS fixture:

```sh
python3 tests/bench/receiver_capacity_control.py --source "$SOURCE" \
    --destinations 16 --seconds 10 --bitrate-bps 8000000 \
    --sender-cpus 4,6 --receiver-cpus 12-15 --paired-stall \
    --out .build/receiver-control
```

The native source bypasses nginx egress and offers constant-paced 1316-byte
SRT messages. Raw attempted/accepted/blocked/error counters, receiver intervals,
TS integrity and CPU snapshots identify this explicit recipe. A qualified
baseline is a lower bound for that receiver recipe, not a production ceiling.
The paired control changes only destination `d0000`'s read behavior; it reports
`receiver-limited` only with full-rate source evidence and healthy lane-mates.
CPU pressure alone is not attribution. Controller exit, including SIGKILL,
terminates its native workloads through Linux parent-death binding.

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
and libsrt vs robotweax/srt (both built from source at the pins in
`scripts/srt-pins.sh` and recorded in each run, so a shift is ours and not the
library's).

PR rungs remain 1, 16 and 32, at 15 seconds by default, matching branch's
15-second default. `BENCH_SECONDS` overrides both tiers consistently. Their
nominal measurement lower bounds are 1.5 runner-minutes for PR
(`2 × 3 × 15 s`) and 3.75 for branch (`3 × 5 × 15 s`), excluding calibration,
startup and builds. Weekly's `all` configuration keeps the seven standard
workloads and the full ladder 1, 32, 64, 128, 192, 256, 384, 512, 768, 1000.
Its separate `contention` configuration runs both supplementary mixes with
calibration at 4 followed by every requested rung above 4. Rungs below 4 are
inapplicable to those proportions, not measured failures. Local harness
`CAPACITY_QUALITY_MIXES=all` still runs all nine and rejects explicit ladders
below a mix's minimum; malformed CI ladders likewise remain setup errors.

The gate (`bench_history.py gate`) fails the job on:

- a setup failure - the rung was never measured;
- an incomplete run - before a configuration runs, `bench-ci.sh` writes the
  mixes and rungs it expects (`expected.json`), and every one of them must end
  in a diagnostics bundle or in the harness's own list of rungs skipped after
  consecutive failures at the capacity boundary; the mixes the harness prints
  (`quality_mixes=`) must equal the manifest's, so the two cannot describe
  different workload sets; a harness that did not reach its end marker, a mix
  never started, or a ladder cut short by a crash is incomplete, never a short
  green run;
- a floor that was not shown to work - the tier's floor rung must have been
  measured and passed for every workload; a quality failure there fails, and
  so does an infrastructure-limited, unknown, skipped or missing outcome at or
  below it (an infrastructure limit above the floor is a notice).

The boundary itself is recorded, never gated. A CPU-per-Gbit/s regression is
only compared for passing rungs in complete configurations with the same
workload, exact runner fingerprint and the same recipe (rung duration, source
rate, mixes, stop-after rule, judge thresholds, worker settings and the hash
of the harness files; a ladder may be a prefix of the other's, so a pull
request is compared with main's `branch` records only if its recipe matches
theirs; historical 10 s pull-request rungs are not a baseline for the new
15 s defaults, and a record without a recipe compares with nothing). Compiler, configure
arguments, binary sha256 and the linked transport library are recorded per
workload as provenance, never compared. It needs at least three baselines and
must exceed their median by 25%. It remains a warning, not a gate, because
shared runners are noisy. The publisher opens one issue per
tier/configuration/workload/rung and skips duplicate open issues; each issue
links the published history and the workflow run.

History schema 3 records the configurations expected from the workflow and
marks absent matrix artifacts incomplete. The viewer plots only complete
configurations with benchmark fingerprints, separates runner fingerprints into
different series, and labels each latest-run row with the benchmark runner.

Viewer efficiency series use a fixed destination rung and separate stable
runner identity, measurement recipe and qualification/probe cohort. Hardware
filters project each mixed record's matching contents; inspector selection uses
the exact record/attempt rather than the commit SHA alone.

Artifacts retain the complete nested per-rung directory, including failed-rung
logs, interval CSVs, CPU/host/process snapshots, preflight and calibration/media
offer evidence. Default CI writes live runtime evidence under the upload root,
including during cancellation; an explicit `CAPACITY_RUN` remains caller-owned.
Generated media payloads and ingest secrets are excluded, not raw measurements.
Uploads explicitly include hidden files because the evidence root is `.build`;
missing required result/trend uploads fail rather than silently producing no
artifact. Trend uploads also exclude worktree `.git` metadata.
Artifact names identify workflow attempts. Publishers poll artifact visibility,
preserve explicit incomplete records, and replay immutable inputs against the
latest history tree after push races. Content IDs deduplicate retries without
overwriting another attempt or earlier incomplete evidence.
Publication and caller queues use GitHub's native `queue: max` with cancellation
disabled. A publisher-global group serializes regression issue list/create as
well as history writes; immutable replay remains protection against other
history writers. The documented platform ceiling is 100 pending jobs; excess
jobs can still be canceled, with source artifacts retained on their runs.
See [GitHub concurrency](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency).

`bench.yml` only measures, with `contents: read` and `actions: write` for
benchmark artifact upload. Publishing is `bench-publish.yml`, the job that
writes history and regression issues, which `master.yml`, `nightly.yml` and
`weekly.yml` call after the benchmark. They grant `contents: write` and
`issues: write`; a pull request's caller grants `contents: read` and never
calls the publisher.
`scripts/check-workflow-permissions.py` (the `workflows` job in `ci.yml`)
refuses any reusable workflow that asks for more than a caller grants, which
GitHub otherwise reports only as a startup failure with no jobs and no log.

`branch`, `nightly` and `weekly` results are appended to `data/<tier>.jsonl`
on the `gh-pages` branch, whose `index.html` (`tests/bench/history/index.html`)
charts them; enable GitHub Pages on that branch to browse it.

Non-PR branch, nightly and weekly jobs also run a bounded historical trend
probe in a separate job on one runner. It rebuilds each selected mainline
revision against the same pinned SRT library, then samples the tier's compact
ladder sequentially: ten recent commits for `branch`, one commit per day for
the previous seven days plus HEAD for `nightly`, and up to two commits per
week for the previous four weeks plus HEAD for `weekly`. These records carry a
workflow-scoped `comparison_group`; regressions compare only samples from that
group and same-runner fingerprint. The sampled profile uses rungs 1, 32 and
128 for 10 seconds each in branch/nightly and 1, 32 and 64 in weekly: 128 was
infrastructure-limited in the single-runner weekly probe, so 64 is the highest
weekly trend rung. Its contention ladder is 4, 32, 64; qualification retains
every higher rung of the full ladder. Trend
probes are appended to history and shown as such in the matrix, but never count
as qualification runs or as capacity-boundary diagnostics. The chart uses
commit time, and its highest passing sampled rung is explicitly not a capacity
limit.

The trend job's nominal receiver-measurement lower bound is 15 runner-minutes
for branch (`10 commits × 3 workloads × 3 rungs × 10 s`), 28 for nightly
(`8 × 7 × 3 × 10 s`), and 40.5 for weekly (`9 × 9 × 3 × 10 s`). These are
derived from the current recipe, not observed wall-clock costs; they exclude
per-revision builds, mixed-workload calibration, startup and retries. The
trend job has a 180-minute ceiling, separate from the full qualification job.


#### The data plane is Python: `tests/bench/capacity_client.py`

The harness (`tests/bench/capacity_harness.py`) owns process lifecycle,
ordering, cleanup, CPU pinning, namespace entry and signal delivery. HTTP
provisioning, JSON, retries and the destination numbering live in
`capacity_client.py`, because every defect found in the former shell harness
was in that half:

* a request body assembled by shell concatenation and then handed to Python
  through a `'''$res'''` literal, which a body containing a quote ends early
* a `sleep 0.2` on a retry path that runs about three times per destination,
  which put 93% of a rung's wall time into setup nothing measured
* a retry loop bounded by attempt count, which would report a slow owner as a
  setup error within half a second

The harness calls it once per program, never once per destination. This is not a
style preference: measured on the same sender, one process per destination
costs **64.86 ms** against **6.13 ms** for a `curl` invocation, because
interpreter start-up dominates. Batched, the same work costs **0.40 ms** per
destination at one worker, **27.64 ms** at four and **35.62 ms** at six - the
same shape as the old loop, which cost 8.80, 635.27 and 972.58 ms at those
worker counts because it slept on every owner miss.

`tests/bench/provision_cost.py` re-measures that comparison:

```sh
tests/bench/provision_cost.py --destinations 256 --workers 1,4,6
```

Two behaviours in the client are load-bearing and tested in
`tests/bench/test_capacity_client.py`:

* **Owner-routed calls open a new connection per attempt.** A stream is served
  by one worker, chosen by hashing its name, and a `reuseport` listener hashes
  each new connection to choose the acceptor. A reused connection is pinned to
  the worker that already refused it, so retrying on one could never succeed.
  Ordinary calls reuse a connection deliberately.
* **Retries end at a deadline, not an attempt count.** A 404 means "the owner
  cannot see this stream yet" as often as "wrong worker".

The harness's receivers (`harness/receivers.py`) expect the destination ids
the client provisions, because a report, the receiver slot mapping and the
stall contract all key off them. `tests/bench/test_capacity_client.py`
compares the two schemes over every id, as part of `make bench-reporting`.


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

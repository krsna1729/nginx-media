# Codebase audit and implementation backlog

Audit baseline: `a3d145a` (`main`). This document records the codebase review
requested after the same-runner benchmark work. It is a tracked backlog, not a
claim that all fixes below have been implemented or every source line audited.

## Decision and implementation order

The next work is correctness and test reliability, not a broad rewrite or a
newer C dialect. Preserve the existing one-owner program model, bounded queues,
revision/incarnation checks, deferred teardown, platform seam and pinned
transport builds.

1. Routing and public-input correctness, with behavioral regressions.
2. Test-gate repairs: missing suites, fatal UBSan, header dependencies, shim
   fidelity and CI wiring.
3. Benchmark validity: identity, recipes, sampling windows, manifests and
   verdict evidence; repair weekly and distributed failures.
4. Testable extractions, stateful fuzzing and concurrency coverage.
5. Critical-section performance measurements and compiler/hardening lanes.

P1 means the next corrective work, before relying on public ingestion,
multi-worker behavior or performance alerts. P2/P3 work remains important but
is not an assertion of an independently reproduced exploit.

## Priority correctness and safety findings

- [x] **R1 / P1: preserve provisioned sources across routed connections.**
  `src/core/ngx_media_runtime.c:1677-1686` removes and recreates an existing
  source on OPEN; `ngx_media_stream.c:177-193` initializes a fresh source with
  zeroed credential/configuration fields and `enabled=1`. Routed CLOSE also
  removes the source and broadcasts deletion. Separate desired graph state
  from transport-session lifetime. Verify key, enabled state and revision
  survive routed reconnects and subsequent direct-owner connections.
- [x] **R2 / P1: use source/session-scoped routed identities.** Slot replacement,
  frame and CLOSE matching use program hash and stream incarnation, not
  publisher identity (`ngx_media_runtime.c:1710-1735,1896-1906,1960-1963`). Test
  two redundant sources routed through non-owner workers and independent CLOSE.
- [ ] **R2b / P2: explicit routed slot-exhaustion feedback.** The owner counts
  `routed_slot_overflows` but the publisher is neither refused nor told; its
  frames are silently ignored.
- [x] **S1 / P1: clean up rejected HLS upload bodies and admit before buffering.**
  At the baseline, three rejected 4096-byte uploads retained 12288 bytes.
  The handler now selects nginx clean file-only mode, checks URI/key admission
  before reading the body, and revalidates at completion. The integration suite
  verifies unknown/wrong-type keys, rejection before 100 Continue, rotation
  during an admitted upload, client abort, failed rename and successful atomic
  creation/replacement, with no retained temporary bodies.
- [x] **S2 / P1: close rejected SRT sessions, including idle sessions.** Unknown,
  empty or wrong-type keys only log and leave the OPEN handler
  (`src/srt/ngx_media_srt_module.c:2048-2078`); the transport has 16 slots.
  Process close requests independently of another incoming data event. Test
  idle invalid publishers, wrong-type keys and valid admission afterwards.
- [x] **S3 / P1: bound aggregate RTMP pre-authentication allocation and lifetime.**
  The reader permits 16 MiB per message and 64 chunk streams, allocating
  advertised capacity before publish admission (`ngx_media_rtmp_wire.c:744-750`).
  Potential allocated capacity is about 1 GiB per connection, not necessarily
  resident until data arrives. Add aggregate/pre-admission budgets and
  handshake/incomplete-message deadlines; test interleaved incomplete messages.
- [x] **S4 / P1: enforce the 64-worker routing ceiling before fixed-array loops.**
  `ngx_media_route.c:56-59` accepts a larger count; worker initialization and
  shutdown iterate that count at `:170-202,218`. Reject unsupported counts
  consistently, including `worker_processes auto` on large hosts.

Except S1, these are source-review findings: their proposed runtime scenarios
were not executed during the audit.

### Further correctness and hardening

- [x] **S5:** bounded HTTP response framing now rejects conflicting/invalid
  Content-Length and transfer encodings, premature EOF, malformed chunks/trailers
  and cap exhaustion. Actual HTTP/TLS tests pass, including reusable 204 responses.
- [x] **S6:** one monotonic budget covers DNS, address fallback, connect, TLS,
  transfer and complete response draining. Sticky cancellation is owner-closed.
  Actual DNS, dribble, blocked upload and HLS remove/stop scenarios pass.
- [x] **S7:** HLS pull logs use the shared credential redactor; RTMP destination
  logs identify the logical program, not its publish key. The actual HLS pull
  test failed before redaction and passes with opened/failing query secrets absent.
- [x] **S8:** reject non-regular file sources promptly; synchronous FIFO/device
  open/read can block an nginx worker (`ngx_media_file.c:99-100,173`).
- [x] **R3:** API/graph admission and decode share identity/path/scalar budgets.
  Native two-worker maxima and next-invalid atomic PATCH/replay scenarios pass;
  six simultaneous real publishers decode through independent encoded HLS paths.
- [x] **R4:** routing matrices are cycle-owned and generation-isolated; owner
  epochs fence old PID heartbeat/release/reclaim. Actual same-count reload,
  old CLOSE/EOF, reconnect, forced timeout and incomplete/failed TLS drain pass.
- [x] **R5:** recover from per-datagram IPC errors without permanently removing
  a peer read event; handle partial frame sends, sequence gaps and backpressure
  at a keyframe boundary.
- [x] **R6:** enforce stream/source/destination quotas against runtime and owner
  directory capacity; expose claim/admission failure rather than dropping it.
- [x] **R7:** preserve reader-backed source priorities on the owner, test failed
  graph admission and repair-journal saturation, and bound long-lived pool
  growth during source/destination/path churn.
- [ ] **D1:** provide a protected administrative API and deployment-specific
  resource/egress budgets. The existing systemd unit already has substantial
  isolation; do not add sandbox directives that break required media output.

## Unit, integration and fuzz testing

- [x] **T1 / P1: restore missing normal suites.** At the audit baseline, eight
  suites were omitted and three were TSan-only. The normal target now discovers
  all 34 standalone `test_*.c` suites, including record, stream and HLS ingest,
  under ASan/UBSan. TSan remains an additional separate run.
- [x] **T2 / P1: make UBSan reports fatal.** The configured default sanitizer
  flags now include `-fno-sanitize-recover=all` and retain frame pointers. A
  throwaway signed-overflow program using those actual flags exits one.
- [x] **T3:** track source/shim header dependencies in normal and TSan rules.
  A simulated `ngx_media_buffer.h` change now schedules recompilation.
  Instrumentation variants remain separate; common-object reuse is O5.
- [x] **T4:** the shim now leaves palloc/pnalloc storage uninitialized and keeps
  explicit poison intact; only pcalloc zeros its allocation. All 35 normal
  ASan/UBSan suites pass both ordinary and poisoned-allocation runs.
- [x] **T5:** replace source-text assertions and replicas in `test_security.c`
  with production behavioral tests. For example, finding `SSL_set1_host` in
  source does not prove actual wrong-host rejection. Delete replaced pins;
  do not repin wording after refactors.
- [x] **T6:** `make bench-reporting` now runs commit-selection tests, and
  `ingest-keys` is in the shared integration targets used by container CI.
- [x] **T7:** add per-file branch coverage plus an explicit inventory of production
  files excluded from unit builds. Establish a baseline before coverage floors.
- [x] **T8:** test routed session/graph/registry/owner-directory state transitions,
  SRT removal while sending, stream deletion while leased, HLS push queue/retry
  ownership and actual HTTP/TLS behavior. Use barriers for real cross-thread
  lifetimes, not concurrent calls to APIs whose contract is single-owner.
- [ ] **T9:** add valid PAT/PMT/PES, AVC/HEVC/ADTS, AMF and FLV fuzz seeds, stateful
  feed partitions, progress/allocation invariants and coverage-guided fuzzing.
  Deterministic seeds and partitions are implemented; coverage feedback and HEVC seeds remain.
- [ ] **T10:** isolate scratch directories/ports and inject time where useful.
  Add an ASan nginx integration lane for teardown/churn/ingest rather than
  assuming the unit shim represents nginx's event loop and allocator.

## Benchmark validity and critical-section performance

- [x] **B1 / P1: separate stable comparison identity from pressure counters.**
  `bench_history.py:235-240` copies the entire cgroup object, including
  cumulative cpu.stat; exact equality at `:456` rejects comparable runs.
  Real branch cohort `36882115060-attempt-1` had eight complete samples, eight
  fingerprints and one identity after cpu.stat removal. Artificially raising
  the current metric in a copied record to 1000000 yielded zero findings;
  removing counters from copied comparison identities yielded seven findings.
  Keep raw diagnostics and strict comparison_group isolation. Earlier claims
  that automatic issue detection was reliable are superseded by this finding.
- [x] **B2 / P1:** repair PR-to-main comparison: PR jobs fetch branch history,
  but Python filters it out because the tier is not `pr`. Compare only compatible
  recipes; PR and branch now share the 15-second default.
- [x] **B3:** store compatible recipes and content-bound compiler/flags, actual
  transport source commit, native receiver versions and binary provenance.
  Pinned artifacts record their actual checkout HEAD; fingerprinting validates
  artifact hashes and resolved linkage. Distribution libraries explicitly retain
  an unknown source commit rather than inheriting a configured pin. Native
  receiver compiler commands, tool/runtime versions and hashes survive history.
  Binary hashes remain evidence, not equality requirements across revisions.
- [x] **B4:** CPU and receiver byte boundaries retain endpoint/skew/coverage and
  independent absolute media anchors. Prepared captures exclude SSH startup
  without widening the 2% bound. Actual requested fifteen-second SRT/RTMP/HLS
  origin/push controls passed one/four rungs locally and over authenticated SSH;
  every protocol and remote CPU interval met the bound. Native receiver clocks
  and PID identities remain separate from controller brackets.
- [x] **B5:** use one workload set for manifest and execution. bench-ci's
  ALL_MIXES has seven names while harness `all` executes nine. Do not mask
  incomplete results by shortening the ladder or discarding failure evidence.
- [x] **B6:** floor passes, finite thresholds, complete intervals/shards/queues
  and successful absolute-rate calibration are required. The independent native
  16-receiver control passed full-rate source/TS checks; a single-peer read stall
  was receiver-limited only with qualified baseline and healthy-lane evidence.
  This is a controlled causal result, not an intrinsic receiver maximum.
- [x] **B7:** native authenticated loopback SSH passed receiver builds, fetched
  metadata/provenance, remote CPU/ownership accounting, fresh preflight and all
  four protocol one/four-rung controls. This is orchestration qualification,
  not a second-physical-host capacity result.
- [x] **B8:** repair weekly failed-build stubs: the weekly branch leaves `configs`
  unset but stub_summary references it under `set -u`.
- [x] **B9:** viewer series use stable runner identity, compatible recipe,
  qualification/probe cohort and a fixed rung. Exact record selection preserves
  repeated SHA attempts; hardware filters project mixed records. Publication
  and deployed-viewer behavioral checks pass; the real browser rendered the
  observed local history and selected the correct inspector record.
- [x] **B10:** per-rung raw artifacts retain preflight, calibration and attempts;
  immutable retry/history-race checks pass. Actual nightly run 38272651721
  collected all four configurations and trend evidence and published to
  `gh-pages`, preserving HLS setup failure as incomplete. Hidden `.build`
  uploads are explicit; publisher-wide FIFO queuing retains pending runs.
- [x] **B11:** the native critical-path driver covers references/feed, TS mux/
  demux, RTMP/AMF, IPC, registry/owner and egress reports. Separate timing/resource
  builds retain throughput, tails and observable allocation/copy counts. Actual
  three-repeat, 10000-sample rows ran on CPU 8 with a CPU-10 concurrent visitor;
  incomplete total allocation/copy counts are explicit, not estimated.
- [ ] **B12:** measure scheduler fairness under visit-budget exhaustion, metrics
  scraping during egress reports, routed reload/reconnect, redundant publishers,
  long-lived churn, loss, p99/p99.9 latency and queue overruns. The egress manager
  currently formats metrics while holding its global mutex. Calibrate noise
  and drift using repeated controls before adding blocking performance gates.

Live-site audit: branch/nightly charts were populated. Latest weekly cohort
`36911599344-attempt-1` had three samples and zero complete samples; seven
capacity panels showed no complete results. The weekly history/publish jobs
finished, but full qualification failed. This is not a successful weekly result.

## Code organization

- [ ] **O1:** extract canonical API JSON/path validation and response formatting
  from the 5600-line API module, leaving nginx request lifecycle separate.
- [ ] **O2:** separate routed-session handling and transform/executor wiring from
  the 2868-line runtime scheduler so the state machines can be tested directly.
- [ ] **O3:** separate case execution, measurement, verdict and workload selection
  in the 4287-line benchmark harness, preserving one recipe/manifest contract.
- [ ] **O4:** consolidate teardown, reader counts and feed defaults; share HTTP
  framing and stable benchmark identity rather than maintaining parallel rules.
- [ ] **O5:** reuse compiled common test objects within each compiler/sanitizer
  variant instead of recompiling the portable core separately for every suite.

Keep existing platform and transport seams. A generic plugin system, message
bus abstraction or build-system replacement is not justified by this audit.

## C standards and compiler ecosystem

The latest published standard checked is **C23 / ISO/IEC 9899:2024**. C2y is still
under development. The project already selects C23-family modes with gnu2x/c2x;
keep supported compiler baselines explicit before changing spellings/features.
Upstream stable versions checked were GCC 16.2 and LLVM/Clang 23.1.2. Local
verification used GCC 16.2.1 and Clang 22.1.8; no compiler was installed/upgraded.

- [ ] **C1:** use C23 checked arithmetic where it improves size calculations and
  static assertions for wire/layout/capacity invariants; feature-check supported
  toolchains. Both local compilers passed the checked-overflow smoke.
- [ ] **C2:** selectively extend nodiscard on significant status APIs, while
  reviewing explicit void discards and migrating all affected callers.
- [x] **C3:** generate a compilation database from the actual build and include
  compiler identity in build/configure invalidation. clangd navigation without
  the database did not reliably resolve the nginx allocator macro.
- [x] **C4:** retain CodeQL and add triaged GCC fanalyzer/Clang analysis lanes.
  Start MemorySanitizer with isolated core/parser targets because dependency
  instrumentation limits prevent treating it as a blanket production flag.
- [ ] **C5:** evaluate FORTIFY_SOURCE=3, stack-clash protection and target-appropriate
  control-flow protection with compatibility/performance checks. Compiler-flag
  verification and smoke inspection are automated; performance benchmarking remains.
- [ ] **C6:** add pinned modern compiler lanes while retaining supported distro
  lanes. Defer C2y, wholesale atomics replacement, blanket O3 and LTO/PGO rollout
  until behavior and measurement are trustworthy.

Primary references:

- [WG14 project status](https://open-std.org/jtc1/sc22/wg14/www/projects.html)
- [GCC C support](https://gcc.gnu.org/projects/c-status.html)
- [Clang C support](https://clang.llvm.org/c_status.html)
- [GCC releases](https://gcc.gnu.org/)
- [LLVM 23.1.2](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.2)
- [GCC instrumentation/hardening](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Instrumentation-Options.html)
- [MemorySanitizer limitations](https://releases.llvm.org/23.1.0/tools/clang/docs/MemorySanitizer.html)

## Audit verification and limits

Executed: fresh rebuild/run of the 23 default unit suites; all eight omitted
suites (484 passing checks); reporting tests (168 passing checks); commit
selection tests; local nginx rejected-upload reproduction; regression-identity
reproduction on real cohort records; sanitizer/C23 probes; ELF inspection;
and live-site browser verification. Disposable reproduction fixtures were
removed. No product source changes were made in the audit.

The other findings are source-derived, with proposed scenarios rather than
claimed runtime results. No full TSan/integration suite, sustained fuzz campaign,
public-endpoint attack or exhaustive security certification was performed.

## Implementation record

Completed work must be recorded here with verification, and its corresponding
checkbox above marked only after the behavior is exercised.

### Test-gate repairs (`6339dff`)

- Every normal unit suite is automatically discovered; all 34 passed under
  ASan/UBSan with fatal sanitizer diagnostics. The seven existing TSan suites
  also passed, including the HLS reader's separate implementation linkage.
- `make bench-reporting` passed 168 reporting checks and the commit-selection
  suite. The shared integration set now includes ingest-key verification;
  `make ingest-keys` passed against actual nginx.
- A dry-run simulated header update schedules `test_buffer` recompilation.
  A disposable signed-overflow executable built with the configured `SAN`
  value reports UB and exits one, rather than allowing a green process.
- Development documentation links this backlog, describes the restored suite
  set and records the still-unfixed shim-zeroing limitation accurately.

### HLS upload cleanup

- `make nginx` built the production module successfully. `make hls-ingest`
  exercised the new cleanup/admission regressions and existing media behavior:
  uploaded H.264 segments reached the program, the standard encoder delivered
  800 frames, and create/replace/delete responses and confinement still passed.
- The partial-upload checks waited for an actual spooled file before rotating
  the key or disconnecting the client, then required cleanup and no published
  partial object. A forced rename failure returned 500 with no retained body.
- `make ingest-keys` and `make smoke` passed against the rebuilt binary.
- Remaining routed-session, SRT/RTMP admission, benchmark comparability, shim
  fidelity and broader coverage work remains unchecked above; it is not
  implemented by these first repairs.

### SRT rejection teardown, routing worker bound, comparison identity

- S2: rejected sessions (empty, unknown or wrong-type key) are now closed by
  the ingest thread; close requests are processed every loop iteration rather
  than only when the session's socket is readable, and the session table lock
  is held only for fixed-size state handoff. `tests/integration/srt_admission_nginx.py`
  (`make srt-admission`) holds sixteen rejected idle callers open and requires
  a valid publisher to be admitted; it fails on the pre-fix binary with the
  valid source never coming up and passes now. It also covers source deletion
  of a zero-payload session, slot reuse, shutdown with an idle session, and
  credential redaction. A packet capture showed the server sends SRT shutdown
  within about a millisecond of each rejection; libsrt callers that receive it
  before their own connect completes stay `CONNECTED`, so the test deliberately
  does not use the caller's view as evidence.
- S4: `worker_processes` above 64 is rejected at configuration time and again
  in `ngx_media_route_master_init` before any array loop; the stale-fd close
  loops now treat fd 0 as valid and the matrix is initialized to -1.
  `make routing-bounds` rejects 65 and 128 workers and shows an invalid reload
  leaves the running master and workers serving.
- B1: `runner_identity()` keeps stable host facts (CPU, kernel, CPUs, governor,
  turbo, NUMA, transport, UDP limits, cgroup `cpu_max`) and excludes
  cumulative cgroup counters, for new and previously published fingerprints.
  `make bench-reporting` covers real-shape fingerprints with differing
  counters.

### Routed publisher identity and provisioned sources (R1, R2)

- IPC header version 3 carries `session`, a cross-worker-unique identity for
  one publisher connection (`ngx_media_route_session_id`: process id and the
  transport's session number). The owner's routed slot is keyed by program
  hash, stream incarnation and session; OPEN replaces only the slot of the same
  source, CLOSE and frames of a superseded connection match nothing, and a slot
  whose source the control plane deleted is dropped instead of dereferenced.
- A routed OPEN attaches to the provisioned source instead of removing and
  recreating it, applies the deployment selection policy and binds the source's
  health to the stream selector as a direct publisher does. CLOSE marks a
  provisioned source's transport down and keeps the source and key; only a
  source created by a routed OPEN (graph not yet delivered) is deleted.
- `make routed-sources` (`routed_sources_nginx.py`, two workers, publishers
  retried until they land on the non-owner): the provisioned source and key
  survive a routed disconnect and publish again; with two redundant routed
  sources the higher-priority one stays active on its own media and closing one
  leaves the other feeding. On the pre-fix source the first scenario fails with
  the source deleted and the second (run alone) with the active source
  receiving no media of its own.

### Routing resilience (R5)

- `ngx_media_ipc_recv` returns `NGX_DECLINED` for a malformed or oversized
  datagram it has already consumed (short, wrong version, length mismatch,
  transient allocation failure); the route handler counts it, logs at most once
  a second and keeps reading. `NGX_ERROR` is only a closed peer or a socket
  error, which still ends the read event, now with a log line.
- A frame's first chunk arriving while another frame is open discards the open
  one (`restarts`, exported as `nginx_media_runtime_routed_frame_restarts_total`)
  instead of refusing both.
- Each routed publisher has an `ngx_media_ipc_flow_t`: a failed hand-over makes
  it drop video until the next delivered keyframe (audio keeps flowing) and
  every frame, sent or dropped, advances `header.sequence`; the owner reports
  skipped numbers as `nginx_media_runtime_routed_sequence_gaps_total`.
- `test_ipc` covers the datagram cases, a valid message behind bad ones, a
  closed peer, an abandoned frame and the flow state machine; `make
  routed-sources` checks both counters are exported and the gap counter stays 0
  on a healthy route. Not covered: a real backpressured socket in nginx (no
  injection point), so the sender-side resync is verified by the state-machine
  unit test only.
- The Ubuntu CI namespace scenario assumed encoder-a stayed active during a
  shaped viewer transfer. It now waits for the preferred source again before
  taking the carrier down, and waits for active=encoder-b with an increased
  switch count in one API snapshot. `make netns` passed: carrier failure,
  recovery, severe-loss failover and bonded-path failure all retained media.
- Review regressions reproduced three failed assertions: a codec-config frame
  ended keyframe resync, and a different frame's continuation completed an
  abandoned payload. Config is now carried separately through the flow API;
  only a non-config video keyframe ends resync. Reassembly checks frame
  identity and media metadata before appending each chunk. `test_ipc` passed
  63 checks, and rebuilt `routed-sources` and `rtmp-workers` scenarios passed.

### File sources are regular files only (S8)

- A file source is opened `O_NONBLOCK` and `fstat`ed; anything but a regular
  file is refused with a log line and leaves no source behind. `make
  file-source` creates sources over a FIFO with no writer, `/dev/zero` and a
  directory: all are refused in under 3 s with the API still answering. On the
  pre-fix binary the FIFO request blocked the worker past the client's 5 s
  timeout.
- Rejection checks require HTTP 400 with `source_open_failed`, then fetch and
  parse the source list successfully before asserting the rejected id is absent.
  A failed GET no longer counts as evidence of cleanup. The tightened
  `make file-source` scenario passed against the rebuilt production binary.

### Benchmark reporting truth (B2, B3, B5, B6, B8; parts of B9, B10)

- B6: the gate needs a passing result at the tier's floor rung (the highest
  declared rung at or below `REQUIRED_RUNG`) for every expected workload; an
  infrastructure-limited, unknown, skipped or missing outcome at or below it is
  an error, not a notice. The SRT, RTMP, HLS push and HLS reader judges reject
  non-finite thresholds, references, intervals and sink/queue measurements: a
  NaN comparison is false both ways, so `--min-delivery-ratio nan`,
  `--interval-floor nan`, `--max-low-s inf`, a NaN reference or a NaN queue
  sample used to pass a half-rate rung. The harness stores a calibration
  reference only from a rung that passed its own quality checks and only if it
  is a finite positive rate (`0.00` matched the old number pattern).
- B5: `tests/bench/capacity-mixes.conf` is the single categorized workload list.
  The harness runs all nine for local `all`; CI weekly `all` selects the seven
  standard workloads and `contention` selects the two supplementary mixes.
  Contention calibrates at 4 and retains every requested higher rung.
  The harness prints `quality_mixes=` and `quality_recipe`; `completeness()` requires the
  mixes and recipe steps to equal the manifest. A harness that never states
  its mixes is incomplete.
- B8: `bench-trend.sh` stub summaries take the single `config_names` list (the
  weekly branch used to leave `configs` unset under `set -u`).
- B3/B2: each configuration's record carries its recipe (seconds, source rate,
  rungs, mixes, stop-after rule, judge thresholds, worker settings, harness
  revision = hash of the harness files). `regressions()` compares only equal
  recipes (ladders may differ if they start at the same rung; a record without
  a recipe compares with nothing). A pull request is compared with main's
  `branch` records (`load_history` and `regressions` both dropped them as
  `tier != pr`). Compiler, configure arguments, binary sha256 and transport are
  recorded per workload as provenance and never compared. `summarize()` also
  discarded `run_environment()`'s lifted copy, so published records never
  carried the fingerprint or peaks regressions need; it is kept now.
- B9/B10 (partial): the scorecard sparkline follows the card's rung across
  runs with the same runner identity and a comparable recipe; `rung_record`
  keeps the preflight verdict and the per-protocol calibration reference.
- Evidence: `make bench-reporting` 322 checks, 0 failures. Manifest-step drift
  failed before the additional check (`bench_gate=pass`) and is now rejected.
  The cases use judge fixtures, the real `bench-ci.sh` and `bench-trend.sh` in scratch trees
  and the harness's own mix-selection function with the ladders stubbed. Run
  against the pre-change tree (`git archive origin/main` plus the new test
  file) they fail: floor, judges, calibration, mix list, manifest, recipe,
  PR-history and weekly-stub cases, the weekly one with
  `bench-trend.sh: line 49: configs: unbound variable`.
- Real pipeline smoke: `scripts/bench-ci.sh branch` with `BENCH_CONFIGS=srt`,
  `BENCH_STEPS="1 16"` and `BENCH_SECONDS=10`, run through `omarchy-benchmark`:
  sender on P-cores 2,4, publisher on P-core 6, receivers on E-cores 12-15.
  Both receiver-verified rungs passed and `bench_gate=pass`; this verifies
  reporting, not a new capacity ceiling.
- Browser smoke used the changed viewer against published legacy records,
  then three comparable recipe-bearing trend fixtures with changed cumulative
  CPU counters and one incompatible-duration fixture. All three scorecard
  sparklines rendered values 110,120,130; the incompatible sample was excluded.
- At this earlier checkpoint, B3 transport/tool provenance, B6 receiver controls,
  B9 chart filtering and B10 evidence/retry/publication behavior remained open.
  Later native and GitHub qualification below records their completed checks.
- Weekly qualification and trend use a distinct contention manifest and ladder.
  Rungs below 4 cannot form these mixes and are inapplicable, rather than
  measured or quality-failure outcomes. Explicit malformed ladders remain
  harness setup errors; the local `all` selection still includes all nine.
- PR defaults now use branch's 15-second duration and honor `BENCH_SECONDS`;
  old 10-second recipes remain incomparable with the 15-second defaults.

### RTMP pre-admission budget (S3)

- The chunk reader tracks the payload it holds for unfinished messages
  (`buffered`, bounded by `max_buffered`); a message that would exceed the sum is
  refused before allocation. The default stays 2 x 16 MiB for media. Until a
  publish key is accepted or a play starts, a connection is held to 64 KiB
  messages and 64 KiB held in total, and to a 30 s deadline from accept.
  Worst case before admission is therefore 64 KiB x 1024 sessions, down from
  about 1 GiB a connection.
- `test_rtmp_wire` covers the aggregate budget (two open messages counted, a
  third refused, a completed message returned to the budget). `make
  rtmp-preauth` against real nginx: a 4000-byte command is waited for, a
  100000-byte message and six 30000-byte unfinished messages are refused, and a
  handshaken but silent connection is dropped at the deadline; the oversize
  case fails on the pre-fix binary.

### Benchmark recipes and complete sampling evidence

- Weekly keeps all seven established workloads in `all`, and both supplemental
  mixes in `contention`. Contention calibrates at four destinations, then runs
  every requested higher rung. Below-four rungs are inapplicable to these
  proportions; malformed explicit ladders remain errors. Measurement,
  publication and historical failed-build manifests all name both configs.
- PR and branch use the same default duration, with matching overrides
  supported. Actual branch and PR SRT summaries recorded 15-second rungs,
  prefix ladders and compatible recipes; old unequal-duration recipes are
  still excluded, not reinterpreted.
- RTMP validates raw intervals before combining short jitter samples, and
  requires interval evidence plus timestamped receiver snapshots. SRT requires
  complete destination, active-shard and queue-window evidence. Invalid data
  emits `quality_measurement_valid=no` and returns a setup failure; complete
  zero-delivery evidence remains a quality failure. Invalid SRT evidence cannot
  claim strict full rate. Existing infrastructure verdicts remain separate;
  no receiver-saturation conclusion is inferred from CPU usage alone.
- Samplers acknowledge readiness before accounting. Opening/closing native
  captures dispatch concurrently with CPU releases; quiescence finishes in-flight
  samples before closing counters. Joins/fetches run after accounting closes,
  and cleanup stops any remaining monitors.
- Reproductions: five-second RTMP sampling gaps and empty SRT sender metrics
  previously passed; both now fail as invalid measurements. Omitting RTMP
  interval evidence also previously bypassed validation and is now rejected.
  An actual-process sampler test showed the old stop-all helper killed SRT
  when asked to stop RTMP; the revised helper preserves SRT and propagates a
  failed monitor's exit status.
- Verification: `make bench-reporting` passed 375 checks plus commit-selection
  checks; the nine workflow permission contracts passed. Actual smoke runs
  passed the following receiver-verified rungs:

  | Workload | Passing smoke rungs |
  |---|---|
  | Pure SRT, branch recipe | 1, 16 |
  | Pure SRT, shipped PR recipe | 1, 16, 32 |
  | Pure RTMP | 1, 16 |
  | Both contention mixes | 4, 8 |
  | HLS origin and HLS push | 1, 4 |

  Sender CPUs were P-cores 4,6; publisher CPU was P-core 10; receivers used
  E-cores 12-15. Another benchmark reserved CPU 2 and the governor helper, so
  these runs used affinity only and made no machine-policy changes. They
  verify reporting behavior, not a new capacity ceiling or CPU-efficiency
  comparison. Raw results are under `.build/reliability-*`.

### Native receiver controls, provenance and publication

- `make bench-reporting` passed 473 reporting checks, commit selection, seventeen
  receiver ownership/capture checks, five causal/lifetime controls, the isolated
  native transport-provenance build and immutable publication/viewer checks.
- A native independent SRT source qualified sixteen receivers at constant
  8 Mbit/s each for ten seconds. The paired `d0000` read stall was classified
  receiver-limited only with full-rate source and healthy-lane evidence.
  This proves a controlled failure cause and recipe lower bound, not an
  intrinsic receiver maximum or production capacity ceiling.
- Real authenticated loopback SSH compiled receiver tools natively, fetched
  compiler/runtime fingerprints and passed fresh path/CPU preflight. SRT,
  HLS-origin and HLS-push passed fifteen-second rungs at one and four
  destinations. RTMP's original four-destination rung failed in its availability
  probe; a kernel smoke demonstrated TIME_WAIT false rejection. Matching SSH's
  socket-reuse semantics then passed a fresh RTMP one/four-rung replay.
  Original failure evidence is retained, not replaced by the replay.
- The original SSH runs correctly withheld CPU/Gbit when capture startup skew
  exceeded the 2% bound. A fresh prepared-RPC run passed all eight quality and
  protocol/remote-CPU alignment checks at the unchanged requested fifteen seconds.
  Maximum protocol skew was 103.206 ms; remote CPU skew was 3.820 ms. A matching
  local eight-rung run passed with maximum protocol skew 163.178 ms.
  Complete raw runs, original failures, native controls and the frozen tested
  source are archived under `docs/capacity-evidence/2026-10-12-*`, with content
  hashes and qualification limits in the checkpoint JSON.
- Default CI keeps live raw evidence inside its artifact root; explicit runtime
  roots retain transport mappings. Publishers preserve attempts and incomplete
  evidence, deduplicate content-identical retries and replay after history races.
  Local publication behavior and actual viewer surface were exercised.
  Nightly run 38272651721 also exercised GitHub artifact upload/download and
  history push; the publisher passed while preserving the failed HLS preflight.
  All six artifacts are archived with their hash in `capacity-results.md`.

### HTTP, identity paths and faithful allocator qualification

- All 35 unit suites passed ASan/UBSan in ordinary and poisoned-allocation modes;
  actual HTTP/TLS/DNS tests now pass 1203 checks in each allocator mode.
  The ordered-address DNS test encounters a real stalled first connect before
  succeeding at the second address within the original total budget.
- Actual HLS pull failed before credential redaction and passed afterward.
  HLS push conformance passed generic and YouTube receivers, reconnect/retry
  budgets, ordered uploads and forward progress after failed segments.
- Native graph-boundary checks round-tripped 256-byte identities, 4096-byte
  source path/CA fields and uint32 priority across owner and replica. Invalid
  boundaries, mixed PATCH and replay left state unchanged; spooled bodies used
  the same admission budget. Six concurrent paced publishers produced distinct
  HLS paths with independently matched decoded color-frame SHA256 hashes.
- The standalone native routing lifecycle passed generation isolation, draining
  descriptor copies, atomic allocation failure and bounded descriptor counts.
  Actual reload exposed early RTMP source-worker exit, then connection-pool
  destruction exposed registry-string aliasing. Session retention/full teardown
  and immutable stream-owned names now pass all native reload/TLS scenarios.
- The native critical-path smoke produced 66 observations: eleven operations
  times three repetitions in each of separate timing/resource builds.
  Linker-wrap counters cover observable calls only; complete allocation/copy
  totals remain unavailable. No production speedup or capacity ceiling is claimed.
- Remaining security-suite source pins and copied validators/rings were removed.
  The retained production parser/storage regressions pass 47 checks in both
  allocator modes; deletion does not count as new coverage of the removed models.

### Production quotas, compiler lanes, coverage baseline and parser fuzzing

- Production quotas enforce finite `media_stream_limit` (up to 256, matching
  the shared owner-directory ceiling), `media_source_limit` and
  `media_destination_limit` per worker. Bounded model rooms refuse objects
  with 503 before mutation; removed objects retain their allocation until the
  last reader/lease/holder releases it. The native churn fixture verified 2,560
  file cycles, 160 ingest cycles, 80 pull cycles, 640 destination cycles and
  1,280 path replacements; retained resident memory plateaued within the 8 MiB
  allowance.
- Owner-directory mutations (claim, heartbeat, release) enforce generation
  epoch fences against obsolete attached directories. In native multi-worker
  routing, 256 real directory records were admitted on worker 3, direct and
  routed publishers refused at the 257th record, and natural stale reclaim
  recovered capacity across reload after closing old-generation publishers.
- Per-file branch coverage reports 7,565 / 9,716 lines hit (77.9%) and
  4,094 / 6,844 branches hit (59.8%) across 39 production C files. An explicit
  inventory identifies the 13 production files excluded from standalone unit
  linking.
- The unit parser fuzz suite incorporates valid MPEG-TS seeds (PAT, PMT, AVC
  keyframe, AAC ADTS) generated through production muxer code and FLV publisher
  seeds (avcC and AudioSpecificConfig sequence headers). Stateful feed
  partitions verify unmutated progress across 1- to 512-byte slices and survive
  72,082 mutated rounds with zero memory leaks under allocation poisoning.
- Compiler analysis lanes verify 52 production translation units under GCC 16
  SARIF and Clang plist analyzers without diagnostic suppression. Nine exact
  fingerprints are triaged in `scripts/analyzer-triage.json` with trace-backed
  invariants. The isolated MemorySanitizer lane passes all eight parser/core
  suites without uninstrumented third-party library linkage. Real compiler flips
  rebuild native objects and unchanged configurations hit the object cache.


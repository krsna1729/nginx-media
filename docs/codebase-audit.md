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

- [ ] **S5:** implement bounded HTTP response framing for GET and upload:
  Content-Length, chunked encoding, premature EOF, cap/timeout truncation and
  conflicting framing headers (`src/core/ngx_media_http.c:499-548`).
- [ ] **S6:** carry a total monotonic deadline through HTTP operations and make
  active operations cancellable; idle socket timeouts alone do not bound
  dribbling responses, DNS or shutdown joins.
- [ ] **S7:** apply the existing credential redactor at lower-level HTTP, HLS
  pull and RTMP destination logging sinks, including reconnect/failure paths.
- [ ] **S8:** reject non-regular file sources promptly; synchronous FIFO/device
  open/read can block an nginx worker (`ngx_media_file.c:99-100,173`).
- [ ] **R3:** align API/graph encoder limits with decoder limits; prevent distinct
  program names colliding after HLS directory sanitization.
- [ ] **R4:** isolate routing socket pairs between worker generations and test
  owner-directory PID takeover after reload.
- [ ] **R5:** recover from per-datagram IPC errors without permanently removing
  a peer read event; handle partial frame sends, sequence gaps and backpressure
  at a keyframe boundary.
- [ ] **R6:** enforce stream/source/destination quotas against runtime and owner
  directory capacity; expose claim/admission failure rather than dropping it.
- [ ] **R7:** preserve reader-backed source priorities on the owner, test failed
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
- [ ] **T4:** match nginx allocator semantics in the shim: palloc/pnalloc do not
  zero memory, pcalloc does. Current zeroing also overwrites pool poison mode;
  the documentation's "stricter than production" claim is incorrect.
- [ ] **T5:** replace source-text assertions and replicas in `test_security.c`
  with production behavioral tests. For example, finding `SSL_set1_host` in
  source does not prove actual wrong-host rejection. Delete replaced pins;
  do not repin wording after refactors.
- [x] **T6:** `make bench-reporting` now runs commit-selection tests, and
  `ingest-keys` is in the shared integration targets used by container CI.
- [ ] **T7:** add per-file branch coverage plus an explicit inventory of production
  files excluded from unit builds. Establish a baseline before coverage floors.
- [ ] **T8:** test routed session/graph/registry/owner-directory state transitions,
  SRT removal while sending, stream deletion while leased, HLS push queue/retry
  ownership and actual HTTP/TLS behavior. Use barriers for real cross-thread
  lifetimes, not concurrent calls to APIs whose contract is single-owner.
- [ ] **T9:** add valid PAT/PMT/PES, AVC/HEVC/ADTS, AMF and FLV fuzz seeds, stateful
  feed partitions, progress/allocation invariants and coverage-guided fuzzing.
  Keep deterministic regressions; uniform random TS primarily rejects input.
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
- [ ] **B2 / P1:** repair PR-to-main comparison: PR jobs fetch branch history,
  but Python filters it out because the tier is not `pr`. Compare only compatible
  recipes; PR/branch duration currently differs.
- [ ] **B3:** store seconds, source rate, rungs, mixes, stop rules and harness
  revision; compare compatible recipes. Record compiler/flags, actual SRT
  commit, receiver versions and binary provenance. Binary hashes are evidence,
  not an equality requirement across the code revisions under comparison.
- [ ] **B4:** align CPU and receiver byte sampling boundaries and report window
  skew; validate absolute expected workload as well as relative delivery.
- [ ] **B5:** use one workload set for manifest and execution. bench-ci's
  ALL_MIXES has seven names while harness `all` executes nine. Do not mask
  incomplete results by shortening the ladder or discarding failure evidence.
- [ ] **B6:** require positive evidence of floor-rung passes; reject non-finite
  thresholds, malformed/missing measurement inputs, sampling gaps and failed
  calibration references. Distinguish receiver saturation from nginx failures
  with controlled receiver-capacity evidence.
- [ ] **B7:** repair the distributed path: fetched receiver metadata, remote PID
  and CPU accounting, fresh ready/probe files and fatal preflight errors.
- [ ] **B8:** repair weekly failed-build stubs: the weekly branch leaves `configs`
  unset but stub_summary references it under `set -u`.
- [ ] **B9:** make viewer series/sparklines use stable identity and a fixed rung;
  fix profile comparability, exact run selection and mixed-record filters.
- [ ] **B10:** preserve per-rung artifacts, preflight verdicts and calibration
  provenance; handle duplicate publication/run attempts and pending publishers.
- [ ] **B11:** add one small critical-section benchmark driver: buffer refs,
  feed publish/read, mux/demux, RTMP parse, IPC framing/reassembly and registry/
  owner-directory operations. Report throughput, allocations/copies and tails;
  do not introduce a framework or compare instrumented and production timings.
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
- [ ] **C3:** generate a compilation database from the actual build and include
  compiler identity in build/configure invalidation. clangd navigation without
  the database did not reliably resolve the nginx allocator macro.
- [ ] **C4:** retain CodeQL and add triaged GCC fanalyzer/Clang analysis lanes.
  Start MemorySanitizer with isolated core/parser targets because dependency
  instrumentation limits prevent treating it as a blanket production flag.
- [ ] **C5:** evaluate FORTIFY_SOURCE=3, stack-clash protection and target-appropriate
  control-flow protection with compatibility/performance checks. The inspected
  artifact already had PIE, RELRO, immediate binding and a non-executable stack.
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

# Capacity results

Measured results of the receiver-verified capacity ladders
(`make bench-capacity-quality`), with the conditions they were taken under.
A capacity number from one host is not a capacity number for another: the
boundary rung moves with the CPU a host gives the sender *and* the receivers.
**Sender CPU per delivered Gbit/s** travels much better than a destination
count, and with the attribution of the first failure (sender, receiver,
kernel) it is what these results compare.  It is not host-independent: CPU
microarchitecture and frequency, kernel, SRT library version, compiler and
IRQ/NIC topology all move it, so it is compared within one host class.  Every row below
comes from a rung's `diagnostics.json`; the method is in
`development.md` ("Per-rung diagnostics and outcomes").

## Reporting corrections (2026-09-26)

Two reporting defects were fixed in the pipeline that produces these tables.
Neither changes what was measured; both change what the measurement is called.

- **A configured threshold was published as an observed ratio.**  The HLS
  reader benchmark printed the acceptance threshold (`--min-delivery-ratio`,
  0.95) in `min_delivery_ratio`, and the diagnostics and history layers read
  that field as the rung's delivery ratio.  Every HLS-origin row in the
  phase-5 evidence files therefore carries `"min_delivery_ratio": 0.95` — the
  gate, not a result — and no observed reader ratio at all.  Those records
  are left exactly as they were written; `bench-history/2` records separate
  `observed_delivery_ratio` from `delivery_ratio_threshold` and add
  `delivery_ratio_basis`, so an old record's basis reads `observed-for-...`
  and its HLS-origin workload is listed as unmeasured rather than as 0.95.
  New runs report the readers' own minimum, p5/p50/p95, aggregate receiver
  bytes, the receiver-side measurement interval and aggregate delivered
  Gbit/s.
- **HLS-origin traffic was absent from CPU per delivered Gbit/s.**  The
  efficiency section summed SRT, RTMP and HLS push only, so a pure HLS-reader
  rung reported no delivered rate.  HLS origin now contributes its
  reader-counted bytes over the readers' own interval, which is why the
  "Pure HLS origin (readers)" row in the phase-5 table below has dashes: that
  run predates the fix.

The phase-5 numbers for the other six workloads are unaffected: their reports
already carried receiver-counted bytes and their own measurement interval.

## Phase 2 — pure SRT diagnosis (2026-09-25)

### Conditions

| | |
|---|---|
| Host | cloud container, 4 vCPU Intel Xeon @ 2.10 GHz, 15 GiB, no SMT, steal 1.25% |
| CPU placement | unpinned; nginx, the SRT receiver and the publisher share all 4 CPUs |
| cgroup quota / frequency | not readable in this container (recorded as unavailable) |
| Kernel | 6.18.44, `net.core.rmem_max`/`wmem_max` 4 MiB |
| nginx | 1.30.5, 4 workers, one program owned by w0 |
| SRT | libsrt 1.5.3 (Ubuntu 24.04); robotweax/srt 0.2.5 (`main`, built from source) |
| Source | 8 Mbit/s 720p25 H.264 TS, 30 s loop; reference 8.807 Mbit/s receiver payload (calibrated at 1 destination) |
| Rung | 20 s measurement; pass = every destination ≥ 0.95 of reference, 1 s floor 0.80 for ≤ 2 s, zero MPEG-TS errors and application drops |
| Receiver | `srt_fanout_sink`, one listener per 16 destinations |
| Caveat | the matrix ran as root, so nginx's workers ran as `nobody` and could not create the HLS directory: segments were prepared in memory but their file writes failed.  This holds for every configuration alike, so the comparison stands, but HLS disk I/O is not in these numbers.  Later runs use an unprivileged user, as CI does. |

### Configurations

| Name | Binary | SRT senders | Notes |
|---|---|---|---|
| base-adaptive | `16b9549` (before this work) | adaptive | one libsrt multiplexer per destination |
| new-adaptive | lane multiplexer groups | adaptive | |
| new-fixed1/2/4 | lane multiplexer groups | fixed 1, 2, 4 | `media_egress_workers srt N` |
| rw-adaptive | same source, linked against robotweax/srt | adaptive | |
| new-nohls | lane multiplexer groups | adaptive | HLS preparation off — exposed a defect, see below |

### Highest passing rung

| Configuration | 1 | 32 | 64 | 96 | 128 | 160 | Highest pass |
|---|---|---|---|---|---|---|---|
| base-adaptive | pass | pass | fail 0.940 | fail 0.557 | fail 0.404 | fail 0.305 | **32** |
| new-adaptive | pass | pass | pass | pass | fail 0.940 | fail 0.630 | **96** |
| new-fixed1 | pass | pass | pass | pass | fail 0.954 | fail 0.482 | **96** |
| new-fixed2 | pass | pass | pass | pass | fail 0.970 | fail 0.622 | **96** |
| new-fixed4 | pass | pass | pass | pass | fail 0.975 (1 s floor 0.698) | fail 0.794 | **96** |
| rw-adaptive | pass | pass | pass | pass | pass 0.995 | fail 0.525 | **128** |

Failing cells show the minimum per-destination average delivery ratio.

### Where the CPU and the packets went

`%` is percent of one core over the measurement window; "workers" is all four
nginx workers; "SndQ/RcvQ" are libsrt's multiplexer threads in the owning
worker; drops are kernel UDP socket drops attributed by process.

| Config | Dest | Workers | App senders | SndQ | RcvQ | Receiver | Retrans | Receiver drops | nginx drops | Lag max ms | Sender %/Gbit/s | Host busy | Threads w0 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| base | 32 | 98 | 23 | 50 | 9 | 59 | 276 | 0 | 0 | 7 | 350 | 0.43 | 90 |
| base | 64 | 184 | 43 | 107 | 20 | 142 | 25 152 | 28 218 | 0 | 689 | 342 | 0.87 | 154 |
| base | 96 | 209 | 46 | 125 | 24 | 103 | 773 961 | 306 392 | 0 | 8 090 | 389 | 0.93 | 218 |
| base | 128 | 188 | 39 | 98 | 36 | 145 | 203 060 | 8 206 | 0 | 7 401 | 360 | 0.89 | 282 |
| new | 32 | 67 | 15 | 32 | 6 | 40 | 0 | 0 | 0 | 38 | 238 | 0.88* | 58 |
| new | 64 | 92 | 20 | 45 | 11 | 87 | 13 820 | 0 | 0 | 17 | 164 | 0.46 | 58 |
| new | 96 | 122 | 25 | 64 | 17 | 162 | 289 | 0 | 0 | 96 | 145 | 0.76 | 58 |
| new | 128 | 137 | 31 | 74 | 17 | 194 | 113 189 | 97 324 | 0 | 850 | 126 | 0.87 | 58 |
| fixed1 | 96 | 137 | 29 | 77 | 18 | 147 | 29 678 | 0 | 0 | 94 | 163 | 0.75 | 58 |
| fixed2 | 96 | 107 | 21 | 59 | 15 | 141 | 21 052 | 0 | 0 | 151 | 128 | 0.65 | 58 |
| fixed4 | 96 | 93 | 17 | 51 | 13 | 105 | 18 708 | 0 | 0 | 17 | 110 | 0.53 | 58 |
| fixed4 | 128 | 102 | 22 | 54 | 14 | 135 | 30 825 | 0 | 0 | 416 | 92 | 0.63 | 58 |
| rw | 96 | 119 | 25 | — | — | 152 | 4 | 0 | 0 | 40 | 141 | 0.72 | 28 |
| rw | 128 | 129 | 30 | — | — | 163 | 128 | 0 | 0 | 19 | 114 | 0.77 | 28 |

\* the 32-destination new-adaptive rung overlapped a unit-test build on the
same host; its host-busy figure is not representative.

### Findings, against the roadmap's decision table

1. **The application sender was never the bottleneck.**  One sender thread
   carried 96 destinations at 29% of a core (`new-fixed1`), and fixed 1, 2
   and 4 senders all pass exactly the same rungs.  The roadmap row "application
   sender approaches one saturated core" does not apply; adding sender threads
   does not raise the boundary.
2. **libsrt's per-destination multiplexer was.**  Every unbound caller socket
   got its own UDP socket plus `SndQ`/`RcvQ` threads: 154 threads at 64
   destinations, 346 at 160, with `SndQ` alone at 107–139% of a core.  Row:
   "application sender lightly loaded but libsrt `SndQ` saturated → profile
   libsrt transport processing and session overhead".  A sampled-stack
   profile of the `SndQ` threads (64 destinations x 8 Mbit/s, identical
   bytes delivered; `capacity-evidence/2026-09-26-sndq-profile.json`) shows
   what that cost is: with a multiplexer per destination, 61.7 of the 90.4%
   of a core the 65 `SndQ` threads used was context switching and wakeups
   (`finish_task_switch` alone 40% of their time), against 11.0 in the send
   path itself.  With lane groups the 17 `SndQ` threads used 38.8%: 19.1 in
   wakeups, 9.0 sending - 42.6 of the 51.6 points saved came off the wakeup
   path, while the send path, carrying the same bytes, barely moved.  The fix is
   structural rather than a tuned buffer: destinations connect in their
   lane's multiplexer group
   (commit `dcdae24`).  Sender CPU per Gbit/s fell from 342–389% to 126–164%,
   threads stayed at 58 whatever the fanout, and the highest passing rung went
   from 32 to 96.
3. **The next limit on this host is the measuring receiver.**  At 128
   destinations (new-adaptive) every dropped datagram — 97 324 — was on the
   receiver's sockets, none on nginx's, with the receiver at 194% of a core
   against the sender's 137% and the host 87% busy.  libsrt's receiver runs a
   `TsbPd` thread per session.  Row: "receiver CPU or kernel network path
   saturates first → change benchmark topology; do not modify sender
   scheduling".  On a 4-CPU host the only topology change left is separate
   sender and receiver machines (or pinning them apart, which halves both).
4. **Adaptive vs fixed.**  Adaptive settled on 1–2 active senders, fixed 4 used
   the least CPU per Gbit/s (92–117%) and came closest at 128 (zero drops,
   0.975 average, 1 s floor 0.698).  Repeated at 96 destinations
   (`capacity-evidence/2026-09-26-srt-senders.json`), sender CPU per
   delivered Gbit/s was 128% of a core with 1 sender, 106% with 2, 97% with
   4, 8 or 16 - flat from one sender per CPU on - and 122–127% adaptive,
   which held 1–2.  Every configuration delivered the same 0.84 Gbit/s; what
   more senders buy is parallel submission to the lane multiplexers, not
   capacity.  The manager grew senders only on backpressure, which a lane
   group absorbs, and counted every other engine's thread as a whole CPU
   against `cpus - 1`.  It now runs one sender per lane in use, up to the
   CPUs the worker is granted (its affinity and cgroup `cpu.max`, read at run
   time), and counts other engines by the CPU they use.  After the change,
   in one session: adaptive ran 4 senders at 104–109%, fixed 4 at 107% (the
   host was noisier than for the first series - compare within a session).
5. **robotweax/srt** has no per-socket threads (28 in the worker at every
   rung), almost no retransmission, and passed one rung higher (128) at the
   same sender CPU per Gbit/s as libsrt with multiplexer groups (114–150%).
   Its receiver-side cost is not measured here: the receiver stays on libsrt
   so both runs share one instrument.
6. **HLS off exposed a defect, not a number.**  With `CAPACITY_SRT_HLS=no` the
   first rung was a setup failure: SRT destinations never received a burst,
   because the shared TS preparation existed only when HLS, recording or a
   transform was configured.  Fixed in `1da6f04`; `make srt-output-mux` (no
   `media_hls`) now carries media to 40 destinations.

### Correctness after the change

| Check | Result |
|---|---|
| `make unit` (ASan + UBSan) | 20 suites, 0 failures (`test_srt_output`: 248 checks incl. lane groups and fallback) |
| `make srt-output-mux` (real libsrt, no `media_hls`) | 16 library send threads for 40 destinations; same after deleting and re-adding all 40; churn of 5 while the rest run; all carry media |
| `make tsan` (unprivileged user) | pass (after allowing TSan's own background thread in one start count, `bcd0843`) |
| Integration, branch, unprivileged user | pass: srt-output, srt-output-mux, srt-crypto, churn, stream-delete, incarnation, failover, multi-worker, rtmp, rtmps, rtmp-workers, hls, hls-push, fault, source-switch (after its stale link list was fixed, `c01da5e`) |
| Integration failures | `srt-ingest-nginx` ("deleting the source did not close its session") and `api-graph` ("the seventeenth output stream was admitted: 201") fail identically on the unmodified baseline `16b9549` as the same user - pre-existing, not addressed here.  One `api-graph` run tripped its thread-steadiness check with a *decrease* (31 -> 30); it did not recur, and the baseline fails earlier than that check can say anything |

Running the suites as root in a container makes nginx drop its workers to
`nobody`, which then cannot create HLS directories under a root-owned build
tree: srt-output, srt-crypto, stream-delete and api-graph all fail that way
and pass as an ordinary user.  CI runs as an ordinary user.

## Phase 4 — timing and accounting validation (2026-09-25)

Same host, now as an unprivileged user (HLS files are written), 30 s rungs.
Per-rung summary: `capacity-evidence/2026-09-25-phase4-validation.json`.

### RTMP: rates over the interval that brackets the counters

| Destinations | Result | Min ratio | Receiver-scrape interval | Shell window | 1 s floor (min) | Longest stall | Delivered | Sender CPU |
|---|---|---|---|---|---|---|---|---|
| 1 | pass | 1.000 | 30.171 s | 30.220 s | 0.844 | 0 s | 8.5 Mbit/s | 6% |
| 16 | pass | 1.001 | 30.172 s | 30.210 s | 0.945 | 0 s | 136 Mbit/s | 7% |
| 64 | pass | 1.001 | 30.179 s | 30.240 s | 0.904 | 0 s | 544 Mbit/s | 9% |

The rate now divides by the monotonic interval between the two receiver
scrapes (`quality_timing=receiver-scrape-monotonic`); the shell window it used
before is kept in the report for comparison and differs by 40–60 ms here.
For scale: RTMP carries 0.54 Gbit/s on 9% of a core (about 17% per Gbit/s)
where SRT needs 120–160% per Gbit/s — TCP's kernel segmentation against
SRT's user-space per-packet pacing.

### HLS push: identified segments instead of window bytes

| Destinations | Result | Segment ratio min | Window byte ratio min | Window segments | Lag p95 / max | Missing | Duplicate | Playlist-before-segment | Uploaders |
|---|---|---|---|---|---|---|---|---|---|
| 1 | pass | 1.000 | 1.000 | 5 (4.0 s) | 0 / 0 s | 0 | 0 | 0 | 1 |
| 16 | pass | 1.000 | 0.9996 | 5 | 0.096 / 0.111 s | 0 | 0 | 0 | 1 |
| 64 | pass | 1.000 | 0.9993 | 5 | 0.288 / 0.370 s | 0 | 0 | 0 | 1 |

The gate is the delivered share of the exact segments first delivered in the
window (lag limit twice the observed segment interval, 8 s).  The uploader
pool stayed at one thread while its queues drained.

### SRT with HLS preparation off (`CAPACITY_SRT_HLS=no`, after `1da6f04`)

| Destinations | Result | Min ratio | Workers | SndQ | Receiver | Receiver drops | Sender %/Gbit/s |
|---|---|---|---|---|---|---|---|
| 1 | pass | 1.000 | 18 | 1 | 2 | 0 | — |
| 64 | pass | 0.999 | 80 | 42 | 93 | 0 | 143 |
| 96 | pass | 0.997 | 122 | 69 | 166 | 0 | 146 |
| 128 | fail | 0.961 | 129 | 71 | 183 | 87 368 | 117 |

Same boundary (96) and the same CPU per Gbit/s as with HLS on: the shared
TS/HLS preparation does not contribute materially to the SRT boundary.  At
128 the drops are again all on the measuring receiver's sockets.

## Phase 5 — all seven workloads requalified (2026-09-25)

Same host, unprivileged user, 8 Mbit/s source, 30 s rungs on the ladder
1/32/64/96/128/192/256, a workload's ladder stopping after two consecutive
failures.  Every workload passes at least as far as it did before this work;
the ladder's top (256) is this host's rig limit for the receivers, not a
boundary found in nginx.  Per-rung summaries:
`capacity-evidence/2026-09-25-phase5-all-workloads.json` and
`capacity-evidence/2026-09-25-phase5-rtmp-sharded-receivers.json`.

| Workload | Highest pass | First failure | Delivered at the highest pass | Sender CPU per Gbit/s | What limited it |
|---|---|---|---|---|---|
| Pure SRT | 96 | 128 | 0.84 Gbit/s | 148% | the measuring SRT receiver (all drops on its sockets) |
| Pure RTMP | **256** (top) | — | 2.22 Gbit/s | 10% | not reached |
| Pure HLS origin (readers) | **256** (top) | — | — | — | not reached |
| Pure HLS push | **256** (top) | — | 1.48 Gbit/s | 14% | not reached |
| RTMP 95% / SRT 5% | **256** (top) | — | 2.22 Gbit/s | 21% | not reached |
| HLS push 95% / SRT 5% | **256** (top) | — | 1.51 Gbit/s | 33% | not reached |
| RTMP 50% / HLS push 45% / SRT 5% | **256** (top) | — | 1.87 Gbit/s | 21% | not reached |

The first pass of this run had pure RTMP failing from 192 and the RTMP/SRT
mix from 256, with every destination's average at 0.999 or better and the
host 17–37% busy: the single-worker RTMP *receiver* stalled its own event loop
(624 ms, 14 late ticks) while the sender's stayed at 116 ms and 0.  With the
receiver sharded (one instance per 48 destinations, `4caabab`) both pass every
rung; at 256 the six receiver instances used 81% of a core against the
sender's 10–21% per Gbit/s.

SRT is the expensive protocol by an order of magnitude — user-space pacing
and a packet per 1316 bytes against TCP's kernel segmentation — and it is the
only workload whose boundary this 4-CPU host could find.  CPU per delivered
Gbit/s is the best guide to a larger host of the same class: at roughly 1.3–1.5 cores
per Gbit/s for the SRT sender, 512 SRT destinations at 8.8 Mbit/s
(4.5 Gbit/s) need about 6–7 cores for nginx alone, plus the receivers.

## Receiver topology and pure-SRT scaling (2026-09-26)

First measurements on a workstation rather than a 4-vCPU container, and the
first taken over anything other than loopback.  Host: Intel i9-13900H, six
P-cores with SMT (12 threads) and eight E-cores, 20 CPUs online, 15 GiB,
kernel 7.2.5, libsrt 1.5.3; source 8 Mbit/s 720p25 H.264 TS; 30 s rungs;
`CAPACITY_NGINX_CPUS` and `CAPACITY_RECEIVER_CPUS` pin sender and receivers
apart, and every rung's bundle records the placement.

### Loopback against a real device path

The receivers were moved into their own network namespace behind a veth pair
(`tests/bench/capacity_veth.sh up`, `CAPACITY_RECEIVER_ADDR=10.200.0.2`), so
the traffic crosses a device with an MTU and the kernel's UDP path instead of
loopback.  Same source, same rungs, same placement (nginx 0-5, receivers
12-19):

| Topology | Destinations | Delivered Gbit/s | Sender %core/Gbit/s | Receiver %core/Gbit/s | Min delivery ratio |
|---|---|---|---|---|---|
| loopback | 1 | 0.0091 | 1235.71 | 296.57 | 1.0 |
| loopback | 64 | 0.5830 | 128.16 | 88.42 | 1.00055 |
| loopback | 128 | 1.1607 | 142.20 | 111.29 | 0.99591 |
| veth | 1 | 0.0091 | 1166.34 | 302.59 | 1.0 |
| veth | 64 | 0.5818 | 134.00 | 92.88 | 1.00020 |
| veth | 128 | 1.1642 | 120.48 | 91.57 | 1.00071 |

The device path costs nothing measurable at this rate: delivered rate,
delivery ratio and CPU per delivered Gbit/s agree within run-to-run
variation.  Nothing in the sender or the receivers depends on loopback.

### Where pure SRT first fails on this host

| Destinations | Outcome | Delivered Gbit/s | Sender %core/Gbit/s | Receiver %core/Gbit/s | Min ratio | Interval floor | SndQ %core | App senders %core | Queue lag median |
|---|---|---|---|---|---|---|---|---|---|
| 128 | pass | 1.1572 | 114.8 | 89.4 | 0.99817 | 0.927 | 81.4 | 26.4 | 5 ms |
| 192 | pass | 1.7462 | 145.1 | 133.2 | 1.00404 | 0.933 | 158.4 | 50.9 | — |
| 256 | pass | 2.3558 | 159.5 | 128.5 | 1.01580 | 0.932 | 229.1 | 82.8 | 5 ms |
| 384 | quality-failure | 2.2740 | 215.3 | 184.0 | 0.56140 | 0.502 | 262.9 | 100.9 | 7115 ms |

At 384 the destination queues back up to seven seconds and the output queue
drops units (no kernel socket drops on either side, and the sender's pinned
CPUs all busy): the SRT send path stops draining them, the delivered rate falls
below what 256 delivered while the offered load rises, and 25 destinations
miss the gate.  The sender's own CPU is 490% of a core (4.9 cores of the 12
P-threads it is given), the receivers 418% of one core, no kernel socket
drops on either side, and libsrt's `SndQ` threads total 263% of a core
across 16 lanes - about 16% each, so no single lane is the bottleneck.

Two controls say what the limit is not:

| 384-destination control | Delivered Gbit/s | Min ratio | Sender %core/Gbit/s | Receiver %core/Gbit/s | Queue lag median | Output queue drops |
|---|---|---|---|---|---|---|
| receivers on E-cores (12-19) | 2.2740 | 0.561 | 215.3 | 184.0 | 7115 ms | 52056 |
| receivers on P-cores (6-11) | 2.7217 | 0.770 | 179.3 | 129.5 | 6906 ms | 8352 |

Faster receivers move the number (delivered +20%, minimum ratio 0.56 to 0.77)
without removing the failure, so the receiver side contributes but is not the
whole story.  What remains is the sender: at this rung nginx needs more CPU
per delivered Gbit/s than it has, and the per-destination output queues
absorb the difference until they overflow.

### What actually saturates, when nothing looks saturated

The 160-destination rung in the phase-2 shape below fails while the host is
14% busy, so it is worth naming what is short.  Its bundle now carries
per-CPU busy and the pinned sets' headroom:

| | |
|---|---|
| Per-CPU busy over the window | cpu0 **99.86%**, cpu2 2.35%, cpu4 0.19%, cpu6 1.32% |
| Pinned set (sender and receivers both on 0,2,4,6) | mean 25.9%, min 0.2%, max 99.9%, three cores idle, one saturated |
| Host busy | 14.2% |
| nginx worker / receiver CPU | 51.9% / 34.6% of one core |
| Receiver socket drops | **390 737** across 10 listener sockets |
| Sender feed drops, destination drops, blocked sends | 0 / 577 / 0 |

One core saturated and three idle is not a distribution accident: with a
small isolated CPU set, the loopback receive softirq is funnelled to one CPU,
so the benchmark's own receiver sockets overflow while the sender has three
cores to spare.  The same signature appears in the phase-2 replication below
(38 448 receiver drops at its 160-destination failure, host 10% busy), which
means that boundary is a **receiver-topology limit, not a sender limit** -
exactly the case the method says to answer by changing the topology rather
than by optimising the sender.  The 384-destination failure on the wider
topology is the opposite case: no kernel drops anywhere, the sender's pinned
CPUs busy, destination queues backing up - a sender CPU budget.

The follow-up measurement, when a machine is free: pin the receivers to CPUs
the sender does not use (or leave the set wider and unisolated), confirm the
receiver drops disappear, and only then re-read the SRT boundary.

### Sampled stacks at the first bottleneck

`perf record -a -g` for 15 s during the 160-destination rung that fails on
four isolated cores, then reported per CPU (the benchmark's own 0,2,4,6; the
machine also carried the user's interactive session on the other CPUs, which
the filter excludes).  The shares are of the samples taken on those four
CPUs:

| Process | Share | What it was doing |
|---|---|---|
| `curl` | 26.2% | the harness polling the control API and the samplers |
| `swapper` (idle) | 15.4% | |
| `bash` | 9.1% | the harness itself |
| `srt-egress-00` | 7.2% | `pthread_mutex_lock` first, then `ngx_media_srt_out_thread` |
| `SRT:RcvQ:w1..w9` | ~1.7-4.0% each | libsrt receive: `CRcvQueue::worker`, `worker_RetrieveUnit`, `CChannel::recvfrom` |
| `nginx` (worker) | 3.1% | spread thin, nothing above 2% |

Two findings, and neither is the transport:

1. **The measurement harness is the largest consumer on the benchmark's own
   cores** - curl plus bash is roughly a third of the samples, more than the
   software under test.  On a four-core budget shared by sender and
   receivers, the sampler competes with the thing it is measuring, which is
   part of why this rung fails while the host reads 14% busy.
2. **The application sender is not blocked in the transport**: its hottest
   symbol is `pthread_mutex_lock`, not `srt_sendmsg` or a libsrt send queue
   call.  With the lane multiplexer already in place, what is left of the
   sender's cost is its own loop and its lock, which is where a future
   scheduling change would have to look - and it needs a rung whose cores are
   not also running the harness before that profile can be trusted.

### Replicating the published 2026-09-25 numbers

The published phase-2 matrix came from a 4-vCPU Xeon container at 2.10 GHz,
unpinned.  The same shape here is four physical P-cores shared by the sender,
the receivers and the publisher, with their SMT siblings offlined and the
CPUs isolated from other work - on this host with the local helper
`omarchy-benchmark --cpu 0,2,4,6 --isolate --turbo on`, whose portable
equivalent is `taskset` on the same CPUs plus
`CAPACITY_NGINX_CPUS`/`CAPACITY_RECEIVER_CPUS` for the harness - so the
comparison is four whole cores against four vCPUs:

| Destinations | Published (new-adaptive) | Local (4 P-cores, no SMT) | Per-destination Mbit/s (published / local) |
|---|---|---|---|
| 1 | pass, 0.0088 Gbit/s, ratio 1.0 | pass, 0.0088, 1.0 | 8.80 / 8.80 |
| 32 | pass, 0.2799, 0.9983 | pass, 0.2810, 0.99698 | 8.75 / 8.78 |
| 64 | pass, 0.5589, 0.99618 | pass, 0.5567, 0.99622 | 8.73 / 8.70 |
| 96 | pass, 0.8391, 0.99227 | pass, 0.8235, 0.99234 | 8.74 / 8.58 |
| 128 | **quality failure**, 1.0873, 0.94043 | **pass**, 0.9844, 0.98730 | 8.49 / 7.69 |
| 160 | quality failure, 0.9550, 0.62979 | quality failure, —, 0.86446 | 5.97 / — |

What replicates is the delivery itself: within 1.6% of the published
per-destination rate at 32, 64 and 96 destinations, on the same 8 Mbit/s
source and the same 20 s windows, with the same minimum-ratio trend
(0.9983 → 0.9923 published, 0.9970 → 0.9923 local).  The boundary lands one
rung higher here (128 passing, 160 failing; 96 passing, 128 failing there).
At 128 the two runs diverge in an instructive way: the published host
delivered more per destination (8.49 vs 7.69 Mbit/s) but not to all of them,
so it failed at 0.940; the local run delivered less but evenly, and passed at
0.987.

CPU per delivered Gbit/s does not replicate, and should not: 49-70% of a core
per Gbit/s here against 126-238% there, because an i9 P-core at 5 GHz does
roughly three times the work of a 2.1 GHz Xeon vCPU.  That is why the repo
compares this ratio only within a host class.

The first attempt at this comparison used the tree's existing local nginx
binary, built 2026-09-25 04:58 - before the HLS interoperability merge.  It
produced the same SRT delivery rates but a different *unprepared* egress
behaviour, so it was rebuilt from the current source and every number above
comes from that build.

## Engineering report, 2026-09-26

### 1. Changes, commits and pull requests

| PR | What it fixes | Root cause | State |
|---|---|---|---|
| #5 | AMF fuzz generator | the nesting depth came from the iteration count, so `NGX_MEDIA_FUZZ_SCALE=25` wrote 6400 bytes into a 512-byte buffer; the parser's recursion bound was implicit | merged |
| #6 | capacity reporting | the HLS reader benchmark printed its configured gate in the field the pipeline read as the measured ratio, and HLS origin was missing from the efficiency sum | merged |
| #8 | per-lane service rate | lanes were keyed by shard number across workers, so a busy lane could report 0 | merged |
| #9 | receiver topologies, preflight, infrastructure-limited, fanout isolation, parallel bench tiers | the harness could only measure with its receivers on loopback, and an environment that could not carry the load was indistinguishable from a software limit | open, green except the container jobs re-running after the test fixes below |

Two test defects found while running the fanout isolation locally rather
than only in CI: a destination's first connection attempt can be retried
while the sink is still setting up (the health check is now a delta across
the measured window), and a lane is assigned when a destination connects, so
the churn check has to restart the sink and the publisher before comparing
placement.  A third: a local nginx binary older than the merged tree broke
the *unprepared* egress path, which made every integration run look broken
and every capacity run look healthy; CI always builds fresh.

### 2. CI

Regular CI green on `main`; the nightly sanitizer job passed on `main` with
the extended fuzz at `NGX_MEDIA_FUZZ_SCALE=25`.  Nightly and branch bench
tiers now run their configurations as parallel jobs instead of one after
another.

### 3. Reporting pipeline

Observed reader ratios (min/p5/p50/p95), aggregate receiver bytes, the
receiver's own measurement interval and delivered Gbit/s for HLS origin;
thresholds reported as thresholds; `delivery_ratio_basis`; a sender-only byte
counter refuses to become an efficiency; the completeness gate distinguishes
a ladder that stopped at its capacity boundary from one that was aborted or
killed; and the SRT report now publishes a strict full-rate verdict beside
the 0.95 gate.

### 4. Where SRT saturates

Per-rung bundles on this host (8 Mbit/s source, 30 s rungs, sender and
receivers pinned apart) are in the tables above.  Two distinct limits
appeared, and the per-CPU accounting separates them:

* **Sender CPU budget** - 384 destinations with the sender on three physical
  P-cores: no kernel drops anywhere, the sender's pinned CPUs busy, the
  destination queues backing up to 7 s; the same rung passes with all six
  P-cores.
* **Receiver topology** - 160 destinations on four isolated cores shared by
  sender and receivers: the host 14% busy, cpu0 at 99.9% with three cores
  idle (loopback receive softirq funnelled onto one CPU), and 390 737
  receiver socket drops.  The method's answer is to change the topology, not
  the sender, and that is what the follow-up should do.

### 5. Concurrency configurations

Fixed 1, 2, 4 and 8 SRT senders against adaptive at 128 and 192
destinations: the delivered rate is identical to three decimals
(1.1426-1.1433 and 1.7134-1.715 Gbit/s) and CPU per delivered Gbit/s falls
from 49.5 (one sender) to 38.7 (eight) at 128 destinations, against 29.7 of
a core in the lane's own `SndQ` threads.  No scheduling optimisation is
justified by this profile: the lane multiplexer is not the limit, and the
adaptive policy matches the best fixed count.

### 6. Seven-workload matrix

Four P-cores for the sender and four E-cores for the receivers, SMT siblings
offlined, 30 s rungs, 8 Mbit/s source, unprivileged (nginx's workers drop to
`nobody`, so a root-run harness cannot create HLS directories - a setup
failure the harness records as such, never as a quality failure).  The whole
run is recorded in `capacity-evidence/2026-09-26-seven-workloads.json`.

| Workload | Highest pass | First failure | Delivered at the top | Strict full rate | Published (4 vCPU) |
|---|---|---|---|---|---|
| Pure SRT | **256** (ladder top) | none | 2.286 Gbit/s | **yes** | 96 / 128 |
| Pure RTMP | **256** (ladder top) | none | 2.214 Gbit/s | not reported | 128 / 192 |
| Pure HLS origin (readers) | **256** (ladder top) | none | 2.207 Gbit/s | not reported | 256 (top) |
| Pure HLS push | **256** (ladder top) | none | 1.855 Gbit/s | not reported | 256 (top) |
| RTMP 95% / SRT 5% | **256** (ladder top) | none | — | not reported | 192 / 256 |
| HLS push 95% / SRT 5% | **256** (ladder top) | none | 2.018 Gbit/s | **no** | 256 (top) |
| RTMP 50% / HLS push 45% / SRT 5% | **256** (ladder top) | none | 2.118 Gbit/s | **yes** | 256 (top) |

Every workload reaches the ladder's top rung on this host, including the two
HLS directions the root-run attempt could not set up and both mixes the
published host bounded.  The ladder's top is 256; a larger host is needed to
find where any of them actually stops.

The strict column is the separate qualification the method asks for beside
the 0.95 gate, and it earns its place: the HLS-push/SRT mix passes the gate
at 256 with an average ratio of 0.99935 and no drops, TS errors or missing
segments, and still fails the strict test because its worst one-second
interval delivered 0.839 of the reference - a sub-second dip the average
hides.  The other two rungs whose bundles carry the verdict pass it.  The
workloads marked "not reported" ran before the verdict existed in the
harness; their bundles are unchanged and their strict result is unknown, not
assumed.

### 7. 1000 destinations

Not reachable on this host, and now reported as such: the preflight at 768
destinations returns `infrastructure-limited` (the probe's own receiver
counted 2.30 Gbit/s and dropped 83 825 datagrams against 2.54 offered, below
the 7.37 Gbit/s the rung needs), the ladder stops there, the rung is listed
as `infrastructure_limited` in the matrix and as a gate notice, and the
higher rungs are recorded as unmeasured rather than failed.

### 8. Fairness and shared-lane impairment

The fanout isolation test above: two impaired destinations sharing lane 0
with two healthy ones, lane 1 as control, 64 destinations in total.  The
healthy lane-mates delivered 100.0% of the control lane, the impaired lane's
shared `SndQ` carried 3809 retransmissions against 0 in the control lane, and
no destination outside the impaired lane moved.  Churn (delete, re-add,
reload) leaves the shards' destination count and the lane placement
identical.  What is not yet measured is mixed SRT session options (different
latency or encryption) sharing one lane, and SRT-driven contention against
RTMP and HLS in one program.

### 9. Remaining production limits

1. **The 1,000-destination claim is unverified.**  It needs a sender with
   more CPU and a receiver host on a wider path; the harness, the preflight
   and the classification are ready, and the exact commands are in the
   handover notes.
2. **The receiver topology can bind before the software does.**  A small
   isolated CPU set funnels loopback softirq onto one core; the receivers
   must be placed on CPUs the sender does not use before any boundary is
   read as a software limit.
3. **Mixed session options and cross-protocol contention** are implemented
   for the shared-lane case but not yet exercised (see 8).
4. **HLS push against a lossy, failing sink** is covered by the conformance
   fixtures; loss, latency, closure and intermittent HTTP errors are not
   injected yet.
5. **The two-host (ssh) receiver path** is implemented and documented but
   has never run against a second machine; only the namespace topology it
   shares code with has been validated.
6. **The local binary caveat**: any capacity number taken with a stale build
   is meaningless for the unprepared path; every number in this report comes
   from a build of the current source.

## Where this leaves the roadmap

| Deliverable | Status |
|---|---|
| Root cause with profiles and before/after | SRT: libsrt's per-destination multiplexer (a socket and a `SndQ`/`RcvQ` pair per stream) - its cost, by sampled stacks, is thread wakeups and context switches, not sending - fixed by lane multiplexer groups; sender CPU/Gbit/s 342–389% → 126–164%, highest pass 32 → 96 on this host, `SndQ` wakeup CPU 61.7% → 19.1% of a core at 64 destinations. |
| Reproducible command and a diagnostic bundle per run | `make bench-capacity-quality` / `scripts/bench-ci.sh <tier>`; `diagnostics.json` per rung, `capacity-matrix.json` per run |
| Capacity matrix, seven workloads | above, and `capacity-evidence/` |
| Fixed vs adaptive SRT senders | Phase 2: fixed 1, 2, 4 and adaptive pass the same rungs; one sender carries 96 destinations at 29% of a core.  CPU per Gbit/s falls to a floor at one sender per CPU; adaptive now runs one per lane in use up to granted CPUs and matches fixed 4 (104–109% vs 107%, was 122–127%) |
| Corrected RTMP timing, sustained HLS push | Phase 4; HLS push on identified segments |
| Unit, sanitizer, integration | pass; the two integration failures that reproduced on the baseline are fixed by the CI repair this is stacked on |
| 1000 destinations | not reachable on a 4-vCPU single host with the receivers co-located; needs the weekly CI tier on a larger runner, or separate sender and receiver hosts |

## Remaining production risks

1. **SRT on a larger host is unmeasured here.**  The fix removes the
   per-destination thread cost; the boundary on an 8+ core host, and whether
   the lane `SndQ` threads (up to 16) become the next limit, needs a run with
   the receivers on separate hardware.
2. **A lane's destinations share one UDP socket.**  Their kernel send buffer
   and the lane's single `SndQ` thread are shared, so a destination with heavy
   retransmission costs its lane-mates CPU.  Per-destination queues and SRT
   send buffers stay separate.  Measured with `make srt-lane-isolation`: a
   lane-mate of a destination behind 30% loss each way, 40 ms delay and a cap
   at half the stream rate - which delivered 6% of the stream, 566 datagrams
   lost and 15 602 over the cap in 15 s - delivered 100.0% of the other
   lanes' median with 0 ms queue lag, on a loopback host.  What remains
   unmeasured is many impaired destinations in one lane at high bitrate,
   where the shared `SndQ` thread's CPU could become the lane's limit.
3. **SRT sender count** now follows lanes in use and granted CPUs; it matched
   fixed 4 on a 4-CPU host.  On a larger host it will run up to 16 senders
   (one per lane); that the curve stays flat there, as it did from 4 to 16
   here, is expected but unmeasured.
4. **HLS push to YouTube** is checked against the published contract with a
   YouTube-shaped endpoint (`make hls-push-conformance`), not against
   YouTube itself: nothing in the test suite talks to the platform.
5. **Outbound HLS push under loss**: persistent connections, retry within a
   segment duration and `EXT-X-GAP` are in place and tested against refusing
   endpoints; their behaviour over a lossy long-distance path is not measured.
6. **robotweax/srt** passed one rung higher than libsrt at equal CPU per
   Gbit/s, but it is pre-1.0 (0.2.5); its receiver-side cost and behaviour
   under loss are not measured here.
7. **Pre-existing failures** `srt-ingest-nginx` and `api-graph`, which
   reproduced on the baseline, are fixed by the CI repair this is stacked on:
   a source delete had stopped removing the source on the worker that
   answered it, and the test still expected an output ceiling that had been
   removed on purpose.

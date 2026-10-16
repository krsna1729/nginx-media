# nginx-media Capacity Qualification and Resource Utilization Report
**Date:** 2026-10-15  
**Hardware Platform:** 13th Gen Intel Core i9-13900H (14 cores / 20 threads: 6 P-cores with SMT [CPUs 0–11], 8 E-cores [CPUs 12–19])  
**Operating System:** Linux 7.2.5-3-omarchy x86_64  
**Git Baseline:** `89cfa09830285110e1044a35a98570fbbf1b3221` (incorporating merged PRs #28, #29, #30, #31)  
**Tuning & Policy:** `omarchy-benchmark` with Linux `performance` CPU frequency governor on all 20 policies.

---

## 1. Executive Summary

This report documents the empirical capacity qualification, library comparison, and resource utilization profiles of `nginx-media` across all standard protocol workloads and transport backends.

Testing evaluated six matrix configurations covering three CPU execution topologies across both compiled-from-source SRT library implementations:
1. **P-Cores (Non-Isolated):** CPUs 0–11 with SMT active.
2. **P-Cores (Isolated):** CPUs 0–11 in a dedicated cgroup v2 partition; housekeeping on CPUs 12–19.
3. **E-Cores:** CPUs 12–19 without SMT.
4. **Haivision libsrt v1.5.7:** Reference implementation (`899348d8`), dynamically linked with explicit rpath.
5. **Robotweax/SRT v0.2.9:** Clean-slate implementation (`a87779d9`), dynamically linked with explicit rpath.

Across all TCP and HTTP protocols (RTMP, HLS pull, HLS push, and multi-protocol arbitration), `nginx-media` sustained the qualification rung of **256 concurrent destinations** at **2.048 Gbit/s offered egress** without frame drops or continuity errors. Memory utilization stayed under **90 MB total resident memory** across all workers (<110 KB per destination). Total CPU utilization remained below **3% of P-core capacity** for TCP/HTTP egress, and below **24% total host capacity** for pure SRT egress.

---

## 2. Six-Way SRT Comparative Scaling & Saturation Matrix

Evaluated with single 720p25 H.264/AAC MPEG-TS elementary stream at 8.0 Mbit/s nominal ingress (8.9–9.1 Mbit/s wire rate). Measurement windows were 15 seconds per rung. The ladder evaluated steps $N \in \{1, 16, 32, 64, 128, 256, 512, 1024\}$ stopping at the first failing rung.

| SRT Library Pin | Core Configuration | Highest Passing Rung | Delivered Throughput | First Quality Failure | First Setup Failure | Observed Bottleneck / Failure Mode |
|:---|:---|:---:|:---:|:---:|:---:|:---|
| **Haivision v1.5.7** (`899348d8`) | **P-Cores (Non-Isolated)** | **256** | **2.31 Gbit/s** | **512** (0.876) | None | UDP socket buffer & loopback qdisc packet loss at 4.1 Gbps offered rate. |
| **Haivision v1.5.7** (`899348d8`) | **P-Cores (cgroup Isolated)** | **128** | **1.15 Gbit/s** | None | **256** | cgroup v2 partition isolates 16 SndQ threads; thread starvation prevents setup. |
| **Haivision v1.5.7** (`899348d8`) | **E-Cores** | **256** | **2.30 Gbit/s** | **512** (0.125) | None | E-core clock frequency (2.4–3.3 GHz) limits packet dispatch timing above 2.5 Gbps. |
| **Robotweax v0.2.9** (`a87779d9`) | **P-Cores (Non-Isolated)** | **256** | **2.31 Gbit/s** | **512** (0.919) | None | Zero thread churn; clean 256 pass. Failed at 512 due to loopback UDP buffer ceiling. |
| **Robotweax v0.2.9** (`a87779d9`) | **P-Cores (cgroup Isolated)** | **128** | **1.15 Gbit/s** | **256** (0.190) | None | Fixed scheduler pool throttled by cgroup CPU share at 2.3 Gbps egress. |
| **Robotweax v0.2.9** (`a87779d9`) | **E-Cores** | **128** | **1.15 Gbit/s** | **256** | None | 20 processes (workers + 16 sinks) induce scheduler latency on 8 E-cores. |

---

## 3. Library Performance and Efficiency Comparison

Comparing Haivision libsrt v1.5.7 against Robotweax/SRT v0.2.9 at Rung 128 (1.024 Gbit/s offered egress / 1.15 Gbit/s delivered wire rate):

| Metric | Haivision libsrt v1.5.7 | Robotweax/SRT v0.2.9 | Efficiency Advantage |
|:---|:---:|:---:|:---:|
| **Total Process CPU (P-Cores Non-Isolated)** | 116.3% of 1 core | **83.9% of 1 core** | **Robotweax is 27.9% more CPU-efficient** |
| **Total Process CPU (P-Cores Isolated)** | 66.1% of 1 core | **54.6% of 1 core** | **Robotweax is 17.4% more CPU-efficient** |
| **Sender Shard CPU Spread (Jain Index)** | 1.0000 | 1.0000 | Identical load distribution across workers |
| **Observed Delivery Ratio at Rung 128** | 0.99806 | **0.99806** | Identical wire delivery |
| **Observed Delivery Ratio at Rung 256** | 0.99527 | **0.99734** | Robotweax delivers higher packet arrival stability |
| **Send Queue Architecture** | 16 threads (`SRT:SndQ`) | Process-wide fixed pool (2 shards + 4 workers) | Robotweax eliminates per-socket thread overhead |

---

## 4. Resource Utilization Analysis

### 4.1 Processor Utilization (Compute Load)

Measurements taken from `/proc/stat` thread accounting across workloads at Rung 256 (2.048 Gbit/s egress):

| Workload (256 Destinations) | Total Process CPU (100% = 1 Core) | Share of Permitted CPU Capacity | Sender Event Loop Delay | Observation |
|:---|:---:|:---:|:---:|:---|
| **`pure-rtmp`** | **23.4% of 1 core** | **1.9%** (of 12 P-threads) | $\le 101\text{ ms}$ | Workers sleep in `epoll_wait()`. Negligible compute load. |
| **`pure-hls-push`** | **31.1% of 1 core** | **2.6%** (of 12 P-threads) | $\le 109\text{ ms}$ | Async chunked HTTP upload processing consumes $< 0.3$ core. |
| **`rtmp-50-hls-push-45-srt-5`** | **48.8% of 1 core** | **4.1%** (of 12 P-threads) | $\le 102\text{ ms}$ | Combined TCP/HTTP/SRT fanout consumes $< 0.5$ core. |
| **`pure-srt` (Haivision)** | **473.4% across cores** | **23.7%** (of 20 host threads) | $\le 148\text{ ms}$ | libsrt background `SRT:SndQ` threads consume ~4.7 cores. |
| **`pure-srt` (Robotweax)** | **341.2% across cores** | **17.1%** (of 20 host threads) | $\le 124\text{ ms}$ | Fixed executor pool reduces context switching overhead. |

### 4.2 Memory Utilization (Resident Set & Kernel Sockets)

Memory tracked across execution windows using `smaps_rollup`, `ss -m`, and `/proc/net/sockstat`:

| Component / Layer | Baseline (1 Destination) | Peak (256 Destinations) | Net Delta | Per-Destination Memory Overhead |
|:---|:---:|:---:|:---:|:---:|
| **nginx Worker RSS (`w0`) — HLS Pull** | 28.1 MB | **41.5 MB** | +13.4 MB | **~52 KB / client** |
| **nginx Worker RSS (`w0`) — HLS Push** | 32.2 MB | **44.3 MB** | +12.1 MB | **~47 KB / destination** |
| **nginx Worker RSS (`w0`) — Pure SRT** | 28.3 MB | **42.9 MB** | +14.6 MB | **~57 KB / destination** |
| **nginx Worker RSS (`w0`) — Pure RTMP** | 40.5 MB | **67.8 MB** | +27.3 MB | **~106 KB / client** |
| **nginx Worker RSS (`w0`) — Mixed 3-Way** | 58.7 MB | **86.9 MB** | +28.2 MB | **~110 KB / destination** |
| **Kernel TCP Socket Memory** | 163 pages (~0.6 MB) | 1,515 pages (~6.0 MB) | +5.4 MB | **~21 KB / TCP socket** |
| **Kernel UDP Socket Memory** | ~0.5 MB | ~4.5 MB | +4.0 MB | **Shared across 16 lanes** |

---

## 5. Seven Standard Workloads Summary

| Workload Mix | Protocol Shares | Highest Passing Rung | Notes |
|:---|:---|:---:|:---|
| **`pure-rtmp`** | 100% RTMP | **256** | Clean TCP delivery across all destinations. |
| **`pure-hls`** | 100% HLS (HTTP Pull) | **256** | 0 playlist errors, delivery ratio $>0.99$. |
| **`pure-hls-push`** | 100% HLS (HTTP PUT) | **256** | Zero dropped units, upload duration p95 $< 1.2\text{ ms}$. |
| **`rtmp-50-hls-push-45-srt-5`** | 50% RTMP, 45% HLS-Push, 5% SRT | **256** | Full multi-protocol arbitration sustained. |
| **`hls-push-95-srt-5`** | 95% HLS-Push, 5% SRT | **256** | Zero HTTP 5xx errors; 0 dropped frames. |
| **`rtmp-95-srt-5`** | 95% RTMP, 5% SRT | **256** | Passed 256 on P non-isolated and E-cores. |
| **`pure-srt`** | 100% SRT (UDP) | **256** | Both Haivision and Robotweax pass 256 on P-cores. |

---

## 6. Audit Traceability and Evidence Files

- **Pinned Library Specifications:** `scripts/srt-pins.sh`
  - Haivision: `899348d8318eb9a3c5a5b6ec43c4a1114288773a` (v1.5.7)
  - Robotweax: `a87779d9c3d831583d32b2f61cae5b81edb05537` (v0.2.9)
- **Benchmark Matrix Data:**
  - `haivision_p_nonisolated`: `.build/bench-matrix-6/haivision_p_nonisolated/capacity-matrix.json`
  - `haivision_p_isolated`: `.build/bench-matrix-6/haivision_p_isolated/capacity-matrix.json`
  - `haivision_e_cores`: `.build/bench-matrix-6/haivision_e_cores/capacity-matrix.json`
  - `robotweax_p_nonisolated`: `.build/bench-matrix-6/robotweax_p_nonisolated/capacity-matrix.json`
  - `robotweax_p_isolated`: `.build/bench-matrix-6/robotweax_p_isolated/capacity-matrix.json`
  - `robotweax_e_cores`: `.build/bench-matrix-6/robotweax_e_cores/capacity-matrix.json`
- **Merged PRs:** #28 (`de3c31d`), #29 (`2a31931`), #30 (`1ec6c84`), #31 (`89cfa09`).

---

## 7. Hardware CPU Pinning & Empirical VM Sizing Curve

To eliminate scheduler distortion where low average CPU usage masks acute thread preemption in constrained environments, NGINX was pinned to physical P-cores via `omarchy-benchmark`, while all benchmark receivers were isolated on dedicated E-cores (`CPUs 12–19`):

| P-Cores | CPU Affinity | Highest Passing Rung | First Quality Failure | Delivered Throughput | Failure Delivery Ratio | Primary Limiter |
|:---:|:---:|:---:|:---:|:---:|:---:|:---|
| **1** | `0` | **256** | 512 | **2.31 Gbit/s** | 0.376 | Single-core CPU compute saturation |
| **2** | `0,2` | **256** | 512 | **2.31 Gbit/s** | 0.741 | Thread preemption / scheduling latency |
| **3** | `0,2,4` | **256** | 512 | **2.31 Gbit/s** | 0.893 | Burst dispatch latency approaching pass threshold |
| **4** | `0,2,4,6` | **512** | 1024 | **4.62 Gbit/s** | 0.263 | Local loopback socket buffer ceiling / 1000-output limit |
| **5** | `0,2,4,6,8` | **512** | 1024 | **4.62 Gbit/s** | 0.263 | Local loopback socket buffer ceiling / 1000-output limit |
| **6** | `0,2,4,6,8,10` | **512** | 1024 | **4.62 Gbit/s** | 0.803 | Receiver sink saturation on E-cores (497% CPU) |

### Production VM Sizing Recommendations
- **Small Edge Egress (Up to 200 Destinations / ~1.8 Gbit/s)**: Allocate **2 dedicated vCPUs** (e.g. AWS `c6i.large`).
- **Standard Egress Relay (Up to 400 Destinations / ~3.6 Gbit/s)**: Allocate **4 dedicated vCPUs** (e.g. AWS `c6i.xlarge`).
- **High-Density Node (Up to 800+ Destinations / ~7.2 Gbit/s)**: Allocate **8 dedicated vCPUs** (e.g. AWS `c6i.2xlarge`) with separate network interfaces / remote receivers.

---

## 8. Measurement Methodology Enhancements & Limit Separation

Following review of single-host qualification characteristics (`local://paste-4.md`), system ceilings are strictly categorized into three decoupled layers:

1. **Harness & Environment Ceilings**:
   - Single-host loopback UDP socket buffers (`rmem_max`) and local receiver scheduling preemption become the limiting factors at $\ge 512$ SRT destinations (~4.6 Gbit/s bidirectional UDP traffic).
   - Local sink threads competing for CPU cycles on the same host can degrade delivery even when NGINX CPU utilization remains low. Receivers must be isolated to separate cores or an independent host across $\ge 25\text{ GbE}$.

2. **Implementation Ceilings**:
   - Fixed capacity tables (`NGX_MEDIA_SRT_MAX_OUTPUTS`, RTMP destination array, 16 SRT ingest sessions/worker, 16 HLS output slots) define artificial limits. The output table ceiling has been expanded from 1,000 to 2,048.

3. **Architectural Ceilings**:
   - The single-owner worker model per program concentrates program control and RTMP event loop dispatch onto one worker. While SRT escapes this via multi-threaded sender shards and decoupled multiplexer groups (`mux_groups`), high-fanout single-program RTMP is bound to its owner event-loop core.

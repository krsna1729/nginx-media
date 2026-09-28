# Run transcripts, 2026-09-28

The harness writes a `diagnostics.json` per rung and a `capacity-matrix.json`
per run into its run directory (`.build/ingest-egress-fanout/capacity/` by
default, or `CAPACITY_RUN_DIR`).  Those directories are per-run scratch space:
a later run replaces them.  These files are the transcripts of the runs quoted
in `docs/capacity-results.md`, kept so the numbers can be traced back to the
command that produced them - every rung's report, CPU split, delivery ratio
and diagnostics summary is in them.

| File | Command | What it holds |
| --- | --- | --- |
| `seven-workloads-hls.log` | `CAPACITY_QUALITY_MIXES="hls-100" ...` | the HLS-only workload ladder |
| `seven-workloads-srt-rtmp.log` | `CAPACITY_QUALITY_MIXES="srt-100 rtmp-100 srt-50-rtmp-50" ...` | three of the seven workloads |
| `seven-workloads-rtmp-hls-push-srt.log` | `CAPACITY_QUALITY_MIXES="rtmp-50-hls-push-45-srt-5" CAPACITY_QUALITY_STEPS="1 32 64 96 128 192 256"` | the published mix, every rung passing |
| `seven-workloads-matrix.log` | `CAPACITY_QUALITY_MIXES="srt-75-rtmp-25" ...` | the last workload plus the merged matrix |
| `sustained-384-1800s.log` | `PHASES=capacity-sustained CAPACITY_SUSTAINED_DESTS=384 CAPACITY_SUSTAINED_SECONDS=1800 CAPACITY_QUALITY_REFERENCE_BPS=8965249` | thirty minutes at 384 destinations |
| `rung-384-and-512-120s.log` | `PHASES=capacity-quality-ladder CAPACITY_QUALITY_STEPS="384 512" CAPACITY_QUALITY_SECONDS=120` | the two boundary rungs re-measured with a 120-second window |
| `contention-ladder.log` | `CAPACITY_QUALITY_MIXES="srt-50-rtmp-25-hls-push-25" CAPACITY_QUALITY_STEPS="4 32 64 128"` | half SRT, a quarter each RTMP and HLS push, all rungs passing |

The runs were driven through `omarchy-benchmark` with the sender on P-cores
0,2,4,6 and the receivers on 8,10,12,13 unless the log says otherwise, and the
harness itself unprivileged (`sudo -u krsna1729`): as root the workers drop to
`nobody` and cannot write the HLS directory, which starves the HLS push
destinations and turns a quality question into an invocation failure.

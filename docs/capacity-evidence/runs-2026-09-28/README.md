# Run transcripts, 2026-09-28

The harness writes a `diagnostics.json` per rung and a `capacity-matrix.json`
per run into its run directory (`.build/ingest-egress-fanout/capacity/` by
default, or `CAPACITY_RUN_DIR`).  Those directories are per-run scratch space:
a later run replaces them.  These files are the transcripts of the runs quoted
in `docs/capacity-results.md`, kept so the numbers can be traced back to a
run - every rung's report, CPU split, delivery ratio and diagnostics summary
is in them.  Each row states what the transcript itself contains, which is
what a reader can verify; the exact environment of each run is in the section
below.

| File | Mixes in the transcript | Rungs |
| --- | --- | --- |
| `seven-workloads-hls.log` | `pure-hls`, `pure-hls-push` | 1-256 |
| `seven-workloads-srt-rtmp.log` | `pure-srt`, `pure-rtmp` | 1-256 |
| `seven-workloads-matrix.log` | `rtmp-95-srt-5`, `hls-push-95-srt-5` | 1-256 |
| `seven-workloads-rtmp-hls-push-srt.log` | `rtmp-50-hls-push-45-srt-5` | 1-256 |
| `boundary-above-30s-ladder.log` | `pure-srt` | 256-512 |
| `rung-384-and-512-120s.log` | `pure-srt` | 1, 24, 384 |
| `rung-512-120s-repeat.log` | `pure-srt` | 32, 512 |
| `sustained-384-1800s.log` | `pure-srt` | 24, 384 (1800 s) |
| `contention-ladder.log` | `srt-50-rtmp-25-hls-push-25` | 4-128 |
| `sender-repeats-driver.log` | `pure-srt` | 128, six sender counts, three repetitions |

How the runs were driven: through `omarchy-benchmark` with the sender on
P-cores 0,2,4,6 and the receivers on 8,10,12,13 unless the transcript says
otherwise, the harness itself unprivileged (`sudo -u krsna1729`) - as root the
workers drop to `nobody` and cannot write the HLS directory, which starves the
HLS push destinations and turns a quality question into an invocation
failure.  Every rung in these transcripts ran under the capacity preflight,
which is what stops a rung beside a competing job on its own CPUs.

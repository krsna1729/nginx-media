# Engine-side proposals from the capacity harness

These are changes to `restream` and to libsrt usage inside it. This repository
does not contain those trees, so nothing here is applied. Each item is written
as evidence plus a concrete change, and each was found while qualifying
`nginx-media` against a receiver-verified measurement.

**Status: nothing here is a capacity claim.** The 1024-destination rung below
is unresolved, and the run that produced it came from a harness that has since
been fixed. Re-measure before acting on any of it.

---

## P1. The 1024-destination restream rung: cause unresolved

**What happened.** The ladder ended `setup-error` at 1024 destinations. The
gate that fired:

```
quality_rung_start mix=pure-srt destinations=1024 srt=1024 ...
   srt_receiver_listeners=64 (<= 16 destinations each)
SRT receiver snapshot was not written
could not capture incomplete SRT receiver snapshot
shared SRT receiver accepted fewer than 1024 peers
quality_rung_result mix=pure-srt destinations=1024 result=setup-error status=1
```

The receiver's log shows peers connecting and then being torn down within about
seven seconds, in three error classes:

| error | count |
|:---|---:|
| `Connection was broken` | 161 |
| `SRT_EPOLL_ERR` | 51 |
| `Operation not supported: Invalid socket ID` | 44 |

**What this does *not* say.** The receiver log records transport errors only.
Peers that connected and stayed healthy produce no line at all, so 255 distinct
destinations appear in it because 255 *broke* - not because only 255 ever
connected. How many connected is exactly what the lost snapshot would have
said.

**The leading hypothesis is receiver CPU, and it is an extrapolation.**
Measured on the rung below the failure, which passed:

```
quality-pure-srt-512 ... result=pass
   diag: host CPU busy 39.1% of all online CPUs
   diag: sender 169% vs receivers 462% of one core
```

The receiver used 462% of one core at 512 destinations for 4 Gbit/s of SRT.
Every destination carries its own 8 Mbit/s, so 1024 destinations means
8 Gbit/s and doubles both the packet rate and the bandwidth - the harness
recorded `offered_egress=8192.0Mbit/s` for that rung. That implies roughly
924% of one core. The receiver is pinned to eight E-cores, which is 800% of
one core before counting that E-cores are narrower than P-cores. The receiver
is therefore the most likely side to run out first - but this is arithmetic on
one measurement, not a measurement of the failing rung, whose CPU figures are
unavailable.

**Why it cannot be attributed further from this run.** `start_restream_engine`
removes `$RUN/restream` on every start, so restream's database and logs for
this rung were overwritten by eight later ladders. The surviving `outputs`
table holds one row, `d0000`, from a one-destination rung. The snapshot that
would have named the connected count was lost because the sink checks for
SIGUSR1 only between one-second `srt_epoll_uwait` calls, and the harness gave
it one second.

**What this repository has already fixed**, so that the re-run can answer it:

* the snapshot budget now exceeds one uwait period, so the capture survives a
  quiet sink and the peer count is recorded
* the failure states how much of the connect window elapsed
* the rung's `restream/{logs,db,cookie.txt}` and an output-state count are
  copied into its own case directory inside `capacity_write_diagnostics`, which
  runs for setup failures and not only for passes

**The re-run that would settle it**, in order of discriminating power:

1. 1024 destinations on restream, with the receiver given more cores. If it
   passes, the limit was the receiver and not the engine.
2. The same rung with `CAPACITY_SINK_MODE=fast`. If it passes, the sink's
   correctness checking was the cost.
3. If it still fails, count `egress.failed` reasons in the preserved restream
   log to see whether the sender gave up on outputs or the receiver dropped
   them.

**Proposal: none yet.** Do not change restream on the strength of this rung.

---

## P2. SRT egress exposes no connection-state counters

**Evidence.** Restream's own counters, from a **single-output rung** of an
earlier mix - the only place they survive, because per-rung state was not kept:

```
srt egress owner final counters family="v4" managed_rx=true service_visits=5590
  service_actions=15932 maintenance_actions=1 service_budget_exhausted=0
  rx_packets=1183 rx_truncated=0 rx_ring_dropped=0 rx_buffer_exhaustions=0
  tx_packets=11910 tx_completed_ok=11910 tx_high_water=16 tx_exhaustions=593
  caller_expired=0 peer_group_collisions=0
```

`tx_exhaustions=593` against `tx_completed_ok=11910` is the only signal that
the transmit path was ever under pressure. There is no counter for the state a
controller needs when a large rung stalls: how many outputs are connecting,
connected, retrying, or backed off.

`managed_rx=true` and the `rx_packets` figure are **expected** and not a
defect: an SRT sender in publish mode still receives ACK, NAK and keepalive
control packets on the same socket.

**Why it matters.** A rung that fails during establishment produces one
aggregate message. With per-output connection state and a reconnect counter,
the harness could say "683 of 1024 outputs were still in backoff when the
window expired" instead of "fewer than 1024 peers", which is the difference
between a diagnosis and a symptom - and it is the single change that would
have made P1 answerable from the artifacts.

**Proposal.** Expose, per pipeline and in aggregate:

* `outputs_connecting`, `outputs_connected`, `outputs_backoff`
* `connection_attempts_total`, `connection_failures_total`
* a max or histogram of time-in-backoff

Cheap counters on an existing state machine, and the missing half of the
`tx_exhaustions` family that already exists.

---

## Withdrawn

Two items were drafted here and are withdrawn, because the evidence did not
support them:

* **"a peer disconnect escalates an output to `egress.failed`"** - the single
  line behind this came from a later one-output rung and most likely records
  the harness stopping the sink at teardown. It does not show that one blip
  kills a healthy output. Verify on the re-run before raising it.
* **"publish-mode SRT enables a receive direction it never uses"** - wrong.
  An SRT sender must receive ACK, NAK and keepalive packets, so `managed_rx`
  and `rx_packets` are required for publish mode to work at all.

And one correction to a table that was here: the three-way wall times
(4406 s, 5716 s, 464 s) are **harness cost, not engine speed**. The two nginx
ladders paid the old `post_owner` `sleep 0.2` at roughly 0.65 s per
destination with four workers; restream does not route by owner and did not
pay it. That gap has since been removed from the harness, so the numbers say
nothing about the engines and are omitted.

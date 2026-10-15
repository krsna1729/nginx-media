# Engine-side proposals from the capacity harness

These are changes to `restream` and to libsrt usage inside it. This repository
does not contain those trees, so nothing here is applied. Each one is written
as evidence plus a concrete change, and each was found while qualifying
`nginx-media` against a receiver-verified measurement.

Nothing below is a capacity claim. Every number is from a run whose artifacts
are in `docs/capacity-evidence/`.

---

## P1. The 1024-destination rung is un-attributable as recorded

**What the harness recorded.** The `restream` pure-SRT ladder at 1024
destinations ended `setup-error`. The gate that fired:

```
quality_rung_start mix=pure-srt destinations=1024 srt=1024 ...
   srt_receiver_listeners=64 (<= 16 destinations each)
SRT receiver snapshot was not written
could not capture incomplete SRT receiver snapshot
shared SRT receiver accepted fewer than 1024 peers
quality_rung_result mix=pure-srt destinations=1024 result=setup-error status=1
```

**What is known.** The receiver connected 255 distinct destinations and then
reported 256 transport errors against them:

| error | count |
|:---|---:|
| `Connection was broken` | 161 |
| `SRT_EPOLL_ERR` | 51 |
| `Operation not supported: Invalid socket ID` | 44 |
| libsrt `remove_usock: ... not found as either socket or group` | 81 |

The receiver's own CPU was not the limit: at comparable rungs the harness
recorded `sender 2% vs receivers 2% of one core`. The receiver was idle while
peers were failing to establish.

**What is *not* known, and why.** The harness reused one run directory for
every rung and every mix, and eight further ladders ran after this rung. So
restream's own database and logs for the rung no longer exist; the surviving
`outputs` table holds one row, `d0000`, from a one-destination rung. The
snapshot that would have named the connected count was lost because its
timeout was fixed at one second regardless of peer count.

This is a harness defect and is being fixed here (snapshot budget scales with
the peer table, and the failure now states the connected count and how much of
the connect window was used). The consequence for the engine is below.

**Proposal.** Nothing yet. The correct next step is to re-run the rung with the
harness fixed so that the split between "the sender never offered the
destinations" and "the receiver could not accept them" is measured rather than
inferred. **Do not act on P1 until that rung is re-measured.**

---

## P2. SRT egress has no connection-establishment telemetry

**Evidence.** Restream's own counters for one output, at a rung where it did
establish:

```
srt egress owner final counters family="v4" managed_rx=true service_visits=5590
  service_actions=15932 maintenance_actions=1 service_budget_exhausted=0
  rx_packets=1183 rx_truncated=0 rx_ring_dropped=0 rx_buffer_exhaustions=0
  tx_packets=11910 tx_completed_ok=11910 tx_high_water=16 tx_exhaustions=593
  caller_expired=0 peer_group_collisions=0
```

`tx_exhaustions=593` against `tx_completed_ok=11910` is the only signal that
the transmit path was ever under pressure, and there is no counter for the
state a controller actually needs when a large rung stalls: how many outputs
are connecting, connected, retrying, or backed off.

**Why it matters to a qualification run.** A rung that fails during
establishment currently produces one aggregate message. With a per-output
connection state and a reconnect counter, the harness could say "683 of 1024
outputs were still in backoff when the window expired" instead of "fewer than
1024 peers", which is the difference between a diagnosis and a symptom.

**Proposal.** Expose, per pipeline and in aggregate:

* `outputs_connecting`, `outputs_connected`, `outputs_backoff`
* `connection_attempts_total`, `connection_failures_total`
* a histogram or max of time-in-backoff

These are cheap counters on an existing state machine, and they are the
missing half of the `tx_exhaustions` family that already exists.

---

## P3. SRT leaf failure is terminal for the output, so a rung measures a fall

**Evidence.**

```
WARN srt egress leaf closed unexpectedly output_id=output_cf05ad1a187343da
  reason="peer disconnected"
WARN output failed pipeline_id=... output_id=... event_class="lifecycle"
  event_type="egress.failed" phase=srt_fabric_leaf
  error=SRT fabric leaf terminated unexpectedly (peer closed, protocol failure,
  or stall recovery)
```

One peer disconnect turns into `egress.failed` for the whole output. In a
capacity measurement that is indistinguishable from the output never having
started: a rung that needs 1024 outputs will be reported as a setup failure
the moment any single peer blips, even if the other 1023 are healthy.

This interacts directly with P1: 255 peers had transport errors, and if each
one became `egress.failed`, the sender's effective destination count can only
fall during a rung.

**Proposal.** Separate "this output's current peer disconnected" from "this
output cannot run". SRT is designed to survive a peer reconnect; the output
state machine should retry with backoff and only escalate to `failed` after a
documented attempt budget, with the retry visible in the counters from P2.
Whether that is already the intent is not something this repository can
determine — hence a proposal, not a patch.

---

## P4. `rx` is enabled on a publish-only SRT leaf

**Evidence.** The same counters report `managed_rx=true` and `rx_packets=1183`
for an output whose streamid is `#!::m=publish,...`. A publish-mode SRT sender
negociates a receive direction it never uses.

**Proposal.** Confirm whether `managed_rx=true` is required for the streamid
mode in use. If it is not, disabling it removes a socket, a buffer and a read
registration per destination, which at 1024 destinations is 1024 fewer things
for the egress fabric to service per cycle.

This is a micro-optimisation and last on the list deliberately: P1 has to be
re-measured, and P2 and P3 change what a rung reports, before a per-destination
cost difference is worth chasing.

---

## What the harness already attributes correctly

Not everything here is a proposal. These were measured and are recorded with
their evidence:

| engine | highest passing | first failure | ladder wall |
|:---|---:|---:|---:|
| `nginx-robotweax` | 256 | 512 (quality, 0.774 delivered) | 4406 s |
| `nginx-haivision` | 512 | 1024 (quality) | 5716 s |
| `restream` | 512 | 1024 (setup, see P1) | 464 s |

The two nginx builds fail differently at the same rung, which localises the
difference to the send path rather than the harness. Restream reaches 512 in
under an eighth of the wall time of either.

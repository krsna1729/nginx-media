# nginx-media specification

The specification this repository is built to.  It is the authority on what the
implementation must do: when the code and this document disagree, one of them
is a bug, and the section numbers are referenced from the source comments so
the two can be checked against each other.

**Primary goal:** a media subsystem that accepts redundant live and file
inputs, selects one logical program, and distributes it through SRT, RTMP, HLS
and recording while remaining bounded, deadline-aware and horizontally
scalable across NGINX workers.

---

## 1. Mission

Build `nginx-media` as a real media subsystem, not as an SRT-to-RTMP bridge and not as a permanent FFmpeg wrapper.

The architectural invariant is:

```text
INPUTS
SRT / bonded SRT / RTMP / file
        |
        v
SOURCES
identity -> probe -> health -> eligibility
        |
        v
SELECTOR
priority + hysteresis + switch policy
        |
        v
PROGRAM
timeline-normalized logical stream
        |
        +---- HLS
        +---- recording
        +---- RTMP
        +---- SRT
```

A logical stream is not a publisher. A logical stream may have multiple independent sources. SRT bonding is transport redundancy *inside one source*. Two encoders publishing the same program are two sources. A file slate is another source.

The implementation must preserve this distinction everywhere: configuration, state, metrics, failover, recording and API.

---

## 2. Non-negotiable design principles

1. **Protocol-neutral media core.** SRT, RTMP and file inputs terminate at a common encoded-media representation.
2. **No decoding in the normal path.** Demux, remux, package and relay encoded media. FFmpeg/libavcodec is reserved for explicit transcoding or broad-format bootstrap paths.
3. **Parse once, share payloads.** Use immutable refcounted buffers and shared transformations.
4. **Bound everything.** Rings, pending bytes, preroll, HLS accumulation, protocol queues, retry work and per-visit work must have hard ceilings.
5. **Source health is not source selection.** Health answers whether a source is eligible. Selection chooses among eligible sources.
6. **Failover is a media operation.** Socket-up is insufficient. Container validity, timestamp progress, keyframes and track compatibility matter.
7. **Switches are generation changes.** Consumers must receive explicit discontinuity/generation semantics.
8. **No per-subscriber media copies or queues in steady state.**
9. **Fanout execution must not starve ingest, health evaluation or failover.**
10. **Capacity is deadline-defined, not connection-count-defined.**
11. **SRT transport scheduling belongs behind an adapter.** Do not leak backend internals into the media core.
12. **Use NGINX event-loop semantics for media state.** Custom threads may bridge foreign runtimes or perform blocking I/O, but they must not own arbitrary NGINX/media state.

---

## 3. Target repository structure

```text
nginx-media/
  config
  README.md
  src/
    core/
      ngx_media.h
      ngx_media_core_module.c
      ngx_media_application.c
      ngx_media_stream.c
      ngx_media_source.c
      ngx_media_selector.c
      ngx_media_timeline.c
      ngx_media_frame.c
      ngx_media_buffer.c
      ngx_media_track.c
      ngx_media_health.c
      ngx_media_feed.c
    srt/
      ngx_media_srt_module.c
      ngx_media_srt_transport.h
      ngx_media_srt_haivision.c
      ngx_media_srt_robotweax.c
      ngx_media_srt_group.c
    rtmp/
      ngx_media_rtmp_module.c
      ngx_media_rtmp_adapter.c
      ngx_media_rtmp_wire.c
    file/
      ngx_media_file_module.c
    mpegts/
      ngx_media_ts_demux.c
      ngx_media_ts_mux.c
    codec/
      ngx_media_h264.c
      ngx_media_h265.c
      ngx_media_aac.c
    hls/
      ngx_media_hls_module.c
      ngx_media_hls_segmenter.c
    record/
      ngx_media_record_module.c
    relay/
      ngx_media_relay_module.c
    fanout/
      ngx_media_fanout.c
      ngx_media_fanout_ring.c
      ngx_media_fanout_scheduler.c
    api/
      ngx_media_api_module.c
    stat/
      ngx_media_stat_module.c
  tests/
    unit/
    integration/
    bench/
```

Names may change, but boundaries should not collapse.

---

## 4. Core object model

### 4.1 Immutable media buffer

```c
typedef struct ngx_media_buf_s ngx_media_buf_t;

struct ngx_media_buf_s {
    ngx_atomic_t    refs;
    size_t          len;
    size_t          capacity;
    u_char         *data;
};
```

Payload ownership must be shareable across demux, program ring, HLS, RTMP and SRT paths. The steady-state fanout path must not allocate/copy payload per receiver.

### 4.2 Media packet/frame

Use a compact encoded-media object:

```c
typedef struct {
    ngx_uint_t       media_type;      /* video/audio/data */
    ngx_uint_t       codec;
    ngx_uint_t       payload_format;  /* explicit representation */
    ngx_uint_t       track_index;
    int64_t          pts;
    int64_t          dts;
    unsigned         keyframe:1;
    unsigned         config:1;
    ngx_media_buf_t *payload;
} ngx_media_frame_t;
```

`payload_format` is important. "Normalized" means common ownership/timestamps/metadata, not that every codec must be converted to one byte framing.

### 4.3 Logical stream

```c
struct ngx_media_stream_s {
    ngx_pool_t              *pool;
    ngx_str_t                application;
    ngx_str_t                name;
    ngx_queue_t              sources;
    ngx_media_source_t      *active;
    ngx_queue_t              consumers;
    ngx_media_timeline_t     timeline;
    ngx_media_selector_t     selector;
    ngx_media_feed_t         program_feed;
    ngx_uint_t               generation;
    unsigned                 running:1;
};
```

### 4.4 Source

```c
struct ngx_media_source_s {
    ngx_media_stream_t      *stream;
    ngx_str_t                id;
    ngx_uint_t               type;
    ngx_uint_t               priority;
    ngx_uint_t               state;
    ngx_msec_t               last_media;
    ngx_msec_t               healthy_since;
    ngx_media_trackset_t    *tracks;
    ngx_media_source_ops_t  *ops;
    void                    *input_ctx;
    unsigned                 healthy:1;
    unsigned                 eligible:1;
    unsigned                 active:1;
};
```

Source types initially: SRT, RTMP, FILE.

---

## 5. Source activation protocol

Do not implement switching as a naked pointer assignment.

Use an explicit forwarding gate inspired by the proven Restream mechanism:

```text
STANDBY
  |
arm
  v
AWAITING_SYNC
  |
keyframe or replay-ready GOP
  v
ACTIVE
```

The gate must also count in-flight writers. Demotion changes the state first, then waits until outstanding writer leases drain. Only then may the replacement become the sole program writer.

Required invariant:

> At no point may packets from the old and new source concurrently enter the logical program feed as active writers.

A producer acquires a short-lived write lease before publishing a frame. Promotion may atomically transition `AWAITING_SYNC -> ACTIVE` when the boundary is acceptable.

Test this state machine independently with randomized/thread-sanitized interleavings.

---

## 6. Standby preroll

Every hot standby source should maintain a bounded **complete-GOP cache**, not merely N seconds of arbitrary packets.

Properties:

- starts only at a video keyframe;
- a new keyframe replaces the previous cached GOP;
- hard byte ceiling;
- hard packet ceiling;
- overflow clears the cache rather than preserving an incomplete GOP;
- audio associated with the GOP is retained;
- sources without video need an explicit alternative activation rule.

This allows immediate failover from a decodable boundary rather than waiting one GOP after failure.

Time-based configuration may be used to derive capacity, but complete decodability is the invariant.

---

## 7. Timeline normalization

Program time must remain monotonic across source switches.

Minimum mapping:

```text
offset = last_program_dts + 1 - first_new_source_dts
program_dts = source_dts + offset
program_pts = source_pts + offset
```

Preserve composition offset:

```text
program_pts - program_dts == source_pts - source_dts
```

Handle:

- B-frames and negative composition offsets;
- PCR/PTS/DTS relationships for MPEG-TS;
- audio/video alignment;
- timestamp wrap;
- source clock discontinuities;
- drift.

The timeline object belongs to the logical stream, not to an input protocol.

Every switch increments `stream->generation`.

---

## 8. Source health and eligibility

Implement health as layered evidence:

```text
TRANSPORT_UP
DATA_FLOWING
CONTAINER_VALID
TIMESTAMPS_ADVANCING
MEDIA_VALID
TRACKS_COMPATIBLE
SOURCE_ELIGIBLE
```

Signals should include, where applicable:

- transport connected;
- bytes progressing;
- PAT/PMT valid;
- PES parsing valid;
- continuity error rate;
- PCR advancing;
- video frames recent;
- audio frames recent;
- keyframe recent;
- PTS/DTS advancing;
- codec configuration known;
- observed track set.

Do **not** choose a source by a blended numeric health score.

Selection policy:

```text
eligible = health policy passes
winner = highest configured priority among eligible sources
```

Add:

- `failure_timeout`;
- `recovery_timeout`;
- `switch_keyframe`;
- `switchback auto|manual|never`.

Priority must come from trusted configuration/control plane, not an encoder-provided Stream ID field.

---

## 9. Compatibility policy

Before a graceful switch, compare the incoming source's track contract with the current program:

- video codec;
- profile/level where relevant;
- resolution;
- frame rate policy;
- audio codec;
- sample rate;
- channel count;
- codec private/config data.

Classify standby sources as READY, DEGRADED or INCOMPATIBLE.

Define policy for hard failure when only an incompatible source remains. The implementation may permit a discontinuous emergency switch, but it must surface that fact to downstream HLS/recording/relay.

---

## 10. MPEG-TS

Implement a lightweight in-process MPEG-TS demuxer early. Minimum:

- 188-byte sync;
- continuity counters;
- PAT;
- PMT;
- PCR;
- PES assembly;
- PTS/DTS extraction;
- H.264;
- H.265;
- AAC;
- corruption/error counters.

No decoding.

Implement a TS muxer usable by HLS and SRT output.

For TS output, batch preparation:

```text
frames
  -> mux burst into one backing allocation
  -> freeze/refcount once
  -> publish slices/descriptors
  -> many consumers
```

Do not allocate/copy one payload buffer per TS chunk if a burst can share one backing allocation.

---

## 11. SRT input and output

### 11.1 Backend abstraction

Keep the public media/SRT boundary independent of Haivision vs. Robotweax.

```text
ngx_media_srt
     |
ngx_media_srt_transport.h
     |
     +-- Haivision libsrt
     +-- Robotweax SRT
```

Use public SRT C API semantics as the compatibility boundary where possible.

### 11.2 Event/runtime integration

Do not pretend an `SRTSOCKET` is a native NGINX fd.

Initial acceptable design:

```text
SRT transport scheduler/helper
        |
SRT readiness/events
        |
bounded queue
        |
eventfd/pipe
        |
NGINX worker
```

The helper performs transport progress and enqueues compact events. It does not parse Stream IDs, modify logical streams, fan out media or perform large allocations.

### 11.3 Shared transport scheduling

For high fanout, design the adapter so one worker/shard-level SRT transport scheduler can own many logical SRT sessions. Restream's current SRT architecture demonstrates why this matters: protocol progress itself is shared schedulable work.

The media core must not assume "one independent event source per receiver" is the only possible backend model.

### 11.4 Bonding

SRT bonding/grouping is below `ngx_media_source_t`.

```text
source encoder_a
        ^
   SRT group
    /     \
 path1   path2
```

The selector sees one source. It never selects individual bond members.

### 11.5 SRT Stream ID

Canonical initial form:

```text
#!::r=live/news,m=publish,s=encoder-a
```

Parse resource, mode and source identity. Authenticate before source registration.

---

## 12. RTMP

Reuse/fork the mature handshake, chunk, AMF and control machinery from nginx-rtmp where practical, but change the internal boundary:

```text
RTMP receive
   -> RTMP adapter
   -> ngx_media_frame_t
   -> media core
```

RTMP playback should consume the logical program, not protocol-specific publisher state.

### 12.1 RTMP fanout

Do not fully serialize/copy every payload per receiver.

Share:

- immutable media payload;
- metadata/config payload where possible.

Keep per connection:

- handshake/control state;
- outbound chunk state;
- partial-write cursor;
- acknowledgements/ping;
- connection-specific headers.

Use `ngx_chain_t`/`writev`-style scatter/gather so small connection-local headers reference shared payload buffers.

Standardize server outbound chunk size where possible to reduce divergent wire state.

---

## 13. Program feed contract

Do not expose raw ring internals to protocol engines.

Define:

```c
typedef struct {
    uint64_t generation;
    uint64_t next_sequence;
} ngx_media_cursor_t;
```

Bounded read API:

```text
read(feed, cursor, max_units, max_bytes)
    -> BATCH
    -> EMPTY
    -> OVERRUN
    -> GENERATION_MISMATCH
```

A cursor belongs to a generation. Source switches and other discontinuities invalidate old cursors explicitly.

Feed retention should have hard limits for:

- units;
- bytes;
- media age.

Track the last sync/keyframe sequence for O(1) resynchronization.

---

## 14. Ring replacement

Support live ring replacement/growth without reconnecting consumers.

Pattern:

```text
old feed/ring
    |
    +---- successor ----> new ring
```

When replacing:

1. allocate new ring;
2. seed readable tail;
3. copy stream/config metadata;
4. atomically install successor;
5. wake old readers;
6. readers migrate cursor to successor;
7. retire old ring after readers leave.

This is useful when initial packet-rate estimates prove too small.

---

## 15. Wake coalescing

Specify the notification protocol, not only the primitive.

```text
publisher:
  append data
  if wake_pending 0 -> 1:
      signal worker

consumer:
  clear wake_pending
  re-read feed head
  if work exists:
      process
  else:
      return/sleep
```

The clear-and-recheck rule is mandatory to avoid lost wakeups.

Implement stress tests around this exact seam.

---

## 16. Fanout execution

One program owner may feed multiple fanout workers.

```text
PROGRAM OWNER
     |
immutable program/feed data
     |
 +---+---+---+
 |       |   |
F0      F1  F2
 |       |   |
receivers...
```

Do not send media over IPC once per subscriber. Cross an ownership boundary once per fanout worker/batch, then perform local fanout.

If a shared-memory ring is used across NGINX processes, it is a special immutable payload path. Do not move normal mutable stream state into shared memory.

---

## 17. Subscriber state and scheduler

A subscriber stores a cursor, not a private media queue:

```c
typedef struct {
    ngx_media_cursor_t cursor;
    size_t             pending_bytes;
    ngx_msec_t         last_progress;
    unsigned           writable:1;
    unsigned           queued:1;
} ngx_media_subscriber_t;
```

Maintain a ready queue. Touch only subscribers that can make progress.

Every scheduler visit receives a three-dimensional budget:

```text
max units
max bytes
absolute deadline
```

Process bounded work and yield to the NGINX event loop.

Fairness across streams should use round-robin/deficit-round-robin style quanta so one hot program cannot monopolize a worker.

---

## 18. Slow receivers

Never allow unbounded subscriber lag.

Classify using:

- application pending bytes;
- native transport pending state;
- feed cursor lag;
- time since progress.

Policy may:

- resynchronize to latest safe keyframe;
- disconnect;
- retry where protocol semantics require.

For RTMP, keyframe resync can be preferable to preserving arbitrarily old TCP backlog.

For SRT, respect backend timing/drop/retransmission semantics; do not create a second unbounded application queue above the protocol library.

---

## 19. HLS

HLS consumes the logical program after source selection.

Classic MPEG-TS HLS first. Design interfaces so fMP4/CMAF/LL-HLS can be added later.

Segment rules:

- begin only from a valid sync boundary;
- normal cut on keyframe after target/min duration;
- hard maximum segment bytes;
- hard maximum segment duration;
- bounded retained segment count/bytes.

On program generation change, HLS receives an explicit source-switch/discontinuity event and inserts `#EXT-X-DISCONTINUITY` when required.

Serve completed segments through ordinary NGINX HTTP/sendfile/cache paths. Large audiences should scale via HTTP caches/CDN, not the live fanout engine.

---

## 20. Recording

Implement explicit tap semantics:

```text
RAW      transport/container bytes before normalized program selection
ISO      one named source before selector
PROGRAM  post-selection, timeline-normalized logical program
```

PROGRAM recording must match what went to air.

Recording I/O must not block the event loop. Use NGINX thread pools/buffered file I/O where appropriate.

On reload/migration, close the current recording part cleanly rather than trying to transfer an active file descriptor.

---

## 21. File input

File input is a first-class source.

Initial efficient path: MPEG-TS file.

```text
file read
 -> TS demux
 -> media frames
 -> timestamp/PCR pacing
 -> source gate
```

Use NGINX timers for pacing; never sleep in the event loop.

Modes:

- ONCE;
- LOOP;
- FOLLOW where meaningful.

Use a worker/thread-pool path for blocking disk reads.

Keep an optional FFmpeg-based broad-format input/transcode boundary outside the core fast path.

---

## 22. Worker ownership

A logical program should have one owner worker for mutable program state:

- sources;
- selector;
- timeline;
- program feed;
- HLS state;
- program recording.

Use deterministic rendezvous/consistent hashing over `application/stream` for preferred ownership.

Shared memory directory contains only small metadata:

```text
stream hash
owner worker/pid/cycle
generation
state
heartbeat/update time
```

Do not store normal mutable media state there.

Transport socket ownership may differ from program ownership. Use internal routing only as an escape hatch.

---

## 23. Inter-worker routing

When a publisher lands on a non-owner worker, use a bounded internal transport. Initial implementation can use Unix-domain `SOCK_SEQPACKET`.

Messages should be versioned and typed:

```text
SOURCE_OPEN
SOURCE_CLOSE
TRACK_CONFIG
FRAME_VIDEO
FRAME_AUDIO
FRAME_DATA
SOURCE_HEALTH
SWITCH_REQUEST
SOURCE_EOF
```

Payloads should be batched/shared where practical.

Do not build distributed consensus. This is intra-host ownership routing.

---

## 24. Reload semantics

Graceful reload must not cause a new worker to steal an active old-worker program immediately.

Owner metadata should include worker slot/PID/cycle generation and heartbeat.

Rules:

- old owner continues active external streams during graceful drain;
- new workers own new streams;
- transfer when external publishers are gone or deliberate migration occurs;
- file fallback alone should not pin ownership forever;
- HLS may preserve minimal sequence/discontinuity metadata;
- recordings roll to a new part.

---

## 25. Control API and observability

Minimum API:

```text
GET  /media/api/v1/streams
GET  /media/api/v1/streams/{app}/{stream}
GET  /media/api/v1/streams/{app}/{stream}/sources
POST /media/api/v1/streams/{app}/{stream}/switch
POST /media/api/v1/streams/{app}/{stream}/switchback
```

Expose:

- active source;
- source states and eligibility;
- health evidence;
- generation;
- track metadata;
- input/output bitrate;
- ring retention/lag;
- fanout worker assignment;
- pending bytes;
- scheduler visit latency;
- deadline misses;
- SRT backend stats;
- HLS segment state;
- recording state;
- failover counters and latency.

Avoid high-cardinality packet-level metrics.

---

## 26. Configuration model

Target shape:

```nginx
media {
    application live {
        dynamic_streams on;

        stream news {
            source encoder_a {
                type srt;
                priority 100;
                bonding broadcast;
            }

            source encoder_b {
                type srt;
                priority 90;
                bonding broadcast;
            }

            source emergency_slate {
                type file;
                path /media/news-slate.ts;
                loop on;
                priority 10;
            }

            failover {
                failure_timeout 1500ms;
                recovery_timeout 10s;
                switch_keyframe on;
                switchback manual;
            }

            hls {
                on;
            }

            record {
                program on;
            }

            output srt {
                on;
            }

            output rtmp {
                on;
            }
        }
    }
}
```

Dynamic streams use application defaults and derive source identity from authenticated publish metadata.

---

## 27. SRT vs. RTMP scaling model

Treat them differently.

RTMP/TCP likely bottlenecks on:

- network bandwidth;
- kernel TCP work;
- socket scheduling;
- memory bandwidth/copies;
- event-loop fairness.

SRT additionally has substantial connection-local protocol work:

- encryption;
- packet sequencing;
- ACK/NAK;
- retransmission;
- congestion/timing;
- TSBPD;
- protocol timers.

Therefore do not use one universal "connections per fanout worker" target.

Admission and sharding thresholds are measured per protocol/bitrate/loss/RTT profile.

---

## 28. Capacity and admission

Primary capacity metric:

```text
fanout_delay = dispatch_time - program_publish_time
```

Track p50/p95/p99/p99.9 plus:

- event-loop delay;
- source-health timer lateness;
- feed lag;
- pending bytes;
- protocol-owner service duration;
- budget exhaustion;
- reconnect population.

A worker that violates its deadline SLA must stop accepting new receivers even if it is below a configured connection maximum.

Use load-aware assignment such as power-of-two choices among eligible fanout workers.

---

## 29. Single-host test topology

No second VM/system is required.

Use Linux network namespaces:

```text
ns-enc-a-isp1 --\
ns-enc-a-isp2 --- SRT bonded source A
ns-enc-b -------- SRT source B
ns-rtmp-pub ----- RTMP source
                   |
                 host NGINX
                   |
ns-view-0 ---------+
ns-view-1 ---------+
...
```

Connect with veth pairs/bridge.

Use `tc netem` independently per path for:

- delay;
- jitter;
- loss;
- reordering;
- duplication;
- rate limit;
- hard link failure.

Loopback is allowed for upper-bound protocol CPU tests, but report it separately from veth/netem results.

---

## 30. Test media

Generate deterministic local media:

- FFmpeg synthetic video/audio;
- fixed MPEG-TS fixtures;
- distinct primary/backup visual/audio identifiers;
- controlled GOP lengths;
- B-frame samples;
- timestamp wrap;
- AV skew;
- continuity errors;
- corrupt PAT/PMT/PES cases.

Use lightweight load sinks for thousands of receivers. Do not spawn one FFmpeg process per receiver for capacity tests.

---

## 31. Test layers

### Unit

- TS parsing;
- timestamp mapping;
- compatibility;
- selector policy;
- health hysteresis;
- ring cursor arithmetic;
- generation mismatch;
- keyframe resync;
- RTMP wire serialization;
- Stream ID parsing.

### Concurrency

- source demote/promote writer exclusion;
- wake coalescing;
- ring replacement;
- reader cancel/data race;
- reference-count lifetime.

Use deterministic stress harnesses and ThreadSanitizer.

### Integration

- SRT ingest;
- bonded SRT;
- RTMP ingest;
- file source;
- HLS;
- recording;
- SRT/RTMP outputs;
- redundant sources.

### Fault injection

- publisher process death;
- frozen publisher;
- one SRT bond leg failure;
- both bond legs failure;
- TS corruption;
- high packet loss;
- slow receivers;
- reconnect storms.

### Performance

- parser;
- TS mux;
- RTMP serializer;
- ring publication/read;
- scheduler;
- fanout ramps.

### Soak

Hours/days with repeated failover, reconnect and slow-client events.

---

## 32. Benchmark matrix

At minimum sweep:

```text
bitrate: 2 / 5 / 10 / 20 / 50 Mbps
GOP: 0.5 / 1 / 2 / 4 s
loss: 0 / 0.1 / 1 / 5 / 10 %
RTT: 0 / 10 / 40 / 80 / 150 / 300 ms
slow receivers: 0 / 1 / 5 / 10 %
RTMP receivers: 100 -> 500 -> 1k -> 2k -> 5k -> deadline failure
SRT receivers: 100 -> 500 -> 1k -> measured hardware/backend limit
```

Also test mixed RTMP+SRT+HLS.

Pin server and load generators to different CPU sets. Record kernel, CPU, governor, affinity, NGINX build flags, SRT backend/version and configuration.

Use `perf`, `pidstat`, `mpstat`, `ss`, `/proc/net`, backend SRT stats and module metrics.

---

## 33. Implementation phases

### Phase 0 - skeleton and invariants

Create module tree, core types, buffer ownership, feed API, tests and build integration.

Exit criteria: unit tests for buffer/feed/cursor/generation semantics.

### Phase 1 - SRT ingest bootstrap

Haivision backend first, Stream ID parsing, one publisher/source, raw TS reception, stats.

Exit criteria: deterministic SRT ingest to internal TS path.

### Phase 2 - MPEG-TS normalization

PAT/PMT/PES/PCR/PTS/DTS plus H.264/H.265/AAC.

Exit criteria: SRT TS -> common frames with correctness fixtures.

### Phase 3 - logical streams and redundant sources

Source registry, gate/leases, complete-GOP standby, timeline mapping, manual promotion.

Exit criteria: two publishers stay hot; manual switch is keyframe-safe and monotonic.

### Phase 4 - health and automatic failover

Eligibility, priority, hysteresis, compatibility, switchback.

Exit criteria: kill/freeze/corrupt active source and recover deterministically.

### Phase 5 - HLS and recording

TS muxer, segmenter, discontinuity generation, PROGRAM/ISO/RAW recording.

### Phase 6 - RTMP ingest/output

Refactor/fork nginx-rtmp protocol machinery into adapters; shared payload fanout.

### Phase 7 - SRT output and fanout

Shared TS preparation, protocol scheduler, bounded subscribers, fanout workers.

### Phase 8 - multi-worker ownership

Deterministic program owner, shared metadata directory, bounded IPC escape hatch.

### Phase 9 - scale hardening

Deadline admission, ring replacement, load-aware fanout placement, soak/fault suite.

### Phase 10 - alternate SRT backend

Qualify Robotweax against Haivision under the same transport interface.

---

## 34. Definition of done

The implementation is not done because streams play.

It is done when:

1. SRT, RTMP and file sources can feed the same logical stream abstraction.
2. Two independent encoders can remain hot for one program.
3. SRT bonding remains transport-internal.
4. Manual and automatic source changes use the same safe promotion path.
5. Old/new active writers never overlap.
6. Switch output begins at a valid sync boundary.
7. Program DTS remains monotonic.
8. Health can detect a connected-but-frozen source.
9. HLS survives failover with correct discontinuity semantics.
10. PROGRAM recording follows the exact selected program.
11. Slow receivers cannot grow memory without bound.
12. Feed overrun/generation mismatch has deterministic recovery.
13. Fanout work is bounded per scheduler visit.
14. Receiver storms do not delay source-health/failover beyond configured SLA.
15. RTMP fanout shares payloads rather than copying media per connection.
16. SRT fanout uses a shared backend scheduler/owner model where supported.
17. Rings and in-progress HLS segments have hard byte/time ceilings.
18. Graceful reload behavior is deterministic.
19. Single-host netns/netem tests reproduce failover and impairment scenarios.
20. Published capacity numbers are accompanied by deadline percentiles and full test conditions.

---

## 35. Engineering guidance to the implementation agent

Prefer small vertical slices that preserve the target abstractions over large temporary shortcuts.

A temporary FFmpeg loopback is acceptable only as a bootstrap seam. Do not let it define the permanent media-core API.

When forced to choose between a clever optimization and a bounded, observable implementation, choose bounded and observable first. Measure before adding io_uring-specific, kernel-bypass or exotic zero-copy mechanisms.

Keep protocol-specific complexity at the edge. The center of the system should remain understandable as:

```text
sources -> safe selection -> continuous program -> bounded shared feeds -> consumers
```

That is the goal.



# Normative Revision: Dynamic Graph and Bidirectional HLS

This section supersedes any earlier implication that streams, sources, or destinations must be declared in static configuration.

## Runtime graph is first-class

The media graph supports runtime create, inspect, update, enable/disable, start/stop, and delete operations:

```text
Application
  |
  +-- Logical Stream                         runtime CRUD
        |
        +-- Source                           runtime CRUD
        |     +-- SRT
        |     +-- RTMP
        |     +-- File
        |     +-- HLS Pull
        |     +-- HLS Push/PUT
        |
        +-- Destination                      runtime CRUD
              +-- SRT
              +-- RTMP
              +-- HLS HTTP PUT/POST
              +-- Local HLS publication
              +-- Recording
```

Static configuration establishes platform capabilities: listeners, authentication, defaults, resource ceilings, worker/shard topology, persistence/control-plane integration, and security policy. It is not the stream database.

A deployment must be able to start with zero statically declared streams and construct the complete graph through the control API. Static declarations are optional bootstrap desired-state objects translated into the same runtime representation. There must not be separate static and dynamic implementations.

Every stream, source, and destination has a stable logical ID distinct from a transport connection/session ID.

## Desired state, observed state, and reconciliation

Separate:

```text
desired state  = persistent/replayable graph requested by controller
observed state = current runtime objects, transport state, health and statistics
```

Every mutation carries an object revision/generation. Support optimistic concurrency so stale controllers cannot silently overwrite newer desired state.

Control operations must be idempotent/reconcilable. A controller may replay desired state after restart without duplicating outputs.

Use a persistence adapter rather than coupling the media core to one database. Initial implementations may use a local durable store or an external controller replay contract.

## Dynamic stream lifecycle

Required API semantics:

```text
POST   /media/api/v1/streams
GET    /media/api/v1/streams/{stream}
PATCH  /media/api/v1/streams/{stream}
DELETE /media/api/v1/streams/{stream}
```

Creating a stream does not require a connected source.

Deleting a stream performs ordered teardown:

1. prevent new children from attaching;
2. stop automatic selector activity;
3. demote/stop sources;
4. drain or cancel destinations according to policy;
5. finalize HLS and recording state;
6. detach feed readers;
7. release program/ring state;
8. remove desired-state record;
9. publish deletion state/event.

Deletion is idempotent.

## Dynamic source lifecycle

Required operations:

```text
POST   /media/api/v1/streams/{stream}/sources
GET    /media/api/v1/streams/{stream}/sources/{source}
PATCH  /media/api/v1/streams/{stream}/sources/{source}
DELETE /media/api/v1/streams/{stream}/sources/{source}
POST   /media/api/v1/streams/{stream}/sources/{source}/enable
POST   /media/api/v1/streams/{stream}/sources/{source}/disable
```

Required source types:

```text
srt
rtmp
file
hls_pull
hls_push
```

Deleting an active source is a selector operation, never a raw object free. If policy permits and an eligible replacement exists, the normal safe failover/promotion path runs first. In-flight writers/callbacks must drain before destruction.

Priority, health thresholds, switchback policy, credentials and endpoint configuration are runtime manageable. Changes that cannot be safely applied in place use replace-and-drain semantics.

## First-class destination lifecycle

Destinations are logical objects rather than only module/config directives.

Required destination types:

```text
srt
rtmp
hls_push
hls_local
record
```

Required operations:

```text
POST   /media/api/v1/streams/{stream}/destinations
GET    /media/api/v1/streams/{stream}/destinations/{destination}
PATCH  /media/api/v1/streams/{stream}/destinations/{destination}
DELETE /media/api/v1/streams/{stream}/destinations/{destination}
POST   /media/api/v1/streams/{stream}/destinations/{destination}/start
POST   /media/api/v1/streams/{stream}/destinations/{destination}/stop
```

Each destination owns lifecycle, health, retry policy, bounded pending state, statistics, and a feed cursor. Failure of one destination must never make the logical program unhealthy.

Deletion cancels timers/retries, stops new scheduling, drains/cancels in-flight work according to policy, detaches the feed cursor, and destroys state only after callbacks can no longer reference it.

## HLS Pull source

`hls_pull` consumes a remote HLS publication:

```text
remote master/media playlist
        |
async HTTP(S) fetch
        |
playlist tracker + rendition selector
        |
bounded segment/part fetch
        |
TS/fMP4 demux
        |
common encoded media representation
        |
normal source health -> selector
```

Requirements:

- master or direct media playlist;
- explicit or policy-driven rendition selection;
- redirects and TLS verification;
- configurable headers/authentication;
- query-token preservation;
- live-edge startup policy;
- bounded download concurrency and bytes;
- playlist reload scheduling;
- media-sequence tracking;
- discontinuity handling;
- retry/backoff;
- upstream stall detection;
- MPEG-TS support in the first milestone;
- fMP4/init-map support as a planned capability;
- VOD/EVENT/LIVE policy;
- hard time/byte limits.

HTTP reachability is not media health. A live playlist that reloads but stops advancing becomes unhealthy.

## HLS Push/PUT source

`hls_push` accepts HLS objects uploaded by an upstream encoder or packager.

Conceptual surface:

```text
PUT/POST /media/ingest/hls/{stream}/{source}/{object}
```

It may accept media playlists, master playlists where useful, MPEG-TS segments, fMP4 init/media objects, and later partial segments.

Requirements:

- authenticate source identity before accepting objects;
- validate object names and content limits;
- hard outstanding object count/byte ceilings;
- sequence ordering and duplicate handling;
- bounded legal out-of-order handling;
- reject pathological future sequence gaps;
- atomic object publication;
- orphan expiry;
- discontinuity handling;
- playlist/media-progress health;
- no mandatory disk write in the hot path;
- optional spool backend.

The HLS ingest adapter demuxes into the same source path used by SRT, RTMP and file. The selector remains protocol-neutral.

## HLS HTTP PUT/POST destination

Remote HLS publishing is distinct from local HLS serving.

```text
selected logical program
       |
HLS packager
       |
bounded publication journal
       |
async HTTPS uploader
       |
remote platform/CDN
```

Required capabilities:

- HTTP PUT and POST;
- HTTPS/TLS verification;
- configurable headers/authentication;
- templated object naming;
- MPEG-TS packaging first and fMP4 profiles where needed;
- rolling playlists;
- playlist/segment publication ordering;
- hard outstanding upload count and byte limits;
- bounded retry/backoff;
- primary/backup endpoint profiles where useful;
- destination health;
- upload latency/backlog metrics;
- graceful drain/cancel;
- runtime CRUD.

Packaging completion must not synchronously wait for remote HTTP completion. If the bounded publication journal is exhausted, destination policy chooses fail, skip/resynchronize, or stop. Remote HLS failure must not create unbounded backpressure into the program.

## YouTube Live HLS destination profile

Provide `youtube_live` as a profile layered on the generic HLS HTTP publisher, not as a special media-core path.

The current YouTube HLS ingest contract should be represented as versioned profile validation/defaults. At this revision it includes:

```text
HTTPS upload
HTTP POST/PUT
MPEG-TS segments
1-4 second segment duration
rolling playlist with no more than 5 outstanding segments
no byte-range HLS
HTTPS transport security rather than HLS content encryption
H.264 and HEVC-capable HLS workflows
```

Platform rules can change; keep them out of the generic media core.

The endpoint/stream credential is secret material. Redact it from normal logs, metrics, API reads, tracing and crash diagnostics.

Do not encode "4K always requires HLS" as a generic rule. The architecture requires HLS publishing because it is an important YouTube ingest workflow and enables HLS-specific HEVC/HDR workflows. Resolution/protocol rules belong in the versioned destination profile.

Example desired-state object:

```json
{
  "type": "hls_push",
  "profile": "youtube_live",
  "endpoint_secret_ref": "youtube/news-primary",
  "segment_format": "mpegts",
  "segment_duration_ms": 2000,
  "playlist_window": 5,
  "http_method": "POST"
}
```

## Reusable HLS components

Avoid a monolithic HLS subsystem. Separate:

```text
playlist parser/tracker
HLS object metadata/journal
HLS packager
HTTP fetch transport
HTTP ingest receiver
HTTP upload transport
local HLS serving
```

HLS pull, HLS PUT ingest, local serving and remote publishing may share parsers/object metadata while keeping independent lifecycle/backpressure state.

## Runtime graph acceptance contract

The implementation is incomplete until these pass:

1. Start with zero statically declared streams; create a stream, source and destination entirely through API.
2. Add a backup source to a live program without interrupting the active source.
3. Delete an inactive source while streaming.
4. Delete the active source and execute policy-driven failover through the normal safe switch path.
5. Add/remove SRT and RTMP destinations while the program continues.
6. Add an HLS pull source at runtime, establish media health and promote it.
7. Upload HLS into an HLS PUT source and make it eligible through the normal health model.
8. Add an HLS HTTP PUT/POST destination while live.
9. Stall the remote HLS server and prove publication backlog remains bounded and other outputs continue.
10. Delete an HLS destination with HTTP operations in flight without leaked callbacks/retries.
11. Restart/reload as applicable and reconcile desired state without duplicate destinations.
12. Reject stale concurrent mutations using object revision.
13. Replayed create/delete requests satisfy documented idempotency semantics.
14. Churn streams/sources/destinations during fanout load without violating health/fanout deadline SLA.
15. Secrets used by HLS destinations never appear in normal diagnostics.

---

# Normative Revision: External Transform Execution and Shared Media Stages

This revision defines the transform architecture.  It does not move codec
execution into an NGINX worker.  NGINX owns control, source arbitration,
timeline/epoch state, bounded compressed journals, package planning and
network egress.  An external transform executor owns decode, scale, audio
processing and encode.

The invariant is:

```text
source arbitration
        |
logical tracks
        |
bounded compressed track journals
        |
shared transform/package plan
        |
prepared compressed feeds
        |
protocol egress
```

CPU software execution is the baseline executor.  Hardware execution is an
optional placement of the same logical plan.  GPU availability, device choice,
NUMA placement and codec implementation/version must never change destination
identity or egress topology.

## External executor boundary

The executor is a supervised child process (or a supervised executor service)
launched by the deployment.  NGINX does not link libavcodec, libavfilter,
NVDEC/NVENC, VAAPI, QSV or another codec SDK.

The process boundary carries compressed media only:

```text
NGINX worker
    |
    | bounded local control/media transport
    v
transform executor
    |
    +-- ffmpeg child: decode -> scale/audio -> encode
    |
    v
encoded rendition journal
```

Raw CPU frames remain inside one executor transform island.  GPU surfaces
remain on the device from decode through scale and encode.  Only compressed
input and compressed output cross the boundary.

The generic executor-service transport is a bounded framed protocol over a
local `SOCK_SEQPACKET` or equivalent local channel.  It MUST NOT be an
unbounded stdin/stdout pipe or an ad-hoc line protocol.

The shipped FFmpeg adapter is intentionally narrower: NGINX launches the
configured FFmpeg executable directly and connects it with nonblocking
stdin/stdout/stderr pipes.  The supervisor's input journal is bounded by both
chunk count and bytes, each tick has an I/O budget, and the adapter never
exposes an unbounded pipe as a generic executor interface.  A future executor
service uses the framed transport above.

Every generic-service message has:

```c
struct media_transform_message {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t length;
    uint64_t sequence;
    uint64_t epoch;
};
```

The implementation MUST validate `magic`, `version`, `type`, `length`,
`sequence`, `epoch` and the maximum message size before allocation or use.
Media payloads are immutable references in NGINX and bounded byte copies at
the process boundary; one receiver never receives one unbounded allocation.

The control types are:

```text
HELLO       executor capability/version negotiation
START       physical transform plan and input contract
INPUT       compressed track-journal units
DISCONTINUITY  program epoch/source boundary
END         end of one input epoch
STOP        drain/cancel one physical stage
```

The executor events are:

```text
READY       accepted physical plan and capability result
OUTPUT      encoded rendition unit
DRAINED     stage stopped at an epoch boundary
ERROR       bounded machine-readable failure
EXIT        executor/ffmpeg child exited
```

An executor MUST identify the physical implementation and version in `READY`.
That identity is part of the physical stage key because CPU and hardware
encoders with nominally equal settings need not produce byte-identical output.

## Supervision and failure

The supervisor owns the process group, control channel, restart backoff and
bounded restart count.  Framed services add heartbeat/deadline handling; the
direct FFmpeg adapter uses its bounded nonblocking I/O budget.  It MUST:

1. launch the configured external executor with a closed inherited descriptor
   set except for the framed contract channel, or the direct adapter's
   stdin/stdout/stderr pipes and explicitly declared resources;
2. put the child in its own process group;
3. detect EOF, protocol failure, service heartbeat failure and abnormal exit;
4. terminate the complete process group, including an ffmpeg descendant;
5. restart with bounded exponential backoff and a hard restart ceiling;
6. increment the physical stage epoch and emit a discontinuity after restart;
7. keep destinations attached to the same logical prepared-feed identity;
8. refuse admission when the executor cannot satisfy the requested capability
   or the configured resource ceiling.

An executor failure MUST NOT free a destination, source or logical stream
while callbacks or journal readers still reference them.  A failed stage
enters `FAILED`/`RESTARTING`; its bounded input/output journals are drained,
discarded or resynchronized according to stage policy.  No process restart
may create an unbounded queue or silently splice bytes from two epochs.

The supervisor MUST never execute a controller-supplied shell string.  The
executable path and the argument vector are configured/validated values;
profiles select structured arguments from an allowlisted capability model.

## Logical track identity

MPEG-TS PID and source-local track index are transport metadata, not stable
downstream identity.  A logical track is identified by program scope plus:

```text
media type
language
role
channel/layout identity
```

Source adapters maintain a source-track-to-logical-track mapping.  A failover
may change PID, stream index or codec configuration while preserving the
logical identity.  The source mapping is replaced; the transform/package
graph is not recreated merely because the source changed.

Each logical track publishes immutable bounded units:

```c
struct logical_media_unit {
    uint64_t       sequence;
    uint64_t       epoch;
    uint32_t       logical_track_id;
    int64_t        pts;
    int64_t        dts;
    uint32_t       flags;       /* keyframe/config/discontinuity */
    ngx_media_buf_t *payload;
};
```

The journal contract is one writer, many cursors, monotonic sequence, explicit
epoch, hard byte/unit/age ceilings, keyframe resynchronization and
`OVERRUN`/`GENERATION_MISMATCH` results.  Track selection is a metadata view:
selecting English and video does not decode, copy or remux Hindi and other
tracks.

A source may become eligible only when its required logical-track contract is
satisfied.  The policy MUST explicitly choose between global ineligibility and
degraded destinations when a backup lacks a required track; it MUST NOT
silently remove a language during failover.

## Shared transform and package stages

The planner hash-conses stages by complete result-affecting keys:

```text
TransformSpec:
    input logical tracks
    codec/profile/level
    geometry/crop/pixel format
    frame rate/GOP/rate control
    color/HDR policy
    audio mapping/sample rate/layout
    quality/encoder behavior
    executor policy
```

The realized physical key is:

```text
PhysicalStageKey = TransformSpec + executor + implementation/version
```

Destination identity, hostname, credentials, retry policy, reconnect policy
and protocol socket state MUST NOT be in a transform key.

Packaging is a separate hash-consed layer.  Examples:

```text
RTMP package key = video representation + selected audio representation
TS package key   = video + audio set + PID/PCR policy + mux settings
HLS package key  = rendition set + segment policy + container format
```

One unique stage/package result is built once and exposed as a prepared feed.
Destinations hold:

```text
prepared_feed_id
cursor
epoch
protocol state
partial write state
retry/health state
```

They MUST NOT hold a pointer to the physical encoder, source PID, or executor
device.  Adding destinations that use an existing prepared feed costs
connection state only.  Deleting the final consumer releases the stage after
its bounded warm-retention policy, if configured.

## API media intent and capability validation

Desired state describes media intent, not an executor command:

```json
{
  "video": {"mode": "profile", "profile": "720p-3m"},
  "audio": {"mode": "select", "languages": ["en"]},
  "executor": "auto"
}
```

`source` means passthrough of the logical source representation.  `profile`
means a validated transform profile.  `select` is a zero-copy logical-track
view; `all` is valid only for packages whose protocol can represent all
selected tracks.  Legacy single-audio protocols MUST reject an incompatible
multi-audio request or require an explicit mix/downmix policy.

Validation and resource admission happen before graph activation.  The
controller receives a bounded capability/resource error rather than a
destination that starts and silently drops tracks.  `auto` chooses
passthrough first, then a capable hardware executor, then the CPU executor.
`hardware_required` fails admission if the requested profile cannot be
realized by hardware.

## Shipped direct FFmpeg adapter profile

The current worker integration exposes a deliberately small first profile:
`"media":"source"` is passthrough and `"media":"profile"` transforms the
selected whole program with the configured external FFmpeg executable.  The
profile is configured by `media_transform_profile`; it is not an executor
command supplied by the API.  One owner creates one external transform and
one prepared package feed for a profile stream, so all destinations of that
stream consume the same transformed result.

Logical-track metadata, hash-consed stage keys and bounded feed primitives are
defined independently of the direct adapter.  The current API does not expose
per-language track selection or hardware-specific capability requests; those
requests MUST NOT be silently interpreted as whole-program passthrough.

## Program epochs and source failover

The logical program epoch is the shared discontinuity boundary for every track,
transform stage, package stage and rendition.  A source switch performs:

```text
program epoch++
replace source-track mapping
emit DISCONTINUITY
resynchronize transform/package cursors
resume prepared feeds
```

Every HLS rendition and every protocol package derived from the program
observes the same epoch.  A physical executor restart uses the same rule.
Destinations remain attached to their prepared feed IDs and do not need to be
recreated.

## Acceptance contract before egress-ladder work

The implementation is not ready for egress scaling work until these pass:

1. A supervisor launches an allowlisted executor and rejects shell injection.
2. A killed ffmpeg child is detected, its process group is reaped, and the
   stage restarts with bounded backoff and a new epoch.
3. A malformed/oversized executor message is rejected without allocation
   growth or worker-loop blockage.
4. Two equal transform requests share one physical stage; differing
   result-affecting keys do not.
5. Track selection does not create a transform or copy unrelated tracks.
6. A source failover changes source mapping and epoch but preserves prepared
   feed/destination identity.
7. Package capability validation rejects impossible multi-track requests before
   destination activation.
8. CPU executor output and hardware executor output use the same logical
   prepared-feed contract.
9. Executor loss leaves unrelated sources, destinations and program health
   bounded and observable.

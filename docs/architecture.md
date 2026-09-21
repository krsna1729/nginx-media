# Architecture

`nginx-media` is an NGINX module that turns redundant live, file and HLS inputs
into one logical program and distributes it over SRT, RTMP, HLS and recording.
This document is the map: what the pieces are, which thread owns what, and where
the boundaries are that keep the design honest.

The invariant everything else follows from:

```
INPUTS                 SOURCES                    SELECTOR            PROGRAM
SRT / bonded SRT       identity -> probe ->       priority +          timeline-normalized
RTMP / file / HLS      health -> eligibility      hysteresis          logical stream
                                                  + switch policy
                                                                          |
                                        +---------------+-----------+-----+------+
                                        |               |           |            |
                                      HLS          recording      RTMP          SRT
```

A **logical stream** is not a publisher.  One logical stream has many
**sources**: two encoders publishing the same program are two sources, a file
slate is another, a pulled HLS stream is another, and SRT bonding is redundancy
*inside* one source.  The selector picks one source at a time; every switch
increments `stream->generation`.

## Modules

| Directory | What lives there |
|---|---|
| `src/core/` | the program model: buffers, frames, tracks, timeline, sources, streams, destinations, the registry, health, compatibility, selection, policy, owner hashing, IPC, routing, the program feed and the runtime tick; also the file source and the HLS ingest, pull and push backends |
| `src/mpegts/` | MPEG-TS: CRC, demux (PSI, PES, PCR, continuity), mux, and the raw-TS ingest queue |
| `src/codec/` | NAL iteration/classification (H.264, H.265) and AAC framing |
| `src/srt/` | the transport contract, its backends, ingest, output destinations and the SRT module |
| `src/rtmp/` | RTMP wire protocol, FLV adapter and the RTMP module |
| `src/hls/` | the segmenter |
| `src/record/` | the recording writer (RAW, ISO, PROGRAM taps) |
| `src/api/` | the HTTP control API, and the `media_hls_ingest` upload endpoint |

`config` in the repository root is the module build description: it lists the
sources, the SRT backend selection and the libraries, and it is what
`--add-module` reads.

## Threads

NGINX workers own media state.  Threads exist only where a blocking call would
otherwise stall an event loop, and most of them hand work back to a worker
through an eventfd rather than touching worker state directly:

| Thread | Created by | Talks to the worker through |
|---|---|---|
| SRT ingest | `ngx_media_srt_ingest.c` | a bounded raw-TS queue plus an eventfd |
| SRT destination sender (one per destination) | `ngx_media_srt_output.c` | a bounded per-destination subscriber queue |
| Recording writer | `ngx_media_record.c` | a work queue |
| HLS ingest reader (one per ingest source) | `ngx_media_hls_ingest.c` | publishes frames through the stream's publish path |
| HLS pull reader (one per pull source) | `ngx_media_hls_pull.c` | publishes frames through the stream's publish path |
| HLS push upload pool (fixed size, shared) | `ngx_media_hls_push.c` | a bounded per-destination queue |

The two HTTP readers are the exception to the eventfd rule: they demux on their
own thread and publish through the stream's own publish path — the same gate a
publisher's frames enter — because the alternative would be to hand raw
segments back and demux them twice.  The push pool is the mirror image of
the SRT senders: one shared pool serves every destination, and each destination
has its own bounded queue so a stalled remote cannot consume another
destination's share.

RTMP, the file source and the HTTP control API run entirely inside worker event
loops: a file source advances from the runtime tick, never from a thread of its
own.  Every thread is joined on the paths that stop it, including reload and
shutdown: a reader is stopped and joined before the source it publishes into is
torn down, and the recording writer is started and stopped repeatedly with work
queued and in flight, under ThreadSanitizer.

## Ownership

A logical program has **one owner worker**, chosen by consistent hashing over
`application/stream`.  The owner holds the mutable state: sources, selector,
timeline, program feed, HLS state and program recording.

Shared memory holds only small metadata — stream hash, owner worker/pid/cycle,
generation, state, heartbeat — never mutable media state.  A publisher that
lands on a non-owner worker is routed to the owner over a bounded internal
transport (Unix `SOCK_SEQPACKET` by default); that escape hatch is for routing
only, not a second data path.

## The runtime graph

What a deployment declares in configuration is small; what it runs is a graph.
A **stream** is the program, and it owns two lists of runtime objects: its
**sources** and its **destinations**.  Stream, source and destination all have a
stable id and are created and addressed at runtime through the control API, not
written into `nginx.conf`, which is what lets a source be added while the
program is live and removed without a reload.

The graph is desired state.  A controller keeps a document of what the graph
should be and applies it; every create in the document is idempotent, so
applying it again changes nothing, and the API can hand the current graph back
in the shape it accepts.  That is what stands in for a database: the graph
survives a controller restart because the controller keeps the document and the
server re-applies it, not because the server stored a desired state it does not
enforce.  A stream absent from the document is not deleted — pruning is the
controller's decision, made with the delete calls, not a side effect of a
replay.

Each object carries a revision, bumped by every mutation of it or of a child,
and a mutation that states the revision it last saw is refused when it is stale
rather than overwriting a newer desired state.  Deletion does not require the
object to exist: the caller asked for an end state and that end state holds,
which is what makes a retry after a timeout safe.

Destinations are the same kind of object as sources, reached through the same
`ngx_media_destination_ops_t` contract the SRT transport uses: the core owns the
model and the list, and a backend registers itself once and is called for every
destination of the types it handles.  A destination type with no backend is
accepted by the API and then fails to start, which is honest about what the
build can carry.

## Data flow

Ingest (SRT): transport thread accepts, reads a stream id, pushes raw TS into a
bounded queue; the worker parses the stream id into an identity, registers the
source, drains the queue, demuxes TS into frames, and feeds the source gate.

Selection: health is layered evidence (transport up, data flowing, container
valid, timestamps advancing, media valid, tracks compatible, source eligible) —
never a blended score.  A source is eligible or it is not; the winner is the
highest configured priority among eligible sources, and priority comes from
operator configuration, never from an encoder-supplied stream id field.

Program: the timeline maps the selected source onto program timestamps by
composition offset, preserving `pts - dts`, and every switch bumps the
generation so downstream taps can resynchronize on a keyframe.

Output: the program is prepared once (TS multiplex for SRT/record taps, FLV for
RTMP, fragmented MPEG-TS for HLS) and each destination consumes its own bounded
queue.  A slow destination is dropped from, never allowed to stall the program.

### Where the other inputs and outputs sit

Everything that reads or writes a file sits at the edge of the program, on the
publishing side of the source gate or the consuming side of the fanout:

- A `file` source reads one bounded chunk of an MPEG-TS file per runtime tick,
  demuxes it and publishes the frames through the same source gate a publisher
  feeds.  Pacing is the tick and never a sleep, so a large file cannot stall a
  worker, and a slow file is simply a source that is not producing yet.
- An `hls_pull` source fetches a playlist and its segments over HTTP or HTTPS
  on its own thread, demuxes them and publishes them the same way.
- An uploaded segment arrives through the `media_hls_ingest` endpoint: nginx
  writes the body to a temp file and the handler renames it into the ingest
  directory, so a reader sees a whole segment or none of it.  A reader of type
  `hls_push` then demuxes it like any other source.  The writer and the reader
  are separate halves on purpose — the ingest endpoint does not know what reads
  the directory.
- An `hls_push` destination watches the HLS output directory and uploads
  segments to its endpoint on a bounded pool, each destination with its own
  bounded queue.

Because all of these publish through the source gate or consume from the
fanout, selection, health and compatibility need to know nothing about where
the bytes came from, and nothing here can change what goes to air: the HLS
output is prepared from the program feed on the runtime tick, strictly
downstream of selection.

## Failure behaviour

- A source that stops producing fails on `failure_timeout` and the selector
  moves on; switching back is governed by `switchback` (`auto`, `manual`,
  `never`) and `recovery_timeout`.  A `file` source that reaches the end of its
  file stops producing and fails the same way, so a one-shot slate hands over
  like a publisher that went away.
- A destination that cannot keep up loses units from its own queue and counts
  them; the program is unaffected.  An `hls_push` destination whose remote has
  stalled loses its oldest queued segment and counts it, and a delete stops it
  and unlinks it first so nothing it queued outlives it.
- Recording I/O never runs on the event loop, and reload closes the current
  recording part cleanly instead of transferring a live descriptor.
- Nothing in the media path allocates per packet or blocks a worker.

## HLS push at fanout: does the copy path show up?

`tests/bench/hls_push_fanout.sh` runs two builds of the same code with one
function changed - the uploader's body transfer - against 8 destinations
receiving the same segments, and reports worker CPU per uploaded segment,
which is where a copy shows up.

| Uploader | Uploads | CPU per upload |
|---|---|---|
| `sendfile` | 48 | 0.3908 s |
| `fread` / `write` | 40 | 0.4702 s |

**About 17% less CPU per upload with sendfile**, and more uploads completed
over the same media.  `sendfile` is not a server-side privilege: it works on
any socket, including the client connection an upload uses, so the page cache
hands its pages straight to the socket and an upload costs one kernel-side
copy per destination instead of two.

Two honest caveats.  The upload counts differ between runs because the
directory scan is timing-dependent, so this is indicative rather than a
controlled A/B; and total worker CPU was nearly identical (18.76 s vs 18.81 s)
because the media pipeline - ingest, demux, mux - dominates, so the upload
copy is a small slice of the whole.  The saving is real and it is the right
default; it is not where the time goes.

## Serving HLS at fanout: what the measurements say

`make bench-hls-fanout` answers the question the sequential bench cannot:
many concurrent readers over a realistic sliding window (40 segments, 20 MiB,
32 concurrent clients, 4 rounds, best of three passes, with the server's
access log used to prove every request actually arrived).

| Variant | Throughput | Requests/s |
|---|---|---|
| disk, `sendfile` off | 3077 MiB/s | 6154 |
| disk, `sendfile on` + `tcp_nopush` | 2759 MiB/s | 5517 |
| tmpfs, `sendfile on` | 2857 MiB/s | 5714 |
| tmpfs, HTTPS with kTLS | 1127 MiB/s | 2254 |

- **Still no advantage from a memory filesystem**, and none from `sendfile`,
  even at 32 concurrent readers over a working set.  The differences are
  within run-to-run noise; the disk and tmpfs variants are the same speed.
- **TLS costs more at fanout than it does single-stream**: about 63% here
  against 38% sequentially, because the crypto is CPU-bound and concurrency
  multiplies that against a fixed core count.
- **The honest limit of this measurement**: it is loopback.  `sendfile`'s real
  value is avoiding the kernel-user copy on the way to a NIC, and loopback has
  no NIC to skip.  A deployment serving over a real interface should expect
  `sendfile` to matter more than this bench can show; what this bench *does*
  establish is that it never costs anything, and that tmpfs is not the lever.

## Serving HLS: what the measurements say

`make bench-hls` serves the same segment many times over each candidate
configuration.  On this machine (400 requests of a 3.3 MiB segment):

| Variant | Throughput | Per request |
|---|---|---|
| disk, `sendfile` off | 690 MiB/s | 4.69 ms |
| disk, `sendfile on` + `tcp_nopush` | 686 MiB/s | 4.71 ms |
| tmpfs, `sendfile on` | 678 MiB/s | 4.77 ms |
| tmpfs, HTTPS with kTLS | 429 MiB/s | 7.55 ms |
| disk, cold page cache | 648 MiB/s | 4.99 ms |

- **A memory filesystem does not help.**  Segments served from disk come out
  of the page cache at the same speed; tmpfs measured *slower* here, within
  noise.  It is not worth the operational cost of a RAM-backed directory that
  fills up and disappears on reboot.
- **`sendfile` is free but not decisive** for this working set: the segments
  are cached, so it is within noise of read+write.  It costs nothing and helps
  when the cache is cold or the files are larger, so enable it — just do not
  expect it to be the lever.
- **TLS is the lever.**  It costs about 38% against plain HTTP even with kTLS,
  because the crypto runs in software and this NIC has no offload.  kTLS still
  buys about 9% over userspace OpenSSL, and needs the `tls` kernel module
  loaded (`modprobe tls`) — without it nginx silently falls back and the cost
  is higher.

## Transport backends

The SRT transport is a contract (`ngx_media_srt_ops_t` in
`src/srt/ngx_media_srt_transport.h`), not a library call.  Haivision/srt is the
reference implementation and the production default; robotweax/srt exposes the
same C API, so it is selected by pointing the build at its headers and library
with no code change.  A UDP implementation of the same contract exists purely
as a test double so the qualification suite can run one scenario against two
implementations; it is not an SRT implementation and is not built by default.
See `configuration.md` for the build and runtime switches.

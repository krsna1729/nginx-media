# Architecture

`nginx-media` is an NGINX module that turns redundant live and file inputs into
one logical program and distributes it over SRT, RTMP, HLS and recording.  This
document is the map: what the pieces are, which thread owns what, and where the
boundaries are that keep the design honest.

The invariant everything else follows from:

```
INPUTS                 SOURCES                    SELECTOR            PROGRAM
SRT / bonded SRT       identity -> probe ->       priority +          timeline-normalized
RTMP / file            health -> eligibility      hysteresis          logical stream
                                                  + switch policy
                                                                          |
                                        +---------------+-----------+-----+------+
                                        |               |           |            |
                                      HLS          recording      RTMP          SRT
```

A **logical stream** is not a publisher.  One logical stream has many
**sources**: two encoders publishing the same program are two sources, a file
slate is another, and SRT bonding is redundancy *inside* one source.  The
selector picks one source at a time; every switch increments
`stream->generation`.

## Modules

| Directory | What lives there |
|---|---|
| `src/core/` | the program model: buffers, frames, tracks, timeline, sources, streams, the registry, health, compatibility, selection, policy, owner hashing, IPC, routing |
| `src/mpegts/` | MPEG-TS: CRC, demux (PSI, PES, PCR, continuity), mux, and the raw-TS ingest queue |
| `src/codec/` | NAL iteration/classification (H.264, H.265) and AAC framing |
| `src/srt/` | the transport contract, its backends, ingest, output destinations and the SRT module |
| `src/rtmp/` | RTMP wire protocol, FLV adapter and the RTMP module |
| `src/hls/` | the segmenter |
| `src/record/` | the recording writer (RAW, ISO, PROGRAM taps) |
| `src/api/` | the HTTP control API |

`config` in the repository root is the module build description: it lists the
sources, the SRT backend selection and the libraries, and it is what
`--add-module` reads.

## Threads

NGINX workers own media state.  Threads exist only where a blocking call would
otherwise stall an event loop, and each one hands work back to a worker through
an eventfd rather than touching worker state directly:

| Thread | Created by | Talks to the worker through |
|---|---|---|
| SRT ingest | `ngx_media_srt_ingest.c` | a bounded raw-TS queue plus an eventfd |
| SRT destination sender (one per destination) | `ngx_media_srt_output.c` | a bounded per-destination subscriber queue |
| Recording writer | `ngx_media_record.c` | a work queue |

RTMP and the HTTP control API run entirely inside worker event loops.  Every
thread is joined on the paths that stop it, including reload and shutdown; the
unit lifecycle suite starts and stops them repeatedly, with work queued and in
flight, under ThreadSanitizer.

## Ownership

A logical program has **one owner worker**, chosen by consistent hashing over
`application/stream`.  The owner holds the mutable state: sources, selector,
timeline, program feed, HLS state and program recording.

Shared memory holds only small metadata — stream hash, owner worker/pid/cycle,
generation, state, heartbeat — never mutable media state.  A publisher that
lands on a non-owner worker is routed to the owner over a bounded internal
transport (Unix `SOCK_SEQPACKET` by default); that escape hatch is for routing
only, not a second data path.

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

## Failure behaviour

- A source that stops producing fails on `failure_timeout` and the selector
  moves on; switching back is governed by `switchback` (`auto`, `manual`,
  `never`) and `recovery_timeout`.
- A destination that cannot keep up loses units from its own queue and counts
  them; the program is unaffected.
- Recording I/O never runs on the event loop, and reload closes the current
  recording part cleanly instead of transferring a live descriptor.
- Nothing in the media path allocates per packet or blocks a worker.

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

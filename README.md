# nginx-media

Production-grade media subsystem for NGINX: redundant live and file inputs,
one logical program, and bounded fanout to SRT, RTMP, HLS and recording.

The architectural invariant (goal document, section 1):

    INPUTS -> SOURCES -> SELECTOR -> PROGRAM -> HLS / recording / RTMP / SRT

A logical stream is not a publisher: one program may have several independent
sources, source health is not source selection, and fanout must never copy or
queue media per subscriber in steady state.

Full requirements: [nginx_media_implementation_agent_goal_v2.md](nginx_media_implementation_agent_goal_v2.md).

## Status

Phase 0 — skeleton and invariants (complete):

- core object model (`src/core/ngx_media.h`): payload buffer, frame, track set,
  bounded feed, and the stream/source/selector/timeline types
- reference-counted immutable payload buffers; payloads are shared, never
  copied per receiver
- bounded program feed with cursor/generation semantics and retention
  ceilings (units, bytes, media age): `BATCH` / `EMPTY` / `OVERRUN` /
  `GENERATION_MISMATCH`, with O(1) keyframe resynchronization
- unit tests (ASan/UBSan clean) and nginx module build integration

Phase 1 — SRT ingest bootstrap (complete):

- Haivision libsrt backend behind the protocol-neutral transport adapter
  (`src/srt/ngx_media_srt_transport.h`), Stream ID parsing for
  `#!::r=live/news,m=publish,s=encoder-a`
- bounded, spinlock-protected raw-TS handoff queue between transport and
  worker (`src/mpegts/ngx_media_ts_ingest.*`); drop-and-count, never blocks
- SRT ingest inside nginx: `media_srt_listen host:port;`, worker 0 owns the
  listener (goal doc 22); a transport helper thread accepts publishers and
  wakes the worker through an eventfd with compact events (raw Stream ID) and
  payload chunks; the worker parses/validates the Stream ID, registers the
  publisher and drains the bounded queue
- `make srt-ingest` (transport) and `make srt-ingest-nginx` (in-nginx) prove
  it deterministically with ffmpeg as publisher: parsed source identity,
  session and drain byte counts agree, zero drops, clean worker shutdown
- next: phase 2 attaches the MPEG-TS demuxer to the drained chunks

Configuration (phase 1 spelling; the goal document's `media {}` block arrives
with the stream database):

    media_srt_listen 127.0.0.1:9000;

Phase 2 — MPEG-TS normalization (complete):

- `src/mpegts/ngx_media_ts_demux.*`: packet layer (sync/resync, transport
  errors, continuity counters, adaptation fields, PCR), PSI (PAT/PMT with
  CRC-32 validation), PES assembly bounded by `PES_packet_length`, and
  access-unit emission
- `src/codec/ngx_media_nal.*`: Annex B NAL iteration and classification for
  H.264 and H.265 (configuration NALs, VCL, IDR/IRAP sync points);
  `src/codec/ngx_media_aac.*`: ADTS framing and AudioSpecificConfig synthesis
- output is the common encoded-media representation: `ngx_media_frame_t` with
  explicit payload formats (Annex B video, ADTS audio, raw config), track sets
  carrying codec configuration, and raw 33-bit PTS/DTS (unwrapping belongs to
  the stream timeline)
- the SRT worker drains ingested chunks directly into the demuxer and reports
  per-session frame, keyframe and error counters
- `make ts-fixture`: ffmpeg-generated H.264+AAC and H.265+AAC MPEG-TS demux to
  100/189 and 50/95 frames with zero transport, continuity, PSI, CRC and PES
  errors; `make srt-ingest-nginx`: 3 s of H.264+AAC over SRT yields 65 video
  and 110 audio frames, 3 keyframes and zero errors inside nginx

Phase 3 — logical streams and redundant sources (complete):

- `src/core/ngx_media_stream.*`: logical stream with a source registry, the
  program feed and the single write path into it; a stream has one active
  source, any number of hot standbys
- `src/core/ngx_media_source.*`: complete-GOP standby cache (starts at a video
  keyframe, replaced wholesale by a newer keyframe, cleared — never kept
  partial — when a ceiling is hit) plus short-lived write leases
- `src/core/ngx_media_timeline.*`: program time stays monotonic across
  switches (`offset = last_program_dts + 1 - first_new_source_dts`), preserves
  the composition offset, and re-anchors instead of regressing on source clock
  discontinuities and 33-bit timestamp wrap
- a promotion completes only at a decodable boundary and only after the
  demoted source's leases drain; a real switch bumps the generation and
  invalidates readers' cursors (the initial activation is not a switch)
- `src/core/ngx_media_registry.*`: per-worker runtime stream registry (the
  goal document's normative revision: configuration is capability, the stream
  graph is runtime state)
- `src/srt/`: the transport helper now runs **one shared poll over the
  listener and every session** (goal doc 11.3) instead of one blocking loop
  per publisher; payload chunks carry their session id and the worker routes
  each session to its own demuxer, source and stream
- `src/api/ngx_media_api_module.c`: `media_api` location handler exposing
  `GET /media/api/v1/streams`, `GET .../{app}/{stream}`,
  `GET .../{app}/{stream}/sources` and
  `POST .../{app}/{stream}/switch?source=<id>` (bounded JSON, explicit status
  codes; `switchback` reports not-implemented until automatic failover lands)
- `make source-switch`: two live sources demuxed from real H.264+AAC fixtures,
  both hot; one manual promotion at frame 41 → generation 2, 0 DTS regressions
  over 475 program frames, the switch starts on a keyframe, the demoted source
  stops writing but keeps a hot 77-unit GOP cache
- `make api-switch`: two real SRT publishers stay hot on `live/news`; the API
  lists both, reports encoder-a active and encoder-b standby with a bounded
  GOP cache, switches to encoder-b (generation 1 → 2, switches 1), and rejects
  unknown sources (404), wrong methods (405) and unknown streams (404)
Phase 4 — health, eligibility and automatic failover (complete):

- `src/core/ngx_media_health.*`: layered evidence (transport up, data flowing,
  container valid, timestamps advancing, media valid, tracks compatible) — no
  blended score.  A closed transport fails immediately; a silent or frozen
  source (connected but no progress) fails after `failure_timeout`; a failed
  source needs `recovery_timeout` of good evidence again (hysteresis)
- `src/core/ngx_media_compat.*`: READY / DEGRADED / INCOMPATIBLE classification
  against the program's track contract.  Codec and media type are hard
  requirements; resolution, profile/level, audio parameters and payload
  representation changes are degradations
- `src/core/ngx_media_selector.*`: `eligible = health policy passes`,
  `winner = highest-priority eligible source`.  Failover when the active
  source loses eligibility, switchback per policy (auto / manual / never), and
  an emergency switch when only an incompatible source remains — counted and
  surfaced through the generation change
- configuration: `media_failover_failure_timeout` and
  `media_failover_recovery_timeout` (bare values are milliseconds, `700ms` and
  `2s` are accepted), `media_failover_switch_keyframe`,
  `media_failover_switchback`, and `media_srt_source_priority <identity> <n>`
  (priority comes from operator configuration, never from the Stream ID)
- the control API exposes per-source `healthy`, `eligible`, `compat`,
  `evidence`, `health_transitions` and `container_errors`, stream
  `emergency_switches` and the active policy, and implements
  `POST .../switchback`
- `make failover` proves the exit criteria with two prioritized publishers:
  killing the active one fails over to the standby (generation 2), the
  returning primary is switched back to automatically, a frozen publisher
  (SIGSTOP) fails after the failure timeout and recovers on thaw, and a
  garbage publisher registering as the primary is reported unhealthy
  (evidence level 37 of 63) while the program keeps flowing on the standby
Phase 5 — HLS and recording (complete):

- `src/mpegts/ngx_media_ts_mux.*`: PAT/PMT, PES packetization with PTS/DTS,
  adaptation-field padding and PCR, continuity counters, and batched bursts
  that own one backing allocation (goal doc 10); a frame that does not fit is
  rejected cleanly, rolling the burst back to the previous frame boundary
- `src/hls/ngx_media_hls_segmenter.*`: segments begin only at a video sync
  boundary, cut on a keyframe after `min_duration`, and are bounded by
  `max_duration` and `max_segment_bytes`; the playlist window (count and bytes)
  evicts segments and removes their files; a generation change closes the
  segment and the next one is announced with `#EXT-X-DISCONTINUITY`; a segment
  that starts mid-burst repeats the PAT/PMT prefix so it stays decodable
- `src/record/ngx_media_record.*`: RAW / ISO / PROGRAM taps with a bounded job
  queue and a writer thread, so recording I/O never blocks the event loop;
  parts roll at a hard byte ceiling and are closed cleanly
- `make unit` covers the muxer round trip, segment boundaries, playlist and
  discontinuity handling, segment decodability (segments are read back through
  the demuxer), retention eviction and the recorder's bounded queue, part
  rolling and written content
- `make ts-fixture` also remuxes the real fixtures (demux -> mux -> demux) and
  requires every frame to match exactly
- `media_hls <dir>`, `media_record <path>`, `media_record_raw <path>` and
  `media_record_iso <source> <path>` wire the outputs into the worker: the
  selection tick drains each stream's program feed, muxes one burst and hands
  it to HLS and to the PROGRAM recording; the ISO tap muxes the named source
  before the selector and the RAW tap records transport bytes before
  normalization, and every output is closed cleanly on exit
- `make hls` proves the exit criteria through nginx: a publisher is killed and
  restarted mid-run, the playlist announces `#EXT-X-DISCONTINUITY` after the
  switch, a segment fetched over HTTP is byte-identical to the file on disk and
  probes as H.264 + AAC, and the PROGRAM, ISO and RAW recordings all probe as
  H.264 + AAC
- heap poisoning in the unit shim (`NGX_MEDIA_TEST_POISON`) caught an
  unterminated path handed to `open()`; the HLS test now runs with it enabled
Phase 6 - RTMP ingest and output (complete):

- `src/rtmp/ngx_media_rtmp_wire.*`: the protocol layer, free of nginx event
  types so it can be unit tested against synthetic byte streams - simple and
  Adobe digest handshakes (HMAC-SHA256 over the media server and flash player
  keys, with the digest offset derived from the packet), chunk streams in all
  four header formats with extended chunk ids and timestamps, chunk size and
  abort control messages, message assembly across partial socket reads, and a
  bounded AMF0 reader/writer
- `src/rtmp/ngx_media_rtmp_adapter.*`: avcC and AudioSpecificConfig handling,
  AVCC <-> Annex B reframing, FLV message bodies, ADTS framing for raw AAC,
  the publisher state machine and the bounded fanout ring
- publishers normalize into the representation the MPEG-TS path uses (Annex B
  video, ADTS audio, config frames plus a track contract carrying the parameter
  sets as an Annex B blob), so an RTMP source is interchangeable with an SRT
  source for the selector, HLS, recording and SRT output (goal doc 34 item 1)
- `src/rtmp/ngx_media_rtmp_module.c`: `media_rtmp_listen` and
  `media_rtmp_source_priority`; `connect`/`createStream`/`publish`/`play` are
  served with the control messages clients expect (window acknowledgement,
  set peer bandwidth, chunk size, stream begin, status), publishers register as
  sources of the same logical streams, and players read the program
- players share payloads: the program is converted to FLV once per stream and
  every connection adds only its own chunk headers, referencing the shared
  media in place (goal doc 12.1)
- `src/core/ngx_media_runtime.*`: one owning-worker timer drives the selection
  tick, the per-stream outputs and the FLV preparation for every transport
- `make rtmp` proves the exit criteria through nginx: an ffmpeg publisher
  appears in the control API with its configured priority, the program reaches
  HLS, and a second ffmpeg plays the program back over RTMP and decodes H.264
  and AAC video/audio packets
Phase 7 - SRT output and fanout (complete):

- `media_srt_output <application/stream> <host:port> [streamid]` adds an SRT
  destination: a caller session that announces a stream id, so the receiving
  side registers it as a normal source
- destinations consume the transport bursts the program runtime already
  prepares for HLS and recording, so one preparation feeds every consumer and
  a destination costs one reference per burst, never a copy (goal doc 16)
- `src/srt/ngx_media_srt_output_queue.*`: a bounded subscriber queue with hard
  unit and byte ceilings; a slow receiver drops whole bursts and resumes at the
  next sync boundary, so it never starts mid-GOP (goal doc 34 items 11, 12)
- `src/srt/ngx_media_srt_output.*`: one sender thread per destination, so a
  stalled receiver cannot delay another output; sends are payload sized, and
  transport backpressure (a timeout or a full send queue) is retried instead of
  dropping media
- the SRT transport gained the caller side (`connect`/`send`) and reports the
  last failure as a string, so the adapter itself never logs
- `make srt-output` proves the exit criteria with two nginx instances: A
  ingests an SRT publisher, prepares the program once and pushes it to two
  destinations; B receives one of them, registers the announced source and
  produces its own decodable HLS output while A's HLS shows the shared
  preparation
Phase 9 - scale hardening (in progress):

Hardening measurements recorded so far:

- `make unit` runs every suite under ASan/UBSan: 23 suites, 0 failures
- the concurrent suites (record writer, lifecycle cycles, program feed, stream
  registry) also run clean under `-fsanitize=thread`, with the binaries
  verified as instrumented; no data races reported
- `tests/unit/test_lifecycle.c` starts and stops a concurrent writer ten times
  with work queued, repeats a stop, stops with work in flight and appends after
  a stop - the shapes that previously produced a shutdown hang and a double
  free
- `tests/unit/test_fuzz.c` fuzzes every parser that consumes network bytes
  (RTMP chunk reader, AMF0, SRT stream id, MPEG-TS demux, IPC reassembly) with
  random data and mutations of valid input: 14289 checks, no crash, overrun or
  leak
- `tests/unit/test_bounds.c` exercises every hard bound at its limit and one
  step past it, and fails every allocation through the shim so NULL handling is
  proven (20 checks)
- `make soak` churns publishers (alternating sources) through six cycles with
  two configuration reloads in the middle and measures the worker: no crash, no
  abort, memory flat (4712 kB -> 4540 kB across the run), HLS and recording
  still produced, clean shutdown
- `make fault` injects mid-stream faults: a publisher is stalled (SIGSTOP) and
  the program fails over through the normal path, a publisher whose transport
  bytes are corrupted on the wire is ignored while the program keeps serving
  from the other source, and the remaining source is killed outright; through
  all of it the worker stays up, HLS keeps producing and shutdown is clean

Phase 8 - multi-worker ownership (complete):

- `src/core/ngx_media_owner.*`: ownership is FNV-1a over `application/stream`,
  so every worker computes the same owner without coordination and across
  restarts; the worker count comes from nginx's core configuration
- `src/core/ngx_media_owner_dir.*`: a shared metadata directory holding only
  small bookkeeping - owner slot and pid, generation, source count, frames and
  a heartbeat - allocated by the master before workers fork; a stale owner is
  reclaimable, so a crashed worker cannot pin a stream (goal doc 22)
- `src/core/ngx_media_ipc.*`: the bounded inter-worker transport, versioned and
  typed over a Unix domain `SOCK_SEQPACKET` pair; frames larger than one
  datagram are chunked with a continuation flag and reassembled or rejected
  deterministically, and backpressure is reported instead of queueing without
  bound (goal doc 23)
- `src/core/ngx_media_route.*`: routing pairs are created in the master, adopted
  per worker and registered with the worker's event loop; OPEN, TRACKS, CLOSE
  and frames travel to the owner, which rebuilds the source locally and feeds it
  through the normal path - the selector, feed, HLS and recordings only ever see
  local state
- the runtime timer now runs in every worker and drives only the programs that
  worker owns; transports keep accepting on worker 0, so transport socket
  ownership differs from program ownership exactly as the goal doc describes
- `make multi-worker` proves the exit criteria with two workers: the publisher
  is routed to the worker its hash selects, the owner receives the source, its
  track contract and every frame, and produces a decodable HLS output
- bugs found by the pass: the IPC datagram bound omitted the header (truncated
  datagrams), the routed sink dropped frames while the source was still awaiting
  its first keyframe (the gate could never open), a double unref at the IPC
  boundary corrupted the heap until malloc aborted the worker, and the OPEN
  payload was sized one byte too large
- next phase: scale hardening


## Layout

    config                nginx add-on config
    src/api/              control API (HTTP location handler)
    src/core/             portable media core (goal doc section 3)
    src/hls/              MPEG-TS HLS segmenter
    src/record/           RAW / ISO / PROGRAM recording taps
    src/codec/            Annex B NAL and ADTS framing helpers
    src/srt/              SRT transport adapter and Stream ID parsing
    src/rtmp/             RTMP wire layer, FLV adapter and transport module
    src/mpegts/           MPEG-TS demux and ingest path
    tests/unit/           unit tests and the test-only nginx shim
    tests/integration/    integration, smoke, ingest and fixture harnesses
    scripts/              developer helper scripts

## Build and test

    make unit             # unit tests, ASan/UBSan; no nginx required
    make nginx            # fetch pinned nginx source, build it with this module
    make smoke            # start the built nginx, serve a request, stop it
    make srt-ingest       # transport-level ffmpeg-over-SRT ingest test
    make srt-ingest-nginx # SRT ingest through nginx, demuxed into frames
    make ts-fixture       # demux ffmpeg-generated H.264/H.265 MPEG-TS files
    make source-switch    # two hot sources, manual switch, timeline checks
    make api-switch       # control API: listing and manual switch via nginx
    make failover         # kill / freeze / corrupt failover and switchback
    make hls               # HLS playlist, discontinuity, recordings over nginx
    make rtmp              # RTMP publish and play of the same logical stream
    make srt-output        # SRT destinations fed from the shared preparation
    make multi-worker      # two workers: owner routing across the IPC transport

Requires `cc`, `make`, `curl`, `tar`, `ffmpeg` and (for the SRT targets)
`libsrt`. `make nginx` builds nginx 1.30.5 with `--add-module` into `.build/`;
`clean` removes `.build/`.

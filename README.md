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

## Layout

    config                nginx add-on config
    src/core/             portable media core (goal doc section 3)
    src/srt/              SRT transport adapter and Stream ID parsing
    src/mpegts/           MPEG-TS ingest path
    tests/unit/           unit tests and the test-only nginx shim
    tests/integration/    integration, smoke and ingest harnesses
    scripts/              developer helper scripts

## Build and test

    make unit             # unit tests, ASan/UBSan; no nginx required
    make nginx            # fetch pinned nginx source, build it with this module
    make smoke            # start the built nginx, serve a request, stop it
    make srt-ingest       # transport-level ffmpeg-over-SRT ingest test
    make srt-ingest-nginx # same publisher, but through nginx + the worker

Requires `cc`, `make`, `curl`, `tar` and, for the SRT targets, `libsrt` and
`ffmpeg`. `make nginx` builds nginx 1.30.5 with `--add-module` into `.build/`;
`clean` removes `.build/`.

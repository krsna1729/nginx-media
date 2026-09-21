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

Phase 0 — skeleton and invariants:

- core object model (`src/core/ngx_media.h`): payload buffer, frame, track set,
  bounded feed, and the stream/source/selector/timeline types
- reference-counted immutable payload buffers; payloads are shared, never
  copied per receiver
- bounded program feed with cursor/generation semantics and retention
  ceilings (units, bytes, media age): `BATCH` / `EMPTY` / `OVERRUN` /
  `GENERATION_MISMATCH`, with O(1) keyframe resynchronization
- unit tests (ASan/UBSan clean) and nginx module build integration

## Layout

    config                nginx add-on config
    src/core/             portable media core (goal doc section 3)
    tests/unit/           unit tests and the test-only nginx shim
    tests/integration/    integration/smoke scripts
    scripts/              developer helper scripts

## Build and test

    make unit     # unit tests, ASan/UBSan; no nginx required
    make nginx    # fetch pinned nginx source, build it with this module
    make smoke    # start the built nginx, serve a request, stop it

Requires `cc`, `make`, `curl`, `tar`. `make nginx` builds nginx 1.30.5 with
`--add-module` into `.build/`; `clean` removes `.build/`.

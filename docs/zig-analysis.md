# Zig analysis: what it would and would not buy for nginx-media

Analysis only.  Nothing here has been implemented; the point is to record what
was actually studied and what the evidence supports, so the decision is not
made on vibes.

## What was studied

Not blog posts alone.  For Mitchell Hashimoto's practice the sources were:

- **The code**: `ghostty-org/ghostty` cloned at 200 commits of history — 591
  Zig files, 43 MB under `src/`, including `src/terminal/` (the core),
  `src/crash/` (crash handling with Sentry), `test/fuzz-libghostty`, and
  `testdata` directories per subsystem.
- **The tests**: 1658 `test` blocks in `src/terminal` alone, spread over 350
  files — the convention is inline tests at the bottom of the file they test,
  not a parallel test tree.
- **The commits**: `subsystem: imperative summary` with a body that explains
  the *why*, the investigation, and links to the discussion thread that
  produced it.
- **The build**: `minimum_zig_version = "0.16.0"` in `build.zig.zon`,
  enforced by `requireZig()` at build time; `zig build test` with
  `-Dtest-filter` for targeted runs; a CI matrix over build options.
- **The talk**: "Introducing Ghostty and Some Useful Zig Patterns" (Zig
  Showtime, Sept 2023) — comptime interfaces, comptime data tables, comptime
  type generation, and the compiler's lazy analysis of unreferenced code.
- **AGENTS.md**: their agent guidance, which is explicitly hostile to
  AI-authored issues and PRs.  Noted and irrelevant here: this is study, not
  contribution.
- **Superlogical**: the company announced 2026-07-29, building a terminal
  multiplexer at scale on top of libghostty (MIT).  It is not Ghostty going
  commercial — Ghostty was donated to a nonprofit — and the interesting part
  for us is architectural: the terminal core is a *library* with a C API, and
  the multiplexer is a separate layer above it.

## The style, concretely

1. **Comptime interfaces for anything that never changes at runtime.**
   Platform-specific fonts, renderers and app runtimes are selected by build
   option, and dispatch collapses to a direct call at compile time.  Their
   warning is as important as the pattern: because Zig only analyzes
   referenced code, untested build options can hide broken paths, so CI runs
   the full option matrix.
2. **Comptime data tables.**  Tables are written in a convenient shape and
   transformed at compile time into the runtime shape, including generating
   entries programmatically instead of with a code-generation script.  Raw
   forms cost nothing in the binary.
3. **Tests live with the code.**  `test "name" { ... }` at the bottom of the
   file, `std.testing.expectEqual` for rich comparisons, and tests that assert
   *invariants* (null termination, ordering, a map agreeing with the table it
   was built from) rather than restating the implementation.
4. **`errdefer`.**  Cleanup is attached to the point of acquisition, so an
   error path cannot forget it.  This is the single largest safety difference
   from C.
5. **A testing allocator that fails leaks by default**, plus Tripwire for
   injecting allocation failures to prove `errdefer` cleanup actually runs.
6. **A project-specific assert** (`quirks.zig`'s `inlineAssert`) rather than
   the language's, so assertion policy is one decision in one place.
7. **Exact toolchain pinning**, enforced by the build, not documented in a
   README.

## Applicability to this project

Our transport seam is already the pattern Hashimoto describes, in C:
`ngx_media_srt_ops_t` is a comptime interface in spirit — Haivision/srt,
robotweax/srt, and the UDP test double are selected at build time, and the
runtime dispatch exists only because C cannot collapse it.  We even
independently arrived at their CI rule: `MEDIA_SRT_BACKEND=srt|udp|both` is
tested as a matrix precisely because an unexercised build option hides broken
paths.

Where the mapping is strong:

| Our C | Zig equivalent | What changes |
|---|---|---|
| `goto cleanup` ladders in parsers and runtimes | `errdefer` | Cleanup becomes attached to acquisition and checked by the compiler |
| Error codes returned and ignored | error unions + `try` | Unhandled failure stops being possible |
| `NGX_MEDIA_SRT_*` macros for constants | `comptime` constants | Same, with type checking |
| MPEG-TS tables (CRC, PID classes), NAL classification, AMF0 markers | comptime tables | Built at compile time, no runtime init, raw forms absent from the binary |
| Unit shim + 26 separate test binaries | inline `test` blocks | Tests sit next to the code; `-Dtest-filter` replaces per-binary targeting |
| ASan/UBSan leak detection | testing allocator | Equivalent guarantee, cheaper to run |
| Threads + eventfd handoffs | Zig's async/defer | Not obviously better; our model is shaped by nginx, not by the language |

Where it does not apply:

- **The module itself cannot move.**  An nginx module is C: the ABI, the
  config parser's macro-based structures, `ngx_pool_t`, the event loop's
  `ngx_event_t` and `ngx_connection_t`.  Zig's C interop can call all of it,
  but a module *is* its integration with those macros, and that integration
  does not translate.
- **The rewrite is large.**  The core is roughly 15k lines of C that we wrote
  and tested; a Zig port would be a rewrite of all of it.
- **Pre-1.0 churn is real, not theoretical.**  Ghostty pins Zig to exactly
  0.16.0 and enforces it at build time.  A project that must build against a
  distribution's toolchain inherits that risk.

## What it would and would not buy

**Would buy:** elimination of the error-path and lifetime bug class that has
actually bitten this project — the double-unref, the dangling source pointer
after removal, the callback that had to be restored by hand.  In C those are
discipline; in Zig the compiler and `errdefer` make them structural.  Plus
comptime tables for the codec and container constants, and a test layout that
makes the suite cheaper to extend.

**Would not buy:** a better nginx module, a faster media path (our hot path is
already allocation-free and copy-free; the language does not change the
algorithm), or an easier integration — the seam with nginx stays C either way.

**The shape that would actually make sense**, if this were pursued: keep the
module in C, and move the parts that are already nginx-free — TS demux and
mux, the codec helpers, the timeline and feed — behind a C ABI in Zig, the way
libghostty exposes a C API to its own consumers.  Those files already compile
against the unit shim with no nginx headers, so the boundary is real rather
than aspirational.  That is the Ghostty/Superlogical split applied here:
library core, thin native integration.

## What the runtime graph and the benchmarks added

Written after the runtime graph, the profiles and the benchmarks were built,
so these are observations rather than predictions.

**The strongest new evidence is a bug that happened three times.**  `ngx_str_t`
is a pointer and a length; its data is a slice of something larger and is not
NUL-terminated.  Reaching for a string function on it reads past the end.  It
happened on the SRT listener host (worked only when the next byte happened to
be NUL), on an `opendir()` call (reported ENOENT for a directory that plainly
existed), and on a request URI (produced a segment name of `"1.1"`).  Each one
failed pointing somewhere else, and each took real time to find.  Zig's slices
do not have this failure mode at all: a slice carries its length, and the
string functions take one.  Of everything studied, this is the clearest case
where the language would have prevented a defect class rather than reshaped
the code around it.

**A type accepted by an API with no implementation behind it.**  The control
API accepted `"type":"rtmp"` for a destination, and no backend registered the
type, so every create failed at start.  In C the backend table and the type
enum are two independent lists and nothing connects them.  A `comptime` table
indexed by the enum would make a missing entry a compile error, which is the
comptime-data-table pattern applied to a case where its value is not speed but
exhaustiveness.

**Cleanup on a path that did not exist when the code was written.**  The
runtime graph creates and deletes streams, sources and destinations while
media is flowing, so every acquisition now has an error path and a teardown
path, and the teardown order matters (stop the transport before dropping the
object).  This is `errdefer`'s exact shape, and it is now the largest single
piece of manual discipline in the codebase.

**The benchmarks argued against the rewrite, not for it.**  The push fanout
benchmark measures worker CPU per uploaded segment and finds the difference
between a sendfile path and a read/write loop — one copy per destination
instead of two.  The program's `fanout_delay` is under two milliseconds with a
file source on an idle machine, and it is bounded by the feed's window rather
than by anything the language could change.  A rewrite buys nothing here: the
hot path is already allocation-free and copy-free, and the algorithm is the
algorithm.

**One portability bug had nothing to do with either.**  `(void) write(...)`
does not compile under `-Werror` on GCC 13, because glibc marks `write`
`warn_unused_result`.  Arch's GCC accepts it, so every local build passed and
CI on Ubuntu would have failed on its first run.  Zig would not have this
specific problem — `try` makes the result impossible to discard — but the
lesson is about building on the same toolchain as CI, which no language fixes.


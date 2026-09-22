# Security Architecture & Codebase Invariants

This document records the security boundaries, threat models, verified vulnerability findings, and critical invariants established during the comprehensive codebase audit.

---

## 1. Threat Boundary & Trust Model

| Component | Trust Boundary | Primary Invariant |
|---|---|---|
| **SRT Ingest** | Untrusted public network (UDP) | Stream ID parsed via strict parser (`#!::...`); raw TS buffered in bounded queues. |
| **RTMP Ingest** | Untrusted public network (TCP) | Message lengths strictly checked against `max_message`; AMF0 strings and containers bounded; buffers unref'd before new message payloads. |
| **HTTP Control API** | Administrative trust boundary | Has **no** built-in auth; must be secured via NGINX `allow/deny` or `auth_basic`. JSON responses are escaped; body sizes bounded. |
| **HLS Ingest / Push / Pull** | External HTTP/HTTPS servers | Hostname verification (`SSL_set1_host`) enforced; DNS resolution supports IPv4 and IPv6 via `getaddrinfo()`; buffers must not share non-thread-safe pools. |
| **Inter-Worker IPC** | Internal Unix domain sockets (`SOCK_SEQPACKET`) | Only authenticated peers in the same cluster communicate; messages are chunked and reassembled with strict length validation. |

---

## 2. Verified Vulnerabilities & Remediations

### 1. RTMP Chunk Reader Buffer Overflow on Length Change
- **Vulnerability**: In `src/rtmp/ngx_media_rtmp_wire.c`, receiving a fmt-0/1 header with a new `length` for an already-allocated chunk stream did not release `cs->payload` if the previous message was incomplete. The subsequent `memcpy` could write past the previous buffer's smaller capacity.
- **Root Cause**: Premature reuse of `cs->payload` pointer without checking if its capacity matched the newly declared message length.
- **Fix**: Free and NULL `cs->payload` whenever a fmt-0 or fmt-1 message header arrives on that stream ID before allocating the new buffer.

### 2. AMF0 Nested Object Name Length Out-of-Bounds Read
- **Vulnerability**: In `src/rtmp/ngx_media_rtmp_wire.c`, during the non-recording skip pass of nested AMF objects (`!record`), `pos` was incremented by `2 + nlen` without verifying `pos + 2 + nlen <= len`.
- **Root Cause**: Assuming `nlen` fits in `len - pos` simply because `pos + 2 <= len`.
- **Fix**: Validate that `pos + 2 + nlen <= len` before advancing `pos`.

### 3. SRT Output Use-After-Free in Destination Removal
- **Vulnerability**: In `src/srt/ngx_media_srt_output.c`, `outputs_remove()` locked the slot mutex, set `dest->session = NULL`, and immediately called `ngx_media_srt_session_close(dest->session)`. If the worker's sender thread had already unlocked the mutex and was executing `ngx_media_srt_session_send()` on that session pointer, it suffered a use-after-free.
- **Root Cause**: Asynchronous teardown of a reference-less resource while another thread holds a raw pointer outside the lock.
- **Fix**: Check `dest->sending` under the lock in `outputs_remove()`. If sending is in progress, defer the session close to the sender thread itself, which detects `dest->epoch != epoch` or `!dest->used` upon re-acquiring the lock and cleans up safely.

### 4. Outbound HTTPS TLS Hostname Verification Missing
- **Vulnerability**: In `src/core/ngx_media_http.c`, `ngx_media_http_tls_start()` configured `SSL_VERIFY_PEER` and SNI, but did not invoke `SSL_set1_host()`.
- **Root Cause**: Relying on certificate chain validation without binding the certificate subject/SAN to the requested destination hostname.
- **Fix**: Explicitly call `SSL_set1_host(ssl, host_z)` prior to `SSL_connect()`.

### 5. Outbound HTTP Client IPv4 Literal Restriction
- **Vulnerability**: `ngx_media_http_connect()` directly called `inet_pton(AF_INET, ...)` only, rejecting any domain names or IPv6 targets.
- **Root Cause**: Missing name resolution layer for outbound HTTP connections.
- **Fix**: Switched to `getaddrinfo()` with `AF_UNSPEC` and `SOCK_STREAM`, supporting both IPv4, IPv6, and DNS hostnames.

### 6. HTTP Header `snprintf` Truncation Write-Past-Buffer
- **Vulnerability**: `snprintf()` return values were directly passed as length arguments to `ngx_media_http_write()`. If the URL exceeded buffer capacity, `snprintf` returned the hypothetical length, causing out-of-bounds reads from the stack.
- **Root Cause**: Confusing `snprintf` return value (bytes that would be written) with actual bytes formatted.
- **Fix**: Explicit bounds check: `if (header_len < 0 || (size_t) header_len >= sizeof(header)) return NGX_ERROR;`.

### 7. Thread-Safety: `ngx_cycle->pool` Usage in Background Worker
- **Vulnerability**: `ngx_media_http_put_file()` in `ngx_media_http.c` allocated request target buffers using `ngx_pnalloc(ngx_cycle->pool, ...)`.
- **Root Cause**: Calling single-threaded NGINX pool allocators from concurrent background push threads.
- **Fix**: Replaced dynamic allocation with bounded stack buffers safe for multi-threaded execution.

### 8. MPEG-TS PSI Reassembly Split Across Packet Boundaries
- **Vulnerability**: In `src/mpegts/ngx_media_ts_demux.c`, when a PSI section header had only 1 or 2 bytes at the end of a TS packet, `psi->len` was saved but `psi->expected` remained 0, causing underflow on continuation packets.
- **Root Cause**: Incomplete state-machine handling for partial PSI headers (< 3 bytes).
- **Fix**: Accumulate the 3-byte header explicitly across packets before evaluating `section_length` and `expected`.

### 9. Owner Directory Open-Addressing Collision Chaining Broken by Deletion
- **Vulnerability**: In `src/core/ngx_media_owner_dir.c`, releasing a stream set its state to `FREE` via `memzero`. This severed probe chains in open-addressing lookup (`find`), making subsequent colliding streams unreachable.
- **Root Cause**: Removing items from an open-addressing hash table without tombstones.
- **Fix**: Introduced `NGX_MEDIA_OWNER_STATE_DELETED` tombstone state. `find()` continues probing past tombstones, and `claim()` reclaims them.

### 10. RTMP Player Queue Under-Reserved Multi-Chunk Messages
- **Vulnerability**: In `src/rtmp/ngx_media_rtmp_module.c`, `queue_message()` reserved one payload slice (`slices = 1`) while the writer emits `ceil(len/4096)` slices. A >256 KiB message passed the admission check, then wrote past the 64-entry `in_flight` ring (overwriting live references, leaking them) and the matching drain path double-unref'd the wrapped slots.
- **Root Cause**: Admission accounting assumed one slice per message instead of one per chunk.
- **Fix**: Compute `chunks = ceil(len/OUT_CHUNK)` up front and admit only when `out_queue + chunks*2 + 1` and `in_flight_count + chunks` fit; added an in-loop ring guard mirroring `rtmp_destination.c`.

### 11. SRT Transport NULL-Backend Dereference
- **Vulnerability**: In `src/srt/ngx_media_srt_transport.c`, `connect()`, `session_send()`, and `last_error()` dereferenced `ngx_media_srt_backend()` without checking for NULL. With neither backend linked the weak symbols resolve to NULL, so any output, send, or error-format call segfaulted the worker.
- **Root Cause**: Three wrappers missed the NULL check every adjacent wrapper performs.
- **Fix**: Added the `backend() == NULL` guard to all three, matching the file's own convention.

### 12. HLS Push Scanner Out-of-Bounds Read on 4-Character Filenames
- **Vulnerability**: In `src/core/ngx_media_hls_push.c`, the directory scan tested `strcmp(name + len - 5, ".m3u8")` for any name with `len >= 4`. A 4-character non-segment name made the pointer `name - 1`, reading one byte before the dirent buffer on every runtime tick.
- **Root Cause**: `size_t` pointer arithmetic underflow in a suffix check.
- **Fix**: Restructured to check `.ts` first, then require `len >= 5` before the `.m3u8` comparison.

### 13. NAL Iterator Unbounded Recursion on Empty Units
- **Vulnerability**: In `src/codec/ngx_media_nal.c`, `ngx_media_nal_iter_next()` recursed once per empty NAL unit (adjacent start codes). A 200k-empty-unit access unit (~600 KiB of `00 00 01`) recursed 200k frames deep and smashed the 8 MB worker stack; any RTMP/SRT publisher controls this input.
- **Root Cause**: Recursion where the depth equals attacker-controlled input length.
- **Fix**: Replaced recursion with a loop (`it->pos` already advances past each empty unit) and added NULL guards.

### 14. Record Part Suffix Read Past Path Length
- **Vulnerability**: In `src/record/ngx_media_record.c`, part 2+ filenames were built with `%s` on `dot`, a pointer into an `ngx_str_t` with no NUL guarantee, over-reading past `path.len`. The fixed `+16` allocation also assumed small part numbers without checking `snprintf`'s return.
- **Root Cause**: Treating a counted slice as a NUL-terminated string.
- **Fix**: Build the suffix with `%.*s` bounded by the slice length, size for the longest uint64 part number, and fail on `snprintf` truncation.

### 15. Routed OPEN/TRACKS Accepted Chunkable Payloads the Sink Cannot Reassemble
- **Vulnerability**: In `src/core/ngx_media_route.c`, `route_open()` and `route_tracks()` sent arbitrarily large payloads through the chunking IPC sender while the runtime sink parses each datagram as a complete message. A TRACKS payload past one datagram (reachable via large codec configs) would parse as truncated and bogus track sets.
- **Root Cause**: Missing the single-datagram bound the graph encoder already enforces.
- **Fix**: Return `NGX_ERROR` when `len > NGX_MEDIA_IPC_MAX_PAYLOAD`, mirroring `graph_encode`.

### 16. HTTP Endpoint Split Accepted Control Bytes and Bad Ports
- **Vulnerability**: In `src/core/ngx_media_http.c`, `http_split()` copied host/target into the request line without charset checks, so a configured URL containing CR/LF could inject headers into the socket write; `atoi` port parsing accepted 0 and >65535, and empty hosts passed through.
- **Root Cause**: No validation between URL configuration and request serialization.
- **Fix**: Reject bytes `<= 0x20`, `0x7F`, and `#` in host, controls in target, require a leading `/`, and enforce port `1-65535` plus a non-empty host.

### 17. Control API Returned Truncated JSON as 200 OK
- **Vulnerability**: In `src/api/ngx_media_api_module.c`, `destination_json()` always returned `NGX_OK` and `desired_get()` never checked truncation, so a large graph (many streams/destinations) returned a cut-mid-array document with 200 OK; replaying it would lose state.
- **Root Cause**: Inconsistent bounds handling: sibling builders already return ERROR on truncation.
- **Fix**: `destination_json()` reports `(*last < end - 1)`, all four call sites and every `desired_get` append check for truncation and answer 500 instead.

---

## 3. Engineering Guidelines for Future Contributors

1. **Buffer Lifecycles in Packet Stream Parsers**:
   - Never assume a parser state object's buffer pointer is reusable across packet headers without verifying whether its capacity matches the new payload's requirements.
2. **Multi-Threaded Output Pools**:
   - If an operation runs without a lock (to avoid blocking event loops on transport I/O), the teardown path **must not** free resources until the worker confirms it has exited the unlocked critical section.
3. **Outbound Network Calls**:
   - Always verify TLS hostnames with `SSL_set1_host()`.
   - Never use single-threaded NGINX memory pools (`cycle->pool` or `r->pool`) inside background worker threads.
4. **Open-Addressing Tables**:
   - Linear probing or secondary hashing tables require tombstone states for deletions; resetting a slot to `FREE` corrupts probe sequences for colliding keys.
5. **Queue Admission Must Mirror the Writer**:
   - When a packetizer fans one message out to N units (chunks, slices, fragments), reserve N before encoding, not 1; re-check the bound inside the fill loop.
6. **Recursion Depth Is Attacker Input**:
   - Never recurse once per input element in a parser reachable from the network; use a loop and keep the iterator's progress in the state object.
7. **Slices Are Not Strings**:
   - `ngx_str_t` carries no NUL guarantee: never pass `data` to `%s`/`str*` without a bound (`%.*s`, explicit length checks).
8. **One Datagram, One Message**:
   - If the sink parses a single datagram as a complete message, the sender must refuse payloads past `NGX_MEDIA_IPC_MAX_PAYLOAD` rather than chunk them.
9. **Validate Before Serializing**:
   - URLs and filenames that reach a socket write or a filesystem call must be charset/range-checked at parse time (controls, ports, suffixes), not at use.
10. **Truncation Is an Error**:
    - A builder that cannot fit its document must report it and the handler must answer 5xx; a cut 200 OK corrupts every controller that replays it.

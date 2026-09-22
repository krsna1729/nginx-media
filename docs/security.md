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

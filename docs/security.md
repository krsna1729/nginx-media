# Security Architecture & Codebase Invariants

This document records the security boundaries, threat models, verified vulnerability findings, and critical invariants established during the comprehensive codebase audit.

---

## 1. Threat Boundary & Trust Model

| Component | Trust Boundary | Primary Invariant |
|---|---|---|
| **SRT Ingest** | Untrusted public network (UDP) | Stream ID parsed via strict parser (`#!::...`); raw TS buffered in bounded queues. |
| **RTMP Ingest** | Untrusted public network (TCP) | Message lengths strictly checked against `max_message`; AMF0 strings and containers bounded; buffers unref'd before new message payloads. |
| **HTTP Control API** | Administrative trust boundary | Has **no** built-in auth, no client-address check and no TLS of its own: the location serving it is the entire boundary, and it must be loopback, authenticated at a proxy, or a Unix socket — see section 3.  JSON responses are escaped; body sizes bounded. |
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

## 3. The Control API's Deployment Boundary

`media_api` is a content handler and nothing more: `ngx_media_api_set()` in
`src/api/ngx_media_api_module.c` installs it on a location, and every request
that reaches that location is dispatched.  There is no authentication, no
client-address or peer-credential check, and no TLS of the module's own, and
enabling it logs no banner — so **the API is exactly as protected as the
location serving it**, and nothing in nginx-media will tell an operator when
that location is public.  (`ngx_media_api_handler()` checks the method, reads
the body, and dispatches; every check inside `src/api/` is a bound or an
escape, never an authorization.)

### What a request that reaches it can do

- **Read the deployment.**  `GET /streams`, `/streams/{app}/{name}`, `/desired`
  and `/metrics` return the whole graph, each program's sources and
  destinations, and the Prometheus series — the shape of the deployment, not
  merely whether it is up.
- **Create and delete programs and their children.**  A stream, a source, a
  destination, a switch, a switchback, and `DELETE` on each of them.  These are
  runtime objects that start immediately and carry media, which is why the
  boundary matters at all.
- **Make the server read a path.**  A `file` source opens the `path` it is
  given and an `hls_push` source opens the directory it is given, both on the
  program's owner, and the difference between opened and not opened comes back
  in the answer (`201` against `400 source_open_failed`), so a reached API is
  also a read oracle for what that worker can open.
- **Make the server fetch a URL.**  An `hls_pull` source is handed a playlist
  URL and fetches it, and its segments, over HTTP or HTTPS, to any host the
  worker can reach.  Reaching the API is reaching the worker's network
  position, not just its media pipeline.
- **Make the server send the program somewhere.**  An `srt`, `rtmp` or
  `hls_push` destination connects out to the host it is given and publishes the
  program to it, so the API is also a way to move a deployment's media to a
  third party.
- What it **cannot** do is change configuration: there is no reload route and no
  directive is writable through it, and the graph is runtime state that a
  reload loses.  That bounds the damage of a momentary exposure; it does not
  bound the damage of a standing one.

### The safe shapes

- **Loopback, with the API on its own listener.**  Bind the API's `server` to
  `127.0.0.1` rather than sharing the public listener, and put the belt beside
  the braces inside the location: `allow 127.0.0.1; allow ::1; deny all;`.  The
  access phase runs before the handler, so that rule is a boundary and not a
  comment.
- **Behind a reverse proxy that authenticates.**  Terminate TLS and
  authenticate at the front — client certificates from an internal CA with
  `ssl_verify_client on;` and `ssl_client_certificate`, HTTP basic against the
  deployment's own file, or `auth_request` against its authorizer — and keep
  the nginx-media listener private, so the API cannot be reached around the
  proxy.  The proxy is also where a rate limit belongs; the API's own limits
  bound one request, not a request rate.
- **A Unix socket.**  `listen unix:/run/nginx-media/api.sock;` moves the
  boundary into the filesystem: only a process that can open the socket reaches
  the API, and the socket's permissions are the authorization.  That is the
  right shape for a controller on the same host, because it removes the network
  question entirely.
- **Never the public listener.**  `location /media/api/ { media_api; }` on a
  `listen 0.0.0.0:80` is a write API for the internet: it can tear down running
  programs, point the server at arbitrary URLs, and publish the deployment's
  media to an arbitrary remote.  An API that must be reachable off-host belongs
  on its own listener behind the authenticating proxy, not in a location beside
  the public site.

The same reasoning applies to `media_hls_ingest`, which is a separate location
that writes uploaded bodies into a directory on disk.  It authenticates nobody
either, so restrict it to the encoder's address or put it behind the same
proxy.  Its surface is narrower than the API's — the URI has to end in `.ts`
and may not start with a dot, and the body size is nginx's own — but a public
ingest endpoint is disk and media pipeline that an anonymous caller controls.

### What the module does do

None of this is authentication; all of it limits what a reached API or a stored
value can be turned into, and it is worth knowing when triaging:

- Request bodies are capped at 8192 bytes (`400 body_too_large`) and every
  response is built into a fixed buffer, where truncation is a `500` and never a
  partial document answered `200` (finding 17).
- Endpoint credentials are redacted wherever an endpoint is reported — the
  query string is dropped and userinfo replaced — so a key carried in an
  `hls_push` URL cannot be read back out of the API, a log line, or the
  desired-state document.
- A destination type with no backend in the build is accepted and then fails to
  start with `500`, so the API cannot start a transport this build does not
  carry (there is no `record` destination backend here).

### Checking it

A boundary that cannot be demonstrated is a hope, so the check belongs in
deployment and in review, from a host that is not the intended peer:

```sh
# the API must not answer an unauthenticated off-host caller: this must not be 200
curl -sS -o /dev/null -w '%{http_code}\n' http://<host>:<port>/media/api/v1/streams
# and it must not be listening anywhere but where it should be
ss -lntp | grep <port>
```

---

## 4. Engineering Guidelines for Future Contributors

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

---

## 5. What the Static Analysis Covers

`.github/workflows/codeql.yml` scans `src` and `tests` — this repository's own
sources — and deliberately not the nginx tree the build fetches into `.build/`.
The analysis is a compiled one, so the build step pulls that tree in and it ends
up in the database either way; analyzing it reported findings in code this
project does not own, cannot act on, and would have to triage on every run (two
of the four open alerts were in it: `ngx_time.c`'s `localtime()` and an
`ngx_log.c` path argument).  Those reports are noise that hides findings in
`src/`, which is the code this module ships, so a finding in the vendored tree
is out of scope here by decision rather than by accident — upstream's business,
recorded upstream, not in this list of remediations.

Both alerts that were ours are fixed, and they are not in section 2 because they
were found this way rather than by audit: an access-unit allocation in
`src/mpegts/ngx_media_ts_demux.c` now checks the reassembled length against
`demux->max_au_bytes` at the allocation and counts the refusal, and the routed
path allocation in `src/core/ngx_media_runtime.c` is bounded the same way.

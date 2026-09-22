# Configuration reference

Every directive this module adds, with its scope, arguments and default.  All
of them live in the `main` context (they are `NGX_MAIN_CONF|NGX_DIRECT_CONF`
unless noted), so they sit beside `events {}` and `http {}`, not inside them.

```nginx
media_srt_listen 127.0.0.1:9000;
media_srt_source_priority encoder-a 100;
media_srt_output live/news 127.0.0.1:9100 "#!::r=live/news,m=publish,s=out";

media_hls /var/lib/nginx/media/hls;
media_record /var/lib/nginx/media/record;

http {
    server {
        listen 8080;
        location /hls/ { alias /var/lib/nginx/media/hls/; }
        location /media/api/ { media_api; }
        location /ingest/ { media_hls_ingest /var/lib/nginx/media/ingest; }
    }
}
```

## Program model

### `media_hls <directory>;`

Writes the segmented HLS output for every program under `<directory>`.  Serve
it with a normal `alias`/`root` location.  The segmenter starts on a keyframe;
no directive configures segment duration yet.

### `media_record <directory>;`

Records the post-selection, timeline-normalized program — what went to air —
as MPEG-TS parts under `<directory>`.  I/O runs on a writer thread, never on
the event loop, and reload closes the current part cleanly.

### `media_record_raw <directory>;`

Records the RAW tap: transport bytes exactly as they arrived, before demux and
before selection.  Useful when the question is "did the publisher send that".

### `media_record_iso <application/stream> <directory>;`

Records one named source, before the selector, into `<directory>`.  This is the
per-source tap; it is independent of which source is currently on air.

## Selection and failover

### `media_failover_failure_timeout <time>;`

Default `1500ms`.  How long a source may stop producing before it is treated as
failed and the selector moves to the next eligible source.  A bare number is
milliseconds; `ms` and `s` suffixes are accepted.

### `media_failover_recovery_timeout <time>;`

Default `10000ms`.  How long a failed source must be producing again before it
is considered recovered.  Same units as above.

### `media_failover_switch_keyframe on|off;`

Default `on`.  When on, a switch takes effect at the incoming source's next
keyframe rather than immediately, so the program stays decodable across the
switch.

### `media_failover_switchback auto|manual|never;`

Default `auto`.  Whether a recovered higher-priority source takes the program
back (`auto`), waits for an operator (`manual`, see the control API), or never
returns (`never`).

## SRT

### `media_srt_listen <host:port>;`

Accepts SRT publishers.  The directive may be given once per worker, and
**worker i binds the i-th entry**, so a publisher that connects to entry i is
accepted by worker i and its receive path is the worker that carries it:

```nginx
worker_processes 4;

media_srt_listen 127.0.0.1:9000;   # worker 0
media_srt_listen 127.0.0.1:9001;   # worker 1
media_srt_listen 127.0.0.1:9002;   # worker 2
media_srt_listen 127.0.0.1:9003;   # worker 3
```

Order is the worker order: entry i is what worker i binds, so the list is
written in worker order.  libsrt exposes no `SO_REUSEPORT` of its own, so one
endpoint per worker is what replaces it — with a single entry the worker that
bound it receives every publisher, which makes worker 0 the whole ingest path
and routing the rule rather than the exception.  A publisher is still routed
over the internal transport to the worker that owns its program (goal doc 22)
whenever that is a different worker; that path is unchanged.
`media_srt_listen_shared`, below, is the other way: one endpoint for every
worker, at the cost of the constraint described there.

* **One entry** is the single-listener shape: worker 0 binds it, and every
  other worker owns programs without accepting publishers.
* **Fewer entries than workers** keeps that: the remaining workers do not
  listen, and still own the programs their hash selects.
* **More entries than workers** is a configuration error naming both counts.
  A worker binds one endpoint, so the extras would never accept anything, and
  a configuration that says otherwise is one an operator would read wrongly:
  `worker_processes 1` with two endpoints is an error, not two endpoints on
  one worker.

The same endpoint twice is refused as well: two workers cannot bind one
address, and without the check that surfaces later as a startup failure about
an address in use.

Every endpoint accepts with the listener's one passphrase (see
`media_srt_crypto`), and `media_srt_listen_bond` applies to all of them: the
same second leg address published on each endpoint's own port, so a bonded
listener per worker obeys the same address rules as a single one.

A publisher's identity comes from its stream id
(`#!::r=<app>/<stream>,m=publish,s=<identity>`), never from a trusted field:
`s=` is only a label.

### `media_srt_listen_shared <host:port>;`

The other way to give ingest a port: **one endpoint that every worker binds**,
instead of one endpoint per worker.

```nginx
worker_processes 4;

media_srt_listen_shared 127.0.0.1:9000;
```

Each worker creates its own UDP socket, sets `SO_REUSEPORT` on it, binds it to
that address and hands it to libsrt with `srt_bind_acquire()`.  The kernel then
spreads incoming publishers across those sockets by 4-tuple — source address
and port, destination address and port — and each publisher's session stays on
the worker it landed on for its whole life: a session is never split across
workers.  The publisher is routed to its program's owner from there, exactly as
in the per-worker mode, so the owner of a program is still `hash % workers` and
still reported by the graph as `owner`; what changes is that the encoder does
not have to be pointed at a particular entry.

It is a directive of its own rather than an option on `media_srt_listen`,
because the mode belongs to the whole listener set: `media_srt_listen` means
"worker i binds entry i", and a flag on one entry could not say what that entry
means beside a shared port.  So the two are refused together, and so is
`media_srt_listen_bond`: the legs of a bonded caller have different source
addresses by construction, which is the point of bonding, and a shared port
hashes each leg on exactly that, so the legs scatter across workers.  libsrt
builds the receiving side of a bond in process state (`CUDT::uglobal()`'s
group registry), so workers that each hold one leg cannot join them into one
bond and would serve the caller's bond as several independent publishers.  That
is a correctness failure rather than a degradation, so it is a configuration
error, and a deployment that bonds keeps one endpoint per worker.  The shared
listener therefore does not enable group acceptance at all: a bonded caller
pointed at it is refused at the handshake, visibly, instead of being split.

**Read "One shared SRT port" in docs/operations.md before choosing this.**  The
mode has one hard constraint: the set of processes bound to the port must not
change while publishers are connected.  A reload changes it — the new
generation's workers bind before the old generation's exit — and the kernel
re-hashes the flows still running onto workers that have no session state for
them, so **a reload ends every live publisher's session** (measured; the
per-worker mode ends them too, when the replaced worker exits).  A worker
respawning after a crash is enough to do it as well, which the per-worker mode
is not subject to.  The instance says so in its error log once per
configuration read, before any worker starts.

What the shared bind can fail with, and what it does about it.  The socket is
this module's own, so every failure is reported where the operator will read
it and none of them is silent.  If the platform has no `SO_REUSEPORT`, the
kernel refuses it, the UDP socket cannot be created, the address cannot be
bound, or libsrt will not take the descriptor, the worker logs

```
media: srt listener 127.0.0.1:9000 is not available yet (the shared listener
could not bind 127.0.0.1:9000: Address already in use (the port is held by
something without SO_REUSEPORT)); retrying until the port is free
```

with the reason it was actually given — `this platform has no SO_REUSEPORT,
which the shared listener is built on`, `SO_REUSEPORT was refused on the
shared listener socket`, `could not create the UDP socket a shared listener
needs`, or the library's own words — and keeps retrying at 100 ms until the
worker exits, saying so again every five seconds.  The instance itself stays
up: ingest runs in a thread of its own, so the master, the control API and
every destination start normally, and what is missing is ingest on that one
endpoint.  The case that resolves itself is a port held by something that does
not share it — a foreign process, or the generation a reload is replacing when
this directive was just added — and then every worker takes the port as soon
as the holder exits, logging `srt listener bound <address> after N
attempt(s): the port was released`.  On a platform without `SO_REUSEPORT`,
that first line is the cue to use `media_srt_listen` and give every worker its
own endpoint instead.

### `media_srt_source_priority <identity> <number>;`

Higher wins.  The identity is the `s=` field of the stream id.  Priority is
operator configuration on purpose — an encoder cannot promote itself.

### `media_srt_output <application/stream> <host:port> [streamid];`

Pushes that program to a destination as an SRT caller.  One sender thread per
destination with a bounded queue and keyframe resynchronization: a destination
that cannot keep up loses units and counts them, and never stalls the program.
The optional stream id is announced to the destination.

### `media_srt_crypto <passphrase> [ctr|gcm] [0|16|24|32] [on|off];`

Enables SRT encryption.  The passphrase must be 10..79 characters (the library's
requirement, enforced at configuration time rather than at handshake time), the
mode defaults to `ctr`, the key length defaults to the library's own (AES-128
for Haivision/srt), and enforcement defaults to `on`, which rejects peers whose
secret does not match.  This directive sets the listener's passphrase and the
default for every destination.

AES-GCM requires a library built with the AEAD API preview.  Haivision/srt as
packaged by most distributions is not, so `gcm` is refused at startup with that
reason in the log instead of silently falling back to AES-CTR.

### `media_srt_crypto_stream <application/stream> <passphrase> [...];`

Same arguments, scoped to one program's destinations.  Resolution is
destination → stream → global; startup logs which scope each destination
resolved to (`media: srt destination 127.0.0.1:9100 crypto=stream`) and never
the passphrase itself.

There is deliberately no per-source form on the ingest side.  The passphrase is
a connection parameter and a stream id only becomes readable after the
handshake completes, so a listener carries exactly one passphrase; per-publisher
secrets would require one listener per publisher.

### `media_srt_backend srt|udp;`

Selects the transport implementation at runtime, when the build carries both.
`srt` is the SRT library — Haivision/srt or robotweax/srt, whichever the build
linked; the module cannot tell them apart because they share one C API, which
is the point.  `udp` is the conformance test double (plain UDP, none of SRT's
reliability or encryption features) and is not built into a production
configuration; selecting it there is a configuration error naming the build
that would provide it.

## RTMP

### `media_rtmp_listen <host:port>;`

Accepts RTMP publishers (handshake, AMF0 command channel, FLV tags) and serves
RTMP play requests for programs that exist.

Every worker opens the listener on the same address with `SO_REUSEPORT`, so the
kernel hashes each arriving connection onto one of them rather than funnelling
all of RTMP through one worker; a publisher that lands on a worker which does
not own its program is routed to the owner over the inter-worker transport
(goal doc 22).  A build whose `configure` found no `SO_REUSEPORT` cannot share
the port, so the listener stays on worker 0 and worker 0 routes every publisher.
The session table is per worker in either case, so the ceiling on how many
publishers one instance carries is that number times the worker count.

`SO_REUSEPORT` shares the port with *any* process that has asked for the same
thing, not only with this instance's other workers: two nginx instances
configured on the same address and port will both bind and split the incoming
publishers between them instead of the second one failing to start.  That is
the same tradeoff `listen ... reuseport` makes for nginx's own listeners, and
the answer is the same - give each instance its own address or port.

### `media_rtmp_source_priority <identity> <number>;`

Same semantics as the SRT form, for RTMP publishers.

### `media_rtmp_ssl on|off;`

### `media_rtmp_ssl_certificate <file>;`

### `media_rtmp_ssl_certificate_key <file>;`

RTMPS: the same RTMP protocol inside TLS, on the listener's own socket.  Both
files are needed; the TLS context is created when the second of the pair is
configured.

```nginx
media_rtmp_ssl on;
media_rtmp_ssl_certificate /etc/nginx/media/cert.pem;
media_rtmp_ssl_certificate_key /etc/nginx/media/key.pem;
```

The handshake is nginx's own, not a hand-rolled one: `ngx_ssl_create_connection`
installs `ngx_ssl_handshake_handler`, which *continues* a handshake — so the
module starts it with `ngx_ssl_handshake()` and sets `c->ssl->handler`, the
callback nginx calls when it finishes.  Two things have to be put right at that
point, and getting either wrong produces a hang rather than an error: the read
and write handlers must be restored (the SSL call replaced them, so later
events would otherwise dispatch into the handshake handler and read nothing),
and the read path must be run directly, because the peer's RTMP handshake is
already in the SSL buffer and the socket will not signal again.

`make rtmps` covers it with a self-signed certificate generated by the test:
rtmps publish, the program reaching HLS, rtmps playback, and a plain RTMP
publisher refused by an RTMPS listener.

## HTTP control API

### `media_api;`

Location-level (`NGX_HTTP_LOC_CONF`, no arguments).  Enables the control API on
that location; see `api.md` for the endpoints.

### `media_hls_ingest <directory>;`

Location-level (`NGX_HTTP_LOC_CONF`).  Turns that location into an upload
endpoint: someone else's encoder `PUT`s or `POST`s an HLS segment to a URL
under it and the body is stored in `<directory>` under the name at the end of
the request URI, so the uploader names its segments the way a reader expects.

```nginx
location /ingest/ {
    media_hls_ingest /var/lib/nginx/media/ingest;
}
```

The endpoint deliberately does not know what reads the directory — the two
halves stay separate, so an upload arriving by any other means works just as
well.  The usual reader is a source of type `hls_push` created through the
control API, which watches this directory.

The body is written to a file by nginx's own machinery rather than read into
memory, and the handler then renames it into place, so a reader either sees a
whole segment or none of it, never a partial one.  Because that is a rename,
`client_body_temp_path` has to be on the same filesystem as the ingest
directory — the same requirement any nginx upload-to-final-location setup has.

## Runtime sources and destinations

A program's sources and destinations are not declared in configuration.  They
are runtime objects created and addressed through the control API, which is
what lets a source be added while the program is live and removed without a
reload; `api.md` has the routes and the JSON.  Three of them are worth naming
here because they read or write files.

A **`file` source** reads an MPEG-TS file, opening it when the source is
created.  It is paced by the runtime tick: one bounded chunk per tick, never a
sleep, so a large file cannot stall a worker.

```json
{"id":"slate","type":"file","priority":10,"path":"/srv/slate.ts"}
```

A **`hls_pull` source** fetches a playlist and its segments over HTTP or HTTPS,
demuxes them and publishes frames through the same source gate as a publisher,
so selection and compatibility need to know nothing about where the bytes came
from.  Relative playlist entries resolve against the playlist's own directory,
an optional `ca_file` supplies a trust anchor for an origin whose certificate
is not in the system store, and when the playlist has nothing new the reader
waits for the origin rather than spinning.

```json
{"id":"upstream","type":"hls_pull",
 "path":"https://origin.example/live/news/index.m3u8"}
```

A **`hls_push` destination** watches the HLS output directory and uploads its
segments and playlists to an HTTP or HTTPS endpoint.  The directory has to be
the one `media_hls` is configured to write to: the runtime tick scans that
directory once per tick and offers anything new to every destination watching
it, so a destination pointed somewhere else simply receives nothing.  Uploads
run on a bounded pool and each destination has its own bounded queue, so a
stalled remote drops its oldest queued segment and counts it rather than
stalling the program or its neighbours.  An endpoint that carries a credential
is redacted before it reaches a log or an API read.

```json
{"id":"cdn","type":"hls_push",
 "host":"https://origin.example/live/news/",
 "path":"/var/lib/nginx/media/hls",
 "profile":"youtube_live"}
```

### Profile

`profile` is a named set of platform rules layered on the generic HLS
publisher, not a special path through the media core: `youtube_live` requires
an `https` endpoint, a segment duration between 1000 and 4000 ms and at most
five outstanding segments in the playlist, and fills the defaults when a field
is unset.  A configuration the platform would reject is refused with
`{"error":"profile_violation",...}` rather than clamped, because silently
changing an operator's number is worse than telling them it is wrong.  The
validated numbers are recorded on the destination; they do not yet drive the
segmenter, which still decides its own segmentation.

## Build-time configuration

### `MEDIA_SRT_BACKEND=srt|udp|both` (default `srt`)

Chooses which implementations of the module's transport contract are compiled
in.  `both` is what `make srt-qualify` builds so the qualification suite can run
one scenario against each.  This is not an `#ifdef` in C: the ops tables are
weak symbols, so whichever implementation is linked resolves the default and an
absent one produces a configuration error naming the build that would provide
it.

### Selecting the SRT library

Haivision/srt and robotweax/srt expose the same C API, so switching is a link
choice with no code change:

The link choice does not change the module's caller-facing transport contract,
but it does change runtime ownership, group capability and scaling limits.  See
`architecture.md` for the call boundary and both runtime topologies, and
`operations.md` for measured threads, buffers, shared-port behavior and
process-scope limitations.

```sh
PKG_CONFIG_PATH=/opt/robotweax/lib/pkgconfig make nginx
SRT_DIR=/opt/robotweax make nginx          # if it ships no .pc file
```

Startup logs which library answered, which is how a deployment knows what it
linked: `library=libsrt 1.5.6` for the packaged Haivision build, `libsrt
1.5.7` for robotweax 0.2.4 (the number is the SRT API target, not the
implementation's own release).

**Qualified**: robotweax 0.2.4 built with `ENABLE_AEAD_API_PREVIEW=ON` and its
libsrt pkg-config compatibility passes `make srt-ingest-nginx` and
`make srt-qualify` unchanged, including a publisher linked against Haivision
talking to a robotweax listener across the wire.  Nothing in this module knows
which of the two it is running on.

**One caveat the qualification found**: their default key lengths differ.  A
robotweax listener configured with `media_srt_crypto <passphrase>` and an
Haivision caller using a bare `passphrase=` fail the handshake with
`ERROR:BADSECRET` — the correct secret, rejected.  Pinning the key length on
both sides fixes it, and 16 is the interoperable choice:

```nginx
media_srt_crypto "correct-horse-battery" ctr 16 on;
```
```sh
ffmpeg ... "srt://host:port?mode=caller&passphrase=correct-horse-battery&pbkeylen=16"
```

With that, encrypted interop works.  If encryption is in play across
implementations, always set the key length explicitly.

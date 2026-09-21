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

Accepts SRT publishers.  The listener belongs to worker 0; accepted publishers
are routed to the worker that owns their program.  A publisher's identity comes
from its stream id (`#!::r=<app>/<stream>,m=publish,s=<identity>`), never from
a trusted field: `s=` is only a label.

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

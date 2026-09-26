# HLS push interoperability

HLS "push" (also called HTTP ingest) is an encoder uploading an HLS
presentation to a server with HTTP `PUT`/`POST`, instead of the server
pulling it.  There is no RFC for the upload side — RFC 8216 describes the
presentation, not how it reaches the origin — so the industry's working
contract is the one YouTube publishes for its HLS ingest and the one the
DASH-IF Live Media Ingest specification (Interface-2) writes down.  They agree
with each other; YouTube's is the stricter subset.  nginx-media follows that
contract in both directions:

- **inbound** — `media_hls_ingest` is a *receiving entity*: an encoder can
  point its standard HLS push at it, and an `hls_push` source reads what
  arrives (see [What to send us](#what-to-send-us));
- **outbound** — an `hls_push` destination is an *ingest source*: it pushes
  the program's HLS to an origin, a CDN or YouTube (see
  [What we send](#what-we-send)).

Where the two references differ, YouTube's rule is the one we hold to.

## The industry contract

| Rule | YouTube HLS ingest | DASH-IF Interface-2 (v1.2) | Akamai MSL4 | AWS MediaPackage |
|---|---|---|---|---|
| Method | HTTPS `PUT` or `POST` | `POST` or `PUT`, HTTP/1.1, no difference between them | `PUT` or `POST` | `PUT` (WebDAV) |
| Transport | HTTPS required | TLS ≥ 1.2 SHOULD; auth by basic, digest or client cert | HTTP(S) | HTTPS, digest credentials |
| Segment container | MPEG-TS (M2TS); byte ranges not supported | `.ts` (HLS only) or CMAF/ISOBMFF (`.m4s`, `.mp4`, `.cmfv`, …) | TS, fMP4, CMAF | TS |
| Codecs | H.264 or HEVC, AAC, closed GOPs, ≤ 60 fps | not restricted | per stream config | H.264/HEVC, AAC |
| Segment duration | 1–4 s | "in the order of a segment duration (1-6 s)" for timeouts | encoder choice | encoder choice |
| Playlist | media playlist, rolling, ≤ 5 outstanding segments; no master playlist | master and/or media playlists | master and media playlists | master and media playlists |
| Upload order | a media playlist after every media segment | segment, then key file, then playlist; never the playlist before its segment succeeds | segments first, then playlists; one request per object | — |
| Naming | monotonically numbered | unique across sessions; monotonically increasing number at the end of the name | — | names must start with `channel` |
| Connections | — | persistent connections SHOULD; parallel connections per rendition SHOULD | persistent connection | — |
| Deleting old segments | — | `DELETE` SHOULD, receiver answers `200` | `DELETE` answered `200`, ignored | — |
| Failed upload | — | retry for up to one segment duration, then move on and mark a discontinuity | — | — |
| End of stream | — | — | `#EXT-X-ENDLIST` | — |
| Receiver errors | — | `403` auth/path, `404` no publishing point, `415` unsupported type, `412` missing init, `400` otherwise | rate limits answered with errors | — |

Platforms that do **not** take HLS push for live contribution: Twitch and
Facebook take RTMP(S); Brightcove takes RTMP, RTP/RTP-FEC and SRT; Dacast
takes RTMP(S), SRT and WHIP; Unified Origin takes fMP4/CMAF ingest (DASH-IF
Interface-1), not HLS.  Wowza Streaming Engine accepts HLS push.  For those
destinations nginx-media's RTMP and SRT outputs are the path.

Sources: [YouTube: Delivering live content via HLS](https://developers.google.com/youtube/v3/live/guides/hls-ingestion),
[YouTube Help: Set up an HLS stream](https://support.google.com/youtube/answer/10349430),
[DASH-IF Live Media Ingest](https://dashif.org/Ingest/) ([source](https://github.com/Dash-Industry-Forum/Ingest)),
[Akamai MSL: HTTP ingest requirements](https://techdocs.akamai.com/msl/docs/http-ingest-requirements-encoder),
[Akamai MSL: HLS requirements](https://techdocs.akamai.com/msl/docs/hls-requirements),
[AWS MediaPackage supported inputs](https://docs.aws.amazon.com/mediapackage/latest/userguide/supported-inputs.html),
[Wowza: ingest an HLS live stream](https://www.wowza.com/docs/how-to-publish-and-play-a-live-stream-apple-hls),
[Unified Streaming: live recommendations](https://docs.unified-streaming.com/best-practice/live.html),
[Brightcove Live best practices](https://live.support.brightcove.com/live-2-0/get-started/live-module-guidelines-and-best-practices.html),
[Dacast: SRT ingest](https://www.dacast.com/blog/srt-ingest/).

## What to send us

This is the contract an encoder, a packager or another nginx-media targets
when it pushes HLS into nginx-media.  Any encoder that can push to YouTube's
HLS ingest can push here unchanged.

```nginx
server {
    listen 443 ssl;                                  # or plain HTTP inside a trusted network
    client_body_temp_path /var/lib/nginx/media/body; # same filesystem as the ingest directory
    client_max_body_size 16m;

    location /ingest/ {
        # authenticate here: auth_basic, auth_request, or ssl_verify_client
        media_hls_ingest /var/lib/nginx/media/ingest;
    }
}
```

Then a source per stream, pointed at the stream's directory.  The directory
need not exist yet - the endpoint creates it with the first upload, and the
source starts reading when it appears:

```sh
curl -X POST -H 'Content-Type: application/json' \
  -d '{"id":"encoder","type":"hls_push","path":"/var/lib/nginx/media/ingest/live/news"}' \
  http://127.0.0.1:8080/media/api/v1/streams/live/news/sources
```

and push to it — for example with ffmpeg's HLS muxer:

```sh
ffmpeg -re -i input -c:v libx264 -g 50 -c:a aac \
  -f hls -hls_time 2 -hls_list_size 5 -hls_flags delete_segments \
  -method PUT https://media.example/ingest/live/news/index.m3u8
```

### Requests

| Request | Answer |
|---|---|
| `PUT` or `POST` `<location>/<path>.ts` — a media segment | `201 Created`, or `204 No Content` when it replaced an object of the same name |
| `PUT` or `POST` `<location>/<path>.m3u8` — a media playlist | `201` / `204` as above |
| `DELETE <location>/<path>` | `200 OK`; `404` when there is no such object |
| `PUT`/`POST` of a type this build cannot carry: `.m4s`, `.mp4`, `.m4v`, `.m4a`, `.cmfv`, `.cmfa`, `.cmft`, `.cmfm`, `.init`, `.header`, `.mpd`, `.key`, `.vtt`, `.aac` | `415 Unsupported Media Type` |
| any other extension, a hidden name, an empty component, more than four path components, or characters outside `[A-Za-z0-9._-]` | `400 Bad Request` |
| any other method | `405 Not Allowed` |

`<path>` is kept below the ingest directory, so one endpoint serves many
streams (`live/news/index7.ts` lands in `<ingest>/live/news/`).  Each object is
written to a temporary file by nginx and renamed into place, so a reader sees
a whole object or none of it.

### What the objects must be

- **Segments**: MPEG-TS, carrying H.264 or H.265 video and/or AAC audio.  Other
  stream types in the same TS are ignored; a TS carrying none of the three
  never goes on air, and the source says so once in the log.  Cut segments at
  keyframes (closed GOPs), as YouTube requires — a segment that starts
  mid-GOP delays the first frame until the next keyframe.
- **Media playlist**: an RFC 8216 media playlist (`#EXTM3U`, `#EXTINF`,
  `#EXT-X-MEDIA-SEQUENCE`) in the same directory as its segments, with
  relative segment URIs.  Upload it after every segment, after the segment it
  names — the reader takes segments in playlist order, by media sequence
  number, and passes over a listed segment that is not there (a failed upload
  is marked with `#EXT-X-DISCONTINUITY` by the encoder, per RFC 8216).
- **Names**: unique, with a number at the end that increases by one per
  segment (`index7.ts`, `seg-00042.ts`).  Padding is not needed.  Without a
  playlist the reader orders segments by that number — as a number, so
  `index10.ts` follows `index9.ts`.
- **One rendition per source**: a master playlist lists variants, not
  segments, and is not read; point each source at one variant's directory.
  (Pushing a multi-variant presentation stores every variant; a source reads
  the one it is pointed at.)
- **Deleting**: `DELETE` expired segments (ffmpeg's `delete_segments`), or the
  directory grows for as long as the stream runs.

### Recommended encoder settings

YouTube's numbers are the safe choice here too: segments of 1–4 s (2 s is
common), a rolling playlist of up to 5 segments, persistent HTTPS connections,
and a new playlist after every segment.  They are also what nginx-media
itself sends by default.

### Not accepted yet

fMP4/CMAF segments (DASH-IF Interface-1 and the CMAF form of Interface-2),
byte-range segments, encrypted segments (`#EXT-X-KEY`), low-latency HLS
partial segments (`#EXT-X-PART`), and in-band captions or timed metadata as
separate renditions.  These are refused with `415` or ignored rather than
misread.

## What we send

An `hls_push` destination uploads the program's own HLS output, segment by
segment, as the segmenter seals each one.  Measured by
`make hls-push-conformance` against recording sinks (numbers from one run on
the development host):

| Rule | YouTube / DASH-IF | nginx-media | |
|---|---|---|---|
| Method | `PUT` or `POST` | `PUT` by default; `POST` for `youtube_live`; `"method"` chooses | ✓ |
| HTTPS | required by YouTube | `https://` endpoints, with kTLS where available; `youtube_live` refuses `http://` | ✓ |
| Endpoint form | YouTube names the object in a query parameter (`http_upload_hls?cid=KEY&copy=0&file=NAME`); origins take a path | an endpoint with a query string gets the name in `file=`; a path endpoint gets `/NAME` | ✓ |
| Container | MPEG-TS | MPEG-TS | ✓ |
| Content-Type | `video/mp2t`, `application/vnd.apple.mpegurl` | by object | ✓ |
| User-Agent | SHOULD name the source | `nginx-media` | ✓ |
| Upload order | segment, then the playlist naming it; a playlist per segment | one upload in flight per destination, in the segmenter's order; a playlist is never sent before the segments it lists, including for a destination added mid-stream | ✓ (0 violations on all three sinks) |
| Segment duration | 1–4 s (YouTube) | 2 s target, cut at the first keyframe ≥ 2 s, forced at 4 s, by default; the stream follows its strictest destination | ✓ (median 2.000 s, max 4.000 s) |
| Media playlist, rolling window | ≤ 5 outstanding segments (YouTube) | 5 by default; each destination's own `playlist_window` by rewriting the playlist it is sent, sequence numbers kept | ✓ (≤ 5 to YouTube, ≤ 3 to a window-3 origin) |
| Persistent connections | SHOULD | HTTP/1.1 keep-alive per destination, reconnecting once on a stale connection | ✓ (1 connection for 40 requests over TLS) |
| Retry and gap | retry for up to a segment duration, then move on and mark the gap | retried with backoff (100 ms doubling to 1 s) within the destination's segment duration, the thread free for other destinations meanwhile; a segment never delivered is listed as `#EXT-X-GAP` | ✓ (5 attempts in 1.5 s, then 5 playlists carry the gap; a once-refused segment arrives on its retry) |
| `DELETE` of expired segments | SHOULD (DASH-IF) | after the playlist that stops listing them; on for generic destinations, off for `youtube_live`; an endpoint answering `403`/`405`/`501` is not asked again | ✓ (18 deletes for 21 segments, the origin holding 3) |
| Master playlist | not accepted by YouTube | not sent | ✓ |

### Settings

Every `hls_push` destination takes these fields; a profile supplies the
defaults and the limits, and without one the generic limits apply:

| Field | `youtube_live` | no profile |
|---|---|---|
| `segment_duration_ms` | 1000–4000, default 2000 | 1000–30000, default: the stream's (2000) |
| `playlist_window` | 1–5, default 5 | 1–32, default: the stream's (5) |
| `method` | default `POST` | default `PUT` |
| `delete_expired` | default `false` | default `true` |

A value outside the limits is refused with `400`
(`profile_violation` with a profile, `invalid_hls_push` without), never
clamped.  The destination read reports the values in force, including
`segment_max_ms`, the longest segment the destination accepts (the profile's
maximum, or twice the duration asked for).

The segmenter is shared by every output of the stream, so it follows the
strictest HLS push destination: the shortest duration asked for, the
tightest maximum, and the longest window.  It changes on the tick after a
destination is added or removed and logs
`hls segmentation for <stream> is now <min>-<max> ms, window <n>`.  With no
destination, the defaults already satisfy YouTube.

### Retries and gaps, precisely

- A retryable answer is no answer (connection failure or timeout), `408`,
  `429` or `5xx`.  Any other `4xx` is not retried.
- The budget is the destination's segment duration (2 s when it states none),
  counted from the first attempt.
- A playlist that runs out of budget is dropped; the next one supersedes it.
  A segment that runs out of budget, or is dropped from a full queue, is
  listed as `#EXT-X-GAP` in the destination's playlists until it leaves the
  window.  It is marked in place, not removed, because RFC 8216 forbids
  removing a segment from the middle of a live playlist.

## Tests

- `make ffmpeg-interop` includes both directions of HLS push with ffmpeg:
  ffmpeg's HLS muxer pushing into `media_hls_ingest` with `PUT` and with
  `POST`, and nginx-media pushing into an ingest endpoint whose stored
  playlist ffmpeg then decodes.

- `make hls-ingest` pushes with ffmpeg's HLS muxer (`-f hls -method PUT`,
  unpadded names past `index9`, a playlist per segment, `delete_segments`) into
  a nested path and checks the program plays, the playlist uploads and the
  deletes were accepted, and every status code in the table above.  On the
  build before this change the same push stored nothing where its source
  reads: nested paths collapsed to the root and every playlist was refused.
- `test_hls_ingest` checks the reader's order: unpadded names by number, a
  playlist's order over name order, the window sliding, a missing listed
  segment passed over, a master playlist ignored.
- `make hls-push-conformance` checks the outbound side against the table
  above, with three destinations on one stream: `youtube_live` over TLS to a
  YouTube-shaped endpoint, a generic origin with a window of 3 and `DELETE`,
  and an origin that refuses one segment always and another once.
- `test_hls_playlist` checks the per-destination rewrite: the window trim with
  sequence numbers kept, the discontinuity sequence, `EXT-X-GAP`, a
  destination that joined later, master playlists declined, CRLF input, and
  an output buffer too small.
- `make hls-push` and the capacity benchmark's HLS push ladder check the
  outbound side against a sink that records every object, its order and the
  playlists that referenced segments not yet uploaded.

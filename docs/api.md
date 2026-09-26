# Control API

Enable it on a location:

```nginx
location /media/api/ {
    media_api;
}
```

The API authenticates nobody: no credential, no client-address check, no TLS of
its own.  It is exactly as protected as the location serving it, so serve that
location on loopback, behind a reverse proxy that authenticates, or on a Unix
socket — `security.md` section 3 has the safe shapes, and what a caller who
reaches it can do to a deployment — and never beside a public `listen`.

All routes are under `/media/api/v1/`.  Responses are bounded JSON with
explicit status codes; the buffer is fixed size, so a pathological registry
produces a truncated error rather than an unbounded allocation.  Everything
below is what `tests/integration/api_switch_nginx.sh` asserts against two real
SRT publishers; the graph routes are what `tests/integration/api_graph_nginx.sh`
drives from a deployment that declares no streams at all.

## Routes

| Method | Route | Meaning |
|---|---|---|
| GET | `/media/api/v1/streams` | every registered program |
| POST | `/media/api/v1/streams` | create a stream |
| GET | `/media/api/v1/streams/{application}/{name}` | one program, with its sources and destinations |
| PATCH | `/media/api/v1/streams/{application}/{name}` | change the program's selector timeouts |
| DELETE | `/media/api/v1/streams/{application}/{name}` | tear the stream down |
| GET | `/media/api/v1/streams/{application}/{name}/sources` | the source list for one program |
| POST | `/media/api/v1/streams/{application}/{name}/sources` | create a source |
| GET | `/media/api/v1/streams/{application}/{name}/sources/{id}` | one source |
| DELETE | `/media/api/v1/streams/{application}/{name}/sources/{id}` | remove a source |
| POST | `.../sources/{id}/enable`, `.../sources/{id}/disable` | put a source in or out of selection |
| GET | `/media/api/v1/streams/{application}/{name}/destinations` | the destination list |
| POST | `/media/api/v1/streams/{application}/{name}/destinations` | create and start a destination |
| GET | `/media/api/v1/streams/{application}/{name}/destinations/{id}` | one destination |
| DELETE | `/media/api/v1/streams/{application}/{name}/destinations/{id}` | stop and remove a destination |
| POST | `/media/api/v1/streams/{application}/{name}/switch?source={id}` | promote a source now |
| POST | `/media/api/v1/streams/{application}/{name}/switchback` | return to the configured winner |
| GET | `/media/api/v1/desired` | the whole graph as one document |
| PUT | `/media/api/v1/desired` | reconcile a desired-state document |
| GET | `/media/api/v1/metrics` | Prometheus text metrics |

Other methods return `405`, unknown streams and unknown sources return `404`,
and a missing or malformed query argument returns `400`.

A stream, a source and a destination are all runtime objects with a stable id,
created and addressed at runtime rather than declared in configuration.  The
graph routes are create-or-update: a create that names an object that already
exists returns the existing one rather than failing, and a delete of an object
that is not there succeeds, because the caller asked for an end state and that
end state holds.  Replay and retry are therefore both safe, which is what makes
a controller restart survivable.

## Collection

```json
{"streams":[
  {"application":"live","name":"news","owner":0,"observed_here":true,
   "revision":9,"media":"source",
   "generation":2,"switches":1,"emergency_switches":0,
   "failure_timeout_ms":1500,"recovery_timeout_ms":10000,
   "switchback":1,"program_frames":696,
   "active":"encoder-b",
   "sources":[
     {"id":"encoder-a","type":1,"state":"standby","priority":100,
      "healthy":true,"eligible":true,"active":false,"compat":"ready",
      "evidence":127,"health_transitions":0,"container_errors":0,
      "frames_in":812,"frames_out":410,"writers":1,
      "preroll_units":77,"preroll_bytes":495866,"preroll_overflows":0},
     {"id":"encoder-b","type":1,"state":"active","priority":50,
      ...}],
   "fanout_ms":{"p50":4,"p95":16,"p99":32,"max":41,
     "bucket_upper_ms":[1,2,4,8,16,32,64,128,256,512,1024,2048,4096,8192,16384,null],
     "bucket_counts":[0,0,1700,1430,220,40,10,0,0,0,0,0,0,0,0,0]},
   "dispatched":3400}
],"count":1}
```

Field notes:

- `generation` increments on every switch; `switches` counts them, and
  `emergency_switches` counts the ones the selector took because the active
  source failed rather than because an operator asked.
- `state` is the source's own view (`standby`, `awaiting_sync`, `active`,
  `draining`); `active` is the program's pointer to the source on air.  They
  agree for the selected source and disagree nowhere else.
- `type` is the source type: `1` srt, `2` rtmp, `3` file, `4` hls_pull, `5`
  hls_push.  The create routes accept the names as well as the numbers.
- `healthy` and `eligible` are the layered health verdict and the selection
  verdict.  A healthy source can be ineligible (incompatible tracks, for
  instance) and the JSON says so rather than hiding it behind one score.
- `evidence` is a bitmask of the health layers currently satisfied — useful
  when the question is *which* layer is unhappy.
- `compat` is the track contract verdict against the program: `ready` for
  interchangeable, `degraded` for switchable but the program changes shape,
  `incompatible` when a program track has no counterpart or the codec differs,
  and `unknown` before the source has declared tracks.
- `preroll_units`/`preroll_bytes` describe the standby GOP cache; the cache is
  what makes a switch land on a keyframe without a visible stall.
- `switchback` is the configured policy (`1` auto, `2` manual, `3` never).
- `owner` is the worker slot that drives the program and `observed_here` says
  whether the worker answering this read is it.  Both are computed the same way
  on every worker — see "Ownership" below — so they agree across the
  deployment, which is the point of them.
- `fanout_ms` reports p50/p95/p99 and the observed maximum delay, in
  milliseconds from program publish to consumer dispatch.  The percentiles
  are conservative upper bounds of the histogram buckets; `max` is the exact
  observed maximum, capped at 60000 ms.  `bucket_upper_ms` lists inclusive
  bucket upper bounds, with `null` for the final bucket above 16384 ms;
  `bucket_counts` gives the per-bucket sample counts in the same order, and
  their sum equals `dispatched`.  `dispatched` counts units consumers have
  taken.

## Ownership

A program is driven by exactly one worker, and which one is a function of the
program's identity rather than of where a request landed: every worker computes
`FNV-1a64(application/stream) % worker_processes` and gets the same answer, so
`owner` is the same slot in every worker's copy of the document and no request
can move it.  With one worker every program is that worker's, and nothing here
can happen.

Reads are answered anywhere, because the graph is replicated: a worker that
does not own a program still reports it, and `observed_here` is how a caller
tells the two apart.  On the owner the whole document is the running program's
own state.  On a replica `generation` and `program_frames` are what the owner
published in the shared directory — and `0` when the owner is not reporting at
all, which is deliberate: a replica saying "this program has carried nothing"
would be indistinguishable from a program that is running elsewhere.  The rest
of the document on a replica — the per-source counters, `switches`,
`fanout_ms`, `dispatched` — belongs to the answering worker's own copy, which
is not driving the program, and so says nothing about it.

Mutations that act on the *running* program are the exception, and they are
answered only by the owner:

- a manual switch (`POST .../switch`) and `POST .../switchback`, which move the
  selector;
- every destination mutation — create, delete, and anything else that is not a
  `GET` on `.../destinations` — because an output started on a worker that does
  not drive the program is never handed media;
- a desired-state document that names a destination the worker does not own.

Anywhere else they are refused with `409` rather than accepted into a copy that
nothing watches:

```json
{"error":"not_owner","owner":2}
```

`owner` names the slot from the same hash, so a controller retries the request
there instead of guessing.  The desired-state form names the stream too —
`{"error":"destination_needs_owner","owner":2,"stream":"live/news"}` — and what
was applied of the document before that point stays applied, which is safe
because applying it again is idempotent.

Streams, sources and a source's desired state are not refused: they are
replicated state, and a worker that does not own the program registers them and
lets the operation reach the owner, which is the worker that opens a source's
reader and runs selection.

## Streams

`POST /media/api/v1/streams` takes `{"application":"live","name":"news"}` and
returns `201` with `{"application":"live","name":"news","revision":1,
"created":true}`.  Creating a stream does not require a connected source, so the
object exists before anything publishes to it and a source can be attached
first.  Creating one that already exists returns `200` with `"created":false`
and the object's current revision.

The optional `media` field selects the stream's representation:

```json
{"application":"live","name":"news","media":"profile"}
```

`"source"` (the default) forwards the selected source representation.
`"profile"` sends the program through the configured external FFmpeg adapter
before package and egress stages.  A profile request is validated before graph
activation and returns `400` with `{"error":"transform_unavailable"}` when
`media_transform_ffmpeg` is not configured; an unknown value returns
`{"error":"unknown_media_mode"}`.  The current mode is returned as
`"media":"source"` or `"media":"profile"` by create, patch, collection and
stream detail responses.  `PATCH` and `PUT /desired` accept the same field and
advance the stream revision when it changes.

`GET /media/api/v1/streams/{application}/{name}` reports the program with
both of the lists it owns: `sources` in the shape the collection route uses,
and `destinations` in the shape the destination routes use.  A destination is
part of what a program is attached to, so reading a stream and then reading
its destinations separately should not be the only way to see the second half
of the answer.  `GET .../destinations` remains the place to address one list
directly.

`PATCH` changes the program's selector timeouts:

```json
{"revision":7,"failure_timeout_ms":2500,"recovery_timeout_ms":8000}
```

Both fields are optional, each must be at least `1`, and a bad value is `400`
with `bad_failure_timeout` or `bad_recovery_timeout`.  `DELETE` answers even
when the stream is gone, with `{"deleted":false,"reason":"absent"}`, because the
caller asked for an end state that already holds.  Deleting a stream that is
not on this worker still tells the workers that do have it: the two requests of
a controller — add a source, remove the stream — can land on different workers,
and the delete is authoritative for the deployment, not for the worker that
happened to answer it.  The response carries the revision the deletion moved
past (`"revision":N`), so a caller can tell a delete that was superseded by a
newer write from one that was applied and then recreated.

### Revisions

Every object carries a `revision`, taken from one sequence shared by every
worker and advanced by every mutation of the object or of a child that belongs
to it.  A mutation may state the revision the caller last saw — `?revision=N`
on `DELETE`, `"revision":N` in a `PATCH` body — and a mismatch is refused with
`409` and `{"error":"stale_revision","revision":N}` rather than overwriting a
newer desired state.  This is compare-and-set, not a version history: only the
current number is kept, which is exactly what a controller needs to notice that
someone else has written.  A mutation that carries no revision is applied
unconditionally.

Because the sequence is shared, the numbers mean the same thing on every worker,
which is what a deployment with more than one of them needs: two writes to one
stream that race are ordered by revision, every worker ends up holding the newer
one, and the loser's revision is gone from the graph.  So an unconditional write
that loses a race does not come back with an error — it is simply superseded —
and a controller that wants to be told about the race states the revision it
last saw and gets `409` instead.  The revision is reported by
`GET .../streams/{app}/{stream}` and by `GET /desired`.

## Sources

```
POST   /media/api/v1/streams/{app}/{name}/sources
GET    /media/api/v1/streams/{app}/{name}/sources
GET    .../sources/{id}
DELETE .../sources/{id}
POST   .../sources/{id}/enable
POST   .../sources/{id}/disable
```

A create takes `{"id":"encoder-b","type":"file","priority":50,
"path":"/srv/slate.ts"}`.  `type` is one of `srt`, `rtmp`, `file`, `hls_pull`,
`hls_push`, matched case-insensitively, and defaults to `srt` when omitted;
`priority` defaults to `0` and higher wins.  A source is created without a
transport attached — the id is a label the operator chooses and a publisher
presenting it attaches to this object — except for the three types that own a
reader:

- `file` needs `path`, the MPEG-TS file to read.  The owner opens it when the
  source is created and reads one bounded chunk per runtime tick, so a large
  file cannot stall a worker.
- `hls_push` needs `path`, the directory the source watches for uploaded
  segments — the `media_hls_ingest` target.  A program's own HLS output is
  `<media_hls>/<application>/<name>`, which is a different thing: that is the
  directory an `hls_push` *destination* is pointed at.
- `hls_pull` needs `path`, the playlist URL to fetch, and accepts an optional
  `ca_file` for an HTTPS origin whose certificate is not in the system store.

A file that cannot be opened, and an ingest directory that is not readable,
fail the create with `400` `{"error":"source_open_failed"}` rather than leaving
a source that never produces.  That is the answer on the worker that owns the
stream, which is the one that opens the reader.  A create that lands on a
replica registers the source as desired state and answers `201`: the reader is
opened by the owner when the operation reaches it, so a path the owner cannot
open is reported in the owner's log (`media: could not register source ...`)
and not in the response the caller got.  A pull source is the exception to the
`400`: its reader does the first fetch on its own thread, so a playlist that
cannot be reached is counted as a fetch failure and the source stays
unproductive until the origin answers — whereupon it fails health like any
source that has produced nothing.

`GET .../sources` returns `{"sources":[...]}` with the same objects the stream
detail embeds.  `GET .../sources/{id}` returns the smaller form the create and
delete replies echo:

```json
{"id":"encoder-b","type":3,"priority":50,"enabled":true,
 "state":"standby","revision":4}
```

The desired-state document also preserves reader inputs: every source includes
`"path"` (empty for transport-attached SRT/RTMP sources), and an HLS pull source
with a custom trust anchor includes `"ca_file"`.  Replaying `/desired` therefore
reopens the same file, ingest directory or playlist on the deterministic owner
instead of reducing a path-bearing source to a label.

`enable` and `disable` are desired state, accepted whatever the transport is
doing: disabling takes a source out of selection without tearing its session
down, which is what an operator wants when they intend to bring it back.

Deleting a source is idempotent like deleting a stream, and removing the
*active* source is a normal operation — the selector fails over through the same
path a failure would take.

## Switching

`POST .../switch?source=encoder-b` returns the same shape as the detail route
and takes effect at the incoming source's next keyframe when
`media_failover_switch_keyframe` is on.  An unknown source is `404`, a missing
`source` argument is `400`, and a promotion the selector refuses is `500` with
`{"error":"switch_failed"}` — the request was well formed, the program just
could not act on it.

`POST .../switchback` returns the program to the configured winner, or `409`
when it is already there or the policy is `never`.

## Destinations

```
POST   /media/api/v1/streams/{app}/{name}/destinations
GET    /media/api/v1/streams/{app}/{name}/destinations
GET    .../destinations/{id}
DELETE .../destinations/{id}
```

A destination is a runtime object with a stable id, and it starts as soon as
it is created, so adding one while a program is live is the normal case.
Creating an existing one returns it, and deleting an absent one succeeds:
replay and retry are both safe.

For a socket destination (`srt`, `rtmp`):

```json
{"id":"sink1","type":"srt","host":"127.0.0.1","port":9100,
 "streamid":"#!::r=live/news,m=publish,s=out"}
```

For an HLS push destination, `host` is the endpoint URL and `path` is the
program's HLS output directory; there is no port:

```json
{"id":"cdn","type":"hls_push",
 "host":"http://origin.example/live/news/",
 "path":"/var/lib/nginx/media/hls/live/news"}
```

The path must exactly match `<media_hls>/<application>/<name>`.  The
segmenter notifies matching destinations after each segment or playlist is
atomically renamed; there is no directory scan.  A destination pointed at a
different directory starts but receives no files.

A destination is answered as `{"id","type","host","port","enabled","revision"}`
with the type numbers `srt` 1, `rtmp` 2, `hls_push` 3, `record` 4, and
`GET .../destinations` wraps the list in `{"destinations":[...],"count":N}`.
A port is required for every type except `hls_push`.  `srt`, `rtmp` and
`hls_push` have a backend in this build; the remaining type names are accepted
by the API and then fail to start with `500`, because nothing here can carry
them.  An `rtmp` destination is an RTMP client: it connects out, publishes the
program as `streamid` (or the stream name when none is given) and carries the
same FLV messages the RTMP players get, so an H.265 program keeps the
enhanced-RTMP signalling it arrived with.

Uploads run on a four-thread-ceiling pool; active concurrency starts at one
and adapts within the worker's shared CPU budget.  Each destination has one
upload in flight, a 64-file queue and one kept HTTP/1.1 connection.  A failed
upload waits at the head of its queue and is retried within the destination's
segment duration; a segment never delivered is listed as `#EXT-X-GAP` in the
playlists that destination is sent.  Queue overflow drops and counts the
oldest queued file.  The notifier opens each sealed inode once, so HLS
retention can unlink or replace its path without changing the queued snapshot.
Deletion removes the destination from scheduling and drops queued files;
an upload already in flight keeps its file and destination references until it
finishes or times out.

### Endpoint credentials are not reported

An HLS endpoint carries its key in the URL — YouTube's ingest does — so the
endpoint is secret material.  Everything that reports one goes through the same
redaction: everything from a `?` on is dropped, so
`https://a.upload.youtube.com/http_upload_hls?cid=SECRET` reports as
`https://a.upload.youtube.com/http_upload_hls`, and a URL that carries its
credential as userinfo, `https://user:key@host/`, keeps its scheme and nothing
else, reporting as `https:***`.  A value that is not URL-shaped passes through
unchanged.

The redaction happens where an endpoint is reported rather than being
remembered at each call site, so the destination read, the desired-state
document and the log line that announces a destination all report the safe
form, and a new reporter cannot forget it.  What is left is enough for an
operator to know which endpoint a destination points at, and not enough to use
it.

### Profiles

An `hls_push` destination may name a platform profile:

```json
{"id":"yt","type":"hls_push","profile":"youtube_live",
 "host":"https://a.upload.youtube.com/http_upload_hls?cid=...",
 "path":"/var/lib/nginx/media/hls/live/news",
 "segment_duration_ms":2000,"playlist_window":5}
```

A profile is validation and defaults layered on the generic HLS publisher, not
a special path through the media core, so when the platform changes its rules
only the profile moves.  `youtube_live` requires an `https` endpoint, a segment
duration between 1000 and 4000 ms, and at most five outstanding segments in the
playlist; it fills 2000 ms, a window of five, `POST` and no `DELETE` when a
field is unset.  A configuration the platform would reject is refused — `400`
with `{"error":"profile_violation","detail":"..."}` — rather than clamped,
because silently changing an operator's number is worse than telling them it
is wrong.  An unknown profile is `400` `{"error":"unknown_profile"}`.

Without a profile the same fields apply with the generic limits:

| Field | Values | Default |
|---|---|---|
| `segment_duration_ms` | 1000–30000 | the stream's (2000) |
| `playlist_window` | 1–32 | the stream's (5) |
| `method` | `"PUT"`, `"POST"` | `"PUT"` |
| `delete_expired` | `true`, `false` | `true` |

A value outside them is `400` `{"error":"invalid_hls_push","detail":"..."}`.
The destination read reports what is in force:

```json
{"id":"yt","type":3,"host":"https://a.upload.youtube.com/http_upload_hls",
 "port":0,"enabled":true,"revision":1,"profile":"youtube_live",
 "segment_duration_ms":2000,"segment_max_ms":4000,"playlist_window":5,
 "method":"POST","delete_expired":false}
```

The settings reach the wire: the stream's segmenter follows its strictest HLS
push destination, and each destination is sent a playlist rewritten to its own
window.  An endpoint with a query string, as YouTube's is, receives each
object's name in its `file=` parameter; a path endpoint receives it as the last
path component.  The contract is tested with a YouTube-shaped endpoint
(`tests/integration/hls_profile_nginx.sh`,
`tests/integration/hls_push_conformance.sh`); nothing in the test suite talks
to the platform.

The profile's validated numbers are recorded on the destination.  They do not
yet drive the segmenter, which still decides its own segmentation, and the
profile's upload-method flag is not consulted by the publisher either — the
generic publisher always uploads with PUT.  What the profile guarantees today is
that the destination is one the platform would accept.

## Desired state

```
GET /media/api/v1/desired
PUT /media/api/v1/desired
```

An apply is accepted as `PUT` or as `POST`.

`GET` returns the whole graph as one document, in the shape `PUT` accepts:

```json
{"streams":[
  {"application":"live","name":"news","revision":9,"media":"source",
   "sources":[{"id":"encoder-a","type":1,"priority":100,"enabled":true,
               "revision":2}],
   "destinations":[{"id":"cdn","type":3,"host":"http://origin/","port":0,
                    "enabled":true,"revision":3}],
   "fanout_ms":{"p50":4,"p95":16,"p99":32,"max":41,
     "bucket_upper_ms":[1,2,4,8,16,32,64,128,256,512,1024,2048,4096,8192,16384,null],
     "bucket_counts":[0,0,1700,1430,220,40,10,0,0,0,0,0,0,0,0,0]},
   "dispatched":3400}],
 "count":1}
```

This is the replay contract that stands in for a database: a controller keeps
the document and re-applies it after a restart, and because every create is
idempotent the replay cannot duplicate anything.
`tests/integration/api_graph_nginx.sh` saves the document, applies it again and
asserts that the child counts are unchanged.

`PUT` is create-or-update.  Streams that exist are reused, sources and
destinations that exist are left exactly as they are, and children the document
does not mention are not touched.  A stream absent from the document is *not*
deleted: pruning is the controller's decision, made with the delete calls, not
a side effect of a replay.  The reply is
`{"applied":N,"children_created":M}`.  A body without a `streams` array is
`400` `streams_array_required`, and a child whose `type` is not a known name or
number is `400` `child_apply_failed`.

A source `type` in the document may be written as a name or as the number the
read side emits, so a document round-trips through `GET` and `PUT` unchanged.
Destinations in the document are started, as the `POST .../destinations` route
starts them.  Sources, though, are registered as the label the document names:
the readers that `POST .../sources` opens for `file`, `hls_pull` and `hls_push`
are not opened from a document, so a deployment that needs one of those creates
it with the `POST .../sources` route and it then appears in the document.

A request body is capped at 8192 bytes — the bodies are flat objects of short
strings, and a fixed parser reads exactly that rather than pulling in a JSON
library — so a document larger than that is `400` `body_too_large`.  This is an
API for a graph of tens of streams, not thousands.

## Metrics

`GET /media/api/v1/metrics` is Prometheus text format.  Every value is already
maintained by the program runtime, so scraping costs one walk of the registry
and nothing is computed on the request path.

Each response contains one `nginx_media_worker_info{worker,pid} 1` series
identifying the worker that served it.  Worker counters are process-local, not
cluster aggregates; with `reuseport`, a new connection can be served by a
different worker.

```
nginx_media_stream_generation{application="live",name="news"} 2
nginx_media_stream_switches{application="live",name="news"} 1
nginx_media_stream_program_frames{application="live",name="news"} 696
nginx_media_stream_fanout_delay_ms{application="live",name="news",percentile="50"} 3
nginx_media_stream_fanout_delay_ms{application="live",name="news",percentile="95"} 11
nginx_media_stream_fanout_delay_ms{application="live",name="news",percentile="99"} 24
nginx_media_stream_dispatched_total{application="live",name="news"} 3400
nginx_media_stream_feed_units{application="live",name="news"} 12
nginx_media_stream_feed_bytes{application="live",name="news"} 491520
nginx_media_source_frames_in{application="live",name="news",source="encoder-a"} 812
nginx_media_source_frames_out{application="live",name="news",source="encoder-a"} 410
nginx_media_source_healthy{application="live",name="news",source="encoder-a"} 1
nginx_media_source_active{application="live",name="news",source="encoder-a"} 0
nginx_media_source_active{application="live",name="news",source="encoder-b"} 1
nginx_media_runtime_outputs 3
nginx_media_streams_draining 0
nginx_media_worker_event_loop_delay_ms 100
nginx_media_worker_event_loop_max_delay_ms 143
nginx_media_worker_late_ticks_total 2
nginx_media_worker_info{worker="0",pid="1234"} 1
nginx_media_source_payload_bytes_in_total{worker="0",application="live",name="news",source="encoder-a"} 8120000
nginx_media_srt_egress_shard_destinations{worker="0",shard="0"} 2
nginx_media_srt_egress_shard_feed_queue_units{worker="0",shard="0"} 0
nginx_media_srt_egress_shard_feed_queue_bytes{worker="0",shard="0"} 0
nginx_media_srt_egress_shard_feed_queue_dropped_total{worker="0",shard="0"} 0
nginx_media_srt_egress_shard_output_queue_units{worker="0",shard="0"} 0
nginx_media_srt_egress_shard_output_queue_bytes{worker="0",shard="0"} 0
nginx_media_srt_egress_shard_output_dropped_total{worker="0",shard="0"} 0
nginx_media_srt_egress_shard_sent_bytes_total{worker="0",shard="0"} 16240000
nginx_media_srt_egress_shard_sent_bursts_total{worker="0",shard="0"} 400
nginx_media_srt_egress_shard_blocked_sends_total{worker="0",shard="0"} 0
nginx_media_srt_egress_shard_retransmitted_packets_total{worker="0",shard="0"} 2
```

Series appear for every registered program and every source of it, so
`nginx_media_source_active` is the cheapest way to alert on "the program lost
its source": no source with value `1` means nothing is on air.

Capacity and health metrics include:

- **Fanout delay.**  `nginx_media_stream_fanout_delay_ms{percentile="50|95|99"}`
  is `dispatch_time - program_publish_time`: how long a unit of media waits
  after the program publishes it before a consumer takes it.  The value is the
  upper bound of the histogram bucket the percentile falls in, which is the
  conservative reading — the true value is somewhere inside that bucket, so
  the series never reports better than reality.  `dispatched_total` is the
  count of units taken: a zero rate here while frames are still being produced
  means nobody is consuming the program.
- **Worker identity.**  `nginx_media_worker_info{worker,pid}` identifies the
  worker that generated the current response; per-worker gauges and counters
  immediately above are not aggregated across the NGINX workers.
- **Source payload bytes.** `nginx_media_source_payload_bytes_in_total` counts
  media-payload bytes accepted by the parser for each source, labeled by
  `worker`, `application`, `name`, and `source`.  It is source-side accounting,
  not bytes delivered to each destination or transport-wire bytes.
- **Feed lag.**  `nginx_media_stream_feed_units` and `_feed_bytes` are what the
  program feed still retains — the backlog a slow consumer is running against.
  Both are gauges with a ceiling, so a value pinned at the ceiling is the
  symptom to alert on, not a number to graph for trend.
- **SRT egress shards.** `nginx_media_srt_egress_shard_*` series are labeled by
  `worker` and one of 16 stable logical `shard` IDs.  Destination placement
  stays on its logical shard while physical sender concurrency adapts.
  `destinations` is the active destination-count gauge.
  `feed_queue_units`/`feed_queue_bytes` and `output_queue_units`/`output_queue_bytes`
  are queue gauges; output values aggregate the destinations assigned to that
  shard.  `nginx_media_srt_egress_shard_feed_queue_dropped_total` and
  `nginx_media_srt_egress_shard_output_dropped_total` count bursts refused by
  the bounded shard-feed and per-destination queues.  The feed queue is capped
  at 64 units / 8 MiB per shard; each destination queue at 256 units / 8 MiB.
  `nginx_media_srt_egress_shard_sent_bytes_total` counts payload accepted by
  local SRT sender sockets, not receiver-delivered bytes.
  `sent_bursts_total`, `blocked_sends_total`, and
  `retransmitted_packets_total` expose completed bursts, send backpressure,
  and transport retransmissions.  Receiver-delivered bytes require
  receiver-side accounting, as used by `bench-capacity-curve`.

- **Worker egress budget.** `nginx_media_egress_available_cpu_milli` reports
  affinity/cgroup capacity.  `nginx_media_egress_worker_cpu_permille` is the
  worker's process CPU share of that capacity, and
  `nginx_media_egress_event_loop_lag_msec` is the runtime-tick gap.  The
  worker-local manager runs one SRT sender per shard in use, up to the
  granted CPUs.  HLS push grows on pressure within the CPUs left after one
  for the owning event loop and the CPU the SRT senders use.  HLS push queue
  pressure requires queue lag or bytes to rise across samples; a draining
  startup backlog alone does not expand the pool.  New drops or backpressure
  are pressure signals; SRT retransmissions do not change the sender count.
- **Destination egress.** `nginx_media_egress_delivered_bytes_total` counts
  bytes accepted by the destination transport, not receiver-acknowledged bytes.
  `nginx_media_egress_dropped_units_total`,
  `nginx_media_egress_transport_errors_total`,
  `nginx_media_egress_backpressure_events_total`,
  `nginx_media_egress_reconnects_total`, and
  `nginx_media_egress_deadline_misses_total` are per-destination counters;
  `nginx_media_egress_queue_bytes` and
  `nginx_media_egress_queue_lag_ms` are gauges.  Labels include worker,
  application, stream name, destination, protocol, engine, placement,
  incarnation, representation ID and epoch, and feed ID and epoch.
  `nginx_media_egress_active_workers` reports SRT sender concurrency, the
  owning RTMP event loop, and HLS upload concurrency.
  `nginx_media_egress_engine_cpu_permille` is emitted for measured SRT and HLS
  sender-thread pools; RTMP CPU remains part of the owning event loop.
- **RTMP media scheduler.** `nginx_media_rtmp_runnable_destinations` and
  `nginx_media_rtmp_write_blocked_destinations` are worker-local gauges for
  queued work and sockets awaiting write readiness.
  `nginx_media_rtmp_oldest_runnable_age_ms` reports scheduler queue age.
  `nginx_media_rtmp_scheduler_visit_destinations`,
  `nginx_media_rtmp_scheduler_visit_bytes_queued`,
  `nginx_media_rtmp_scheduler_visit_units_pumped`, and
  `nginx_media_rtmp_scheduler_visit_service_us` describe the last bounded
  visit. `nginx_media_rtmp_scheduler_destinations_visited_total`,
  `nginx_media_rtmp_scheduler_bytes_queued_total`,
  `nginx_media_rtmp_scheduler_units_pumped_total`,
  `nginx_media_rtmp_scheduler_service_us_total`, and
  `nginx_media_rtmp_scheduler_reposts_total` are cumulative counters.

- **Worker event loop.**  `nginx_media_worker_event_loop_delay_ms` is the gap
  between the last two runtime ticks, which the timer asks to be 100 ms, so
  anything above it is time the worker could not get back to its timer.
  `_max_delay_ms` is the worst gap this worker has seen and
  `_late_ticks_total` counts ticks that missed their interval by more than
  half.  `nginx_media_runtime_outputs` is the number of per-stream outputs in
  use, allocated on demand; a count that does not fall after streams are
  deleted means teardown is leaking one.  `nginx_media_streams_draining`
  counts deleted streams whose memory is still held by a reader that owns a
  thread and is stopping: it returns to zero on its own within a tick or two,
  and a value that stays up is a reader that will not leave.

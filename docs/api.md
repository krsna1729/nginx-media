# Control API

Enable it on a location:

```nginx
location /media/api/ {
    media_api;
}
```

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
| GET | `/media/api/v1/streams/{application}/{name}` | one program, with its sources |
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
  {"application":"live","name":"news",
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
   "fanout_ms":{"p50":3,"p95":11,"p99":24,"max":41},
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
- `fanout_ms` is the program's fanout delay — the time from when the program
  published a unit of media to when a consumer took it — at the 50th, 95th and
  99th percentile and the observed maximum, and `dispatched` counts the units
  consumers have taken.  This is the number an operator can feel: it says
  whether the deployment has headroom or is spending its slack on a slow
  destination.

## Streams

`POST /media/api/v1/streams` takes `{"application":"live","name":"news"}` and
returns `201` with `{"application":"live","name":"news","revision":1,
"created":true}`.  Creating a stream does not require a connected source, so the
object exists before anything publishes to it and a source can be attached
first.  Creating one that already exists returns `200` with `"created":false`
and the object's current revision.

`PATCH` changes the program's selector timeouts:

```json
{"revision":7,"failure_timeout_ms":2500,"recovery_timeout_ms":8000}
```

Both fields are optional, each must be at least `1`, and a bad value is `400`
with `bad_failure_timeout` or `bad_recovery_timeout`.  `DELETE` answers even
when the stream is gone, with `{"deleted":false,"reason":"absent"}`, because the
caller asked for an end state that already holds.

### Revisions

Every object carries a `revision`, incremented by every mutation of it or of a
child that belongs to it.  A mutation may state the revision the caller last
saw — `?revision=N` on `DELETE`, `"revision":N` in a `PATCH` body — and a
mismatch is refused with `409` and `{"error":"stale_revision","revision":N}`
rather than overwriting a newer desired state.  This is compare-and-set, not a
version history: only the current number is kept, which is exactly what a
controller needs to notice that someone else has written.  A mutation that
carries no revision is applied unconditionally.

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

- `file` needs `path`, the MPEG-TS file to read.  The file is opened by the
  create call and read one bounded chunk per runtime tick, so a large file
  cannot stall a worker.
- `hls_push` needs `path`, the directory an uploader writes segments into.
- `hls_pull` needs `path`, the playlist URL to fetch, and accepts an optional
  `ca_file` for an HTTPS origin whose certificate is not in the system store.

A file that cannot be opened, and an ingest directory that is not readable,
fail the create with `400` rather than leaving a source that never produces.
A pull source is the exception: its reader does the first fetch on its own
thread, so a playlist that cannot be reached is counted as a fetch failure and
the source stays unproductive until the origin answers — whereupon it fails
health like any source that has produced nothing.

`GET .../sources` returns `{"sources":[...]}` with the same objects the stream
detail embeds.  `GET .../sources/{id}` returns the smaller form the create and
delete replies echo:

```json
{"id":"encoder-b","type":3,"priority":50,"enabled":true,
 "state":"standby","revision":4}
```

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
output directory it watches; there is no port:

```json
{"id":"cdn","type":"hls_push",
 "host":"http://origin.example/live/news/",
 "path":"/var/lib/nginx/media/hls"}
```

A destination is answered as `{"id","type","host","port","enabled","revision"}`
with the type numbers `srt` 1, `rtmp` 2, `hls_push` 3, `record` 4, and
`GET .../destinations` wraps the list in `{"destinations":[...],"count":N}`.
A port is required for every type except `hls_push`.  Only `srt` and
`hls_push` have a backend in this build: the other type names are accepted by
the API and then fail to start with `500`, because nothing here can carry them.

Uploads run on a bounded pool, and each destination has a bounded queue: a
stalled remote drops its oldest queued segment and counts it rather than
stalling the program or its neighbours.  Deleting one stops it and unlinks it
first, so nothing it queued outlives it.

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
 "path":"/var/lib/nginx/media/hls",
 "segment_duration_ms":2000,"playlist_window":5}
```

A profile is validation and defaults layered on the generic HLS publisher, not
a special path through the media core, so when the platform changes its rules
only the profile moves.  `youtube_live` requires an `https` endpoint, a segment
duration between 1000 and 4000 ms, and at most five outstanding segments in the
playlist; it fills 2000 ms and a window of five when a field is unset.  A
configuration the platform would reject is refused — `400` with
`{"error":"profile_violation","detail":"..."}` — rather than clamped, because
silently changing an operator's number is worse than telling them it is wrong.
An unknown profile is `400` `{"error":"unknown_profile"}`.  The profile is
tested as a contract (`tests/integration/hls_profile_nginx.sh`); nothing in the
test suite talks to the platform.

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
  {"application":"live","name":"news","revision":9,
   "sources":[{"id":"encoder-a","type":1,"priority":100,"enabled":true,
               "revision":2}],
   "destinations":[{"id":"cdn","type":3,"host":"http://origin/","port":0,
                    "enabled":true,"revision":3}],
   "fanout_ms":{"p50":3,"p95":11,"p99":24,"max":41},
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
nginx_media_worker_event_loop_delay_ms 100
nginx_media_worker_event_loop_max_delay_ms 143
nginx_media_worker_late_ticks_total 2
```

Series appear for every registered program and every source of it, so
`nginx_media_source_active` is the cheapest way to alert on "the program lost
its source": no source with value `1` means nothing is on air.

Three groups are worth knowing:

- **Fanout delay.**  `nginx_media_stream_fanout_delay_ms{percentile="50|95|99"}`
  is `dispatch_time - program_publish_time`: how long a unit of media waits
  after the program publishes it before a consumer takes it.  The value is the
  upper bound of the histogram bucket the percentile falls in, which is the
  conservative reading — the true value is somewhere inside that bucket, so
  the series never reports better than reality.  `dispatched_total` is the
  count of units taken: a zero rate here while frames are still being produced
  means nobody is consuming the program.
- **Feed lag.**  `nginx_media_stream_feed_units` and `_feed_bytes` are what the
  program feed still retains — the backlog a slow consumer is running against.
  Both are gauges with a ceiling, so a value pinned at the ceiling is the
  symptom to alert on, not a number to graph for trend.
- **Worker event loop.**  `nginx_media_worker_event_loop_delay_ms` is the gap
  between the last two runtime ticks, which the timer asks to be 100 ms, so
  anything above it is time the worker could not get back to its timer.
  `_max_delay_ms` is the worst gap this worker has seen and
  `_late_ticks_total` counts ticks that missed their interval by more than
  half.  `nginx_media_runtime_outputs` is the number of per-stream output slots
  in use; it is bounded, and a count that does not fall after streams are
  deleted means teardown is leaking a slot.

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
SRT publishers.

## Routes

| Method | Route | Meaning |
|---|---|---|
| GET | `/media/api/v1/streams` | every registered program |
| GET | `/media/api/v1/streams/{application}/{name}` | one program, with its sources |
| GET | `/media/api/v1/streams/{application}/{name}/sources` | the source list for one program |
| POST | `/media/api/v1/streams/{application}/{name}/switch?source={id}` | promote a source now |
| POST | `/media/api/v1/streams/{application}/{name}/switchback` | return to the configured winner |
| GET | `/media/api/v1/metrics` | Prometheus text metrics |

Other methods return `405`, unknown streams and unknown sources return `404`,
and a missing or malformed query argument returns `400`.

## Collection

```json
{"streams":[
  {"application":"live","name":"news",
   "generation":2,"switches":1,"emergency_switches":0,
   "failure_timeout_ms":1500,"recovery_timeout_ms":10000,
   "switchback":0,"program_frames":696,
   "active":"encoder-b",
   "sources":[
     {"id":"encoder-a","type":0,"state":"standby","priority":100,
      "healthy":true,"eligible":true,"active":false,"compat":"compatible",
      "evidence":127,"health_transitions":0,"container_errors":0,
      "frames_in":812,"frames_out":410,"writers":1,
      "preroll_units":77,"preroll_bytes":495866,"preroll_overflows":0},
     {"id":"encoder-b","type":0,"state":"active","priority":50,
      ...}]}
]}
```

Field notes:

- `generation` increments on every switch; `switches` counts them, and
  `emergency_switches` counts the ones the selector took because the active
  source failed rather than because an operator asked.
- `state` is the source's own view (`active`, `standby`, `failed`, ...);
  `active` is the program's pointer to the source on air.  They agree for the
  selected source and disagree nowhere else.
- `healthy` and `eligible` are the layered health verdict and the selection
  verdict.  A healthy source can be ineligible (incompatible tracks, for
  instance) and the JSON says so rather than hiding it behind one score.
- `evidence` is a bitmask of the health layers currently satisfied — useful
  when the question is *which* layer is unhappy.
- `preroll_units`/`preroll_bytes` describe the standby GOP cache; the cache is
  what makes a switch land on a keyframe without a visible stall.
- `switchback` is the configured policy (`1` auto, `2` manual, `3` never).

## Switching

`POST .../switch?source=encoder-b` returns the same shape as the detail route
and takes effect at the incoming source's next keyframe when
`media_failover_switch_keyframe` is on.  An unknown source is `404`, a missing
`source` argument is `400`, and a promotion the selector refuses is `500` with
`{"error":"switch_failed"}` — the request was well formed, the program just
could not act on it.

`POST .../switchback` returns the program to the configured winner, or `409`
when it is already there or the policy is `never`.

## Metrics

`GET /media/api/v1/metrics` is Prometheus text format.  Every value is already
maintained by the program runtime, so scraping costs one walk of the registry
and nothing is computed on the request path.

```
nginx_media_stream_generation{application="live",name="news"} 2
nginx_media_stream_switches{application="live",name="news"} 1
nginx_media_stream_program_frames{application="live",name="news"} 696
nginx_media_source_frames_in{application="live",name="news",source="encoder-a"} 812
nginx_media_source_frames_out{application="live",name="news",source="encoder-a"} 410
nginx_media_source_healthy{application="live",name="news",source="encoder-a"} 1
nginx_media_source_active{application="live",name="news",source="encoder-a"} 0
nginx_media_source_active{application="live",name="news",source="encoder-b"} 1
```

Series appear for every registered program and every source of it, so
`nginx_media_source_active` is the cheapest way to alert on "the program lost
its source": no source with value `1` means nothing is on air.

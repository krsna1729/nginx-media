# Quickstart

A program that carries media, built entirely through the control API, in
about five minutes.  Nothing here needs a second machine.

Everything below was run to produce this document.  Where a number appears, it
came out of the run rather than out of the design.

## Build

```sh
make nginx      # fetches the pinned nginx and builds it with the module
make unit       # the core suites, under ASan/UBSan
```

`make nginx` puts the result in `.build/nginx-install/sbin/nginx`.  It never
touches a system nginx.

## A configuration with nothing declared

The runtime graph is built through the API, so the configuration only says
what the deployment *can* do, not what it *is* doing:

```nginx
worker_processes 1;
error_log logs/error.log info;
pid logs/nginx.pid;

events { worker_connections 256; }

media_hls /var/lib/nginx/media/hls;

http {
    server {
        listen 8080;

        location /media/api/ { media_api; }

        location /hls/ { alias /var/lib/nginx/media/hls/; }
    }
}
```

Start it and ask what programs exist:

```sh
curl -s http://127.0.0.1:8080/media/api/v1/streams
{"streams":[],"count":0}
```

An empty list, and that is the point: no stream is declared anywhere.

## A program

```sh
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"application":"live","name":"demo"}' \
    http://127.0.0.1:8080/media/api/v1/streams
```

## Something to carry

A file is the easiest first source, because it needs nothing else running.
It is paced by the runtime tick rather than read as fast as the disk allows,
so a large file cannot stall a worker:

```sh
ffmpeg -f lavfi -i "testsrc2=size=320x240:rate=25" -t 30 \
    -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p \
    -c:a aac -b:a 32k -f mpegts /tmp/demo.ts

curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"id":"file1","type":"file","path":"/tmp/demo.ts"}' \
    http://127.0.0.1:8080/media/api/v1/streams/live/demo/sources
```

The source is admitted through the same health model a live encoder goes
through - transport, data flow, container validity, advancing timestamps,
media validity - so a file that is corrupt or has the wrong shape is refused
here rather than downstream.

## Watch it come out

The local HLS output belongs to the program and starts with it:

```sh
curl -s http://127.0.0.1:8080/hls/index.m3u8
```

The playlist is `index.m3u8` in the HLS directory, and it is a rolling window
rather than an append-only log: segments that fall out of the window are
deleted, so the directory does not grow with uptime.

And the program's own view of itself:

```sh
curl -s http://127.0.0.1:8080/media/api/v1/streams/live/demo
```

Two fields are worth knowing about:

- `fanout_ms` is the document's primary capacity metric:
  `dispatch_time - program_publish_time`, reported at p50/p95/p99.  On an idle
  machine it reads `1`, which is the histogram's bucket bound and means
  "under two milliseconds".  When this climbs, the worker is falling behind.
- `generation` increments on every switch, and every consumer sees the
  discontinuity.  A player that reconnects has a reason to.

## A live encoder instead

An SRT publisher takes the same route:

```sh
ffmpeg -re -f lavfi -i "testsrc2=size=1280x720:rate=25" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p \
    -c:a aac -b:a 128k \
    -f mpegts "srt://127.0.0.1:9000?streamid=#!::r=live/demo,m=publish"
```

Add `media_srt_listen 0.0.0.0:9000;` to the configuration first.  A second
encoder publishing the same program is a second *source*, not a replacement:
give it a priority and it stands by until the selector promotes it.

## Where to go next

- `docs/configuration.md` - every directive
- `docs/api.md` - every route, with the JSON each one returns
- `docs/architecture.md` - why the program is not a publisher

## Container

```sh
docker build -f Containerfile -t nginx-media .
docker run -p 8080:8080 -p 9000:9000/udp -p 1935:1935 nginx-media
```

The image starts with the same empty graph and the same API on port 8080.

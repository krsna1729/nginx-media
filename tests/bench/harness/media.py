"""Media sources and publishers."""

import json
import os
import re

from . import api, proc


def make_source(cfg, path, bitrate):
    """A 30 s 720p25 x264 transport stream at `bitrate`, built once per run."""
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    proc.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
              "-i", "testsrc2=size=1280x720:rate=25",
              "-c:v", "libx264", "-preset", "ultrafast", "-g", "50", "-pix_fmt", "yuv420p",
              "-b:v", bitrate, "-maxrate", bitrate, "-minrate", bitrate, "-bufsize", "60M",
              "-t", "30", "-f", "mpegts", path], timeout=600)
    size = os.path.getsize(path) if os.path.exists(path) else 0
    if not size:
        raise proc.Failed("could not build the %s source" % bitrate)
    print("   source %s: %d KiB over 30s (%.1f Mbit/s)" % (bitrate, size // 1024, size * 8 / 30 / 1e6),
          flush=True)


def rate_bps(rate):
    """'8M' -> 8000000; positive and at most 60 Mbit/s."""
    match = re.fullmatch(r"(\d+(?:\.\d+)?)([kKmMgG]?)", rate)
    if match is None:
        raise proc.Failed("invalid bitrate: %s" % rate)
    bps = round(float(match.group(1)) * {"": 1, "k": 10**3, "m": 10**6, "g": 10**9}[match.group(2).lower()])
    if not 0 < bps <= 60_000_000:
        raise proc.Failed("bitrate must be positive and no higher than 60M")
    return bps


def ingest_key(cfg, name, kind="srt", source=None, priority=0):
    """Create live/<name> and its source on the sender; the source's ingest key."""
    source = source or "enc-%s" % name
    endpoint = api.base(cfg) + "/streams/live/" + name
    headers = ("Content-Type: application/json",)
    proc.curl(api.base(cfg) + "/streams", proc.SENDER, method="POST", headers=headers,
              data=json.dumps({"application": "live", "name": name}))
    _, body = proc.curl(endpoint + "/sources", proc.SENDER, method="POST", headers=headers,
                        data=json.dumps({"id": source, "type": kind, "priority": priority}))
    key = _key(body) or _key(proc.curl(endpoint + "/sources/%s/key" % source, proc.SENDER)[1])
    if not key:
        raise proc.Failed("sender issued no ingest key for %s" % name)
    return key


def _key(body):
    try:
        key = json.loads(body).get("key")
    except (ValueError, AttributeError):
        return None
    return key if isinstance(key, str) and key else None


def publish(scope, cfg, port, name, source, rate=None):
    """Loop `source` into the sender from the harness namespace, pinned to the
    publisher CPUs. `rate` pads the stream (-muxrate) so the packet rate is a
    parameter. nginx: SRT to media_addr:port; restream: its RTMP ingest."""
    argv = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-stream_loop", "-1",
            "-i", source, "-c", "copy"] + (["-muxrate", rate] if rate else [])
    if cfg.engine == "restream":
        argv += ["-f", "flv", "rtmp://%s:%d/live/live" % (cfg.media_addr, cfg.restream_rtmp_port)]
    else:
        argv += ["-f", "mpegts", "srt://%s:%d?mode=caller&streamid=%s"
                 % (cfg.media_addr, port, ingest_key(cfg, name))]
    return scope.spawn("publisher %s" % name, argv, ns=proc.HARNESS, cpus=cfg.publisher_cpus)

#!/usr/bin/env python3
"""Prepared elementary payload and independently observed HLS production."""
import argparse
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import time


def prepared_rate(path):
    with open(path, encoding="utf-8") as source:
        evidence = json.load(source)
    if (not isinstance(evidence["payload_bytes"], int) or evidence["payload_bytes"] <= 0
            or not math.isfinite(evidence["duration_s"]) or evidence["duration_s"] <= 0):
        raise ValueError("prepared media has invalid payload size or presentation duration")
    rate = evidence["payload_bytes"] * 8 / evidence["duration_s"]
    if not math.isfinite(rate) or rate <= 0 or not evidence.get("provenance"):
        raise ValueError("prepared elementary media evidence is not a positive rate")
    return rate, evidence["provenance"]


def production(path, start, end):
    with open(path, encoding="utf-8") as source:
        evidence = json.load(source)
    if (evidence.get("clock_domain") != "controller-monotonic"
            or not evidence.get("provenance")):
        raise ValueError("HLS production lacks independent controller-clock provenance")
    if any(not math.isfinite(entry["at"]) or not isinstance(entry["bytes"], int)
           or entry["bytes"] <= 0 for entry in evidence["segments"].values()):
        raise ValueError("HLS production contains invalid cut timestamps or segment sizes")
    entries = sorted(((name, entry) for name, entry in evidence["segments"].items()
                      if start <= entry["at"] <= end), key=lambda item: item[1]["at"])
    if len(entries) < 2:
        raise ValueError("HLS production needs at least two identified cuts inside its window")
    span = entries[-1][1]["at"] - entries[0][1]["at"]
    if span <= 0:
        raise ValueError("HLS production cut timestamps did not advance")
    rate = sum(entry["bytes"] for _, entry in entries[1:]) * 8 / span
    if not math.isfinite(rate) or rate <= 0:
        raise ValueError("invalid HLS production rate")
    return dict(entries), rate, span


def prepare(source, output):
    result = subprocess.run(["ffprobe", "-v", "error", "-show_packets",
                             "-show_entries", "packet=size,pts_time,duration_time",
                             "-of", "json", source], check=True, capture_output=True,
                            text=True, timeout=300)
    packets = json.loads(result.stdout)["packets"]
    begin = min(float(packet["pts_time"]) for packet in packets)
    finish = max(float(packet["pts_time"]) + float(packet.get("duration_time", 0))
                 for packet in packets)
    evidence = {"payload_bytes": sum(int(packet["size"]) for packet in packets),
                "duration_s": finish - begin,
                "provenance": "ffprobe elementary packet sizes / packet presentation span",
                "source": os.path.abspath(source)}
    Path(output).write_text(json.dumps(evidence), encoding="utf-8")
    prepared_rate(output)


def observe(playlist, output):
    stopped = False
    parent = os.getppid()
    def stop(*_):
        nonlocal stopped
        stopped = True
    signal.signal(signal.SIGTERM, stop)
    segments = {}
    playlist = Path(playlist)
    while True:
        try:
            lines = playlist.read_text(encoding="utf-8").splitlines()
            for name in lines:
                if not name or name.startswith("#") or name in segments:
                    continue
                before = time.monotonic_ns()
                realtime = time.time_ns()
                stat = (playlist.parent / name).stat()
                after = time.monotonic_ns()
                # Only playlist-published files are final. Their last write is
                # the segment cut; map local filesystem realtime to monotonic.
                at = (before + after) / 2 / 1e9 + (stat.st_mtime_ns - realtime) / 1e9
                segments[name] = {"bytes": stat.st_size, "at": at,
                                  "clock_uncertainty_s": (after - before) / 2e9}
        except FileNotFoundError:
            pass
        temporary = output + ".tmp"
        Path(temporary).write_text(json.dumps({"segments": segments,
            "provenance": "playlist-identified final segment sizes / local last-write cut timestamps",
            "clock_domain": "controller-monotonic"}), encoding="utf-8")
        os.replace(temporary, output)
        # Also stop if the harness died without signalling us.
        if stopped or os.getppid() != parent:
            return
        time.sleep(0.05)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "observe"))
    parser.add_argument("source")
    parser.add_argument("output")
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.source, args.output)
    else:
        observe(args.source, args.output)

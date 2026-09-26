#!/usr/bin/env python3
"""Exercise an HLS playlist with concurrent readers and report delivery quality."""

import argparse
import asyncio
import csv
import math
import signal
import sys
import time
from dataclasses import dataclass
from urllib.parse import parse_qsl, urlencode, urljoin, urlsplit, urlunsplit

REQUEST_TIMEOUT = 8.0
SEGMENT_CONCURRENCY_LIMIT = 32


class MeasurementError(Exception):
    pass


class HTTPError(Exception):
    pass


@dataclass
class Response:
    status: int
    headers: dict
    body: bytes


class HTTPClient:
    def __init__(self, reader_id):
        self.reader_id = reader_id
        self.reader = None
        self.writer = None

    async def close(self):
        if self.writer is not None:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except (ConnectionError, OSError):
                pass
            self.writer = None
            self.reader = None

    async def request(self, url):
        parts = urlsplit(url)
        if parts.scheme != "http" or not parts.hostname:
            raise HTTPError("only plain HTTP URLs are supported")
        query = parse_qsl(parts.query, keep_blank_values=True)
        query.append(("reader", str(self.reader_id)))
        target = urlunsplit(("", "", parts.path or "/", urlencode(query), ""))
        try:
            async with asyncio.timeout(REQUEST_TIMEOUT):
                if self.writer is None or self.writer.is_closing():
                    self.reader, self.writer = await asyncio.open_connection(
                        parts.hostname, parts.port or 80, limit=1024 * 1024)
                host = f"[{parts.hostname}]" if ":" in parts.hostname else parts.hostname
                if parts.port and parts.port != 80:
                    host = f"{host}:{parts.port}"
                self.writer.write((f"GET {target} HTTP/1.1\r\nHost: {host}\r\n"
                                   "User-Agent: hls-capacity-reader/1\r\n"
                                   "Accept: */*\r\nConnection: keep-alive\r\n\r\n").encode("ascii"))
                await self.writer.drain()
                status_line = await self.reader.readline()
                if not status_line or len(status_line) > 8192:
                    raise HTTPError("invalid HTTP status line")
                fields = status_line.decode("latin-1").strip().split(None, 2)
                if len(fields) < 2 or not fields[0].startswith("HTTP/"):
                    raise HTTPError("invalid HTTP status line")
                try:
                    status = int(fields[1])
                except ValueError as exc:
                    raise HTTPError("invalid HTTP status code") from exc
                headers = {}
                while True:
                    line = await self.reader.readline()
                    if not line:
                        raise HTTPError("connection closed in HTTP headers")
                    if line in (b"\r\n", b"\n"):
                        break
                    if len(line) > 65536 or b":" not in line:
                        raise HTTPError("invalid HTTP header")
                    key, value = line.split(b":", 1)
                    headers[key.decode("latin-1").strip().lower()] = value.decode("latin-1").strip()
                if "chunked" in headers.get("transfer-encoding", "").lower():
                    body = await self._read_chunked()
                elif "content-length" in headers:
                    try:
                        length = int(headers["content-length"])
                    except ValueError as exc:
                        raise HTTPError("invalid Content-Length") from exc
                    if length < 0:
                        raise HTTPError("negative Content-Length")
                    body = await self.reader.readexactly(length)
                else:
                    body = await self.reader.read()
                    await self.close()
                if headers.get("connection", "").lower() == "close":
                    await self.close()
                return Response(status, headers, body)
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, OSError,
                UnicodeError, HTTPError) as exc:
            await self.close()
            if isinstance(exc, HTTPError):
                raise
            raise HTTPError(str(exc) or "HTTP request failed") from exc

    async def _read_chunked(self):
        chunks = []
        total = 0
        while True:
            line = await self.reader.readline()
            try:
                size = int(line.split(b";", 1)[0].strip(), 16)
            except (ValueError, IndexError) as exc:
                raise HTTPError("invalid chunk size") from exc
            if size < 0:
                raise HTTPError("negative chunk size")
            if size == 0:
                while True:
                    trailer = await self.reader.readline()
                    if trailer in (b"\r\n", b"\n", b""):
                        return b"".join(chunks)
            chunk = await self.reader.readexactly(size)
            ending = await self.reader.readexactly(2)
            if ending != b"\r\n":
                raise HTTPError("invalid chunk terminator")
            chunks.append(chunk)
            total += size
            if total > 512 * 1024 * 1024:
                raise HTTPError("response body is too large")


@dataclass
class ReaderStats:
    reader_id: int
    bytes_received: int = 0
    segments: int = 0
    playlist_errors: int = 0
    segment_errors: int = 0
    sequence_gaps: int = 0
    ts_sync_errors: int = 0
    rate_bps: float = 0.0
    ready: bool = False


def parse_playlist(body):
    try:
        text = body.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ValueError("playlist is not UTF-8") from exc
    lines = [line.strip() for line in text.splitlines()]
    if not lines or lines[0] != "#EXTM3U":
        raise ValueError("invalid HLS playlist header")
    sequence = 0
    for line in lines:
        if line.startswith("#EXT-X-MEDIA-SEQUENCE:"):
            try:
                sequence = int(line.partition(":")[2])
            except ValueError as exc:
                raise ValueError("invalid media sequence") from exc
            if sequence < 0:
                raise ValueError("negative media sequence")
            break
    uris = [line for line in lines if line and not line.startswith("#")]
    return sequence, uris


def validate_ts(body):
    return bool(body) and len(body) % 188 == 0 and all(
        body[pos] == 0x47 for pos in range(0, len(body), 188))


def percentile(sorted_values, fraction):
    """Nearest-rank percentile over an ascending list: the smallest sample at
    or above `fraction` of the samples.  No interpolation, so every ratio
    reported here is one a reader actually measured."""
    if not sorted_values:
        return None
    rank = max(1, math.ceil(fraction * len(sorted_values)))
    return sorted_values[min(rank, len(sorted_values)) - 1]


def delivery_ratios(stats, reference):
    """What the readers counted, per reader: delivered rate over the reader's
    own measurement interval, divided by the reference rate.  The configured
    acceptance threshold is not part of any of these numbers."""
    if reference <= 0:
        ratios = sorted(item.rate_bps * 0.0 for item in stats)
    else:
        ratios = sorted(item.rate_bps / reference for item in stats)
    lowest = min(stats, key=lambda item: item.rate_bps) if stats else None
    return {
        "min": ratios[0] if ratios else 0.0,
        "min_reader_id": lowest.reader_id if lowest else -1,
        "p5": percentile(ratios, 0.05),
        "p50": percentile(ratios, 0.50),
        "p95": percentile(ratios, 0.95),
    }


async def read_playlist(client, playlist_url, stats):
    try:
        response = await client.request(playlist_url)
    except HTTPError:
        stats.playlist_errors += 1
        raise
    if response.status != 200:
        stats.playlist_errors += 1
        raise HTTPError(f"playlist returned HTTP {response.status}")
    declared = response.headers.get("content-length")
    if declared is not None:
        try:
            matches_length = int(declared) == len(response.body)
        except ValueError:
            matches_length = False
        if not matches_length:
            stats.playlist_errors += 1
            raise HTTPError("invalid playlist Content-Length")
    try:
        return parse_playlist(response.body)
    except ValueError as exc:
        stats.playlist_errors += 1
        raise HTTPError(str(exc)) from exc


async def fetch_segment(client, url, stats, semaphore, concurrency, warmup=False):
    async with semaphore:
        concurrency[0] += 1
        concurrency[1] = max(concurrency[1], concurrency[0])
        try:
            return await _fetch_segment(client, url, stats, warmup)
        finally:
            concurrency[0] -= 1


async def _fetch_segment(client, url, stats, warmup):
    try:
        response = await client.request(url)
        declared = response.headers.get("content-length")
        if response.status != 200:
            raise HTTPError(f"segment returned HTTP {response.status}")
        if declared is not None:
            try:
                matches_length = int(declared) == len(response.body)
            except ValueError:
                matches_length = False
            if not matches_length:
                raise HTTPError("invalid segment Content-Length")
        if not validate_ts(response.body):
            stats.ts_sync_errors += 1
            raise HTTPError("segment MPEG-TS sync or packet length error")
        if not warmup:
            stats.bytes_received += len(response.body)
            stats.segments += 1
        return True
    except (HTTPError, ValueError):
        if not warmup:
            stats.segment_errors += 1
        return False


async def run_reader(stats, playlist_url, client, ready_event, measurement_event,
                     stop_event, fatal_event, fatal_messages, poll_interval,
                     segment_semaphore, concurrency):
    try:
        sequence, uris = await read_playlist(client, playlist_url, stats)
        if not uris:
            stats.playlist_errors += 1
            raise MeasurementError(f"reader {stats.reader_id}: playlist has no segments")
        tail_sequence = sequence + len(uris) - 1
        tail_url = urljoin(playlist_url, uris[-1])
        if not await fetch_segment(client, tail_url, stats, segment_semaphore,
                                   concurrency, warmup=True):
            raise MeasurementError(f"reader {stats.reader_id}: failed to fetch warmup tail")
        stats.ready = True
        ready_event.set()
        last_sequence = tail_sequence
        await measurement_event.wait()
        try:
            current_sequence, current_uris = await read_playlist(client, playlist_url, stats)
            if current_uris:
                last_sequence = current_sequence + len(current_uris) - 1
        except HTTPError:
            pass
        while not stop_event.is_set():
            try:
                current_sequence, current_uris = await read_playlist(client, playlist_url, stats)
                if current_uris:
                    current_end = current_sequence + len(current_uris) - 1
                    if current_sequence > last_sequence + 1:
                        stats.sequence_gaps += current_sequence - last_sequence - 1
                    for segment_sequence in range(max(last_sequence + 1, current_sequence), current_end + 1):
                        uri = current_uris[segment_sequence - current_sequence]
                        await fetch_segment(client, urljoin(playlist_url, uri), stats,
                                            segment_semaphore, concurrency)
                    last_sequence = max(last_sequence, current_end)
            except HTTPError:
                pass
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=poll_interval)
            except asyncio.TimeoutError:
                pass
    except asyncio.CancelledError:
        raise
    except Exception as exc:
        fatal_messages.append(str(exc))
        fatal_event.set()
        stop_event.set()
        ready_event.set()
    finally:
        await client.close()


async def async_main(args):
    loop = asyncio.get_running_loop()
    ready_event = asyncio.Event()
    measurement_event = asyncio.Event()
    stop_event = asyncio.Event()
    fatal_event = asyncio.Event()
    fatal_messages = []
    stats = [ReaderStats(i) for i in range(args.readers)]
    clients = [HTTPClient(i) for i in range(args.readers)]
    segment_semaphore = asyncio.Semaphore(SEGMENT_CONCURRENCY_LIMIT)
    concurrency = [0, 0]
    tasks = [asyncio.create_task(run_reader(item, args.url, client, ready_event,
                                            measurement_event, stop_event,
                                            fatal_event, fatal_messages,
                                            args.poll_interval, segment_semaphore,
                                            concurrency))
             for item, client in zip(stats, clients)]

    def on_signal(sig):
        if sig == signal.SIGUSR1:
            measurement_event.set()
        else:
            stop_event.set()

    for sig in (signal.SIGUSR1, signal.SIGTERM):
        loop.add_signal_handler(sig, on_signal, sig)
    try:
        while True:
            ready_event.clear()
            if all(item.ready for item in stats) or fatal_event.is_set():
                break
            ready_wait = asyncio.create_task(ready_event.wait())
            fatal_wait = asyncio.create_task(fatal_event.wait())
            _, pending = await asyncio.wait({ready_wait, fatal_wait},
                                            return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
        if fatal_event.is_set():
            raise MeasurementError("; ".join(fatal_messages) or "reader warmup failed")
        with open(args.ready_file, "w", encoding="utf-8") as ready:
            ready.write("ready\n")
        start_wait = asyncio.create_task(measurement_event.wait())
        stop_wait = asyncio.create_task(stop_event.wait())
        _, pending = await asyncio.wait({start_wait, stop_wait},
                                        return_when=asyncio.FIRST_COMPLETED)
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)
        if stop_event.is_set() and not measurement_event.is_set():
            raise MeasurementError("received SIGTERM before measurement started")
        start_ns = time.monotonic_ns()
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=args.duration)
        except asyncio.TimeoutError:
            stop_event.set()
        elapsed = (time.monotonic_ns() - start_ns) / 1e9
        for item in stats:
            item.rate_bps = item.bytes_received * 8 / elapsed if elapsed > 0 else 0.0
        try:
            await asyncio.wait_for(asyncio.gather(*tasks), timeout=REQUEST_TIMEOUT + 1)
        except asyncio.TimeoutError:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
        reference = args.reference_bps
        if reference == 0:
            reference = stats[0].rate_bps
            if reference <= 0:
                raise MeasurementError("one-reader calibration delivered no measured bytes")
        quality_pass = all(
            item.bytes_received > 0 and item.rate_bps >= reference * args.min_delivery_ratio
            and item.playlist_errors == 0 and item.segment_errors == 0
            and item.sequence_gaps == 0 and item.ts_sync_errors == 0
            for item in stats)
        if fatal_event.is_set():
            raise MeasurementError("; ".join(fatal_messages) or "reader failed during measurement")
        observed = delivery_ratios(stats, reference)
        receiver_bytes = sum(item.bytes_received for item in stats)
        delivered_gbps = receiver_bytes * 8 / elapsed / 1e9 if elapsed > 0 else 0.0
        write_report(args.report, stats, elapsed, reference, args.min_delivery_ratio,
                     SEGMENT_CONCURRENCY_LIMIT, concurrency[1])
        print(f"quality_pass={'yes' if quality_pass else 'no'}")
        print(f"readers={args.readers}")
        print(f"duration_s={elapsed:.6f}")
        print(f"reference_bps={reference:.2f}")
        # The gate and the measurement are different numbers: the threshold is
        # what the run was asked to accept, the observed values are what the
        # readers counted.  min_delivery_ratio keeps its original meaning - the
        # configured threshold - so published history that read it is not
        # reinterpreted; anything that wants a measurement reads observed_*.
        print(f"delivery_ratio_threshold={args.min_delivery_ratio:.6f}")
        print(f"min_delivery_ratio={args.min_delivery_ratio:.6f}")
        print(f"observed_min_delivery_ratio={observed['min']:.6f}")
        print(f"observed_min_reader_id={observed['min_reader_id']}")
        print(f"observed_p5_delivery_ratio={observed['p5']:.6f}")
        print(f"observed_p50_delivery_ratio={observed['p50']:.6f}")
        print(f"observed_p95_delivery_ratio={observed['p95']:.6f}")
        print(f"observed_ratio_basis=reader-counted-bytes")
        print(f"receiver_bytes_total={receiver_bytes}")
        print(f"receiver_measurement_s={elapsed:.6f}")
        print(f"delivered_gbps={delivered_gbps:.6f}")
        print(f"total_bytes={receiver_bytes}")
        print(f"total_segments={sum(item.segments for item in stats)}")
        print(f"playlist_errors={sum(item.playlist_errors for item in stats)}")
        print(f"segment_errors={sum(item.segment_errors for item in stats)}")
        print(f"sequence_gaps={sum(item.sequence_gaps for item in stats)}")
        print(f"ts_sync_errors={sum(item.ts_sync_errors for item in stats)}")
        print(f"segment_concurrency_limit={SEGMENT_CONCURRENCY_LIMIT}")
        print(f"segment_concurrency_max={concurrency[1]}")
        return 0
    finally:
        stop_event.set()
        for task in tasks:
            if not task.done():
                task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        for sig in (signal.SIGUSR1, signal.SIGTERM):
            loop.remove_signal_handler(sig)


def write_report(path, stats, duration, reference, ratio, segment_limit, concurrency_max):
    with open(path, "w", newline="", encoding="utf-8") as report:
        writer = csv.writer(report)
        writer.writerow(("reader_id", "bytes_received", "successful_segments",
                         "playlist_errors", "segment_errors", "sequence_gaps",
                         "ts_sync_errors", "rate_bps", "reference_ratio",
                         "duration_s", "min_delivery_ratio",
                         "segment_concurrency_limit", "segment_concurrency_max"))
        for item in stats:
            comparison = item.rate_bps / reference if reference > 0 else 0.0
            writer.writerow((item.reader_id, item.bytes_received, item.segments,
                             item.playlist_errors, item.segment_errors,
                             item.sequence_gaps, item.ts_sync_errors,
                             f"{item.rate_bps:.2f}", f"{comparison:.6f}",
                             f"{duration:.6f}", f"{ratio:.6f}",
                             segment_limit, concurrency_max))


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--readers", type=int, required=True)
    parser.add_argument("--duration", type=float, required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--ready-file", required=True)
    parser.add_argument("--reference-bps", type=float, required=True)
    parser.add_argument("--min-delivery-ratio", type=float, default=0.95)
    parser.add_argument("--poll-interval", type=float, default=1.0)
    args = parser.parse_args(argv)
    parts = urlsplit(args.url)
    if parts.scheme != "http" or not parts.hostname or parts.username or parts.password:
        parser.error("--url must be an unauthenticated http:// URL")
    if not 1 <= args.readers <= 1000:
        parser.error("--readers must be between 1 and 1000")
    if args.duration <= 0 or args.poll_interval <= 0:
        parser.error("--duration and --poll-interval must be positive")
    if args.reference_bps < 0 or not 0 < args.min_delivery_ratio <= 1:
        parser.error("invalid reference bitrate or delivery ratio")
    if args.reference_bps == 0 and args.readers != 1:
        parser.error("reference-bps=0 requires exactly one reader")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        return asyncio.run(async_main(args))
    except (OSError, MeasurementError, ValueError) as exc:
        print(f"error={exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Rebuild the configured native recipe in isolation; retain raw observations."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import tempfile


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, text=True, **kwargs)


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nginx-tree', type=Path, default=root / '.build/nginx-1.30.5')
    parser.add_argument('--fixture', type=Path, default=root / '.build/fixtures/h264_aac.ts')
    parser.add_argument('--output', type=Path, default=root / '.build/core-critical-paths')
    parser.add_argument('--samples', type=int, default=2000)
    parser.add_argument('--cpu', type=int, help='pin both timing and resource processes to this CPU')
    parser.add_argument('--visitor-cpu', type=int,
                        help='pin the concurrent metrics visitor separately')
    args = parser.parse_args()
    if not 1000 <= args.samples <= 100000:
        parser.error('--samples must be 1000..100000')
    tree, fixture, output = args.nginx_tree.resolve(), args.fixture.resolve(), args.output.resolve()
    recipe = (tree / 'objs/Makefile').read_text()
    if '-fsanitize' in recipe:
        parser.error('configured nginx linker recipe contains sanitizer flags; use a non-sanitized tree')
    if not fixture.is_file() or not 0 < fixture.stat().st_size <= 4 * 1024 * 1024:
        parser.error('a real H.264/AAC TS fixture of at most 4 MiB is required (make ts-fixture)')
    output.mkdir(parents=True, exist_ok=True)
    cc = shlex.split(os.environ.get('CC', re.search(r'^CC\s*=\s*(.+)$', recipe, re.M)[1]))
    flags = '-O3 -g -std=gnu2x -Wall -Wextra -Wno-unused-parameter'
    source_hash = hashlib.sha256()
    for path in sorted((root / 'src').rglob('*')):
        if path.is_file() and path.suffix in ('.c', '.h'):
            source_hash.update(str(path.relative_to(root)).encode() + b'\0' + path.read_bytes())
    provenance = {
        'recipe': 'native-configured-nginx-isolated-rebuild-v1',
        'compiler': run(cc + ['--version'], capture_output=True).stdout,
        'flags': flags, 'nginx_tree': str(tree),
        'configured_recipe_sha256': hashlib.sha256(recipe.encode()).hexdigest(),
        'driver_sha256': hashlib.sha256((root / 'tests/bench/core_critical_paths.c').read_bytes()).hexdigest(),
        'production_sources_sha256': source_hash.hexdigest(),
        'configured_link_recipe': recipe.split('\t$(LINK) -o objs/nginx', 1)[1].split('\n\n', 1)[0],
        'resource_extra_driver_flags': '-DCORE_RESOURCE_COUNTS',
        'resource_link_wraps': ['malloc', 'calloc', 'realloc', 'free', 'memcpy', 'memmove'],
        'fixture': str(fixture), 'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
        'fixture_bytes': fixture.stat().st_size, 'kernel': platform.release(),
        'machine': platform.machine(), 'affinity': sorted(os.sched_getaffinity(0)),
        'requested_cpu': args.cpu, 'samples': args.samples, 'repeats': 3,
        'requested_visitor_cpu': args.visitor_cpu,
        'clock': 'CLOCK_MONOTONIC_RAW', 'warmup_operations_per_row': 32,
        'latency_scope': 'one full operation plus invariant checks; two clock reads per sample',
        'resource_scope': 'separate linker-wrapped allocator calls and out-of-line memcpy/memmove calls; inlined and shared-library internals unavailable',
        'resource_realloc_semantics': 'each successful realloc counts as one allocator call; frees count explicit non-null free calls',
        'byte_scope': 'logical payload bytes for refs/feed; encoded bytes for mux/demux/RTMP; reassembled payload bytes for IPC; compared label bytes for registry; record/report struct bytes for metadata',
        'comparison': 'same native recipe only; no nginx production capacity or blocking performance gate',
    }
    if args.visitor_cpu is not None and args.visitor_cpu not in os.sched_getaffinity(0):
        parser.error('--visitor-cpu is outside the allowed affinity mask')
    if args.cpu is not None:
        if args.cpu not in os.sched_getaffinity(0):
            parser.error('--cpu is outside the allowed affinity mask')
        os.sched_setaffinity(0, {args.cpu})
    provenance['effective_affinity'] = sorted(os.sched_getaffinity(0))
    provenance['effective_visitor_affinity'] = (
        [args.visitor_cpu] if args.visitor_cpu is not None
        else provenance['effective_affinity'])
    observations = []
    for mode in ('timing', 'resources'):
        with tempfile.TemporaryDirectory(prefix='core-critical-', dir=output) as scratch:
            scratch = Path(scratch)
            # Reuse nginx's generated dependency/link recipe, never its cached
            # objects: all objects and binaries live in the bounded temporary tree.
            transformed = re.sub(r'\bobjs/[^\s\\]+\.o\b', lambda m: str(scratch / m[0]), recipe)
            for obj in re.findall(r'\S+\.o\b', transformed):
                path = Path(obj)
                if path.is_absolute() and path.is_relative_to(scratch):
                    path.parent.mkdir(parents=True, exist_ok=True)
            binary = scratch / 'driver'
            transformed = transformed.replace('objs/nginx', str(binary))
            driver = scratch / 'driver.o'
            nginx_obj = str(scratch / 'objs/src/core/nginx.o')
            # Both the link prerequisites and link command need the driver.
            transformed = transformed.replace(nginx_obj, nginx_obj + ' ' + str(driver), 2)
            extra = ' -DCORE_RESOURCE_COUNTS' if mode == 'resources' else ''
            wrap = ' '.join('-Wl,--wrap=' + symbol for symbol in ('malloc', 'calloc', 'realloc', 'free', 'memcpy', 'memmove')) if mode == 'resources' else ''
            transformed = transformed.replace('-o ' + str(binary), wrap + ' -o ' + str(binary))
            transformed += (f'\n{driver}: {root}/tests/bench/core_critical_paths.c\n'
                            f'\t$(CC) $(CFLAGS) $(ALL_INCS){extra} -c $< -o $@\n'
                            f'\n{nginx_obj}: override CFLAGS += -Dmain=ngx_media_bench_unused_main\n')
            makefile = scratch / 'Makefile'
            makefile.write_text(transformed)
            with (output / f'{mode}-build.log').open('w') as log:
                run(['make', '-f', str(makefile), '-j' + str(min(8, os.cpu_count() or 1)),
                     'CC=' + shlex.join(cc), 'CFLAGS=' + flags, str(binary)], cwd=tree, stdout=log, stderr=subprocess.STDOUT)
            argv = [str(binary), str(fixture), str(args.samples)]
            if args.visitor_cpu is not None:
                argv.append(str(args.visitor_cpu))
            result = run(argv, capture_output=True)
            (output / f'{mode}-stderr.log').write_text(result.stderr)
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            if len(rows) != 33:
                raise RuntimeError(f'{mode}: expected 11 operations x 3 repeats, got {len(rows)}')
            for row in rows:
                row['provenance'] = provenance
            observations.extend(rows)
    (output / 'raw.json').write_text(json.dumps(observations, indent=2) + '\n')
    (output / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    keys = list(observations[0])
    with (output / 'raw.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        for row in observations:
            writer.writerow({**row, 'provenance': json.dumps(provenance, sort_keys=True)})
    print(f'{len(observations)} verified observations: {output}/raw.json and raw.csv')


if __name__ == '__main__':
    main()

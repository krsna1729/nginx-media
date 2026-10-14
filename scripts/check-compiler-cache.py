#!/usr/bin/env python3
"""Exercise real nginx builds across a compiler flip and an unchanged cache hit."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--first", required=True)
    parser.add_argument("--second", required=True)
    parser.add_argument("--output", default=".build/compiler-cache-check")
    args = parser.parse_args()
    if shlex.split(args.first) == shlex.split(args.second):
        raise RuntimeError("choose two distinct compiler commands")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    snapshots = []
    for index, compiler in enumerate((args.first, args.second, args.second), start=1):
        environment = os.environ.copy()
        environment.update(CC=compiler, BUILD_DIR=str(output),
                           NGINX_PREFIX=str(output / "nginx-install"))
        with (output / f"build-{index}.log").open("wb") as log:
            process = subprocess.run([str(ROOT / "scripts/build-nginx.sh")], cwd=ROOT,
                                     env=environment, stdout=log, stderr=subprocess.STDOUT)
        if process.returncode:
            print((output / f"build-{index}.log").read_text(errors="replace"), end="")
            raise RuntimeError(f"compiler cache build {index} failed; raw log retained")
        database = json.loads((output / "compile_commands.json").read_text())
        prefix = shlex.split(compiler)
        if not database or not all(record["arguments"][:len(prefix)] == prefix for record in database):
            raise RuntimeError("compile database contains stale or missing compiler argv")
        snapshot = {"compiler": compiler, "identity": json.loads((output / ".compiler").read_text()),
                    "objects": {record["output"]: Path(record["output"]).stat().st_mtime_ns
                                for record in database}}
        (output / f"database-{index}.json").write_text(json.dumps(database, indent=2) + "\n")
        snapshots.append(snapshot)
    (output / "snapshots.json").write_text(json.dumps(snapshots, indent=2) + "\n")
    first, flipped, cached = snapshots
    if first["identity"] == flipped["identity"]:
        raise RuntimeError("compiler identity failed to change")
    shared = set(first["objects"]) & set(flipped["objects"])
    if not shared or any(first["objects"][obj] == flipped["objects"][obj] for obj in shared):
        raise RuntimeError("compiler flip reused stale native objects")
    if flipped["objects"] != cached["objects"]:
        raise RuntimeError("unchanged compiler rebuilt native objects instead of reusing them")
    print(f"Real compiler flip rebuilt {len(shared)} native objects; unchanged cache hit preserved them. "
          f"Raw evidence: {output}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build a native SRT receiver and publish content-bound build evidence."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess


def digest(path):
    with open(path, "rb") as source:
        value = hashlib.sha256()
        for block in iter(lambda: source.read(1 << 20), b""):
            value.update(block)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--pkg", default="srt")
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cflags", default="-O2 -g -Wall -Wextra -Werror -std=c2x")
    args = parser.parse_args()
    source = Path(args.source).resolve(strict=True)
    binary = Path(args.binary).resolve()
    metadata = Path(str(binary) + ".provenance.json")
    # Failed builds cannot leave a previous successful build's evidence.
    metadata.unlink(missing_ok=True)
    compiler = shlex.split(args.compiler)
    if not compiler:
        parser.error("--compiler must name a compiler")
    compiler[0] = shutil.which(compiler[0]) or compiler[0]
    compiler_version = subprocess.run(compiler + ["--version"], check=True, timeout=60,
                                      capture_output=True, text=True).stdout.strip()
    package = {}
    for flag, key in (("--cflags", "cflags"), ("--libs", "libs"),
                      ("--modversion", "version"), ("--variable=pcfiledir", "pcfiledir")):
        package[key] = subprocess.run(["pkg-config", flag, args.pkg], check=True, timeout=60,
                                      capture_output=True, text=True).stdout.strip()
    cflags = shlex.split(args.cflags) + shlex.split(package["cflags"])
    ldflags = shlex.split(package["libs"])
    command = compiler + cflags + [str(source), "-o", str(binary)] + ldflags
    source_sha256 = digest(source)
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True, timeout=600)
    if digest(source) != source_sha256:
        raise SystemExit("receiver source changed during compilation")
    evidence = {
        "schema": "nginx-media.binary-build/1",
        "sha256": digest(binary),
        "compiler": compiler_version,
        "compiler_command": compiler,
        "cflags": cflags,
        "ldflags": ldflags,
        "compile_command": command,
        "source": {"path": str(source), "sha256": source_sha256},
        "package": dict(package, name=args.pkg),
    }
    temporary = Path(str(metadata) + ".tmp")
    temporary.write_text(json.dumps(evidence, indent=1) + "\n", encoding="utf-8")
    temporary.replace(metadata)
    print(f"native receiver build evidence: {metadata}")


if __name__ == "__main__":
    main()

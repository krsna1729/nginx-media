#!/usr/bin/env python3
"""Compile and run dependency-isolated core/parser suites with MemorySanitizer.

No OpenSSL, libsrt, c-ares, nginx binary or network-client implementation is linked.
System libc remains uninstrumented and relies on MSan's supported interceptors;
this is not evidence for the whole module or its third-party libraries.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ["src/core/ngx_media_buffer.c", "src/core/ngx_media_frame.c",
           "src/core/ngx_media_track.c", "src/core/ngx_media_timeline.c",
           "src/codec/ngx_media_nal.c", "src/codec/ngx_media_aac.c",
           "src/mpegts/ngx_media_ts_crc.c", "src/mpegts/ngx_media_ts_demux.c",
           "src/srt/ngx_media_srt_streamid.c", "src/rtmp/ngx_media_rtmp_wire.c",
           "tests/unit/shim/ngx_shim.c"]
TESTS = ["buffer", "frame", "track", "timeline", "codec", "ts_demux",
         "srt_streamid", "rtmp_wire"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--output", default=".build/msan")
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(args.compiler)
    version = subprocess.check_output(compiler + ["--version"], stderr=subprocess.STDOUT)
    if b"clang" not in version.lower():
        raise RuntimeError("MemorySanitizer requires Clang, not a fallback compiler")
    (output / "compiler.txt").write_bytes(version)
    flags = ["-std=gnu2x", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=memory", "-fsanitize-memory-track-origins=2",
             "-fno-omit-frame-pointer", "-fno-optimize-sibling-calls", "-fPIE"]
    includes = ["-DNGX_MEDIA_UNIT_TEST", "-Itests/unit", "-Itests/unit/shim"]
    includes += ["-I" + str(path.relative_to(ROOT)) for path in sorted((ROOT / "src").iterdir())
                 if path.is_dir()]
    records, commands = [], []

    def execute(command, name, environment=None, cwd=ROOT):
        commands.append({"directory": str(cwd), "arguments": command})
        # Save before execution, including the failed invocation's raw log.
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        with (output / (name + ".log")).open("wb") as log:
            process = subprocess.run(command, cwd=cwd, env=environment,
                                     stdout=log, stderr=subprocess.STDOUT)
        print((output / (name + ".log")).read_text(errors="replace"), end="", flush=True)
        if process.returncode:
            raise RuntimeError(f"{name} failed ({process.returncode}); see {output / (name + '.log')}")

    objects = []
    for source in SOURCES + [f"tests/unit/test_{test}.c" for test in TESTS]:
        obj = output / "objects" / (source + ".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        command = compiler + flags + includes + ["-c", source, "-o", str(obj)]
        records.append({"directory": str(ROOT), "file": str(ROOT / source),
                        "output": str(obj), "arguments": command})
        (output / "compile_commands.json").write_text(json.dumps(records, indent=2) + "\n")
        execute(command, Path(source).stem + ".compile")
        if source in SOURCES:
            objects.append(str(obj))
    manifest = {"sources": SOURCES, "suites": TESTS, "instrumentation": flags,
                "excluded_dependencies": ["OpenSSL", "libsrt", "c-ares", "nginx"],
                "limitations": ["libc is uninstrumented; MSan interceptors cover supported calls",
                                "unit shim is instrumented; real HTTP/SRT/TLS paths are not covered",
                                "no ignorelist, unpoison calls, PIE retries or diagnostic suppression"]}
    (output / "scope.json").write_text(json.dumps(manifest, indent=2) + "\n")
    binaries = []
    for test in TESTS:
        binary = output / ("test_" + test)
        test_object = output / "objects" / f"tests/unit/test_{test}.c.o"
        execute(compiler + flags + ["-pie", "-o", str(binary), str(test_object)] + objects,
                "test_" + test + ".link")
        execute(["ldd", str(binary)], "test_" + test + ".dependencies")
        dependencies = (output / ("test_" + test + ".dependencies.log")).read_text()
        if any(name in dependencies for name in ("libcrypto", "libssl", "libsrt", "libcares")):
            raise RuntimeError("uninstrumented third-party dependency entered isolated MSan lane")
        binaries.append(binary)
    environment = os.environ.copy()
    environment["MSAN_OPTIONS"] = "halt_on_error=1:exit_code=86:print_stats=1"
    for binary in binaries:
        execute([str(binary)], binary.name + ".run", environment, output)
    print(f"Isolated MSan suites completed; raw evidence: {output}")


if __name__ == "__main__":
    main()

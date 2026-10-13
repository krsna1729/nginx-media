#!/usr/bin/env python3
"""Build/smoke O2 baseline and opt-in FORTIFY3/stack-clash/x86-CET candidates.

Save commands, feature macros, ELF metadata and disassembly. This is correctness
and emitted-code evidence, not a throughput benchmark or whole-process CET claim.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output", default=".build/hardening")
    args = parser.parse_args()
    if platform.machine() not in ("x86_64", "AMD64"):
        raise RuntimeError("this CET comparison requires x86-64; do not treat another architecture as evidence")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    commands, summary = [], []
    compiler = shlex.split(args.compiler)
    for lane, level, hardening in (
            ("baseline", "2", "-fstack-protector-strong"),
            ("candidate", "3", "-fstack-protector-strong -fstack-clash-protection -fcf-protection=full")):
        directory = output / lane
        directory.mkdir(parents=True, exist_ok=True)
        environment = os.environ.copy()
        environment.update(CC=args.compiler, BUILD_DIR=str(directory),
                           NGINX_PREFIX=str(directory / "nginx-install"), NGINX_CC_OPT="",
                           NGINX_FORTIFY=level, NGINX_HARDENING_OPT=hardening)

        def execute(command, name, input=None):
            commands.append({"lane": lane, "directory": str(ROOT), "arguments": command,
                             "configuration": {key: environment[key] for key in (
                                 "CC", "BUILD_DIR", "NGINX_PREFIX", "NGINX_CC_OPT",
                                 "NGINX_FORTIFY", "NGINX_HARDENING_OPT")}})
            (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
            process = subprocess.run(command, cwd=ROOT, env=environment, input=input,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (directory / (name + ".log")).write_bytes(process.stdout)
            if name == "smoke":
                smoke_log = ROOT / ".build/smoke/logs/error.log"
                if smoke_log.exists():
                    (directory / "smoke-nginx.log").write_bytes(smoke_log.read_bytes())
            if process.returncode:
                print(process.stdout.decode(errors="replace"), end="")
                raise RuntimeError(f"{lane}/{name} failed ({process.returncode}); raw log retained")
            return process.stdout.decode(errors="replace")

        execute(compiler + ["--version"], "compiler")
        macros = execute(compiler + ["-O2", "-D_FORTIFY_SOURCE=" + level] + shlex.split(hardening)
                         + ["-Werror", "-dM", "-E", "-x", "c", "-"],
                         "feature-macros", b"#include <features.h>\n")
        if f"#define __USE_FORTIFY_LEVEL {level}\n" not in macros:
            raise RuntimeError(f"libc/compiler did not activate requested FORTIFY level {level}")
        execute([str(ROOT / "scripts/build-nginx.sh")], "build-driver")
        binary = directory / "nginx-install/sbin/nginx"
        environment["NGINX_BIN"] = str(binary)
        execute([str(ROOT / "tests/integration/smoke.sh")], "smoke")
        elf = execute(["readelf", "-n", "-l", "-d", str(binary)], "readelf")
        assembly = execute(["objdump", "-d", str(binary)], "objdump")
        symbols = execute(["readelf", "-Ws", str(binary)], "symbols")
        summary.append({"lane": lane, "fortify_level": int(level), "hardening": hardening,
                        "smoke": "passed", "endbr64_instructions": assembly.count("endbr64"),
                        "stack_chk_fail_reference": "__stack_chk_fail" in symbols,
                        "checked_libc_references": sorted({line.split()[-1] for line in symbols.splitlines()
                                                          if "_chk@" in line}),
                        "elf_mentions_ibt": "IBT" in elf, "elf_mentions_shstk": "SHSTK" in elf})
    report = {"lanes": summary,
              "limitations": ["stack-clash probes require human inspection of saved disassembly",
                               "CET flags/instructions do not prove enforcement across shared libraries",
                               "smoke is not full compatibility or performance qualification",
                               "O3, LTO and PGO are not enabled or measured"]}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

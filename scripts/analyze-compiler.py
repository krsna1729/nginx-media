#!/usr/bin/env python3
"""Analyze executed production compile commands without suppressing diagnostics.

Triage file: [{"lane": "gcc|clang", "fingerprint": "sha256 from summary.json", "reason": "evidence",
              "owner": "reviewer", "expires": "YYYY-MM-DD"}].
An empty triage means every finding fails. Raw diagnostics are always retained.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shlex
import subprocess
import sys
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parent.parent


def analyzer_arguments(arguments):
    # Replace only the compiler/launcher and build-output/instrumentation flags;
    # retain the actual include paths, defines, dialect and warning switches.
    start = next(i for i, word in enumerate(arguments) if word.startswith("-"))
    result = []
    iterator = iter(arguments[start:])
    for word in iterator:
        if word in ("-o", "-MF", "-MT", "-MQ"):
            next(iterator)
        elif word in ("-c", "-MMD", "-MD", "-MP", "--coverage", "-Werror"):
            continue
        elif word.startswith(("-Werror=", "-fsanitize", "-fno-sanitize", "-flto",
                              "-fprofile", "-fcoverage", "-fdiagnostics-format=")):
            continue
        else:
            result.append(word)
    return result


def finding(lane, directory, file, line, rule, message):
    file = (directory / file).resolve()
    name = str(file.relative_to(ROOT)) if file.is_relative_to(ROOT) else str(file)
    item = {"lane": lane, "file": name, "line": line,
            "rule": rule, "message": message}
    item["fingerprint"] = hashlib.sha256(json.dumps(item, sort_keys=True).encode()).hexdigest()
    return item


def sarif_findings(report, directory, source):
    for run in report["runs"]:
        for result in run.get("results", []):
            if result.get("level") == "note":
                continue
            locations = result.get("locations", [])
            physical = locations[0].get("physicalLocation", {}) if locations else {}
            uri = physical.get("artifactLocation", {}).get("uri", source)
            parsed = urlparse(uri)
            file = unquote(parsed.path) if parsed.scheme == "file" else unquote(uri)
            yield finding("gcc", directory, file,
                          physical.get("region", {}).get("startLine", 0),
                          result.get("ruleId", result.get("level", "warning")),
                          result["message"]["text"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lane", choices=("gcc", "clang"))
    parser.add_argument("--database", default=".build/compile_commands.json")
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--output")
    parser.add_argument("--triage", default="scripts/analyzer-triage.json")
    args = parser.parse_args()
    output = Path(args.output or f".build/analysis/{args.lane}").resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(args.compiler)
    version = subprocess.check_output(compiler + ["--version"], stderr=subprocess.STDOUT)
    (output / "compiler.txt").write_bytes(version)
    records = json.loads(Path(args.database).read_text())
    records = [record for record in records
               if Path(record["file"]).resolve().is_relative_to(ROOT / "src")]
    if not records:
        raise RuntimeError("database contains no production source commands")
    triage = json.loads(Path(args.triage).read_text())
    approved = {}
    for entry in triage:
        if entry["lane"] not in ("gcc", "clang"):
            raise RuntimeError("triage lane must be gcc or clang")
        if entry["lane"] != args.lane:
            continue
        if not entry["reason"].strip() or not entry["owner"].strip():
            raise RuntimeError("triage requires evidence and an accountable owner")
        if datetime.date.fromisoformat(entry["expires"]) < datetime.date.today():
            raise RuntimeError(f"expired triage: {entry['fingerprint']}")
        if entry["fingerprint"] in approved:
            raise RuntimeError("duplicate triage fingerprint")
        approved[entry["fingerprint"]] = entry
    findings, failures, commands = [], [], []
    for index, record in enumerate(records):
        directory = Path(record["directory"])
        stem = output / f"{index:04d}-{Path(record['file']).stem}"
        arguments = analyzer_arguments(record["arguments"])
        if args.lane == "gcc":
            command = compiler + arguments + ["-fanalyzer", "-fdiagnostics-format=sarif-stderr",
                                               "-c", "-o", str(stem.with_suffix(".o"))]
        else:
            command = compiler + arguments + ["--analyze", "-Xanalyzer",
                                               "-analyzer-output=plist", "-o",
                                               str(stem.with_suffix(".plist"))]
        # Do not accidentally consume an earlier run's successful report.
        stem.with_suffix(".plist").unlink(missing_ok=True)
        process = subprocess.run(command, cwd=directory, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        stem.with_suffix(".stdout").write_bytes(process.stdout)
        stem.with_suffix(".stderr").write_bytes(process.stderr)
        commands.append({"directory": str(directory), "arguments": command,
                         "file": record["file"], "returncode": process.returncode})
        print(f"== {args.lane}: {record['file']}", flush=True)
        sys.stdout.buffer.write(process.stdout)
        sys.stderr.buffer.write(process.stderr)
        if process.returncode:
            failures.append({"file": record["file"], "returncode": process.returncode})
        if args.lane == "gcc":
            try:
                findings.extend(sarif_findings(json.loads(process.stderr),
                                              directory, record["file"]))
            except (json.JSONDecodeError, KeyError, TypeError):
                failures.append({"file": record["file"], "error": "invalid GCC SARIF diagnostics"})
        else:
            report = stem.with_suffix(".plist")
            if not report.exists():
                failures.append({"file": record["file"], "error": "Clang emitted no plist report"})
                continue
            with report.open("rb") as stream:
                data = plistlib.load(stream)
            for diagnostic in data["diagnostics"]:
                location = diagnostic["location"]
                findings.append(finding(args.lane, directory,
                    data["files"][location["file"]], location["line"],
                    diagnostic["check_name"], diagnostic["description"]))
            # Frontend warnings are not represented in the analyzer plist.
            for line in process.stderr.decode(errors="replace").splitlines():
                match = re.match(r"(.+?):(\d+):\d+: (warning|error): (.+)", line)
                if match:
                    item = finding(args.lane, directory, match[1], int(match[2]),
                                   match[3], re.sub(r" \[[^\]]+\]$", "", match[4]))
                    if not any(existing["file"] == item["file"]
                               and existing["line"] == item["line"]
                               and existing["message"] == item["message"] for existing in findings):
                        findings.append(item)
    used = set()
    for item in findings:
        review = approved.get(item["fingerprint"])
        item["status"] = "triaged" if review else "untriaged"
        if review:
            item["review"] = review
            used.add(item["fingerprint"])
    stale = sorted(set(approved) - used)
    summary = {"lane": args.lane, "compiler": version.decode(errors="replace"),
               "database": str(Path(args.database).resolve()), "translation_units": len(records),
               "findings": findings, "failed_commands": failures, "stale_triage": stale,
               "policy": "exact reviewed fingerprints only; no diagnostic suppression"}
    (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    untriaged = sum(item["status"] == "untriaged" for item in findings)
    print(f"{len(records)} units; {untriaged} untriaged findings; {len(failures)} failures; "
          f"{len(stale)} stale reviews; raw reports: {output}")
    return int(bool(untriaged or failures or stale))


if __name__ == "__main__":
    sys.exit(main())

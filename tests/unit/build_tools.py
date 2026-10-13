#!/usr/bin/env python3
"""Unit-build invalidation, executed-command provenance and gcov reporting."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def save_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def config(args):
    compiler = shlex.split(args.compiler)
    identity = subprocess.check_output(compiler + ["--version"], stderr=subprocess.STDOUT)
    digest = hashlib.sha256(identity)
    # Include resolved binaries, not only the name/version: an in-place compiler
    # replacement and compiler wrappers must invalidate the objects as well.
    for word in compiler:
        executable = shutil.which(word)
        if executable:
            binary = Path(executable).resolve()
            digest.update(str(binary).encode())
            digest.update(binary.read_bytes())
    for file in (Path(__file__), Path("Makefile")):
        digest.update(file.read_bytes())
    digest.update(json.dumps(args.command).encode())
    value = {"signature": digest.hexdigest(), "compiler": compiler,
             "version": identity.decode(errors="replace"), "arguments": args.command}
    path = Path(args.path)
    if not path.exists() or json.loads(path.read_text()) != value:
        save_json(path, value)


def compile_command(args):
    command = args.command
    source = command[command.index("-c") + 1]
    target = command[command.index("-o") + 1]
    Path(target).parent.mkdir(parents=True, exist_ok=True)
    record = {"directory": str(Path.cwd()), "file": str(Path(source).resolve()),
              "output": str(Path(target).resolve()), "arguments": command}
    # Record the exact argv being executed, even if the compiler rejects it.
    save_json(args.path, record)
    subprocess.run(command, check=True)


def database(args):
    records = []
    for directory in args.directories:
        for record in sorted(Path(directory).rglob("*.command.json")):
            records.append(json.loads(record.read_text()))
    save_json(args.path, records)


def run_suites(binaries, directory, logs=None, poison=None):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    failures = []
    for binary in binaries:
        binary = Path(binary).resolve()
        environment = os.environ.copy()
        if poison:
            environment["NGX_MEDIA_TEST_POISON"] = "1"
        elif poison is False:
            environment.pop("NGX_MEDIA_TEST_POISON", None)
        log = None
        if logs:
            logs = Path(logs)
            logs.mkdir(parents=True, exist_ok=True)
            log = (logs / (binary.name + (".poison" if poison else "") + ".log")).open("w")
        try:
            process = subprocess.Popen([str(binary)], cwd=directory, env=environment,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, errors="replace")
            for line in process.stdout:
                print(line, end="", flush=True)
                if log:
                    log.write(line)
            if process.wait():
                failures.append(binary.name)
        finally:
            if log:
                log.close()
    return failures


def counts(lines, branches):
    return {"lines_hit": sum(value > 0 for value in lines.values()),
            "lines_total": len(lines),
            "branches_hit": sum(value > 0 for value in branches.values()),
            "branches_total": len(branches)}


def coverage(args):
    root = Path(args.path).resolve()
    objects = Path(args.objects).resolve()
    source_root = Path("../../src").resolve()
    repository = source_root.parent
    root.mkdir(parents=True, exist_ok=True)
    # Every baseline starts at zero; keep the resulting .gcda/.gcno and JSON.
    for data in objects.rglob("*.gcda"):
        data.unlink()
    raw = root / "raw"
    if raw.exists():
        shutil.rmtree(raw)
    failures = run_suites(args.binaries, root / "run", root / "logs", poison=False)
    failures += run_suites(args.binaries, root / "run", root / "logs", poison=True)
    files = {}
    records = json.loads((root / "compile_commands.json").read_text())
    linked = {str(Path(record["file"]).relative_to(repository)) for record in records
              if Path(record["file"]).is_relative_to(source_root)}
    for notes in sorted(Path(record["output"]).with_suffix(".gcno") for record in records):
        if not notes.exists():
            raise RuntimeError(f"missing raw coverage notes: {notes}")
        destination = raw / notes.relative_to(objects).with_suffix("")
        destination.mkdir(parents=True, exist_ok=True)
        command = shlex.split(args.gcov) + ["-b", "-c", "-j", str(notes)]
        result = subprocess.run(command, cwd=destination, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, check=True)
        (destination / "gcov.log").write_text(result.stdout)
        variant = notes.relative_to(objects).parts[0]
        for compressed in destination.glob("*.gcov.json.gz"):
            with gzip.open(compressed, "rt") as stream:
                report = json.load(stream)
            compilation_directory = Path(report["current_working_directory"])
            for entry in report["files"]:
                source = (compilation_directory / entry["file"]).resolve()
                if source.suffix != ".c" or not source.is_relative_to(source_root):
                    continue
                name = str(source.relative_to(repository))
                item = files.setdefault(name, {"lines": {}, "branches": {}, "variants": {}})
                lane = item["variants"].setdefault(variant, {"lines": {}, "branches": {}})
                for line in entry["lines"]:
                    number = line["line_number"]
                    for bucket in (item, lane):
                        bucket["lines"][number] = bucket["lines"].get(number, 0) + line["count"]
                    for index, branch in enumerate(line.get("branches", [])):
                        key = (number, index)
                        for bucket in (item, lane):
                            bucket["branches"][key] = bucket["branches"].get(key, 0) + branch["count"]
    if not files:
        raise RuntimeError("gcov produced no production-source coverage")
    if set(files) != linked:
        raise RuntimeError(f"gcov production inventory disagrees with compile database: "
                           f"{sorted(set(files) ^ linked)}")
    inventory = sorted(str(source.relative_to(repository)) for source in source_root.rglob("*.c"))
    excluded = sorted(set(inventory) - set(files))
    rows = []
    text = ["Production file\tLines hit/total\tBranches hit/total"]
    total = {key: 0 for key in ("lines_hit", "lines_total", "branches_hit", "branches_total")}
    for name, item in sorted(files.items()):
        row = {"file": name, **counts(item["lines"], item["branches"]),
               "variants": {name: counts(value["lines"], value["branches"])
                            for name, value in sorted(item["variants"].items())}}
        rows.append(row)
        text.append(f"{name}\t{row['lines_hit']}/{row['lines_total']}\t"
                    f"{row['branches_hit']}/{row['branches_total']}")
        for key in total:
            total[key] += row[key]
    text.append(f"TOTAL\t{total['lines_hit']}/{total['lines_total']}\t"
                f"{total['branches_hit']}/{total['branches_total']}")
    text.append("\nExcluded production .c files (not linked into standalone unit suites):")
    text.extend(excluded)
    summary = {"files": rows, "total": total, "excluded_production_files": excluded,
               "excluded_reason": "not linked into standalone unit suites",
               "failed_suites": failures,
               "branch_identity": "source line and gcov branch index; union across common/http builds",
               "runs": ["normal allocator", "poison allocator"], "coverage_floor": None}
    save_json(root / "coverage.json", summary)
    (root / "coverage.txt").write_text("\n".join(text) + "\n")
    (root / "excluded-production.txt").write_text("\n".join(excluded) + "\n")
    print("\n".join(text))
    if failures:
        raise RuntimeError("unit suite failures: " + ", ".join(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    stamp = commands.add_parser("config")
    stamp.add_argument("path")
    stamp.add_argument("--compiler", required=True)
    stamp.add_argument("command", nargs=argparse.REMAINDER)
    compile_parser = commands.add_parser("compile")
    compile_parser.add_argument("path")
    compile_parser.add_argument("command", nargs=argparse.REMAINDER)
    db = commands.add_parser("database")
    db.add_argument("path")
    db.add_argument("directories", nargs="+")
    run = commands.add_parser("run")
    run.add_argument("path")
    run.add_argument("binaries", nargs="+")
    cov = commands.add_parser("coverage")
    cov.add_argument("path")
    cov.add_argument("--objects", required=True)
    cov.add_argument("--gcov", required=True)
    cov.add_argument("binaries", nargs="+")
    args = parser.parse_args()
    if hasattr(args, "command") and args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if args.action == "run":
        if run_suites(args.binaries, args.path):
            sys.exit(1)
    else:
        {"config": config, "compile": compile_command, "database": database,
         "coverage": coverage}[args.action](args)


if __name__ == "__main__":
    main()

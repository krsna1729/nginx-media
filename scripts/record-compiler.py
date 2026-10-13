#!/usr/bin/env python3
"""Capture native compiler argv, aggregate its database, and fingerprint CC."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def save_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
        temporary = Path(stream.name)
    temporary.replace(path)


def identity(compiler):
    version = subprocess.check_output(compiler + ["--version"], stderr=subprocess.STDOUT)
    digest = hashlib.sha256(version)
    resolved = []
    for word in compiler:
        executable = shutil.which(word)
        if executable:
            binary = Path(executable).resolve()
            digest.update(str(binary).encode())
            with binary.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            resolved.append(str(binary))
    digest.update(json.dumps(compiler).encode())
    return {"arguments": compiler, "resolved": resolved,
            "version": version.decode(errors="replace"), "sha256": digest.hexdigest()}


def compile_record(command, directory):
    if "-c" not in command or "-o" not in command:
        return None
    sources = [word for word in command if word.endswith(".c") and not word.startswith("-")]
    source = sources[-1] if sources else None
    if source is None:
        return None
    output = command[command.index("-o") + 1]
    return {"directory": str(directory), "file": str((directory / source).resolve()),
            "output": str((directory / output).resolve()), "arguments": command}


def main():
    compiler = shlex.split(os.environ.get("MEDIA_REAL_CC", "cc"))
    if sys.argv[1:2] == ["identity"]:
        print(json.dumps(identity(compiler), sort_keys=True))
        return 0
    if sys.argv[1:2] == ["database"]:
        directory, target = map(Path, sys.argv[2:4])
        records = [json.loads(path.read_text()) for path in sorted(directory.glob("*.json"))]
        # Configure probes are executed too, but are not native build units.
        records = [record for record in records
                   if Path(record["file"]).name != "autotest.c"
                   and record.get("returncode") == 0
                   and Path(record["file"]).exists() and Path(record["output"]).exists()]
        if not records:
            raise RuntimeError("no executed native compilation records; rebuild with the recorder")
        save_json(target, records)
        return 0
    command = compiler + sys.argv[1:]
    directory = Path.cwd()
    record = compile_record(command, directory)
    if record:
        key = hashlib.sha256(record["output"].encode()).hexdigest()
        path = Path(os.environ["MEDIA_COMMAND_DIR"]) / (key + ".json")
        record["returncode"] = None
        save_json(path, record)
    status = subprocess.run(command).returncode
    if record:
        record["returncode"] = status
        save_json(path, record)
    return status


if __name__ == "__main__":
    sys.exit(main())

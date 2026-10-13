#!/usr/bin/env python3
"""Small behavior checks for compiler record and analyzer argv handling."""
import importlib.util
import json
from pathlib import Path
import tempfile
from concurrent.futures import ThreadPoolExecutor

SCRIPTS = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), SCRIPTS / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    recorder = load("record-compiler")
    analyzer = load("analyze-compiler")
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        command = ["ccache", "clang-21", "-DVALUE=not-a-source.c", "-Iinclude path",
                   "-std=gnu2x", "-Werror", "-c", "actual source.c", "-o", "out file.o"]
        record = recorder.compile_record(command, directory)
        assert record["file"] == str(directory / "actual source.c")
        assert record["output"] == str(directory / "out file.o")
        assert recorder.compile_record(["cc", "input.c", "-o", "binary"], directory) is None
        assert recorder.compile_record(["cc", "-c", "input.c"], directory) is None
        assert recorder.compile_record(["cc", "-c", "input.s", "-o", "out.o"], directory) is None
        # Simultaneous capture writes to one output must never expose partial JSON.
        path = directory / "record.json"
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda _: recorder.save_json(path, record), range(20)))
        assert json.loads(path.read_text()) == record
        arguments = command + ["-MMD", "-MP", "-MF", "dependencies.d", "-MT", "out.o",
                               "--coverage", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                               "-Wall", "-Wextra", "-Werror=unused", "-DNGX_MEDIA_UNIT_TEST"]
        assert analyzer.analyzer_arguments(arguments) == [
            "-DVALUE=not-a-source.c", "-Iinclude path", "-std=gnu2x", "actual source.c",
            "-Wall", "-Wextra", "-DNGX_MEDIA_UNIT_TEST"]
        finding = analyzer.finding("gcc", directory, "actual source.c", 7, "-Wanalyzer-leak", "leak")
        assert finding["fingerprint"] != analyzer.finding(
            "gcc", directory, "actual source.c", 8, "-Wanalyzer-leak", "leak")["fingerprint"]
        # SARIF locations are URIs; spaces must not corrupt diagnostic identity.
        report = {"runs": [{"results": [
            {"ruleId": "-Wanalyzer-leak", "message": {"text": "leak"},
             "locations": [{"physicalLocation": {
                 "artifactLocation": {"uri": (directory / "actual source.c").as_uri()},
                 "region": {"startLine": 7}}}]},
            {"level": "note", "message": {"text": "not a finding"}}
        ]}, {}]}
        parsed = list(analyzer.sarif_findings(report, directory, "fallback.c"))
        assert parsed == [finding]
        # Malformed reports must not become successful empty analysis.
        try:
            list(analyzer.sarif_findings({}, directory, "fallback.c"))
        except KeyError:
            pass
        else:
            raise AssertionError("invalid SARIF silently accepted")
    print("Compiler record, atomic write, analysis argv and exact fingerprint checks passed")


if __name__ == "__main__":
    main()

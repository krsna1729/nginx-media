#!/usr/bin/env python3
"""Exercise source attribution against real linked artifacts, including stale metadata.

Run: python3 tests/bench/test_transport_provenance.py
Uses a tiny local transport library; no network or nginx build is required.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


preflight = load("capacity_preflight_provenance", HERE / "capacity_preflight.py")
history = load("bench_history_provenance", HERE / "bench_history.py")


@unittest.skipUnless(all(shutil.which(tool) for tool in
                         ("cc", "git", "cmake", "ninja", "pkg-config", "ldd")),
                     "native transport fixture requires cc/git/cmake/ninja/pkg-config/ldd")
class TransportProvenance(unittest.TestCase):
    def test_checked_out_commit_follows_resolved_library_not_configured_pin(self):
        with tempfile.TemporaryDirectory() as work:
            base = Path(work)
            repository = base / "repo"
            repository.mkdir()
            (repository / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(provenance_fixture C)\n"
                "add_library(srt SHARED srt.c)\n"
                "install(TARGETS srt DESTINATION lib)\n")
            (repository / "srt.c").write_text(
                "unsigned srt_getversion(void) { return 0x010507; }\n")
            subprocess.run(["git", "init", "-q", str(repository)], check=True)
            subprocess.run(["git", "-C", str(repository), "add", "."], check=True)
            subprocess.run(["git", "-C", str(repository), "-c", "user.name=Fixture",
                            "-c", "user.email=fixture@example.invalid", "commit", "-qm", "transport"],
                           check=True)
            commit = subprocess.check_output(
                ["git", "-C", str(repository), "rev-parse", "HEAD"], text=True).strip()
            prefix = base / "pinned"
            self.enterContext(patch.dict(os.environ, {"LD_LIBRARY_PATH": str(prefix / "lib")}))
            environment = dict(os.environ, SRT_BUILD_DIR=str(base / "build"), SRT_SHARED="yes")
            build = [str(ROOT / "scripts/build-srt.sh"), str(repository), commit,
                     str(prefix), "fixture-srt"]
            subprocess.run(build, env=environment, check=True)
            library = (prefix / "lib/libsrt.so").resolve()
            metadata = json.loads(Path(str(library) + ".provenance.json").read_text())
            self.assertEqual(metadata["source"]["commit"], commit)
            self.assertEqual(metadata["artifact"]["sha256"], preflight.file_sha256(library))
            source = base / "receiver.c"
            source.write_text("extern unsigned srt_getversion(void);\n"
                              "int main(void) { return srt_getversion() == 0x010507 ? 0 : 1; }\n")
            binary = base / "receiver"
            environment["PKG_CONFIG_PATH"] = str(prefix / "lib/pkgconfig")
            environment["LD_RUN_PATH"] = str(prefix / "lib")
            subprocess.run(["python3", str(ROOT / "scripts/build-capacity-receiver.py"),
                            "--source", str(source), "--binary", str(binary), "--pkg", "fixture-srt",
                            "--compiler", "cc", "--cflags=-O0 -Wall"], env=environment, check=True)
            subprocess.run([str(binary)], check=True)
            transport = preflight.transport_provenance(str(binary))
            self.assertEqual(transport["source_commit"], commit)
            self.assertEqual(transport["library_version"], "1.5.7")
            self.assertEqual(transport["library_path"], str(library))
            native = preflight.nginx_build(str(binary))
            self.assertIn("-O0", native["cflags"])
            self.assertEqual(native["source"]["sha256"], preflight.file_sha256(source))
            self.assertEqual(native["compiler"], subprocess.check_output(
                ["cc", "--version"], text=True).strip())
            record = history.run_environment({"rungs": [{"receiver_host_fingerprint": {
                "build": native, "transport_provenance": transport}}]})
            self.assertEqual(record["provenance"]["receiver"]["transport_provenance"]["source_commit"], commit)
            self.assertNotIn("provenance", record.get("fingerprint", {}))

            # Metadata from another artifact cannot identify an unchanged path.
            stale = dict(metadata, artifact=dict(metadata["artifact"], sha256="0" * 64))
            sidecar = Path(str(library) + ".provenance.json")
            sidecar.write_text(json.dumps(stale))
            self.assertIsNone(preflight.transport_provenance(str(binary))["source_commit"])
            self.assertEqual(preflight.transport_provenance(str(binary))["library_version"], "1.5.7")
            subprocess.run(build, env=environment, check=True)
            self.assertEqual(preflight.transport_provenance(str(binary))["source_commit"], commit)

            # A system/unprovenanced resolution remains unknown, even though the
            # receiver's still-valid sidecar records a configured pinned package.
            system = base / "system"
            system.mkdir()
            shutil.copy2(library, system / "libsrt.so")
            previous = os.environ.get("LD_LIBRARY_PATH")
            os.environ["LD_LIBRARY_PATH"] = str(system)
            try:
                resolved = preflight.transport_provenance(str(binary))
                self.assertEqual(resolved["library_path"], str(system / "libsrt.so"))
                self.assertIsNone(resolved["source_commit"])
            finally:
                if previous is None:
                    del os.environ["LD_LIBRARY_PATH"]
                else:
                    os.environ["LD_LIBRARY_PATH"] = previous

            # A native binary replacement invalidates its compiler/source sidecar.
            with binary.open("ab") as output:
                output.write(b"changed artifact")
            replaced = preflight.nginx_build(str(binary))
            self.assertNotIn("source", replaced)
            self.assertNotIn("cflags", replaced)
            self.assertEqual(replaced["sha256"], preflight.file_sha256(binary))


if __name__ == "__main__":
    unittest.main()

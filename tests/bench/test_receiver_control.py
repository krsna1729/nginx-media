#!/usr/bin/env python3
"""Controlled receiver attribution must not blame an unknown or weak source."""
import copy
import importlib.util
from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("receiver_capacity_control", HERE / "receiver_capacity_control.py")
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


def peer(healthy=True, stalled=False):
    return {"source_full_rate": True, "source_offered_ratio": 1.0,
            "receiver_full_rate": healthy, "receiver_counters": {"stalled": int(stalled)}}


def result(stall=False):
    return {"status": "insufficient-evidence" if stall else "qualified",
            "recipe": {"protocol": "srt", "destinations": 2, "bitrate_bps": 8_000_000,
                       "packet_pacing": "constant", "receiver_stall_destination": "d0000" if stall else None},
            "destinations": {"d0000": peer(not stall, stall), "d0001": peer()}}


class Attribution(unittest.TestCase):
    def test_changed_receiver_with_full_rate_healthy_control_is_attributed(self):
        verdict = control.classify_pair(result(), result(True), "d0000")
        self.assertEqual(verdict["status"], "receiver-limited")
        self.assertEqual(verdict["affected_destination"], "d0000")

    def test_unqualified_baseline_and_weak_source_cannot_blame_receiver(self):
        normal, impaired = result(), result(True)
        normal["status"] = "insufficient-evidence"
        self.assertEqual(control.classify_pair(normal, impaired, "d0000")["status"],
                         "insufficient-evidence")
        impaired["destinations"]["d0000"]["source_offered_ratio"] = 0.5
        self.assertEqual(control.classify_pair(result(), impaired, "d0000")["status"],
                         "insufficient-evidence")
        impaired = result(True)
        impaired["destinations"]["d0001"]["source_full_rate"] = False
        self.assertEqual(control.classify_pair(result(), impaired, "d0000")["status"],
                         "insufficient-evidence")

    def test_cpu_pressure_without_receiver_fault_does_not_establish_cause(self):
        impaired = result(True)
        impaired["cpu_accounting"] = {"receiver_cpu_pct": 100.0}
        impaired["destinations"]["d0000"]["receiver_counters"]["stalled"] = 0
        self.assertEqual(control.classify_pair(result(), impaired, "d0000")["status"],
                         "insufficient-evidence")

    def test_different_load_or_placement_is_not_a_controlled_comparison(self):
        for change in ({"bitrate_bps": 4_000_000}, {"receiver_cpus": "12"}):
            impaired = copy.deepcopy(result(True))
            impaired["recipe"].update(change)
            with self.assertRaises(ValueError):
                control.classify_pair(result(), impaired, "d0000")

    def test_killed_controller_does_not_leave_workload_running(self):
        with tempfile.TemporaryDirectory() as directory:
            identity = Path(directory) / "child.pid"
            code = (
                f"import sys; sys.path.insert(0, {str(HERE)!r}); "
                "import receiver_capacity_control as control; "
                "import pathlib,subprocess,time; "
                "child=subprocess.Popen(['sleep','60'],preexec_fn=control.owner_binding()); "
                f"pathlib.Path({str(identity)!r}).write_text(str(child.pid)); time.sleep(60)")
            owner = subprocess.Popen([sys.executable, "-c", code])
            child = None
            try:
                deadline = time.monotonic() + 5
                while not identity.exists():
                    self.assertIsNone(owner.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.01)
                child = int(identity.read_text())
                owner.kill()
                owner.wait(timeout=5)
                while True:
                    try:
                        state = Path(f"/proc/{child}/stat").read_text().rsplit(")", 1)[1].split()[0]
                    except FileNotFoundError:
                        break
                    if state == "Z":
                        break
                    self.assertLess(time.monotonic(), deadline, "controller's workload survived SIGKILL")
                    time.sleep(0.01)
            finally:
                if owner.poll() is None:
                    owner.kill()
                    owner.wait(timeout=5)
                if child is not None:
                    try:
                        os.kill(child, signal.SIGKILL)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    unittest.main()

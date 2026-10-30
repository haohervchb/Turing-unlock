# SPDX-License-Identifier: copyleft-next-0.3.1
"""Host-only input/monitor tests; no CUDA imports, driver calls, or workloads."""
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest
from unittest import mock

import tensor_validate_windows as harness


class HostGuards(unittest.TestCase):
    def test_domain_formats_compare_equal(self):
        self.assertEqual(harness.normalize_bdf("0000:0A:01.0"), "00000000:0a:01.0")
        self.assertEqual(harness.normalize_bdf("00000000:0a:01.0"), "00000000:0a:01.0")

    def test_invalid_pci_addresses_refused(self):
        for value in ("0", "GPU-identifier", "0000:01:20.0", "0000:01:00.8", "0000:01:00.0\n"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                harness.normalize_bdf(value)

    def test_telemetry_must_match_selected_gpu(self):
        with self.assertRaises(RuntimeError):
            harness.parse_telemetry("0000:02:00.0, 45, 0, 25, 250, 300, 405, 120", "00000000:01:00.0")

    def test_bad_telemetry_refused(self):
        for temperature in ("N/A", "nan", "inf", "-1"):
            with self.subTest(value=temperature), self.assertRaises((ValueError, RuntimeError)):
                harness.parse_telemetry(f"0000:01:00.0, {temperature}, 0, 25, 250, 300, 405, 120", "00000000:01:00.0")
        with self.assertRaises(RuntimeError):
            harness.parse_telemetry("0000:01:00.0, 45", "00000000:01:00.0")

    def test_over_temperature_stops_before_monitor_thread(self):
        with tempfile.TemporaryDirectory() as temp:
            monitor = harness.Monitor(Path(temp), "not-executed", "00000000:01:00.0", max_temp=80)
            result = subprocess.CompletedProcess([], 0, "0000:01:00.0, 80, 0, 25, 250, 300, 405, 120")
            with mock.patch.object(harness.subprocess, "run", return_value=result):
                with self.assertRaises(RuntimeError):
                    monitor.start()
            self.assertIsNone(monitor.thread)
            self.assertNotIn("pci", monitor.samples[0])

    def test_deadline_stops_next_batch(self):
        with tempfile.TemporaryDirectory() as temp:
            monitor = harness.Monitor(Path(temp), "not-executed", "00000000:01:00.0", max_seconds=30)
            with mock.patch.object(harness.time, "monotonic", return_value=monitor.deadline + 1):
                with self.assertRaises(RuntimeError):
                    monitor.check()

    def test_gpu_workload_requires_explicit_run(self):
        command = [sys.executable, str(Path(harness.__file__)), "--out", "not-created", "--pci-bus-id", "0000:01:00.0"]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--run is required", result.stderr)
        self.assertNotIn("torch", result.stderr)


if __name__ == "__main__":
    unittest.main()

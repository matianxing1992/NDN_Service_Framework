from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core.v3_lifecycle import (  # noqa: E402
    V3AdmissionController, V3LifecycleState, V3QueueRecord,
)


class Spec170AdmissionLifecycleTest(unittest.TestCase):
    def test_no_partial_device_set(self):
        controller = V3AdmissionController(
            "p", boot_epoch="boot-0001", visible_devices=("cuda:0", "cuda:1"))
        self.assertEqual(controller.held_devices, ())
        # No record means no partial or speculative admission is possible.
        with self.assertRaises(ValueError):
            controller.admit_devices("missing", 1, ("cuda:0",), resource_sequence=1)

    def test_progress_is_monotonic_and_deadline_observable(self):
        controller = V3AdmissionController(
            "p", boot_epoch="boot-0001", visible_devices=("cpu",))
        record = V3QueueRecord("req", 1, "p", ())
        controller._records[("req", 1)] = record
        controller.report_progress("req", 1, 0.25)
        with self.assertRaises(ValueError):
            controller.report_progress("req", 1, 0.2)
        self.assertTrue(controller.no_progress_expired(
            "req", 1, now_ms=record.last_progress_ms + 101,
            no_progress_ms=100))


if __name__ == "__main__":
    unittest.main()

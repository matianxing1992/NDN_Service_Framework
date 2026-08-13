from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.core.device_scheduler import (  # noqa: E402
    DeviceJobV3, MultiDeviceSchedulerV3,
)


class Spec170MultiDeviceProviderTest(unittest.TestCase):
    def test_ack_queue_does_not_hold_two_independent_gpus(self) -> None:
        scheduler = MultiDeviceSchedulerV3({"cuda:0": 12_000, "cuda:1": 12_000})
        scheduler.submit(DeviceJobV3("req-0", "stage-0", 8_000))
        scheduler.submit(DeviceJobV3("req-1", "stage-1", 8_000))
        self.assertEqual(scheduler.used_memory_mb, {"cuda:0": 0, "cuda:1": 0})
        first = scheduler.admit("req-0")
        second = scheduler.admit("req-1")
        self.assertEqual((first.device, second.device), ("cuda:0", "cuda:1"))
        self.assertEqual(scheduler.used_memory_mb, {"cuda:0": 8_000, "cuda:1": 8_000})
        scheduler.complete("req-0")
        scheduler.complete("req-1")
        self.assertEqual(scheduler.used_memory_mb, {"cuda:0": 0, "cuda:1": 0})

    def test_unsplittable_role_is_rejected_instead_of_memory_pooling(self) -> None:
        scheduler = MultiDeviceSchedulerV3({"cuda:0": 12_000, "cuda:1": 12_000})
        with self.assertRaisesRegex(ValueError, "single visible device"):
            scheduler.submit(DeviceJobV3("req-large", "stage", 20_000))

    def test_admission_is_atomic_when_a_device_becomes_busy(self) -> None:
        scheduler = MultiDeviceSchedulerV3({"cuda:0": 12_000, "cuda:1": 12_000})
        scheduler.submit(DeviceJobV3("req-a", "stage-a", 9_000, "cuda:0"))
        scheduler.submit(DeviceJobV3("req-b", "stage-b", 9_000, "cuda:0"))
        scheduler.admit("req-a")
        with self.assertRaisesRegex(RuntimeError, "capacity"):
            scheduler.admit("req-b")
        self.assertEqual(tuple(job.request_id for job in scheduler.queued), ("req-b",))


if __name__ == "__main__":
    unittest.main()

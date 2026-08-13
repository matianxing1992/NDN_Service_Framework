from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceResourceSnapshot, DeviceTopologyProfile,
)


class Spec170RuntimeTopologyTest(unittest.TestCase):
    def test_zero_devices_is_valid_for_cpu_profile(self):
        profile = DeviceTopologyProfile("p", (), "cpu")
        self.assertEqual(profile.devices, ())

    def test_duplicate_or_invalid_devices_fail_closed(self):
        with self.assertRaises(ValueError):
            DeviceTopologyProfile("p", ("cuda:0", "cuda:0"), "cuda")
        with self.assertRaises(ValueError):
            DeviceTopologyProfile("p", ("gpu0",), "cuda")

    def test_resource_snapshot_is_bounded(self):
        with self.assertRaises(ValueError):
            DeviceResourceSnapshot("cuda:0", 10, 11)


if __name__ == "__main__":
    unittest.main()

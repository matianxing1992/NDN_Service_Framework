from __future__ import annotations

from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[2] / "NDNSF-UAV-APP" / "tools"
sys.path.insert(0, str(TOOLS))
from multiview_contract import MultiViewJob, ViewReference, validate_job  # noqa: E402


class MultiViewIntegrationContractTest(unittest.TestCase):
    def _view(self, producer: str, view_id: str, target: str = "car-1") -> ViewReference:
        return ViewReference(view_id, producer, f"{producer}/UAV/IMAGE/{view_id}/v=1",
                             "sha256:" + "1" * 64, 100, target)

    def test_cpu_flow_accepts_only_exact_verified_references(self) -> None:
        job = MultiViewJob("mission", "job", 1, "car-1", 0, 1000,
                           [self._view("/uav/A", "a"), self._view("/uav/B", "b")])
        self.assertEqual(validate_job(job), [])

    def test_missing_or_cross_target_view_fails_before_algorithm(self) -> None:
        job = MultiViewJob("mission", "job", 1, "car-1", 0, 1000,
                           [self._view("/uav/A", "a"), self._view("/uav/B", "b", "car-2")])
        self.assertTrue(any("cross-target" in error for error in validate_job(job)))


if __name__ == "__main__":
    unittest.main()


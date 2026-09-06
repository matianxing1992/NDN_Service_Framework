from __future__ import annotations

from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[2] / "NDNSF-UAV-APP" / "tools"
sys.path.insert(0, str(TOOLS))
from multiview_contract import MultiViewJob, ViewReference, validate_job  # noqa: E402


class MultiViewSecurityContractTest(unittest.TestCase):
    def _job(self) -> MultiViewJob:
        return MultiViewJob(
            "mission", "job", 1, "car", 0, 1000,
            [ViewReference("a", "/uav/A", "/uav/A/UAV/IMAGE/a/v=1", "sha256:" + "1" * 64, 10, "car"),
             ViewReference("b", "/uav/B", "/uav/B/UAV/IMAGE/b/v=1", "sha256:" + "2" * 64, 10, "car")],
        )

    def test_endpoint_and_bad_digest_are_rejected(self) -> None:
        job = self._job()
        job.views[0] = ViewReference("a", "/10.0.0.1:6363", "http://10.0.0.1/a",
                                     "sha256:" + "1" * 64, 10, "car")
        errors = validate_job(job)
        self.assertTrue(any("exactDataName" in error or "producerIdentity" in error
                            for error in errors))
        job = self._job()
        job.views[0] = ViewReference(**{**job.views[0].__dict__, "content_digest": "bad"})
        self.assertTrue(any("contentDigest" in error for error in validate_job(job)))

    def test_replay_duplicate_and_single_provider_policy_are_rejected(self) -> None:
        job = self._job()
        job.views[1] = job.views[0]
        self.assertTrue(any("duplicate" in error for error in validate_job(job)))
        job = self._job()
        job.minimum_distinct_producers = 2
        job.views[1] = ViewReference("b", "/uav/A", "/uav/A/UAV/IMAGE/b/v=1",
                                     "sha256:" + "2" * 64, 10, "car")
        self.assertTrue(any("distinct" in error for error in validate_job(job)))


if __name__ == "__main__":
    unittest.main()


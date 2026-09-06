from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[2] / "NDNSF-UAV-APP" / "tools"
sys.path.insert(0, str(TOOLS))

from multiview_contract import (  # noqa: E402
    MultiViewJob,
    ViewReference,
    load_json,
    validate_fixture_manifest,
    validate_job,
    validate_result,
)


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "NDNSF-UAV-APP" / "testdata" / "multiview-car"
MODEL_REGISTRY = ROOT / "NDNSF-UAV-APP" / "configs" / "uav_multiview_models.json"


class MultiViewContractTest(unittest.TestCase):
    def test_registered_model_profile_and_fixture_policy(self) -> None:
        registry = load_json(MODEL_REGISTRY)
        self.assertEqual(registry["schema"], "ndnsf-uav-multiview-model-registry/v2")
        profile = registry["profiles"][0]
        self.assertEqual(profile["algorithm_id"], "mvcnn-onnx-maxpool/v1")
        self.assertEqual(profile["execution_provider"], "CPUExecutionProvider")
        self.assertEqual(profile["minimum_views"], 2)
        self.assertEqual(profile["maximum_views"], 6)
        self.assertEqual(profile["minimum_distinct_producers"], 2)
        self.assertTrue(profile["model_digest"].startswith("sha256:"))

    def test_fixture_hashes_and_functional_only_marker(self) -> None:
        manifest = load_json(FIXTURE / "manifest.json")
        self.assertEqual(validate_fixture_manifest(manifest, FIXTURE), [])

    def _job(self, count: int = 2) -> MultiViewJob:
        manifest = load_json(FIXTURE / "manifest.json")
        views = []
        for entry in manifest["views"][:count]:
            views.append(ViewReference(
                view_id=entry["view_id"],
                producer_identity=entry["producer_identity"],
                exact_data_name=f"{entry['producer_identity']}/UAV/IMAGE/{entry['view_id']}/v=1",
                content_digest=f"sha256:{entry['sha256']}",
                capture_time_ms=100,
                target_id=manifest["target_id"],
                viewpoint=entry["viewpoint"],
            ))
        return MultiViewJob("mission-001", "job-001", 1, manifest["target_id"],
                            0, 1000, views)

    def test_job_requires_two_distinct_verified_views(self) -> None:
        job = self._job(2)
        self.assertEqual(validate_job(job), [])
        one = self._job(1)
        self.assertTrue(any("minimumViews" in error or "distinct" in error
                            for error in validate_job(one)))

    def test_job_rejects_duplicate_and_cross_target_references(self) -> None:
        job = self._job(2)
        job.views[1] = job.views[0]
        errors = validate_job(job)
        self.assertTrue(any("duplicate" in error for error in errors))
        job = self._job(2)
        job.views[1] = ViewReference(**{**job.views[1].__dict__, "target_id": "other"})
        self.assertTrue(any("cross-target" in error for error in validate_job(job)))

    def test_result_requires_one_annotation_per_contributing_view(self) -> None:
        job = self._job(2)
        result = {
            "schema": "ndnsf-uav-multiview-result/v1",
            "status": "completed",
            "missionSessionId": job.mission_session_id,
            "jobId": job.job_id,
            "contributingViews": ["view-01", "view-02"],
            "annotatedViews": [{"viewId": "view-01", "sourceDigest": "sha256:x",
                                "exactDataName": "/provider/a", "contentDigest": "sha256:y",
                                "signerIdentity": "/provider"}],
        }
        self.assertTrue(any("annotation count" in error for error in validate_result(result, job)))


if __name__ == "__main__":
    unittest.main()

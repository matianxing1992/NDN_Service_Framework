from __future__ import annotations

import json
from pathlib import Path
import unittest

from ndnsf_distributed_inference.deployment import deployment_role_provider_preference


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/ndnsf-di-core-app-separation/deployment-preference-golden.json"


class _Native:
    def pump(self, _milliseconds: int) -> None:
        pass


class _ServiceUser:
    _native = _Native()

    def __init__(self, deployments: str) -> None:
        self._deployments = deployments

    def get_ndnsd_services(self):
        return [{"serviceMetaInfo": {"deployments": self._deployments}}]


class DeploymentPreferenceGoldenTest(unittest.TestCase):
    def test_current_legacy_translation_matches_golden(self) -> None:
        payload = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.assertEqual(payload["schema"], "ndnsf-di-spec111-deployment-preference-golden-v1")
        for case in payload["cases"]:
            deployments = case.get("rawDeployments", json.dumps(case.get("records", [])))
            with self.subTest(case=case["id"]):
                self.assertEqual(
                    deployment_role_provider_preference(_ServiceUser(deployments), case["deploymentId"]),
                    case["expected"],
                )


if __name__ == "__main__":
    unittest.main()

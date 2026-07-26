from __future__ import annotations

import json
from pathlib import Path
import unittest

from ndnsf_distributed_inference.core.decision_validation import validate_assignment
from ndnsf_distributed_inference.core.ports import ProviderCandidate
from ndnsf_distributed_inference.planner.cost_policy import score_cost
from ndnsf_distributed_inference.planner.fixed_policy import FixedProviderAssignmentPolicy
from ndnsf_distributed_inference.planner.provider_assignment_policy import CostProviderAssignmentPolicy
from support.spec111_policy import request


ROOT = Path(__file__).resolve().parents[2]


class ProviderAssignmentPolicyTest(unittest.TestCase):
    def test_fixed_one_and_multi_role_fixtures(self):
        fixture = json.loads((ROOT / "tests/fixtures/ndnsf-di-core-app-separation/fixed-policy-cases.json").read_text())
        for case in fixture["cases"]:
            policy = FixedProviderAssignmentPolicy(case["providersByRole"])
            result = policy.propose(request(metadata={"plan_id": "p", "variant_id": "v"}))
            self.assertEqual(result.value.providers_by_role, case["providersByRole"])
            validate_assignment(result.value, case["roles"], case["providersByRole"].values())

    def test_cost_golden_parity(self):
        fixture = json.loads((ROOT / "tests/fixtures/ndnsf-di-core-app-separation/cost-policy-cases.json").read_text())
        for case in fixture["cases"]:
            self.assertAlmostEqual(score_cost(
                compute_ms=case["computeMs"], residency_ready_ms=case["residencyReadyMs"],
                queue_wait_ms=case["queueWaitMs"], queue_length=case["queueLength"],
                confidence=case["confidence"]), case["scoreMs"])

    def test_cost_assignment_is_role_scoped(self):
        providers = (ProviderCandidate("/b", "1", ("decode",), score=2),
                     ProviderCandidate("/a", "1", ("prefill", "decode"), score=1))
        result = CostProviderAssignmentPolicy().propose(request(providers, metadata={
            "roles": ["prefill", "decode"], "plan_id": "p", "variant_id": "v"}))
        self.assertEqual(result.value.providers_by_role, {"prefill": "/a", "decode": "/a"})


if __name__ == "__main__": unittest.main()

from __future__ import annotations

import unittest

from ndnsf_distributed_inference.app_sdk.engine import DistributedInferenceEngine
from ndnsf_distributed_inference.core.ports import (
    CandidateBudget, EngineSnapshot, MetricValue, ModelCandidate,
    OptimizationObjective, PolicyEvidence, PolicyResult, PolicyState,
)
from ndnsf_distributed_inference.planner.defaults import DefaultOptimizationSuite
from ndnsf_distributed_inference.sdk.suite import OptimizationSuite

from ndnsf_distributed_inference.core import (
    CandidateFacts,
    EligibilityRequirements,
    eligible_candidates,
    evaluate_candidate,
)


class CoreEligibilityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.facts = CandidateFacts(
            "/p1", "boot-1", 900, ("sha256:fragment",), "lease-1", 2_000)
        self.requirements = EligibilityRequirements(
            1_000, 200, "sha256:fragment", True, "boot-1")

    def test_eligible_candidate_passes_and_normalization_is_deterministic(self) -> None:
        self.assertTrue(evaluate_candidate(self.facts, self.requirements).eligible)
        candidates = eligible_candidates([
            {"providerName": "/p2", "providerBootEpoch": "boot-2",
             "observedAtMs": 950, "fragmentDigests": ["sha256:fragment"],
             "leaseId": "l2", "leaseExpiresAtMs": 2_000},
            self.facts,
        ], EligibilityRequirements(1_000, 200, "sha256:fragment", True))
        self.assertEqual([item.provider for item in candidates], ["/p1", "/p2"])

    def test_stale_infeasible_fragment_lease_boot_and_exclusion_fail_closed(self) -> None:
        cases = (
            (CandidateFacts("/p1", "boot-1", 700), EligibilityRequirements(1_000, 200), "CANDIDATE_STALE"),
            (CandidateFacts("/p1", "boot-1", 900, feasible=False), EligibilityRequirements(1_000, 200), "CANDIDATE_INFEASIBLE"),
            (self.facts, EligibilityRequirements(1_000, 200, "sha256:missing"), "FRAGMENT_UNAVAILABLE"),
            (CandidateFacts("/p1", "boot-1", 900), EligibilityRequirements(1_000, 200, lease_required=True), "LEASE_REQUIRED"),
            (CandidateFacts("/p1", "boot-1", 900, lease_id="l", lease_expires_at_ms=1_000), EligibilityRequirements(1_000, 200, lease_required=True), "LEASE_EXPIRED"),
            (self.facts, EligibilityRequirements(1_000, 200, expected_provider_boot_epoch="boot-2"), "PROVIDER_BOOT_EPOCH_MISMATCH"),
            (self.facts, EligibilityRequirements(1_000, 200, excluded_providers=("/p1",)), "PROVIDER_EXCLUDED"),
        )
        for candidate, requirements, expected in cases:
            with self.subTest(expected=expected):
                decision = evaluate_candidate(candidate, requirements)
                self.assertFalse(decision.eligible)
                self.assertEqual(decision.reason, expected)

    def test_core_final_validation_rejects_malicious_policy_result(self) -> None:
        allowed = ModelCandidate("qwen", "7b", True, "fp16", "sha256:" + "7" * 64)
        outsider = ModelCandidate("qwen", "72b", True, "fp16", "sha256:" + "2" * 64)

        class Malicious:
            def propose(self, request):
                return PolicyResult("model_variant", outsider, PolicyEvidence(
                    "model_variant", "malicious", "1", request.snapshot.epoch,
                    request.snapshot.state.digest_value, outsider.digest()))

        defaults = DefaultOptimizationSuite()
        policies = {kind: defaults.policy(kind) for kind in defaults.policy_names()}
        policies["model_variant"] = Malicious()
        suite = OptimizationSuite(policies, name="malicious", version="1",
                                  state_digest="sha256:" + "a" * 64)
        engine = DistributedInferenceEngine(suite)
        with self.assertRaisesRegex(ValueError, "outside authorized"):
            engine.choose_model(
                objective=OptimizationObjective(
                    {"latency": MetricValue(100, "ms", "max")},
                    {"latency": 1}, {"latency": 100}),
                snapshot=EngineSnapshot(
                    "s", 1, 1000, {}, {}, PolicyState(1, "sha256:s")),
                candidates=(allowed,), budget=CandidateBudget(1))


if __name__ == "__main__":
    unittest.main()

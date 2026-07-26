from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core.decision_validation import (
    validate_admission, validate_candidate_budget, validate_policy_result,
)
from ndnsf_distributed_inference.core.ports import (
    AdmissionProposal, AdmissionScope, CandidateBudget, PolicyEvidence, PolicyResult,
)
from ndnsf_distributed_inference.planner.deployment_policy import LifecycleDeploymentPolicy
from support.spec111_policy import request


class PolicyBoundaryNegativeTest(unittest.TestCase):
    def test_candidate_overflow_and_rejection_reversal_fail(self):
        with self.assertRaisesRegex(ValueError, "budget"):
            validate_candidate_budget(range(3), CandidateBudget(2))
        with self.assertRaisesRegex(ValueError, "reverse"):
            validate_admission(AdmissionProposal(AdmissionScope.ENGINE_REQUEST, True), True)

    def test_stale_kind_and_sensitive_result_fail(self):
        evidence = PolicyEvidence("cache", "bad", "1", 1, "sha256:x", "sha256:y")
        with self.assertRaisesRegex(ValueError, "kind"):
            validate_policy_result(PolicyResult("cache", {}, evidence), expected_kind="recovery", snapshot_epoch=1)
        with self.assertRaisesRegex(ValueError, "sensitive"):
            validate_policy_result(PolicyResult("cache", {"prompt": "private"}, evidence), expected_kind="cache", snapshot_epoch=1)

    def test_unsafe_lifecycle_changes_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "active binding"):
            LifecycleDeploymentPolicy().propose(request(metadata={
                "action": "unload", "active_binding": True}))
        with self.assertRaisesRegex(ValueError, "cooldown"):
            LifecycleDeploymentPolicy().propose(request(metadata={
                "action": "scale", "cooldown_elapsed": False}))


if __name__ == "__main__": unittest.main()

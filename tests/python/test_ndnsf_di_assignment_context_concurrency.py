from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import unittest

from ndnsf_distributed_inference.core import AssignmentContext
from ndnsf_distributed_inference.planner.fixed_policy import FixedProviderAssignmentPolicy
from ndnsf_distributed_inference.planner.defaults import DefaultOptimizationSuite
from ndnsf_distributed_inference.sdk.suite import OptimizationSuite
from support.spec111_policy import request


ROOT = Path(__file__).resolve().parents[2]


class AssignmentContextConcurrencyTest(unittest.TestCase):
    def test_100_overlapping_opposite_assignments_have_zero_bleed_and_no_env_write(self):
        fixture = json.loads((ROOT / "tests/fixtures/ndnsf-di-core-app-separation/concurrent-assignments.json").read_text())
        before = dict(os.environ)

        def one(index):
            selected = fixture["left" if index % 2 == 0 else "right"]
            policy = FixedProviderAssignmentPolicy(selected)
            defaults = DefaultOptimizationSuite()
            suite = OptimizationSuite(
                {"provider_assignment": policy}, name=f"suite-{index}", version="1",
                state_digest="sha256:" + f"{index:064x}"[-64:])
            resolved, _ = suite.resolve(defaults)
            proposal = resolved.policy("provider_assignment").propose(request(metadata={
                "plan_id": "p", "variant_id": "v", "assignment_id": str(index)})).value
            context = AssignmentContext(
                f"r-{index}", 1, "sha256:p", "v",
                tuple(sorted(proposal.providers_by_role.items())), 1000)
            return context.providers_by_role()

        with ThreadPoolExecutor(max_workers=16) as pool:
            outputs = list(pool.map(one, range(fixture["repetitions"])))
        self.assertEqual(outputs[::2], [fixture["left"]] * 50)
        self.assertEqual(outputs[1::2], [fixture["right"]] * 50)
        self.assertEqual(dict(os.environ), before)


if __name__ == "__main__": unittest.main()

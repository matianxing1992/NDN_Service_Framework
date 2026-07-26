from __future__ import annotations

from dataclasses import FrozenInstanceError
import unittest

from ndnsf_distributed_inference.core import AssignmentContext
from ndnsf_distributed_inference.core.placement import create_assignment_context
from ndnsf_distributed_inference.core.ports import AssignmentProposal
from ndnsf_distributed_inference.core.recovery import replan_assignment_context


class AssignmentContextTest(unittest.TestCase):
    def test_round_trip_and_immutability(self):
        context = AssignmentContext(
            "r", 1, "sha256:plan", "qwen-7b", (("decode", "/b"), ("prefill", "/a")), 1000)
        self.assertEqual(AssignmentContext.from_bytes(context.to_bytes()), context)
        with self.assertRaises(FrozenInstanceError):
            context.attempt_epoch = 2

    def test_final_placement_creates_context(self):
        proposal = AssignmentProposal("a", "p", "qwen-7b", {"prefill": "/a"})
        context = create_assignment_context(
            proposal, request_id="r", attempt_epoch=1, plan_digest="sha256:p",
            model_variant_id="qwen-7b", original_deadline_ms=1000,
            roles=("prefill",), eligible_providers=("/a",))
        self.assertEqual(context.providers_by_role(), {"prefill": "/a"})

    def test_malformed_duplicate_or_excluded_provider_rejected(self):
        with self.assertRaisesRegex(ValueError, "bind exactly once"):
            AssignmentContext("r", 1, "p", "v", (("x", "/a"), ("x", "/b")), 10)
        with self.assertRaisesRegex(ValueError, "excluded"):
            AssignmentContext("r", 1, "p", "v", (("x", "/a"),), 10, ("/a",))

    def test_replan_preserves_deadline_and_exclusion_lineage(self):
        current = AssignmentContext("r", 1, "p", "v", (("x", "/a"),), 1000)
        newer = replan_assignment_context(
            current, role_providers={"x": "/b"}, newly_excluded=("/a",))
        self.assertEqual((newer.attempt_epoch, newer.original_deadline_ms), (2, 1000))
        self.assertEqual(newer.excluded_providers, ("/a",))


if __name__ == "__main__": unittest.main()

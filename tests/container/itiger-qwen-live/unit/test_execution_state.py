from __future__ import annotations

import unittest

from _support import load_tool


state = load_tool("spec110_state")


class ExecutionStateTests(unittest.TestCase):
    def test_prestart_blocker_cannot_close_live_task(self):
        record = state.new_execution_state("spec110-cell-" + "a" * 20)
        blocked = state.transition(record, "PREFLIGHT_BLOCKED", {"reasonCode": "VPN_UNAVAILABLE"})
        self.assertFalse(state.can_close_live_task(blocked))

    def test_post_start_failure_with_boundary_can_close(self):
        record = state.new_execution_state("spec110-cell-" + "a" * 20)
        record = state.transition(record, "READY_TO_SUBMIT", {"preflightDigest": "sha256:" + "a" * 64})
        record = state.transition(record, "SUBMITTED_NOT_STARTED", {"jobId": "123"})
        record = state.transition(record, "CANDIDATE_EXECUTION_STARTED", {"executionProofDigest": "sha256:" + "b" * 64})
        record = state.transition(record, "EXECUTED_FAIL", {"failureBoundary": "stage-execution"})
        self.assertTrue(state.can_close_live_task(record))

    def test_post_start_failure_without_boundary_is_rejected(self):
        record = state.new_execution_state("spec110-cell-" + "a" * 20)
        for target, evidence in (
            ("READY_TO_SUBMIT", {"preflightDigest": "sha256:" + "a" * 64}),
            ("SUBMITTED_NOT_STARTED", {"jobId": "123"}),
            ("CANDIDATE_EXECUTION_STARTED", {"executionProofDigest": "sha256:" + "b" * 64}),
        ):
            record = state.transition(record, target, evidence)
        with self.assertRaisesRegex(state.ExecutionStateError, "FAILURE_BOUNDARY_REQUIRED"):
            state.transition(record, "EXECUTED_FAIL", {})

    def test_states_cannot_skip_execution_boundary(self):
        record = state.new_execution_state("spec110-cell-" + "a" * 20)
        with self.assertRaisesRegex(state.ExecutionStateError, "EXECUTION_TRANSITION_INVALID"):
            state.transition(record, "EXECUTED_PASS", {})

    def test_pass_requires_placement_semantics(self):
        record = state.new_execution_state("spec110-cell-" + "a" * 20)
        record = state.transition(record, "READY_TO_SUBMIT", {"preflightDigest": "sha256:" + "a" * 64})
        record = state.transition(record, "SUBMITTED_NOT_STARTED", {"jobId": "123"})
        record = state.transition(record, "CANDIDATE_EXECUTION_STARTED", {"executionProofDigest": "sha256:" + "b" * 64})
        record = state.transition(record, "EXECUTED_PASS", {"placementSemanticsValid": False})
        self.assertFalse(state.can_close_live_task(record))


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core.recovery import BoundedRecoveryController, RecoveryReason
from ndnsf_distributed_inference.core.ports import ProgressRecord, RecoveryProposal


class StreamRecoveryTest(unittest.TestCase):
    def test_old_output_epoch_is_rejected_after_replacement(self):
        recovery = BoundedRecoveryController("r", request_deadline_ms=1000,
                                             started_at_ms=100, max_replacements=1)
        recovery.start("/a")
        action = recovery.recover(RecoveryReason.PROVIDER_LOST, at_ms=200,
                                  replacement_provider="/b")
        self.assertEqual(action.attempt_epoch, 2)
        self.assertFalse(recovery.accept_result(1, b"stale"))
        self.assertTrue(recovery.accept_result(2, b"visible"))
        self.assertFalse(recovery.accept_result(2, b"duplicate"))

    def test_policy_transition_preserves_deadline_and_output_epoch(self):
        recovery = BoundedRecoveryController("r", request_deadline_ms=1000,
                                             started_at_ms=100, max_replacements=1)
        recovery.start("/a")
        action = recovery.apply_transition(
            RecoveryProposal("retry", 2, 1000, "/b"), at_ms=200,
            progress=ProgressRecord("r", 1, "decode", "streaming", 2))
        self.assertEqual((action.provider, action.attempt_epoch), ("/b", 2))
        with self.assertRaisesRegex(ValueError, "stale"):
            recovery.apply_transition(
                RecoveryProposal("retry", 3, 1000, "/c"), at_ms=300,
                progress=ProgressRecord("r", 1, "decode", "streaming", 1))


if __name__ == "__main__": unittest.main()

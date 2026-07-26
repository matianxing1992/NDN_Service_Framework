from __future__ import annotations

from dataclasses import replace
import unittest

from ndnsf_distributed_inference.core.execution_intent import ExecutionIntentCoordinator
from ndnsf_distributed_inference.core.ports import AssignmentProposal, ValidatedExecutionIntent


def intent(state="PREPARED"):
    assignment = AssignmentProposal("a", "p", "v", {"r": "/provider"})
    return ValidatedExecutionIntent("i", "request", 1, 1, "v", "p", assignment,
                                    "sha256:lease", 1, 1, state)


class ExecutionIntentTest(unittest.TestCase):
    def test_prepare_revalidate_commit_release(self):
        coordinator = ExecutionIntentCoordinator(); coordinator.prepare(intent())
        coordinator.revalidate("i", lambda value: self.assertEqual(value.snapshot_epoch, 1))
        self.assertEqual(coordinator.commit("i").state, "COMMITTED")
        self.assertEqual(coordinator.release("i").state, "RELEASED")

    def test_torn_or_conflicting_intent_is_rejected(self):
        coordinator = ExecutionIntentCoordinator(); coordinator.prepare(intent())
        with self.assertRaisesRegex(ValueError, "conflicting"):
            coordinator.prepare(replace(intent(), lease_digest="sha256:other"))
        with self.assertRaisesRegex(ValueError, "cannot be aborted"):
            coordinator.commit("i"); coordinator.abort("i")


if __name__ == "__main__": unittest.main()

from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core.decision_validation import validate_scheduling
from ndnsf_distributed_inference.core.ports import SchedulingProposal, SchedulingScope
from ndnsf_distributed_inference.planner.scheduling_policy import ScopedFifoSchedulingPolicy
from ndnsf_distributed_inference.plan import ContentionRetryPolicy
from support.spec111_policy import request


class SchedulingContractTest(unittest.TestCase):
    def test_batch_is_adapter_bounded(self):
        result = ScopedFifoSchedulingPolicy().dispatch(request(
            ("a", "b"), scope="PROVIDER_LOCAL",
            metadata={"batch_size": 8, "adapter_max_batch": 2}))
        self.assertEqual(result.value.batch_size, 2)
        validate_scheduling(result.value, ("a", "b"))

    def test_cross_scope_and_duplicates_rejected(self):
        with self.assertRaisesRegex(ValueError, "crosses"):
            validate_scheduling(SchedulingProposal(SchedulingScope.REQUEST_DAG, ("foreign",)), ("local",))
        with self.assertRaisesRegex(ValueError, "duplicates"):
            validate_scheduling(SchedulingProposal(SchedulingScope.PROVIDER_LOCAL, ("x", "x")), ("x",))

    def test_r1_contention_policy_uses_absolute_bounded_deadline(self):
        retry = ContentionRetryPolicy(
            max_attempts=2, total_deadline_ms=100,
            base_backoff_ms=10, max_backoff_ms=20).controller(
                started_at_ms=1_000, seed=7)
        self.assertEqual(retry.begin(now_ms=1_000), 1)
        retry.close_partial({"lease": 1_020},
                            send_not_selected=lambda _reservation: None)
        with self.assertRaisesRegex(RuntimeError, "waiting"):
            retry.next_backoff(now_ms=1_010)
        self.assertLessEqual(retry.next_backoff(now_ms=1_020), 10)
        self.assertEqual(retry.begin(now_ms=1_030), 2)
        with self.assertRaisesRegex(RuntimeError, "exhausted"):
            retry.next_backoff(now_ms=1_030)


if __name__ == "__main__": unittest.main()

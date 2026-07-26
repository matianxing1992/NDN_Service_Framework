from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core import (
    BoundedRecoveryController, ContentionRetryController, RecoveryReason,
)


class CoreRecoveryTest(unittest.TestCase):
    def test_replacement_is_bounded_and_stale_result_is_fenced(self) -> None:
        controller = BoundedRecoveryController(
            "req-1", request_deadline_ms=2_000, started_at_ms=1_000,
            max_replacements=1)
        self.assertEqual(controller.start("/p1").attempt_epoch, 1)
        action = controller.recover(
            RecoveryReason.PROVIDER_LOST, at_ms=1_100,
            replacement_provider="/p2")
        self.assertEqual((action.action, action.attempt_epoch, action.provider),
                         ("replace", 2, "/p2"))
        self.assertFalse(controller.accept_result(1, b"stale"))
        self.assertTrue(controller.accept_result(2, b"current"))
        self.assertFalse(controller.accept_result(2, b"duplicate"))

    def test_deadline_and_exclusion_fail_terminally(self) -> None:
        deadline = BoundedRecoveryController(
            "req-1", request_deadline_ms=2_000, started_at_ms=1_000)
        deadline.start("/p1")
        action = deadline.recover(
            RecoveryReason.STRAGGLER_DEADLINE, at_ms=2_000,
            replacement_provider="/p2")
        self.assertEqual(action.terminal_reason, RecoveryReason.REQUEST_DEADLINE)

        excluded = BoundedRecoveryController(
            "req-2", request_deadline_ms=2_000, started_at_ms=1_000)
        excluded.start("/p1")
        action = excluded.recover(
            RecoveryReason.PROVIDER_LOST, at_ms=1_100,
            replacement_provider="/p2", excluded_providers=("/p2",))
        self.assertEqual(action.terminal_reason, RecoveryReason.NO_COMPATIBLE_REPLACEMENT)

    def test_full_context_retry_advances_attempt_on_same_provider(self) -> None:
        controller = BoundedRecoveryController(
            "req-1", request_deadline_ms=2_000, started_at_ms=1_000)
        controller.start("/p1")
        action = controller.recover(
            RecoveryReason.CACHE_MISS_FULL_CONTEXT_REQUIRED, at_ms=1_100)
        self.assertEqual(action.action, "retry-full-context")
        self.assertTrue(action.full_context_required)
        self.assertEqual(action.attempt_epoch, 2)

    def test_contention_retry_releases_before_seeded_full_jitter(self) -> None:
        sent = []
        retry = ContentionRetryController(
            max_attempts=3, total_deadline_ms=1000,
            base_backoff_ms=100, max_backoff_ms=400, seed=7)
        self.assertEqual(retry.begin(now_ms=0), 1)
        retry.close_partial({"r1": 200, "r2": 250},
                            send_not_selected=sent.append)
        self.assertEqual(sent, ["r1", "r2"])
        self.assertTrue(retry.accept_receipt("r1"))
        with self.assertRaisesRegex(RuntimeError, "waiting"):
            retry.next_backoff(now_ms=100)
        delay = retry.next_backoff(now_ms=250)
        self.assertGreaterEqual(delay, 0); self.assertLessEqual(delay, 100)
        self.assertEqual(retry.release_receipts, 1)
        self.assertEqual(retry.expiry_fallbacks, 1)
        self.assertEqual(retry.begin(now_ms=250 + delay), 2)

    def test_contention_retry_is_bounded_by_attempts_and_deadline(self) -> None:
        retry = ContentionRetryController(
            max_attempts=1, total_deadline_ms=10,
            base_backoff_ms=5, max_backoff_ms=5, seed=1)
        retry.begin(now_ms=0)
        with self.assertRaisesRegex(RuntimeError, "exhausted"):
            retry.next_backoff(now_ms=1)
        self.assertEqual(retry.exhausted, 1)


if __name__ == "__main__":
    unittest.main()

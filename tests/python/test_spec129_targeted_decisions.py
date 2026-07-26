from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core import (
    AckWindowDecisionCoordinator, AtomicReservationBook,
    BoundedExactTargetRetry, ReservationDecisionAuthority, SelectionDecision,
)


def reserve(book, request_id: str, now: int = 1):
    return book.reserve(requester="/u", service="/s", request_id=request_id,
                        attempt=1, units=1, now_ms=now, requested_lease_ms=100,
                        authorized=True, signature="sig")


class TargetedDecisionTest(unittest.TestCase):
    def test_close_resolves_every_eligible_and_late_ack_is_negative(self):
        book = AtomicReservationBook(
            "/p", "boot", capacity=4, per_requester_limit=4,
            per_service_limit=4, max_lease_ms=100, committed_lease_ms=200)
        first, second = reserve(book, "r1"), reserve(book, "r2")
        coordinator = AckWindowDecisionCoordinator(
            "request", 1, ack_deadline_ms=50, retain_until_ms=150)
        self.assertIsNone(coordinator.admit(first, completed_at_ms=10))
        self.assertIsNone(coordinator.admit(second, completed_at_ms=20))
        decisions = coordinator.close(
            now_ms=50, selected_reservation_ids={first.fields["reservationId"]})
        self.assertEqual({d.fields["decision"] for d in decisions},
                         {"SELECTED", "NOT_SELECTED"})
        late = reserve(book, "r3")
        self.assertEqual(coordinator.admit(late, completed_at_ms=51).fields["decision"],
                         "NOT_SELECTED")
        self.assertEqual(coordinator.tombstone().fields["retainUntilMs"], "150")

    def test_first_valid_decision_is_immutable_and_duplicate_is_idempotent(self):
        book = AtomicReservationBook(
            "/p", "boot", capacity=2, per_requester_limit=2,
            per_service_limit=2, max_lease_ms=100, committed_lease_ms=200)
        lease = reserve(book, "r")
        authority = ReservationDecisionAuthority(book)
        selected = SelectionDecision({"decision": "SELECTED",
                                      "reservationId": lease.fields["reservationId"]})
        first = authority.apply(selected, now_ms=2)
        duplicate = authority.apply(selected, now_ms=3)
        self.assertEqual(first.fields["decisionDigest"], duplicate.fields["decisionDigest"])
        conflicting = SelectionDecision({"decision": "NOT_SELECTED",
                                         "reservationId": lease.fields["reservationId"],
                                         "sequence": "999"})
        with self.assertRaisesRegex(RuntimeError, "conflicting immutable"):
            authority.apply(conflicting, now_ms=4)

    def test_invalid_ack_does_not_trigger_reflection_decision(self):
        book = AtomicReservationBook(
            "/p", "boot", capacity=1, per_requester_limit=1,
            per_service_limit=1, max_lease_ms=100, committed_lease_ms=200)
        coordinator = AckWindowDecisionCoordinator(
            "request", 1, ack_deadline_ms=50, retain_until_ms=150)
        self.assertIsNone(coordinator.admit(
            reserve(book, "r"), completed_at_ms=10, authenticated=False))
        self.assertEqual(coordinator.close(now_ms=50, selected_reservation_ids=set()), ())

    def test_lost_receipt_retry_is_exact_target_bounded_and_does_not_extend_lease(self):
        retry = BoundedExactTargetRetry(max_retries=2, deadline_ms=100)
        self.assertEqual(retry.record("/provider/a", now_ms=10), 1)
        self.assertEqual(retry.record("/provider/a", now_ms=20), 2)
        self.assertEqual(retry.record("/provider/a", now_ms=30), 3)
        with self.assertRaisesRegex(RuntimeError, "exhausted"):
            retry.record("/provider/a", now_ms=40)
        with self.assertRaisesRegex(RuntimeError, "exhausted"):
            retry.record("/provider/b", now_ms=100)
        self.assertEqual(retry.attempts, {"/provider/a": 3})
        self.assertEqual(retry.retry_attempts, 2)


if __name__ == "__main__":
    unittest.main()

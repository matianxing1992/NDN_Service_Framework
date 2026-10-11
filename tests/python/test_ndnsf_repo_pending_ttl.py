#!/usr/bin/env python3
"""Generic Core ACK retention survives retirement of Python Repo state."""

import unittest
from dataclasses import FrozenInstanceError

from ndnsf import AckDecision
from ndnsf.service import _to_native_ack


class RepoPendingStateTtlTest(unittest.TestCase):
    def test_default_ack_preserves_the_core_default_horizon(self):
        decision = AckDecision()
        self.assertEqual(decision.pending_state_ttl_ms, 0)
        self.assertEqual(_to_native_ack(decision).pending_state_ttl_ms, 0)

    def test_explicit_horizon_reaches_the_real_native_ack(self):
        decision = AckDecision(pending_state_ttl_ms=60_000)
        self.assertEqual(_to_native_ack(decision).pending_state_ttl_ms, 60_000)

    def test_negative_horizon_is_rejected_at_the_native_boundary(self):
        with self.assertRaisesRegex(ValueError, "pending_state_ttl_ms must be non-negative"):
            _to_native_ack(AckDecision(pending_state_ttl_ms=-1))

    def test_ack_decision_receives_ttl_at_construction(self):
        decision = AckDecision(
            status=True,
            message="repo-store-offer",
            pending_state_ttl_ms=60_000,
        )
        self.assertEqual(decision.pending_state_ttl_ms, 60_000)
        with self.assertRaises(FrozenInstanceError):
            decision.pending_state_ttl_ms = 1  # type: ignore[misc]


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Regression checks for long-running DistributedRepo collaborations."""

import unittest
from dataclasses import FrozenInstanceError

from ndnsf import AckDecision
from py_repoclient.network_artifact_backend import _repo_pending_state_ttl_ms


class RepoPendingStateTtlTest(unittest.TestCase):
    def test_small_artifacts_keep_the_default_horizon_bounded(self):
        self.assertEqual(_repo_pending_state_ttl_ms(0), 300_000)
        self.assertEqual(_repo_pending_state_ttl_ms(8 * 1024 * 1024), 301_000)

    def test_large_artifacts_get_a_transfer_aware_horizon(self):
        self.assertEqual(_repo_pending_state_ttl_ms(18_530_194_174), 2_508_972)

    def test_negative_sizes_are_safe(self):
        self.assertEqual(_repo_pending_state_ttl_ms(-1), 300_000)

    def test_ack_decision_receives_ttl_at_construction(self):
        decision = AckDecision(
            status=True,
            message="repo-store-offer",
            pending_state_ttl_ms=_repo_pending_state_ttl_ms(1 << 30),
        )
        self.assertGreater(decision.pending_state_ttl_ms, 300_000)
        with self.assertRaises(FrozenInstanceError):
            decision.pending_state_ttl_ms = 1  # type: ignore[misc]


if __name__ == "__main__":
    unittest.main()

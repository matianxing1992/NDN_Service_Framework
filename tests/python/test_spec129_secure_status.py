from __future__ import annotations

from dataclasses import replace
import json
import unittest

from ndnsf_distributed_inference.core import (
    BoundedStatusPoller, SecureStatusProvider, SecureStatusRequester,
    StatusHandleBinding, StatusQuery,
)


class SecureStatusTest(unittest.TestCase):
    def setUp(self):
        self.key = b"k" * 32
        self.binding = StatusHandleBinding.create(
            requester="/user", provider="/provider", request_id="request-1",
            attempt=1, selection_digest="sha256:selection",
            instance_id="instance-1", role="prefill",
            recipient_key_id="/user/KEY/1", expires_at_ms=10_000)
        self.provider = SecureStatusProvider(
            query_verifier=lambda query: query.signature == "sig:user",
            signer=lambda wire: "sig:" + str(len(wire)), max_events=3)
        self.provider.register(self.binding, self.key)
        self.requester = SecureStatusRequester(
            signature_verifier=lambda wire, signature: signature == "sig:" + str(len(wire)))

    def query(self, nonce="nonce-1"):
        return StatusQuery(self.binding.handle, "/user", "request-1", 1,
                           nonce, 900, 2_000, "sig:user")

    def test_transition_is_pull_only_and_authorized_query_is_confidential(self):
        self.provider.transition(self.binding.handle, "WARMING", 0.5, "warming",
                                 observed_at_ms=1_000)
        self.assertEqual(self.provider.query_count, 0)
        self.assertEqual(self.provider.unsolicited_status_count, 0)
        query = self.query()
        snapshot = self.provider.query(query, now_ms=1_000)
        wire = snapshot.nonce + snapshot.ciphertext
        self.assertNotIn(b"WARMING", wire)
        self.assertNotIn(b"request-1", wire)
        payload = self.requester.decrypt(snapshot, self.binding, query, self.key,
                                         now_ms=1_000)
        self.assertEqual(payload["state"], "WARMING")
        self.assertEqual(payload["sequence"], 1)

    def test_wrong_identity_replay_tamper_key_and_stale_sequence_fail_closed(self):
        self.provider.transition(self.binding.handle, "READY", 1.0, "",
                                 observed_at_ms=1_000)
        with self.assertRaisesRegex(ValueError, "authentication"):
            self.provider.query(replace(self.query(), requester="/other"), now_ms=1_000)
        query = self.query()
        snapshot = self.provider.query(query, now_ms=1_000)
        with self.assertRaisesRegex(ValueError, "replay"):
            self.provider.query(query, now_ms=1_000)
        with self.assertRaisesRegex(ValueError, "signature"):
            self.requester.decrypt(replace(snapshot, signature="bad"), self.binding,
                                   query, self.key, now_ms=1_000)
        with self.assertRaisesRegex(ValueError, "authentication"):
            self.requester.decrypt(snapshot, self.binding, query, b"x" * 32,
                                   now_ms=1_000)
        self.requester.decrypt(snapshot, self.binding, query, self.key, now_ms=1_000)
        with self.assertRaisesRegex(ValueError, "monotonic"):
            self.requester.decrypt(snapshot, self.binding, query, self.key, now_ms=1_000)

    def test_cursor_returns_strictly_newer_events_and_signals_retention_gap(self):
        for index in range(1, 6):
            self.provider.transition(self.binding.handle, "WARMING", index / 5,
                                     str(index), observed_at_ms=1_000 + index)
        events, gap = self.provider.events_after(self.binding.handle, 0)
        self.assertTrue(gap)
        self.assertEqual([item.sequence for item in events], [3, 4, 5])
        later, gap = self.provider.events_after(self.binding.handle, 4)
        self.assertFalse(gap)
        self.assertEqual([item.sequence for item in later], [5])

    def test_poller_is_opt_in_bounded_backoff_and_terminal_stop(self):
        poller = BoundedStatusPoller(initial_interval_ms=100, max_interval_ms=400,
                                    max_queries=3, deadline_ms=5_000)
        self.assertEqual([poller.admit_query(now_ms=1_000) for _ in range(3)],
                         [100, 200, 400])
        with self.assertRaisesRegex(RuntimeError, "exhausted"):
            poller.admit_query(now_ms=1_000)
        terminal = BoundedStatusPoller(deadline_ms=5_000)
        terminal.observe("COMPLETED")
        with self.assertRaisesRegex(RuntimeError, "stopped"):
            terminal.admit_query(now_ms=1_000)


if __name__ == "__main__":
    unittest.main()

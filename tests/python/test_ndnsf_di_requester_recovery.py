from __future__ import annotations

from dataclasses import replace
import unittest

from ndnsf_distributed_inference.core import ResultRendezvousRecord, ResultRendezvousStore


class RequesterRecoveryTest(unittest.TestCase):
    def record(self, **changes):
        base = ResultRendezvousRecord(
            "/u", "req", 1, "sha256:cert", 1, "sha256:terminal", True, 10_000)
        return replace(base, **changes)

    def test_same_identity_restart_resumes_durable_result(self) -> None:
        backing = {}
        first = ResultRendezvousStore(backing)
        record = first.publish(self.record())
        restarted = ResultRendezvousStore(backing)
        self.assertEqual(restarted.resume("/u", "req"), record)
        self.assertIsNone(restarted.resume("/other", "req"))

    def test_exactly_one_visible_terminal_result_and_idempotent_replay(self) -> None:
        store = ResultRendezvousStore()
        record = self.record()
        self.assertEqual(store.publish(record), record)
        self.assertEqual(store.publish(record), record)
        with self.assertRaisesRegex(ValueError, "already visible"):
            store.publish(self.record(terminal_digest="sha256:different"))

    def test_higher_attempt_fences_late_result(self) -> None:
        store = ResultRendezvousStore()
        store.fence_attempt("/u", "req", 2)
        with self.assertRaisesRegex(ValueError, "stale"):
            store.publish(self.record())
        current = self.record(attempt_epoch=2, output_epoch=2)
        self.assertEqual(store.publish(current), current)


if __name__ == "__main__":
    unittest.main()

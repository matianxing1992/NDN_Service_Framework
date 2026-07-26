from __future__ import annotations

from dataclasses import replace
import unittest

from ndnsf_distributed_inference.core import BoundStateStore, StateBinding


class CoreStateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.store = BoundStateStore()
        self.binding = StateBinding(
            "req-1", 1, "sha256:plan", "/p1", "boot-1", 7, 3, "session-1")

    def test_value_requires_exact_state_binding(self) -> None:
        self.store.put(self.binding, b"value")
        self.assertEqual(self.store.get(self.binding), b"value")
        for changed in (
            replace(self.binding, plan_digest="sha256:other"),
            replace(self.binding, provider_boot_epoch="boot-2"),
            replace(self.binding, security_epoch=8),
            replace(self.binding, cache_epoch=4),
        ):
            self.assertIsNone(self.store.get(changed))

    def test_attempt_fence_removes_old_state_and_rejects_regression(self) -> None:
        self.store.put(self.binding, b"old")
        self.assertEqual(self.store.fence_attempt("req-1", 2), 1)
        self.assertIsNone(self.store.get(self.binding))
        with self.assertRaisesRegex(ValueError, "stale attempt"):
            self.store.put(self.binding, b"stale")
        with self.assertRaisesRegex(ValueError, "regressed"):
            self.store.fence_attempt("req-1", 1)

    def test_provider_restart_invalidates_prior_boot_epoch_only(self) -> None:
        self.store.put(self.binding, b"old")
        current = replace(
            self.binding, attempt_epoch=2, provider_boot_epoch="boot-2",
            session_id="session-2")
        self.store.put(current, b"current")
        self.assertEqual(self.store.invalidate_provider_boot("/p1", "boot-2"), 1)
        self.assertEqual(self.store.get(current), b"current")


if __name__ == "__main__":
    unittest.main()

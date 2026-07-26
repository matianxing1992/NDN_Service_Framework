from __future__ import annotations

from dataclasses import replace
import unittest

from ndnsf_distributed_inference.app_sdk import APPDeploymentLifecycleStore
from ndnsf_distributed_inference.core import DeploymentLifecycleRecord


def record(epoch=1, owner="/owner", action="CREATE", digest="sha256:a",
           previous_epoch=0, previous_digest=""):
    return DeploymentLifecycleRecord(
        "dep", owner, epoch, "ACTIVE", f"sha256:state-{epoch}", action,
        digest, previous_epoch, previous_digest, {"/p1": "boot-1"})


class DeploymentFencingTest(unittest.TestCase):
    def test_monotonic_cas_and_idempotent_replay(self) -> None:
        store = APPDeploymentLifecycleStore()
        first = record()
        self.assertEqual(store.apply(first), first)
        self.assertEqual(store.apply(first), first)
        second = record(2, action="SCALE", digest="sha256:b", previous_epoch=1,
                        previous_digest=first.state_digest)
        self.assertEqual(store.apply(second), second)

    def test_conflicting_writer_and_stale_epoch_fail_closed(self) -> None:
        store = APPDeploymentLifecycleStore()
        first = store.apply(record())
        with self.assertRaisesRegex(ValueError, "owner or lifecycle CAS"):
            store.apply(record(2, owner="/intruder", action="SCALE", digest="sha256:b",
                               previous_epoch=1, previous_digest=first.state_digest))
        with self.assertRaisesRegex(ValueError, "owner or lifecycle CAS"):
            store.apply(record(3, action="SCALE", digest="sha256:c",
                               previous_epoch=1, previous_digest=first.state_digest))

    def test_destructive_partial_action_is_rejected(self) -> None:
        store = APPDeploymentLifecycleStore()
        first = store.apply(record())
        destructive = record(2, action="UNLOAD", digest="sha256:unload",
                             previous_epoch=1, previous_digest=first.state_digest)
        with self.assertRaisesRegex(ValueError, "complete action certificate"):
            store.apply(destructive, complete_provider_receipts=False)
        self.assertEqual(store.get("dep"), first)


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import unittest

from ndnsf.service import ServiceUser
from ndnsf_distributed_inference.deployment import DistributedLeaseTransaction


class ExecutionLeaseFallbackRemovalTest(unittest.TestCase):
    def test_generic_user_has_no_coordinator_or_local_execution_lease_authority(self) -> None:
        self.assertFalse(hasattr(ServiceUser, "acquire_execution_lease"))
        self.assertFalse(hasattr(ServiceUser, "release_execution_lease"))
        self.assertFalse(hasattr(ServiceUser, "evict_deployment"))

    def test_di_uses_provider_transaction_instead_of_granted_local(self) -> None:
        self.assertTrue(callable(DistributedLeaseTransaction.acquire))


if __name__ == "__main__":
    unittest.main()

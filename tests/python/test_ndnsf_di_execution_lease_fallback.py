from __future__ import annotations

import unittest
from unittest import mock

import ndnsf.service as service_module
from ndnsf.service import ServiceUser


class ExecutionLeaseFallbackBaselineTest(unittest.TestCase):
    def setUp(self) -> None:
        self.user = ServiceUser.__new__(ServiceUser)
        self.user.user = "/NDNSF-DI/User/baseline"

    def test_missing_coordination_service_name_currently_fails_before_request(self) -> None:
        with self.assertRaisesRegex(NameError, "COORDINATION_ADVISORY_SERVICE"):
            self.user.acquire_execution_lease("deployment-1", ttl_ms=2500)

    @mock.patch(
        "ndnsf.coordination.CoordinationServiceClient.request",
        side_effect=RuntimeError("coordinator unavailable"),
    )
    def test_coordinator_failure_reaches_missing_execution_lease_symbol(
        self, _request: mock.Mock
    ) -> None:
        with mock.patch.object(
            service_module,
            "COORDINATION_ADVISORY_SERVICE",
            "/NDNSF/Coordination/Advisory",
            create=True,
        ):
            with self.assertRaisesRegex(ImportError, "ExecutionLease"):
                self.user.acquire_execution_lease("deployment-2")


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import unittest

import ndnsf
from ndnsf import _ndnsf


def make_lease(request_id: str = "request-1"):
    lease = ndnsf.GenericExecutionLease()
    lease.provider_name = "/provider/A"
    lease.requester_name = "/user/A"
    lease.request_id = request_id
    lease.service_name = "/Inference/NativeTracer"
    lease.plan_digest = "plan-sha256"
    lease.resource_binding_schema = "ndnsf-di-binding-v1"
    lease.resource_binding_proof = b"role=/Backbone;fragment=f1"
    lease.conflict_keys = ["compute-slot:0"]
    lease.expires_at_ms = 5000
    lease.idempotency_key = f"prepare-{request_id}"
    return lease


def make_binding():
    binding = ndnsf.ExecutionLeaseBinding()
    binding.requester_name = "/user/A"
    binding.request_id = "request-1"
    binding.service_name = "/Inference/NativeTracer"
    binding.plan_digest = "plan-sha256"
    binding.resource_binding_schema = "ndnsf-di-binding-v1"
    binding.resource_binding_proof = b"role=/Backbone;fragment=f1"
    return binding


class ProviderExecutionLeaseTableBindingTest(unittest.TestCase):
    def test_public_python_names_are_the_bound_cpp_types(self) -> None:
        self.assertIs(ndnsf.GenericExecutionLease, _ndnsf.GenericExecutionLease)
        self.assertIs(
            ndnsf.ProviderExecutionLeaseTable,
            _ndnsf.ProviderExecutionLeaseTable,
        )

    def test_bound_table_runs_prepare_commit_activate_release(self) -> None:
        table = ndnsf.ProviderExecutionLeaseTable("epoch-A")
        prepared = table.prepare(make_lease(), 1000)
        self.assertTrue(prepared.status)
        self.assertEqual(prepared.lease.state, ndnsf.ExecutionLeaseState.PREPARED)

        committed = table.commit(
            prepared.lease.lease_id, "epoch-A", "/user/A", "commit-1", 1100
        )
        self.assertTrue(committed.status)
        self.assertTrue(
            table.has_pinned_binding_proof(b"role=/Backbone;fragment=f1", 1100)
        )
        activated = table.validate_and_activate(
            prepared.lease.lease_id,
            "epoch-A",
            make_binding(),
            "activate-1",
            1200,
            10000,
        )
        self.assertTrue(activated.status)
        self.assertEqual(activated.lease.state, ndnsf.ExecutionLeaseState.EXECUTING)
        released = table.release(
            prepared.lease.lease_id, "epoch-A", "/user/A", "release-1", 1300
        )
        self.assertTrue(released.status)
        self.assertEqual(released.lease.state, ndnsf.ExecutionLeaseState.RELEASED)
        self.assertFalse(
            table.has_pinned_binding_proof(b"role=/Backbone;fragment=f1", 1300)
        )

    def test_bound_table_rejects_conflict_and_stale_epoch(self) -> None:
        table = ndnsf.ProviderExecutionLeaseTable("epoch-A")
        first = table.prepare(make_lease(), 1000)
        self.assertTrue(first.status)
        conflict = table.prepare(make_lease("request-2"), 1100)
        self.assertFalse(conflict.status)
        self.assertEqual(conflict.reason_code, "LEASE_CAPACITY_REJECTED")

        wrong_requester = table.commit(
            first.lease.lease_id, "epoch-A", "/user/other", "commit-other", 1150
        )
        self.assertFalse(wrong_requester.status)
        self.assertEqual(wrong_requester.reason_code, "LEASE_REQUESTER_MISMATCH")

        stale = table.commit(
            first.lease.lease_id, "epoch-old", "/user/A", "commit-1", 1200
        )
        self.assertFalse(stale.status)
        self.assertEqual(stale.reason_code, "LEASE_STALE_EPOCH")


if __name__ == "__main__":
    unittest.main()

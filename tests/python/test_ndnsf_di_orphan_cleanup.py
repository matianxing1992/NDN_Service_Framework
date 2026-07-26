from __future__ import annotations

import unittest
import time

from ndnsf_distributed_inference.core import OrphanResourceRegistry
from ndnsf_distributed_inference.deployment import PythonExecutionLeaseProviderAdapter


class OrphanCleanupTest(unittest.TestCase):
    def test_periodic_sweep_reclaims_all_categories_without_new_traffic(self) -> None:
        registry = OrphanResourceRegistry("/p", "boot-1", tombstone_ttl_ms=500)
        for kind in registry.RESOURCE_KINDS:
            registry.retain(kind, f"{kind}-1", expires_at_ms=1_000,
                            request_id="req", attempt_epoch=1)
        cleaned = []
        result = registry.sweep(
            at_ms=1_001, sweep_id="timer-1",
            cleanup=lambda kind, resource: cleaned.append((kind, resource)))
        self.assertEqual(len(cleaned), len(registry.RESOURCE_KINDS))
        self.assertTrue(all(result.reclaimed[kind] for kind in registry.RESOURCE_KINDS))
        self.assertEqual(result.tombstone_high_watermarks["req"], 1)

    def test_stale_operation_after_cleanup_is_fenced(self) -> None:
        registry = OrphanResourceRegistry("/p", "boot-1")
        registry.retain("leases", "l1", expires_at_ms=1_000,
                        request_id="req", attempt_epoch=2)
        registry.sweep(at_ms=1_001, sweep_id="timer-1")
        with self.assertRaisesRegex(ValueError, "stale operation"):
            registry.retain("leases", "late", expires_at_ms=2_000,
                            request_id="req", attempt_epoch=1)

    def test_provider_restart_reclaims_prior_boot_resources(self) -> None:
        registry = OrphanResourceRegistry("/p", "boot-1")
        registry.retain("sessions", "s1", expires_at_ms=9_000,
                        request_id="req", attempt_epoch=1)
        result = registry.restart("boot-2", at_ms=2_000, sweep_id="restart-1")
        self.assertEqual(result.provider_boot_epoch, "boot-2")
        self.assertEqual(result.reclaimed["sessions"], ("s1",))

    def test_canonical_lease_table_cleanup_runs_without_operation_entry(self) -> None:
        clock = [1_000]
        adapter = PythonExecutionLeaseProviderAdapter(
            "/p", "/Inference/Test", lambda *_: ("slot:1",),
            provider_epoch="boot-1", cleanup_interval_ms=5)
        lease = __import__("ndnsf").GenericExecutionLease()
        lease.provider_name = "/p"
        lease.requester_name = "/u"
        lease.request_id = "req"
        lease.service_name = "/Inference/Test"
        lease.plan_digest = "sha256:plan"
        lease.resource_binding_schema = "ndnsf-di-binding-v1"
        lease.resource_binding_proof = b"binding"
        lease.conflict_keys = ["slot:1"]
        lease.expires_at_ms = 1_010
        lease.idempotency_key = "prepare:req"
        prepared = adapter.table.prepare(lease, clock[0])
        adapter.start_periodic_cleanup(lambda: clock[0])
        clock[0] = 1_011
        deadline = time.monotonic() + 0.5
        while adapter.table.find(prepared.lease.lease_id).state.name != "EXPIRED":
            if time.monotonic() >= deadline:
                self.fail("periodic lease cleanup did not run")
            time.sleep(0.005)
        adapter.close()


if __name__ == "__main__":
    unittest.main()

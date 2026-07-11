from __future__ import annotations

import concurrent.futures
import threading
import time
import unittest

from ndnsf_distributed_inference.deployment import (
    DistributedLeaseTransaction,
    LeaseOperationRequest,
    LeaseTransactionError,
    ProviderLeaseAssignment,
    PythonExecutionLeaseProviderAdapter,
)


class AdapterTransport:
    def __init__(self, adapter: PythonExecutionLeaseProviderAdapter) -> None:
        self.adapter = adapter
        self._lock = threading.Lock()
        self._wire_request = 0
        self._now_ms = 1000

    def request(self, provider: str, payload: bytes) -> bytes:
        with self._lock:
            self._wire_request += 1
            self._now_ms += 1
            wire_request = self._wire_request
            now_ms = self._now_ms
        return self.adapter.handle(
            {
                "requesterIdentity": "/user/stress",
                "providerName": provider,
                "serviceName": "/Inference/Control/Lease",
                "requestId": f"wire-{wire_request}",
            },
            payload,
            now_ms,
        )


class ExecutionLeaseStressTest(unittest.TestCase):
    def test_one_thousand_transactions_never_overlap_conflict_keys(self) -> None:
        holder: dict[str, PythonExecutionLeaseProviderAdapter] = {}

        def choose_slot(_request: LeaseOperationRequest, _context):
            adapter = holder["adapter"]
            for slot in range(8):
                key = f"compute-slot:{slot}"
                if not adapter.table.has_active_conflict_key(key, 0):
                    return (key,)
            return ()

        adapter = PythonExecutionLeaseProviderAdapter(
            "/provider/stress",
            "/Inference/NativeTracer",
            choose_slot,
            provider_epoch="epoch-stress",
        )
        holder["adapter"] = adapter
        transport = AdapterTransport(adapter)
        active_keys: set[str] = set()
        active_lock = threading.Lock()
        conflicts: list[str] = []

        def run_transaction(index: int) -> None:
            request_id = f"stress-{index}"
            transaction = DistributedLeaseTransaction(transport)
            while True:
                try:
                    transaction.run(
                        request_id=request_id,
                        plan_digest="plan-stress",
                        service_name="/Inference/NativeTracer",
                        assignments=(
                            ProviderLeaseAssignment(
                                "/provider/stress",
                                ("/Stage",),
                                f"proof-{index}".encode(),
                            ),
                        ),
                        expires_at_ms=1_000_000,
                        execute=lambda lease_set: exercise_slot(
                            lease_set.leases[0].conflict_keys[0]
                        ),
                    )
                    return
                except LeaseTransactionError as exc:
                    if exc.response.reason_code != "LEASE_CAPACITY_REJECTED":
                        raise
                    time.sleep(0.0001)

        def exercise_slot(key: str) -> None:
            with active_lock:
                if key in active_keys:
                    conflicts.append(key)
                active_keys.add(key)
            time.sleep(0.00005)
            with active_lock:
                active_keys.remove(key)

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            list(pool.map(run_transaction, range(1000)))

        counters = adapter.table.counters(1_000_000)
        self.assertEqual(conflicts, [])
        self.assertEqual(active_keys, set())
        self.assertEqual(counters.prepared, 1000)
        self.assertEqual(counters.committed, 1000)
        self.assertEqual(counters.released, 1000)
        for slot in range(8):
            self.assertFalse(
                adapter.table.has_active_conflict_key(
                    f"compute-slot:{slot}", 1_000_000
                )
            )


if __name__ == "__main__":
    unittest.main()

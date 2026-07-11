from __future__ import annotations

import unittest

from ndnsf_distributed_inference.deployment import (
    DistributedLeaseTransaction,
    LeaseOperation,
    LeaseOperationRequest,
    LeaseOperationResponse,
    LeaseTransactionError,
    NdnsfLeaseTransport,
    ProviderLeaseAssignment,
)


class FakeLeaseTransport:
    def __init__(self, fail_phase: tuple[str, LeaseOperation] | None = None):
        self.fail_phase = fail_phase
        self.calls: list[tuple[str, LeaseOperationRequest]] = []

    def request(self, provider: str, payload: bytes) -> bytes:
        request = LeaseOperationRequest.from_bytes(payload)
        self.calls.append((provider, request))
        if self.fail_phase == (provider, request.operation):
            return LeaseOperationResponse(
                False, request.operation, "LEASE_CAPACITY_REJECTED"
            ).to_bytes()
        return LeaseOperationResponse(
            True,
            request.operation,
            "OK",
            lease_id=request.lease_id or f"lease:{provider}",
            provider_epoch=request.provider_epoch or f"epoch:{provider}",
            state=("PREPARED" if request.operation is LeaseOperation.PREPARE else "COMMITTED"),
            expires_at_ms=request.expires_at_ms or 5000,
            conflict_keys=(f"slot:{provider}",),
        ).to_bytes()


def assignments() -> tuple[ProviderLeaseAssignment, ...]:
    return (
        ProviderLeaseAssignment("/provider/A", ("/Backbone",), b"binding-A"),
        ProviderLeaseAssignment("/provider/B", ("/Merge",), b"binding-B"),
    )


class DistributedLeaseTransactionTest(unittest.TestCase):
    def test_ndnsf_transport_retries_idempotent_wire_operation(self) -> None:
        class Response:
            def __init__(self, status, payload=b"", error=""):
                self.status = status
                self.payload = payload
                self.error = error

        class User:
            def __init__(self):
                self.calls = 0

            def request_service_targeted(self, *_args, **_kwargs):
                self.calls += 1
                if self.calls < 3:
                    return Response(False, error="transient timeout")
                return Response(True, payload=b"lease-response")

        user = User()
        payload = NdnsfLeaseTransport(user, retries=2).request(
            "/provider/A", b"lease-request"
        )
        self.assertEqual(payload, b"lease-response")
        self.assertEqual(user.calls, 3)

    def test_capacity_rejection_retries_with_fresh_idempotency_key(self) -> None:
        class CapacityOnceTransport(FakeLeaseTransport):
            def __init__(self):
                super().__init__()
                self.rejected = False

            def request(self, provider: str, payload: bytes) -> bytes:
                request = LeaseOperationRequest.from_bytes(payload)
                if (
                    provider == "/provider/A"
                    and request.operation is LeaseOperation.PREPARE
                    and not self.rejected
                ):
                    self.rejected = True
                    self.calls.append((provider, request))
                    return LeaseOperationResponse(
                        False,
                        request.operation,
                        "LEASE_CAPACITY_REJECTED",
                        retry_after_ms=1,
                    ).to_bytes()
                return super().request(provider, payload)

        transport = CapacityOnceTransport()
        lease_set = DistributedLeaseTransaction(transport).acquire(
            request_id="request-retry",
            plan_digest="plan-1",
            service_name="/Inference/NativeTracer",
            assignments=assignments(),
            expires_at_ms=5000,
            capacity_wait_ms=100,
            capacity_poll_ms=1,
            reservation_ttl_ms=1000,
        )
        prepare_keys = [
            request.idempotency_key
            for _, request in transport.calls
            if request.operation is LeaseOperation.PREPARE
            and request.request_id == "request-retry"
        ]
        self.assertGreaterEqual(len(prepare_keys), 3)
        self.assertNotEqual(prepare_keys[0], prepare_keys[1])
        prepare_expiries = [
            request.expires_at_ms
            for _, request in transport.calls
            if request.operation is LeaseOperation.PREPARE
            and request.request_id == "request-retry"
        ]
        self.assertGreater(prepare_expiries[-1], 5000)
        DistributedLeaseTransaction(transport).release(lease_set)

    def test_execute_runs_only_after_every_provider_commits(self) -> None:
        transport = FakeLeaseTransport()
        observed = []
        result = DistributedLeaseTransaction(transport).run(
            request_id="request-1",
            plan_digest="plan-1",
            service_name="/Inference/NativeTracer",
            assignments=assignments(),
            expires_at_ms=5000,
            execute=lambda lease_set: observed.append(lease_set) or "executed",
        )
        self.assertEqual(result, "executed")
        self.assertEqual(len(observed[0].leases), 2)
        self.assertEqual(
            [request.operation for _, request in transport.calls[:4]],
            [
                LeaseOperation.PREPARE,
                LeaseOperation.PREPARE,
                LeaseOperation.COMMIT,
                LeaseOperation.COMMIT,
            ],
        )
        self.assertEqual(
            [request.operation for _, request in transport.calls[4:]],
            [LeaseOperation.RELEASE, LeaseOperation.RELEASE],
        )

    def test_prepare_rejection_aborts_prior_prepares_and_never_executes(self) -> None:
        transport = FakeLeaseTransport(("/provider/B", LeaseOperation.PREPARE))
        executed = []
        with self.assertRaises(LeaseTransactionError):
            DistributedLeaseTransaction(transport).run(
                request_id="request-1",
                plan_digest="plan-1",
                service_name="/Inference/NativeTracer",
                assignments=assignments(),
                expires_at_ms=5000,
                execute=lambda lease_set: executed.append(lease_set),
            )
        self.assertEqual(executed, [])
        self.assertEqual(transport.calls[-1][1].operation, LeaseOperation.ABORT)
        self.assertEqual(transport.calls[-1][0], "/provider/A")

    def test_partial_commit_releases_commit_and_aborts_remaining_prepare(self) -> None:
        transport = FakeLeaseTransport(("/provider/B", LeaseOperation.COMMIT))
        with self.assertRaises(LeaseTransactionError):
            DistributedLeaseTransaction(transport).acquire(
                request_id="request-1",
                plan_digest="plan-1",
                service_name="/Inference/NativeTracer",
                assignments=assignments(),
                expires_at_ms=5000,
            )
        cleanup = [(provider, request.operation) for provider, request in transport.calls[-2:]]
        self.assertEqual(
            cleanup,
            [
                ("/provider/A", LeaseOperation.RELEASE),
                ("/provider/B", LeaseOperation.ABORT),
            ],
        )

    def test_delayed_response_for_another_operation_is_rejected(self) -> None:
        class DelayedResponseTransport(FakeLeaseTransport):
            def request(self, provider: str, payload: bytes) -> bytes:
                request = LeaseOperationRequest.from_bytes(payload)
                return LeaseOperationResponse(
                    True,
                    LeaseOperation.COMMIT,
                    "OK",
                    lease_id="late-lease",
                    provider_epoch="epoch-A",
                ).to_bytes()

        with self.assertRaisesRegex(ValueError, "operation does not match"):
            DistributedLeaseTransaction(DelayedResponseTransport()).acquire(
                request_id="request-1",
                plan_digest="plan-1",
                service_name="/Inference/NativeTracer",
                assignments=assignments(),
                expires_at_ms=5000,
            )


if __name__ == "__main__":
    unittest.main()

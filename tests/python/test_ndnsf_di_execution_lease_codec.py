from __future__ import annotations

import unittest

from ndnsf_distributed_inference.deployment import (
    LeaseOperation,
    LeaseOperationRequest,
    LeaseOperationResponse,
)


class ExecutionLeaseCodecTest(unittest.TestCase):
    def test_request_round_trip_is_deterministic_and_binary_safe(self) -> None:
        request = LeaseOperationRequest(
            operation=LeaseOperation.PREPARE,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="prepare-1",
            target_service_name="/Inference/NativeTracer",
            resource_binding_proof=b"\x00binding\xff",
            roles=("/Backbone",),
            expires_at_ms=5000,
        )
        wire = request.to_bytes()
        self.assertEqual(LeaseOperationRequest.from_bytes(wire), request)
        self.assertEqual(LeaseOperationRequest.from_bytes(wire).to_bytes(), wire)

    def test_response_round_trip_carries_typed_failure(self) -> None:
        response = LeaseOperationResponse(
            False,
            LeaseOperation.COMMIT,
            "LEASE_STALE_EPOCH",
            lease_id="lease-1",
            provider_epoch="epoch-old",
            state="PREPARED",
        )
        self.assertEqual(
            LeaseOperationResponse.from_bytes(response.to_bytes()), response
        )

    def test_malformed_and_unknown_schema_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            LeaseOperationRequest.from_bytes(b"not-json")
        with self.assertRaises(ValueError):
            LeaseOperationRequest.from_bytes(b'{"schema":"future-v2"}')


if __name__ == "__main__":
    unittest.main()

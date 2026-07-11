from __future__ import annotations

import unittest

from ndnsf import ExecutionLeaseBinding, ExecutionLeaseState

from ndnsf_distributed_inference.deployment import (
    LEASE_SERVICE_NAME,
    LeaseOperation,
    LeaseOperationRequest,
    LeaseOperationResponse,
    PythonExecutionLeaseProviderAdapter,
)
from ndnsf_distributed_inference.runtime_v1 import (
    ModelFragmentKey,
    ProviderFragmentInventoryManager,
)


def context() -> dict[str, str]:
    return {
        "requesterIdentity": "/user/A",
        "providerName": "/provider/A",
        "serviceName": LEASE_SERVICE_NAME,
        "requestId": "request-1",
    }


def prepare(adapter: PythonExecutionLeaseProviderAdapter) -> LeaseOperationResponse:
    request = LeaseOperationRequest(
        operation=LeaseOperation.PREPARE,
        request_id="request-1",
        plan_digest="plan-1",
        idempotency_key="prepare-1",
        target_service_name="/Inference/NativeTracer",
        resource_binding_proof=b"binding-1",
        roles=("/Backbone",),
        expires_at_ms=5000,
    )
    return LeaseOperationResponse.from_bytes(adapter.handle(context(), request.to_bytes(), 1000))


class ExecutionLeaseRestartTest(unittest.TestCase):
    def make_adapter(self, epoch: str) -> PythonExecutionLeaseProviderAdapter:
        return PythonExecutionLeaseProviderAdapter(
            "/provider/A",
            "/Inference/NativeTracer",
            lambda _request, _context: ("compute-slot:0",),
            provider_epoch=epoch,
        )

    def test_restart_rejects_delayed_old_epoch_message(self) -> None:
        old = self.make_adapter("epoch-old")
        prepared = prepare(old)
        self.assertTrue(prepared.status)

        restarted = self.make_adapter("epoch-new")
        delayed_commit = LeaseOperationRequest(
            operation=LeaseOperation.COMMIT,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="commit-1",
            target_service_name="/Inference/NativeTracer",
            lease_id=prepared.lease_id,
            provider_epoch=prepared.provider_epoch,
        )
        response = LeaseOperationResponse.from_bytes(
            restarted.handle(context(), delayed_commit.to_bytes(), 1200)
        )
        self.assertFalse(response.status)
        self.assertEqual(response.reason_code, "LEASE_STALE_EPOCH")

    def test_committed_binding_is_pinned_until_expiry(self) -> None:
        adapter = self.make_adapter("epoch-A")
        prepared = prepare(adapter)
        commit = LeaseOperationRequest(
            operation=LeaseOperation.COMMIT,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="commit-1",
            target_service_name="/Inference/NativeTracer",
            lease_id=prepared.lease_id,
            provider_epoch=prepared.provider_epoch,
        )
        committed = LeaseOperationResponse.from_bytes(
            adapter.handle(context(), commit.to_bytes(), 1100)
        )
        self.assertTrue(committed.status)
        self.assertTrue(adapter.table.has_pinned_binding_proof(b"binding-1", 4999))
        self.assertFalse(adapter.table.has_pinned_binding_proof(b"binding-1", 5000))

    def test_payload_identity_cannot_override_authenticated_context(self) -> None:
        adapter = self.make_adapter("epoch-A")
        bad_context = context()
        bad_context["requesterIdentity"] = ""
        request = LeaseOperationRequest(
            operation=LeaseOperation.PREPARE,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="prepare-1",
            target_service_name="/Inference/NativeTracer",
            resource_binding_proof=b"binding-1",
            roles=("/Backbone",),
            expires_at_ms=5000,
        )
        response = LeaseOperationResponse.from_bytes(
            adapter.handle(bad_context, request.to_bytes(), 1000)
        )
        self.assertFalse(response.status)
        self.assertEqual(response.reason_code, "LEASE_BINDING_MISMATCH")

    def test_renewal_replay_and_release_loss_are_idempotent(self) -> None:
        adapter = self.make_adapter("epoch-A")
        prepared = prepare(adapter)
        commit = LeaseOperationRequest(
            operation=LeaseOperation.COMMIT,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="commit-1",
            target_service_name="/Inference/NativeTracer",
            lease_id=prepared.lease_id,
            provider_epoch=prepared.provider_epoch,
        )
        self.assertTrue(
            LeaseOperationResponse.from_bytes(
                adapter.handle(context(), commit.to_bytes(), 1100)
            ).status
        )
        renew = LeaseOperationRequest(
            operation=LeaseOperation.RENEW,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="renew-1",
            target_service_name="/Inference/NativeTracer",
            lease_id=prepared.lease_id,
            provider_epoch=prepared.provider_epoch,
            expires_at_ms=7000,
        )
        first_renew = adapter.handle(context(), renew.to_bytes(), 1200)
        replayed_renew = adapter.handle(context(), renew.to_bytes(), 1201)
        self.assertEqual(first_renew, replayed_renew)

        release = LeaseOperationRequest(
            operation=LeaseOperation.RELEASE,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="release-1",
            target_service_name="/Inference/NativeTracer",
            lease_id=prepared.lease_id,
            provider_epoch=prepared.provider_epoch,
        )
        first_release = adapter.handle(context(), release.to_bytes(), 1300)
        replayed_after_lost_response = adapter.handle(
            context(), release.to_bytes(), 1301
        )
        self.assertEqual(first_release, replayed_after_lost_response)

    def test_executing_binding_survives_ordinary_expiry_until_hard_deadline(self) -> None:
        adapter = self.make_adapter("epoch-A")
        prepared = prepare(adapter)
        commit = LeaseOperationRequest(
            operation=LeaseOperation.COMMIT,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="commit-1",
            target_service_name="/Inference/NativeTracer",
            lease_id=prepared.lease_id,
            provider_epoch=prepared.provider_epoch,
        )
        adapter.handle(context(), commit.to_bytes(), 1100)
        binding = ExecutionLeaseBinding()
        binding.requester_name = "/user/A"
        binding.request_id = "request-1"
        binding.service_name = "/Inference/NativeTracer"
        binding.plan_digest = "plan-1"
        binding.resource_binding_schema = "ndnsf-di-binding-v1"
        binding.resource_binding_proof = b"binding-1"
        activated = adapter.table.validate_and_activate(
            prepared.lease_id,
            prepared.provider_epoch,
            binding,
            "activate:request-1",
            1200,
            8000,
        )
        self.assertTrue(activated.status)
        self.assertTrue(adapter.table.has_pinned_binding_proof(b"binding-1", 6000))
        adapter.table.cleanup_expired(8000)
        expired = adapter.table.find(prepared.lease_id)
        self.assertEqual(expired.state, ExecutionLeaseState.EXPIRED)

    def test_fragment_eviction_is_blocked_while_lease_binding_is_pinned(self) -> None:
        adapter = self.make_adapter("epoch-A")
        prepared = prepare(adapter)
        commit = LeaseOperationRequest(
            operation=LeaseOperation.COMMIT,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="commit-1",
            target_service_name="/Inference/NativeTracer",
            lease_id=prepared.lease_id,
            provider_epoch=prepared.provider_epoch,
        )
        adapter.handle(context(), commit.to_bytes(), 1100)
        fragment = ModelFragmentKey(model_id="qwen", fragment_digest="fragment-1")
        manager = ProviderFragmentInventoryManager(
            "/provider/A",
            lease_pin_checker=lambda proof, _now: (
                adapter.table.has_pinned_binding_proof(proof, 1200)
            ),
        )
        manager.register_fragment(
            fragment,
            lease_binding_proof=b"binding-1",
        )
        manager.mark_gpu_loaded(fragment)
        with self.assertRaisesRegex(RuntimeError, "LEASE_BINDING_PINNED"):
            manager.evict(fragment)

        release = LeaseOperationRequest(
            operation=LeaseOperation.RELEASE,
            request_id="request-1",
            plan_digest="plan-1",
            idempotency_key="release-1",
            target_service_name="/Inference/NativeTracer",
            lease_id=prepared.lease_id,
            provider_epoch=prepared.provider_epoch,
        )
        adapter.handle(context(), release.to_bytes(), 1200)
        manager.evict(fragment)
        self.assertFalse(manager._entry_for(fragment).gpu_loaded)


if __name__ == "__main__":
    unittest.main()

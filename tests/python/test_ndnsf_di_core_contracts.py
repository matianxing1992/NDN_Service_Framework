from __future__ import annotations

import json
import unittest

from ndnsf_distributed_inference.core import (
    CoreAssignment,
    CoreExecutionEvidence,
    CoreExecutionPlan,
    CorePlanDependency,
    DeploymentLifecycleRecord,
    ExecutionActivateMessage,
    OrphanCleanupRecord,
    ProviderAssignment,
    ReadySetMember,
    RequestCoordinatorBinding,
    ResultRendezvousRecord,
    EncryptedRequestInput, RequestCapabilities, ReservationLease,
    SelectionDecision, SelectionInputKeyGrant, SelectionInputKeyOffer,
    validate_r1_capability_combination,
)


def make_plan() -> CoreExecutionPlan:
    return CoreExecutionPlan(
        "plan-1", "model-1", ("prefill", "decode"),
        (CorePlanDependency("prefill", "decode", "kv", "sha256:kv"),),
        {"weights": "sha256:weights"}, "r1")


def make_binding(plan_digest: str, assignment_digest: str) -> RequestCoordinatorBinding:
    return RequestCoordinatorBinding(
        "/requester", "req-1", 1, "sha256:intent", "sha256:objective",
        "sha256:snapshot", plan_digest, assignment_digest, 10_000,
        "/requester/results/req-1")


class CoreContractsTest(unittest.TestCase):
    def test_r1_field_contracts_round_trip_and_four_capability_combinations(self) -> None:
        contracts = (
            RequestCapabilities({"SelectionGatedInputV1": "required"}),
            EncryptedRequestInput({"ciphertext": "opaque", "keyId": "input-1"}),
            SelectionInputKeyOffer({"certificateDigest": "sha256:cert"}),
            SelectionInputKeyGrant({"wrappedInputKey": "opaque-key"}),
            ReservationLease({"reservationId": "r-1", "expiresAtMs": "10"}),
            SelectionDecision({"decision": "SELECTED", "reservationId": "r-1"}),
        )
        for contract in contracts:
            self.assertEqual(type(contract).from_bytes(contract.to_bytes()), contract)
        for di, gated in ((False, False), (False, True), (True, False), (True, True)):
            validate_r1_capability_combination(
                di_reservation=di, gated_input=gated,
                has_deployment_intent=di, targeted_fast_path=False)
        with self.assertRaisesRegex(ValueError, "requires DeploymentIntent"):
            validate_r1_capability_combination(
                di_reservation=True, gated_input=False,
                has_deployment_intent=False, targeted_fast_path=False)
        with self.assertRaisesRegex(ValueError, "requires ACK/Selection"):
            validate_r1_capability_combination(
                di_reservation=False, gated_input=True,
                has_deployment_intent=False, targeted_fast_path=True)

    def test_r1_field_contract_bounds_fail_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "exceeds bounds"):
            RequestCapabilities({"x" * 65: "required"})

    def test_plan_assignment_and_evidence_round_trip(self) -> None:
        plan = make_plan()
        self.assertEqual(CoreExecutionPlan.from_bytes(plan.to_bytes()), plan)
        members = (
            ProviderAssignment("prefill", "/p1", "boot-1", "lease-1", "sha256:r1"),
            ProviderAssignment("decode", "/p2", "boot-2", "lease-2", "sha256:r2"),
        )
        assignment = CoreAssignment("a-1", "req-1", 1, plan.digest(), members)
        self.assertEqual(CoreAssignment.from_bytes(assignment.to_bytes()), assignment)
        evidence = CoreExecutionEvidence(
            "/p1", "boot-1", "req-1", 1, plan.digest(), assignment.digest(),
            "native:test", "sha256:result")
        self.assertEqual(CoreExecutionEvidence.from_bytes(evidence.to_bytes()), evidence)

    def test_consistency_contracts_round_trip_and_stable_digest(self) -> None:
        plan = make_plan()
        members = (
            ProviderAssignment("prefill", "/p1", "boot-1", "lease-1", "sha256:r1"),
            ProviderAssignment("decode", "/p2", "boot-2", "lease-2", "sha256:r2"),
        )
        assignment = CoreAssignment("a-1", "req-1", 1, plan.digest(), members)
        binding = make_binding(plan.digest(), assignment.digest())
        activation = ExecutionActivateMessage(
            binding.requester_identity, binding.request_id,
            binding.attempt_epoch, "sha256:selection", binding.plan_digest,
            tuple(ReadySetMember(
                member.provider, member.role, member.provider_boot_epoch,
                f"sha256:ready-{member.role}") for member in members),
            binding.execution_deadline_ms, 1, "requester-signature")
        decoded = ExecutionActivateMessage.from_bytes(activation.to_bytes())
        self.assertEqual(decoded, activation)
        self.assertEqual(decoded.digest(), activation.digest())

        lifecycle = DeploymentLifecycleRecord(
            "dep-1", "/owner", 2, "ACTIVE", "sha256:state", "SCALE",
            "sha256:action", 1, "sha256:old", {"/p1": "boot-1"})
        self.assertEqual(DeploymentLifecycleRecord.from_bytes(lifecycle.to_bytes()), lifecycle)
        cleanup = OrphanCleanupRecord(
            "/p1", "boot-1", "sweep-1", 1_000,
            {"leases": ("lease-1",)}, {"req-1": 2}, 2_000, 1_500)
        self.assertEqual(OrphanCleanupRecord.from_bytes(cleanup.to_bytes()), cleanup)
        result = ResultRendezvousRecord(
            "/requester", "req-1", 1, activation.digest(), 1,
            "sha256:terminal", True, 5_000)
        self.assertEqual(ResultRendezvousRecord.from_bytes(result.to_bytes()), result)

    def test_unknown_or_malformed_schema_fails_closed(self) -> None:
        plan = make_plan()
        payload = json.loads(plan.to_bytes())
        payload["schema"] = "ndnsf-di-core-execution-plan-v999"
        with self.assertRaisesRegex(ValueError, "unsupported contract schema"):
            CoreExecutionPlan.from_bytes(json.dumps(payload).encode())
        with self.assertRaisesRegex(ValueError, "malformed canonical contract"):
            CoreExecutionPlan.from_bytes(b"not-json")

    def test_activation_rejects_empty_and_duplicate_ready_members(self) -> None:
        plan = make_plan()
        members = (
            ProviderAssignment("prefill", "/p1", "boot-1", "lease-1", "sha256:r1"),
            ProviderAssignment("decode", "/p2", "boot-2", "lease-2", "sha256:r2"),
        )
        first = ReadySetMember("/p1", "prefill", "boot-1", "sha256:ready")
        with self.assertRaisesRegex(ValueError, "invalid ExecutionActivateMessage"):
            ExecutionActivateMessage(
                "/requester", "req-1", 1, "sha256:selection",
                plan.digest(), (), 10_000, 1, "signature")
        with self.assertRaisesRegex(ValueError, "invalid ExecutionActivateMessage"):
            ExecutionActivateMessage(
                "/requester", "req-1", 1, "sha256:selection",
                plan.digest(), (first, first), 10_000, 1, "signature")


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

from dataclasses import replace
import json
from pathlib import Path
import unittest

from ndnsf_distributed_inference.core import (
    DATA_DRIVEN_V2, LEGACY_READY_SET_V1,
    ProviderAssignment, DeploymentPlan, ProviderReadyMessage, ReadySetCoordinator,
    ProviderActivationGate, new_legacy_rollback_plan,
)
from ndnsf_distributed_inference.policy import (
    ServicePolicy, native_execution_plan_spec,
)

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/ndnsf-di-core-app-separation/distributed-consistency/transport-faults.json"


class ExecutionConsistencyTest(unittest.TestCase):
    def _selection_plan(self):
        members = (
            ProviderAssignment("r1", "/p1", "boot-1", "l1", "sha256:b1"),
            ProviderAssignment("r2", "/p2", "boot-2", "l2", "sha256:b2"),
        )
        return DeploymentPlan(
            "/u", "req", 1, "variant", ("sha256:artifact",), members,
            "distributed", 10_000, "sha256:selection",
            {"/p1": "a" * 48, "/p2": "b" * 48}, "/u/KEY/1")

    def _ready(self, plan, member, sequence=1):
        return ProviderReadyMessage(
            plan.request_id, plan.attempt, plan.selection_digest, plan.digest(),
            member.provider, member.provider_boot_epoch, member.role,
            plan.artifact_digests[0], f"ready:{member.role}", sequence,
            9_000, f"/cert{member.provider}", f"sig:{member.role}:{sequence}")

    def test_ready_set_requires_exact_members_and_activation_is_idempotent(self):
        plan = self._selection_plan()
        self.assertEqual(plan.execution_policy, LEGACY_READY_SET_V1)
        coordinator = ReadySetCoordinator(plan, verifier=lambda message: bool(message.signature))
        first = self._ready(plan, plan.assignments[0])
        self.assertTrue(coordinator.accept(first, now_ms=1_000).accepted)
        self.assertTrue(coordinator.accept(first, now_ms=1_000).accepted)
        self.assertFalse(coordinator.complete)
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            coordinator.activate(sequence=1, signature="sig:user", now_ms=1_000)
        second = self._ready(plan, plan.assignments[1])
        self.assertTrue(coordinator.accept(second, now_ms=1_000).accepted)
        activation = coordinator.activate(sequence=1, signature="sig:user", now_ms=1_000)
        gate = ProviderActivationGate(plan, "/p1", "r1", verifier=lambda message: bool(message.signature))
        self.assertTrue(gate.validate(activation, now_ms=1_000))
        self.assertFalse(gate.validate(activation, now_ms=1_000))
        self.assertEqual(gate.legacy_activation_count, 1)

    def test_data_driven_plan_cannot_enter_legacy_ready_set_path(self):
        plan = replace(
            self._selection_plan(), execution_policy=DATA_DRIVEN_V2)
        with self.assertRaisesRegex(ValueError, "LEGACY_READY_SET_V1"):
            ReadySetCoordinator(plan, verifier=lambda _message: True)
        with self.assertRaisesRegex(ValueError, "LEGACY_READY_SET_V1"):
            ProviderActivationGate(
                plan, "/p1", "r1", verifier=lambda _message: True)

    def test_rollback_to_v1_requires_a_new_invocation_identity(self):
        current = replace(
            self._selection_plan(), execution_policy=DATA_DRIVEN_V2)
        fresh_assignments = tuple(
            replace(item, lease_id=item.lease_id + "-rollback")
            for item in current.assignments)
        with self.assertRaisesRegex(ValueError, "new request_id"):
            new_legacy_rollback_plan(
                current, request_id=current.request_id,
                assignments=fresh_assignments,
                selection_digest="sha256:rollback-selection",
                deadline_ms=20_000)
        with self.assertRaisesRegex(ValueError, "fresh assignments"):
            new_legacy_rollback_plan(
                current, request_id="req-rollback-v1",
                assignments=current.assignments,
                selection_digest="sha256:rollback-selection",
                deadline_ms=20_000)
        rolled_back = new_legacy_rollback_plan(
            current, request_id="req-rollback-v1",
            assignments=fresh_assignments,
            selection_digest="sha256:rollback-selection",
            deadline_ms=20_000)
        self.assertEqual(rolled_back.execution_policy, LEGACY_READY_SET_V1)
        self.assertEqual(rolled_back.request_id, "req-rollback-v1")
        self.assertEqual(rolled_back.attempt, 1)
        self.assertNotEqual(rolled_back.selection_digest,
                            current.selection_digest)
        self.assertEqual(rolled_back.assignments, fresh_assignments)
        self.assertEqual(dict(rolled_back.status_handles), {})
        self.assertEqual(current.execution_policy, DATA_DRIVEN_V2)

    def test_native_plan_generator_seals_execution_policy(self):
        service = ServicePolicy(
            name="/Inference/Test", model_name="model-v1",
            roles=("stage-0",), dependencies=(), users=(), providers=(),
        )
        generated = native_execution_plan_spec((service,))
        self.assertEqual(
            generated["services"][0]["executionPolicy"], DATA_DRIVEN_V2)
        legacy = native_execution_plan_spec((replace(
            service, metadata={"executionPolicy": LEGACY_READY_SET_V1}),))
        self.assertEqual(
            legacy["services"][0]["executionPolicy"],
            LEGACY_READY_SET_V1)
        with self.assertRaisesRegex(ValueError, "execution policy"):
            native_execution_plan_spec((replace(
                service, metadata={"executionPolicy": "AUTOMATIC_FALLBACK"}),))

    def test_ready_set_rejects_stale_restart_conflict_and_wrong_role(self):
        plan = self._selection_plan()
        coordinator = ReadySetCoordinator(plan, verifier=lambda message: bool(message.signature))
        valid = self._ready(plan, plan.assignments[0])
        self.assertFalse(coordinator.accept(replace(valid, role="wrong"), now_ms=1_000).accepted)
        self.assertFalse(coordinator.accept(replace(valid, provider_boot_epoch="reboot"), now_ms=1_000).accepted)
        self.assertFalse(coordinator.accept(replace(valid, expires_at_ms=500), now_ms=1_000).accepted)
        self.assertTrue(coordinator.accept(valid, now_ms=1_000).accepted)
        self.assertFalse(coordinator.accept(replace(valid, sequence=2, signature="different"),
                                            now_ms=1_000).accepted)

    def test_fault_fixture_covers_closed_operation_set(self) -> None:
        payload = json.loads(FIXTURE.read_text())
        self.assertEqual(payload["schema"], "ndnsf-di-spec111-transport-faults-v1")
        self.assertEqual({item["fault"] for item in payload["cases"]}, {
            "drop", "duplicate", "reorder", "conflict", "boot-epoch-change"})

    def test_activation_rejects_wrong_membership_reboot_and_expiry(self) -> None:
        plan = self._selection_plan()
        coordinator = ReadySetCoordinator(plan, verifier=lambda message: True)
        for member in plan.assignments:
            coordinator.accept(self._ready(plan, member), now_ms=1_000)
        activation = coordinator.activate(
            sequence=1, signature="sig:user", now_ms=1_000)
        gate = ProviderActivationGate(
            plan, "/p1", "r1", verifier=lambda message: True)
        with self.assertRaisesRegex(ValueError, "ReadySet is incomplete"):
            gate.validate(replace(activation, members=activation.members[:1]),
                          now_ms=1_000)
        rebooted = replace(
            activation.members[0], provider_boot_epoch="boot-restarted")
        with self.assertRaisesRegex(ValueError, "ReadySet is incomplete"):
            gate.validate(replace(
                activation, members=(rebooted, activation.members[1])),
                now_ms=1_000)
        with self.assertRaisesRegex(ValueError, "authority failed"):
            gate.validate(activation, now_ms=activation.deadline_ms)


if __name__ == "__main__":
    unittest.main()

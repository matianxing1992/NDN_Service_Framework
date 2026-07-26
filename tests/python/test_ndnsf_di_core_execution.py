from __future__ import annotations

from dataclasses import replace
import unittest

from ndnsf_distributed_inference.core import (
    CoreAssignment,
    CoreExecutionEvidence,
    CoreExecutionPlan,
    CoreExecutor,
    DependencyDrivenExecution,
    ExecutionActivateMessage,
    ProviderAssignment,
    ReadySetMember,
    RequestCoordinatorBinding,
)


class FakeSession:
    def __init__(self, assignment: CoreAssignment, bad: bool = False) -> None:
        self.assignment = assignment
        self.bad = bad
        self.cancelled = False

    def execute(self, payload: bytes):
        evidence = CoreExecutionEvidence(
            "/p1", "boot-1", self.assignment.request_id,
            self.assignment.attempt_epoch + int(self.bad),
            self.assignment.plan_digest, self.assignment.digest(), "fake")
        return payload.upper(), (evidence,)

    def cancel(self) -> None:
        self.cancelled = True


class FakeAdapter:
    def __init__(self, bad: bool = False) -> None:
        self.bad = bad
        self.session = None

    def create_session(self, plan, assignment):
        self.session = FakeSession(assignment, self.bad)
        return self.session


def setup_contracts():
    plan = CoreExecutionPlan("plan-1", "model-1", ("role-1",))
    member = ProviderAssignment("role-1", "/p1", "boot-1", "lease-1", "sha256:r1")
    assignment = CoreAssignment("a-1", "req-1", 1, plan.digest(), (member,))
    binding = RequestCoordinatorBinding(
        "/requester", "req-1", 1, "sha256:intent", "sha256:objective",
        "sha256:snapshot", plan.digest(), assignment.digest(), 10_000,
        "/requester/results/req-1")
    return plan, member, assignment, binding


def activation(binding, member):
    return ExecutionActivateMessage(
        binding.requester_identity, binding.request_id, binding.attempt_epoch,
        "sha256:selection", binding.plan_digest,
        (ReadySetMember(member.provider, member.role,
                        member.provider_boot_epoch, "sha256:ready"),),
        binding.execution_deadline_ms, 1, "requester-signature")


class CoreExecutionTest(unittest.TestCase):
    def test_deterministic_native_adapter_success(self) -> None:
        plan, _, assignment, _ = setup_contracts()
        result = CoreExecutor(FakeAdapter()).execute(plan, assignment, b"hello")
        self.assertEqual(result.payload, b"HELLO")
        self.assertEqual(len(result.evidence), 1)

    def test_plan_and_native_evidence_mismatch_fail_closed(self) -> None:
        plan, _, assignment, _ = setup_contracts()
        with self.assertRaisesRegex(ValueError, "not bound"):
            CoreExecutor(FakeAdapter()).execute(
                plan, replace(assignment, plan_digest="sha256:other"), b"hello")
        adapter = FakeAdapter(bad=True)
        with self.assertRaisesRegex(ValueError, "evidence binding mismatch"):
            CoreExecutor(adapter).execute(plan, assignment, b"hello")
        self.assertTrue(adapter.session.cancelled)

    def test_exact_activation_gates_execution(self) -> None:
        plan, member, assignment, binding = setup_contracts()
        authorized = activation(binding, member)
        result = CoreExecutor(FakeAdapter()).execute(
            plan, assignment, b"hello", activation=authorized)
        self.assertEqual(result.activation_digest, authorized.digest())
        with self.assertRaisesRegex(ValueError, "activation attempt mismatch"):
            CoreExecutor(FakeAdapter()).execute(
                plan, assignment, b"hello",
                activation=replace(authorized, attempt=2))

    def test_r1_dependency_driven_stages_overlap_without_readyset(self) -> None:
        gate = DependencyDrivenExecution(
            request_id="r", attempt=1, plan_digest="sha256:plan",
            roles=("left", "right", "merge"),
            edges=(("left", "merge"), ("right", "merge")),
            terminal_role="merge", evidence_verifier=lambda fields:
                fields.get("signature") == "valid")
        for role in ("left", "right", "merge"):
            gate.select(role); gate.ready(role)
        self.assertTrue(gate.eligible("left")); self.assertTrue(gate.eligible("right"))
        self.assertFalse(gate.eligible("merge"))
        gate.start("left", at_ms=0); gate.start("right", at_ms=5)
        gate.complete("left", at_ms=20); gate.complete("right", at_ms=25)
        self.assertEqual(gate.overlap_ms("left", "right"), 15)
        for src in ("left", "right"):
            self.assertTrue(gate.accept_input({
                "producerRole": src, "consumerRole": "merge",
                "requestId": "r", "attempt": "1", "planDigest": "sha256:plan",
                "sequence": "1", "chunk": "0", "payloadDigest": "sha256:data",
                "signature": "valid"}))
        self.assertTrue(gate.eligible("merge"))
        gate.start("merge", at_ms=26); gate.complete("merge", at_ms=30)
        self.assertTrue(gate.terminal_output_accepted)

    def test_r1_stale_input_and_abort_never_accept_partial_terminal(self) -> None:
        gate = DependencyDrivenExecution(
            request_id="r", attempt=2, plan_digest="sha256:p", roles=("a", "b"),
            edges=(("a", "b"),), terminal_role="b",
            evidence_verifier=lambda _fields: True)
        gate.select("a"); gate.ready("a"); gate.select("b"); gate.ready("b")
        self.assertFalse(gate.accept_input({
            "producerRole": "a", "consumerRole": "b", "requestId": "r",
            "attempt": "1", "planDigest": "sha256:p", "sequence": "1",
            "chunk": "0", "payloadDigest": "sha256:x"}))
        gate.start("a", at_ms=0); gate.complete("a", at_ms=1)
        self.assertTrue(gate.abort("downstream failed"))
        self.assertFalse(gate.abort("duplicate"))
        self.assertFalse(gate.terminal_output_accepted)


if __name__ == "__main__":
    unittest.main()

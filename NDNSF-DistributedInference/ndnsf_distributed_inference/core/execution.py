"""Core execution orchestration and requester-authorized activation."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Iterable, Mapping, Protocol

from .contracts import (
    CoreAssignment,
    CoreExecutionEvidence,
    CoreExecutionPlan,
    ProviderAssignment,
    DeploymentPlan,
    ExecutionActivateMessage,
    ProviderReadyMessage,
    ReadyAcknowledgement,
    ReadySetMember,
)


class NativeSession(Protocol):
    def execute(self, payload: bytes) -> tuple[bytes, tuple[CoreExecutionEvidence, ...]]: ...
    def cancel(self) -> None: ...


class ExecutionAdapter(Protocol):
    def create_session(self, plan: CoreExecutionPlan,
                       assignment: CoreAssignment) -> NativeSession: ...


@dataclass(frozen=True)
class ExecutionResult:
    payload: bytes
    evidence: tuple[CoreExecutionEvidence, ...]
    activation_digest: str = ""


class CoreExecutor:
    def __init__(self, adapter: ExecutionAdapter) -> None:
        self._adapter = adapter

    def execute(self, plan: CoreExecutionPlan, assignment: CoreAssignment,
                payload: bytes, *,
                activation: ExecutionActivateMessage | None = None) -> ExecutionResult:
        if plan.digest() != assignment.plan_digest:
            raise ValueError("assignment is not bound to execution plan")
        if activation is not None:
            if activation.request_id != assignment.request_id:
                raise ValueError("activation request mismatch")
            if activation.attempt != assignment.attempt_epoch:
                raise ValueError("activation attempt mismatch")
            if activation.deployment_plan_digest != assignment.plan_digest:
                raise ValueError("activation plan mismatch")
            expected = {(item.role, item.provider, item.provider_boot_epoch)
                        for item in assignment.providers}
            activated = {(item.role, item.provider, item.provider_boot_epoch)
                         for item in activation.members}
            if expected != activated:
                raise ValueError("activation assignment mismatch")
        session = self._adapter.create_session(plan, assignment)
        output, evidence = session.execute(bytes(payload))
        for item in evidence:
            if (item.request_id != assignment.request_id
                    or item.attempt_epoch != assignment.attempt_epoch
                    or item.plan_digest != assignment.plan_digest
                    or item.assignment_digest != assignment.digest()):
                session.cancel()
                raise ValueError("native execution evidence binding mismatch")
        return ExecutionResult(bytes(output), tuple(evidence),
                               "" if activation is None else activation.digest())


class DependencyDrivenExecution:
    """R1 local eligibility authority; no complete-ReadySet barrier exists."""

    def __init__(self, *, request_id: str, attempt: int, plan_digest: str,
                 roles: Iterable[str], edges: Iterable[tuple[str, str]],
                 terminal_role: str,
                 evidence_verifier: Callable[[Mapping[str, str]], bool]) -> None:
        self.request_id = str(request_id)
        self.attempt = int(attempt)
        self.plan_digest = str(plan_digest)
        self.roles = frozenset(str(role) for role in roles)
        self.edges = frozenset((str(src), str(dst)) for src, dst in edges)
        self.terminal_role = str(terminal_role)
        self._verify = evidence_verifier
        if (not self.request_id or self.attempt <= 0 or not self.plan_digest
                or not self.roles or self.terminal_role not in self.roles
                or any(src not in self.roles or dst not in self.roles or src == dst
                       for src, dst in self.edges)):
            raise ValueError("invalid dependency execution plan")
        incoming = {role: set() for role in self.roles}
        outgoing = {role: set() for role in self.roles}
        for src, dst in self.edges:
            incoming[dst].add(src); outgoing[src].add(dst)
        frontier = [role for role in self.roles if not incoming[role]]
        visited = set()
        while frontier:
            role = frontier.pop(); visited.add(role)
            for dst in outgoing[role]:
                if incoming[dst] <= visited: frontier.append(dst)
        if visited != set(self.roles):
            raise ValueError("dependency plan must be acyclic")
        self._incoming = incoming
        self._selected: set[str] = set()
        self._ready: set[str] = set()
        self._inputs: dict[str, set[str]] = {role: set() for role in self.roles}
        self._seen_chunks: set[tuple[str, str, int, int]] = set()
        self._running: dict[str, int] = {}
        self._started: dict[str, int] = {}
        self._completed: dict[str, int] = {}
        self._aborted = False
        self._abort_reason = ""

    def select(self, role: str) -> None:
        self._require_role(role); self._selected.add(role)

    def ready(self, role: str) -> None:
        self._require_role(role); self._ready.add(role)

    def accept_input(self, fields: Mapping[str, str]) -> bool:
        src, dst = fields.get("producerRole", ""), fields.get("consumerRole", "")
        try:
            sequence = int(fields.get("sequence", "0"))
            chunk = int(fields.get("chunk", "0"))
        except ValueError:
            return False
        valid = (
            (src, dst) in self.edges and
            fields.get("requestId") == self.request_id and
            fields.get("attempt") == str(self.attempt) and
            fields.get("planDigest") == self.plan_digest and
            bool(fields.get("payloadDigest")) and sequence > 0 and chunk >= 0 and
            self._verify(fields))
        identity = (src, dst, sequence, chunk)
        if not valid or identity in self._seen_chunks:
            return False
        self._seen_chunks.add(identity); self._inputs[dst].add(src)
        return True

    def eligible(self, role: str) -> bool:
        self._require_role(role)
        return (not self._aborted and role in self._selected and role in self._ready
                and self._incoming[role] <= self._inputs[role]
                and role not in self._running and role not in self._completed)

    def start(self, role: str, *, at_ms: int) -> None:
        if not self.eligible(role) or at_ms < 0:
            raise RuntimeError("stage prerequisites are incomplete")
        self._running[role] = int(at_ms); self._started[role] = int(at_ms)

    def complete(self, role: str, *, at_ms: int) -> None:
        if role not in self._running or at_ms < self._running[role]:
            raise RuntimeError("stage is not running")
        self._completed[role] = int(at_ms); self._running.pop(role)

    def abort(self, reason: str) -> bool:
        if self._aborted: return False
        self._aborted = True; self._abort_reason = str(reason) or "ABORTED"
        self._running.clear(); return True

    @property
    def terminal_output_accepted(self) -> bool:
        return not self._aborted and self.terminal_role in self._completed

    def overlap_ms(self, first: str, second: str) -> int:
        if first not in self._completed or second not in self._completed:
            return 0
        return max(0, min(self._completed[first], self._completed[second]) -
                   max(self._started[first], self._started[second]))

    def _require_role(self, role: str) -> None:
        if role not in self.roles: raise ValueError("unknown stage role")


class ReadySetCoordinator:
    """Canonical requester-owned exact-member readiness authority."""

    def __init__(self, plan: DeploymentPlan, *,
                 verifier: Callable[[ProviderReadyMessage], bool]) -> None:
        self.plan = plan
        self._verifier = verifier
        self._accepted: dict[tuple[str, str], ProviderReadyMessage] = {}
        self._ack_sequence = 0
        self._activation: ExecutionActivateMessage | None = None

    @property
    def complete(self) -> bool:
        expected = {(item.provider, item.role) for item in self.plan.assignments}
        return set(self._accepted) == expected

    def accept(self, message: ProviderReadyMessage, *, now_ms: int) -> ReadyAcknowledgement:
        self._ack_sequence += 1
        expected = {(item.provider, item.role): item for item in self.plan.assignments}
        member = expected.get(message.membership_key())
        reason = "OK"
        accepted = True
        if member is None:
            accepted, reason = False, "UNSELECTED_MEMBER"
        elif (message.request_id != self.plan.request_id
              or message.attempt != self.plan.attempt
              or message.selection_digest != self.plan.selection_digest
              or message.deployment_plan_digest != self.plan.digest()
              or message.provider_boot_epoch != member.provider_boot_epoch):
            accepted, reason = False, "BINDING_MISMATCH"
        elif message.expires_at_ms <= now_ms:
            accepted, reason = False, "EXPIRED"
        elif not self._verifier(message):
            accepted, reason = False, "SIGNATURE_INVALID"
        else:
            previous = self._accepted.get(message.membership_key())
            if previous is not None and previous.digest() != message.digest():
                accepted, reason = False, "CONFLICTING_READY"
            elif previous is None:
                self._accepted[message.membership_key()] = message
        return ReadyAcknowledgement(
            message.digest(), accepted, reason, self.plan.requester_identity,
            self._ack_sequence, f"requester-signature:{self._ack_sequence}")

    def activate(self, *, sequence: int, signature: str, now_ms: int) -> ExecutionActivateMessage:
        if not self.complete:
            raise RuntimeError("cannot activate an incomplete ReadySet")
        if now_ms >= self.plan.deadline_ms:
            raise RuntimeError("cannot activate an expired DeploymentPlan")
        members = tuple(sorted((ReadySetMember(
            item.provider, item.role, item.provider_boot_epoch,
            self._accepted[(item.provider, item.role)].digest())
            for item in self.plan.assignments), key=lambda item: (item.provider, item.role)))
        candidate = ExecutionActivateMessage(
            self.plan.requester_identity, self.plan.request_id, self.plan.attempt,
            self.plan.selection_digest, self.plan.digest(), members,
            self.plan.deadline_ms, sequence, signature)
        if self._activation is not None and self._activation.digest() != candidate.digest():
            raise RuntimeError("conflicting execution activation")
        self._activation = candidate
        return candidate


class ProviderActivationGate:
    """Provider-side idempotent activation fence for one selected role."""

    def __init__(self, plan: DeploymentPlan, provider: str, role: str,
                 *, verifier: Callable[[ExecutionActivateMessage], bool]) -> None:
        self.plan = plan
        self.provider = provider
        self.role = role
        self._verifier = verifier
        self._accepted_digest = ""

    def validate(self, activation: ExecutionActivateMessage, *, now_ms: int) -> bool:
        assignment = next((item for item in self.plan.assignments
                           if item.provider == self.provider and item.role == self.role), None)
        if assignment is None:
            raise ValueError("Provider role is not selected")
        if (activation.requester_identity != self.plan.requester_identity
                or activation.request_id != self.plan.request_id
                or activation.attempt != self.plan.attempt
                or activation.selection_digest != self.plan.selection_digest
                or activation.deployment_plan_digest != self.plan.digest()
                or activation.deadline_ms != self.plan.deadline_ms
                or now_ms >= activation.deadline_ms
                or not self._verifier(activation)):
            raise ValueError("execution activation binding or authority failed")
        expected = {(item.provider, item.role, item.provider_boot_epoch)
                    for item in self.plan.assignments}
        observed = {(item.provider, item.role, item.provider_boot_epoch)
                    for item in activation.members}
        if expected != observed:
            raise ValueError("execution activation ReadySet is incomplete")
        digest = activation.digest()
        if self._accepted_digest and self._accepted_digest != digest:
            raise ValueError("conflicting execution activation")
        duplicate = self._accepted_digest == digest
        self._accepted_digest = digest
        return not duplicate

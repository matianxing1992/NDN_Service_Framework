"""Core execution orchestration and requester-authorized activation."""

from __future__ import annotations

from dataclasses import dataclass
from threading import RLock
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
    canonical_digest,
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


@dataclass(frozen=True)
class RoleExecutionBinding:
    role: str
    provider: str
    provider_boot_epoch: str

    def __post_init__(self) -> None:
        if (not self.role or not self.provider
                or len(self.provider_boot_epoch) < 8):
            raise ValueError("invalid role execution binding")


@dataclass(frozen=True)
class InputOutputObjectManifest:
    """Authenticated identity for one dependency object, not its payload."""

    object_name: str
    request_id: str
    attempt: int
    plan_digest: str
    producer_role: str
    producer_provider: str
    producer_boot_epoch: str
    generation: int
    consumer_roles: tuple[str, ...]
    lineage_digests: tuple[str, ...]
    schema_digest: str
    segment_count: int
    total_bytes: int
    payload_digest: str
    aead_algorithm: str
    key_grant_digest: str
    signer_key_id: str
    signature: str
    captured_at_ms: int
    expires_at_ms: int

    def __post_init__(self) -> None:
        object.__setattr__(self, "consumer_roles",
                           tuple(self.consumer_roles))
        object.__setattr__(self, "lineage_digests",
                           tuple(self.lineage_digests))
        if (not self.object_name.startswith("/") or not self.request_id
                or self.attempt <= 0 or not self.producer_role
                or not self.producer_provider
                or len(self.producer_boot_epoch) < 8
                or self.generation <= 0
                or len(set(self.consumer_roles)) != len(self.consumer_roles)
                or not self.lineage_digests
                or self.segment_count <= 0 or self.total_bytes < 0
                or self.aead_algorithm not in {
                    "AES-256-GCM", "CHACHA20-POLY1305"}
                or not self.signer_key_id or not self.signature
                or self.captured_at_ms < 0
                or self.expires_at_ms <= self.captured_at_ms):
            raise ValueError("invalid InputOutputObjectManifest")
        for name in (
                "plan_digest", "schema_digest", "payload_digest",
                "key_grant_digest"):
            _require_execution_digest(getattr(self, name), name)
        for value in self.lineage_digests:
            _require_execution_digest(value, "lineage_digest")

    def digest(self) -> str:
        return canonical_digest(self)


@dataclass(frozen=True)
class ResultContract:
    sink_roles: tuple[str, ...]
    result_schema_digest: str
    result_semantics_digest: str
    aggregator_role: str = ""
    aggregation_rule_digest: str = ""
    aggregation_output_role: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(self, "sink_roles", tuple(self.sink_roles))
        if (not self.sink_roles
                or len(set(self.sink_roles)) != len(self.sink_roles)):
            raise ValueError("invalid ResultContract sinks")
        if len(self.sink_roles) > 1 and not (
                self.aggregator_role or (
                    self.aggregation_rule_digest
                    and self.aggregation_output_role)):
            raise ValueError(
                "multiple sinks require an aggregator or aggregation rule")
        if self.aggregator_role and self.aggregator_role not in self.sink_roles:
            raise ValueError("ResultContract aggregator is not a sink role")
        if (self.aggregation_output_role
                and self.aggregation_output_role not in self.sink_roles):
            raise ValueError(
                "ResultContract aggregation output is not a sink role")
        for name in ("result_schema_digest", "result_semantics_digest"):
            _require_execution_digest(getattr(self, name), name)
        if self.aggregation_rule_digest:
            _require_execution_digest(
                self.aggregation_rule_digest, "aggregation_rule_digest")

    def digest(self) -> str:
        return canonical_digest(self)

    @property
    def result_role(self) -> str:
        if self.aggregator_role:
            return self.aggregator_role
        if len(self.sink_roles) == 1:
            return self.sink_roles[0]
        return self.aggregation_output_role


@dataclass(frozen=True)
class DIResultEnvelopeV2:
    request_id: str
    attempt: int
    plan_digest: str
    producer_role: str
    producer_provider: str
    producer_boot_epoch: str
    generation: int
    result_contract_digest: str
    output_manifest_digest: str
    result_schema_digest: str
    result_semantics_digest: str
    payload_digest: str
    signer_key_id: str
    signature: str

    def __post_init__(self) -> None:
        if (not self.request_id or self.attempt <= 0
                or not self.producer_role or not self.producer_provider
                or len(self.producer_boot_epoch) < 8
                or self.generation <= 0 or not self.signer_key_id
                or not self.signature):
            raise ValueError("invalid DIResultEnvelopeV2")
        for name in (
                "plan_digest", "result_contract_digest",
                "output_manifest_digest", "result_schema_digest",
                "result_semantics_digest", "payload_digest"):
            _require_execution_digest(getattr(self, name), name)

    def digest(self) -> str:
        return canonical_digest(self)


def _require_execution_digest(value: str, field: str) -> None:
    if (not isinstance(value, str) or len(value) != 71
            or not value.startswith("sha256:")):
        raise ValueError(f"{field} must be a canonical sha256 digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(
            f"{field} must be a canonical sha256 digest") from exc


class DependencyDrivenExecution:
    """Generation-fenced local DAG authority; admission is at-most-once.

    A RUNNING transition is durable admission evidence, not a claim that
    physical computation cannot be repeated after a process crash.
    """

    def __init__(self, *, request_id: str, attempt: int, plan_digest: str,
                 roles: Iterable[str], edges: Iterable[tuple[str, str]],
                 terminal_role: str,
                 evidence_verifier: Callable[[Any], bool],
                 role_bindings: Mapping[str, RoleExecutionBinding] | None = None,
                 generation: int = 1,
                 deadline_ms: int = (1 << 63) - 1,
                 result_contract: ResultContract | None = None,
                 start_callback: Callable[[str], None] | None = None,
                 response_callback: Callable[
                     [DIResultEnvelopeV2, InputOutputObjectManifest],
                     None] | None = None) -> None:
        self.request_id = str(request_id)
        self.attempt = int(attempt)
        self.plan_digest = str(plan_digest)
        self.roles = frozenset(str(role) for role in roles)
        self.edges = frozenset((str(src), str(dst)) for src, dst in edges)
        self.terminal_role = str(terminal_role)
        self._verify = evidence_verifier
        self.generation = int(generation)
        self.deadline_ms = int(deadline_ms)
        self.result_contract = result_contract
        self._start_callback = start_callback
        self._response_callback = response_callback
        self._bindings = dict(role_bindings or {})
        if (not self.request_id or self.attempt <= 0 or not self.plan_digest
                or not self.roles or self.terminal_role not in self.roles
                or self.generation <= 0 or self.deadline_ms <= 0
                or any(src not in self.roles or dst not in self.roles or src == dst
                       for src, dst in self.edges)):
            raise ValueError("invalid dependency execution plan")
        if (self._bindings and set(self._bindings) != set(self.roles)
                or any(key != value.role
                       for key, value in self._bindings.items())):
            raise ValueError("role execution bindings are incomplete")
        if (result_contract is not None
                and (not set(result_contract.sink_roles) <= set(self.roles)
                     or result_contract.result_role != self.terminal_role)):
            raise ValueError("ResultContract is not bound to the terminal role")
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
        self._seen_manifests: set[str] = set()
        self._running: dict[str, int] = {}
        self._started: dict[str, int] = {}
        self._completed: dict[str, int] = {}
        self._aborted = False
        self._abort_reason = ""
        self._terminal_outcome = ""
        self._terminal_at_ms = -1
        self._lock = RLock()

    def _binding_matches(
        self, role: str, provider: str, boot_epoch: str,
        generation: int | None,
    ) -> bool:
        expected = self._bindings.get(role)
        return (
            (expected is None or (
                provider == expected.provider
                and boot_epoch == expected.provider_boot_epoch))
            and (generation is None or generation == self.generation)
        )

    def _event_open(self, at_ms: int | None = None) -> bool:
        return (
            not self._terminal_outcome
            and not self._aborted
            and (at_ms is None or at_ms < self.deadline_ms)
        )

    def select(self, role: str, *, provider: str = "",
               boot_epoch: str = "", generation: int | None = None) -> bool:
        with self._lock:
            self._require_role(role)
            if (not self._event_open()
                    or not self._binding_matches(
                        role, provider, boot_epoch, generation)):
                return False
            self._selected.add(role)
            return True

    def ready(self, role: str, *, provider: str = "",
              boot_epoch: str = "", generation: int | None = None,
              at_ms: int = 0) -> bool:
        with self._lock:
            self._require_role(role)
            if (not self._event_open(at_ms)
                    or not self._binding_matches(
                        role, provider, boot_epoch, generation)):
                return False
            self._ready.add(role)
            self._admit_if_eligible_locked(role, at_ms)
        return True

    def accept_input(self, fields: Mapping[str, str]) -> bool:
        src, dst = fields.get("producerRole", ""), fields.get("consumerRole", "")
        try:
            sequence = int(fields.get("sequence", "0"))
            chunk = int(fields.get("chunk", "0"))
        except ValueError:
            return False
        with self._lock:
            valid = (
                self._event_open()
                and (src, dst) in self.edges
                and fields.get("requestId") == self.request_id
                and fields.get("attempt") == str(self.attempt)
                and fields.get("planDigest") == self.plan_digest
                and bool(fields.get("payloadDigest"))
                and sequence > 0 and chunk >= 0 and self._verify(fields))
            identity = (src, dst, sequence, chunk)
            if not valid or identity in self._seen_chunks:
                return False
            self._seen_chunks.add(identity)
            self._inputs[dst].add(src)
            return True

    def accept_manifest(
        self, value: InputOutputObjectManifest, *, at_ms: int,
    ) -> bool:
        with self._lock:
            if (not self._event_open(at_ms)
                    or value.request_id != self.request_id
                    or value.attempt != self.attempt
                    or value.plan_digest != self.plan_digest
                    or value.generation != self.generation
                    or at_ms >= value.expires_at_ms
                    or not self._binding_matches(
                        value.producer_role, value.producer_provider,
                        value.producer_boot_epoch, value.generation)
                    or not value.consumer_roles
                    or any((value.producer_role, consumer) not in self.edges
                           for consumer in value.consumer_roles)
                    or not self._verify(value)):
                return False
            identity = value.digest()
            if identity in self._seen_manifests:
                return False
            self._seen_manifests.add(identity)
            for consumer in value.consumer_roles:
                self._inputs[consumer].add(value.producer_role)
                self._admit_if_eligible_locked(consumer, at_ms)
        return True

    def eligible(self, role: str) -> bool:
        with self._lock:
            self._require_role(role)
            return self._eligible_locked(role)

    def _eligible_locked(self, role: str) -> bool:
        return (self._event_open()
                and role in self._selected and role in self._ready
                and self._incoming[role] <= self._inputs[role]
                and role not in self._running and role not in self._completed)

    def _admit_if_eligible_locked(
        self, role: str, at_ms: int,
    ) -> bool:
        if self._start_callback is None or not self._eligible_locked(role):
            return False
        self._running[role] = int(at_ms)
        self._started[role] = int(at_ms)
        try:
            self._start_callback(role)
        except Exception:
            self._running.pop(role, None)
            self._aborted = True
            self._abort_reason = "START_CALLBACK_FAILED"
            self._terminal_outcome = "FAILED"
            self._terminal_at_ms = int(at_ms)
            raise
        return True

    def accept_status(
        self, *, role: str, provider: str, boot_epoch: str,
        generation: int, request_id: str, attempt: int,
        plan_digest: str, at_ms: int,
    ) -> bool:
        """Validate an observational role status against the live fence."""

        with self._lock:
            self._require_role(role)
            return (
                self._event_open(at_ms)
                and request_id == self.request_id
                and attempt == self.attempt
                and plan_digest == self.plan_digest
                and self._binding_matches(
                    role, provider, boot_epoch, generation)
            )

    def start(self, role: str, *, at_ms: int) -> None:
        with self._lock:
            if (at_ms < 0 or at_ms >= self.deadline_ms
                    or not self._eligible_locked(role)):
                raise RuntimeError("stage prerequisites are incomplete")
            self._running[role] = int(at_ms)
            self._started[role] = int(at_ms)

    def complete(self, role: str, *, at_ms: int) -> None:
        with self._lock:
            if (not self._event_open(at_ms)
                    or role not in self._running
                    or at_ms < self._running[role]):
                raise RuntimeError("stage is not running")
            self._completed[role] = int(at_ms)
            self._running.pop(role)
            if self.result_contract is None and role == self.terminal_role:
                self._terminal_outcome = "RESPONSE"
                self._terminal_at_ms = int(at_ms)

    def abort(self, reason: str) -> bool:
        return self.fail("", reason, at_ms=0)

    def fail(self, role: str, reason: str, *, at_ms: int) -> bool:
        del role
        with self._lock:
            if not self._event_open():
                return False
            self._aborted = True
            self._abort_reason = str(reason) or "FAILED"
            self._running.clear()
            self._terminal_outcome = "FAILED"
            self._terminal_at_ms = int(at_ms)
            return True

    def cancel(self, reason: str, *, at_ms: int) -> bool:
        with self._lock:
            if not self._event_open():
                return False
            self._aborted = True
            self._abort_reason = str(reason) or "CANCELLED"
            self._running.clear()
            self._terminal_outcome = "CANCELLED"
            self._terminal_at_ms = int(at_ms)
            return True

    def expire(self, *, at_ms: int) -> bool:
        with self._lock:
            if self._terminal_outcome or at_ms < self.deadline_ms:
                return False
            self._aborted = True
            self._abort_reason = "EXPIRED"
            self._running.clear()
            self._terminal_outcome = "EXPIRED"
            self._terminal_at_ms = int(at_ms)
            return True

    def supersede(self, *, new_attempt: int, at_ms: int) -> bool:
        with self._lock:
            if self._terminal_outcome or new_attempt <= self.attempt:
                return False
            self._aborted = True
            self._abort_reason = "SUPERSEDED"
            self._running.clear()
            self._terminal_outcome = "SUPERSEDED"
            self._terminal_at_ms = int(at_ms)
            return True

    def accept_result(
        self, envelope: DIResultEnvelopeV2, *,
        output_manifest: InputOutputObjectManifest, at_ms: int,
    ) -> bool:
        with self._lock:
            contract = self.result_contract
            binding = self._bindings.get(envelope.producer_role)
            if (contract is None or not self._event_open(at_ms)
                    or envelope.request_id != self.request_id
                    or envelope.attempt != self.attempt
                    or envelope.plan_digest != self.plan_digest
                    or envelope.generation != self.generation
                    or envelope.producer_role != contract.result_role
                    or envelope.producer_role not in self._completed
                    or binding is None
                    or envelope.producer_provider != binding.provider
                    or envelope.producer_boot_epoch
                    != binding.provider_boot_epoch
                    or envelope.result_contract_digest != contract.digest()
                    or envelope.result_schema_digest
                    != contract.result_schema_digest
                    or envelope.result_semantics_digest
                    != contract.result_semantics_digest
                    or output_manifest.digest()
                    != envelope.output_manifest_digest
                    or output_manifest.payload_digest != envelope.payload_digest
                    or output_manifest.schema_digest
                    != contract.result_schema_digest
                    or output_manifest.consumer_roles
                    or output_manifest.producer_role != envelope.producer_role
                    or output_manifest.producer_provider
                    != envelope.producer_provider
                    or output_manifest.producer_boot_epoch
                    != envelope.producer_boot_epoch
                    or output_manifest.request_id != self.request_id
                    or output_manifest.attempt != self.attempt
                    or output_manifest.plan_digest != self.plan_digest
                    or output_manifest.generation != self.generation
                    or at_ms >= output_manifest.expires_at_ms
                    or not self._verify(output_manifest)
                    or not self._verify(envelope)):
                return False
            self._terminal_outcome = "RESPONSE"
            self._terminal_at_ms = int(at_ms)
            if self._response_callback is not None:
                try:
                    self._response_callback(envelope, output_manifest)
                except Exception:
                    self._terminal_outcome = "FAILED"
                    self._abort_reason = "RESPONSE_CALLBACK_FAILED"
                    self._aborted = True
                    return False
            return True

    @property
    def terminal_output_accepted(self) -> bool:
        with self._lock:
            return self._terminal_outcome == "RESPONSE"

    @property
    def terminal_outcome(self) -> str:
        with self._lock:
            return self._terminal_outcome

    def state(self, role: str) -> str:
        with self._lock:
            self._require_role(role)
            if role in self._completed:
                return "COMPLETED"
            if role in self._running:
                return "RUNNING"
            if self._terminal_outcome:
                return self._terminal_outcome
            return "WAITING"

    def overlap_ms(self, first: str, second: str) -> int:
        with self._lock:
            if first not in self._completed or second not in self._completed:
                return 0
            return max(
                0, min(self._completed[first], self._completed[second])
                - max(self._started[first], self._started[second]))

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

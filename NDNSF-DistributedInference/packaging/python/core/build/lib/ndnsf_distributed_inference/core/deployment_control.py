"""Spec 129 workload-neutral selection, preparation, and activation authority.

This module is intentionally transport-neutral.  Core C++ carries the signed
wire messages; DI adapters supply capability and model lifecycle callbacks.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from concurrent.futures import Future, ThreadPoolExecutor
import base64
import hashlib
import json
from typing import Any, Callable, Iterable, Mapping
from threading import RLock

from .contracts import (
    DISelectionAcceptanceV2, DISelectionAssignmentV2, DIRoleAssignmentV2,
    ShardResidencyEvidenceV2, StateReuseBindingV2,
    DeploymentInstance, DeploymentInstanceState, DeploymentIntent,
    DeploymentPlan, ExecutionActivateMessage,
    ProviderAssignment, ProviderCapabilityOffer, ProviderReadyMessage,
    ReservationLease,
    SelectionDecision, SelectionDecisionReceipt, SelectionDecisionTombstone,
)
from .execution import ProviderActivationGate


@dataclass(frozen=True)
class SelectionPreparationContext:
    """Least-authority role preparation input created after WAL COMMITTED."""

    transaction_id: str
    invocation_id: str
    request_id: str
    attempt: int
    plan_digest: str
    provider: str
    provider_boot_epoch: str
    deadline_ms: int
    generation: int
    role: DIRoleAssignmentV2


@dataclass(frozen=True)
class SelectionPreparationCallbacks:
    prepare_role: Callable[[SelectionPreparationContext], None]
    start_role: Callable[[str], None]
    release_role: Callable[[str, str], None] = (
        lambda _role, _reason: None)


@dataclass(frozen=True)
class ShardPreparationCallbacks:
    fetch_from_repository: Callable[[str], bytes]
    verify_content: Callable[[str, bytes], None]
    promote_to_disk: Callable[[str, bytes], None]
    load_to_ram: Callable[[str], None]
    load_to_gpu: Callable[[str], None]


class ShardPreparationPipeline:
    """Selection-scoped exact residency revalidation and miss promotion."""

    def __init__(self, *, provider: str, boot_epoch: str, cache_epoch: int,
                 callbacks: ShardPreparationCallbacks,
                 verify_signature: Callable[[ShardResidencyEvidenceV2], bool]):
        self.provider = provider
        self.boot_epoch = boot_epoch
        self.cache_epoch = int(cache_epoch)
        self.callbacks = callbacks
        self.verify_signature = verify_signature

    def ensure_gpu(
        self, artifact_digest: str,
        evidence: ShardResidencyEvidenceV2 | None,
        *, now_ms: int, pin_live: bool,
    ) -> str:
        tier = "REPOSITORY"
        if evidence is not None and evidence.artifact_digest == artifact_digest:
            try:
                evidence.revalidate(
                    now_ms=now_ms, provider=self.provider,
                    boot_epoch=self.boot_epoch, cache_epoch=self.cache_epoch,
                    pin_live=pin_live,
                    verify_signature=self.verify_signature)
                tier = evidence.tier
            except ValueError:
                tier = "REPOSITORY"
        if tier in {"PINNED_GPU", "RELOAD_SAFE_GPU"}:
            return tier
        if tier == "HOST_RAM":
            self.callbacks.load_to_gpu(artifact_digest)
            return "GPU_LOADED"
        if tier == "DISK":
            self.callbacks.load_to_ram(artifact_digest)
            self.callbacks.load_to_gpu(artifact_digest)
            return "GPU_LOADED"
        payload = bytes(
            self.callbacks.fetch_from_repository(artifact_digest))
        self.callbacks.verify_content(artifact_digest, payload)
        self.callbacks.promote_to_disk(artifact_digest, payload)
        self.callbacks.load_to_ram(artifact_digest)
        self.callbacks.load_to_gpu(artifact_digest)
        return "GPU_LOADED"


@dataclass
class _RetainedShard:
    artifact_digest: str
    last_used_sequence: int
    selected_refs: int = 0
    inflight_refs: int = 0


class ModelShardRetentionCache:
    """Bounded cross-request shard retention with non-eviction fences."""

    def __init__(self, max_entries: int) -> None:
        if max_entries <= 0:
            raise ValueError("model shard retention bound must be positive")
        self.max_entries = int(max_entries)
        self._entries: dict[str, _RetainedShard] = {}
        self._sequence = 0
        self._lock = RLock()

    def retain(self, artifact_digest: str) -> None:
        with self._lock:
            self._sequence += 1
            entry = self._entries.get(artifact_digest)
            created = entry is None
            if entry is None:
                entry = _RetainedShard(artifact_digest, self._sequence)
                self._entries[artifact_digest] = entry
            entry.last_used_sequence = self._sequence
            self._evict()
            if len(self._entries) > self.max_entries:
                if created:
                    self._entries.pop(artifact_digest, None)
                raise RuntimeError(
                    "model shard retention is full of selected/in-flight entries")

    def pin_selected(self, artifact_digest: str) -> None:
        with self._lock:
            self.retain(artifact_digest)
            if artifact_digest not in self._entries:
                raise RuntimeError(
                    "no retention capacity for selected model shard")
            self._entries[artifact_digest].selected_refs += 1

    def begin_inflight(self, artifact_digest: str) -> None:
        with self._lock:
            self.retain(artifact_digest)
            if artifact_digest not in self._entries:
                raise RuntimeError(
                    "no retention capacity for in-flight model shard")
            self._entries[artifact_digest].inflight_refs += 1

    def release(self, artifact_digest: str, *, selected: bool = False,
                inflight: bool = False) -> None:
        with self._lock:
            entry = self._entries[artifact_digest]
            if selected:
                entry.selected_refs = max(0, entry.selected_refs - 1)
            if inflight:
                entry.inflight_refs = max(0, entry.inflight_refs - 1)
            self._evict()

    def _evict(self) -> None:
        while len(self._entries) > self.max_entries:
            candidates = [
                value for value in self._entries.values()
                if value.selected_refs == 0 and value.inflight_refs == 0
            ]
            if not candidates:
                return
            victim = min(
                candidates, key=lambda value: value.last_used_sequence)
            self._entries.pop(victim.artifact_digest, None)

    def contains(self, artifact_digest: str) -> bool:
        with self._lock:
            return artifact_digest in self._entries


class DerivedStateStore:
    """Provider-local derived-state references; terminal cleanup is default."""

    def __init__(self, *, max_entries: int = 128) -> None:
        if max_entries <= 0:
            raise ValueError("derived-state bound must be positive")
        self.max_entries = int(max_entries)
        self._entries: dict[str, dict[str, Any]] = {}
        self._lock = RLock()

    def put(self, *, request_id: str, binding: StateReuseBindingV2) -> None:
        with self._lock:
            current = self._entries.get(binding.state_key)
            if current is not None and int(current["pins"]) != 0:
                raise RuntimeError("cannot replace pinned derived state")
            if current is not None:
                self._entries.pop(binding.state_key)
            if len(self._entries) >= self.max_entries:
                unpinned = [
                    (key, value) for key, value in self._entries.items()
                    if int(value["pins"]) == 0
                ]
                if not unpinned:
                    raise RuntimeError("all derived state is pinned")
                self._entries.pop(unpinned[0][0])
            self._entries[binding.state_key] = {
                "request_id": request_id,
                "binding": binding,
                "pins": 0,
            }

    def acquire(self, binding: StateReuseBindingV2, **revalidation) -> None:
        with self._lock:
            entry = self._entries.get(binding.state_key)
            if entry is None or entry["binding"] != binding:
                raise ValueError("derived state is not locally available")
            binding.revalidate(**revalidation)
            entry["pins"] = int(entry["pins"]) + 1

    def release(self, state_key: str) -> None:
        with self._lock:
            entry = self._entries.get(state_key)
            if entry is not None:
                entry["pins"] = max(0, int(entry["pins"]) - 1)

    def destroy_request_state(self, request_id: str) -> None:
        with self._lock:
            for key, value in tuple(self._entries.items()):
                if value["request_id"] == request_id:
                    if int(value["pins"]) != 0:
                        raise RuntimeError(
                            "cannot destroy pinned derived state")
                    self._entries.pop(key)

    def contains(self, state_key: str) -> bool:
        with self._lock:
            return state_key in self._entries


class GpuMiBAdmissionLedger:
    """Provider-local GPU-MiB offer/commit projection.

    Held offers reserve capacity immediately, so concurrent positive ACKs
    cannot promise the same capacity. The Core WAL commit blob remains the
    authority; this ledger is an idempotent runtime projection.
    """

    def __init__(self, *, provider: str, boot_epoch: str,
                 capacity_mib: int) -> None:
        if not provider or len(boot_epoch) < 8 or capacity_mib <= 0:
            raise ValueError("invalid GPU admission ledger identity/capacity")
        self.provider = provider
        self.boot_epoch = boot_epoch
        self.capacity_mib = int(capacity_mib)
        self._held: dict[str, dict[str, Any]] = {}
        self._committed: dict[str, dict[str, Any]] = {}
        self._lock = RLock()

    def _live_held_mib(self, now_ms: int | None = None) -> int:
        if now_ms is not None:
            for key, value in tuple(self._held.items()):
                if int(value["expires_at_ms"]) <= now_ms:
                    self._held.pop(key, None)
        return sum(int(value["gpu_mib"]) for value in self._held.values())

    def held_mib(self, *, now_ms: int | None = None) -> int:
        with self._lock:
            return self._live_held_mib(now_ms)

    def committed_mib(self) -> int:
        with self._lock:
            return sum(
                int(value["gpu_mib"]) for value in self._committed.values())

    def hold_offer(self, offer, *, now_ms: int) -> None:
        digest = offer.digest()
        with self._lock:
            if (offer.provider != self.provider
                    or offer.boot_epoch != self.boot_epoch
                    or offer.expires_at_ms <= now_ms
                    or offer.offered_gpu_memory_mb <= 0):
                raise ValueError("GPU offer identity, epoch, or expiry mismatch")
            current = self._held.get(digest)
            value = {
                "offer_digest": digest,
                "provider": self.provider,
                "provider_boot_epoch": self.boot_epoch,
                "request_id": offer.request_id,
                "attempt": int(offer.attempt),
                "resource_sequence": int(offer.resource_sequence),
                "gpu_mib": int(offer.offered_gpu_memory_mb),
                "expires_at_ms": int(offer.expires_at_ms),
            }
            if current is not None:
                if current != value:
                    raise ValueError("conflicting GPU offer digest")
                return
            available = (
                self.capacity_mib - self._live_held_mib(now_ms)
                - self.committed_mib()
            )
            if value["gpu_mib"] > available:
                raise ValueError("GPU offer would overlap committed/held capacity")
            self._held[digest] = value

    def validate_transition(
        self, *, offer_digest: str, request_id: str, attempt: int,
        resource_sequence: int, gpu_mib: int, now_ms: int,
    ) -> dict[str, Any]:
        with self._lock:
            self._live_held_mib(now_ms)
            held = self._held.get(offer_digest)
            if (held is None or held["request_id"] != request_id
                    or held["attempt"] != attempt
                    or held["resource_sequence"] != resource_sequence
                    or gpu_mib <= 0 or gpu_mib > held["gpu_mib"]):
                raise ValueError("GPU admission transition is not offer-bound")
            return dict(held, committed_gpu_mib=int(gpu_mib))

    def commit(self, transaction_id: str, transition: Mapping[str, Any]) -> None:
        with self._lock:
            existing = self._committed.get(transaction_id)
            normalized = dict(transition)
            if existing is not None:
                if existing != normalized:
                    raise ValueError("conflicting committed GPU projection")
                return
            offer_digest = str(normalized["offer_digest"])
            held = self._held.get(offer_digest)
            if held is None:
                if (normalized.get("provider") != self.provider
                        or normalized.get("provider_boot_epoch")
                        != self.boot_epoch
                        or int(normalized["committed_gpu_mib"])
                        > self.capacity_mib - self.committed_mib()):
                    raise ValueError(
                        "WAL GPU projection exceeds current Provider capacity")
            else:
                self._held.pop(offer_digest)
            self._committed[transaction_id] = normalized

    def release_offer(self, offer_digest: str, *, reason: str) -> None:
        del reason
        with self._lock:
            self._held.pop(offer_digest, None)

    def release_transaction(self, transaction_id: str, *, reason: str) -> None:
        del reason
        with self._lock:
            self._committed.pop(transaction_id, None)


@dataclass
class _RoleGateV2:
    selected: bool = True
    local_ready: bool = False
    input_ready: bool = False
    started: bool = False
    failed: bool = False


class DISelectionParticipant:
    """NDNSF-DI owner behind Core's generic opaque Selection seam."""

    PARTICIPANT_ID = "ndnsf-di-v2"
    PARTICIPANT_VERSION = 2
    COMMIT_SCHEMA = "ndnsf-di-selection-commit-blob-v2"

    def __init__(
        self, *, provider: str, boot_epoch: str,
        ledger: GpuMiBAdmissionLedger,
        offer_lookup: Callable[[str], Any],
        callbacks: SelectionPreparationCallbacks,
        clock_ms: Callable[[], int],
        offer_verifier: Callable[[Any], bool] | None = None,
        state_revalidator: Callable[[Any, DISelectionAssignmentV2], None]
            | None = None,
        max_workers: int = 4,
    ) -> None:
        if provider != ledger.provider or boot_epoch != ledger.boot_epoch:
            raise ValueError("DI participant and GPU ledger binding mismatch")
        self.provider = provider
        self.boot_epoch = boot_epoch
        self.ledger = ledger
        self._offer_lookup = offer_lookup
        self._offer_verifier = offer_verifier or (
            lambda value: bool(
                getattr(value, "signer_key_id", "")
                and getattr(value, "signature", "")))
        self._state_revalidator = state_revalidator
        self._callbacks = callbacks
        self._clock_ms = clock_ms
        self._executor = ThreadPoolExecutor(
            max_workers=max(1, int(max_workers)),
            thread_name_prefix="ndnsf-di-selection-prepare",
        )
        self._lock = RLock()
        self._assignments: dict[str, DISelectionAssignmentV2] = {}
        self._gates: dict[tuple[str, str], _RoleGateV2] = {}
        self._futures: list[Future] = []
        self._role_futures: dict[tuple[str, str], Future] = {}
        self._prepared_offers: dict[str, str] = {}
        self._assignment_transactions: dict[str, str] = {}
        self._terminal_roles: dict[str, set[str]] = {}
        self._completed_assignments: set[str] = set()

    @staticmethod
    def _digest_bytes(value: bytes) -> str:
        return "sha256:" + hashlib.sha256(bytes(value)).hexdigest()

    def prepare(
        self, context: Mapping[str, Any], payload: bytes,
    ) -> dict[str, bytes]:
        assignment = DISelectionAssignmentV2.from_bytes(bytes(payload))
        now_ms = int(self._clock_ms())
        offer = self._offer_lookup(assignment.offer_digest)
        if (context.get("request_id") != assignment.request_id
                or int(context.get("attempt", 0)) != assignment.attempt
                or offer is None
                or context.get("service_name") != offer.service
                or context.get("provider_identity") != self.provider
                or context.get("provider_boot_epoch") != self.boot_epoch
                or context.get("selection_payload_digest")
                != self._digest_bytes(payload)
                or assignment.provider != self.provider
                or assignment.provider_boot_epoch != self.boot_epoch
                or assignment.deadline_ms <= now_ms
                or int(context.get("expires_at_unix_ms", 0))
                < assignment.deadline_ms):
            raise ValueError("DI Selection/Core context binding mismatch")
        if (not self._offer_verifier(offer)
                or offer.digest() != assignment.offer_digest
                or offer.request_id != assignment.request_id
                or offer.attempt != assignment.attempt
                or offer.provider != assignment.provider
                or offer.boot_epoch != assignment.provider_boot_epoch
                or offer.resource_sequence != assignment.resource_sequence
                or offer.expires_at_ms <= now_ms
                or offer.accepted_deadline_ms < assignment.deadline_ms
                or any(role.role not in offer.accepted_roles
                       for role in assignment.roles)):
            raise ValueError("DI Selection is not exactly ACK-offer bound")
        if assignment.state_reuse_binding is not None:
            binding = assignment.state_reuse_binding
            if (binding.provider != assignment.provider
                    or binding.provider_boot_epoch
                    != assignment.provider_boot_epoch
                    or binding.expires_at_ms <= now_ms
                    or not any(
                        role.layer_start == binding.layer_start
                        and role.layer_end == binding.layer_end
                        for role in assignment.roles)
                    or self._state_revalidator is None):
                raise ValueError(
                    "DI derived-state reuse lacks exact live binding")
            self._state_revalidator(binding, assignment)
        transition = self.ledger.validate_transition(
            offer_digest=assignment.offer_digest,
            request_id=assignment.request_id,
            attempt=assignment.attempt,
            resource_sequence=assignment.resource_sequence,
            gpu_mib=assignment.required_gpu_mib(),
            now_ms=now_ms,
        )
        transaction_id = str(context.get("transaction_id", ""))
        if not transaction_id:
            raise ValueError("DI Selection transaction id is missing")
        acceptance = DISelectionAcceptanceV2(
            invocation_id=assignment.invocation_id,
            request_id=assignment.request_id,
            attempt=assignment.attempt,
            assignment_digest=assignment.digest(),
            provider=self.provider,
            provider_boot_epoch=self.boot_epoch,
            offer_digest=assignment.offer_digest,
            role_tuple_digest=assignment.role_tuple_digest(),
            accepted_gpu_mib=assignment.required_gpu_mib(),
            generation=assignment.generation,
            transaction_id=transaction_id,
            accepted_at_ms=now_ms,
            expires_at_ms=assignment.deadline_ms,
        )
        blob = {
            "schema": self.COMMIT_SCHEMA,
            "participant_id": self.PARTICIPANT_ID,
            "participant_version": self.PARTICIPANT_VERSION,
            "transaction_id": transaction_id,
            "assignment": base64.b64encode(assignment.to_bytes()).decode(),
            "assignment_digest": assignment.digest(),
            "transition": transition,
            "acceptance": base64.b64encode(acceptance.to_bytes()).decode(),
            "acceptance_digest": acceptance.digest(),
        }
        commit_blob = json.dumps(
            blob, sort_keys=True, separators=(",", ":")).encode()
        with self._lock:
            self._prepared_offers[transaction_id] = assignment.offer_digest
        return {
            "commit_blob": commit_blob,
            "acceptance_payload": acceptance.to_bytes(),
        }

    def on_committed(self, view: Mapping[str, Any]) -> None:
        blob_wire = bytes(view["commit_blob"])
        try:
            blob = json.loads(blob_wire.decode())
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("malformed DI commit blob") from exc
        if (blob_wire != json.dumps(
                blob, sort_keys=True, separators=(",", ":")).encode()
                or blob.get("schema") != self.COMMIT_SCHEMA
                or blob.get("participant_id") != self.PARTICIPANT_ID
                or blob.get("participant_version") != self.PARTICIPANT_VERSION
                or blob.get("transaction_id") != view.get("transaction_id")):
            raise ValueError("DI commit blob identity/canonical form mismatch")
        assignment_wire = base64.b64decode(
            blob["assignment"], validate=True)
        assignment = DISelectionAssignmentV2.from_bytes(assignment_wire)
        acceptance_wire = base64.b64decode(
            blob["acceptance"], validate=True)
        acceptance = DISelectionAcceptanceV2.from_bytes(acceptance_wire)
        acceptance.validate_assignment(
            assignment, transaction_id=str(view["transaction_id"]))
        if (assignment.digest() != blob["assignment_digest"]
                or acceptance.digest() != blob["acceptance_digest"]
                or bytes(view["acceptance_payload"]) != acceptance_wire):
            raise ValueError("DI committed assignment/acceptance mismatch")
        transaction_id = str(view["transaction_id"])
        with self._lock:
            existing = self._assignments.get(transaction_id)
            if existing is not None:
                if existing != assignment:
                    raise ValueError("conflicting DI committed projection")
                return
            self.ledger.commit(transaction_id, blob["transition"])
            self._assignments[transaction_id] = assignment
            self._assignment_transactions[assignment.digest()] = transaction_id
            self._terminal_roles[transaction_id] = set()
            for role in assignment.roles:
                self._gates[(transaction_id, role.role)] = _RoleGateV2()
                context = SelectionPreparationContext(
                    transaction_id=transaction_id,
                    invocation_id=assignment.invocation_id,
                    request_id=assignment.request_id,
                    attempt=assignment.attempt,
                    plan_digest=assignment.plan_digest,
                    provider=assignment.provider,
                    provider_boot_epoch=assignment.provider_boot_epoch,
                    deadline_ms=assignment.deadline_ms,
                    generation=assignment.generation,
                    role=role,
                )
                future = self._executor.submit(
                    self._prepare_role, context)
                self._futures.append(future)
                self._role_futures[(transaction_id, role.role)] = future

    def _prepare_role(self, context: SelectionPreparationContext) -> None:
        key = (context.transaction_id, context.role.role)
        try:
            self._callbacks.prepare_role(context)
        except Exception:
            with self._lock:
                self._gates[key].failed = True
            self._callbacks.release_role(context.role.role,
                                         "PREPARATION_FAILED")
            self._release_failed_transaction(
                context.transaction_id, reason="PREPARATION_FAILED")
            raise
        with self._lock:
            self._gates[key].local_ready = True
            self._maybe_start(key)

    def _maybe_start(self, key: tuple[str, str]) -> None:
        gate = self._gates[key]
        if (gate.selected and gate.local_ready and gate.input_ready
                and not gate.started and not gate.failed):
            gate.started = True
            self._callbacks.start_role(key[1])

    def mark_input_ready(self, role: str, *,
                         transaction_id: str | None = None) -> None:
        with self._lock:
            matches = [
                key for key in self._gates
                if key[1] == role and (
                    transaction_id is None or key[0] == transaction_id)
            ]
            if len(matches) != 1:
                raise ValueError("role input readiness is ambiguous or unknown")
            self._gates[matches[0]].input_ready = True
            self._maybe_start(matches[0])

    def roles(self, transaction_id: str | None = None) -> tuple[str, ...]:
        with self._lock:
            values = {
                key[1] for key in self._gates
                if transaction_id is None or key[0] == transaction_id
            }
            return tuple(sorted(values))

    def wait_for_preparation(self, *, timeout: float) -> None:
        deadline = __import__("time").monotonic() + timeout
        for future in tuple(self._futures):
            remaining = deadline - __import__("time").monotonic()
            if remaining <= 0:
                raise TimeoutError("DI role preparation did not finish")
            future.result(timeout=remaining)

    def transaction_for_assignment(self, payload: bytes) -> str:
        """Resolve a committed Core transaction from its exact DI assignment."""
        assignment = DISelectionAssignmentV2.from_bytes(bytes(payload))
        digest = assignment.digest()
        with self._lock:
            transaction_id = self._assignment_transactions.get(digest, "")
            if not transaction_id:
                if digest in self._completed_assignments:
                    return ""
                raise ValueError("DI assignment is not committed on this Provider")
            if self._assignments.get(transaction_id) != assignment:
                raise ValueError("DI assignment transaction binding mismatch")
            return transaction_id

    def wait_role_prepared(
        self, payload: bytes, role: str, *, timeout: float,
    ) -> str:
        """Block execution until this selected role finishes local preparation."""
        transaction_id = self.transaction_for_assignment(payload)
        if not transaction_id:
            raise ValueError("DI assignment already reached terminal state")
        with self._lock:
            future = self._role_futures.get((transaction_id, role))
        if future is None:
            raise ValueError("DI role is not part of the committed assignment")
        future.result(timeout=timeout)
        with self._lock:
            gate = self._gates.get((transaction_id, role))
            if gate is None or gate.failed or not gate.local_ready:
                raise RuntimeError("DI role preparation did not reach local-ready")
        return transaction_id

    def mark_role_terminal(
        self, payload: bytes, role: str, *, reason: str,
    ) -> bool:
        """Release only the request reservation after every local role terminates.

        Model/shard residency is deliberately untouched. The return value is
        true exactly once, when the transaction's committed GPU reservation is
        released.
        """
        assignment = DISelectionAssignmentV2.from_bytes(bytes(payload))
        digest = assignment.digest()
        with self._lock:
            if digest in self._completed_assignments:
                return False
            transaction_id = self._assignment_transactions.get(digest, "")
            if not transaction_id or self._assignments.get(transaction_id) != assignment:
                raise ValueError("DI terminal role is not assignment-bound")
            expected_roles = {item.role for item in assignment.roles}
            if role not in expected_roles:
                raise ValueError("DI terminal role is outside the assignment")
            terminal = self._terminal_roles.setdefault(transaction_id, set())
            terminal.add(role)
            if terminal != expected_roles:
                return False
            self._complete_transaction_locked(
                transaction_id, assignment, digest, reason=reason)
            return True

    def _release_failed_transaction(self, transaction_id: str, *,
                                    reason: str) -> None:
        with self._lock:
            assignment = self._assignments.get(transaction_id)
            if assignment is None:
                return
            digest = assignment.digest()
            if digest in self._completed_assignments:
                return
            self._complete_transaction_locked(
                transaction_id, assignment, digest, reason=reason)

    def _complete_transaction_locked(
        self,
        transaction_id: str,
        assignment: DISelectionAssignmentV2,
        assignment_digest: str,
        *,
        reason: str,
    ) -> None:
        self.ledger.release_transaction(transaction_id, reason=reason)
        self._completed_assignments.add(assignment_digest)
        self._assignment_transactions.pop(assignment_digest, None)
        self._assignments.pop(transaction_id, None)
        self._terminal_roles.pop(transaction_id, None)
        self._prepared_offers.pop(transaction_id, None)
        for item in assignment.roles:
            key = (transaction_id, item.role)
            self._gates.pop(key, None)
            self._role_futures.pop(key, None)

    def on_aborted(self, transaction_id: str, reason: str) -> None:
        with self._lock:
            offer_digest = self._prepared_offers.pop(transaction_id, None)
            assignment = self._assignments.get(transaction_id)
            if assignment is not None:
                digest = assignment.digest()
                self._complete_transaction_locked(
                    transaction_id, assignment, digest, reason=reason)
        if offer_digest:
            self.ledger.release_offer(offer_digest, reason=reason)


@dataclass
class DeploymentSideEffectCounters:
    verify: int = 0
    fetch: int = 0
    load: int = 0
    warm: int = 0
    reserve: int = 0
    execute: int = 0
    release: int = 0
    ready_notifications: int = 0
    ready_duplicates: int = 0
    activation_accepts: int = 0
    activation_duplicates: int = 0
    retry_exhaustions: int = 0

    def mutation_total(self) -> int:
        return self.fetch + self.load + self.warm + self.reserve + self.execute


@dataclass(frozen=True)
class PreparationCallbacks:
    verify: Callable[[DeploymentPlan, ProviderAssignment], None]
    load: Callable[[DeploymentPlan, ProviderAssignment], None]
    warm: Callable[[DeploymentPlan, ProviderAssignment], None]
    release: Callable[[DeploymentInstance], None] = lambda _instance: None


@dataclass
class BoundedExactTargetRetry:
    max_retries: int
    deadline_ms: int
    attempts: dict[str, int] = field(default_factory=dict)
    total_attempts: int = 0
    retry_attempts: int = 0
    exhausted: int = 0

    def eligible(self, target: str, *, now_ms: int) -> bool:
        return (bool(target) and now_ms < self.deadline_ms
                and self.attempts.get(target, 0) < 1 + self.max_retries)

    def record(self, target: str, *, now_ms: int) -> int:
        if not self.eligible(target, now_ms=now_ms):
            self.exhausted += 1
            raise RuntimeError("bounded retry exhausted or expired")
        previous = self.attempts.get(target, 0)
        self.attempts[target] = previous + 1
        self.total_attempts += 1
        self.retry_attempts += int(previous > 0)
        return self.attempts[target]


@dataclass
class TentativeReservation:
    requester: str
    service: str
    request_id: str
    attempt: int
    reservation_id: str
    units: int
    created_at_ms: int
    tentative_expires_at_ms: int
    state: str = "TENTATIVE"
    committed_expires_at_ms: int = 0
    release_reason: str = ""
    canonical_resource_id: str = ""
    resource_sequence: int = 0
    released_at_ms: int = 0


@dataclass(frozen=True)
class ReservationLedgerEvent:
    event_id: str
    sequence: int
    kind: str
    reservation_id: str
    request_identity: tuple[str, str, int]
    canonical_resource_id: str
    provider_boot_epoch: str
    units: int
    logical_time_ms: int
    predecessor: str
    digest: str


class AtomicReservationBook:
    """Provider-local authorization-first bounded reservation authority."""

    def __init__(self, provider: str, boot_epoch: str, *, capacity: int,
                 per_requester_limit: int, per_service_limit: int,
                 max_lease_ms: int, committed_lease_ms: int) -> None:
        if (not provider or not boot_epoch or capacity <= 0
                or min(per_requester_limit, per_service_limit, max_lease_ms,
                       committed_lease_ms) <= 0):
            raise ValueError("invalid reservation book limits")
        self.provider = provider
        self.boot_epoch = boot_epoch
        self.capacity = capacity
        self.per_requester_limit = per_requester_limit
        self.per_service_limit = per_service_limit
        self.max_lease_ms = max_lease_ms
        self.committed_lease_ms = committed_lease_ms
        self._lock = RLock()
        self._items: dict[tuple[str, str, int], TentativeReservation] = {}
        self.release_counters: dict[str, int] = {}
        self._ledger_events: list[ReservationLedgerEvent] = []

    @property
    def ledger_events(self) -> tuple[ReservationLedgerEvent, ...]:
        return tuple(self._ledger_events)

    def _append_ledger(self, kind: str, item: TentativeReservation,
                       *, logical_time_ms: int) -> ReservationLedgerEvent:
        sequence = len(self._ledger_events) + 1
        predecessor = (self._ledger_events[-1].digest
                       if self._ledger_events else "GENESIS")
        body = {
            "event_id": hashlib.sha256(
                f"{self.provider}|{self.boot_epoch}|{sequence}|{kind}|{item.reservation_id}".encode()
            ).hexdigest()[:32],
            "sequence": sequence, "kind": str(kind),
            "reservation_id": item.reservation_id,
            "request_identity": (item.requester, item.request_id, item.attempt),
            "canonical_resource_id": item.canonical_resource_id,
            "provider_boot_epoch": self.boot_epoch, "units": item.units,
            "logical_time_ms": int(logical_time_ms), "predecessor": predecessor,
        }
        digest = hashlib.sha256(json.dumps(
            body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        event = ReservationLedgerEvent(digest=digest, **body)
        self._ledger_events.append(event)
        return event

    def _live(self, now_ms: int) -> list[TentativeReservation]:
        self.expire(now_ms=now_ms)
        return [item for item in self._items.values()
                if item.state in {"TENTATIVE", "COMMITTED"}]

    def reserve(self, *, requester: str, service: str, request_id: str,
                attempt: int, units: int, now_ms: int, requested_lease_ms: int,
                authorized: bool, signature: str,
                canonical_resource_id: str = "",
                resource_sequence: int = 0) -> ReservationLease:
        if not authorized:
            raise PermissionError("reservation requires authorization")
        if (not requester or not service or not request_id or attempt <= 0
                or units <= 0 or requested_lease_ms <= 0 or not signature):
            raise ValueError("invalid reservation request")
        if bool(canonical_resource_id) != bool(resource_sequence > 0):
            raise ValueError("canonical resource identity and sequence must be paired")
        key = (requester, request_id, attempt)
        with self._lock:
            existing = self._items.get(key)
            if existing and existing.state == "TENTATIVE" and now_ms < existing.tentative_expires_at_ms:
                return self._lease(existing, signature)
            live = self._live(now_ms)
            if (sum(item.units for item in live) + units > self.capacity
                    or sum(item.units for item in live if item.requester == requester) + units
                    > self.per_requester_limit
                    or sum(item.units for item in live if item.service == service) + units
                    > self.per_service_limit):
                raise RuntimeError("reservation quota exhausted")
            reservation_id = hashlib.sha256(
                f"{requester}|{request_id}|{attempt}|{self.provider}|{self.boot_epoch}".encode()
            ).hexdigest()[:32]
            item = TentativeReservation(
                requester, service, request_id, attempt, reservation_id, units,
                now_ms, now_ms + min(requested_lease_ms, self.max_lease_ms),
                canonical_resource_id=canonical_resource_id,
                resource_sequence=resource_sequence)
            self._items[key] = item
            self._append_ledger("ACQUIRE", item, logical_time_ms=now_ms)
            return self._lease(item, signature)

    def _lease(self, item: TentativeReservation, signature: str) -> ReservationLease:
        return ReservationLease({
            "requester": item.requester, "requestId": item.request_id,
            "attempt": str(item.attempt), "provider": self.provider,
            "providerBootEpoch": self.boot_epoch,
            "reservationId": item.reservation_id, "service": item.service,
            "units": str(item.units),
            "createdAtMs": str(item.created_at_ms),
            "expiresAtMs": str(item.tentative_expires_at_ms),
            "canonicalResourceId": item.canonical_resource_id,
            "resourceSequence": str(item.resource_sequence),
            "signature": signature,
        })

    def reservation_binding(self, reservation_id: str, *, now_ms: int) -> dict:
        """Return an attempt-bound binding for Spec 130 activation validation."""
        with self._lock:
            self.expire(now_ms=now_ms)
            item = next((value for value in self._items.values()
                         if value.reservation_id == reservation_id), None)
            if item is None:
                raise RuntimeError("unknown reservation")
            return {
                "request_identity": (item.requester, item.request_id, item.attempt),
                "provider_boot_epoch": self.boot_epoch,
                "reservation_id": item.reservation_id,
                "quantity": item.units,
                "canonical_resource_id": item.canonical_resource_id,
                "resource_sequence": item.resource_sequence,
                "live": item.state in {"TENTATIVE", "COMMITTED"},
                "state": item.state,
            }

    def commit(self, reservation_id: str, *, now_ms: int) -> TentativeReservation:
        with self._lock:
            item = next((value for value in self._items.values()
                         if value.reservation_id == reservation_id), None)
            if item is None or item.state != "TENTATIVE" or now_ms >= item.tentative_expires_at_ms:
                raise RuntimeError("cannot commit absent or expired tentative reservation")
            item.state = "COMMITTED"
            item.committed_expires_at_ms = now_ms + self.committed_lease_ms
            self._append_ledger("COMMIT", item, logical_time_ms=now_ms)
            return item

    def release(self, reservation_id: str, *, reason: str,
                now_ms: int = 0) -> bool:
        with self._lock:
            item = next((value for value in self._items.values()
                         if value.reservation_id == reservation_id), None)
            if item is None or item.state == "RELEASED":
                return False
            item.state = "RELEASED"
            item.release_reason = reason
            item.released_at_ms = int(now_ms)
            self.release_counters[reason] = self.release_counters.get(reason, 0) + 1
            self._append_ledger("RELEASE", item, logical_time_ms=now_ms)
            return True

    def expire(self, *, now_ms: int) -> int:
        expired = 0
        with self._lock:
            for item in self._items.values():
                deadline = (item.committed_expires_at_ms if item.state == "COMMITTED"
                            else item.tentative_expires_at_ms)
                if item.state in {"TENTATIVE", "COMMITTED"} and now_ms >= deadline:
                    item.state = "RELEASED"
                    item.release_reason = "LEASE_EXPIRED"
                    item.released_at_ms = int(now_ms)
                    self.release_counters["LEASE_EXPIRED"] = (
                        self.release_counters.get("LEASE_EXPIRED", 0) + 1)
                    self._append_ledger("EXPIRE", item, logical_time_ms=now_ms)
                    expired += 1
        return expired

    def live_units(self, *, now_ms: int) -> int:
        with self._lock:
            return sum(item.units for item in self._live(now_ms))

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "provider": self.provider,
                "boot_epoch": self.boot_epoch,
                "items": [item.__dict__.copy() for item in self._items.values()],
                "release_counters": dict(self.release_counters),
                "ledger_events": [event.__dict__.copy()
                                  for event in self._ledger_events],
            }

    def restore(self, snapshot: dict, *, now_ms: int) -> int:
        if (snapshot.get("provider") != self.provider
                or snapshot.get("boot_epoch") != self.boot_epoch):
            raise ValueError("reservation journal provider epoch mismatch")
        with self._lock:
            self._items.clear()
            for raw in snapshot.get("items", ()):
                item = TentativeReservation(**dict(raw))
                self._items[(item.requester, item.request_id, item.attempt)] = item
            self.release_counters = {
                str(key): int(value)
                for key, value in dict(snapshot.get("release_counters", {})).items()
            }
            self._ledger_events = []
            previous = "GENESIS"
            for expected, raw in enumerate(snapshot.get("ledger_events", ()), 1):
                value = dict(raw)
                value["request_identity"] = tuple(value["request_identity"])
                event = ReservationLedgerEvent(**value)
                body = event.__dict__.copy(); digest = body.pop("digest")
                computed = hashlib.sha256(json.dumps(
                    body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
                if (event.sequence != expected or event.predecessor != previous
                        or event.provider_boot_epoch != self.boot_epoch
                        or digest != computed):
                    raise ValueError("reservation ledger gap, fork, or digest mismatch")
                self._ledger_events.append(event); previous = event.digest
            return self.expire(now_ms=now_ms)

    def shutdown(self) -> int:
        with self._lock:
            live = [item for item in self._items.values()
                    if item.state in {"TENTATIVE", "COMMITTED"}]
            for item in live:
                self.release(item.reservation_id, reason="PROVIDER_SHUTDOWN")
            return len(live)

    def ownership_intervals(self) -> list[dict]:
        """Derive half-open Provider-authoritative intervals from its ledger."""
        acquired: dict[str, ReservationLedgerEvent] = {}
        intervals = []
        for event in self._ledger_events:
            if event.kind == "ACQUIRE":
                acquired[event.reservation_id] = event
            elif event.kind in {"RELEASE", "EXPIRE"}:
                start = acquired.pop(event.reservation_id, None)
                if start is not None:
                    intervals.append({
                        "reservation_id": event.reservation_id,
                        "request_identity": start.request_identity,
                        "canonical_resource_id": start.canonical_resource_id,
                        "start_sequence": start.sequence,
                        "end_sequence": event.sequence,
                        "start_ms": start.logical_time_ms,
                        "end_ms": event.logical_time_ms,
                        "half_open": True,
                    })
        for reservation_id, start in acquired.items():
            intervals.append({
                "reservation_id": reservation_id,
                "request_identity": start.request_identity,
                "canonical_resource_id": start.canonical_resource_id,
                "start_sequence": start.sequence, "end_sequence": None,
                "start_ms": start.logical_time_ms, "end_ms": None,
                "half_open": True,
            })
        return intervals


class ReservationDecisionAuthority:
    """Applies the first valid immutable decision to an exact reservation."""

    def __init__(self, book: AtomicReservationBook) -> None:
        self.book = book
        self._accepted: dict[str, tuple[str, str]] = {}

    def apply(self, decision: SelectionDecision, *, now_ms: int,
              signature_valid: bool = True) -> SelectionDecisionReceipt:
        fields = decision.fields
        if not signature_valid:
            raise PermissionError("invalid Selection signature")
        reservation_id = fields.get("reservationId", "")
        value = fields.get("decision", "")
        digest = decision.digest()
        if not reservation_id or value not in {"SELECTED", "NOT_SELECTED"}:
            raise ValueError("malformed Selection decision")
        previous = self._accepted.get(reservation_id)
        if previous is not None:
            if previous != (value, digest):
                raise RuntimeError("conflicting immutable Selection decision")
            state = "COMMITTED" if value == "SELECTED" else "RELEASED"
        elif value == "SELECTED":
            self.book.commit(reservation_id, now_ms=now_ms)
            self._accepted[reservation_id] = (value, digest)
            state = "COMMITTED"
        else:
            self.book.release(reservation_id, reason="NOT_SELECTED")
            self._accepted[reservation_id] = (value, digest)
            state = "RELEASED"
        return SelectionDecisionReceipt({
            "decisionDigest": digest, "reservationId": reservation_id,
            "state": state, "provider": self.book.provider,
            "providerBootEpoch": self.book.boot_epoch,
        })


class AckWindowDecisionCoordinator:
    """Atomic ACK closure plus bounded late-ACK tombstone."""

    def __init__(self, request_id: str, attempt: int, *, ack_deadline_ms: int,
                 retain_until_ms: int) -> None:
        if (not request_id or attempt <= 0 or ack_deadline_ms <= 0
                or retain_until_ms < ack_deadline_ms):
            raise ValueError("invalid ACK window")
        self.request_id = request_id
        self.attempt = attempt
        self.ack_deadline_ms = ack_deadline_ms
        self.retain_until_ms = retain_until_ms
        self.closed = False
        self._leases: dict[str, ReservationLease] = {}
        self._decisions: dict[str, SelectionDecision] = {}

    def admit(self, lease: ReservationLease, *, completed_at_ms: int,
              authenticated: bool = True) -> SelectionDecision | None:
        if not authenticated:
            return None
        reservation_id = lease.fields.get("reservationId", "")
        if not reservation_id:
            raise ValueError("ACK lease lacks reservation identity")
        if self.closed or completed_at_ms >= self.ack_deadline_ms:
            return self._make_decision(lease, "NOT_SELECTED")
        self._leases.setdefault(reservation_id, lease)
        return None

    def close(self, *, now_ms: int, selected_reservation_ids: set[str]) -> tuple[SelectionDecision, ...]:
        if now_ms < self.ack_deadline_ms:
            raise ValueError("ACK window cannot close early")
        if self.closed:
            return tuple(self._decisions.values())
        unknown = selected_reservation_ids - set(self._leases)
        if unknown:
            raise ValueError("selected reservation was not an eligible ACK")
        self.closed = True
        for reservation_id, lease in self._leases.items():
            self._decisions[reservation_id] = self._make_decision(
                lease, "SELECTED" if reservation_id in selected_reservation_ids
                else "NOT_SELECTED")
        return tuple(self._decisions.values())

    def _make_decision(self, lease: ReservationLease, value: str) -> SelectionDecision:
        fields = lease.fields
        return SelectionDecision({
            "decision": value,
            "requestId": self.request_id,
            "attempt": str(self.attempt),
            "targetProvider": fields.get("provider", ""),
            "providerBootEpoch": fields.get("providerBootEpoch", ""),
            "reservationId": fields.get("reservationId", ""),
            "reservationDigest": lease.digest(),
            "expiresAtMs": fields.get("expiresAtMs", ""),
        })

    def tombstone(self) -> SelectionDecisionTombstone:
        if not self.closed:
            raise RuntimeError("decision set is not closed")
        selected = sorted(key for key, decision in self._decisions.items()
                          if decision.fields["decision"] == "SELECTED")
        return SelectionDecisionTombstone({
            "requestId": self.request_id, "attempt": str(self.attempt),
            "selectedReservationIds": ",".join(selected),
            "retainUntilMs": str(self.retain_until_ms),
        })


class SelectionGatedProvider:
    """One Provider's canonical no-mutation-before-Selection state machine."""

    def __init__(self, provider: str, boot_epoch: str,
                 capability: Callable[[DeploymentIntent], ProviderCapabilityOffer],
                 preparation: PreparationCallbacks,
                 activation_verifier: Callable[[ExecutionActivateMessage], bool],
                 reservation_book: AtomicReservationBook | None = None,
                 reservation_authorizer: Callable[[DeploymentIntent], bool] | None = None) -> None:
        self.provider = provider
        self.boot_epoch = boot_epoch
        self._capability = capability
        self._preparation = preparation
        self._activation_verifier = activation_verifier
        self._reservation_book = reservation_book
        self._reservation_authorizer = reservation_authorizer
        self.counters = DeploymentSideEffectCounters()
        self.instances: dict[tuple[str, str], DeploymentInstance] = {}
        self._gates: dict[tuple[str, str], ProviderActivationGate] = {}
        self._ready_digests: dict[tuple[str, str], str] = {}

    def acknowledge(self, intent: DeploymentIntent, *, now_ms: int) -> ProviderCapabilityOffer:
        if now_ms >= intent.deadline_ms:
            raise ValueError("deployment intent expired")
        before = self.counters.mutation_total()
        offer = self._capability(intent)
        if (offer.provider != self.provider or offer.provider_boot_epoch != self.boot_epoch
                or offer.expires_at_ms <= now_ms):
            raise ValueError("capability offer binding or expiry failed")
        if self.counters.mutation_total() != before:
            raise RuntimeError("REQUEST/ACK capability handling caused deployment side effects")
        return offer

    def acknowledge_reservation(self, intent: DeploymentIntent, *, service: str,
                                units: int, lease_ms: int, now_ms: int,
                                signature: str) -> ReservationLease:
        if self._reservation_book is None or self._reservation_authorizer is None:
            raise RuntimeError("DI reservation capability is not configured")
        if now_ms >= intent.deadline_ms:
            raise ValueError("deployment intent expired")
        authorized = bool(self._reservation_authorizer(intent))
        lease = self._reservation_book.reserve(
            requester=intent.requester_identity, service=service,
            request_id=intent.request_id, attempt=intent.attempt,
            units=units, now_ms=now_ms, requested_lease_ms=lease_ms,
            authorized=authorized, signature=signature)
        self.counters.reserve = self._reservation_book.live_units(now_ms=now_ms)
        return lease

    def select(self, plan: DeploymentPlan, role: str, *, now_ms: int) -> DeploymentInstance:
        if now_ms >= plan.deadline_ms:
            raise ValueError("Selection DeploymentPlan expired")
        assignment = next((item for item in plan.assignments
                           if item.provider == self.provider and item.role == role), None)
        if assignment is None or assignment.provider_boot_epoch != self.boot_epoch:
            raise ValueError("Selection does not bind this Provider role and boot epoch")
        key = (plan.digest(), role)
        current = self.instances.get(key)
        if current is not None and current.state in {
                DeploymentInstanceState.READY, DeploymentInstanceState.ACTIVE}:
            return current
        instance_id = hashlib.sha256(
            f"{plan.digest()}|{self.provider}|{self.boot_epoch}|{role}".encode()).hexdigest()[:32]
        instance = DeploymentInstance(instance_id, plan.digest(), self.provider,
                                      self.boot_epoch, role,
                                      DeploymentInstanceState.SELECTED, 0,
                                      plan.deadline_ms)
        self.counters.reserve += 1
        try:
            instance = instance.transition(DeploymentInstanceState.VERIFYING)
            self.counters.verify += 1
            self._preparation.verify(plan, assignment)
            instance = instance.transition(DeploymentInstanceState.LOADING)
            self.counters.load += 1
            self._preparation.load(plan, assignment)
            instance = instance.transition(DeploymentInstanceState.WARMING)
            self.counters.warm += 1
            self._preparation.warm(plan, assignment)
            instance = instance.transition(DeploymentInstanceState.READY)
        except Exception as exc:
            instance = instance.transition(DeploymentInstanceState.FAILED, reason=str(exc))
            self._preparation.release(instance)
            self.counters.release += 1
            self.instances[key] = instance
            raise
        self.instances[key] = instance
        self._gates[key] = ProviderActivationGate(
            plan, self.provider, role, verifier=self._activation_verifier)
        return instance

    def ready_message(self, plan: DeploymentPlan, role: str, *, sequence: int,
                      expires_at_ms: int, signer: str, signature: str) -> ProviderReadyMessage:
        instance = self.instances.get((plan.digest(), role))
        if instance is None or instance.state is not DeploymentInstanceState.READY:
            raise RuntimeError("Provider role is not locally READY")
        message = ProviderReadyMessage(
            plan.request_id, plan.attempt, plan.selection_digest, plan.digest(),
            self.provider, self.boot_epoch, role, plan.artifact_digests[0],
            instance.instance_id, sequence, expires_at_ms, signer, signature)
        previous = self._ready_digests.get((plan.digest(), role))
        if previous:
            self.counters.ready_duplicates += 1
        else:
            self._ready_digests[(plan.digest(), role)] = message.digest()
            self.counters.ready_notifications += 1
        return message

    def activate(self, plan: DeploymentPlan, role: str,
                 message: ExecutionActivateMessage, *, now_ms: int) -> bool:
        key = (plan.digest(), role)
        instance = self.instances.get(key)
        gate = self._gates.get(key)
        if instance is None or gate is None or instance.state not in {
                DeploymentInstanceState.READY, DeploymentInstanceState.ACTIVE}:
            raise RuntimeError("activation requires a READY DeploymentInstance")
        first = gate.validate(message, now_ms=now_ms)
        if first:
            self.instances[key] = instance.transition(DeploymentInstanceState.ACTIVE)
            self.counters.activation_accepts += 1
        else:
            self.counters.activation_duplicates += 1
        return first

    def expire(self, plan: DeploymentPlan, role: str) -> None:
        """Release a non-terminal prepared instance after bounded exhaustion."""
        key = (plan.digest(), role)
        instance = self.instances.get(key)
        if instance is None or instance.state in {
                DeploymentInstanceState.COMPLETED,
                DeploymentInstanceState.RELEASED}:
            return
        expired = instance.transition(DeploymentInstanceState.EXPIRED)
        self._preparation.release(expired)
        self.counters.release += 1
        self.instances[key] = expired.transition(DeploymentInstanceState.RELEASED)

    def execute(self, plan: DeploymentPlan, role: str,
                operation: Callable[[], bytes]) -> bytes:
        key = (plan.digest(), role)
        instance = self.instances.get(key)
        if instance is None or instance.state is not DeploymentInstanceState.ACTIVE:
            raise RuntimeError("execution requires accepted ExecutionActivateMessage")
        self.instances[key] = instance.transition(DeploymentInstanceState.EXECUTING)
        self.counters.execute += 1
        try:
            result = bytes(operation())
        except Exception as exc:
            self.instances[key] = self.instances[key].transition(
                DeploymentInstanceState.FAILED, reason=str(exc))
            raise
        self.instances[key] = self.instances[key].transition(
            DeploymentInstanceState.COMPLETED)
        return result


class DeploymentControlJournal:
    """V2-only writer with one observable, bounded legacy import seam."""

    PLAN_KIND = "deployment-plan-v2"
    INSTANCE_KIND = "deployment-instance-v2"

    def __init__(self, journal) -> None:
        self.journal = journal
        self.legacy_import_count = 0

    def write_plan(self, plan: DeploymentPlan) -> None:
        self.journal.append(self.PLAN_KIND, plan.to_dict())

    def write_instance(self, instance: DeploymentInstance) -> None:
        self.journal.append(self.INSTANCE_KIND, instance.to_dict())

    def restore(self, *, legacy_importer: Callable[[dict], tuple[
            DeploymentPlan, Iterable[DeploymentInstance]]] | None = None
                ) -> tuple[dict[str, DeploymentPlan], dict[str, DeploymentInstance]]:
        plans: dict[str, DeploymentPlan] = {}
        instances: dict[str, DeploymentInstance] = {}
        migrated: list[tuple[DeploymentPlan, tuple[DeploymentInstance, ...]]] = []
        for record in self.journal.records():
            kind, payload = record["kind"], dict(record["payload"])
            if kind == self.PLAN_KIND:
                plan = DeploymentPlan.from_bytes(
                    json.dumps(payload, sort_keys=True, separators=(",", ":")).encode())
                plans[plan.digest()] = plan
            elif kind == self.INSTANCE_KIND:
                instance = DeploymentInstance.from_bytes(
                    json.dumps(payload, sort_keys=True, separators=(",", ":")).encode())
                instances[instance.instance_id] = instance
            elif kind == "deployment-state" and legacy_importer is not None:
                plan, imported_instances = legacy_importer(payload)
                migrated.append((plan, tuple(imported_instances)))
                self.legacy_import_count += 1
        # v1 records are read once and rewritten through the sole v2 writer.
        for plan, imported_instances in migrated:
            if plan.digest() not in plans:
                self.write_plan(plan)
                plans[plan.digest()] = plan
            for instance in imported_instances:
                if instance.instance_id not in instances:
                    self.write_instance(instance)
                    instances[instance.instance_id] = instance
        return plans, instances

"""Spec 129 workload-neutral selection, preparation, and activation authority.

This module is intentionally transport-neutral.  Core C++ carries the signed
wire messages; DI adapters supply capability and model lifecycle callbacks.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
from typing import Callable
from typing import Iterable
from threading import RLock

from .contracts import (
    DeploymentInstance, DeploymentInstanceState, DeploymentIntent,
    DeploymentPlan, ExecutionActivateMessage,
    ProviderAssignment, ProviderCapabilityOffer, ProviderReadyMessage,
    ReservationLease,
    SelectionDecision, SelectionDecisionReceipt, SelectionDecisionTombstone,
)
from .execution import ProviderActivationGate


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

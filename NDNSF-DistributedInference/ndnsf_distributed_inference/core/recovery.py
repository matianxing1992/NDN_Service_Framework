"""Bounded, attempt-fenced recovery independent of APP policy."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
import json
import logging
import random
from threading import RLock
from typing import Any, Callable, Iterable, Mapping

from .contracts import AssignmentContext, OrphanCleanupRecord, ResultRendezvousRecord
from .ports import CheckpointRecord, ProgressRecord, RecoveryProposal
from .decision_validation import validate_recovery


_LOG = logging.getLogger("ndnsf.di.recovery")


class ContentionRetryController:
    """Bounded full-jitter retry after every partial lease is resolved."""

    def __init__(self, *, max_attempts: int, total_deadline_ms: int,
                 base_backoff_ms: int, max_backoff_ms: int,
                 seed: int | None = None, started_at_ms: int = 0) -> None:
        if (max_attempts <= 0 or total_deadline_ms <= 0 or base_backoff_ms <= 0
                or max_backoff_ms < base_backoff_ms):
            raise ValueError("invalid contention retry bounds")
        self.max_attempts = int(max_attempts)
        if started_at_ms < 0:
            raise ValueError("invalid contention retry start")
        self.deadline_ms = int(started_at_ms) + int(total_deadline_ms)
        self.base_ms = int(base_backoff_ms)
        self.cap_ms = int(max_backoff_ms)
        self._random = random.Random(seed)
        self.attempt = 0
        self.awaiting: dict[str, int] = {}
        self.release_receipts = 0
        self.expiry_fallbacks = 0
        self.backoffs: list[int] = []
        self.exhausted = 0

    def begin(self, *, now_ms: int) -> int:
        if self.awaiting:
            raise RuntimeError("partial reservations are not resolved")
        if self.attempt >= self.max_attempts or now_ms >= self.deadline_ms:
            self.exhausted += 1
            raise RuntimeError("contention retry exhausted")
        self.attempt += 1
        return self.attempt

    def close_partial(self, reservations: Mapping[str, int], *,
                      send_not_selected: Callable[[str], None]) -> None:
        if self.awaiting:
            raise RuntimeError("previous partial release is still pending")
        for reservation_id, expires_at_ms in reservations.items():
            if not reservation_id or int(expires_at_ms) <= 0:
                raise ValueError("invalid partial reservation")
            send_not_selected(str(reservation_id))
            self.awaiting[str(reservation_id)] = int(expires_at_ms)

    def accept_receipt(self, reservation_id: str) -> bool:
        if reservation_id not in self.awaiting: return False
        self.awaiting.pop(reservation_id); self.release_receipts += 1
        return True

    def next_backoff(self, *, now_ms: int) -> int:
        expired = [key for key, expiry in self.awaiting.items() if now_ms >= expiry]
        for key in expired:
            self.awaiting.pop(key); self.expiry_fallbacks += 1
        if self.awaiting:
            raise RuntimeError("waiting for release receipt or lease expiry")
        if self.attempt >= self.max_attempts or now_ms >= self.deadline_ms:
            self.exhausted += 1
            raise RuntimeError("contention retry exhausted")
        window = min(self.cap_ms, self.base_ms * (2 ** max(0, self.attempt - 1)))
        delay = self._random.randint(0, window)
        if now_ms + delay >= self.deadline_ms:
            self.exhausted += 1
            raise RuntimeError("contention retry exceeds total deadline")
        self.backoffs.append(delay)
        return delay


class RecoveryReason(str, Enum):
    PROVIDER_LOST = "PROVIDER_LOST"
    STRAGGLER_DEADLINE = "STRAGGLER_DEADLINE"
    TELEMETRY_STALE = "TELEMETRY_STALE"
    CACHE_MISS_FULL_CONTEXT_REQUIRED = "CACHE_MISS_FULL_CONTEXT_REQUIRED"
    NO_COMPATIBLE_REPLACEMENT = "NO_COMPATIBLE_REPLACEMENT"
    REQUEST_DEADLINE = "REQUEST_DEADLINE"


@dataclass(frozen=True)
class RecoveryAttempt:
    request_id: str
    attempt_epoch: int
    provider: str
    remaining_deadline_ms: int


@dataclass(frozen=True)
class RecoveryAction:
    action: str
    request_id: str
    attempt_epoch: int
    provider: str = ""
    remaining_deadline_ms: int = 0
    terminal_reason: RecoveryReason | None = None
    full_context_required: bool = False
    control_payloads: tuple[dict[str, object], ...] = ()


class BoundedRecoveryController:
    def __init__(self, request_id: str, *, request_deadline_ms: int,
                 started_at_ms: int, max_replacements: int = 1) -> None:
        if not request_id or request_deadline_ms <= started_at_ms or max_replacements < 0:
            raise ValueError("recovery requires identity, future deadline and bound")
        self.request_id = request_id
        self.request_deadline_ms = int(request_deadline_ms)
        self.started_at_ms = int(started_at_ms)
        self.max_replacements = int(max_replacements)
        self._attempt_epoch = 0
        self._provider = ""
        self._replacements = 0
        self._terminal = False
        self._terminal_reason: RecoveryReason | None = None
        self._excluded: set[str] = set()
        self._events: list[dict[str, object]] = []
        self._visible_output_epoch = 0
        self._lock = RLock()

    def start(self, provider: str) -> RecoveryAttempt:
        with self._lock:
            if self._attempt_epoch or not provider:
                raise ValueError("recovery starts exactly once")
            self._attempt_epoch = 1; self._provider = provider
            self._record("ATTEMPT_STARTED", provider=provider)
            return RecoveryAttempt(self.request_id, 1, provider,
                                   self.request_deadline_ms - self.started_at_ms)

    def recover(self, reason: RecoveryReason | str, *, at_ms: int,
                replacement_provider: str = "",
                excluded_providers: Iterable[str] = ()) -> RecoveryAction:
        with self._lock:
            if not self._attempt_epoch:
                raise RuntimeError("recovery controller has not started")
            reason = RecoveryReason(reason)
            remaining = max(0, self.request_deadline_ms - int(at_ms))
            self._excluded.update(str(item) for item in excluded_providers)
            if self._terminal:
                return self._failure(self._terminal_reason or reason, remaining)
            if remaining <= 0:
                return self._terminate(RecoveryReason.REQUEST_DEADLINE, remaining)
            cache_retry = reason is RecoveryReason.CACHE_MISS_FULL_CONTEXT_REQUIRED
            next_provider = self._provider if cache_retry else replacement_provider
            if (self._replacements >= self.max_replacements or not next_provider
                    or next_provider in self._excluded):
                return self._terminate(RecoveryReason.NO_COMPATIBLE_REPLACEMENT, remaining)
            old_epoch, old_provider = self._attempt_epoch, self._provider
            self._replacements += 1; self._attempt_epoch += 1; self._provider = next_provider
            controls = tuple({
                "schema": "ndnsf-di-execution-control-v1",
                "transport": "existing-di-service-payload",
                "operation": operation,
                "requestId": self.request_id,
                "attemptEpoch": old_epoch,
                "providerName": old_provider,
                "supersededByAttemptEpoch": self._attempt_epoch,
            } for operation in ("CANCEL", "SUPERSEDE"))
            action = "retry-full-context" if cache_retry else "replace"
            self._record(
                "REPLACEMENT_DECISION", reason=reason.value, action=action,
                oldAttemptEpoch=old_epoch, provider=next_provider,
                remainingDeadlineMs=remaining)
            return RecoveryAction(
                action,
                self.request_id, self._attempt_epoch, next_provider, remaining,
                full_context_required=cache_retry, control_payloads=controls)

    def accept_result(self, attempt_epoch: int, payload: bytes) -> bool:
        del payload
        with self._lock:
            if self._terminal or int(attempt_epoch) != self._attempt_epoch:
                self._record(
                    "RESULT_REJECTED", observedAttemptEpoch=int(attempt_epoch),
                    reason="OLD_OR_DUPLICATE_ATTEMPT")
                return False
            self._terminal = True
            self._record("RESULT_AUTHORITATIVE", observedAttemptEpoch=int(attempt_epoch))
            return True

    def apply_transition(self, proposal: RecoveryProposal, *, at_ms: int,
                         progress: ProgressRecord | None = None,
                         checkpoint: CheckpointRecord | None = None) -> RecoveryAction:
        """Apply a policy-selected transition without delegating Core fences."""
        validate_recovery(
            proposal, current_attempt=self._attempt_epoch,
            original_deadline_ms=self.request_deadline_ms)
        if progress is not None:
            if progress.request_id != self.request_id or progress.attempt_epoch != self._attempt_epoch:
                raise ValueError("stale or foreign progress record")
            if progress.output_epoch < self._visible_output_epoch:
                raise ValueError("visible output epoch regressed")
            self._visible_output_epoch = progress.output_epoch
        if checkpoint is not None:
            if (checkpoint.request_id != self.request_id
                    or checkpoint.attempt_epoch != self._attempt_epoch
                    or checkpoint.output_epoch < self._visible_output_epoch):
                raise ValueError("checkpoint is stale or torn")
        reason = (RecoveryReason.CACHE_MISS_FULL_CONTEXT_REQUIRED
                  if proposal.action == "retry-full-context"
                  else RecoveryReason.PROVIDER_LOST)
        return self.recover(
            reason, at_ms=at_ms,
            replacement_provider=proposal.replacement_provider)

    def events(self) -> tuple[dict[str, object], ...]:
        with self._lock:
            return tuple(dict(item) for item in self._events)

    def _terminate(self, reason: RecoveryReason, remaining: int) -> RecoveryAction:
        self._terminal = True; self._terminal_reason = reason
        self._record(
            "TERMINAL_FAILURE", reason=reason.value,
            remainingDeadlineMs=remaining)
        return self._failure(reason, remaining)

    def _failure(self, reason: RecoveryReason, remaining: int) -> RecoveryAction:
        return RecoveryAction("fail", self.request_id, self._attempt_epoch,
                              self._provider, remaining, reason)

    def _record(self, event: str, **fields: object) -> None:
        record = {
            "event": event,
            "requestId": self.request_id,
            "attemptEpoch": self._attempt_epoch,
            **fields,
        }
        self._events.append(record)
        _LOG.info("NDNSF_DI_RECOVERY %s", json.dumps(
            record, sort_keys=True, separators=(",", ":")))


class ResultRendezvousStore:
    """Durable-store-shaped exactly-one terminal result authority.

    The backing mapping may be replaced by an APP persistence adapter.  Core
    owns fencing and identity semantics, not storage policy.
    """

    def __init__(self, backing: dict[tuple[str, str], ResultRendezvousRecord] | None = None) -> None:
        self._records = backing if backing is not None else {}
        self._high_watermarks: dict[tuple[str, str], int] = {
            key: record.attempt_epoch for key, record in self._records.items()
        }
        self._lock = RLock()

    def publish(self, record: ResultRendezvousRecord) -> ResultRendezvousRecord:
        key = (record.requester_identity, record.request_id)
        with self._lock:
            current = self._records.get(key)
            high = self._high_watermarks.get(key, 0)
            if record.attempt_epoch < high:
                raise ValueError("stale result attempt is fenced")
            if current is not None:
                if current == record:
                    return current
                if current.visible:
                    raise ValueError("terminal result is already visible")
            self._high_watermarks[key] = record.attempt_epoch
            self._records[key] = record
            return record

    def resume(self, requester_identity: str, request_id: str) -> ResultRendezvousRecord | None:
        with self._lock:
            return self._records.get((requester_identity, request_id))

    def fence_attempt(self, requester_identity: str, request_id: str,
                      attempt_epoch: int) -> None:
        key = (requester_identity, request_id)
        with self._lock:
            current = self._high_watermarks.get(key, 0)
            if attempt_epoch < current:
                raise ValueError("attempt high watermark cannot regress")
            self._high_watermarks[key] = attempt_epoch


class OrphanResourceRegistry:
    """Provider-owned periodic cleanup for all DI resource categories."""

    RESOURCE_KINDS = (
        "leases", "reservations", "sessions", "cache_pins", "runner_handles")

    def __init__(self, provider: str, provider_boot_epoch: str, *,
                 tombstone_ttl_ms: int = 60_000) -> None:
        if not provider or not provider_boot_epoch or tombstone_ttl_ms <= 0:
            raise ValueError("orphan registry requires provider identity and TTL")
        self.provider = provider
        self.provider_boot_epoch = provider_boot_epoch
        self.tombstone_ttl_ms = tombstone_ttl_ms
        self._resources: dict[str, dict[str, tuple[int, str, int]]] = {
            kind: {} for kind in self.RESOURCE_KINDS}
        self._high_watermarks: dict[str, int] = {}
        self._lock = RLock()

    def retain(self, kind: str, resource_id: str, *, expires_at_ms: int,
               request_id: str, attempt_epoch: int) -> None:
        if kind not in self._resources or not resource_id or expires_at_ms <= 0:
            raise ValueError("invalid orphan-managed resource")
        with self._lock:
            high = self._high_watermarks.get(request_id, 0)
            if attempt_epoch < high:
                raise ValueError("stale operation is fenced by cleanup tombstone")
            self._resources[kind][resource_id] = (
                int(expires_at_ms), request_id, int(attempt_epoch))

    def sweep(self, *, at_ms: int, sweep_id: str,
              cleanup: Callable[[str, str], None] | None = None) -> OrphanCleanupRecord:
        if at_ms <= 0 or not sweep_id:
            raise ValueError("cleanup sweep requires timestamp and identity")
        reclaimed: dict[str, tuple[str, ...]] = {}
        with self._lock:
            for kind, resources in self._resources.items():
                expired = sorted(resource_id for resource_id, (expiry, _, _) in resources.items()
                                 if expiry <= at_ms)
                for resource_id in expired:
                    _, request_id, attempt_epoch = resources.pop(resource_id)
                    self._high_watermarks[request_id] = max(
                        self._high_watermarks.get(request_id, 0), attempt_epoch)
                    if cleanup is not None:
                        cleanup(kind, resource_id)
                reclaimed[kind] = tuple(expired)
            return OrphanCleanupRecord(
                self.provider, self.provider_boot_epoch, sweep_id, int(at_ms),
                reclaimed, dict(self._high_watermarks),
                int(at_ms) + self.tombstone_ttl_ms)

    def restart(self, provider_boot_epoch: str, *, at_ms: int,
                sweep_id: str) -> OrphanCleanupRecord:
        if not provider_boot_epoch or provider_boot_epoch == self.provider_boot_epoch:
            raise ValueError("restart requires a new Provider boot epoch")
        with self._lock:
            for resources in self._resources.values():
                for resource_id, (_, request_id, attempt_epoch) in tuple(resources.items()):
                    self._high_watermarks[request_id] = max(
                        self._high_watermarks.get(request_id, 0), attempt_epoch)
                    resources[resource_id] = (at_ms, request_id, attempt_epoch)
            self.provider_boot_epoch = provider_boot_epoch
        return self.sweep(at_ms=at_ms, sweep_id=sweep_id)


def replan_assignment_context(context: AssignmentContext, *, role_providers,
                              newly_excluded=()) -> AssignmentContext:
    """Advance attempt while preserving original deadline and exclusion lineage."""
    excluded = tuple(sorted(set(context.excluded_providers) | set(newly_excluded)))
    return AssignmentContext(
        context.request_id, context.attempt_epoch + 1, context.plan_digest,
        context.model_variant_id, tuple(sorted(dict(role_providers).items())),
        context.original_deadline_ms, excluded)

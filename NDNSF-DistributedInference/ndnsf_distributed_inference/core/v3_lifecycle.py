"""Provider-local V3 queue and just-in-time device admission.

This module is intentionally independent of the legacy V2 reservation book.
ACK inspection only returns an observational offer.  A Selection creates a
queue record without a GPU hold; the complete device set is acquired once,
atomically, immediately before load/execution.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
import hashlib
import time
from typing import Callable, Mapping

from ..sdk.placement import (
    DeviceBinding, DeviceBindingMode, ExecutionDisposition,
    PlacementPlanCoreV3, ProviderOfferV3, ProviderSelectionProjectionV3,
    RoleAssemblySpec, canonical_digest, is_cpu_backend,
)


class V3LifecycleState(str, Enum):
    SELECTION_VALIDATED = "SELECTION_VALIDATED"
    QUEUE_ACCEPTED = "QUEUE_ACCEPTED"
    HOST_PREPARING = "HOST_PREPARING"
    HOST_READY = "HOST_READY"
    DEVICE_ADMISSION_PENDING = "DEVICE_ADMISSION_PENDING"
    DEVICE_ADMITTED = "DEVICE_ADMITTED"
    COMPLETED = "COMPLETED"
    CANCELLED = "CANCELLED"
    FAILED = "FAILED"


@dataclass(frozen=True)
class V3FencingToken:
    provider: str
    boot_epoch: str
    resource_sequence: int
    token: str

    @classmethod
    def issue(cls, provider: str, boot_epoch: str,
              resource_sequence: int) -> "V3FencingToken":
        if not provider or not boot_epoch or resource_sequence <= 0:
            raise ValueError("invalid V3 fencing identity")
        token = hashlib.sha256(
            f"{provider}|{boot_epoch}|{resource_sequence}".encode()).hexdigest()
        return cls(provider, boot_epoch, resource_sequence, token)


@dataclass
class V3QueueRecord:
    request_id: str
    attempt: int
    provider: str
    roles: tuple[RoleAssemblySpec, ...]
    device_binding: DeviceBinding | None = None
    state: V3LifecycleState = V3LifecycleState.SELECTION_VALIDATED
    device_set: tuple[str, ...] = ()
    fencing_token: V3FencingToken | None = None
    progress: float = 0.0
    last_progress_ms: int = field(default_factory=lambda: int(time.time() * 1000))


class V3AdmissionController:
    """Fail-closed V3 lifecycle with no ACK-time reservation side effects."""

    def __init__(self, provider: str, *, boot_epoch: str,
                 visible_devices: tuple[str, ...] = (),
                 max_queue_records: int = 1024) -> None:
        if not provider or not boot_epoch or max_queue_records <= 0:
            raise ValueError("provider and boot epoch are required")
        self.provider = provider
        self.boot_epoch = boot_epoch
        self.visible_devices = tuple(visible_devices)
        self.max_queue_records = int(max_queue_records)
        self._resource_sequence = 0
        self._records: dict[tuple[str, int], V3QueueRecord] = {}
        self._held: set[str] = set()
        self.events: list[tuple[str, str]] = []

    @property
    def queue_records(self) -> tuple[V3QueueRecord, ...]:
        return tuple(self._records.values())

    @property
    def held_devices(self) -> tuple[str, ...]:
        return tuple(sorted(self._held))

    def observe_ack(self, offer: ProviderOfferV3) -> ProviderOfferV3:
        if offer.provider != self.provider or offer.boot_epoch != self.boot_epoch:
            raise ValueError("V3 ACK Provider/boot binding mismatch")
        # Deliberately no queue or held-device mutation here.
        self.events.append((offer.request_id, "ACK_OBSERVED"))
        return offer

    def accept_selection(
        self, plan: PlacementPlanCoreV3,
        projection: ProviderSelectionProjectionV3,
        offer: ProviderOfferV3,
    ) -> V3QueueRecord:
        if projection.provider != self.provider or projection.plan_digest == "":
            raise ValueError("V3 Selection Provider binding mismatch")
        if (projection.request_id != plan.request_id
                or projection.attempt != plan.attempt
                or projection.plan_core_digest != plan.plan_core_digest
                or projection.ack_closed_digest != plan.ack_closed_digest):
            raise ValueError("V3 Selection plan binding mismatch")
        if (offer.provider != self.provider
                or offer.request_id != plan.request_id
                or offer.attempt != plan.attempt
                or projection.offer_digest != offer.digest()):
            raise ValueError("V3 Selection offer binding mismatch")
        if (projection.device_binding.topology_profile_digest
                != offer.topology.digest()
                or projection.device_binding.resource_snapshot_digest
                != canonical_digest(offer.resources)):
            raise ValueError("V3 Selection resource binding mismatch")
        role_names = tuple(item.role for item in projection.roles)
        roles = tuple(
            role for role in projection.roles
            if plan.provider_by_role.get(
                role.role if role_names.count(role.role) == 1
                else f"{role.role}#{role.rank}") == self.provider)
        if not roles:
            raise ValueError("V3 Selection contains no local role")
        if offer.execution_disposition == ExecutionDisposition.REJECT or not offer.status:
            raise ValueError("negative V3 ACK cannot be selected")
        for role in roles:
            exact = any(
                proof.role == role.role and proof.rank == role.rank
                and proof.artifact_digest == role.artifact_digest
                for proof in offer.residency)
            if (not exact and offer.execution_disposition
                    != ExecutionDisposition.ACCEPT_WITH_PREPARATION):
                raise ValueError("exact reuse Selection lacks exact residency proof")
        key = (plan.request_id, plan.attempt)
        if key in self._records:
            return self._records[key]
        active = sum(
            record.state not in {
                V3LifecycleState.COMPLETED,
                V3LifecycleState.CANCELLED,
                V3LifecycleState.FAILED,
            }
            for record in self._records.values())
        if active >= self.max_queue_records:
            raise ValueError("V3 Selection queue is full")
        record = V3QueueRecord(
            plan.request_id, plan.attempt, self.provider, roles,
            device_binding=projection.device_binding,
            state=V3LifecycleState.QUEUE_ACCEPTED)
        self._records[key] = record
        self.events.append((plan.request_id, "QUEUE_ACCEPTED"))
        return record

    def mark_host_preparing(self, request_id: str, attempt: int) -> None:
        record = self._record(request_id, attempt)
        self._transition(record, V3LifecycleState.HOST_PREPARING)

    def mark_host_ready(self, request_id: str, attempt: int) -> None:
        record = self._record(request_id, attempt)
        if record.state not in {V3LifecycleState.QUEUE_ACCEPTED,
                                V3LifecycleState.HOST_PREPARING}:
            raise ValueError("V3 host readiness transition is out of order")
        self._transition(record, V3LifecycleState.HOST_READY)

    def admit_devices(self, request_id: str, attempt: int,
                      device_set: tuple[str, ...],
                      *, resource_sequence: int) -> V3FencingToken:
        record = self._record(request_id, attempt)
        if record.state not in {V3LifecycleState.HOST_READY,
                                V3LifecycleState.QUEUE_ACCEPTED}:
            raise ValueError("V3 device admission requires host readiness")
        devices = tuple(device_set)
        if len(record.roles) != 1 or record.device_binding is None:
            raise ValueError("V3 admission requires one sealed role/device binding")
        binding = record.device_binding
        if resource_sequence != binding.resource_sequence:
            raise ValueError("V3 device resource snapshot is stale")
        if binding.mode is DeviceBindingMode.CPU:
            if devices or not is_cpu_backend(record.roles[0].backend):
                raise ValueError("V3 CPU admission cannot use an accelerator")
        elif (devices != (binding.offer_scoped_device_handle,)
              or is_cpu_backend(record.roles[0].backend)):
            raise ValueError(
                "V3 single-device admission requires its exact offer handle")
        if any(device not in self.visible_devices for device in devices):
            raise ValueError("V3 device set is not visible")
        if set(devices) & self._held:
            raise ValueError("V3 device set is busy")
        self._transition(record, V3LifecycleState.DEVICE_ADMISSION_PENDING)
        # Atomic set acquisition: either all devices enter the held set or none.
        self._held.update(devices)
        self._resource_sequence = max(self._resource_sequence, resource_sequence)
        token = V3FencingToken.issue(self.provider, self.boot_epoch,
                                     self._resource_sequence)
        record.device_set = devices
        record.fencing_token = token
        self._transition(record, V3LifecycleState.DEVICE_ADMITTED)
        return token

    def complete(self, request_id: str, attempt: int,
                 token: V3FencingToken) -> None:
        record = self._record(request_id, attempt)
        if record.state != V3LifecycleState.DEVICE_ADMITTED:
            raise ValueError("V3 request is not device-admitted")
        self._validate_token(record, token)
        self._held.difference_update(record.device_set)
        self._transition(record, V3LifecycleState.COMPLETED)

    def cancel(self, request_id: str, attempt: int, reason: str = "cancelled") -> None:
        record = self._record(request_id, attempt)
        if record.fencing_token is not None:
            self._held.difference_update(record.device_set)
        record.progress = 0.0
        self._transition(record, V3LifecycleState.CANCELLED, reason)

    def report_progress(self, request_id: str, attempt: int, progress: float) -> None:
        """Record monotonic data-driven progress for deadline decisions."""
        record = self._record(request_id, attempt)
        if record.state in {V3LifecycleState.COMPLETED, V3LifecycleState.CANCELLED,
                            V3LifecycleState.FAILED}:
            raise ValueError("terminal V3 request cannot report progress")
        value = float(progress)
        if value < record.progress or value < 0.0 or value > 1.0:
            raise ValueError("V3 progress must be monotonic in [0,1]")
        record.progress = value
        record.last_progress_ms = int(time.time() * 1000)
        self.events.append((record.request_id, f"PROGRESS:{value:.6f}"))

    def no_progress_expired(self, request_id: str, attempt: int,
                            *, now_ms: int, no_progress_ms: int) -> bool:
        if no_progress_ms < 0:
            raise ValueError("no-progress bound must be non-negative")
        record = self._record(request_id, attempt)
        return int(now_ms) - record.last_progress_ms > int(no_progress_ms)

    def _record(self, request_id: str, attempt: int) -> V3QueueRecord:
        try:
            return self._records[(request_id, attempt)]
        except KeyError as exc:
            raise ValueError("unknown V3 Selection") from exc

    def _validate_token(self, record: V3QueueRecord, token: V3FencingToken) -> None:
        if (record.fencing_token is None or token != record.fencing_token
                or token.boot_epoch != self.boot_epoch):
            raise ValueError("stale or substituted V3 fencing token")

    def _transition(self, record: V3QueueRecord, state: V3LifecycleState,
                    reason: str = "") -> None:
        record.state = state
        record.last_progress_ms = int(time.time() * 1000)
        self.events.append((record.request_id, state.value + (":" + reason if reason else "")))


__all__ = ["V3AdmissionController", "V3FencingToken", "V3LifecycleState", "V3QueueRecord"]

"""Provider-local multi-device queue and just-in-time admission for Spec 170."""

from __future__ import annotations

from dataclasses import dataclass
from threading import RLock
from typing import Mapping


@dataclass(frozen=True)
class DeviceJobV3:
    request_id: str
    role: str
    required_memory_mb: int
    preferred_device: str = ""

    def __post_init__(self) -> None:
        if (not self.request_id or not self.role
                or self.required_memory_mb <= 0):
            raise ValueError("invalid V3 device job")


@dataclass(frozen=True)
class DeviceAdmissionV3:
    request_id: str
    role: str
    device: str
    fencing_sequence: int


class MultiDeviceSchedulerV3:
    """Schedule independent SINGLE_DEVICE roles without ACK-time holds.

    ``submit`` is queue-only and therefore safe to call while processing an
    ACK. Device memory is consumed only by ``admit`` immediately before the
    Provider starts execution. Admission is atomic for each job and chooses a
    deterministic visible device; two devices are never silently pooled for an
    unsplittable role.
    """

    def __init__(self, device_memory_mb: Mapping[str, int], *, max_queue: int = 1024) -> None:
        devices = {str(device): int(memory) for device, memory in device_memory_mb.items()}
        if (not devices or any(not device or memory <= 0 for device, memory in devices.items())
                or max_queue <= 0):
            raise ValueError("invalid V3 device scheduler topology")
        self._capacity = devices
        self._used = {device: 0 for device in devices}
        self._active: dict[str, DeviceAdmissionV3] = {}
        self._active_memory: dict[str, int] = {}
        self._queue: dict[str, DeviceJobV3] = {}
        self._sequence = 0
        self._max_queue = int(max_queue)
        self._lock = RLock()

    @property
    def queued(self) -> tuple[DeviceJobV3, ...]:
        with self._lock:
            return tuple(self._queue.values())

    @property
    def active(self) -> tuple[DeviceAdmissionV3, ...]:
        with self._lock:
            return tuple(self._active.values())

    @property
    def used_memory_mb(self) -> dict[str, int]:
        with self._lock:
            return dict(self._used)

    def submit(self, job: DeviceJobV3) -> None:
        with self._lock:
            if job.request_id in self._queue or job.request_id in self._active:
                raise ValueError("duplicate V3 device request")
            if len(self._queue) >= self._max_queue:
                raise RuntimeError("V3 device queue is full")
            candidates = self._candidate_devices(job)
            if not candidates:
                raise ValueError("no single visible device can fit V3 role")
            # This operation deliberately changes only the queue.
            self._queue[job.request_id] = job

    def admit(self, request_id: str) -> DeviceAdmissionV3:
        with self._lock:
            try:
                job = self._queue.pop(request_id)
            except KeyError as exc:
                raise ValueError("unknown or already admitted V3 request") from exc
            candidates = self._candidate_devices(job)
            if not candidates:
                # Put the job back: no partial device set or lost work.
                self._queue[request_id] = job
                raise RuntimeError("V3 device capacity became unavailable")
            device = candidates[0]
            self._sequence += 1
            admission = DeviceAdmissionV3(
                request_id=request_id, role=job.role, device=device,
                fencing_sequence=self._sequence)
            self._used[device] += job.required_memory_mb
            self._active[request_id] = admission
            self._active_memory[request_id] = job.required_memory_mb
            return admission

    def complete(self, request_id: str) -> None:
        with self._lock:
            try:
                admission = self._active.pop(request_id)
            except KeyError as exc:
                raise ValueError("unknown V3 active request") from exc
            job_memory = self._active_memory.pop(request_id)
            self._used[admission.device] -= job_memory

    def _candidate_devices(self, job: DeviceJobV3) -> list[str]:
        candidates = [
            device for device in sorted(self._capacity)
            if (not job.preferred_device or device == job.preferred_device)
            and self._capacity[device] - self._used[device]
            >= job.required_memory_mb
        ]
        return candidates


__all__ = ["DeviceAdmissionV3", "DeviceJobV3", "MultiDeviceSchedulerV3"]

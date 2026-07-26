"""Core-owned state binding and fencing mechanisms."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from threading import RLock
from typing import Any


class TerminalReasonV1(str, Enum):
    NONE = "NONE"
    PROVIDER_LOST = "PROVIDER_LOST"
    STRAGGLER_DEADLINE = "STRAGGLER_DEADLINE"
    DEPENDENCY_MISSING = "DEPENDENCY_MISSING"
    DEPENDENCY_HASH_MISMATCH = "DEPENDENCY_HASH_MISMATCH"
    PLAN_STALE = "PLAN_STALE"
    TELEMETRY_STALE = "TELEMETRY_STALE"
    CACHE_MISS_FULL_CONTEXT_REQUIRED = "CACHE_MISS_FULL_CONTEXT_REQUIRED"
    ATTEMPT_CANCELLED = "ATTEMPT_CANCELLED"
    NO_COMPATIBLE_REPLACEMENT = "NO_COMPATIBLE_REPLACEMENT"
    REQUEST_DEADLINE = "REQUEST_DEADLINE"


@dataclass(frozen=True)
class ExecutionAttemptV1:
    request_id: str
    attempt_epoch: int
    plan_id: str
    terminal_reason: TerminalReasonV1 = TerminalReasonV1.NONE

    def __post_init__(self) -> None:
        if (not self.request_id or not self.plan_id
                or self.attempt_epoch not in (0, 1)):
            raise ValueError("invalid execution attempt")


@dataclass(frozen=True)
class StateBinding:
    request_id: str
    attempt_epoch: int
    plan_digest: str
    provider: str
    provider_boot_epoch: str
    security_epoch: int
    cache_epoch: int
    session_id: str = ""

    def __post_init__(self) -> None:
        if (not self.request_id or self.attempt_epoch <= 0 or not self.plan_digest
                or not self.provider or not self.provider_boot_epoch
                or self.security_epoch < 0 or self.cache_epoch < 0):
            raise ValueError("invalid Core state binding")

    def key(self) -> tuple[str, str, str]:
        return (self.request_id, self.provider, self.session_id)


class BoundStateStore:
    """Small mechanism store; values are valid only under an exact binding."""

    def __init__(self) -> None:
        self._lock = RLock()
        self._values: dict[tuple[str, str, str], tuple[StateBinding, Any]] = {}
        self._attempt_high_watermark: dict[str, int] = {}

    def put(self, binding: StateBinding, value: Any) -> None:
        with self._lock:
            highest = self._attempt_high_watermark.get(binding.request_id, 0)
            if binding.attempt_epoch < highest:
                raise ValueError("stale attempt cannot bind state")
            self._attempt_high_watermark[binding.request_id] = max(
                highest, binding.attempt_epoch)
            self._values[binding.key()] = (binding, value)

    def get(self, binding: StateBinding) -> Any | None:
        with self._lock:
            item = self._values.get(binding.key())
            return item[1] if item is not None and item[0] == binding else None

    def fence_attempt(self, request_id: str, attempt_epoch: int) -> int:
        if not request_id or attempt_epoch <= 0:
            raise ValueError("invalid attempt fence")
        with self._lock:
            current = self._attempt_high_watermark.get(request_id, 0)
            if attempt_epoch < current:
                raise ValueError("attempt fence regressed")
            self._attempt_high_watermark[request_id] = attempt_epoch
            stale = [key for key, (binding, _) in self._values.items()
                     if binding.request_id == request_id
                     and binding.attempt_epoch < attempt_epoch]
            for key in stale:
                self._values.pop(key, None)
            return len(stale)

    def invalidate_provider_boot(self, provider: str, boot_epoch: str) -> int:
        with self._lock:
            stale = [key for key, (binding, _) in self._values.items()
                     if binding.provider == provider
                     and binding.provider_boot_epoch != boot_epoch]
            for key in stale:
                self._values.pop(key, None)
            return len(stale)

    def size(self) -> int:
        with self._lock:
            return len(self._values)

"""Core-owned state binding and fencing mechanisms."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from threading import RLock
from typing import Any


class FailureBoundaryV1(str, Enum):
    """Primary lifecycle ownership for one classified terminal failure."""

    ENVIRONMENT = "ENVIRONMENT"
    BOOTSTRAP = "BOOTSTRAP"
    ROUTING = "ROUTING"
    ACK = "ACK"
    PLAN = "PLAN"
    REPO_PUBLISH = "REPO_PUBLISH"
    REPO_FETCH = "REPO_FETCH"
    PREP = "PREP"
    DEPENDENCY = "DEPENDENCY"
    EXEC = "EXEC"
    TOKEN = "TOKEN"
    RESPONSE = "RESPONSE"
    CLEANUP = "CLEANUP"
    ANALYZER = "ANALYZER"
    UNRESOLVED = "UNRESOLVED"


_FAILURE_CODE_BOUNDARIES = {
    "ENV_": FailureBoundaryV1.ENVIRONMENT,
    "BOOT_": FailureBoundaryV1.BOOTSTRAP,
    "ROUTE_": FailureBoundaryV1.ROUTING,
    "ACK_": FailureBoundaryV1.ACK,
    "PLAN_": FailureBoundaryV1.PLAN,
    "REPO_PUBLISH_": FailureBoundaryV1.REPO_PUBLISH,
    "REPO_FETCH_": FailureBoundaryV1.REPO_FETCH,
    "PREP_": FailureBoundaryV1.PREP,
    "DEPENDENCY_": FailureBoundaryV1.DEPENDENCY,
    "EXEC_": FailureBoundaryV1.EXEC,
    "TOKEN_": FailureBoundaryV1.TOKEN,
    "RESPONSE_": FailureBoundaryV1.RESPONSE,
    "CLEANUP_": FailureBoundaryV1.CLEANUP,
    "ANALYZER_": FailureBoundaryV1.ANALYZER,
}


def failure_boundary_for_code(code: str) -> FailureBoundaryV1:
    """Map one exact failure code to its primary lifecycle boundary."""

    value = str(code or "")
    if value == "UNRESOLVED_EVIDENCE_GAP":
        return FailureBoundaryV1.UNRESOLVED
    for prefix, boundary in _FAILURE_CODE_BOUNDARIES.items():
        if value.startswith(prefix) and len(value) > len(prefix):
            return boundary
    raise ValueError("failure code does not identify an exact lifecycle boundary")


@dataclass(frozen=True)
class FailureRecordV1:
    """Durable primary-boundary failure record with a causal checkpoint."""

    request_id: str
    attempt_epoch: int
    component: str
    failure_code: str
    last_checkpoint: str
    terminal_reason: str
    boundary: FailureBoundaryV1 | str | None = None
    provider: str = ""
    role: str = ""
    operation_id: str = ""
    artifact_range: str = ""
    evidence_paths: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not self.request_id or self.attempt_epoch <= 0 or not self.component:
            raise ValueError("failure record identity is incomplete")
        if not self.last_checkpoint or not self.terminal_reason:
            raise ValueError("failure record requires last checkpoint and reason")
        inferred = failure_boundary_for_code(self.failure_code)
        boundary = inferred if self.boundary is None else FailureBoundaryV1(
            self.boundary)
        if boundary is not inferred:
            raise ValueError("failure code and primary boundary disagree")
        object.__setattr__(self, "boundary", boundary)
        paths = tuple(str(path) for path in self.evidence_paths)
        if any(not path for path in paths):
            raise ValueError("failure evidence paths must be non-empty")
        object.__setattr__(self, "evidence_paths", paths)

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": "ndnsf-di.failure-record.v1",
            "requestId": self.request_id,
            "attemptEpoch": self.attempt_epoch,
            "component": self.component,
            "boundary": self.boundary.value,
            "failureCode": self.failure_code,
            "provider": self.provider,
            "role": self.role,
            "operationId": self.operation_id,
            "artifactRange": self.artifact_range,
            "lastCheckpoint": self.last_checkpoint,
            "terminalReason": self.terminal_reason,
            "evidencePaths": list(self.evidence_paths),
        }

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "FailureRecordV1":
        expected = {
            "schema", "requestId", "attemptEpoch", "component", "boundary",
            "failureCode", "provider", "role", "operationId", "artifactRange",
            "lastCheckpoint", "terminalReason", "evidencePaths",
        }
        if set(payload) != expected or payload.get("schema") != (
                "ndnsf-di.failure-record.v1"):
            raise ValueError("failure record schema mismatch")
        return cls(
            request_id=str(payload["requestId"]),
            attempt_epoch=int(payload["attemptEpoch"]),
            component=str(payload["component"]),
            failure_code=str(payload["failureCode"]),
            last_checkpoint=str(payload["lastCheckpoint"]),
            terminal_reason=str(payload["terminalReason"]),
            boundary=str(payload["boundary"]),
            provider=str(payload["provider"]),
            role=str(payload["role"]),
            operation_id=str(payload["operationId"]),
            artifact_range=str(payload["artifactRange"]),
            evidence_paths=tuple(str(path) for path in payload["evidencePaths"]),
        )


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

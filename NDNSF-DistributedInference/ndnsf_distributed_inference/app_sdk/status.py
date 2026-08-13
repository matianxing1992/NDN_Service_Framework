"""Stable APP lifecycle and durable-request status schemas."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from ..core.state import FailureBoundaryV1, failure_boundary_for_code


class RevisionState(str, Enum):
    RESOLVED = "RESOLVED"; READY = "READY"; ACTIVE = "ACTIVE"
    DRAINING = "DRAINING"; INACTIVE = "INACTIVE"; DELETED = "DELETED"
    RETIRED = "RETIRED"; FAILED = "FAILED"


class InstancePhase(str, Enum):
    PENDING = "PENDING"; STAGING = "STAGING"; READY = "READY"
    ACTIVE = "ACTIVE"; DRAINING = "DRAINING"; STOPPED = "STOPPED"


class RequestState(str, Enum):
    CREATED = "CREATED"
    PLANNING = "PLANNING"
    PREPARING = "PREPARING"
    CERTIFIED = "CERTIFIED"
    EXECUTING = "EXECUTING"
    COMPLETED = "COMPLETED"
    FAILED = "FAILED"
    CANCELLED = "CANCELLED"
    EXPIRED = "EXPIRED"

    # Bounded source compatibility for callers that consumed the pre-Spec-111
    # process-local vocabulary.  New journal records always use the canonical
    # names above.
    PENDING = CREATED
    RUNNING = EXECUTING
    SUCCEEDED = COMPLETED
    CANCELED = CANCELLED

    @classmethod
    def _missing_(cls, value):
        return {
            "PENDING": cls.CREATED,
            "RUNNING": cls.EXECUTING,
            "SUCCEEDED": cls.COMPLETED,
            "CANCELED": cls.CANCELLED,
        }.get(value)


@dataclass(frozen=True)
class StatusCondition:
    kind: str; status: bool; reason_code: str; message: str = ""


@dataclass(frozen=True)
class RequestEvent:
    request_id: str; sequence: int; state: RequestState; timestamp_ms: int
    result_digest: str = ""; reason_code: str = ""
    failure_boundary: FailureBoundaryV1 | None = None
    last_checkpoint: str = ""


@dataclass(frozen=True)
class RequestFailureStatus:
    """Status projection used by operators and analyzers for one failure."""

    request_id: str
    state: RequestState
    failure_code: str
    last_checkpoint: str
    terminal_reason: str
    boundary: FailureBoundaryV1 | str | None = None

    def __post_init__(self) -> None:
        if not self.request_id or not self.last_checkpoint or not self.terminal_reason:
            raise ValueError("failure status identity/checkpoint is incomplete")
        inferred = failure_boundary_for_code(self.failure_code)
        actual = inferred if self.boundary is None else FailureBoundaryV1(
            self.boundary)
        if actual is not inferred:
            raise ValueError("failure status boundary disagrees with code")
        if self.state not in {
            RequestState.FAILED, RequestState.EXPIRED,
            RequestState.CANCELLED,
        }:
            raise ValueError("failure status requires a terminal failure state")
        object.__setattr__(self, "boundary", actual)


__all__ = [name for name in globals() if not name.startswith("_")]

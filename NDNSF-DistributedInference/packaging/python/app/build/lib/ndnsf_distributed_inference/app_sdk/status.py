"""Stable APP lifecycle and durable-request status schemas."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


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


__all__ = [name for name in globals() if not name.startswith("_")]

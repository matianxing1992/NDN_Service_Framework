"""Append-only invocation lineage admission for NDNSF-DI evidence."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any, Optional


@dataclass(frozen=True)
class InvocationIdentity:
    request_id: str
    attempt: int
    plan_id: str
    selection_digest: str
    model_identity_digest: str


class LineageLedger:
    def __init__(self, identity: InvocationIdentity) -> None:
        if not identity.request_id or not identity.model_identity_digest:
            raise ValueError("request and model identity are required")
        self.identity = identity
        self.events: list[dict[str, Any]] = []
        self.terminal = False
        self._last_operation_position: dict[str, tuple[int, int]] = {}

    def append(
        self,
        *,
        event_type: str,
        identity: InvocationIdentity,
        authenticated: bool,
        provider: str = "",
        role: str = "",
        operation_id: str = "",
        epoch: Optional[int] = None,
        sequence: Optional[int] = None,
        terminal: bool = False,
        evidence_digest: str = "",
    ) -> dict[str, Any]:
        reasons: list[str] = []
        if self.terminal:
            reasons.append("post-terminal")
        if not authenticated:
            reasons.append("unauthenticated")
        for field in (
            "request_id",
            "attempt",
            "plan_id",
            "selection_digest",
            "model_identity_digest",
        ):
            if getattr(identity, field) != getattr(self.identity, field):
                reasons.append("wrong-" + field.replace("_", "-"))
        if operation_id:
            if epoch is None or sequence is None or epoch < 0 or sequence < 0:
                reasons.append("invalid-operation-position")
            else:
                position = (epoch, sequence)
                previous = self._last_operation_position.get(operation_id)
                if previous is not None and position <= previous:
                    reasons.append("duplicate-or-reordered")

        admitted = not reasons
        event = {
            "ordinal": len(self.events),
            "eventType": event_type,
            "identity": asdict(identity),
            "provider": provider,
            "role": role,
            "operationId": operation_id,
            "epoch": epoch,
            "sequence": sequence,
            "authenticated": authenticated,
            "admitted": admitted,
            "rejectionReason": ",".join(reasons),
            "evidenceDigest": evidence_digest,
        }
        self.events.append(event)
        if admitted and operation_id:
            self._last_operation_position[operation_id] = (epoch, sequence)  # type: ignore[arg-type]
        if admitted and terminal:
            self.terminal = True
        return event

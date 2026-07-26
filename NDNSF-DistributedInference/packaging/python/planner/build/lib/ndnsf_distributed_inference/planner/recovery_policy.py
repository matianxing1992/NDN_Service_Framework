"""Transition-only bounded recovery policy."""

from __future__ import annotations

from ..core.ports import PolicyRequest, PolicyResult, RecoveryProposal
from .defaults import _result


class BoundedRecoveryPolicy:
    def transition(self, request: PolicyRequest) -> PolicyResult:
        attempt = int(request.metadata["attempt_epoch"])
        max_attempts = int(request.metadata.get("max_attempts", attempt + 1))
        action = "retry" if attempt < max_attempts else "fail"
        value = RecoveryProposal(
            action, attempt + 1, int(request.metadata["original_deadline_ms"]),
            str(request.metadata.get("replacement_provider", "")),
            str(request.metadata.get("checkpoint_digest", "")))
        return _result("recovery", request, value, self.__class__.__name__)


__all__ = ["BoundedRecoveryPolicy"]

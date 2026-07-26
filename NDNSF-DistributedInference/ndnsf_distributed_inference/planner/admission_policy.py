"""Scoped admission defaults that cannot weaken Core/provider floors."""

from __future__ import annotations

from ..core.ports import AdmissionProposal, AdmissionScope, PolicyRequest, PolicyResult
from .defaults import _result


class FloorPreservingAdmissionPolicy:
    def admit(self, request: PolicyRequest) -> PolicyResult:
        scope = request.scope.value if isinstance(request.scope, AdmissionScope) else request.scope
        accepted = not bool(request.metadata.get("prior_rejected", False))
        value = AdmissionProposal(
            AdmissionScope(scope or AdmissionScope.ENGINE_REQUEST.value),
            accepted, "DEFAULT_ACCEPT" if accepted else "PRIOR_REJECTION")
        return _result("admission", request, value, self.__class__.__name__)


__all__ = ["FloorPreservingAdmissionPolicy"]

"""Scoped scheduling parity defaults."""

from __future__ import annotations

from ..core.ports import PolicyRequest, PolicyResult, SchedulingProposal, SchedulingScope
from .defaults import _result


class ScopedFifoSchedulingPolicy:
    def dispatch(self, request: PolicyRequest) -> PolicyResult:
        scope = request.scope.value if isinstance(request.scope, SchedulingScope) else request.scope
        scope_value = SchedulingScope(scope or SchedulingScope.REQUEST_DAG.value)
        max_batch = int(request.metadata.get("adapter_max_batch", 1))
        requested = int(request.metadata.get("batch_size", 1))
        batch = min(max(1, requested), max(1, max_batch))
        value = SchedulingProposal(
            scope_value, tuple(str(item) for item in request.candidates), batch,
            bool(request.metadata.get("preempt", False)),
            tuple(request.metadata.get("authorized_hedge_roles", ())),
        )
        return _result("scheduling", request, value, self.__class__.__name__)


__all__ = ["ScopedFifoSchedulingPolicy"]

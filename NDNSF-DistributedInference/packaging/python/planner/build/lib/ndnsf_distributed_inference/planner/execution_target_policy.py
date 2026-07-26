"""Bounded execution-target selection from advertised alternatives."""

from __future__ import annotations

from ..core.ports import ExecutionTargetProposal, PolicyRequest, PolicyResult
from .defaults import _result


class AdvertisedExecutionTargetPolicy:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        if not request.candidates:
            raise ValueError("no execution target alternatives")
        candidates = tuple(item for item in request.candidates
                           if isinstance(item, ExecutionTargetProposal))
        required = set(request.metadata.get("required_capabilities", ()))
        capabilities = request.metadata.get("capabilities_by_adapter", {})
        supported = tuple(item for item in candidates
                          if required.issubset(set(capabilities.get(item.adapter_name, ()))))
        if not supported:
            raise ValueError("no compatible execution target")
        return _result("execution_target", request, supported[0],
                       self.__class__.__name__)


__all__ = ["AdvertisedExecutionTargetPolicy"]

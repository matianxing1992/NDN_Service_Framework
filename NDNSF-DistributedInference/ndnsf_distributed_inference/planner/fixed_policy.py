"""Deterministic fixed assignment used by tests and explicit deployments."""

from __future__ import annotations

from typing import Mapping

from ..core.ports import AssignmentProposal, PolicyRequest, PolicyResult
from .defaults import _result


class FixedProviderAssignmentPolicy:
    def __init__(self, providers_by_role: Mapping[str, str]) -> None:
        if not providers_by_role:
            raise ValueError("fixed assignment cannot be empty")
        self.providers_by_role = dict(providers_by_role)

    def propose(self, request: PolicyRequest) -> PolicyResult:
        value = AssignmentProposal(
            assignment_id=str(request.metadata.get("assignment_id", "fixed")),
            plan_id=str(request.metadata["plan_id"]),
            model_variant_id=str(request.metadata["variant_id"]),
            providers_by_role=self.providers_by_role,
        )
        return _result("provider_assignment", request, value,
                       self.__class__.__name__)


__all__ = ["FixedProviderAssignmentPolicy"]

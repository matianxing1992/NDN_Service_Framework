"""Provider assignment policies, including Runtime-v1 cost parity."""

from __future__ import annotations

from ..core.ports import AssignmentProposal, PolicyRequest, PolicyResult, ProviderCandidate
from .defaults import _result


class CostProviderAssignmentPolicy:
    """Select the lowest advertised score per role with deterministic ties."""

    version = "runtime-v1-assignment-parity-v1"

    def propose(self, request: PolicyRequest) -> PolicyResult:
        roles = tuple(str(item) for item in request.metadata.get("roles", ()))
        candidates = tuple(item for item in request.candidates
                           if isinstance(item, ProviderCandidate))
        mapping = {}
        for role in roles:
            eligible = tuple(item for item in candidates if role in item.roles)
            if not eligible:
                raise ValueError(f"no valid provider for role {role}")
            mapping[role] = min(eligible, key=lambda item: (item.score, item.provider)).provider
        value = AssignmentProposal(
            assignment_id=str(request.metadata.get("assignment_id", "cost")),
            plan_id=str(request.metadata["plan_id"]),
            model_variant_id=str(request.metadata["variant_id"]),
            providers_by_role=mapping,
            affinity=dict(request.metadata.get("affinity", {})),
        )
        return _result("provider_assignment", request, value,
                       self.__class__.__name__)


class AckSelectionProviderAssignmentPolicy(CostProviderAssignmentPolicy):
    """One-role First/Random/AllSelected compatibility without hidden authority."""

    def __init__(self, strategy: str = "FirstResponding") -> None:
        if strategy not in {"FirstResponding", "RandomSelection", "AllSelected"}:
            raise ValueError("unknown ACK selection strategy")
        self.strategy = strategy


__all__ = ["CostProviderAssignmentPolicy", "AckSelectionProviderAssignmentPolicy"]

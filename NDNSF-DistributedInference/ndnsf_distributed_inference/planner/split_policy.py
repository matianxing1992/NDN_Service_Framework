"""Candidate-budgeted sequential split reference planner."""

from __future__ import annotations

from ..core.ports import PlanCandidate, PlanCandidateSet, PolicyRequest, PolicyResult
from .defaults import _result


class SequentialPartitionPlanner:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        plans = tuple(item for item in request.candidates
                      if isinstance(item, PlanCandidate))
        if len(plans) > request.budget.max_candidates:
            plans = tuple(sorted(plans, key=lambda item: (item.estimated_cost,
                                                           item.plan_id))[
                          :request.budget.max_candidates])
        value = PlanCandidateSet(plans, request.budget)
        return _result("partition", request, value, self.__class__.__name__)


__all__ = ["SequentialPartitionPlanner"]

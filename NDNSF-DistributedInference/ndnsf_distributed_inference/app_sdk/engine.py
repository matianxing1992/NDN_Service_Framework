"""Process-local APP-owned NDNSF-DI optimization engine."""

from __future__ import annotations

from typing import Iterable, Mapping, Any

from ..core.decision_validation import (
    validate_candidate_budget, validate_model_selection, validate_objective,
    validate_policy_result, validate_snapshot,
)
from ..core.ports import (
    CandidateBudget, EngineSnapshot, ModelCandidate, OptimizationObjective,
    PolicyRequest,
)
from ..sdk.executor import BoundedPolicyExecutor


class DistributedInferenceEngine:
    def __init__(self, suite, *, executor=None) -> None:
        self.suite = suite
        self.executor = executor or BoundedPolicyExecutor()
        self._last_snapshot_epoch = 0
        self._decision_epoch = 0

    def decision_epoch(self, snapshot: EngineSnapshot) -> int:
        if snapshot.epoch < self._last_snapshot_epoch:
            raise ValueError("snapshot epoch regressed")
        if snapshot.epoch > self._last_snapshot_epoch:
            self._decision_epoch += 1
            self._last_snapshot_epoch = snapshot.epoch
        return self._decision_epoch

    def choose_model(self, *, objective: OptimizationObjective,
                     snapshot: EngineSnapshot,
                     candidates: Iterable[ModelCandidate],
                     budget: CandidateBudget,
                     exact_required: bool = True,
                     at_ms: int | None = None) -> ModelCandidate:
        validate_objective(objective)
        validate_snapshot(snapshot, at_ms=(snapshot.captured_at_ms if at_ms is None else at_ms))
        values = validate_candidate_budget(candidates, budget)
        request = PolicyRequest(objective, snapshot, budget, values)
        policy = self.suite.policy("model_variant")
        result = self.executor.execute(
            policy.propose, request, budget.max_policy_ms)
        validate_policy_result(
            result, expected_kind="model_variant", snapshot_epoch=snapshot.epoch)
        return validate_model_selection(
            result.value, values, exact_required=exact_required)

    def run_decision_graph(self, requests: Mapping[str, PolicyRequest]) -> dict[str, Any]:
        """Invoke every supplied policy once in the declared process-local DAG.

        Core validation is always applied after an implementation returns. The
        APP caller remains responsible for constructing least-input requests and
        performing type-specific Core validation before preparing an execution
        intent.
        """
        ordered = (
            "deployment", "model_variant", "partition", "provider_assignment",
            "scheduling", "admission", "execution_tuning", "cache",
            "recovery", "execution_target",
        )
        results: dict[str, Any] = {}
        for kind in ordered:
            if kind not in requests:
                continue
            request = requests[kind]
            validate_objective(request.objective)
            validate_snapshot(request.snapshot, at_ms=request.snapshot.captured_at_ms)
            validate_candidate_budget(request.candidates, request.budget)
            policy = self.suite.policy(kind)
            method_name = "dispatch" if kind == "scheduling" else (
                "admit" if kind == "admission" else (
                    "transition" if kind == "recovery" else "propose"))
            result = self.executor.execute(
                getattr(policy, method_name), request, request.budget.max_policy_ms)
            results[kind] = validate_policy_result(
                result, expected_kind=kind, snapshot_epoch=request.snapshot.epoch)
        self.decision_epoch(next(iter(requests.values())).snapshot) if requests else None
        return results


__all__ = ["DistributedInferenceEngine"]

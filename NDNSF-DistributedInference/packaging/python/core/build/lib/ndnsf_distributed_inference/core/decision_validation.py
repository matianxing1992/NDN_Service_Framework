"""Core-owned final validation for every optimizer proposal."""

from __future__ import annotations

import math
from typing import Any, Iterable, Mapping
from dataclasses import fields, is_dataclass

from .ports import (
    AdmissionProposal, AssignmentProposal, CacheProposal, CandidateBudget,
    EngineSnapshot, ExecutionTargetProposal, MetricValue, ModelCandidate,
    OptimizationObjective, PolicyResult, RecoveryProposal,
    SchedulingProposal, TuningProposal,
)


FORBIDDEN_POLICY_FIELDS = frozenset({
    "prompt", "payload", "tensor", "token", "secret", "credential",
    "privatekey", "decryptedpolicy", "crosstenantdata",
})


def validate_objective(objective: OptimizationObjective) -> None:
    if not objective.hard_constraints:
        raise ValueError("objective requires hard constraints")
    for name, metric in objective.hard_constraints.items():
        if not isinstance(metric, MetricValue) or not metric.unit or not metric.aggregation:
            raise ValueError(f"objective metric semantics missing: {name}")
    for name, weight in objective.weights.items():
        if name not in objective.normalization or objective.normalization[name] <= 0:
            raise ValueError(f"objective normalization missing: {name}")
        if not math.isfinite(float(weight)):
            raise ValueError("objective weight is not finite")


def validate_snapshot(snapshot: EngineSnapshot, *, at_ms: int,
                      max_age_ms: int = 30_000) -> None:
    if snapshot.captured_at_ms > at_ms or at_ms - snapshot.captured_at_ms > max_age_ms:
        raise ValueError("stale engine snapshot")
    for name, estimate in snapshot.estimates.items():
        if estimate.measured_at_ms > at_ms:
            raise ValueError(f"future estimate: {name}")
        if at_ms - estimate.measured_at_ms > estimate.horizon_ms:
            raise ValueError(f"stale estimate: {name}")


def validate_candidate_budget(candidates: Iterable[Any], budget: CandidateBudget) -> tuple[Any, ...]:
    values = tuple(candidates)
    if len(values) > budget.max_candidates:
        raise ValueError("candidate budget exceeded")
    return values


def validate_model_selection(selected: ModelCandidate,
                             allowed: Iterable[ModelCandidate],
                             *, exact_required: bool) -> ModelCandidate:
    values = tuple(allowed)
    if selected not in values:
        raise ValueError("model variant is outside authorized candidate set")
    if exact_required and not selected.exact_semantics:
        raise ValueError("exact model semantics cannot be replaced")
    return selected


def validate_assignment(proposal: AssignmentProposal, roles: Iterable[str],
                        providers: Iterable[str]) -> AssignmentProposal:
    expected_roles = set(roles)
    if set(proposal.providers_by_role) != expected_roles:
        raise ValueError("assignment does not bind every role exactly once")
    allowed = set(providers)
    if any(provider not in allowed for provider in proposal.providers_by_role.values()):
        raise ValueError("assignment selected an ineligible Provider")
    return proposal


def validate_policy_result(result: PolicyResult, *, expected_kind: str,
                           snapshot_epoch: int) -> PolicyResult:
    if result.policy_kind != expected_kind:
        raise ValueError("policy result kind mismatch")
    if result.evidence.policy_kind != expected_kind:
        raise ValueError("policy evidence kind mismatch")
    if result.evidence.state_epoch != snapshot_epoch:
        raise ValueError("stale policy result epoch")
    reject_sensitive(result.value)
    return result


def reject_sensitive(value: Any) -> None:
    if isinstance(value, Mapping):
        for key, item in value.items():
            if str(key).replace("_", "").lower() in FORBIDDEN_POLICY_FIELDS:
                raise ValueError("policy result contains sensitive payload")
            reject_sensitive(item)
    elif isinstance(value, (tuple, list)):
        for item in value:
            reject_sensitive(item)
    elif is_dataclass(value):
        for field in fields(value):
            key = field.name.replace("_", "").lower()
            if key in FORBIDDEN_POLICY_FIELDS:
                raise ValueError("policy result contains sensitive payload")
            reject_sensitive(getattr(value, field.name))


def validate_scheduling(value: SchedulingProposal, allowed_items: Iterable[str]) -> None:
    if len(value.ordered_items) != len(set(value.ordered_items)):
        raise ValueError("scheduling proposal duplicates work")
    if not set(value.ordered_items).issubset(set(allowed_items)):
        raise ValueError("scheduling proposal crosses scope")
    if value.batch_size <= 0:
        raise ValueError("invalid scheduling batch")


def validate_admission(value: AdmissionProposal, prior_rejected: bool) -> None:
    if prior_rejected and value.accepted:
        raise ValueError("admission policy cannot reverse prior rejection")


def validate_tuning(value: TuningProposal) -> None:
    allowed = {
        "transfer_chunk_bytes", "prefetch_depth", "microbatch_size",
        "compression_level", "overlap", "speculative_window",
    }
    if set(value.parameters) - allowed:
        raise ValueError("undeclared execution tuning parameter")
    if any(not math.isfinite(float(item)) or float(item) < 0
           for item in value.parameters.values()):
        raise ValueError("invalid execution tuning value")


def validate_cache(value: CacheProposal, *, expected_epoch: int) -> None:
    if value.state_epoch != expected_epoch:
        raise ValueError("cache proposal state epoch mismatch")
    if not value.cache_key_digest.startswith("sha256:"):
        raise ValueError("cache key is not digest bound")


def validate_recovery(value: RecoveryProposal, *, current_attempt: int,
                      original_deadline_ms: int) -> None:
    if value.attempt_epoch <= current_attempt:
        raise ValueError("recovery attempt is stale")
    if value.original_deadline_ms != original_deadline_ms:
        raise ValueError("recovery changed the original deadline")


def validate_target(value: ExecutionTargetProposal,
                    compatible_adapters: Iterable[str]) -> None:
    if value.adapter_name not in set(compatible_adapters):
        raise ValueError("execution target selected incompatible adapter")


__all__ = [name for name in globals() if not name.startswith("_")]

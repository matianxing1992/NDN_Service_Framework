"""Core-owned final validation for every optimizer proposal."""

from __future__ import annotations

import math
import io
import os
import socket
from enum import Enum
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

FORBIDDEN_PLACEMENT_FIELDS = frozenset({
    "prompt", "payload", "rawinput", "inputbytes", "tensor", "token",
    "usertoken", "providertoken", "secret", "credential", "privatekey",
    "decryptedpolicy", "crosstenantdata", "callback", "networkhandle",
    "devicehandle", "filehandle", "workingdirectory", "writablepath",
    "mountpath", "repositoryhandle",
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


def validate_one_to_one_role_provider(
    provider_by_role: Mapping[str, str],
    *,
    expected_roles: Iterable[str] = (),
) -> None:
    """Validate the V3 invariant: one role per Provider per Attempt.

    This is intentionally separate from the legacy generic assignment
    validator.  Explicit V2 compatibility can preserve its historical
    placement shape, while every V3 proposal, sealed core, and Selection set
    calls this core-owned invariant.
    """

    assignments = {
        str(role): str(provider)
        for role, provider in provider_by_role.items()
    }
    expected = tuple(str(role) for role in expected_roles)
    if (not assignments
            or any(not role or not provider
                   for role, provider in assignments.items())):
        raise ValueError("V3 role/Provider assignment is incomplete")
    if expected and (len(set(expected)) != len(expected)
                     or set(assignments) != set(expected)):
        raise ValueError(
            "V3 role/Provider assignment does not cover each role exactly once")
    providers = tuple(assignments.values())
    if len(set(providers)) != len(providers):
        raise ValueError("V3 role/Provider assignment must be one-to-one")


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


def reject_placement_sensitive(value: Any, *, _field: str = "") -> None:
    """Enforce the data-only least-authority placement boundary.

    The planner receives canonical values and digests, never raw application
    bytes, tokens, mutable paths, callbacks, open file/network/device handles,
    or opaque runtime objects.
    """

    normalized = _field.replace("_", "").lower()
    if (normalized in FORBIDDEN_PLACEMENT_FIELDS
            or normalized.endswith("callback")
            or normalized.endswith("handle")
            or normalized.endswith("writablepath")):
        raise ValueError("placement strategy boundary contains authority or sensitive data")
    if isinstance(value, (bytes, bytearray, memoryview, os.PathLike,
                          io.IOBase, socket.socket)) or callable(value):
        raise ValueError("placement strategy boundary contains authority or sensitive data")
    if value is None or isinstance(value, (str, bool, int, float, Enum)):
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError("placement strategy boundary contains non-finite data")
        return
    if isinstance(value, Mapping):
        for key, item in value.items():
            reject_placement_sensitive(item, _field=str(key))
        return
    if isinstance(value, (tuple, list)):
        for item in value:
            reject_placement_sensitive(item)
        return
    if is_dataclass(value):
        for field in fields(value):
            reject_placement_sensitive(
                getattr(value, field.name), _field=field.name)
        return
    raise ValueError("placement strategy boundary contains an unsupported runtime object")


def validate_joint_placement(request: Any, decision: Any) -> None:
    """Validate role coverage and every selected Provider offer envelope."""

    providers = tuple(request.providers)
    by_name = {item.provider: item for item in providers}
    if len(by_name) != len(providers):
        raise ValueError("placement request contains duplicate Provider views")
    assigned_roles = tuple(item.role for item in decision.assignments)
    if (len(set(assigned_roles)) != len(assigned_roles)
            or set(assigned_roles) != set(request.required_roles)):
        raise ValueError("placement decision does not cover every role exactly once")
    aggregate_gpu: dict[str, int] = {}
    for assignment in decision.assignments:
        provider = by_name.get(assignment.provider)
        if provider is None:
            raise ValueError("placement decision selected a Provider outside the ACK set")
        if assignment.role not in provider.accepted_roles:
            raise ValueError("placement role is outside the Provider offer")
        if assignment.backend not in provider.backends:
            raise ValueError("placement backend is outside the Provider offer")
        aggregate_gpu[assignment.provider] = (
            aggregate_gpu.get(assignment.provider, 0)
            + assignment.required_gpu_memory_mb)
    for provider_name, required_mb in aggregate_gpu.items():
        if required_mb > by_name[provider_name].usable_gpu_memory_mb:
            raise ValueError("placement aggregate GPU memory exceeds Provider offer")
    allowed = set(by_name)
    for role, fallbacks in decision.fallback_order.items():
        if role not in set(request.required_roles):
            raise ValueError("placement fallback references an unknown role")
        if len(fallbacks) != len(set(fallbacks)) or not set(fallbacks).issubset(allowed):
            raise ValueError("placement fallback is outside the ACK set")


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

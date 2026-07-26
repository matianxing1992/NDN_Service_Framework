"""Versioned parity defaults composed through the same public policy seams."""

from __future__ import annotations

from typing import Any

from ..core.ports import (
    AdmissionProposal, AdmissionScope, AssignmentProposal, CacheAction,
    AdmissionResult, CacheProposal, CacheResult, DeploymentProposal,
    DeploymentResult, ExecutionTargetProposal, ExecutionTargetResult,
    ExecutionTuningResult, ModelVariantResult, PartitionResult, PolicyEvidence,
    PolicyRequest, PolicyResult, ProviderAssignmentResult, RecoveryProposal,
    RecoveryResult, SchedulingResult, SchedulingProposal,
    SchedulingScope, TuningProposal,
)
from ..sdk.contracts import POLICY_KINDS
from ..sdk.suite import OptimizationSuite


def _result(kind: str, request: PolicyRequest, value: Any,
            implementation: str) -> PolicyResult:
    evidence = PolicyEvidence(
        policy_kind=kind, implementation=implementation, version="1",
        state_epoch=request.snapshot.epoch,
        state_digest=request.snapshot.state.digest_value,
        decision_digest=(value.digest() if callable(getattr(value, "digest", None))
                         else request.digest()),
    )
    result_type = {
        "model_variant": ModelVariantResult,
        "partition": PartitionResult,
        "deployment": DeploymentResult,
        "provider_assignment": ProviderAssignmentResult,
        "scheduling": SchedulingResult,
        "admission": AdmissionResult,
        "execution_tuning": ExecutionTuningResult,
        "cache": CacheResult,
        "recovery": RecoveryResult,
        "execution_target": ExecutionTargetResult,
    }[kind]
    return result_type(kind, value, evidence)


class DefaultModelVariantPolicy:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        exact = [item for item in request.candidates
                 if bool(getattr(item, "exact_semantics", False))]
        values = exact or list(request.candidates)
        if not values:
            raise ValueError("no model variant candidates")
        return _result("model_variant", request, values[0], self.__class__.__name__)


class DefaultPartitionPlanner:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        if not request.candidates:
            raise ValueError("no partition candidates")
        return _result("partition", request, request.candidates[0], self.__class__.__name__)


class DefaultDeploymentPolicy:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        value = DeploymentProposal(
            action=str(request.metadata.get("action", "use-existing")),
            revision=str(request.metadata.get("revision", "current")),
            lifecycle_epoch=int(request.metadata.get("lifecycle_epoch", 1)),
            provider=str(request.metadata.get("provider", "local")),
            idempotency_key=str(request.metadata.get("idempotency_key", "default")))
        return _result("deployment", request, value, self.__class__.__name__)


class DefaultProviderAssignmentPolicy:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        roles = tuple(str(item) for item in request.metadata.get("roles", ()))
        providers = tuple(str(item) for item in request.metadata.get("providers", ()))
        if not roles or not providers:
            raise ValueError("assignment requires roles and Providers")
        mapping = {role: providers[min(index, len(providers) - 1)]
                   for index, role in enumerate(roles)}
        value = AssignmentProposal(
            assignment_id=str(request.metadata.get("assignment_id", "default")),
            plan_id=str(request.metadata.get("plan_id", "plan")),
            model_variant_id=str(request.metadata.get("variant_id", "exact")),
            providers_by_role=mapping)
        return _result("provider_assignment", request, value, self.__class__.__name__)


class DefaultSchedulingPolicy:
    def dispatch(self, request: PolicyRequest) -> PolicyResult:
        raw = request.scope.value if isinstance(request.scope, SchedulingScope) else request.scope
        scope = SchedulingScope(raw or "REQUEST_DAG")
        value = SchedulingProposal(scope, tuple(str(item) for item in request.candidates))
        return _result("scheduling", request, value, self.__class__.__name__)


class DefaultAdmissionPolicy:
    def admit(self, request: PolicyRequest) -> PolicyResult:
        raw = request.scope.value if isinstance(request.scope, AdmissionScope) else request.scope
        scope = AdmissionScope(raw or "ENGINE_REQUEST")
        value = AdmissionProposal(scope, True, "DEFAULT_ACCEPT")
        return _result("admission", request, value, self.__class__.__name__)


class DefaultExecutionTuningPolicy:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        value = TuningProposal(
            parameters={"microbatch_size": 1.0},
            declared_families=("microbatch",))
        return _result("execution_tuning", request, value, self.__class__.__name__)


class DefaultCachePolicy:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        value = CacheProposal(
            CacheAction.LOOKUP_OR_REUSE,
            str(request.metadata.get("cache_key_digest", "sha256:" + "0" * 64)),
            {}, request.snapshot.epoch)
        return _result("cache", request, value, self.__class__.__name__)


class DefaultRecoveryPolicy:
    def transition(self, request: PolicyRequest) -> PolicyResult:
        value = RecoveryProposal(
            str(request.metadata.get("action", "fail")),
            int(request.metadata.get("attempt_epoch", 1)) + 1,
            int(request.metadata.get("original_deadline_ms", 1)))
        return _result("recovery", request, value, self.__class__.__name__)


class DefaultExecutionTargetPolicy:
    def propose(self, request: PolicyRequest) -> PolicyResult:
        value = ExecutionTargetProposal(
            role=str(request.metadata.get("role", "role")),
            provider=str(request.metadata.get("provider", "provider")),
            adapter_name=str(request.metadata.get("adapter_name", "native")),
            device=str(request.metadata.get("device", "cpu")))
        return _result("execution_target", request, value, self.__class__.__name__)


class DefaultOptimizationSuite(OptimizationSuite):
    def __init__(self) -> None:
        policies = {
            "model_variant": DefaultModelVariantPolicy(),
            "partition": DefaultPartitionPlanner(),
            "deployment": DefaultDeploymentPolicy(),
            "provider_assignment": DefaultProviderAssignmentPolicy(),
            "scheduling": DefaultSchedulingPolicy(),
            "admission": DefaultAdmissionPolicy(),
            "execution_tuning": DefaultExecutionTuningPolicy(),
            "cache": DefaultCachePolicy(),
            "recovery": DefaultRecoveryPolicy(),
            "execution_target": DefaultExecutionTargetPolicy(),
        }
        super().__init__(
            policies, name="ndnsf-di-defaults", version="1",
            state_digest="sha256:" + "0" * 64)


__all__ = [name for name in globals() if name.startswith("Default")]

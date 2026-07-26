"""Public optimizer and execution-adapter SPIs."""

from __future__ import annotations

from typing import Any, Iterable, Protocol

from ..core.ports import (
    AdmissionRequest, AdmissionResult, CacheRequest, CacheResult,
    DeploymentRequest, DeploymentResult, ExecutionTargetRequest,
    ExecutionTargetResult, ExecutionTuningRequest, ExecutionTuningResult,
    ModelVariantRequest, ModelVariantResult, PartitionRequest, PartitionResult,
    ProviderAssignmentRequest, ProviderAssignmentResult, RecoveryRequest,
    RecoveryResult, SchedulingRequest, SchedulingResult,
)


POLICY_KINDS = (
    "model_variant", "partition", "deployment", "provider_assignment",
    "scheduling", "admission", "execution_tuning", "cache", "recovery",
    "execution_target",
)


class ModelVariantPolicy(Protocol):
    def propose(self, request: ModelVariantRequest) -> ModelVariantResult: ...


class PartitionPlanner(Protocol):
    def propose(self, request: PartitionRequest) -> PartitionResult: ...


class DeploymentPolicy(Protocol):
    def propose(self, request: DeploymentRequest) -> DeploymentResult: ...


class ProviderAssignmentPolicy(Protocol):
    def propose(self, request: ProviderAssignmentRequest) -> ProviderAssignmentResult: ...


class SchedulingPolicy(Protocol):
    def dispatch(self, request: SchedulingRequest) -> SchedulingResult: ...


class AdmissionPolicy(Protocol):
    def admit(self, request: AdmissionRequest) -> AdmissionResult: ...


class ExecutionTuningPolicy(Protocol):
    def propose(self, request: ExecutionTuningRequest) -> ExecutionTuningResult: ...


class CachePolicy(Protocol):
    def propose(self, request: CacheRequest) -> CacheResult: ...


class RecoveryPolicy(Protocol):
    def transition(self, request: RecoveryRequest) -> RecoveryResult: ...


class ExecutionTargetPolicy(Protocol):
    def propose(self, request: ExecutionTargetRequest) -> ExecutionTargetResult: ...


class RunnerAdapter(Protocol):
    name: str
    version: str

    def supports(self, target: Any) -> bool: ...
    def create_runner(self, target: Any, artifacts: Iterable[str]) -> Any: ...


class OptimizationObserver(Protocol):
    name: str
    version: str

    def observe(self, outcome: Any, idempotency_key: str) -> None: ...


__all__ = [
    "AdmissionPolicy", "AdmissionRequest", "AdmissionResult", "CachePolicy",
    "CacheRequest", "CacheResult", "DeploymentPolicy", "DeploymentRequest",
    "DeploymentResult", "ExecutionTargetPolicy", "ExecutionTargetRequest",
    "ExecutionTargetResult", "ExecutionTuningPolicy", "ExecutionTuningRequest",
    "ExecutionTuningResult", "ModelVariantPolicy", "ModelVariantRequest",
    "ModelVariantResult", "OptimizationObserver", "PartitionPlanner",
    "PartitionRequest", "PartitionResult", "POLICY_KINDS",
    "ProviderAssignmentPolicy", "ProviderAssignmentRequest",
    "ProviderAssignmentResult", "RecoveryPolicy", "RecoveryRequest",
    "RecoveryResult", "RunnerAdapter", "SchedulingPolicy",
    "SchedulingRequest", "SchedulingResult",
]

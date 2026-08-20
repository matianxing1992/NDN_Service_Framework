"""Stable, explicit Python SDK for external NDNSF-DI optimizers."""

from .adapters import RunnerAdapterRegistry
from .contracts import (
    AdmissionPolicy, AdmissionRequest, AdmissionResult, CachePolicy,
    CacheRequest, CacheResult, DeploymentPolicy, DeploymentRequest,
    DeploymentResult, ExecutionTargetPolicy, ExecutionTargetRequest,
    ExecutionTargetResult, ExecutionTuningPolicy, ExecutionTuningRequest,
    ExecutionTuningResult, ModelVariantPolicy, ModelVariantRequest,
    ModelVariantResult, OptimizationObserver, PartitionPlanner,
    PartitionRequest, PartitionResult, POLICY_KINDS,
    ProviderAssignmentPolicy, ProviderAssignmentRequest,
    ProviderAssignmentResult, RecoveryPolicy, RecoveryRequest,
    RecoveryResult, RunnerAdapter, SchedulingPolicy, SchedulingRequest,
    SchedulingResult,
)
from .executor import BoundedPolicyExecutor, PolicyExecutionTimeout
from .loader import (
    PlacementStrategyAllowlistEntry, discover_optimizers,
    discover_placement_strategies, distribution_record_digest,
    select_placement_strategy,
)
from .observer import ObserverRegistry
from .placement import (
    ArtifactPreparationMode, DIProviderOfferV2, ModelPlacementStrategy,
    PlacementDecision,
    PlacementRequest, ProviderAssignment, ProviderPlanningView,
    build_provider_planning_view, evaluate_placement_strategy,
)
from .registry import PolicyRegistry, RegisteredPolicy
from .suite import OptimizationSuite, OptimizationSuiteBuilder
from .worker import (
    FORBIDDEN_INPUT_KEYS, decode_worker_envelope, encode_worker_envelope,
    least_input_projection,
)

__all__ = [
    "AdmissionPolicy", "AdmissionRequest", "AdmissionResult",
    "ArtifactPreparationMode",
    "BoundedPolicyExecutor", "CachePolicy", "CacheRequest", "CacheResult",
    "DeploymentPolicy", "DeploymentRequest", "DeploymentResult",
    "ExecutionTargetPolicy", "ExecutionTargetRequest", "ExecutionTargetResult",
    "ExecutionTuningPolicy", "ExecutionTuningRequest", "ExecutionTuningResult",
    "FORBIDDEN_INPUT_KEYS", "ModelVariantPolicy", "ModelVariantRequest",
    "ModelVariantResult", "ModelPlacementStrategy", "ObserverRegistry",
    "OptimizationObserver",
    "OptimizationSuite", "OptimizationSuiteBuilder", "POLICY_KINDS",
    "PartitionPlanner", "PartitionRequest", "PartitionResult",
    "PlacementDecision", "PlacementRequest", "PolicyExecutionTimeout",
    "PolicyRegistry", "ProviderAssignment", "ProviderAssignmentPolicy",
    "ProviderAssignmentRequest", "ProviderAssignmentResult", "RecoveryPolicy",
    "ProviderPlanningView", "RecoveryRequest", "RecoveryResult",
    "RegisteredPolicy", "RunnerAdapter",
    "RunnerAdapterRegistry", "SchedulingPolicy", "SchedulingRequest",
    "SchedulingResult", "decode_worker_envelope", "discover_optimizers",
    "distribution_record_digest", "encode_worker_envelope",
    "DIProviderOfferV2", "build_provider_planning_view",
    "evaluate_placement_strategy", "least_input_projection",
    "PlacementStrategyAllowlistEntry", "discover_placement_strategies",
    "select_placement_strategy",
]

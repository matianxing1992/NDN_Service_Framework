"""Workload-neutral values exchanged across NDNSF-DI optimization ports."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, is_dataclass
from enum import Enum
import hashlib
import json
from typing import Any, Mapping, Tuple


def _plain(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if is_dataclass(value):
        return {key: _plain(item) for key, item in asdict(value).items()}
    if isinstance(value, Mapping):
        return {str(key): _plain(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [_plain(item) for item in value]
    return value


class ContractValue:
    def to_dict(self) -> dict[str, Any]:
        return _plain(self)

    def to_bytes(self) -> bytes:
        return json.dumps(
            self.to_dict(), sort_keys=True, separators=(",", ":")
        ).encode("utf-8")

    def digest(self) -> str:
        return "sha256:" + hashlib.sha256(self.to_bytes()).hexdigest()


@dataclass(frozen=True)
class MetricValue(ContractValue):
    value: float
    unit: str
    aggregation: str
    direction: str = "minimize"
    missing_data: str = "reject"

    def __post_init__(self) -> None:
        if (not self.unit or not self.aggregation
                or self.direction not in {"minimize", "maximize"}
                or self.missing_data not in {"reject", "ignore", "penalize"}):
            raise ValueError("metric requires unit and aggregation")


@dataclass(frozen=True)
class EstimateEnvelope(ContractValue):
    value: float
    unit: str
    horizon_ms: int
    measured_at_ms: int
    source: str
    confidence: float = 1.0

    def __post_init__(self) -> None:
        if (not self.unit or not self.source or self.horizon_ms <= 0
                or self.measured_at_ms <= 0 or not 0.0 <= self.confidence <= 1.0):
            raise ValueError("invalid estimate envelope")


@dataclass(frozen=True)
class OptimizationObjective(ContractValue):
    hard_constraints: Mapping[str, MetricValue]
    weights: Mapping[str, float]
    normalization: Mapping[str, float]


@dataclass(frozen=True)
class PolicyState(ContractValue):
    epoch: int
    digest_value: str

    def __post_init__(self) -> None:
        if self.epoch <= 0 or not self.digest_value.startswith("sha256:"):
            raise ValueError("invalid policy state")


@dataclass(frozen=True)
class EngineSnapshot(ContractValue):
    snapshot_id: str
    epoch: int
    captured_at_ms: int
    metrics: Mapping[str, MetricValue]
    estimates: Mapping[str, EstimateEnvelope]
    state: PolicyState

    def __post_init__(self) -> None:
        if not self.snapshot_id or self.epoch <= 0 or self.captured_at_ms <= 0:
            raise ValueError("invalid engine snapshot")
        if self.state.epoch != self.epoch:
            raise ValueError("snapshot and policy-state epochs differ")


@dataclass(frozen=True)
class EngineGraph(ContractValue):
    nodes: Tuple[str, ...]
    edges: Tuple[Tuple[str, str], ...]

    def __post_init__(self) -> None:
        known = set(self.nodes)
        if len(known) != len(self.nodes) or any(
                source not in known or target not in known
                for source, target in self.edges):
            raise ValueError("invalid engine graph")


@dataclass(frozen=True)
class CandidateBudget(ContractValue):
    max_candidates: int
    max_policy_ms: int = 100
    max_reentries: int = 1

    def __post_init__(self) -> None:
        if self.max_candidates <= 0 or self.max_policy_ms <= 0 or self.max_reentries < 0:
            raise ValueError("invalid candidate budget")


@dataclass(frozen=True)
class ModelCandidate(ContractValue):
    model_id: str
    variant_id: str
    exact_semantics: bool
    precision: str
    artifact_digest: str
    capabilities: Tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if (not self.model_id or not self.variant_id or not self.precision
                or not self.artifact_digest.startswith("sha256:")):
            raise ValueError("invalid model candidate")


@dataclass(frozen=True)
class PlanCandidate(ContractValue):
    plan_id: str
    model_variant_id: str
    roles: Tuple[str, ...]
    plan_digest: str
    estimated_cost: float = 0.0


@dataclass(frozen=True)
class PlanCandidateSet(ContractValue):
    candidates: Tuple[PlanCandidate, ...]
    budget: CandidateBudget


@dataclass(frozen=True)
class ProviderCandidate(ContractValue):
    provider: str
    boot_epoch: str
    roles: Tuple[str, ...]
    capabilities: Tuple[str, ...] = ()
    score: float = 0.0


@dataclass(frozen=True)
class AssignmentProposal(ContractValue):
    assignment_id: str
    plan_id: str
    model_variant_id: str
    providers_by_role: Mapping[str, str]
    target_by_role: Mapping[str, str] = field(default_factory=dict)
    affinity: Mapping[str, float] = field(default_factory=dict)


class SchedulingScope(str, Enum):
    REQUEST_DAG = "REQUEST_DAG"
    PROVIDER_LOCAL = "PROVIDER_LOCAL"


class AdmissionScope(str, Enum):
    ENGINE_REQUEST = "ENGINE_REQUEST"
    PROVIDER_LOCAL = "PROVIDER_LOCAL"


@dataclass(frozen=True)
class SchedulingProposal(ContractValue):
    scope: SchedulingScope
    ordered_items: Tuple[str, ...]
    batch_size: int = 1
    preempt: bool = False
    hedge_roles: Tuple[str, ...] = ()


@dataclass(frozen=True)
class AdmissionProposal(ContractValue):
    scope: AdmissionScope
    accepted: bool
    reason_code: str = ""


@dataclass(frozen=True)
class TuningProposal(ContractValue):
    parameters: Mapping[str, float]
    declared_families: Tuple[str, ...]


class CacheAction(str, Enum):
    LOOKUP_OR_REUSE = "LOOKUP_OR_REUSE"
    PLACE = "PLACE"
    PREFETCH = "PREFETCH"
    ADMIT = "ADMIT"
    RETAIN = "RETAIN"
    EVICT = "EVICT"
    MIGRATE_OR_REPLICATE = "MIGRATE_OR_REPLICATE"


@dataclass(frozen=True)
class CacheProposal(ContractValue):
    action: CacheAction
    cache_key_digest: str
    provider_affinity: Mapping[str, float]
    state_epoch: int


@dataclass(frozen=True)
class DeploymentProposal(ContractValue):
    action: str
    revision: str
    lifecycle_epoch: int
    provider: str
    idempotency_key: str


@dataclass(frozen=True)
class RecoveryProposal(ContractValue):
    action: str
    attempt_epoch: int
    original_deadline_ms: int
    replacement_provider: str = ""
    checkpoint_digest: str = ""


@dataclass(frozen=True)
class ExecutionTargetProposal(ContractValue):
    role: str
    provider: str
    adapter_name: str
    device: str


@dataclass(frozen=True)
class ProgressRecord(ContractValue):
    request_id: str
    attempt_epoch: int
    role: str
    phase: str
    output_epoch: int


@dataclass(frozen=True)
class CheckpointRecord(ContractValue):
    request_id: str
    attempt_epoch: int
    checkpoint_digest: str
    output_epoch: int


@dataclass(frozen=True)
class ValidatedExecutionIntent(ContractValue):
    intent_id: str
    request_id: str
    attempt_epoch: int
    snapshot_epoch: int
    model_variant_id: str
    plan_id: str
    assignment: AssignmentProposal
    lease_digest: str
    cache_epoch: int
    checkpoint_epoch: int
    state: str = "PREPARED"


@dataclass(frozen=True)
class ExecutionOutcome(ContractValue):
    request_id: str
    attempt_epoch: int
    status: str
    result_digest: str
    policy_evidence: Tuple[str, ...] = ()


@dataclass(frozen=True)
class PolicyEvidence(ContractValue):
    policy_kind: str
    implementation: str
    version: str
    state_epoch: int
    state_digest: str
    decision_digest: str
    used_default: bool = False


@dataclass(frozen=True)
class PolicyRequest(ContractValue):
    objective: OptimizationObjective
    snapshot: EngineSnapshot
    budget: CandidateBudget
    candidates: Tuple[Any, ...]
    scope: str = ""
    metadata: Mapping[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class PolicyResult(ContractValue):
    policy_kind: str
    value: Any
    evidence: PolicyEvidence


# Each optimization seam has a distinct runtime type.  They intentionally share
# canonical serialization through PolicyRequest/PolicyResult, but they are not
# aliases and therefore cannot be accidentally passed to the wrong policy.
@dataclass(frozen=True)
class ModelVariantRequest(PolicyRequest):
    candidates: Tuple[ModelCandidate, ...]
    allowed_model_ids: Tuple[str, ...] = ()


@dataclass(frozen=True)
class ModelVariantResult(PolicyResult):
    value: ModelCandidate


@dataclass(frozen=True)
class PartitionRequest(PolicyRequest):
    candidates: Tuple[PlanCandidate, ...]
    model_variant_id: str = ""


@dataclass(frozen=True)
class PartitionResult(PolicyResult):
    value: PlanCandidate


@dataclass(frozen=True)
class DeploymentRequest(PolicyRequest):
    candidates: Tuple[DeploymentProposal, ...]
    definition_digest: str = ""


@dataclass(frozen=True)
class DeploymentResult(PolicyResult):
    value: DeploymentProposal


@dataclass(frozen=True)
class ProviderAssignmentRequest(PolicyRequest):
    candidates: Tuple[ProviderCandidate, ...]
    required_roles: Tuple[str, ...] = ()
    deployment_offers: Tuple[Mapping[str, Any], ...] = ()


@dataclass(frozen=True)
class ProviderAssignmentResult(PolicyResult):
    value: AssignmentProposal


@dataclass(frozen=True)
class SchedulingRequest(PolicyRequest):
    candidates: Tuple[str, ...]
    dependency_edges: Tuple[Tuple[str, str], ...] = ()


@dataclass(frozen=True)
class SchedulingResult(PolicyResult):
    value: SchedulingProposal


@dataclass(frozen=True)
class AdmissionRequest(PolicyRequest):
    candidates: Tuple[str, ...]
    requested_resources: Mapping[str, MetricValue] = field(default_factory=dict)


@dataclass(frozen=True)
class AdmissionResult(PolicyResult):
    value: AdmissionProposal


@dataclass(frozen=True)
class ExecutionTuningRequest(PolicyRequest):
    candidates: Tuple[str, ...]
    tunable_families: Tuple[str, ...] = ()


@dataclass(frozen=True)
class ExecutionTuningResult(PolicyResult):
    value: TuningProposal


@dataclass(frozen=True)
class CacheRequest(PolicyRequest):
    candidates: Tuple[str, ...]
    cache_key_digest: str = ""


@dataclass(frozen=True)
class CacheResult(PolicyResult):
    value: CacheProposal


@dataclass(frozen=True)
class RecoveryRequest(PolicyRequest):
    candidates: Tuple[str, ...]
    failure_evidence_digest: str = ""


@dataclass(frozen=True)
class RecoveryResult(PolicyResult):
    value: RecoveryProposal


@dataclass(frozen=True)
class ExecutionTargetRequest(PolicyRequest):
    candidates: Tuple[ExecutionTargetProposal, ...]
    assigned_provider: str = ""


@dataclass(frozen=True)
class ExecutionTargetResult(PolicyResult):
    value: ExecutionTargetProposal


__all__ = [name for name in globals() if not name.startswith("_")]

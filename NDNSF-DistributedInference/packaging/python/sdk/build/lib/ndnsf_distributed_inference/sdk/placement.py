"""Data-only joint split and Provider placement contract.

This module is intentionally side-effect free.  It contains immutable values
that trusted NDNSF-DI code may pass to an operator-selected strategy after the
generic Collaboration ACK set has closed.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field, fields, is_dataclass
from enum import Enum
import hashlib
import json
import math
from types import MappingProxyType
from typing import Any, Callable, Mapping, Tuple

from ..core.ports import CandidateBudget
from ..core.decision_validation import (
    reject_placement_sensitive, validate_joint_placement,
)
from .executor import BoundedPolicyExecutor, PolicyExecutionTimeout


def _plain(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if isinstance(value, Mapping):
        return {str(key): _plain(item) for key, item in value.items()}
    if is_dataclass(value):
        return {
            item.name: _plain(getattr(value, item.name))
            for item in fields(value)
        }
    if isinstance(value, (tuple, list)):
        return [_plain(item) for item in value]
    return value


def _freeze(value: Any) -> Any:
    if isinstance(value, Mapping):
        return MappingProxyType({
            str(key): _freeze(item) for key, item in value.items()
        })
    if isinstance(value, (tuple, list)):
        return tuple(_freeze(item) for item in value)
    if isinstance(value, (set, frozenset)):
        return tuple(sorted((_freeze(item) for item in value), key=repr))
    return value


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        _plain(value), sort_keys=True, separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")


def canonical_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_bytes(value)).hexdigest()


def _require_digest(value: str, field_name: str) -> None:
    if (not isinstance(value, str) or not value.startswith("sha256:")
            or len(value) != 71):
        raise ValueError(f"{field_name} must be a canonical sha256 digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(
            f"{field_name} must be a canonical sha256 digest") from exc


@dataclass(frozen=True)
class DIProviderOfferV2:
    """Authenticated DI interpretation of one positive generic ACK payload."""

    profile: str
    profile_version: int
    request_id: str
    attempt: int
    service: str
    provider: str
    model_intent_digest: str
    boot_epoch: str
    resource_sequence: int
    captured_at_ms: int
    expires_at_ms: int
    accepted_deadline_ms: int
    accepted_roles: Tuple[str, ...]
    backends: Tuple[str, ...]
    offered_gpu_memory_mb: int
    queue_depth: int | None
    estimated_wait_ms: float | None
    rtt_ms: float | None
    bandwidth_mbps: float | None
    capability_resource_digest: str
    acceptance_predicate_digest: str
    evidence_digest: str
    signer_key_id: str
    signature: str
    cached_shards: Tuple[Any, ...] = ()
    reusable_state: Tuple[Any, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "accepted_roles", tuple(self.accepted_roles))
        object.__setattr__(self, "backends", tuple(self.backends))
        object.__setattr__(self, "cached_shards", _freeze(self.cached_shards))
        object.__setattr__(self, "reusable_state", _freeze(self.reusable_state))
        if len(self.boot_epoch) < 8:
            raise ValueError("invalid DI Provider offer boot epoch")
        if (self.profile != "ndnsf-di-provider-offer-v2"
                or self.profile_version != 2
                or not self.request_id or self.attempt <= 0
                or not self.service or not self.provider
                or self.resource_sequence <= 0
                or self.captured_at_ms <= 0
                or self.expires_at_ms <= self.captured_at_ms
                or self.accepted_deadline_ms <= self.captured_at_ms
                or not self.accepted_roles or len(set(self.accepted_roles)) != len(
                    self.accepted_roles)
                or not self.backends or self.offered_gpu_memory_mb <= 0
                or self.queue_depth is not None and self.queue_depth < 0
                or not self.signer_key_id or not self.signature):
            raise ValueError("invalid DI Provider offer")
        for name, value in (
                ("model_intent_digest", self.model_intent_digest),
                ("capability_resource_digest", self.capability_resource_digest),
                ("acceptance_predicate_digest", self.acceptance_predicate_digest),
                ("evidence_digest", self.evidence_digest)):
            _require_digest(value, name)
        for value in (
                self.estimated_wait_ms, self.rtt_ms, self.bandwidth_mbps):
            if value is not None and (not math.isfinite(value) or value < 0):
                raise ValueError("invalid DI Provider offer metric")

    def canonical_dict(self) -> dict[str, Any]:
        value = _plain(self)
        value.pop("signature", None)
        return value

    def digest(self) -> str:
        return canonical_digest(self.canonical_dict())

    def to_bytes(self) -> bytes:
        payload = _plain(self)
        payload.update({
            "schema": "ndnsf-di-provider-offer-v2",
            "schema_version": 2,
            "canonical_encoding_version": "canonical-json-v1",
            "capability_version": "SELECTION_DATAFLOW_V2",
            "acceptance_predicate_version": "DI_ACCEPTANCE_V2",
        })
        return json.dumps(
            payload, sort_keys=True, separators=(",", ":"),
            allow_nan=False,
        ).encode()

    @classmethod
    def from_bytes(cls, wire: bytes) -> "DIProviderOfferV2":
        if len(bytes(wire)) > 1024 * 1024:
            raise ValueError("DIProviderOfferV2 exceeds the bounded wire size")
        try:
            payload = json.loads(bytes(wire).decode())
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("malformed DIProviderOfferV2") from exc
        if (not isinstance(payload, dict)
                or bytes(wire) != json.dumps(
                    payload, sort_keys=True, separators=(",", ":"),
                    allow_nan=False).encode()):
            raise ValueError("DIProviderOfferV2 is not canonically encoded")
        metadata = {
            "schema": "ndnsf-di-provider-offer-v2",
            "schema_version": 2,
            "canonical_encoding_version": "canonical-json-v1",
            "capability_version": "SELECTION_DATAFLOW_V2",
            "acceptance_predicate_version": "DI_ACCEPTANCE_V2",
        }
        expected = {item.name for item in fields(cls)} | set(metadata)
        if set(payload) != expected or any(
                payload.get(key) != value for key, value in metadata.items()):
            raise ValueError(
                "DIProviderOfferV2 version mismatch; downgrade is forbidden")
        for key in metadata:
            payload.pop(key)
        for key in ("accepted_roles", "backends", "cached_shards",
                    "reusable_state"):
            payload[key] = tuple(payload[key])
        return cls(**payload)


@dataclass(frozen=True)
class ProviderPlanningView:
    """Sanitized immutable projection passed to a placement strategy."""

    provider: str
    service: str
    boot_epoch: str
    resource_sequence: int
    offer_digest: str
    evidence_digest: str
    expires_at_ms: int
    accepted_deadline_ms: int
    accepted_roles: Tuple[str, ...]
    backends: Tuple[str, ...]
    usable_gpu_memory_mb: int
    queue_depth: int | None
    estimated_wait_ms: float | None
    rtt_ms: float | None
    bandwidth_mbps: float | None
    cached_shards: Tuple[Any, ...] = ()
    reusable_state: Tuple[Any, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "accepted_roles", tuple(self.accepted_roles))
        object.__setattr__(self, "backends", tuple(self.backends))
        object.__setattr__(self, "cached_shards", _freeze(self.cached_shards))
        object.__setattr__(self, "reusable_state", _freeze(self.reusable_state))
        if (not self.provider or not self.service or len(self.boot_epoch) < 8
                or self.resource_sequence <= 0 or self.expires_at_ms <= 0
                or self.accepted_deadline_ms <= 0
                or not self.accepted_roles or not self.backends
                or self.usable_gpu_memory_mb <= 0):
            raise ValueError("invalid Provider planning view")
        _require_digest(self.offer_digest, "offer_digest")
        _require_digest(self.evidence_digest, "evidence_digest")

    def to_dict(self) -> dict[str, Any]:
        return _plain(self)

    def digest(self) -> str:
        return canonical_digest(self)


def build_provider_planning_view(
    offer: DIProviderOfferV2,
    *,
    ack_status: bool,
    at_ms: int,
    request_id: str,
    attempt: int,
    model_intent_digest: str,
    deadline_ms: int,
    verify_signature: Callable[[DIProviderOfferV2], bool],
) -> ProviderPlanningView:
    """Validate one ACK-bound offer and remove signature/private proof fields."""

    if not ack_status:
        raise ValueError("negative ACK cannot become a Provider planning view")
    if not callable(verify_signature) or not verify_signature(offer):
        raise ValueError("DI Provider offer signature is invalid")
    if (offer.request_id != request_id or offer.attempt != attempt
            or offer.model_intent_digest != model_intent_digest):
        raise ValueError("DI Provider offer request binding mismatch")
    if offer.captured_at_ms > at_ms or offer.expires_at_ms <= at_ms:
        raise ValueError("DI Provider offer is stale or expired")
    if offer.accepted_deadline_ms < deadline_ms:
        raise ValueError("DI Provider offer does not cover the request deadline")
    return ProviderPlanningView(
        provider=offer.provider,
        service=offer.service,
        boot_epoch=offer.boot_epoch,
        resource_sequence=offer.resource_sequence,
        offer_digest=offer.digest(),
        evidence_digest=offer.evidence_digest,
        expires_at_ms=offer.expires_at_ms,
        accepted_deadline_ms=offer.accepted_deadline_ms,
        accepted_roles=offer.accepted_roles,
        backends=offer.backends,
        usable_gpu_memory_mb=offer.offered_gpu_memory_mb,
        queue_depth=offer.queue_depth,
        estimated_wait_ms=offer.estimated_wait_ms,
        rtt_ms=offer.rtt_ms,
        bandwidth_mbps=offer.bandwidth_mbps,
        cached_shards=offer.cached_shards,
        reusable_state=offer.reusable_state,
    )


@dataclass(frozen=True)
class ProviderAssignment:
    role: str
    provider: str
    required_gpu_memory_mb: int
    backend: str

    def __post_init__(self) -> None:
        if (not self.role or not self.provider or not self.backend
                or self.required_gpu_memory_mb < 0):
            raise ValueError("invalid Provider assignment")


@dataclass(frozen=True)
class PlacementRequest:
    request_id: str
    attempt: int
    deadline_ms: int
    model_digest: str
    graph_digest: str
    candidate_ids: Tuple[str, ...]
    providers: Tuple[Any, ...]
    required_roles: Tuple[str, ...]
    budget: CandidateBudget
    objective: Any = None
    constraints: Mapping[str, Any] = field(default_factory=dict)
    task_digest: str = ""
    state_contracts: Tuple[Any, ...] = ()
    model: Any = None
    graph: Any = None
    candidates: Tuple[Any, ...] = ()
    network_snapshot: Any = None
    catalog_snapshot: Any = None
    runtime_estimates: Any = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "candidate_ids", tuple(self.candidate_ids))
        object.__setattr__(self, "providers", _freeze(self.providers))
        object.__setattr__(self, "required_roles", tuple(self.required_roles))
        object.__setattr__(self, "objective", _freeze(self.objective))
        object.__setattr__(self, "constraints", _freeze(self.constraints))
        object.__setattr__(self, "state_contracts", _freeze(self.state_contracts))
        object.__setattr__(self, "candidates", _freeze(self.candidates))
        object.__setattr__(
            self, "network_snapshot", _freeze(self.network_snapshot))
        object.__setattr__(
            self, "catalog_snapshot", _freeze(self.catalog_snapshot))
        object.__setattr__(
            self, "runtime_estimates", _freeze(self.runtime_estimates))
        if (not self.request_id or self.attempt <= 0 or self.deadline_ms <= 0
                or not self.candidate_ids or not self.required_roles):
            raise ValueError("invalid placement request")
        if len(set(self.candidate_ids)) != len(self.candidate_ids):
            raise ValueError("duplicate split candidate")
        if len(set(self.required_roles)) != len(self.required_roles):
            raise ValueError("duplicate required role")
        if len(self.candidate_ids) > self.budget.max_candidates:
            raise ValueError("placement candidate budget exceeded")
        _require_digest(self.model_digest, "model_digest")
        _require_digest(self.graph_digest, "graph_digest")
        if self.task_digest:
            _require_digest(self.task_digest, "task_digest")
        if self.model is not None and getattr(
                self.model, "model_digest", "") != self.model_digest:
            raise ValueError("placement model descriptor digest mismatch")
        if self.graph is not None and getattr(
                self.graph, "graph_digest", "") != self.graph_digest:
            raise ValueError("placement graph snapshot digest mismatch")
        if self.candidates:
            candidate_digests = tuple(
                getattr(item, "candidate_digest", "") for item in self.candidates
            )
            if candidate_digests != self.candidate_ids:
                raise ValueError("placement candidate snapshot binding mismatch")

    def digest(self) -> str:
        return canonical_digest(self)


class ArtifactPreparationMode(str, Enum):
    """Trusted coordinator action requested by a data-only decision."""

    GENERATED = "GENERATED"
    PRE_SPLIT = "PRE_SPLIT"


@dataclass(frozen=True)
class PlacementDecision:
    split_id: str
    split_digest: str
    assignments: Tuple[ProviderAssignment, ...]
    fallback_order: Mapping[str, Tuple[str, ...]]
    input_digest: str
    evidence_digest: str
    artifact_preparation: ArtifactPreparationMode | str = (
        ArtifactPreparationMode.GENERATED)
    evidence: Mapping[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(
            self, "artifact_preparation",
            ArtifactPreparationMode(self.artifact_preparation))
        object.__setattr__(self, "assignments", tuple(self.assignments))
        object.__setattr__(
            self, "fallback_order", _freeze(self.fallback_order))
        object.__setattr__(self, "evidence", _freeze(self.evidence))
        if not self.split_id or not self.assignments:
            raise ValueError("placement decision is incomplete")
        for name, value in (
                ("split_digest", self.split_digest),
                ("input_digest", self.input_digest),
                ("evidence_digest", self.evidence_digest)):
            _require_digest(value, name)

    def digest(self) -> str:
        return canonical_digest(self)


class ModelPlacementStrategy(ABC):
    """Operator-trusted strategy returning an untrusted data-only proposal."""

    name: str
    version: str
    state_digest: str
    deterministic: bool = True

    @abstractmethod
    def plan(self, request: PlacementRequest) -> PlacementDecision:
        raise NotImplementedError


def _validate_strategy_identity(strategy: ModelPlacementStrategy) -> None:
    if not strategy.name or not strategy.version:
        raise ValueError("placement strategy identity is incomplete")
    _require_digest(strategy.state_digest, "strategy state_digest")


def _validate_basic_decision(
    request: PlacementRequest, decision: PlacementDecision,
) -> None:
    if decision.input_digest != request.digest():
        raise ValueError("placement decision input digest mismatch")
    if decision.split_id not in request.candidate_ids:
        raise ValueError("placement decision selected an unknown split")
    roles = tuple(item.role for item in decision.assignments)
    if len(set(roles)) != len(roles) or set(roles) != set(request.required_roles):
        raise ValueError("placement decision does not cover every role exactly once")
    if request.providers:
        validate_joint_placement(request, decision)


def evaluate_placement_strategy(
    strategy: ModelPlacementStrategy,
    request: PlacementRequest,
    *,
    replay_deterministic: bool = False,
    executor: BoundedPolicyExecutor | None = None,
) -> PlacementDecision:
    """Run, budget-check, replay-check, and validate one pure proposal."""

    _validate_strategy_identity(strategy)
    reject_placement_sensitive(request)
    runner = executor or BoundedPolicyExecutor()
    try:
        decision = runner.execute(
            strategy.plan, request, request.budget.max_policy_ms)
    except PolicyExecutionTimeout as exc:
        raise TimeoutError(
            "placement strategy exceeded its time budget") from exc
    if not isinstance(decision, PlacementDecision):
        raise TypeError("placement strategy returned a non-decision value")
    reject_placement_sensitive(decision)
    _validate_basic_decision(request, decision)
    if replay_deterministic and strategy.deterministic:
        try:
            replay = runner.execute(
                strategy.plan, request, request.budget.max_policy_ms)
        except PolicyExecutionTimeout as exc:
            raise TimeoutError(
                "placement strategy replay exceeded its time budget") from exc
        if not isinstance(replay, PlacementDecision) or replay.digest() != decision.digest():
            raise ValueError("deterministic placement strategy changed its decision")
    return decision


__all__ = [
    "DIProviderOfferV2", "ModelPlacementStrategy", "PlacementDecision",
    "PlacementRequest", "ProviderAssignment", "ProviderPlanningView",
    "build_provider_planning_view", "canonical_bytes", "canonical_digest",
    "evaluate_placement_strategy",
]

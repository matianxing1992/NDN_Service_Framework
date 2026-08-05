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

from ..core.contracts import DATA_DRIVEN_V2, LEGACY_READY_SET_V1
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


def is_cpu_backend(backend: str) -> bool:
    """Return whether an explicit runtime backend is CPU-bound.

    Adapter backends use the ``-cpu`` suffix (for example
    ``onnxruntime-cpu`` and ``transformers-cpu``). Keeping this classification
    beside the shared placement contracts prevents the strategy and final plan
    sealer from accepting different backend/device pairs.
    """

    normalized = str(backend).strip().lower()
    return normalized == "cpu" or normalized.endswith("-cpu")


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
    execution_policies: Tuple[str, ...] = (DATA_DRIVEN_V2,)
    devices: Tuple[str, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "accepted_roles", tuple(self.accepted_roles))
        object.__setattr__(self, "backends", tuple(self.backends))
        object.__setattr__(self, "devices", tuple(self.devices))
        object.__setattr__(self, "cached_shards", _freeze(self.cached_shards))
        object.__setattr__(self, "reusable_state", _freeze(self.reusable_state))
        object.__setattr__(
            self, "execution_policies", tuple(self.execution_policies))
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
                or len(set(self.devices)) != len(self.devices)
                or any(not value for value in self.devices)
                or DATA_DRIVEN_V2 in self.execution_policies
                and not self.devices
                or any(
                    device != "cpu" and not (
                        device.startswith("cuda:") and device[5:].isdigit())
                    for device in self.devices)
                or not self.execution_policies
                or len(set(self.execution_policies))
                != len(self.execution_policies)
                or set(self.execution_policies) - {
                    DATA_DRIVEN_V2, LEGACY_READY_SET_V1}
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
        for key in ("accepted_roles", "backends", "devices", "cached_shards",
                    "reusable_state", "execution_policies"):
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
    execution_policies: Tuple[str, ...] = (DATA_DRIVEN_V2,)
    devices: Tuple[str, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "accepted_roles", tuple(self.accepted_roles))
        object.__setattr__(self, "backends", tuple(self.backends))
        object.__setattr__(self, "devices", tuple(self.devices))
        object.__setattr__(self, "cached_shards", _freeze(self.cached_shards))
        object.__setattr__(self, "reusable_state", _freeze(self.reusable_state))
        object.__setattr__(
            self, "execution_policies", tuple(self.execution_policies))
        if (not self.provider or not self.service or len(self.boot_epoch) < 8
                or self.resource_sequence <= 0 or self.expires_at_ms <= 0
                or self.accepted_deadline_ms <= 0
                or not self.accepted_roles or not self.backends
                or self.usable_gpu_memory_mb <= 0
                or not self.execution_policies):
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
    if DATA_DRIVEN_V2 not in offer.execution_policies:
        raise ValueError(
            "DI Provider offer lacks required DATA_DRIVEN_V2 execution policy")
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
        devices=offer.devices,
        usable_gpu_memory_mb=offer.offered_gpu_memory_mb,
        queue_depth=offer.queue_depth,
        estimated_wait_ms=offer.estimated_wait_ms,
        rtt_ms=offer.rtt_ms,
        bandwidth_mbps=offer.bandwidth_mbps,
        cached_shards=offer.cached_shards,
        reusable_state=offer.reusable_state,
        execution_policies=offer.execution_policies,
    )


@dataclass(frozen=True)
class ProviderAssignment:
    role: str
    provider: str
    required_gpu_memory_mb: int
    backend: str
    device: str = ""

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
    # All required artifacts are already resident on the selected Providers
    # (GPU/RAM/disk) according to the post-ACK cache evidence.  The trusted
    # coordinator may resolve the existing Repo names, but must not
    # materialize or publish the split again.
    REUSE_CACHED = "REUSE_CACHED"


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


# ---------------------------------------------------------------------------
# Spec 170 V3 contracts.  These are deliberately adjacent to, but separate
# from, the legacy V2 values above.  A V3 wire object never decodes as V2 and
# no caller may silently downgrade a V3 plan to PREASSEMBLED_PARTITION_SINGLE_DEVICE.

DI_PLACEMENT_V3 = "DI_PLACEMENT_V3"
UNBOUND_GRAPH_DIGEST_V3 = "sha256:" + ("0" * 64)


class ExecutionDisposition(str, Enum):
    ACCEPT_IF_EXACT_REUSE = "ACCEPT_IF_EXACT_REUSE"
    ACCEPT_WITH_PREPARATION = "ACCEPT_WITH_PREPARATION"
    REJECT = "REJECT"


class ResidencyTierV3(str, Enum):
    GPU = "GPU"
    RAM = "RAM"
    DISK = "DISK"
    CANONICAL = "CANONICAL"


@dataclass(frozen=True)
class DeviceTopologyProfile:
    """Stable Provider-visible device identity (zero, one, or many devices)."""

    provider: str
    devices: Tuple[str, ...] = ()
    backend: str = "cpu"
    topology_digest: str = ""

    def __post_init__(self) -> None:
        devices = tuple(str(item) for item in self.devices)
        object.__setattr__(self, "devices", devices)
        if not self.provider or len(set(devices)) != len(devices):
            raise ValueError("invalid V3 device topology")
        if any(not item or (item != "cpu" and not item.startswith("cuda:"))
               for item in devices):
            raise ValueError("invalid V3 device identity")
        if not self.backend:
            raise ValueError("V3 topology backend is required")

    def to_dict(self) -> dict[str, Any]:
        return _plain(self)

    def digest(self) -> str:
        return canonical_digest(self.to_dict())


@dataclass(frozen=True)
class DeviceResourceSnapshot:
    device: str
    total_memory_mb: int
    free_memory_mb: int
    active_requests: int = 0
    resource_sequence: int = 1
    captured_at_ms: int = 1
    topology_digest: str = ""

    def __post_init__(self) -> None:
        if (not self.device or self.total_memory_mb < 0
                or self.free_memory_mb < 0
                or self.free_memory_mb > self.total_memory_mb
                or self.active_requests < 0 or self.resource_sequence <= 0
                or self.captured_at_ms <= 0):
            raise ValueError("invalid V3 device resource snapshot")


@dataclass(frozen=True)
class ResidencyProofV3:
    artifact_digest: str
    role: str
    rank: int
    tier: ResidencyTierV3 | str
    device_set: Tuple[str, ...] = ()
    boot_epoch: str = ""
    process_epoch: str = ""
    topology_digest: str = ""
    captured_at_ms: int = 1
    expires_at_ms: int = 2
    proof_digest: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(self, "tier", ResidencyTierV3(self.tier))
        object.__setattr__(self, "device_set", tuple(self.device_set))
        _require_digest(self.artifact_digest, "artifact_digest")
        if (not self.role or self.rank < 0 or self.expires_at_ms <= self.captured_at_ms
                or self.captured_at_ms <= 0 or not self.boot_epoch
                or not self.process_epoch or not self.topology_digest):
            raise ValueError("invalid V3 residency proof")
        if self.proof_digest:
            _require_digest(self.proof_digest, "proof_digest")

    def digest(self) -> str:
        return canonical_digest(self)


def _validate_v3_tuple(status: bool, disposition: ExecutionDisposition,
                       preparation_accepted: bool) -> None:
    valid = {
        (True, ExecutionDisposition.ACCEPT_IF_EXACT_REUSE, False),
        (True, ExecutionDisposition.ACCEPT_WITH_PREPARATION, True),
        (False, ExecutionDisposition.REJECT, False),
    }
    if (bool(status), disposition, bool(preparation_accepted)) not in valid:
        raise ValueError("malformed V3 ACK disposition tuple")


@dataclass(frozen=True)
class ProviderOfferV3:
    request_id: str
    attempt: int
    service: str
    provider: str
    model_digest: str
    graph_digest: str
    status: bool
    execution_disposition: ExecutionDisposition | str
    preparation_accepted: bool
    topology: DeviceTopologyProfile
    resources: Tuple[DeviceResourceSnapshot, ...] = ()
    residency: Tuple[ResidencyProofV3, ...] = ()
    accepted_roles: Tuple[str, ...] = ()
    backends: Tuple[str, ...] = ()
    queue_depth: int = 0
    estimated_wait_ms: float = 0.0
    rtt_ms: float = 0.0
    bandwidth_mbps: float = 0.0
    boot_epoch: str = ""
    captured_at_ms: int = 1
    expires_at_ms: int = 2
    signer_key_id: str = ""
    signature: str = ""
    ack_reservation: bool = False
    schema: str = DI_PLACEMENT_V3
    schema_version: int = 3

    def __post_init__(self) -> None:
        object.__setattr__(self, "execution_disposition",
                           ExecutionDisposition(self.execution_disposition))
        object.__setattr__(self, "resources", tuple(self.resources))
        object.__setattr__(self, "residency", tuple(self.residency))
        object.__setattr__(self, "accepted_roles", tuple(self.accepted_roles))
        object.__setattr__(self, "backends", tuple(self.backends))
        _require_digest(self.model_digest, "model_digest")
        _require_digest(self.graph_digest, "graph_digest")
        _validate_v3_tuple(self.status, self.execution_disposition,
                           self.preparation_accepted)
        if (self.schema != DI_PLACEMENT_V3 or self.schema_version != 3
                or not self.request_id or self.attempt <= 0 or not self.service
                or self.provider != self.topology.provider
                or self.captured_at_ms <= 0 or self.expires_at_ms <= self.captured_at_ms
                or self.queue_depth < 0 or self.estimated_wait_ms < 0
                or self.rtt_ms < 0 or self.bandwidth_mbps < 0
                or not self.boot_epoch or not self.signer_key_id
                or not self.signature
                or self.ack_reservation):
            raise ValueError("invalid V3 Provider offer")
        if self.execution_disposition == ExecutionDisposition.REJECT and self.status:
            raise ValueError("REJECT cannot have positive ACK status")

    def canonical_dict(self) -> dict[str, Any]:
        value = _plain(self)
        value.pop("signature", None)
        return value

    def digest(self) -> str:
        return canonical_digest(self.canonical_dict())

    def to_bytes(self) -> bytes:
        return canonical_bytes(self)

    @classmethod
    def from_bytes(cls, wire: bytes) -> "ProviderOfferV3":
        if len(bytes(wire)) > 1024 * 1024:
            raise ValueError("V3 Provider offer exceeds bounded wire size")
        try:
            payload = json.loads(bytes(wire).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("malformed V3 Provider offer") from exc
        if bytes(wire) != canonical_bytes(payload):
            raise ValueError("V3 Provider offer is not canonically encoded")
        if payload.get("schema") != DI_PLACEMENT_V3 or payload.get("schema_version") != 3:
            raise ValueError("V3/V2 placement schema mismatch")
        topology = DeviceTopologyProfile(**payload.pop("topology"))
        payload["topology"] = topology
        payload["resources"] = tuple(DeviceResourceSnapshot(**item)
                                     for item in payload.get("resources", ()))
        payload["residency"] = tuple(ResidencyProofV3(**item)
                                      for item in payload.get("residency", ()))
        return cls(**payload)


@dataclass(frozen=True)
class ProviderPlanningViewV3:
    provider: str
    offer_digest: str
    request_id: str
    attempt: int
    topology: DeviceTopologyProfile
    resources: Tuple[DeviceResourceSnapshot, ...]
    residency: Tuple[ResidencyProofV3, ...]
    accepted_roles: Tuple[str, ...]
    backends: Tuple[str, ...]
    execution_disposition: ExecutionDisposition
    preparation_accepted: bool
    queue_depth: int
    estimated_wait_ms: float
    rtt_ms: float
    bandwidth_mbps: float

    @classmethod
    def from_offer(
        cls, offer: ProviderOfferV3, *, request_id: str = "",
        model_digest: str = "", graph_digest: str = "", now_ms: int = 0,
        deadline_ms: int = 0, verify_signature=None,
    ) -> "ProviderPlanningViewV3":
        if not offer.status:
            raise ValueError("negative V3 ACK cannot enter strategy input")
        if request_id and offer.request_id != request_id:
            raise ValueError("V3 offer request binding mismatch")
        if model_digest and offer.model_digest != model_digest:
            raise ValueError("V3 offer model binding mismatch")
        if (graph_digest and offer.graph_digest != UNBOUND_GRAPH_DIGEST_V3
                and offer.graph_digest != graph_digest):
            raise ValueError("V3 offer graph binding mismatch")
        if now_ms and (offer.captured_at_ms > now_ms or offer.expires_at_ms <= now_ms):
            raise ValueError("V3 offer is stale or expired")
        if deadline_ms and offer.expires_at_ms < deadline_ms:
            raise ValueError("V3 offer does not cover the request deadline")
        if verify_signature is not None and (
                not callable(verify_signature) or not verify_signature(offer)):
            raise ValueError("V3 Provider offer signature is invalid")
        return cls(
            provider=offer.provider, offer_digest=offer.digest(),
            request_id=offer.request_id, attempt=offer.attempt,
            topology=offer.topology, resources=offer.resources,
            residency=offer.residency, accepted_roles=offer.accepted_roles,
            backends=offer.backends,
            execution_disposition=offer.execution_disposition,
            preparation_accepted=offer.preparation_accepted,
            queue_depth=offer.queue_depth,
            estimated_wait_ms=offer.estimated_wait_ms,
            rtt_ms=offer.rtt_ms, bandwidth_mbps=offer.bandwidth_mbps)

    def to_dict(self) -> dict[str, Any]:
        return _plain(self)


@dataclass(frozen=True)
class RoleAssemblySpec:
    role: str
    rank: int
    layer_begin: int
    layer_end: int
    recipe_digest: str
    artifact_digest: str
    backend: str
    device_set: Tuple[str, ...] = ()
    adapter_id: str = ""
    adapter_version: str = ""
    # Canonical artifact identity is keyed by an adapter-defined semantic
    # role kind, not by the slash-delimited Collaboration role name.  Keep a
    # default for source compatibility with the first V3 draft; live V3
    # planning populates the value from the adapter/graph role family.
    role_kind: str = "PIPELINE_RANGE"

    def __post_init__(self) -> None:
        if (not self.role or self.rank < 0 or self.layer_begin < 0
                or self.layer_end <= self.layer_begin or not self.backend):
            raise ValueError("invalid role assembly spec")
        _require_digest(self.recipe_digest, "recipe_digest")
        _require_digest(self.artifact_digest, "artifact_digest")
        object.__setattr__(self, "device_set", tuple(self.device_set))
        if self.role_kind not in {
                "PIPELINE_RANGE", "TENSOR_RANK", "HYBRID_RANK",
                "COMPONENT_SET",
        }:
            raise ValueError("RoleAssemblySpec role_kind is not allowlisted")
        if bool(self.adapter_id) != bool(self.adapter_version):
            raise ValueError("RoleAssemblySpec adapter identity is incomplete")


@dataclass(frozen=True)
class PlacementProposalV3:
    request_id: str
    attempt: int
    model_digest: str
    graph_digest: str
    roles: Tuple[RoleAssemblySpec, ...]
    provider_by_role: Mapping[str, str]
    dependencies: Tuple[Mapping[str, Any], ...] = ()
    # Optional for source compatibility with the first V3 draft.  A live
    # coordinator must populate it so the sealed plan is bound to the exact
    # graph-derived split candidate rather than only to role/artifact bytes.
    candidate_digest: str = ""
    strategy_name: str = ""
    strategy_version: str = ""
    strategy_state_digest: str = ""

    def __post_init__(self) -> None:
        _require_digest(self.model_digest, "model_digest")
        _require_digest(self.graph_digest, "graph_digest")
        if self.candidate_digest:
            _require_digest(self.candidate_digest, "candidate_digest")
        object.__setattr__(self, "roles", tuple(self.roles))
        object.__setattr__(self, "dependencies", tuple(_freeze(self.dependencies)))
        object.__setattr__(self, "provider_by_role", _freeze(self.provider_by_role))
        role_names = tuple(item.role for item in self.roles)
        # A single-stage pipeline may address a role by its logical name. A
        # tensor group with multiple ranks uses the stable ``role#rank`` key.
        role_keys = tuple(
            name if role_names.count(name) == 1 else f"{name}#{item.rank}"
            for item, name in zip(self.roles, role_names))
        if (not self.request_id or self.attempt <= 0 or not self.roles
                or len(set(role_keys)) != len(role_keys)
                or set(self.provider_by_role) != set(role_keys)):
            raise ValueError("incomplete V3 placement proposal")

    def digest(self) -> str:
        return canonical_digest(self)


@dataclass(frozen=True)
class PlacementPlanCoreV3:
    request_id: str
    attempt: int
    model_digest: str
    graph_digest: str
    roles: Tuple[RoleAssemblySpec, ...]
    provider_by_role: Mapping[str, str]
    dependencies: Tuple[Mapping[str, Any], ...]
    ack_closed_digest: str
    strategy_digest: str
    plan_core_digest: str = ""
    candidate_digest: str = ""

    def __post_init__(self) -> None:
        _require_digest(self.model_digest, "model_digest")
        _require_digest(self.graph_digest, "graph_digest")
        _require_digest(self.ack_closed_digest, "ack_closed_digest")
        _require_digest(self.strategy_digest, "strategy_digest")
        if self.candidate_digest:
            _require_digest(self.candidate_digest, "candidate_digest")
        object.__setattr__(self, "roles", tuple(self.roles))
        object.__setattr__(self, "provider_by_role", _freeze(self.provider_by_role))
        object.__setattr__(self, "dependencies", tuple(_freeze(self.dependencies)))

    def unsigned_dict(self) -> dict[str, Any]:
        value = _plain(self)
        value.pop("plan_core_digest", None)
        return value

    def digest(self) -> str:
        return canonical_digest(self.unsigned_dict())


@dataclass(frozen=True)
class ProviderGrantViewV1:
    provider: str
    request_id: str
    attempt: int
    plan_core_digest: str
    offer_digest: str
    role_digests: Tuple[str, ...]
    security_policy_snapshot_digest: str

    def __post_init__(self) -> None:
        for name, value in (
            ("plan_core_digest", self.plan_core_digest),
            ("offer_digest", self.offer_digest),
            ("security_policy_snapshot_digest", self.security_policy_snapshot_digest),
        ):
            _require_digest(value, name)
        if not self.provider or not self.request_id or self.attempt <= 0:
            raise ValueError("invalid Provider grant view")
        object.__setattr__(self, "role_digests", tuple(self.role_digests))

    def digest(self) -> str:
        return canonical_digest(self)


@dataclass(frozen=True)
class ProviderSelectionProjectionV3:
    provider: str
    request_id: str
    attempt: int
    plan_core_digest: str
    plan_digest: str
    roles: Tuple[RoleAssemblySpec, ...]
    dependencies: Tuple[Mapping[str, Any], ...]
    deadline_ms: int
    schema: str = "ndnsf-di-selection-v3"
    schema_version: int = 3

    def __post_init__(self) -> None:
        for name, value in (("plan_core_digest", self.plan_core_digest),
                            ("plan_digest", self.plan_digest)):
            _require_digest(value, name)
        if not self.provider or not self.request_id or self.attempt <= 0:
            raise ValueError("invalid V3 Selection projection")
        if self.deadline_ms <= 0:
            raise ValueError("invalid V3 Selection projection deadline")
        if self.schema != "ndnsf-di-selection-v3" or self.schema_version != 3:
            raise ValueError("invalid V3 Selection projection schema")
        object.__setattr__(self, "roles", tuple(self.roles))
        object.__setattr__(self, "dependencies", tuple(_freeze(self.dependencies)))

    def to_bytes(self) -> bytes:
        """Encode the opaque per-Provider Selection payload canonically."""

        return canonical_bytes(self)

    @classmethod
    def from_bytes(cls, wire: bytes) -> "ProviderSelectionProjectionV3":
        if len(bytes(wire)) > 1024 * 1024:
            raise ValueError("V3 Selection projection exceeds bounded wire size")
        try:
            payload = json.loads(bytes(wire).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("malformed V3 Selection projection") from exc
        if not isinstance(payload, dict) or bytes(wire) != canonical_bytes(payload):
            raise ValueError("V3 Selection projection is not canonically encoded")
        if (payload.get("schema") != "ndnsf-di-selection-v3"
                or payload.get("schema_version") != 3):
            raise ValueError("V3 Selection projection schema mismatch")
        payload["roles"] = tuple(RoleAssemblySpec(**item)
                                  for item in payload.get("roles", ()))
        payload["dependencies"] = tuple(payload.get("dependencies", ()))
        return cls(**payload)


class PlanSealerV3:
    """Trusted, deterministic boundary between external proposals and Selection."""

    @staticmethod
    def seal_core(request: Mapping[str, Any], proposal: PlacementProposalV3,
                  offers: Mapping[str, ProviderPlanningViewV3]) -> PlacementPlanCoreV3:
        if proposal.request_id != str(request.get("request_id", "")):
            raise ValueError("proposal request binding mismatch")
        now_ms = int(request.get("now_ms", 0) or 0)
        deadline_ms = int(request.get("deadline_ms", 0) or 0)
        if deadline_ms and now_ms and deadline_ms <= now_ms:
            raise TimeoutError("placement proposal deadline expired")
        if int(request.get("attempt", proposal.attempt)) != proposal.attempt:
            raise ValueError("proposal attempt binding mismatch")
        expected_candidate = str(request.get("candidate_digest", "") or "")
        if (expected_candidate and proposal.candidate_digest
                and proposal.candidate_digest != expected_candidate):
            raise ValueError("proposal candidate binding mismatch")
        if expected_candidate and not proposal.candidate_digest:
            raise ValueError("V3 proposal omitted candidate binding")
        role_names = tuple(item.role for item in proposal.roles)
        for role in proposal.roles:
            role_key = (role.role if role_names.count(role.role) == 1
                        else f"{role.role}#{role.rank}")
            provider = str(proposal.provider_by_role[role_key])
            view = offers.get(provider)
            if view is None or view.request_id != proposal.request_id:
                raise ValueError("proposal references unknown Provider offer")
            if (view.execution_disposition == ExecutionDisposition.REJECT
                    or (view.execution_disposition
                        == ExecutionDisposition.ACCEPT_IF_EXACT_REUSE
                        and not any(item.artifact_digest == role.artifact_digest
                                    and item.role == role.role
                                    and item.rank == role.rank
                                    for item in view.residency))):
                raise ValueError("proposal is not covered by Provider disposition/residency")
            if (view.execution_disposition == ExecutionDisposition.ACCEPT_WITH_PREPARATION
                    and not view.preparation_accepted):
                raise ValueError("preparation proposal is not accepted by Provider")
        core = PlacementPlanCoreV3(
            request_id=proposal.request_id, attempt=proposal.attempt,
            model_digest=proposal.model_digest, graph_digest=proposal.graph_digest,
            roles=proposal.roles, provider_by_role=proposal.provider_by_role,
            dependencies=proposal.dependencies,
            ack_closed_digest=str(request["ack_closed_digest"]),
            strategy_digest=canonical_digest({
                "name": proposal.strategy_name, "version": proposal.strategy_version,
                "state": proposal.strategy_state_digest}),
            candidate_digest=proposal.candidate_digest,
        )
        return PlacementPlanCoreV3(
            request_id=core.request_id, attempt=core.attempt,
            model_digest=core.model_digest, graph_digest=core.graph_digest,
            roles=core.roles, provider_by_role=core.provider_by_role,
            dependencies=core.dependencies, ack_closed_digest=core.ack_closed_digest,
            strategy_digest=core.strategy_digest, candidate_digest=core.candidate_digest,
            plan_core_digest=core.digest())

    @staticmethod
    def grant_view(core: PlacementPlanCoreV3, provider: str,
                   offer: ProviderPlanningViewV3,
                   security_policy_snapshot_digest: str) -> ProviderGrantViewV1:
        if provider != offer.provider:
            raise ValueError("grant Provider/offer mismatch")
        role_names = tuple(item.role for item in core.roles)
        role_digests = tuple(sorted(
            canonical_digest(role) for role in core.roles
            if core.provider_by_role[
                role.role if role_names.count(role.role) == 1
                else f"{role.role}#{role.rank}"] == provider))
        return ProviderGrantViewV1(
            provider=provider, request_id=core.request_id, attempt=core.attempt,
            plan_core_digest=core.plan_core_digest or core.digest(),
            offer_digest=offer.offer_digest, role_digests=role_digests,
            security_policy_snapshot_digest=security_policy_snapshot_digest)

    @staticmethod
    def finalize_security(core: PlacementPlanCoreV3,
                          grants: Tuple[ProviderGrantViewV1, ...],
                          security_policy_snapshot_digest: str) -> str:
        expected = {
            provider for provider in core.provider_by_role.values()
        }
        got = {grant.provider for grant in grants}
        if got != expected or any(
                grant.plan_core_digest != (core.plan_core_digest or core.digest())
                or grant.security_policy_snapshot_digest
                != security_policy_snapshot_digest for grant in grants):
            raise ValueError("incomplete or substituted Provider grant cover")
        return canonical_digest({
            "core": core.plan_core_digest or core.digest(),
            "grants": sorted((grant.provider, grant.digest()) for grant in grants),
            "securityPolicySnapshotDigest": security_policy_snapshot_digest,
        })


def decode_placement_wire(wire: bytes) -> DIProviderOfferV2 | ProviderOfferV3:
    """Dispatch exactly one version; never reinterpret V2 bytes as V3."""

    try:
        payload = json.loads(bytes(wire).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("malformed placement wire") from exc
    if not isinstance(payload, dict):
        raise ValueError("placement wire must be an object")
    schema = payload.get("schema")
    if schema == DI_PLACEMENT_V3:
        return ProviderOfferV3.from_bytes(bytes(wire))
    if schema == "ndnsf-di-provider-offer-v2":
        return DIProviderOfferV2.from_bytes(bytes(wire))
    raise ValueError("unknown placement schema; downgrade is forbidden")


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
    "evaluate_placement_strategy", "DI_PLACEMENT_V3", "UNBOUND_GRAPH_DIGEST_V3", "ExecutionDisposition",
    "ResidencyTierV3", "DeviceTopologyProfile", "DeviceResourceSnapshot",
    "ResidencyProofV3", "ProviderOfferV3", "ProviderPlanningViewV3",
    "RoleAssemblySpec", "PlacementProposalV3", "PlacementPlanCoreV3",
    "ProviderGrantViewV1", "ProviderSelectionProjectionV3", "PlanSealerV3",
    "decode_placement_wire",
]

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
    validate_one_to_one_role_provider,
)
from ..core.hybrid_contracts import validate_role_dataflow_contracts
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


_V3_FORBIDDEN_RUNTIME_KEYS = frozenset({
    "admission_fencing_token",
    "authorization_override",
    "device_lease",
    "executable",
    "executable_bytes",
    "executable_path",
    "model_bytes",
    "native_handle",
    "reservation_id",
    "runtime_object",
})


def _validate_v3_data_only(value: Any, *, path: str, depth: int = 0) -> None:
    """Reject executable/runtime objects at the untrusted V3 proposal seam."""

    if depth > 16:
        raise ValueError(f"{path} exceeds the V3 data-only nesting bound")
    if isinstance(value, Mapping):
        if len(value) > 256:
            raise ValueError(f"{path} exceeds the V3 data-only mapping bound")
        for key, item in value.items():
            if not isinstance(key, str) or not key:
                raise ValueError(f"{path} has a non-string or empty key")
            normalized = key.strip().lower().replace("-", "_")
            if normalized in _V3_FORBIDDEN_RUNTIME_KEYS:
                raise ValueError(
                    f"{path}.{key} contains forbidden executable/runtime state")
            _validate_v3_data_only(
                item, path=f"{path}.{key}", depth=depth + 1)
        return
    if isinstance(value, (tuple, list)):
        if len(value) > 4096:
            raise ValueError(f"{path} exceeds the V3 data-only sequence bound")
        for index, item in enumerate(value):
            _validate_v3_data_only(
                item, path=f"{path}[{index}]", depth=depth + 1)
        return
    if value is None or isinstance(value, (bool, int, str)):
        if isinstance(value, str) and len(value) > 1024 * 1024:
            raise ValueError(f"{path} exceeds the V3 data-only string bound")
        return
    if isinstance(value, float) and math.isfinite(value):
        return
    raise ValueError(f"{path} contains opaque executable/runtime content")


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


class ResidencyClassV3(str, Enum):
    """Semantic identity level represented by one bounded ACK proof."""

    CANONICAL = "CANONICAL"
    ASSEMBLED_FRAGMENT = "ASSEMBLED_FRAGMENT"
    LOADED_RUNTIME = "LOADED_RUNTIME"


@dataclass(frozen=True)
class DeviceTopologyProfile:
    """Stable Provider-visible accelerator identity (zero, one, or many)."""

    provider: str
    devices: Tuple[str, ...] = ()
    backend: str = "cpu"
    topology_digest: str = ""

    def __post_init__(self) -> None:
        devices = tuple(str(item) for item in self.devices)
        object.__setattr__(self, "devices", devices)
        if not self.provider or len(set(devices)) != len(devices):
            raise ValueError("invalid V3 device topology")
        if any(not item or not item.startswith("cuda:") for item in devices):
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
    residency_class: ResidencyClassV3 | str = ResidencyClassV3.CANONICAL
    identity_digest: str = ""
    assembly_spec_digest: str = ""
    model_manifest_digest: str = ""
    artifact_profile_digest: str = ""
    graph_digest: str = ""
    backend: str = ""
    protection_epoch: str = ""
    runtime_generation: int = 0
    fencing_token: str = ""
    missing_verified_bytes: int = 0
    estimated_assembly_ms: float = 0.0
    estimated_load_ms: float = 0.0

    def __post_init__(self) -> None:
        object.__setattr__(self, "tier", ResidencyTierV3(self.tier))
        object.__setattr__(self, "residency_class",
                           ResidencyClassV3(self.residency_class))
        object.__setattr__(self, "device_set", tuple(self.device_set))
        _require_digest(self.artifact_digest, "artifact_digest")
        if (not self.role or self.rank < 0 or self.expires_at_ms <= self.captured_at_ms
                or self.captured_at_ms <= 0 or not self.boot_epoch
                or not self.process_epoch or not self.topology_digest):
            raise ValueError("invalid V3 residency proof")
        if self.proof_digest:
            _require_digest(self.proof_digest, "proof_digest")
        for name in (
                "identity_digest", "assembly_spec_digest",
                "model_manifest_digest", "artifact_profile_digest",
                "graph_digest"):
            value = getattr(self, name)
            if value:
                _require_digest(value, name)
        if (self.missing_verified_bytes < 0 or self.estimated_assembly_ms < 0
                or self.estimated_load_ms < 0 or self.runtime_generation < 0):
            raise ValueError("invalid V3 residency cost evidence")

    def is_exact_reuse_proof(self) -> bool:
        """Return whether this proof can authorize no-preparation selection."""

        common = all((
            self.identity_digest, self.assembly_spec_digest,
            self.model_manifest_digest, self.artifact_profile_digest,
            self.graph_digest, self.backend, self.protection_epoch,
        ))
        if not common or self.residency_class is ResidencyClassV3.CANONICAL:
            return False
        if self.residency_class is ResidencyClassV3.ASSEMBLED_FRAGMENT:
            return self.tier in {ResidencyTierV3.DISK, ResidencyTierV3.RAM}
        return bool(
            self.tier in {ResidencyTierV3.GPU, ResidencyTierV3.RAM}
            and self.process_epoch and self.topology_digest
            and self.runtime_generation > 0 and self.fencing_token
            and (self.device_set or is_cpu_backend(self.backend)))

    def matches_exact_role(
        self, role: "RoleAssemblySpec", *, provider_boot_epoch: str,
        topology_digest: str, expected_graph_digest: str = "",
    ) -> bool:
        if (not self.is_exact_reuse_proof()
                or self.role != role.role or self.rank != role.rank
                or self.artifact_digest != role.artifact_digest
                or self.assembly_spec_digest != role.recipe_digest
                or (role.model_manifest_digest
                    and self.model_manifest_digest
                    != role.model_manifest_digest)
                or (role.artifact_profile_digest
                    and self.artifact_profile_digest
                    != role.artifact_profile_digest)
                or self.graph_digest
                != (role.graph_digest or expected_graph_digest)
                or self.protection_epoch != role.protection_epoch
                or self.backend != role.backend):
            return False
        if self.residency_class is ResidencyClassV3.ASSEMBLED_FRAGMENT:
            return True
        expected_devices = tuple(role.device_set)
        return bool(
            self.device_set == expected_devices
            and self.boot_epoch == provider_boot_epoch
            and self.topology_digest == topology_digest)

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
        resource_devices = tuple(item.device for item in self.resources)
        if (len(set(resource_devices)) != len(resource_devices)
                or any(device not in self.topology.devices
                       for device in resource_devices)):
            raise ValueError("V3 resource snapshot is outside offered topology")

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
    boot_epoch: str = ""
    model_digest: str = ""
    graph_digest: str = ""

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
            rtt_ms=offer.rtt_ms, bandwidth_mbps=offer.bandwidth_mbps,
            boot_epoch=offer.boot_epoch, model_digest=offer.model_digest,
            graph_digest=offer.graph_digest)

    def to_dict(self) -> dict[str, Any]:
        return _plain(self)


class DeviceBindingMode(str, Enum):
    CPU = "CPU"
    SINGLE_DEVICE = "SINGLE_DEVICE"


@dataclass(frozen=True)
class ExecutionRole:
    """One complete executable pipeline-stage/rank unit."""

    role_id: str
    stage_id: str
    rank: int
    layer_begin: int
    layer_end: int
    backend: str
    adapter_id: str = ""
    adapter_version: str = ""

    def __post_init__(self) -> None:
        if (not self.role_id or not self.stage_id or self.rank < 0
                or self.layer_begin < 0 or self.layer_end <= self.layer_begin
                or not self.backend):
            raise ValueError("invalid execution role")
        if bool(self.adapter_id) != bool(self.adapter_version):
            raise ValueError("execution role adapter identity is incomplete")

    def digest(self) -> str:
        return canonical_digest(self)


@dataclass(frozen=True)
class DeviceBinding:
    """Provider-local CPU or single-device binding for one execution role."""

    mode: DeviceBindingMode | str
    provider: str
    role: str
    offer_digest: str
    topology_profile_digest: str
    resource_snapshot_digest: str
    resource_sequence: int
    offer_scoped_device_handle: str = ""
    sharing_policy: str = "EXCLUSIVE_ROLE"

    def __post_init__(self) -> None:
        object.__setattr__(self, "mode", DeviceBindingMode(self.mode))
        for name, value in (
            ("offer_digest", self.offer_digest),
            ("topology_profile_digest", self.topology_profile_digest),
            ("resource_snapshot_digest", self.resource_snapshot_digest),
        ):
            _require_digest(value, name)
        if (not self.provider or not self.role or self.resource_sequence <= 0
                or not self.sharing_policy):
            raise ValueError("invalid device binding")
        if (self.mode is DeviceBindingMode.CPU
                and self.offer_scoped_device_handle):
            raise ValueError("CPU binding cannot carry an accelerator handle")
        if (self.mode is DeviceBindingMode.SINGLE_DEVICE
                and not self.offer_scoped_device_handle):
            raise ValueError("single-device binding requires one offer handle")

    def digest(self) -> str:
        return canonical_digest(self)


class TensorEndpointSource(str, Enum):
    ROLE = "ROLE"
    APPLICATION_INPUT = "APPLICATION_INPUT"


def _ndn_component(value: object) -> str:
    """Encode one opaque identity as a reversible slash-free NDN component."""

    return str(value).encode("utf-8").hex()


@dataclass(frozen=True)
class TensorEndpoint:
    """Exact consumer-pull name and integrity contract for one tensor object."""

    producer_namespace: str
    requester: str
    request_id: str
    attempt: int
    plan_digest: str
    group_id: str
    group_epoch: str
    operation: str
    round: int
    source_kind: TensorEndpointSource | str
    producer_role: str
    producer_rank: int
    consumer_role: str
    tensor_id: str
    tensor_digest: str
    layout_digest: str
    microbatch: int
    segment_count: int
    manifest_digest: str
    security_profile: str
    no_progress_deadline_ms: int
    hard_deadline_ms: int
    # All roles authorized to fetch this immutable object. ``consumer_role``
    # identifies the local projection that carries the endpoint; it is not
    # part of the shared object identity.
    consumer_roles: Tuple[str, ...] = ()
    target_layout_digest: str = ""
    endpoint_digest: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(self, "source_kind", TensorEndpointSource(self.source_kind))
        consumers = tuple(str(role) for role in self.consumer_roles)
        if not consumers:
            consumers = (self.consumer_role,)
        object.__setattr__(self, "consumer_roles", consumers)
        if not self.target_layout_digest:
            object.__setattr__(
                self, "target_layout_digest", self.layout_digest)
        for name, value in (
            ("plan_digest", self.plan_digest),
            ("tensor_digest", self.tensor_digest),
            ("layout_digest", self.layout_digest),
            ("target_layout_digest", self.target_layout_digest),
            ("manifest_digest", self.manifest_digest),
        ):
            _require_digest(value, name)
        if (not self.producer_namespace.startswith("/")
                or not self.requester.startswith("/")
                or not self.request_id or self.attempt <= 0
                or not self.group_id or not self.group_epoch
                or not self.operation or self.round < 0
                or self.producer_rank < 0 or not self.consumer_role
                or not self.consumer_roles
                or self.consumer_role not in self.consumer_roles
                or len(set(self.consumer_roles)) != len(self.consumer_roles)
                or not self.tensor_id or self.microbatch < 0
                or self.segment_count <= 0 or not self.security_profile
                or self.no_progress_deadline_ms <= 0
                or self.hard_deadline_ms <= 0):
            raise ValueError("invalid tensor endpoint")
        if (self.source_kind is TensorEndpointSource.ROLE
                and not self.producer_role):
            raise ValueError("role tensor endpoint requires a producer role")
        if (self.source_kind is TensorEndpointSource.APPLICATION_INPUT
                and self.producer_role):
            raise ValueError(
                "application-input endpoint cannot claim a producer role")
        computed = canonical_digest(self.unsigned_dict())
        if self.endpoint_digest:
            _require_digest(self.endpoint_digest, "endpoint_digest")
            if self.endpoint_digest != computed:
                raise ValueError("tensor endpoint digest mismatch")
        else:
            object.__setattr__(self, "endpoint_digest", computed)

    def unsigned_dict(self) -> dict[str, Any]:
        value = _plain(self)
        value.pop("endpoint_digest", None)
        # The exact object is shared by every authorized consumer. The local
        # projection's singular consumer_role must not fork its identity.
        value.pop("consumer_role", None)
        return value

    @property
    def name_template(self) -> str:
        return f"{self.name_prefix}/SEG/<segment>"

    @property
    def name_prefix(self) -> str:
        """Return the exact V3 tensor-object prefix shared with native code."""

        prefix = self.producer_namespace.rstrip("/")
        labelled = (
            ("REQUESTER", self.requester),
            ("REQ", self.request_id),
            ("PLAN", self.plan_digest),
            ("GROUP", self.group_id),
            ("EPOCH", self.group_epoch),
            ("OP", self.operation),
            ("SOURCE-ROLE", self.producer_role or "INPUT"),
            ("TENSOR", self.tensor_id),
        )
        encoded = {label: _ndn_component(value) for label, value in labelled}
        return (
            f"{prefix}/NDNSF-DI/TENSOR/v1"
            f"/REQUESTER/{encoded['REQUESTER']}"
            f"/REQ/{encoded['REQ']}"
            f"/ATTEMPT/{self.attempt}"
            f"/PLAN/{encoded['PLAN']}"
            f"/GROUP/{encoded['GROUP']}"
            f"/EPOCH/{encoded['EPOCH']}"
            f"/OP/{encoded['OP']}"
            f"/ROUND/{self.round}"
            f"/SOURCE-ROLE/{encoded['SOURCE-ROLE']}"
            f"/RANK/{self.producer_rank}"
            f"/TENSOR/{encoded['TENSOR']}/{_ndn_component(self.tensor_digest)}"
            f"/MICROBATCH/{self.microbatch}"
        )

    @property
    def manifest_name(self) -> str:
        return f"{self.name_prefix}/MANIFEST"

    def segment_name(self, segment: int) -> str:
        if segment < 0 or segment >= self.segment_count:
            raise ValueError("tensor segment is outside the declared range")
        return f"{self.name_prefix}/SEG/seg={segment}"


@dataclass(frozen=True)
class TensorObjectManifestV1:
    """Signed root manifest for one immutable request-scoped tensor object.

    ``producer_signature`` authenticates ``unsigned_dict()``.  The manifest is
    additionally carried in an identity-signed NDN Data packet.  The endpoint's
    ``manifest_digest`` is the pre-execution manifest-contract identity; the
    ``object_manifest_digest`` below binds the concrete runtime object and its
    ciphertext segment digests, avoiding a circular requirement to know output
    bytes before the producing role executes.
    """

    capability_digest: str
    epoch_key_id: str
    requester: str
    request_id: str
    attempt_id: str
    plan_digest: str
    group_id: str
    epoch: str
    operation_index: int
    round: int
    operation_kind: str
    producer_role: str
    producer_rank: int
    consumer_roles: Tuple[str, ...]
    microbatch: int
    source_layout_digest: str
    target_layout_digest: str
    tensor_id: str
    tensor_digest: str
    content_digest: str
    total_bytes: int
    segment_size: int
    segment_count: int
    ordered_segment_digests: Tuple[str, ...]
    created_at_ms: int
    no_progress_ms: int
    hard_deadline_ms: int
    endpoint_digest: str
    manifest_contract_digest: str
    producer_signature: str
    object_manifest_digest: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(self, "consumer_roles", tuple(self.consumer_roles))
        object.__setattr__(
            self, "ordered_segment_digests",
            tuple(self.ordered_segment_digests))
        for name, value in (
            ("capability_digest", self.capability_digest),
            ("plan_digest", self.plan_digest),
            ("source_layout_digest", self.source_layout_digest),
            ("target_layout_digest", self.target_layout_digest),
            ("tensor_digest", self.tensor_digest),
            ("content_digest", self.content_digest),
            ("endpoint_digest", self.endpoint_digest),
            ("manifest_contract_digest", self.manifest_contract_digest),
        ):
            _require_digest(value, name)
        for value in self.ordered_segment_digests:
            _require_digest(value, "ordered_segment_digest")
        if (not self.epoch_key_id or not self.requester or not self.request_id
                or not self.attempt_id or not self.group_id
                or not self.operation_kind or not self.producer_role
                or not self.consumer_roles
                or len(set(self.consumer_roles)) != len(self.consumer_roles)
                or not self.tensor_id or self.operation_index < 0
                or self.round < 0 or self.producer_rank < 0
                or self.microbatch < 0 or self.total_bytes <= 0
                or self.segment_size <= 0 or self.segment_count <= 0
                or self.segment_count != len(self.ordered_segment_digests)
                or self.created_at_ms <= 0 or self.no_progress_ms <= 0
                or self.hard_deadline_ms < self.no_progress_ms
                or not self.producer_signature):
            raise ValueError("invalid TensorObjectManifestV1")
        computed = canonical_digest(self.unsigned_dict())
        if self.object_manifest_digest:
            _require_digest(
                self.object_manifest_digest, "object_manifest_digest")
            if self.object_manifest_digest != computed:
                raise ValueError("tensor object manifest digest mismatch")
        else:
            object.__setattr__(self, "object_manifest_digest", computed)

    def unsigned_dict(self) -> dict[str, Any]:
        value = _plain(self)
        value.pop("producer_signature", None)
        value.pop("object_manifest_digest", None)
        return value

    def to_bytes(self) -> bytes:
        return canonical_bytes(self)

    @classmethod
    def from_bytes(cls, wire: bytes) -> "TensorObjectManifestV1":
        try:
            value = json.loads(bytes(wire).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("malformed TensorObjectManifestV1") from exc
        if not isinstance(value, dict) or bytes(wire) != canonical_bytes(value):
            raise ValueError("TensorObjectManifestV1 is not canonical")
        try:
            return cls(**value)
        except (TypeError, ValueError, KeyError) as exc:
            raise ValueError("malformed TensorObjectManifestV1") from exc

    def validate_endpoint(self, endpoint: TensorEndpoint) -> None:
        if (self.requester != endpoint.requester
                or self.request_id != endpoint.request_id
                or self.attempt_id != str(endpoint.attempt)
                or self.plan_digest != endpoint.plan_digest
                or self.group_id != endpoint.group_id
                or self.epoch != endpoint.group_epoch
                or self.operation_kind != endpoint.operation
                or self.round != endpoint.round
                or self.producer_role != endpoint.producer_role
                or self.producer_rank != endpoint.producer_rank
                or self.consumer_roles != endpoint.consumer_roles
                or endpoint.consumer_role not in self.consumer_roles
                or self.microbatch != endpoint.microbatch
                or self.tensor_id != endpoint.tensor_id
                or self.tensor_digest != endpoint.tensor_digest
                or self.source_layout_digest != endpoint.layout_digest
                or self.target_layout_digest != endpoint.target_layout_digest
                or self.segment_count > endpoint.segment_count
                or self.endpoint_digest != endpoint.endpoint_digest
                or self.manifest_contract_digest != endpoint.manifest_digest
                or self.no_progress_ms != endpoint.no_progress_deadline_ms
                or self.hard_deadline_ms != endpoint.hard_deadline_ms):
            raise ValueError("tensor object manifest endpoint mismatch")


class ReadinessMode(str, Enum):
    ALL = "ALL"
    ANY = "ANY"
    QUORUM = "QUORUM"


@dataclass(frozen=True)
class ReadinessPredicate:
    mode: ReadinessMode | str
    endpoint_digests: Tuple[str, ...]
    quorum: int = 0

    def __post_init__(self) -> None:
        object.__setattr__(self, "mode", ReadinessMode(self.mode))
        object.__setattr__(self, "endpoint_digests", tuple(self.endpoint_digests))
        if (not self.endpoint_digests
                or len(set(self.endpoint_digests)) != len(self.endpoint_digests)):
            raise ValueError("readiness predicate has invalid endpoint cover")
        for value in self.endpoint_digests:
            _require_digest(value, "readiness endpoint_digest")
        if self.mode is ReadinessMode.QUORUM:
            if self.quorum <= 0 or self.quorum > len(self.endpoint_digests):
                raise ValueError("invalid readiness quorum")
        elif self.quorum != 0:
            raise ValueError("non-quorum readiness cannot carry quorum")


@dataclass(frozen=True)
class RoleDataflowContract:
    """Sealed consumer-pull dataflow authority for exactly one role."""

    request_id: str
    attempt: int
    plan_digest: str
    role: str
    may_publish: Tuple[TensorEndpoint, ...] = ()
    must_fetch: Tuple[TensorEndpoint, ...] = ()
    wait_for: Tuple[ReadinessPredicate, ...] = ()
    terminal_response_owner: bool = False
    dataflow_digest: str = ""

    def __post_init__(self) -> None:
        _require_digest(self.plan_digest, "plan_digest")
        object.__setattr__(self, "may_publish", tuple(self.may_publish))
        object.__setattr__(self, "must_fetch", tuple(self.must_fetch))
        object.__setattr__(self, "wait_for", tuple(self.wait_for))
        if not self.request_id or self.attempt <= 0 or not self.role:
            raise ValueError("invalid role dataflow contract")
        endpoints = self.may_publish + self.must_fetch
        if len({item.endpoint_digest for item in endpoints}) != len(endpoints):
            raise ValueError("role dataflow contract contains a duplicate endpoint")
        for endpoint in endpoints:
            if (endpoint.request_id != self.request_id
                    or endpoint.attempt != self.attempt
                    or endpoint.plan_digest != self.plan_digest):
                raise ValueError("tensor endpoint plan/attempt binding mismatch")
        if any(endpoint.producer_role != self.role
               for endpoint in self.may_publish):
            raise ValueError("mayPublish endpoint has the wrong producer role")
        if any(endpoint.consumer_role != self.role
               for endpoint in self.must_fetch):
            raise ValueError("mustFetch endpoint has the wrong consumer role")
        fetch_ids = {item.endpoint_digest for item in self.must_fetch}
        waited_ids = {
            endpoint_digest
            for predicate in self.wait_for
            for endpoint_digest in predicate.endpoint_digests
        }
        if waited_ids - fetch_ids:
            raise ValueError("waitFor references an undeclared mustFetch endpoint")
        computed = canonical_digest(self.unsigned_dict())
        if self.dataflow_digest:
            _require_digest(self.dataflow_digest, "dataflow_digest")
            if self.dataflow_digest != computed:
                raise ValueError("role dataflow digest mismatch")
        else:
            object.__setattr__(self, "dataflow_digest", computed)

    def unsigned_dict(self) -> dict[str, Any]:
        value = _plain(self)
        value.pop("dataflow_digest", None)
        return value

    def to_bytes(self) -> bytes:
        return canonical_bytes(self)

    @classmethod
    def from_bytes(cls, wire: bytes) -> "RoleDataflowContract":
        try:
            value = json.loads(bytes(wire).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("malformed role dataflow contract") from exc
        if not isinstance(value, dict):
            raise ValueError("role dataflow contract must be an object")
        try:
            value["may_publish"] = tuple(
                TensorEndpoint(**item) for item in value.get("may_publish", ()))
            value["must_fetch"] = tuple(
                TensorEndpoint(**item) for item in value.get("must_fetch", ()))
            value["wait_for"] = tuple(
                ReadinessPredicate(**item) for item in value.get("wait_for", ()))
            return cls(**value)
        except (TypeError, ValueError, KeyError) as exc:
            raise ValueError("malformed role dataflow contract") from exc


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
    required_device_memory_mb: int = 0
    adapter_id: str = ""
    adapter_version: str = ""
    # Canonical artifact identity is keyed by an adapter-defined semantic
    # role kind, not by the slash-delimited Collaboration role name.  Keep a
    # default for source compatibility with the first V3 draft; live V3
    # planning populates the value from the adapter/graph role family.
    role_kind: str = "PIPELINE_RANGE"
    model_manifest_digest: str = ""
    artifact_profile_digest: str = ""
    graph_digest: str = ""
    canonical_initializer_digest: str = ""
    adapter_descriptor_digest: str = ""
    assembler_descriptor_digest: str = ""
    backend_abi: str = ""
    node_indices: Tuple[int, ...] = ()
    expected_inputs: Tuple[Mapping[str, Any], ...] = ()
    expected_outputs: Tuple[Mapping[str, Any], ...] = ()
    precision: str = ""
    quantization: str = "none"
    layout: str = "native"
    padding: str = "none"
    resource_envelope: Mapping[str, int] = field(default_factory=dict)
    protection_epoch: str = "plaintext-v1"

    def __post_init__(self) -> None:
        if (not self.role or self.rank < 0 or self.layer_begin < 0
                or self.layer_end <= self.layer_begin or not self.backend
                or self.required_device_memory_mb < 0
                or not self.protection_epoch):
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
        identity_fields = (
            self.model_manifest_digest, self.artifact_profile_digest,
            self.graph_digest, self.canonical_initializer_digest,
            self.adapter_descriptor_digest,
            self.assembler_descriptor_digest,
        )
        if any(identity_fields) and not all(identity_fields):
            raise ValueError("RoleAssemblySpec V3 assembly identity is incomplete")
        for name, value in (
            ("model_manifest_digest", self.model_manifest_digest),
            ("artifact_profile_digest", self.artifact_profile_digest),
            ("graph_digest", self.graph_digest),
            ("canonical_initializer_digest", self.canonical_initializer_digest),
            ("adapter_descriptor_digest", self.adapter_descriptor_digest),
            ("assembler_descriptor_digest", self.assembler_descriptor_digest),
        ):
            if value:
                _require_digest(value, name)
        indices = tuple(int(index) for index in self.node_indices)
        if (indices and (any(index < 0 for index in indices)
                         or indices != tuple(sorted(set(indices))))):
            raise ValueError("RoleAssemblySpec node cover is not exact")
        object.__setattr__(self, "node_indices", indices)
        object.__setattr__(self, "expected_inputs", tuple(
            _freeze(dict(item)) for item in self.expected_inputs))
        object.__setattr__(self, "expected_outputs", tuple(
            _freeze(dict(item)) for item in self.expected_outputs))
        envelope = {str(name): int(value)
                    for name, value in dict(self.resource_envelope).items()}
        if any(not name or value < 0 for name, value in envelope.items()):
            raise ValueError("RoleAssemblySpec resource envelope is invalid")
        object.__setattr__(self, "resource_envelope", _freeze(envelope))
        if any(identity_fields) and (
                not self.backend_abi or not indices
                or not self.expected_inputs or not self.expected_outputs
                or not self.precision or not self.quantization
                or not self.layout or not self.padding
                or not envelope):
            raise ValueError("RoleAssemblySpec certified recipe is incomplete")


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
        _validate_v3_data_only(self.dependencies, path="proposal.dependencies")
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
        validate_one_to_one_role_provider(
            self.provider_by_role, expected_roles=role_keys)

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
        role_names = tuple(item.role for item in self.roles)
        role_keys = tuple(
            name if role_names.count(name) == 1 else f"{name}#{item.rank}"
            for item, name in zip(self.roles, role_names))
        if (not self.roles or len(set(role_keys)) != len(role_keys)
                or set(self.provider_by_role) != set(role_keys)):
            raise ValueError(
                "V3 plan core requires one-to-one role/Provider ownership")
        validate_one_to_one_role_provider(
            self.provider_by_role, expected_roles=role_keys)

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
        if len(self.role_digests) != 1:
            raise ValueError("Provider grant must bind exactly one role")

    def digest(self) -> str:
        return canonical_digest(self)


@dataclass(frozen=True)
class GrantBindingV1:
    """Non-secret reference to one authority-issued Provider KeyGrant."""

    provider: str
    grant_name: str
    grant_digest: str
    request_id: str
    attempt: int
    plan_core_digest: str
    security_policy_snapshot_digest: str
    protection_epoch: str

    def __post_init__(self) -> None:
        if (not self.provider or not self.grant_name or not self.request_id
                or self.attempt <= 0 or not self.protection_epoch
                or not self.grant_name.startswith("/")
                or len(self.grant_name.encode("utf-8")) > 4096):
            raise ValueError("invalid Provider grant binding")
        _require_digest(self.grant_digest, "grant_digest")
        _require_digest(self.plan_core_digest, "plan_core_digest")
        _require_digest(
            self.security_policy_snapshot_digest,
            "security_policy_snapshot_digest")

    def digest(self) -> str:
        return canonical_digest(self)


@dataclass(frozen=True)
class ProviderSelectionProjectionV3:
    provider: str
    request_id: str
    attempt: int
    plan_core_digest: str
    plan_digest: str
    ack_closed_digest: str
    offer_digest: str
    security_policy_snapshot_digest: str
    roles: Tuple[RoleAssemblySpec, ...]
    dependencies: Tuple[Mapping[str, Any], ...]
    deadline_ms: int
    execution_role: ExecutionRole
    assembly: RoleAssemblySpec
    dataflow: RoleDataflowContract
    device_binding: DeviceBinding
    # Lower-hex canonical GroupCapabilityV1 wire. Empty for plans whose data
    # dependencies remain within one Provider.
    group_capability_v1: str = ""
    # Non-secret reference to this Provider's authority-issued KeyGrant.
    # Plaintext roles carry no grant binding.
    grant_binding: GrantBindingV1 | None = None
    # Optional per-role execution binding carried by the request-scoped
    # Selection projection.  DATA_V1 selections which also use execution
    # leases must not fall back to the legacy semicolon assignment envelope;
    # the Provider needs these authenticated fields on the same projection.
    # Keys are local role names and values use the stable snake_case wire keys
    # documented by the native Provider parser.
    execution_bindings: Mapping[str, Mapping[str, str]] = field(
        default_factory=dict)
    schema: str = "ndnsf-di-selection-v3"
    schema_version: int = 3

    def __post_init__(self) -> None:
        for name, value in (
            ("plan_core_digest", self.plan_core_digest),
            ("plan_digest", self.plan_digest),
            ("ack_closed_digest", self.ack_closed_digest),
            ("offer_digest", self.offer_digest),
            ("security_policy_snapshot_digest",
             self.security_policy_snapshot_digest),
        ):
            _require_digest(value, name)
        if not self.provider or not self.request_id or self.attempt <= 0:
            raise ValueError("invalid V3 Selection projection")
        if len(self.roles) != 1:
            raise ValueError(
                "V3 Selection projection must contain exactly one role")
        if self.assembly != self.roles[0]:
            raise ValueError("V3 Selection assembly/role mismatch")
        if (self.execution_role.role_id != self.dataflow.role
                or self.execution_role.role_id != self.device_binding.role
                or self.execution_role.stage_id != self.assembly.role
                or self.execution_role.rank != self.assembly.rank
                or self.execution_role.layer_begin != self.assembly.layer_begin
                or self.execution_role.layer_end != self.assembly.layer_end
                or self.execution_role.backend != self.assembly.backend
                or self.execution_role.adapter_id != self.assembly.adapter_id
                or self.execution_role.adapter_version
                != self.assembly.adapter_version
                or self.dataflow.request_id != self.request_id
                or self.dataflow.attempt != self.attempt
                or self.dataflow.plan_digest != self.plan_digest
                or self.device_binding.provider != self.provider
                or self.device_binding.offer_digest != self.offer_digest):
            raise ValueError("V3 Selection role projection binding mismatch")
        cpu = is_cpu_backend(self.assembly.backend)
        if ((cpu and (self.assembly.device_set
                      or self.device_binding.mode is not DeviceBindingMode.CPU))
                or (not cpu and (
                    len(self.assembly.device_set) != 1
                    or self.device_binding.mode
                    is not DeviceBindingMode.SINGLE_DEVICE
                    or self.assembly.device_set[0]
                    != self.device_binding.offer_scoped_device_handle))):
            raise ValueError("V3 Selection CPU/single-device binding mismatch")
        if self.deadline_ms <= 0:
            raise ValueError("invalid V3 Selection projection deadline")
        if self.group_capability_v1:
            if (len(self.group_capability_v1) % 2 != 0
                    or len(self.group_capability_v1) > 2 * 1024 * 1024
                    or self.group_capability_v1 != self.group_capability_v1.lower()):
                raise ValueError("invalid GroupCapabilityV1 projection")
            try:
                bytes.fromhex(self.group_capability_v1)
            except ValueError as exc:
                raise ValueError("invalid GroupCapabilityV1 projection") from exc
        protected = self.assembly.protection_epoch != "plaintext-v1"
        if (protected and (
                self.grant_binding is None
                or self.grant_binding.provider != self.provider)):
            raise ValueError(
                "protected V3 Selection requires its Provider grant binding")
        if not protected and self.grant_binding is not None:
            raise ValueError(
                "plaintext V3 Selection must not carry a grant binding")
        bindings: dict[str, Mapping[str, str]] = {}
        allowed_binding_keys = frozenset({
            "provider_boot_id",
            "lease_id",
            "lease_epoch",
            "lease_plan_digest",
            "lease_binding_proof",
            "lease_provider_role_count",
            "activation_digest",
            "activation_members",
            "activation_local_member",
            "fencing_token",
        })
        for role, raw_binding in self.execution_bindings.items():
            role_name = str(role)
            if not role_name or not isinstance(raw_binding, Mapping):
                raise ValueError("invalid V3 execution binding")
            if len(raw_binding) > 16:
                raise ValueError("V3 execution binding has too many fields")
            binding = {str(key): str(value)
                       for key, value in raw_binding.items()}
            if (set(binding) - allowed_binding_keys
                    or any(not value or len(value) > 4096
                           for value in binding.values())):
                raise ValueError("invalid V3 execution binding")
            if ("lease_plan_digest" in binding
                    and not binding["lease_plan_digest"].startswith("sha256:")):
                raise ValueError("invalid V3 execution lease plan digest")
            bindings[role_name] = binding
        if self.schema != "ndnsf-di-selection-v3" or self.schema_version != 3:
            raise ValueError("invalid V3 Selection projection schema")
        object.__setattr__(self, "roles", tuple(self.roles))
        object.__setattr__(self, "dependencies", tuple(_freeze(self.dependencies)))
        object.__setattr__(self, "execution_bindings", _freeze(bindings))

    def to_bytes(self) -> bytes:
        """Encode the opaque per-Provider Selection payload canonically."""

        # Keep the original V3 wire byte-for-byte compatible when no lease
        # binding is present.  New lease-aware projections include the
        # execution_bindings object and remain canonical JSON.
        payload = _plain(self)
        if not self.execution_bindings:
            payload.pop("execution_bindings", None)
        if self.grant_binding is None:
            payload.pop("grant_binding", None)
        return canonical_bytes(payload)

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
        try:
            payload["execution_role"] = ExecutionRole(
                **payload["execution_role"])
            payload["assembly"] = RoleAssemblySpec(**payload["assembly"])
            payload["dataflow"] = RoleDataflowContract.from_bytes(
                canonical_bytes(payload["dataflow"]))
            payload["device_binding"] = DeviceBinding(
                **payload["device_binding"])
            if payload.get("grant_binding") is not None:
                payload["grant_binding"] = GrantBindingV1(
                    **payload["grant_binding"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError("malformed V3 role projection") from exc
        if "execution_bindings" in payload:
            raw_bindings = payload["execution_bindings"]
            if not isinstance(raw_bindings, dict):
                raise ValueError("malformed V3 execution bindings")
            payload["execution_bindings"] = {
                str(role): dict(binding)
                for role, binding in raw_bindings.items()
                if isinstance(binding, dict)
            }
            if len(payload["execution_bindings"]) != len(raw_bindings):
                raise ValueError("malformed V3 execution bindings")
        return cls(**payload)


class PlanSealerV3:
    """Trusted, deterministic boundary between external proposals and Selection."""

    @staticmethod
    def seal_core(request: Mapping[str, Any], proposal: PlacementProposalV3,
                  offers: Mapping[str, ProviderPlanningViewV3]) -> PlacementPlanCoreV3:
        _validate_v3_data_only(proposal.dependencies,
                               path="proposal.dependencies")
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
                        and not any(item.matches_exact_role(
                            role, provider_boot_epoch=view.boot_epoch,
                            topology_digest=view.topology.digest(),
                            expected_graph_digest=proposal.graph_digest)
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
                          grants: Tuple[GrantBindingV1, ...],
                          security_policy_snapshot_digest: str) -> str:
        _require_digest(
            security_policy_snapshot_digest,
            "security_policy_snapshot_digest")
        role_names = tuple(item.role for item in core.roles)
        expected = {
            core.provider_by_role[
                role.role if role_names.count(role.role) == 1
                else f"{role.role}#{role.rank}"]
            for role in core.roles
            if role.protection_epoch != "plaintext-v1"
        }
        if any(not isinstance(grant, GrantBindingV1) for grant in grants):
            raise ValueError("incomplete or substituted Provider grant cover")
        got = {grant.provider for grant in grants}
        core_digest = core.plan_core_digest or core.digest()
        protected_epoch_by_provider = {
            core.provider_by_role[
                role.role if role_names.count(role.role) == 1
                else f"{role.role}#{role.rank}"]: role.protection_epoch
            for role in core.roles
            if role.protection_epoch != "plaintext-v1"
        }
        if (len(got) != len(grants) or got != expected
                or any(grant.request_id != core.request_id
                       or grant.attempt != core.attempt
                       or grant.plan_core_digest != core_digest
                       or grant.security_policy_snapshot_digest
                       != security_policy_snapshot_digest
                       or grant.protection_epoch
                       != protected_epoch_by_provider.get(grant.provider)
                       for grant in grants)):
            raise ValueError("incomplete or substituted Provider grant cover")
        return canonical_digest({
            "core": core.plan_core_digest or core.digest(),
            "grants": sorted(
                (grant.provider, grant.grant_name, grant.grant_digest)
                for grant in grants),
            "securityPolicySnapshotDigest": security_policy_snapshot_digest,
        })

    @staticmethod
    def project(
        core: PlacementPlanCoreV3,
        *,
        plan_digest: str,
        provider: str,
        offer: ProviderPlanningViewV3,
        security_policy_snapshot_digest: str,
        execution_role: ExecutionRole,
        assembly: RoleAssemblySpec,
        dataflow: RoleDataflowContract,
        device_binding: DeviceBinding,
        deadline_ms: int,
        dependencies: Tuple[Mapping[str, Any], ...] = (),
        group_capability_v1: str = "",
        grant_binding: GrantBindingV1 | None = None,
    ) -> ProviderSelectionProjectionV3:
        """Create one complete, non-executable Provider Selection projection."""

        _require_digest(plan_digest, "plan_digest")
        _require_digest(
            security_policy_snapshot_digest,
            "security_policy_snapshot_digest")
        if provider != offer.provider:
            raise ValueError("projection Provider/offer mismatch")
        role_names = tuple(item.role for item in core.roles)
        role_key = (assembly.role if role_names.count(assembly.role) == 1
                    else f"{assembly.role}#{assembly.rank}")
        if (core.provider_by_role.get(role_key) != provider
                or assembly not in core.roles):
            raise ValueError("projection role is not owned by Provider")
        _validate_v3_data_only(dependencies, path="projection.dependencies")
        return ProviderSelectionProjectionV3(
            provider=provider,
            request_id=core.request_id,
            attempt=core.attempt,
            plan_core_digest=core.plan_core_digest or core.digest(),
            plan_digest=plan_digest,
            ack_closed_digest=core.ack_closed_digest,
            offer_digest=offer.offer_digest,
            security_policy_snapshot_digest=security_policy_snapshot_digest,
            roles=(assembly,),
            dependencies=dependencies,
            deadline_ms=deadline_ms,
            execution_role=execution_role,
            assembly=assembly,
            dataflow=dataflow,
            device_binding=device_binding,
            group_capability_v1=group_capability_v1,
            grant_binding=grant_binding,
        )


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
    "ResidencyTierV3", "ResidencyClassV3", "DeviceTopologyProfile",
    "DeviceResourceSnapshot",
    "ResidencyProofV3", "ProviderOfferV3", "ProviderPlanningViewV3",
    "DeviceBindingMode", "ExecutionRole", "DeviceBinding",
    "TensorEndpointSource", "TensorEndpoint", "TensorObjectManifestV1",
    "ReadinessMode",
    "ReadinessPredicate", "RoleDataflowContract",
    "validate_role_dataflow_contracts", "RoleAssemblySpec",
    "PlacementProposalV3", "PlacementPlanCoreV3",
    "ProviderGrantViewV1", "GrantBindingV1",
    "ProviderSelectionProjectionV3", "PlanSealerV3",
    "decode_placement_wire",
]

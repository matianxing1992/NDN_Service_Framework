"""Workload-neutral NDNSF-DI execution and consistency contracts.

This module intentionally depends only on the Python standard library.  It is
the canonical serialization boundary shared by APP, providers, optimizers and
native adapters; policy and model semantics do not belong here.
"""

from __future__ import annotations

from dataclasses import dataclass, field, fields as dataclass_fields, is_dataclass
from enum import Enum
import base64
import hashlib
import json
from typing import Any, Callable, ClassVar, Iterable, Mapping


DATA_DRIVEN_V2 = "DATA_DRIVEN_V2"
LEGACY_READY_SET_V1 = "LEGACY_READY_SET_V1"
DI_PLACEMENT_V3 = "DI_PLACEMENT_V3"


def placement_wire_schema(wire: bytes | bytearray | str) -> str:
    """Return the explicit placement schema without performing downgrade."""

    try:
        raw = wire.encode() if isinstance(wire, str) else bytes(wire)
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError) as exc:
        raise ValueError("malformed placement wire") from exc
    if not isinstance(payload, dict) or not isinstance(payload.get("schema"), str):
        raise ValueError("placement wire lacks schema")
    schema = payload["schema"]
    if schema not in {DI_PLACEMENT_V3, "ndnsf-di-provider-offer-v2"}:
        raise ValueError("unknown placement schema; downgrade is forbidden")
    return schema


def to_plain(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if is_dataclass(value):
        # Avoid dataclasses.asdict(): it deep-copies MappingProxyType fields
        # used by sealed V3 contracts and therefore fails before canonical
        # serialization. Walk fields directly, as the SDK serializer does.
        return {
            item.name: to_plain(getattr(value, item.name))
            for item in dataclass_fields(value)
        }
    if isinstance(value, Mapping):
        return {str(key): to_plain(item) for key, item in value.items()}
    if isinstance(value, (tuple, list, set, frozenset)):
        return [to_plain(item) for item in value]
    return value


def canonical_json(value: Any) -> str:
    return json.dumps(to_plain(value), sort_keys=True, separators=(",", ":"))


def canonical_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode()).hexdigest()


def _decode(wire: bytes, schema: str) -> dict[str, Any]:
    try:
        payload = json.loads(bytes(wire).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("malformed canonical contract") from exc
    if not isinstance(payload, dict) or payload.get("schema") != schema:
        raise ValueError(f"unsupported contract schema; expected {schema}")
    return payload


class CanonicalContract:
    SCHEMA: ClassVar[str]

    def to_dict(self) -> dict[str, Any]:
        payload = to_plain(self)
        payload["schema"] = self.SCHEMA
        return payload

    def to_bytes(self) -> bytes:
        return canonical_json(self.to_dict()).encode()

    def digest(self) -> str:
        return canonical_digest(self.to_dict())


def _require_sha256(value: str, field_name: str) -> None:
    if (not isinstance(value, str) or len(value) != 71
            or not value.startswith("sha256:")):
        raise ValueError(f"{field_name} must be a canonical sha256 digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(
            f"{field_name} must be a canonical sha256 digest") from exc


def _strict_v2_decode(
    wire: bytes, schema: str, expected_keys: set[str],
) -> dict[str, Any]:
    if len(bytes(wire)) > 4 * 1024 * 1024:
        raise ValueError(f"{schema} exceeds the bounded wire size")
    payload = _decode(wire, schema)
    if set(payload) != expected_keys | {"schema"}:
        raise ValueError(f"{schema} contains unknown or missing fields")
    if bytes(wire) != canonical_json(payload).encode():
        raise ValueError(f"{schema} is not canonically encoded")
    if (payload.get("schema_version") != 2
            or payload.get("canonical_encoding_version")
            != "canonical-json-v1"
            or payload.get("capability_version")
            != "SELECTION_DATAFLOW_V2"
            or payload.get("acceptance_predicate_version")
            != "DI_ACCEPTANCE_V2"):
        raise ValueError(f"{schema} version mismatch; downgrade is forbidden")
    payload.pop("schema")
    return payload


@dataclass(frozen=True)
class DIRequestEnvelopeV2(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-request-envelope-v2"
    invocation_id: str
    request_id: str
    attempt: int
    service: str
    model_name: str
    model_identity_hash: str
    task_kind: str
    input_manifest_digest: str
    input_payload_b64: str
    options_payload_b64: str
    plan_deadline_ms: int
    security_domain: str
    model: Mapping[str, Any] = field(default_factory=dict)
    task: Mapping[str, Any] = field(default_factory=dict)
    schema_version: int = 2
    canonical_encoding_version: str = "canonical-json-v1"
    capability_version: str = "SELECTION_DATAFLOW_V2"
    acceptance_predicate_version: str = "DI_ACCEPTANCE_V2"

    def __post_init__(self) -> None:
        model = dict(self.model) or {
            "name": self.model_name,
            "identity_hash": self.model_identity_hash,
        }
        task = dict(self.task) or {"name": self.task_kind}
        object.__setattr__(self, "model", model)
        object.__setattr__(self, "task", task)
        if (not self.invocation_id or not self.request_id or self.attempt <= 0
                or not self.service.startswith("/") or not self.model_name
                or not self.task_kind
                or self.plan_deadline_ms <= 0 or not self.security_domain
                or self.schema_version != 2
                or self.canonical_encoding_version != "canonical-json-v1"
                or self.capability_version != "SELECTION_DATAFLOW_V2"
                or self.acceptance_predicate_version != "DI_ACCEPTANCE_V2"):
            raise ValueError("invalid DIRequestEnvelopeV2")
        if (model.get("name") != self.model_name
                or model.get("identity_hash") != self.model_identity_hash
                or task.get("name") != self.task_kind):
            raise ValueError("DI request model/task compatibility view mismatch")
        _require_sha256(self.model_identity_hash, "model_identity_hash")
        _require_sha256(
            self.input_manifest_digest, "input_manifest_digest")
        try:
            input_payload = base64.b64decode(
                self.input_payload_b64, validate=True)
            options_payload = base64.b64decode(
                self.options_payload_b64, validate=True)
        except ValueError as exc:
            raise ValueError("DI request payload is not canonical base64") from exc
        if (base64.b64encode(input_payload).decode("ascii")
                != self.input_payload_b64
                or base64.b64encode(options_payload).decode("ascii")
                != self.options_payload_b64):
            raise ValueError("DI request payload is not canonical base64")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "DIRequestEnvelopeV2":
        keys = {item.name for item in dataclass_fields(cls)}
        return cls(**_strict_v2_decode(wire, cls.SCHEMA, keys))


@dataclass(frozen=True)
class DIDataDependencyV2:
    """One committed data edge carried inside a Provider assignment."""

    producers: tuple[str, ...]
    consumers: tuple[str, ...]
    key_scope: str
    topic_prefix: str
    required: bool = True
    tensors: tuple[str, ...] = ()
    object_name_template: str = ""
    expected_segments: int = 0
    expected_bytes: int = 0

    def __post_init__(self) -> None:
        object.__setattr__(self, "producers", tuple(self.producers))
        object.__setattr__(self, "consumers", tuple(self.consumers))
        object.__setattr__(self, "tensors", tuple(self.tensors))
        if (not self.producers or not self.consumers
                or len(set(self.producers)) != len(self.producers)
                or len(set(self.consumers)) != len(self.consumers)
                or any(not value for value in self.producers + self.consumers)
                or not self.key_scope
                or not self.topic_prefix.startswith("/")
                or any(not value for value in self.tensors)
                or self.expected_segments < 0 or self.expected_bytes < 0):
            raise ValueError("invalid DI data dependency")


@dataclass(frozen=True)
class DIRoleAssignmentV2:
    role: str
    graph_node_id: str
    layer_start: int | None
    layer_end: int | None
    artifact_digest: str
    dependency_digest: str
    adapter_id: str
    adapter_version: str
    dependencies: tuple[DIDataDependencyV2, ...]
    required_gpu_mib: int
    input_grant_digests: tuple[str, ...] = ()
    required_input_scopes: tuple[str, ...] = ()
    backend: str = ""
    device: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(
            self, "input_grant_digests", tuple(self.input_grant_digests))
        object.__setattr__(
            self, "required_input_scopes", tuple(self.required_input_scopes))
        object.__setattr__(self, "dependencies", tuple(self.dependencies))
        dependency_scopes = [item.key_scope for item in self.dependencies]
        required_inputs = tuple(
            item.key_scope for item in self.dependencies
            if item.required and self.role in item.consumers)
        if (not self.role or not self.graph_node_id
                or (self.layer_start is None) != (self.layer_end is None)
                or self.layer_start is not None and (
                    self.layer_start < 0 or self.layer_end <= self.layer_start)
                or not self.adapter_id or not self.adapter_version
                or self.required_gpu_mib <= 0
                or not self.input_grant_digests
                or len(self.input_grant_digests) > 64
                or len(self.required_input_scopes) > 64
                or len(set(self.required_input_scopes))
                != len(self.required_input_scopes)
                or any(not value for value in self.required_input_scopes)
                or len(set(dependency_scopes)) != len(dependency_scopes)
                or any(self.role not in item.producers + item.consumers
                       for item in self.dependencies)
                or required_inputs != self.required_input_scopes
                or bool(self.backend) != bool(self.device)):
            raise ValueError("invalid DI role assignment")
        _require_sha256(self.artifact_digest, "artifact_digest")
        _require_sha256(self.dependency_digest, "dependency_digest")
        if self.dependency_digest != canonical_digest(self.dependencies):
            raise ValueError(
                "DI role dependency digest does not match committed edges")
        for value in self.input_grant_digests:
            _require_sha256(value, "input_grant_digest")


@dataclass(frozen=True)
class ExactPrefixKvKeyV1(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-exact-prefix-kv-key-v1"
    model_identity_hash: str
    model_semantics_digest: str
    adapter_digest: str
    runner_digest: str
    split_digest: str
    tokenizer_digest: str
    prefix_token_digest: str
    prefix_length: int
    position_digest: str
    layer_start: int
    layer_end: int
    precision: str
    layout_digest: str
    runtime_abi_digest: str
    security_domain: str

    def __post_init__(self) -> None:
        if (self.prefix_length <= 0
                or self.layer_start < 0 or self.layer_end <= self.layer_start
                or not self.precision or not self.security_domain):
            raise ValueError("invalid exact-prefix KV key")
        for name in (
                "model_identity_hash", "model_semantics_digest",
                "adapter_digest", "runner_digest", "split_digest",
                "tokenizer_digest", "prefix_token_digest", "position_digest",
                "layout_digest", "runtime_abi_digest"):
            _require_sha256(getattr(self, name), name)

    @classmethod
    def create(cls, **values) -> "ExactPrefixKvKeyV1":
        return cls(**values)


@dataclass(frozen=True)
class StateReuseBindingV2:
    contract_kind: str
    state_key: str
    provider: str
    provider_boot_epoch: str
    cache_epoch: int
    pin_id: str
    security_domain: str
    layer_start: int
    layer_end: int
    expires_at_ms: int
    authorized_requester: str
    migration_mode: str = "DISABLED"
    fallback: str = "CLEAN_COMPUTE"

    def __post_init__(self) -> None:
        if (self.contract_kind not in {
                "EXACT_PREFIX_KV_V1", "GENERIC_DERIVED_STATE_V1"}
                or not self.provider or len(self.provider_boot_epoch) < 8
                or self.cache_epoch <= 0 or not self.pin_id
                or not self.security_domain or self.layer_start < 0
                or self.layer_end <= self.layer_start
                or self.expires_at_ms <= 0 or not self.authorized_requester
                or self.migration_mode not in {
                    "DISABLED", "EXPLICIT_ENCRYPTED"}
                or self.fallback not in {"CLEAN_COMPUTE", "REPLAN"}):
            raise ValueError("invalid StateReuseBindingV2")
        _require_sha256(self.state_key, "state_key")

    def revalidate(
        self, *, now_ms: int, provider: str, boot_epoch: str,
        cache_epoch: int, pin_live: bool, security_domain: str,
        requester: str, layer_start: int, layer_end: int,
    ) -> None:
        if (now_ms >= self.expires_at_ms or not pin_live
                or provider != self.provider
                or boot_epoch != self.provider_boot_epoch
                or cache_epoch != self.cache_epoch
                or security_domain != self.security_domain
                or requester != self.authorized_requester
                or layer_start != self.layer_start
                or layer_end != self.layer_end):
            raise ValueError("derived-state reuse binding is stale or mismatched")


@dataclass(frozen=True)
class ShardResidencyEvidenceV2(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-shard-residency-evidence-v2"
    artifact_digest: str
    provider: str
    provider_boot_epoch: str
    tier: str
    cache_epoch: int
    captured_at_ms: int
    expires_at_ms: int
    pin_until_ms: int
    reload_feasible: bool
    content_verified: bool
    signer_key_id: str
    signature: str

    def __post_init__(self) -> None:
        allowed = {
            "PINNED_GPU", "RELOAD_SAFE_GPU", "HOST_RAM", "DISK",
            "REPOSITORY",
        }
        if (not self.provider or len(self.provider_boot_epoch) < 8
                or self.tier not in allowed or self.cache_epoch <= 0
                or self.captured_at_ms <= 0
                or self.expires_at_ms <= self.captured_at_ms
                or self.pin_until_ms < self.captured_at_ms
                or not self.content_verified or not self.signer_key_id
                or not self.signature
                or self.tier == "PINNED_GPU"
                and self.pin_until_ms <= self.captured_at_ms
                or self.tier == "RELOAD_SAFE_GPU"
                and not self.reload_feasible):
            raise ValueError("invalid signed shard residency evidence")
        _require_sha256(self.artifact_digest, "artifact_digest")

    def revalidate(
        self, *, now_ms: int, provider: str, boot_epoch: str,
        cache_epoch: int, pin_live: bool,
        verify_signature: Callable[["ShardResidencyEvidenceV2"], bool],
    ) -> None:
        if (provider != self.provider or boot_epoch != self.provider_boot_epoch
                or cache_epoch != self.cache_epoch
                or now_ms >= self.expires_at_ms
                or self.tier == "PINNED_GPU" and (
                    not pin_live or now_ms >= self.pin_until_ms)
                or not verify_signature(self)):
            raise ValueError("shard residency evidence is stale or untrusted")


@dataclass(frozen=True)
class ProviderResidencyIdentity(CanonicalContract):
    """Complete compatibility identity for one reusable Provider shard.

    The durable portion deliberately excludes request identity, Provider boot
    epoch, and device so compatible requests can share verified bytes.  RAM
    reuse is additionally boot-bound and GPU reuse is boot-and-device-bound by
    the Provider residency ledger.
    """

    SCHEMA: ClassVar[str] = "ndnsf-di-provider-residency-identity-v1"
    model_content_digest: str
    graph_digest: str
    partition_digest: str
    artifact_digest: str
    adapter_id: str
    adapter_version: str
    backend: str
    device: str
    provider_boot_epoch: str

    def __post_init__(self) -> None:
        for name in (
                "model_content_digest", "graph_digest", "partition_digest",
                "artifact_digest"):
            _require_sha256(getattr(self, name), name)
        if not all((self.adapter_id, self.adapter_version, self.backend,
                    self.device, self.provider_boot_epoch)):
            raise ValueError("Provider residency identity is incomplete")

    @property
    def durable_key(self) -> tuple[str, ...]:
        return (
            self.model_content_digest, self.graph_digest,
            self.partition_digest, self.artifact_digest,
            self.adapter_id, self.adapter_version, self.backend,
        )


@dataclass(frozen=True)
class DISelectionAssignmentV2(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-selection-assignment-v2"
    invocation_id: str
    request_id: str
    attempt: int
    plan_digest: str
    provider: str
    provider_boot_epoch: str
    offer_digest: str
    resource_sequence: int
    roles: tuple[DIRoleAssignmentV2, ...]
    artifact_set_digest: str
    dependency_graph_digest: str
    deadline_ms: int
    generation: int
    state_reuse_binding: StateReuseBindingV2 | None = None
    execution_policy: str = DATA_DRIVEN_V2
    schema_version: int = 2
    canonical_encoding_version: str = "canonical-json-v1"
    capability_version: str = "SELECTION_DATAFLOW_V2"
    acceptance_predicate_version: str = "DI_ACCEPTANCE_V2"

    def __post_init__(self) -> None:
        object.__setattr__(self, "roles", tuple(self.roles))
        if self.execution_policy != DATA_DRIVEN_V2:
            raise ValueError(
                "DI Selection execution policy must be DATA_DRIVEN_V2")
        if (not self.invocation_id or not self.request_id or self.attempt <= 0
                or not self.provider or len(self.provider_boot_epoch) < 8
                or self.resource_sequence <= 0 or not self.roles
                or len(self.roles) > 256
                or self.deadline_ms <= 0 or self.generation <= 0
                or len({item.role for item in self.roles}) != len(self.roles)
                or self.schema_version != 2
                or self.canonical_encoding_version != "canonical-json-v1"
                or self.capability_version != "SELECTION_DATAFLOW_V2"
                or self.acceptance_predicate_version != "DI_ACCEPTANCE_V2"):
            raise ValueError("invalid DISelectionAssignmentV2")
        grants = [
            value for role in self.roles
            for value in role.input_grant_digests
        ]
        if len(grants) != len(set(grants)):
            raise ValueError(
                "DI input grant cannot be replayed across roles")
        ranged = [
            role for role in self.roles if role.layer_start is not None
        ]
        for index, left in enumerate(ranged):
            for right in ranged[index + 1:]:
                if (left.layer_start < right.layer_end
                        and right.layer_start < left.layer_end):
                    raise ValueError(
                        "DI Provider role layer ownership overlaps")
        for name in (
                "plan_digest", "offer_digest", "artifact_set_digest",
                "dependency_graph_digest"):
            _require_sha256(getattr(self, name), name)

    def required_gpu_mib(self) -> int:
        return sum(item.required_gpu_mib for item in self.roles)

    def role_tuple_digest(self) -> str:
        return canonical_digest(to_plain(self.roles))

    @classmethod
    def from_bytes(cls, wire: bytes) -> "DISelectionAssignmentV2":
        keys = {item.name for item in dataclass_fields(cls)}
        payload = _strict_v2_decode(wire, cls.SCHEMA, keys)
        roles = payload.get("roles")
        if not isinstance(roles, list):
            raise ValueError("DI role tuple must be an array")
        decoded_roles = []
        for item in roles:
            if not isinstance(item, dict):
                raise ValueError("DI role tuple contains a non-object")
            role_payload = dict(item)
            dependencies = role_payload.get("dependencies")
            if not isinstance(dependencies, list):
                raise ValueError("DI role assignment lacks dependency edges")
            role_payload["dependencies"] = tuple(
                DIDataDependencyV2(**dependency)
                for dependency in dependencies
            )
            decoded_roles.append(DIRoleAssignmentV2(**role_payload))
        payload["roles"] = tuple(decoded_roles)
        binding = payload.get("state_reuse_binding")
        if binding is not None:
            if not isinstance(binding, dict):
                raise ValueError("state reuse binding must be an object")
            payload["state_reuse_binding"] = StateReuseBindingV2(**binding)
        return cls(**payload)


@dataclass(frozen=True)
class DISelectionAcceptanceV2(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-selection-acceptance-v2"
    invocation_id: str
    request_id: str
    attempt: int
    assignment_digest: str
    provider: str
    provider_boot_epoch: str
    offer_digest: str
    role_tuple_digest: str
    accepted_gpu_mib: int
    generation: int
    transaction_id: str
    accepted_at_ms: int
    expires_at_ms: int
    schema_version: int = 2
    canonical_encoding_version: str = "canonical-json-v1"
    capability_version: str = "SELECTION_DATAFLOW_V2"
    acceptance_predicate_version: str = "DI_ACCEPTANCE_V2"

    def __post_init__(self) -> None:
        if (not self.invocation_id or not self.request_id or self.attempt <= 0
                or not self.provider or len(self.provider_boot_epoch) < 8
                or self.accepted_gpu_mib <= 0 or self.generation <= 0
                or not self.transaction_id or self.accepted_at_ms <= 0
                or self.expires_at_ms <= self.accepted_at_ms
                or self.schema_version != 2
                or self.canonical_encoding_version != "canonical-json-v1"
                or self.capability_version != "SELECTION_DATAFLOW_V2"
                or self.acceptance_predicate_version != "DI_ACCEPTANCE_V2"):
            raise ValueError("invalid DISelectionAcceptanceV2")
        for name in (
                "assignment_digest", "offer_digest", "role_tuple_digest"):
            _require_sha256(getattr(self, name), name)

    @classmethod
    def from_bytes(cls, wire: bytes) -> "DISelectionAcceptanceV2":
        keys = {item.name for item in dataclass_fields(cls)}
        return cls(**_strict_v2_decode(wire, cls.SCHEMA, keys))

    def validate_assignment(
        self, assignment: DISelectionAssignmentV2, *,
        transaction_id: str,
    ) -> None:
        if (self.invocation_id != assignment.invocation_id
                or self.request_id != assignment.request_id
                or self.attempt != assignment.attempt
                or self.assignment_digest != assignment.digest()
                or self.provider != assignment.provider
                or self.provider_boot_epoch
                != assignment.provider_boot_epoch
                or self.offer_digest != assignment.offer_digest
                or self.role_tuple_digest != assignment.role_tuple_digest()
                or self.accepted_gpu_mib != assignment.required_gpu_mib()
                or self.generation != assignment.generation
                or self.transaction_id != transaction_id
                or self.expires_at_ms != assignment.deadline_ms):
            raise ValueError(
                "DI Selection acceptance is not exactly assignment-bound")


@dataclass(frozen=True)
class R1FieldContract(CanonicalContract):
    """Bounded workload-neutral mirror of Core's canonical R1 field container."""

    MAX_FIELDS: ClassVar[int] = 64
    MAX_FIELD_NAME: ClassVar[int] = 64
    MAX_FIELD_VALUE: ClassVar[int] = 4096
    fields: Mapping[str, str] = field(default_factory=dict)

    def __post_init__(self) -> None:
        normalized = {str(key): str(value) for key, value in self.fields.items()}
        if (len(normalized) > self.MAX_FIELDS
                or any(not key or len(key) > self.MAX_FIELD_NAME
                       or len(value.encode()) > self.MAX_FIELD_VALUE
                       for key, value in normalized.items())):
            raise ValueError("R1 field contract exceeds bounds")
        object.__setattr__(self, "fields", normalized)

    @classmethod
    def from_bytes(cls, wire: bytes):
        payload = _decode(wire, cls.SCHEMA)
        fields = payload.get("fields", {})
        if not isinstance(fields, dict):
            raise ValueError("R1 fields must be an object")
        return cls(fields={str(key): str(value) for key, value in fields.items()})


class RequestCapabilities(R1FieldContract):
    SCHEMA = "ndnsf-request-capabilities-v1"


class EncryptedRequestInput(R1FieldContract):
    SCHEMA = "ndnsf-encrypted-request-input-v1"


class SelectionInputKeyOffer(R1FieldContract):
    SCHEMA = "ndnsf-selection-input-key-offer-v1"


class SelectionInputKeyGrant(R1FieldContract):
    SCHEMA = "ndnsf-selection-input-key-grant-v1"


class ReservationLease(R1FieldContract):
    SCHEMA = "ndnsf-di-reservation-lease-v1"


class SelectionDecision(R1FieldContract):
    SCHEMA = "ndnsf-selection-decision-v1"


class SelectionDecisionReceipt(R1FieldContract):
    SCHEMA = "ndnsf-selection-decision-receipt-v1"


class RecipientEncryptedAssignment(R1FieldContract):
    SCHEMA = "ndnsf-recipient-encrypted-assignment-v1"


class StageInputEvidence(R1FieldContract):
    SCHEMA = "ndnsf-di-stage-input-evidence-v1"


class StageAbort(R1FieldContract):
    SCHEMA = "ndnsf-di-stage-abort-v1"


class SelectionDecisionTombstone(R1FieldContract):
    SCHEMA = "ndnsf-selection-decision-tombstone-v1"


def validate_r1_capability_combination(*, di_reservation: bool,
                                       gated_input: bool,
                                       has_deployment_intent: bool,
                                       targeted_fast_path: bool) -> None:
    if di_reservation and not has_deployment_intent:
        raise ValueError("DIReservationSelectionV1 requires DeploymentIntent")
    if gated_input and targeted_fast_path:
        raise ValueError("SelectionGatedInputV1 requires ACK/Selection")


@dataclass(frozen=True)
class CorePlanDependency(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-core-plan-dependency-v1"
    producer_role: str
    consumer_role: str
    key_scope: str
    expected_digest: str = ""

    def __post_init__(self) -> None:
        if not self.producer_role or not self.consumer_role or not self.key_scope:
            raise ValueError("plan dependency requires producer, consumer and scope")


@dataclass(frozen=True)
class CoreExecutionPlan(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-core-execution-plan-v1"
    plan_id: str
    model_id: str
    roles: tuple[str, ...]
    dependencies: tuple[CorePlanDependency, ...] = ()
    artifact_digests: Mapping[str, str] = field(default_factory=dict)
    revision: str = ""

    def __post_init__(self) -> None:
        if not self.plan_id or not self.model_id or not self.roles:
            raise ValueError("execution plan requires identity, model and roles")
        if len(set(self.roles)) != len(self.roles):
            raise ValueError("execution plan roles must be unique")
        known = set(self.roles)
        if any(dep.producer_role not in known or dep.consumer_role not in known
               for dep in self.dependencies):
            raise ValueError("dependency references an unknown role")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "CoreExecutionPlan":
        payload = _decode(wire, cls.SCHEMA)
        return cls(
            plan_id=str(payload.get("plan_id", "")),
            model_id=str(payload.get("model_id", "")),
            roles=tuple(str(item) for item in payload.get("roles", [])),
            dependencies=tuple(CorePlanDependency(
                producer_role=str(item.get("producer_role", "")),
                consumer_role=str(item.get("consumer_role", "")),
                key_scope=str(item.get("key_scope", "")),
                expected_digest=str(item.get("expected_digest", "")),
            ) for item in payload.get("dependencies", [])),
            artifact_digests={str(k): str(v) for k, v in
                              dict(payload.get("artifact_digests", {})).items()},
            revision=str(payload.get("revision", "")),
        )


@dataclass(frozen=True, order=True)
class ProviderAssignment(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-core-provider-assignment-v1"
    role: str
    provider: str
    provider_boot_epoch: str
    lease_id: str = ""
    resource_binding_digest: str = ""

    def __post_init__(self) -> None:
        if not self.role or not self.provider or not self.provider_boot_epoch:
            raise ValueError("provider assignment is missing a binding identity")

    def membership_key(self) -> tuple[str, str, str, str, str]:
        return (self.role, self.provider, self.provider_boot_epoch,
                self.lease_id, self.resource_binding_digest)


@dataclass(frozen=True)
class CoreAssignment(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-core-assignment-v1"
    assignment_id: str
    request_id: str
    attempt_epoch: int
    plan_digest: str
    providers: tuple[ProviderAssignment, ...]

    def __post_init__(self) -> None:
        if (not self.assignment_id or not self.request_id or self.attempt_epoch <= 0
                or not self.plan_digest or not self.providers):
            raise ValueError("invalid Core assignment")
        if len({item.role for item in self.providers}) != len(self.providers):
            raise ValueError("a role may have only one primary assignment")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "CoreAssignment":
        payload = _decode(wire, cls.SCHEMA)
        return cls(
            assignment_id=str(payload.get("assignment_id", "")),
            request_id=str(payload.get("request_id", "")),
            attempt_epoch=int(payload.get("attempt_epoch", 0)),
            plan_digest=str(payload.get("plan_digest", "")),
            providers=tuple(ProviderAssignment(
                role=str(item.get("role", "")),
                provider=str(item.get("provider", "")),
                provider_boot_epoch=str(item.get("provider_boot_epoch", "")),
                lease_id=str(item.get("lease_id", "")),
                resource_binding_digest=str(item.get("resource_binding_digest", "")),
            ) for item in payload.get("providers", [])),
        )


@dataclass(frozen=True)
class AssignmentContext(CanonicalContract):
    """Immutable per-request placement authority passed explicitly to calls."""

    SCHEMA: ClassVar[str] = "ndnsf-di-assignment-context-v1"
    request_id: str
    attempt_epoch: int
    plan_digest: str
    model_variant_id: str
    role_providers: tuple[tuple[str, str], ...]
    original_deadline_ms: int
    excluded_providers: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if (not self.request_id or self.attempt_epoch <= 0 or not self.plan_digest
                or not self.model_variant_id or self.original_deadline_ms <= 0
                or not self.role_providers):
            raise ValueError("invalid assignment context")
        roles = [role for role, provider in self.role_providers
                 if role and provider]
        if len(roles) != len(self.role_providers) or len(set(roles)) != len(roles):
            raise ValueError("assignment context roles must bind exactly once")
        if any(provider in set(self.excluded_providers)
               for _, provider in self.role_providers):
            raise ValueError("assignment context binds an excluded provider")

    def providers_by_role(self) -> dict[str, str]:
        return dict(self.role_providers)

    @classmethod
    def from_bytes(cls, wire: bytes) -> "AssignmentContext":
        payload = _decode(wire, cls.SCHEMA)
        return cls(
            request_id=str(payload.get("request_id", "")),
            attempt_epoch=int(payload.get("attempt_epoch", 0)),
            plan_digest=str(payload.get("plan_digest", "")),
            model_variant_id=str(payload.get("model_variant_id", "")),
            role_providers=tuple((str(item[0]), str(item[1]))
                                 for item in payload.get("role_providers", ())),
            original_deadline_ms=int(payload.get("original_deadline_ms", 0)),
            excluded_providers=tuple(str(item) for item in
                                     payload.get("excluded_providers", ())),
        )


@dataclass(frozen=True)
class CoreExecutionEvidence(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-core-execution-evidence-v1"
    provider: str
    provider_boot_epoch: str
    request_id: str
    attempt_epoch: int
    plan_digest: str
    assignment_digest: str
    runner_identity: str
    result_digest: str = ""

    def __post_init__(self) -> None:
        if (not self.provider or not self.provider_boot_epoch or not self.request_id
                or self.attempt_epoch <= 0 or not self.plan_digest
                or not self.assignment_digest or not self.runner_identity):
            raise ValueError("execution evidence is missing identity bindings")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "CoreExecutionEvidence":
        payload = _decode(wire, cls.SCHEMA)
        return cls(**{name: payload.get(name, "") for name in (
            "provider", "provider_boot_epoch", "request_id", "plan_digest",
            "assignment_digest", "runner_identity", "result_digest")},
            attempt_epoch=int(payload.get("attempt_epoch", 0)))


@dataclass(frozen=True)
class RequestCoordinatorBinding(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-request-coordinator-binding-v1"
    requester_identity: str
    request_id: str
    attempt_epoch: int
    intent_digest: str
    objective_digest: str
    snapshot_digest: str
    plan_digest: str
    assignment_digest: str
    execution_deadline_ms: int
    result_rendezvous: str

    def __post_init__(self) -> None:
        required = (self.requester_identity, self.request_id, self.intent_digest,
                    self.objective_digest, self.snapshot_digest, self.plan_digest,
                    self.assignment_digest, self.result_rendezvous)
        if not all(required) or self.attempt_epoch <= 0 or self.execution_deadline_ms <= 0:
            raise ValueError("invalid requester coordinator binding")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "RequestCoordinatorBinding":
        payload = _decode(wire, cls.SCHEMA)
        values = {name: payload.get(name, "") for name in (
            "requester_identity", "request_id", "intent_digest", "objective_digest",
            "snapshot_digest", "plan_digest", "assignment_digest", "result_rendezvous")}
        return cls(**values, attempt_epoch=int(payload.get("attempt_epoch", 0)),
                   execution_deadline_ms=int(payload.get("execution_deadline_ms", 0)))


class ReceiptOperation(str, Enum):
    PREPARE = "PREPARE"
    COMMIT = "COMMIT"
    ABORT = "ABORT"
    RELEASE = "RELEASE"
    ACTIVATE = "ACTIVATE"


@dataclass(frozen=True)
class AuthenticatedProviderReceipt(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-authenticated-provider-receipt-v1"
    operation: ReceiptOperation
    requester_identity: str
    request_id: str
    attempt_epoch: int
    intent_digest: str
    provider: str
    provider_boot_epoch: str
    lease_id: str
    lease_state: str
    expires_at_ms: int
    plan_digest: str
    role: str
    resource_binding_digest: str
    idempotency_key: str
    data_name: str
    signer_certificate: str
    wire_digest: str
    status: bool = True
    reason: str = "OK"

    def __post_init__(self) -> None:
        required = (self.requester_identity, self.request_id, self.intent_digest,
                    self.provider, self.provider_boot_epoch, self.lease_id,
                    self.lease_state, self.plan_digest, self.role,
                    self.resource_binding_digest, self.idempotency_key,
                    self.data_name, self.signer_certificate, self.wire_digest)
        if not all(required) or self.attempt_epoch <= 0 or self.expires_at_ms <= 0:
            raise ValueError("provider receipt lacks authenticated binding evidence")

    def membership_key(self) -> tuple[str, str, str, str, str]:
        return (self.role, self.provider, self.provider_boot_epoch,
                self.lease_id, self.resource_binding_digest)

    @classmethod
    def from_bytes(cls, wire: bytes) -> "AuthenticatedProviderReceipt":
        payload = _decode(wire, cls.SCHEMA)
        payload.pop("schema", None)
        payload["operation"] = ReceiptOperation(payload["operation"])
        payload["attempt_epoch"] = int(payload.get("attempt_epoch", 0))
        payload["expires_at_ms"] = int(payload.get("expires_at_ms", 0))
        payload["status"] = bool(payload.get("status", False))
        return cls(**payload)


@dataclass(frozen=True)
class ExecutionCommitCertificate(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-execution-commit-certificate-v1"
    coordinator: RequestCoordinatorBinding
    expected_members: tuple[ProviderAssignment, ...]
    commit_receipts: tuple[AuthenticatedProviderReceipt, ...]
    output_visibility_epoch: int

    def __post_init__(self) -> None:
        if self.output_visibility_epoch <= 0:
            raise ValueError("output visibility epoch must be positive")
        expected = {item.membership_key() for item in self.expected_members}
        observed = {item.membership_key() for item in self.commit_receipts}
        if len(observed) != len(self.commit_receipts) or observed != expected:
            raise ValueError("commit receipt set does not exactly match assignment")
        for receipt in self.commit_receipts:
            if (receipt.operation is not ReceiptOperation.COMMIT or not receipt.status
                    or receipt.requester_identity != self.coordinator.requester_identity
                    or receipt.request_id != self.coordinator.request_id
                    or receipt.attempt_epoch != self.coordinator.attempt_epoch
                    or receipt.intent_digest != self.coordinator.intent_digest
                    or receipt.plan_digest != self.coordinator.plan_digest):
                raise ValueError("commit receipt is not bound to the coordinator")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "ExecutionCommitCertificate":
        payload = _decode(wire, cls.SCHEMA)
        coordinator = RequestCoordinatorBinding.from_bytes(canonical_json({
            **payload["coordinator"], "schema": RequestCoordinatorBinding.SCHEMA}).encode())
        members = tuple(ProviderAssignment(**item) for item in payload["expected_members"])
        receipts = tuple(AuthenticatedProviderReceipt.from_bytes(canonical_json({
            **item, "schema": AuthenticatedProviderReceipt.SCHEMA}).encode())
            for item in payload["commit_receipts"])
        return cls(coordinator, members, receipts,
                   int(payload.get("output_visibility_epoch", 0)))


@dataclass(frozen=True)
class DeploymentLifecycleRecord(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-deployment-lifecycle-record-v1"
    deployment_id: str
    owner_identity: str
    lifecycle_epoch: int
    state: str
    state_digest: str
    desired_action: str
    action_digest: str
    expected_previous_epoch: int
    expected_previous_state_digest: str
    provider_boot_epochs: Mapping[str, str] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if (not self.deployment_id or not self.owner_identity
                or self.lifecycle_epoch <= 0 or not self.state or not self.state_digest
                or not self.desired_action or not self.action_digest):
            raise ValueError("invalid deployment lifecycle record")

    def accepts_successor(self, successor: "DeploymentLifecycleRecord") -> bool:
        return (
            successor.deployment_id == self.deployment_id
            and successor.owner_identity == self.owner_identity
            and successor.lifecycle_epoch == self.lifecycle_epoch + 1
            and successor.expected_previous_epoch == self.lifecycle_epoch
            and successor.expected_previous_state_digest == self.state_digest
        )

    @classmethod
    def from_bytes(cls, wire: bytes) -> "DeploymentLifecycleRecord":
        payload = _decode(wire, cls.SCHEMA); payload.pop("schema", None)
        payload["lifecycle_epoch"] = int(payload.get("lifecycle_epoch", 0))
        payload["expected_previous_epoch"] = int(payload.get("expected_previous_epoch", 0))
        return cls(**payload)


@dataclass(frozen=True)
class OrphanCleanupRecord(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-orphan-cleanup-record-v1"
    provider: str
    provider_boot_epoch: str
    sweep_id: str
    swept_at_ms: int
    reclaimed: Mapping[str, tuple[str, ...]]
    tombstone_high_watermarks: Mapping[str, int]
    tombstones_expire_at_ms: int
    next_retry_at_ms: int = 0

    def __post_init__(self) -> None:
        if (not self.provider or not self.provider_boot_epoch or not self.sweep_id
                or self.swept_at_ms <= 0
                or self.tombstones_expire_at_ms < self.swept_at_ms):
            raise ValueError("invalid orphan cleanup record")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "OrphanCleanupRecord":
        payload = _decode(wire, cls.SCHEMA)
        return cls(
            provider=str(payload.get("provider", "")),
            provider_boot_epoch=str(payload.get("provider_boot_epoch", "")),
            sweep_id=str(payload.get("sweep_id", "")),
            swept_at_ms=int(payload.get("swept_at_ms", 0)),
            reclaimed={str(k): tuple(str(x) for x in v) for k, v in
                       dict(payload.get("reclaimed", {})).items()},
            tombstone_high_watermarks={str(k): int(v) for k, v in
                                       dict(payload.get("tombstone_high_watermarks", {})).items()},
            tombstones_expire_at_ms=int(payload.get("tombstones_expire_at_ms", 0)),
            next_retry_at_ms=int(payload.get("next_retry_at_ms", 0)),
        )


@dataclass(frozen=True)
class ResultRendezvousRecord(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-result-rendezvous-record-v1"
    requester_identity: str
    request_id: str
    attempt_epoch: int
    certificate_digest: str
    output_epoch: int
    terminal_digest: str
    visible: bool
    expires_at_ms: int

    def __post_init__(self) -> None:
        if (not self.requester_identity or not self.request_id
                or self.attempt_epoch <= 0 or not self.certificate_digest
                or self.output_epoch <= 0 or not self.terminal_digest
                or self.expires_at_ms <= 0):
            raise ValueError("invalid result rendezvous record")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "ResultRendezvousRecord":
        payload = _decode(wire, cls.SCHEMA); payload.pop("schema", None)
        for name in ("attempt_epoch", "output_epoch", "expires_at_ms"):
            payload[name] = int(payload.get(name, 0))
        payload["visible"] = bool(payload.get("visible", False))
        return cls(**payload)


def exact_receipt_membership(
    expected: Iterable[ProviderAssignment],
    receipts: Iterable[AuthenticatedProviderReceipt],
) -> bool:
    expected_keys = [item.membership_key() for item in expected]
    receipt_keys = [item.membership_key() for item in receipts]
    return len(receipt_keys) == len(set(receipt_keys)) and set(expected_keys) == set(receipt_keys)


# Spec 129 canonical authority contracts.  These deliberately live beside the
# v1 readers above so old journals can be imported without leaving two writers.
@dataclass(frozen=True)
class DeploymentIntent(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-deployment-intent-v1"
    requester_identity: str
    request_id: str
    attempt: int
    artifact_locator: str
    artifact_digest: str
    allowed_variants: tuple[str, ...]
    required_roles: tuple[str, ...]
    deadline_ms: int
    protected_input_locator: str = ""
    constraints: Mapping[str, str] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if (not self.requester_identity or not self.request_id or self.attempt <= 0
                or not self.artifact_locator or not self.artifact_digest
                or not self.allowed_variants or not self.required_roles
                or self.deadline_ms <= 0):
            raise ValueError("invalid DeploymentIntent")
        if len(set(self.allowed_variants)) != len(self.allowed_variants):
            raise ValueError("DeploymentIntent variants must be unique")
        if len(set(self.required_roles)) != len(self.required_roles):
            raise ValueError("DeploymentIntent roles must be unique")
        if (len(self.allowed_variants) > 32 or len(self.required_roles) > 64
                or len(self.constraints) > 64):
            raise ValueError("DeploymentIntent exceeds collection bounds")


@dataclass(frozen=True)
class ProviderCapabilityOffer(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-provider-capability-offer-v1"
    provider: str
    provider_boot_epoch: str
    supported_versions: tuple[int, ...]
    artifact_resident: bool
    runtimes: tuple[str, ...]
    devices: tuple[str, ...]
    precisions: tuple[str, ...]
    roles: tuple[str, ...]
    available_capacity: int
    queue_depth: int
    estimated_prepare_ms: int
    expires_at_ms: int
    lease_id: str = ""

    def __post_init__(self) -> None:
        if (not self.provider or not self.provider_boot_epoch
                or 1 not in self.supported_versions or not self.runtimes
                or not self.devices or not self.roles or self.available_capacity < 0
                or self.queue_depth < 0 or self.estimated_prepare_ms < 0
                or self.expires_at_ms <= 0):
            raise ValueError("invalid ProviderCapabilityOffer")
        if any(len(values) > 64 for values in
               (self.supported_versions, self.runtimes, self.devices,
                self.precisions, self.roles)):
            raise ValueError("ProviderCapabilityOffer exceeds collection bounds")


@dataclass(frozen=True)
class DeploymentPlan(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-deployment-plan-v2"
    requester_identity: str
    request_id: str
    attempt: int
    model_variant: str
    artifact_digests: tuple[str, ...]
    assignments: tuple[ProviderAssignment, ...]
    execution_target: str
    deadline_ms: int
    selection_digest: str
    status_handles: Mapping[str, str] = field(default_factory=dict)
    requester_encryption_key: str = ""
    execution_policy: str = LEGACY_READY_SET_V1

    def __post_init__(self) -> None:
        required = (self.requester_identity, self.request_id, self.model_variant,
                    self.execution_target, self.selection_digest)
        if (not all(required) or self.attempt <= 0 or self.deadline_ms <= 0
                or not self.artifact_digests or not self.assignments):
            raise ValueError("invalid DeploymentPlan")
        if self.execution_policy not in {
                DATA_DRIVEN_V2, LEGACY_READY_SET_V1}:
            raise ValueError("unsupported DeploymentPlan execution policy")
        keys = [(item.provider, item.role) for item in self.assignments]
        if len(keys) != len(set(keys)) or len({item.role for item in self.assignments}) != len(self.assignments):
            raise ValueError("DeploymentPlan requires exact unique provider-role membership")
        providers = {item.provider for item in self.assignments}
        if set(self.status_handles) - providers:
            raise ValueError("status handle names an unselected Provider")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "DeploymentPlan":
        payload = _decode(wire, cls.SCHEMA)
        return cls(
            requester_identity=str(payload.get("requester_identity", "")),
            request_id=str(payload.get("request_id", "")), attempt=int(payload.get("attempt", 0)),
            model_variant=str(payload.get("model_variant", "")),
            artifact_digests=tuple(str(v) for v in payload.get("artifact_digests", ())),
            assignments=tuple(ProviderAssignment(**item) for item in payload.get("assignments", ())),
            execution_target=str(payload.get("execution_target", "")),
            deadline_ms=int(payload.get("deadline_ms", 0)),
            selection_digest=str(payload.get("selection_digest", "")),
            status_handles={str(k): str(v) for k, v in dict(payload.get("status_handles", {})).items()},
            requester_encryption_key=str(payload.get("requester_encryption_key", "")),
            execution_policy=str(payload.get(
                "execution_policy", LEGACY_READY_SET_V1)),
        )


class DeploymentInstanceState(str, Enum):
    SELECTED = "SELECTED"
    VERIFYING = "VERIFYING"
    LOADING = "LOADING"
    WARMING = "WARMING"
    READY = "READY"
    ACTIVE = "ACTIVE"
    EXECUTING = "EXECUTING"
    COMPLETED = "COMPLETED"
    FAILED = "FAILED"
    EXPIRED = "EXPIRED"
    RELEASED = "RELEASED"


@dataclass(frozen=True)
class DeploymentInstance(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-deployment-instance-v2"
    instance_id: str
    plan_digest: str
    provider: str
    provider_boot_epoch: str
    role: str
    state: DeploymentInstanceState
    sequence: int
    expires_at_ms: int
    reason: str = ""

    def __post_init__(self) -> None:
        if (not self.instance_id or not self.plan_digest or not self.provider
                or not self.provider_boot_epoch or not self.role
                or self.sequence < 0 or self.expires_at_ms <= 0):
            raise ValueError("invalid DeploymentInstance")

    def transition(self, state: DeploymentInstanceState, *, reason: str = "") -> "DeploymentInstance":
        allowed = {
            DeploymentInstanceState.SELECTED: {DeploymentInstanceState.VERIFYING},
            DeploymentInstanceState.VERIFYING: {DeploymentInstanceState.LOADING},
            DeploymentInstanceState.LOADING: {DeploymentInstanceState.WARMING},
            DeploymentInstanceState.WARMING: {DeploymentInstanceState.READY},
            DeploymentInstanceState.READY: {DeploymentInstanceState.ACTIVE},
            DeploymentInstanceState.ACTIVE: {DeploymentInstanceState.EXECUTING},
            DeploymentInstanceState.EXECUTING: {DeploymentInstanceState.COMPLETED},
        }
        terminal = {DeploymentInstanceState.FAILED, DeploymentInstanceState.EXPIRED,
                    DeploymentInstanceState.RELEASED}
        if state not in allowed.get(self.state, set()) and state not in terminal:
            raise ValueError(f"invalid deployment transition {self.state.value}->{state.value}")
        return DeploymentInstance(self.instance_id, self.plan_digest, self.provider,
                                  self.provider_boot_epoch, self.role, state,
                                  self.sequence + 1, self.expires_at_ms, reason)

    @classmethod
    def from_bytes(cls, wire: bytes) -> "DeploymentInstance":
        payload = _decode(wire, cls.SCHEMA)
        return cls(
            instance_id=str(payload.get("instance_id", "")),
            plan_digest=str(payload.get("plan_digest", "")),
            provider=str(payload.get("provider", "")),
            provider_boot_epoch=str(payload.get("provider_boot_epoch", "")),
            role=str(payload.get("role", "")),
            state=DeploymentInstanceState(str(payload.get("state", ""))),
            sequence=int(payload.get("sequence", -1)),
            expires_at_ms=int(payload.get("expires_at_ms", 0)),
            reason=str(payload.get("reason", "")),
        )


@dataclass(frozen=True)
class ProviderReadyMessage(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-provider-ready-v1"
    request_id: str
    attempt: int
    selection_digest: str
    deployment_plan_digest: str
    provider: str
    provider_boot_epoch: str
    role: str
    artifact_digest: str
    operation_id: str
    sequence: int
    expires_at_ms: int
    signer: str
    signature: str

    def __post_init__(self) -> None:
        values = (self.request_id, self.selection_digest, self.deployment_plan_digest,
                  self.provider, self.provider_boot_epoch, self.role,
                  self.artifact_digest, self.operation_id, self.signer, self.signature)
        if not all(values) or self.attempt <= 0 or self.sequence <= 0 or self.expires_at_ms <= 0:
            raise ValueError("invalid ProviderReadyMessage")

    def membership_key(self) -> tuple[str, str]:
        return self.provider, self.role


@dataclass(frozen=True)
class ReadyAcknowledgement(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-ready-acknowledgement-v1"
    ready_message_digest: str
    accepted: bool
    reason: str
    requester_identity: str
    sequence: int
    signature: str

    def __post_init__(self) -> None:
        if (not self.ready_message_digest or not self.requester_identity
                or self.sequence <= 0 or not self.signature):
            raise ValueError("invalid ReadyAcknowledgement")


@dataclass(frozen=True)
class ReadySetMember(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-ready-set-member-v1"
    provider: str
    role: str
    provider_boot_epoch: str
    ready_message_digest: str

    def __post_init__(self) -> None:
        if not all((self.provider, self.role, self.provider_boot_epoch, self.ready_message_digest)):
            raise ValueError("invalid ReadySet member")


@dataclass(frozen=True)
class ExecutionActivateMessage(CanonicalContract):
    SCHEMA: ClassVar[str] = "ndnsf-di-execution-activate-v1"
    requester_identity: str
    request_id: str
    attempt: int
    selection_digest: str
    deployment_plan_digest: str
    members: tuple[ReadySetMember, ...]
    deadline_ms: int
    activation_sequence: int
    signature: str

    def __post_init__(self) -> None:
        required = (self.requester_identity, self.request_id, self.selection_digest,
                    self.deployment_plan_digest, self.signature)
        keys = [(item.provider, item.role) for item in self.members]
        if (not all(required) or self.attempt <= 0 or not self.members
                or self.deadline_ms <= 0 or self.activation_sequence <= 0
                or len(keys) != len(set(keys))):
            raise ValueError("invalid ExecutionActivateMessage")

    @classmethod
    def from_bytes(cls, wire: bytes) -> "ExecutionActivateMessage":
        payload = _decode(wire, cls.SCHEMA)
        return cls(
            requester_identity=str(payload.get("requester_identity", "")),
            request_id=str(payload.get("request_id", "")),
            attempt=int(payload.get("attempt", 0)),
            selection_digest=str(payload.get("selection_digest", "")),
            deployment_plan_digest=str(
                payload.get("deployment_plan_digest", "")),
            members=tuple(ReadySetMember(**item)
                          for item in payload.get("members", ())),
            deadline_ms=int(payload.get("deadline_ms", 0)),
            activation_sequence=int(payload.get("activation_sequence", 0)),
            signature=str(payload.get("signature", "")),
        )

    @property
    def ready_set_digest(self) -> str:
        return canonical_digest(tuple(sorted(
            (to_plain(item) for item in self.members),
            key=lambda item: (item["provider"], item["role"]))))


# Spec 168 deployment-fidelity evidence is deliberately workload-neutral.  It
# belongs in NDNSF-DI's canonical boundary rather than in the generic NDNSF
# collaboration protocol.
LIFECYCLE_EVENT_TYPES = frozenset({
    "REQUEST_CREATED", "REQUEST_PUBLISHED", "ACK_ACCEPTED", "ACK_REJECTED",
    "ACK_CLOSED", "GRAPH_INSPECTED", "PLAN_VALIDATED", "ARTIFACT_PUBLISHED",
    "PLAN_COMMITTED", "FINAL_SELECTION", "ROLE_ASSIGNED",
    "ARTIFACT_FETCHED", "VERIFIED_DISK", "HOST_RESIDENT", "GPU_RESIDENT",
    "LOCAL_READY", "DEPENDENCY_INPUT_ACCEPTED", "STAGE_EXECUTING",
    "STAGE_OUTPUT_PUBLISHED", "STAGE_COMPLETED", "TOKEN_RECORDED",
    "RESPONSE_PUBLISHED", "FAILURE_PUBLISHED", "CANCELED", "EXPIRED",
    "CLEANUP_RECORDED",
})

TERMINAL_LIFECYCLE_EVENT_TYPES = frozenset({
    "RESPONSE_PUBLISHED", "FAILURE_PUBLISHED", "CANCELED", "EXPIRED",
})

FAILURE_CODE_PREFIXES = (
    "ENV_", "BOOT_", "ROUTE_", "ACK_", "PLAN_", "REPO_PUBLISH_",
    "REPO_FETCH_", "PREP_", "DEPENDENCY_", "EXEC_", "TOKEN_",
    "RESPONSE_", "CLEANUP_", "ANALYZER_",
)


def validate_lifecycle_failure_code(value: str) -> None:
    """Reject ambiguous terminal labels such as ``TIMEOUT`` or ``ERROR``."""
    if value == "UNRESOLVED_EVIDENCE_GAP":
        return
    if (not isinstance(value, str) or not value
            or not value.replace("_", "").isalnum()
            or value != value.upper()
            or not any(value.startswith(prefix)
                       and len(value) > len(prefix)
                       for prefix in FAILURE_CODE_PREFIXES)):
        raise ValueError("failure_code must identify an exact lifecycle boundary")


@dataclass(frozen=True)
class LifecycleEventV1(CanonicalContract):
    """One immutable, fully bound NDNSF-DI lifecycle observation."""

    SCHEMA: ClassVar[str] = "ndnsf-di.lifecycle-event.v1"
    experiment_id: str
    request_id: str
    attempt_epoch: int
    event_id: str
    event_type: str
    component: str
    provider: str | None
    provider_boot_epoch: str | None
    role: str | None
    plan_digest: str | None
    operation_id: str | None
    epoch: int
    sequence: int
    monotonic_ns: int
    wall_time_utc: str
    authenticated: bool
    details_schema: str
    details: dict[str, Any]

    def __post_init__(self) -> None:
        if not all((self.experiment_id, self.request_id, self.event_id,
                    self.component, self.wall_time_utc, self.details_schema)):
            raise ValueError("lifecycle event identities are required")
        if self.attempt_epoch <= 0 or self.epoch < 0 or self.sequence <= 0:
            raise ValueError("lifecycle event epochs and sequence are invalid")
        if self.monotonic_ns < 0:
            raise ValueError("lifecycle event monotonic_ns is invalid")
        if self.event_type not in LIFECYCLE_EVENT_TYPES:
            raise ValueError("unsupported lifecycle event_type")
        if type(self.authenticated) is not bool or not isinstance(self.details, dict):
            raise ValueError("invalid lifecycle authentication/details")
        scoped = (self.provider, self.provider_boot_epoch, self.role)
        if any(value is not None for value in scoped) and not all(scoped):
            raise ValueError("provider, boot epoch and role form one binding")
        if self.plan_digest is not None:
            _require_sha256(self.plan_digest, "plan_digest")
        if self.operation_id is None and self.epoch != 0:
            raise ValueError("operation epoch requires operation_id")
        if self.event_type == "FAILURE_PUBLISHED":
            validate_lifecycle_failure_code(
                str(self.details.get("failure_code", "")))

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "LifecycleEventV1":
        expected = {item.name for item in dataclass_fields(cls)}
        if set(payload) != expected | {"schema"} or payload.get("schema") != cls.SCHEMA:
            raise ValueError("lifecycle event contains unknown or missing fields")
        return cls(**{name: payload[name] for name in expected})

    @classmethod
    def from_bytes(cls, wire: bytes) -> "LifecycleEventV1":
        payload = _decode(wire, cls.SCHEMA)
        if bytes(wire) != canonical_json(payload).encode():
            raise ValueError("lifecycle event is not canonically encoded")
        return cls.from_dict(payload)


@dataclass(frozen=True)
class InvocationSummaryV1(CanonicalContract):
    """Exactly one terminal analysis row for one frozen schedule row."""

    SCHEMA: ClassVar[str] = "ndnsf-di.invocation-summary.v1"
    experiment_id: str
    schedule_row: int
    prompt_id: str
    request_id: str
    attempt_epoch: int
    model_identity: str
    plan_digest: str
    assignment_digest: str
    cache_class: str
    terminal_kind: str
    terminal_reason: str
    accepted: bool
    answer: str
    answer_digest: str
    token_count: int
    request_to_ack_close_ms: float
    planning_ms: float
    publication_ms: float
    artifact_fetch_ms: float
    disk_to_ram_ms: float
    ram_to_gpu_ms: float
    dependency_wait_ms: float
    execution_ms: float
    response_ms: float
    total_ms: float
    ttft_ms: float | None
    inter_token_latency_ms: tuple[float, ...]
    tokens_per_second: float
    repo_unique_bytes: int
    repo_wire_bytes: int
    duplicate_model_payload_bytes: int
    device_load_count: int
    cpu_fallback_count: int
    security_verdict: str
    failure_class: str
    failure_code: str
    evidence_path: str

    def __post_init__(self) -> None:
        if not all((self.experiment_id, self.prompt_id, self.request_id,
                    self.model_identity, self.terminal_kind,
                    self.terminal_reason, self.security_verdict,
                    self.evidence_path)):
            raise ValueError("invocation summary identities are required")
        if self.schedule_row <= 0 or self.attempt_epoch <= 0:
            raise ValueError("invocation summary row/attempt is invalid")
        if self.cache_class not in {"cold", "disk", "ram", "gpu", "mixed", "invalid"}:
            raise ValueError("invalid cache_class")
        if self.terminal_kind not in {
                "RESPONSE", "FAILURE", "CANCELED", "EXPIRED",
                "SCHEDULER_NOT_ADMITTED", "ENVIRONMENTAL_FAILURE"}:
            raise ValueError("invalid terminal_kind")
        if type(self.accepted) is not bool:
            raise ValueError("accepted must be boolean")
        if self.plan_digest:
            _require_sha256(self.plan_digest, "plan_digest")
        if self.assignment_digest:
            _require_sha256(self.assignment_digest, "assignment_digest")
        if self.answer_digest:
            _require_sha256(self.answer_digest, "answer_digest")
        numeric = (
            self.request_to_ack_close_ms, self.planning_ms,
            self.publication_ms, self.artifact_fetch_ms, self.disk_to_ram_ms,
            self.ram_to_gpu_ms, self.dependency_wait_ms, self.execution_ms,
            self.response_ms, self.total_ms, self.tokens_per_second,
            self.repo_unique_bytes, self.repo_wire_bytes,
            self.duplicate_model_payload_bytes, self.device_load_count,
            self.cpu_fallback_count, self.token_count,
        )
        if any(value < 0 for value in numeric):
            raise ValueError("invocation summary metrics must be nonnegative")
        if self.ttft_ms is not None and self.ttft_ms < 0:
            raise ValueError("ttft_ms must be nonnegative or null")
        if any(value < 0 for value in self.inter_token_latency_ms):
            raise ValueError("inter-token latency must be nonnegative")
        if self.terminal_kind == "RESPONSE":
            if not self.answer_digest or self.failure_code:
                raise ValueError("successful response summary is inconsistent")
        else:
            validate_lifecycle_failure_code(self.failure_code)

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "InvocationSummaryV1":
        expected = {item.name for item in dataclass_fields(cls)}
        if set(payload) != expected | {"schema"} or payload.get("schema") != cls.SCHEMA:
            raise ValueError("invocation summary contains unknown or missing fields")
        values = {name: payload[name] for name in expected}
        values["inter_token_latency_ms"] = tuple(
            float(item) for item in values["inter_token_latency_ms"])
        return cls(**values)

    @classmethod
    def from_bytes(cls, wire: bytes) -> "InvocationSummaryV1":
        payload = _decode(wire, cls.SCHEMA)
        if bytes(wire) != canonical_json(payload).encode():
            raise ValueError("invocation summary is not canonically encoded")
        return cls.from_dict(payload)

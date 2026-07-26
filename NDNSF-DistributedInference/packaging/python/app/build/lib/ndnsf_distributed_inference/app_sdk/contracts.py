"""Application-owned deployment and durable invocation value contracts."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from datetime import datetime, timedelta, timezone
from enum import Enum
import hashlib
import json
import warnings
from typing import Any, Mapping, Optional, Tuple, Union


def _stable(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def _digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(_stable(value)).hexdigest()


def _reject_secrets(value: Any) -> None:
    forbidden = {"secret", "password", "token", "privatekey", "credential"}
    if isinstance(value, Mapping):
        for key, item in value.items():
            if str(key).replace("_", "").lower() in forbidden:
                raise ValueError("deployment definition contains secret material")
            _reject_secrets(item)
    elif isinstance(value, (list, tuple)):
        for item in value: _reject_secrets(item)


@dataclass(frozen=True)
class ArtifactReference:
    uri: str
    digest: str
    size_bytes: int
    mount_path: str = ""

    def __post_init__(self):
        if (not self.uri or not self.digest.startswith("sha256:")
                or self.size_bytes < 0 or any(x in self.mount_path.split("/") for x in ("..",))):
            raise ValueError("invalid external artifact reference")


@dataclass(frozen=True)
class ModelIntent:
    allowed: Tuple[str, ...]

    def __post_init__(self):
        if not self.allowed or any(not str(item) for item in self.allowed):
            raise ValueError("model intent requires bounded alternatives")


@dataclass(frozen=True)
class RequestContract:
    input_schema: str
    output_schema: str
    continuous_output: bool = False

    def __post_init__(self):
        if not self.input_schema or not self.output_schema:
            raise ValueError("request contract requires input and output schemas")


@dataclass(frozen=True)
class OptimizationObjective:
    primary: str
    unit: str = ""
    target: float | None = None

    def __post_init__(self):
        if not self.primary:
            raise ValueError("optimization objective requires a primary metric")


@dataclass(frozen=True)
class DeploymentConstraints:
    minimum_providers: int = 1
    allowed_partition_kinds: Tuple[str, ...] = ()
    maximum_latency_ms: int = 0
    privacy_scope: str = ""

    def __post_init__(self):
        if self.minimum_providers <= 0 or self.maximum_latency_ms < 0:
            raise ValueError("invalid deployment constraints")


@dataclass(frozen=True)
class DeploymentDefinition:
    # The first fields retain the Spec 111 compatibility constructor. Preferred
    # callers use InferenceApplication.define(), which fills and signs typed
    # intent instead of placing controlling values in configuration.
    deployment_id: str
    model_id: str = ""
    artifacts: Tuple[ArtifactReference, ...] = ()
    roles: Tuple[str, ...] = ()
    configuration: Mapping[str, Any] = field(default_factory=dict)
    schema_version: str = "ndnsf-di-deployment-definition-v1"
    application_identity: str = ""
    deployment_owner: str = ""
    coordinator_service: str = ""
    service: str = ""
    model_intent: ModelIntent | None = None
    request_contract: RequestContract | None = None
    objective: OptimizationObjective | None = None
    constraints: DeploymentConstraints | None = None
    optimization_profile: str = ""
    metadata: Mapping[str, Any] = field(default_factory=dict)
    created_at: str = ""
    expires_at: str = ""
    previous_revision: str = ""
    signer_key_id: str = ""
    signer_public_key: str = ""
    signature: str = ""

    def __post_init__(self):
        if not self.deployment_id or "/" in self.deployment_id:
            raise ValueError("invalid deployment definition identity")
        if not self.model_id and self.model_intent is None:
            raise ValueError("deployment definition requires model intent")
        if not self.roles and not self.application_identity:
            raise ValueError("legacy deployment definition requires roles")
        if self.previous_revision and not self.previous_revision.startswith("sha256:"):
            raise ValueError("previous deployment revision must be a digest")
        _reject_secrets(self.configuration)
        _reject_secrets(self.metadata)

    @property
    def signed(self) -> bool:
        return bool(self.application_identity and self.signer_key_id and
                    self.signer_public_key and self.signature)

    def canonical_dict(self) -> dict[str, Any]:
        value = asdict(self)
        value.pop("signature", None)
        return value

    def to_dict(self): return asdict(self)
    def digest(self): return _digest(self.canonical_dict())

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "DeploymentDefinition":
        value = dict(payload)
        value["artifacts"] = tuple(
            item if isinstance(item, ArtifactReference) else ArtifactReference(**item)
            for item in value.get("artifacts", ()))
        value["roles"] = tuple(value.get("roles", ()))
        for key, owner in (
            ("model_intent", ModelIntent),
            ("request_contract", RequestContract),
            ("objective", OptimizationObjective),
            ("constraints", DeploymentConstraints),
        ):
            item = value.get(key)
            if isinstance(item, Mapping):
                if key == "model_intent":
                    item = {**item, "allowed": tuple(item.get("allowed", ())) }
                if key == "constraints":
                    item = {**item, "allowed_partition_kinds": tuple(
                        item.get("allowed_partition_kinds", ())) }
                value[key] = owner(**item)
        return cls(**value)


@dataclass(frozen=True)
class DeploymentDefinitionRef:
    application_identity: str
    deployment_owner: str
    coordinator_service: str
    deployment_id: str
    service: str
    record_name: str
    definition_digest: str
    expires_at: str
    signer_key_id: str
    schema_version: str = "ndnsf-di-definition-ref-v1"

    def __post_init__(self):
        if (not self.application_identity or not self.deployment_owner or
                not self.coordinator_service or not self.deployment_id or
                not self.record_name or
                not self.definition_digest.startswith("sha256:")):
            raise ValueError("invalid signed deployment definition reference")


@dataclass(frozen=True)
class DeploymentActivationRecord:
    application_identity: str
    deployment_owner: str
    deployment_id: str
    revision: str
    service: str
    definition_digest: str
    revision_digest: str
    activation_certificate_digest: str
    lifecycle_epoch: int
    activated_at: str
    expires_at: str
    signer_key_id: str
    signature: str
    state: str = "ACTIVE"
    record_name: str = ""
    supersedes: str = ""
    revoked: bool = False
    schema_version: str = "ndnsf-di-activation-v1"

    def __post_init__(self):
        if (self.state not in {"ACTIVE", "REVOKED"} or
                (self.state == "ACTIVE" and self.revoked) or
                (self.state == "REVOKED" and not self.revoked) or
                self.lifecycle_epoch <= 0 or
                any(not value.startswith("sha256:") for value in (
                    self.definition_digest, self.revision_digest,
                    self.activation_certificate_digest))):
            raise ValueError("invalid deployment activation record")

    def canonical_dict(self) -> dict[str, Any]:
        value = asdict(self)
        value.pop("signature", None)
        return value

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    def digest(self) -> str:
        return _digest(self.canonical_dict())

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "DeploymentActivationRecord":
        return cls(**dict(payload))


@dataclass(frozen=True)
class DeploymentHandleRef:
    deployment_id: str
    revision: str
    lifecycle_epoch: int
    owner_identity: str
    journal_locator: str
    journal_digest: str
    schema_version: str = "ndnsf-di-deployment-handle-v1"

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))

    @classmethod
    def from_json(cls, value: str) -> "DeploymentHandleRef":
        return cls(**json.loads(value))


@dataclass(frozen=True)
class DeploymentRef:
    deployment_id: str
    revision: str
    service: str
    definition_digest: str
    activation_certificate_digest: str
    activation_record_name: str
    activation_record_digest: str
    lifecycle_epoch: int
    application_identity: str = ""
    deployment_owner: str = ""
    coordinator_service: str = ""
    definition_record_name: str = ""
    expires_at: str = ""

    def __post_init__(self):
        if self.lifecycle_epoch <= 0 or any(not value.startswith("sha256:") for value in (
                self.revision, self.definition_digest,
                self.activation_certificate_digest,
                self.activation_record_digest)):
            raise ValueError("invalid active deployment reference")

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))

    @classmethod
    def from_json(cls, value: str) -> "DeploymentRef":
        return cls(**json.loads(value))


class DeploymentAvailability(str, Enum):
    READY = "READY"
    NEEDS_PREPARATION = "NEEDS_PREPARATION"
    UNAVAILABLE = "UNAVAILABLE"


@dataclass(frozen=True)
class ProviderDeploymentOffer:
    role: str
    availability: DeploymentAvailability | str
    definition_digest: str = ""
    revision_digest: str = ""
    artifact_digests: Tuple[str, ...] = ()
    adapter_identity: str = ""
    boot_epoch: str = ""
    capability_digest: str = ""
    capacity: int = 0
    lease_offer: Mapping[str, Any] = field(default_factory=dict)

    def __post_init__(self):
        object.__setattr__(self, "availability",
                           DeploymentAvailability(self.availability))
        if not self.role or self.capacity < 0:
            raise ValueError("invalid Provider deployment offer")
        if self.availability is DeploymentAvailability.READY:
            if (not self.revision_digest.startswith("sha256:") or
                    not self.artifact_digests or not self.adapter_identity or
                    not self.boot_epoch):
                raise ValueError("READY offer requires exact advisory binding")


@dataclass(frozen=True)
class ProviderDeploymentOffers:
    request_id: str
    attempt: int
    provider: str
    observed_at: datetime
    expires_at: datetime
    offers: Tuple[ProviderDeploymentOffer, ...]
    signature: str = ""

    def __post_init__(self):
        if (not self.request_id or not self.provider or self.attempt <= 0 or
                self.observed_at.tzinfo is None or self.expires_at.tzinfo is None or
                self.expires_at <= self.observed_at or not self.offers or
                len({item.role for item in self.offers}) != len(self.offers)):
            raise ValueError("invalid multi-role Provider deployment offers")

    @property
    def all_roles_certified_ready(self) -> bool:
        return False

    def to_wire(self) -> bytes:
        value = asdict(self)
        value["observed_at"] = self.observed_at.isoformat()
        value["expires_at"] = self.expires_at.isoformat()
        for offer in value["offers"]:
            offer["availability"] = offer["availability"].value
        return _stable(value)

    @classmethod
    def from_wire(cls, wire: bytes) -> "ProviderDeploymentOffers":
        value = json.loads(bytes(wire).decode("utf-8"))
        value["observed_at"] = datetime.fromisoformat(value["observed_at"])
        value["expires_at"] = datetime.fromisoformat(value["expires_at"])
        value["offers"] = tuple(ProviderDeploymentOffer(
            **{**item, "artifact_digests": tuple(item.get("artifact_digests", ()))})
            for item in value["offers"])
        return cls(**value)


class DeploymentProgressPhase(str, Enum):
    ACCEPTED = "ACCEPTED"
    FETCHING = "FETCHING"
    VERIFYING = "VERIFYING"
    LOADING = "LOADING"
    WARMING = "WARMING"
    READY = "READY"
    FAILED = "FAILED"
    CANCELLED = "CANCELLED"
    EXPIRED = "EXPIRED"


@dataclass(frozen=True)
class DeploymentProgress:
    request_id: str
    attempt: int
    revision: str
    role: str
    provider: str
    operation_id: str
    phase: DeploymentProgressPhase | str
    sequence: int
    progress: float
    reason: str = ""

    def __post_init__(self):
        object.__setattr__(self, "phase", DeploymentProgressPhase(self.phase))
        if self.attempt <= 0 or self.sequence <= 0 or not 0 <= self.progress <= 1:
            raise ValueError("invalid deployment progress")

    def to_dict(self) -> dict[str, Any]:
        value = asdict(self)
        value["phase"] = self.phase.value
        return value

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "DeploymentProgress":
        return cls(**dict(value))


@dataclass(frozen=True)
class DeploymentStatus:
    state: str
    revision: str
    roles: Tuple[DeploymentProgress, ...] = ()
    readiness_certificate_digest: str = ""
    coordinator_epoch: int = 1
    reason: str = ""

    def __post_init__(self):
        if self.coordinator_epoch <= 0 or not self.revision.startswith("sha256:"):
            raise ValueError("invalid deployment status")
        keys = {(item.role, item.provider) for item in self.roles}
        if len(keys) != len(self.roles):
            raise ValueError("duplicate deployment role status")
        if self.state == "READY" and (
                not self.roles or any(item.phase is not DeploymentProgressPhase.READY
                                      for item in self.roles) or
                not self.readiness_certificate_digest.startswith("sha256:")):
            raise ValueError("READY deployment requires exact all-role readiness")
        if self.state == "ACTIVE" and not self.readiness_certificate_digest.startswith(
                "sha256:"):
            raise ValueError("ACTIVE deployment requires an activation certificate")

    def to_wire(self) -> bytes:
        return _stable({
            **asdict(self),
            "roles": [item.to_dict() for item in self.roles],
        })

    @classmethod
    def from_wire(cls, value: bytes) -> "DeploymentStatus":
        payload = json.loads(bytes(value).decode("utf-8"))
        payload["roles"] = tuple(DeploymentProgress.from_dict(item)
                                 for item in payload.get("roles", ()))
        return cls(**payload)


@dataclass(frozen=True)
class DeploymentSummary:
    deployment: Union[DeploymentDefinitionRef, DeploymentRef]
    state: str
    service: str
    expires_at: str


@dataclass(frozen=True)
class RequestTiming:
    timeout: timedelta | None = None
    deadline: datetime | None = None

    def __post_init__(self):
        if (self.timeout is None) == (self.deadline is None):
            raise ValueError("provide exactly one timeout or deadline")
        if self.timeout is not None:
            if not isinstance(self.timeout, timedelta):
                raise TypeError("timeout must be datetime.timedelta")
            if self.timeout.total_seconds() <= 0:
                raise ValueError("timeout must be positive")
        if self.deadline is not None:
            if not isinstance(self.deadline, datetime):
                raise TypeError("deadline must be datetime.datetime")
            if self.deadline.tzinfo is None or self.deadline.utcoffset() is None:
                raise ValueError("deadline must be timezone-aware")
            if self.deadline <= datetime.now(timezone.utc):
                raise ValueError("deadline must be in the future")

    def deadline_utc(self, now: datetime | None = None) -> datetime:
        if self.deadline is not None:
            return self.deadline.astimezone(timezone.utc)
        current = now or datetime.now(timezone.utc)
        return current + self.timeout


@dataclass(frozen=True)
class InferenceOptions:
    metadata: Mapping[str, Any] = field(default_factory=dict)
    output_encoding: str = "application/octet-stream"

    def __post_init__(self):
        _reject_secrets(self.metadata)


@dataclass(frozen=True)
class DeploymentPlan:
    deployment_id: str
    plan_digest: str
    definition_digest: str
    lifecycle_epoch: int
    definition: DeploymentDefinition

    def __post_init__(self):
        if (self.deployment_id != self.definition.deployment_id
                or not self.plan_digest.startswith("sha256:")
                or self.definition_digest != self.definition.digest()
                or self.lifecycle_epoch <= 0):
            raise ValueError("invalid immutable DeploymentPlan")

    @classmethod
    def resolve(cls, definition: DeploymentDefinition, epoch: int = 1):
        digest = definition.digest()
        plan_digest = _digest({"definition": digest, "epoch": epoch})
        return cls(definition.deployment_id, plan_digest, digest, epoch, definition)

    @property
    def revision(self) -> str:
        """Deprecated read-only spelling for old external serialized callers."""
        warnings.warn("DeploymentPlan.revision is deprecated; use plan_digest",
                      DeprecationWarning, stacklevel=2)
        return self.plan_digest


class DeploymentRevision:
    """Deprecated read/import shim; maintained runtime code must use DeploymentPlan."""

    @classmethod
    def resolve(cls, definition: DeploymentDefinition, epoch: int = 1):
        warnings.warn("DeploymentRevision is deprecated; use DeploymentPlan",
                      DeprecationWarning, stacklevel=2)
        return DeploymentPlan.resolve(definition, epoch)


@dataclass(frozen=True)
class DeploymentOperationHandle:
    operation_id: str
    deployment_id: str
    revision: str
    action: str
    status: str
    reason: str = ""
    lifecycle_epoch: int = 0
    idempotency_digest: str = ""
    event_cursor: str = ""
    retryable: bool = False


# Source-compatible name retained for the already published APP SDK surface;
# the value is now the durable, reopenable operation handle required by the
# lifecycle contract.
DeploymentOperation = DeploymentOperationHandle


@dataclass(frozen=True)
class RequestEnvelopeReference:
    requester_identity: str
    request_id: str
    locator: str
    wire_digest: str
    security_context: str
    expires_at_ms: int
    retention_owner: str
    cleanup_state: str = "RETAINED"
    repository_id: str = ""


@dataclass(frozen=True)
class ResultRendezvousRecord:
    requester_identity: str
    request_id: str
    attempt_epoch: int
    activation_digest: str
    output_epoch: int
    terminal_digest: str
    protected_wire_digest: str
    locator: str
    terminal_state: str
    expires_at_ms: int
    provider_result_data_name: str = ""
    signer_certificate: str = ""
    network_wire_digest: str = ""


@dataclass(frozen=True)
class InferenceRequestHandle:
    request_id: str
    deployment_id: str
    revision: str
    created_at_ms: int
    expires_at_ms: int
    envelope_digest: str
    requester_identity: str = ""
    attempt_epoch: int = 1
    envelope_reference: Optional[RequestEnvelopeReference] = None
    intent_digest: str = ""
    certificate_digest: str = ""
    result_rendezvous_digest: str = ""
    event_cursor: int = 0
    cancellation_id: str = ""
    cancellation_reason: str = ""
    terminal_evidence_digest: str = ""
    service_name: str = ""
    activation_digest: str = ""
    execution_activation_wire: str = ""
    # Deprecated v1 import-only fields. New writers leave both empty.
    execution_certificate_wire: str = ""

    def to_record(self) -> dict[str, Any]:
        return asdict(self)

    def to_json(self) -> str:
        return json.dumps(self.to_record(), sort_keys=True, separators=(",", ":"))

    @classmethod
    def from_record(cls, payload: Mapping[str, Any]):
        value = dict(payload)
        if (not value.get("activation_digest") and
                value.get("certificate_digest")):
            value["activation_digest"] = value["certificate_digest"]
        reference = value.get("envelope_reference")
        if isinstance(reference, Mapping):
            value["envelope_reference"] = RequestEnvelopeReference(**reference)
        return cls(**value)

    @classmethod
    def from_json(cls, value: str) -> "InferenceRequestHandle":
        return cls.from_record(json.loads(value))


# Bounded source-compatible spelling; the canonical public contract is the
# explicit durable identity above.
RequestHandle = InferenceRequestHandle


@dataclass(frozen=True)
class RuntimeAllocationHandoff:
    candidate_digest: str
    offline_gate_digest: str
    deployment_revision: str
    oci_digest: str
    sif_digest: str
    model_artifact_digests: Tuple[str, ...]
    roles: Tuple[str, ...]
    process_map_digest: str
    network_profile_digest: str
    identity_state_digest: str
    authorization_digest: str

    def __post_init__(self):
        digests = (
            self.candidate_digest, self.offline_gate_digest,
            self.deployment_revision, self.oci_digest, self.sif_digest,
            *self.model_artifact_digests, self.process_map_digest,
            self.network_profile_digest, self.identity_state_digest,
            self.authorization_digest,
        )
        if (not self.roles or len(set(self.roles)) != len(self.roles)
                or any(not item.startswith("sha256:") for item in digests)):
            raise ValueError("runtime allocation handoff requires immutable digests")

    def to_dict(self): return asdict(self)
    def digest(self): return _digest(self.to_dict())


@dataclass(frozen=True)
class InfrastructureAllocationHandle:
    adapter: str
    handoff_digest: str
    scheduler_job_id: str
    requested_resources: Mapping[str, Any]
    observed_resources: Mapping[str, Any]
    scheduler_state: str
    scheduler_reason: str = ""
    event_cursor: str = ""

    def __post_init__(self):
        if (not self.adapter or not self.scheduler_job_id
                or not self.handoff_digest.startswith("sha256:")
                or self.scheduler_state in {"READY", "ACTIVE", "SUCCEEDED"}):
            raise ValueError("invalid infrastructure allocation handle")


RequestRef = InferenceRequestHandle
RequestableDeployment = Union[
    DeploymentDefinition, DeploymentDefinitionRef, "DeploymentHandle",
    DeploymentRef,
]


__all__ = [
    "ArtifactReference", "DeploymentActivationRecord", "DeploymentAvailability",
    "DeploymentConstraints", "DeploymentDefinition", "DeploymentDefinitionRef",
    "DeploymentHandleRef", "DeploymentOperation", "DeploymentOperationHandle",
    "DeploymentProgress", "DeploymentProgressPhase", "DeploymentRef",
    "DeploymentPlan", "DeploymentRevision", "DeploymentStatus", "DeploymentSummary",
    "InferenceOptions", "InferenceRequestHandle", "InfrastructureAllocationHandle", "ModelIntent",
    "OptimizationObjective", "ProviderDeploymentOffer", "ProviderDeploymentOffers",
    "RequestContract", "RequestEnvelopeReference", "RequestHandle", "RequestRef",
    "RequestTiming", "RequestableDeployment", "ResultRendezvousRecord",
    "RuntimeAllocationHandoff",
]

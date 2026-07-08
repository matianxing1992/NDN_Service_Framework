"""Python-facing NDNSF service API backed by a pybind11 extension.

Python application code defines request handlers and issues service requests in
Python. The NDNSF runtime itself stays in C++ through ``ndnsf._ndnsf``: Face,
SVS, NAC-ABE, signing, token checks, and worker threads are managed by the
framework rather than by Python.
"""

from __future__ import annotations

import base64
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import tempfile
import threading
from typing import Any, Callable, Optional

from . import _ndnsf
from .runtime_telemetry import (
    ProviderCapabilityHint,
    ServiceOperationState,
    ServiceOperationStatus,
    parse_ack_metadata,
    to_plain,
)
from .service_discovery import ServiceDiscoveryRecord

NEGATIVE_ACK_REASON_QUEUE_FULL = "QUEUE_FULL"
NEGATIVE_ACK_REASON_PROVIDER_BUSY = "PROVIDER_BUSY"
NEGATIVE_ACK_REASON_GPU_BUSY = "GPU_BUSY"
NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE = "MODEL_UNAVAILABLE"
NEGATIVE_ACK_REASON_PERMISSION_DENIED = "PERMISSION_DENIED"
NEGATIVE_ACK_REASON_UNSUPPORTED_REQUEST = "UNSUPPORTED_REQUEST"
NEGATIVE_ACK_REASON_INTERNAL_ERROR = "INTERNAL_ERROR"

RECOMMENDED_NEGATIVE_ACK_REASONS = frozenset({
    NEGATIVE_ACK_REASON_QUEUE_FULL,
    NEGATIVE_ACK_REASON_PROVIDER_BUSY,
    NEGATIVE_ACK_REASON_GPU_BUSY,
    NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
    NEGATIVE_ACK_REASON_PERMISSION_DENIED,
    NEGATIVE_ACK_REASON_UNSUPPORTED_REQUEST,
    NEGATIVE_ACK_REASON_INTERNAL_ERROR,
})


def default_large_data_interest_lifetime_ms() -> int:
    """InterestLifetime for segmented large-object fetches.

    DI dependency prefetch can intentionally issue Interests before upstream
    providers publish the corresponding Data. Keep this long enough for normal
    distributed inference runs so the Interest stays pending instead of being
    re-expressed every second.
    """

    return max(50, int(os.environ.get("NDNSF_LARGE_DATA_INTEREST_LIFETIME_MS", "10000")))


@dataclass(frozen=True)
class ServiceResponse:
    status: bool
    payload: bytes = b""
    error: str = ""


@dataclass(frozen=True)
class AckDecision:
    status: bool = True
    payload: bytes = b""
    message: str = "ok"
    suppress: bool = False


@dataclass(frozen=True)
class AckCandidate:
    provider_name: str
    service_name: str
    request_id: str
    status: bool
    message: str = ""
    payload: bytes = b""
    telemetry: Optional[dict[str, Any]] = None


def _deployment_roles_from_ack_candidate(candidate: AckCandidate) -> list[str]:
    """Return deployment roles represented by a ready or provisioning ACK.

    Ready providers use the core ProviderCapabilityHint/ServiceDiscoveryRecord
    path when present. Provisioning is intentionally narrower: a negative ACK is
    recorded only when it explicitly says the model is unavailable and therefore
    needs provisioning.
    """

    fields = parse_ack_metadata(bytes(candidate.payload))

    def _roles_from(value: Any) -> list[str]:
        if value is None:
            return []
        if isinstance(value, str):
            return [item.strip() for item in value.split(",") if item.strip()]
        if isinstance(value, (list, tuple)):
            return [str(item).strip() for item in value if str(item).strip()]
        return [str(value).strip()] if str(value).strip() else []

    if candidate.status:
        capability_payload = fields.get("providerCapabilityHint")
        if isinstance(capability_payload, dict):
            try:
                hint = ProviderCapabilityHint.from_dict(capability_payload)
                record = ServiceDiscoveryRecord.from_provider_capability_hint(hint)
                if not record.ready_for_new_request():
                    return []
            except Exception:
                return []
        return _roles_from(fields.get("roles"))

    reason = str(fields.get("negativeAckReason", candidate.message)).strip()
    normalized_reason = reason.replace("_", "").replace("-", "").upper()
    if normalized_reason != "MODELUNAVAILABLE":
        return []
    roles = _roles_from(fields.get("provisioningRole"))
    roles.extend(role for role in _roles_from(fields.get("roles")) if role not in roles)
    return roles


_DEPLOYMENT_STATUS_PRIORITY = {
    "ACTIVE": 0,
    "IDLE": 1,
    "DEGRADED": 2,
    "DISK_RESIDENT": 3,
    "PROVISIONING": 4,
    "EVICTED": 5,
    "REJECTED": 6,
    "NOT_FOUND": 7,
}


def _deployment_operation_state(status: str) -> ServiceOperationState:
    normalized = str(status or "").upper()
    if normalized == "PROVISIONING":
        return ServiceOperationState.RUNNING
    if normalized in {"REJECTED", "NOT_FOUND"}:
        return ServiceOperationState.FAILED
    if normalized == "EVICTED":
        return ServiceOperationState.CANCELED
    if normalized == "DEGRADED":
        return ServiceOperationState.WAITING_INPUT
    return ServiceOperationState.DONE


def _deployment_operation_progress(status: str) -> float:
    normalized = str(status or "").upper()
    if normalized == "PROVISIONING":
        return 0.5
    if normalized in {"REJECTED", "NOT_FOUND"}:
        return 0.0
    if normalized == "DEGRADED":
        return 0.75
    return 1.0


def _deployment_operation_status(deployment: dict[str, Any],
                                 *,
                                 operation: str = "DEPLOYMENT") -> dict[str, Any]:
    existing = deployment.get("operationStatus", deployment.get("operation_status"))
    if isinstance(existing, dict):
        try:
            return to_plain(ServiceOperationStatus.from_dict(existing))
        except Exception:
            pass
    status = str(deployment.get("status", "")).upper()
    deployment_id = str(deployment.get("deploymentId", deployment.get("deployment_id", "")))
    service_name = str(deployment.get("serviceName", deployment.get("service_name", "")))
    reason = str(deployment.get("reason", ""))
    op_status = ServiceOperationStatus(
        operation_id=deployment_id or operation.lower(),
        operation=operation,
        service_name=service_name,
        state=_deployment_operation_state(status),
        reason_code=status if status in {"REJECTED", "NOT_FOUND"} else "",
        message=reason or status.lower(),
        progress=_deployment_operation_progress(status),
        updated_at_ms=int(deployment.get("updatedAtMs", deployment.get("updated_at_ms", 0)) or 0),
        metadata={
            "deploymentStatus": status,
            "planId": deployment.get("planId", deployment.get("plan_id", "")),
            "refCount": deployment.get("refCount", deployment.get("ref_count", 0)),
        },
    )
    return to_plain(op_status)


def _with_deployment_operation_status(deployment: dict[str, Any],
                                      *,
                                      operation: str = "DEPLOYMENT") -> dict[str, Any]:
    result = dict(deployment)
    result["operationStatus"] = _deployment_operation_status(result, operation=operation)
    return result


def _deployment_sort_key(deployment: dict[str, Any]) -> tuple[int, str]:
    status = ""
    op_payload = deployment.get("operationStatus", deployment.get("operation_status"))
    if isinstance(op_payload, dict):
        try:
            op_status = ServiceOperationStatus.from_dict(op_payload)
            status = str(op_status.metadata.get("deploymentStatus", "")).upper()
            if not status:
                state_priority = {
                    ServiceOperationState.DONE: 0,
                    ServiceOperationState.WAITING_INPUT: 2,
                    ServiceOperationState.RUNNING: 4,
                    ServiceOperationState.CANCELED: 5,
                    ServiceOperationState.FAILED: 6,
                    ServiceOperationState.EXPIRED: 6,
                }
                return (state_priority.get(op_status.state, 99),
                        str(deployment.get("deploymentId", "")))
        except Exception:
            status = ""
    if not status:
        status = str(deployment.get("status", "")).upper()
    return (_DEPLOYMENT_STATUS_PRIORITY.get(status, 99),
            str(deployment.get("deploymentId", "")))


@dataclass(frozen=True)
class AllowedService:
    """A service permission entry visible to a Python NDNSF user.

    provider_service is the full permission namespace, typically
    /<provider>/<service>. service is the unified service name applications pass
    to request_service(), such as /HELLO. token is retained only for legacy
    compatibility; current dynamic-runtime permissions normally leave it empty.
    """

    provider_service: str
    service: str
    token: str = ""


@dataclass(frozen=True)
class LargeDataPublishResult:
    success: bool
    encrypted_data_name: str = ""
    object_id: str = ""
    error: str = ""


@dataclass(frozen=True)
class LargeDataReference:
    data_name: str
    object_type: str = ""
    object_id: str = ""
    plaintext_size: int = 0
    encrypted: bool = True
    digest: str = ""


def encode_large_data_reference_payload(reference: LargeDataReference) -> bytes:
    """Encode a standard NDNSF large-data reference payload."""

    return bytes(_ndnsf.encode_large_data_reference_payload(
        reference.data_name,
        reference.object_type,
        reference.object_id,
        int(reference.plaintext_size),
        bool(reference.encrypted),
        reference.digest,
    ))


def parse_large_data_reference_payload(payload: bytes) -> Optional[LargeDataReference]:
    """Parse a standard NDNSF large-data reference payload, or return None."""

    parsed = _ndnsf.parse_large_data_reference_payload(bytes(payload))
    if parsed is None:
        return None
    return LargeDataReference(
        data_name=str(parsed.get("data_name", "")),
        object_type=str(parsed.get("object_type", "")),
        object_id=str(parsed.get("object_id", "")),
        plaintext_size=int(parsed.get("plaintext_size", 0)),
        encrypted=bool(parsed.get("encrypted", True)),
        digest=str(parsed.get("digest", "")),
    )


class SegmentedObjectProducer:
    """Serve one payload as signed segmented NDN Data.

    This is a thin Python wrapper around ndn-cxx Segmenter. The base name is a
    generic object name, not an AI artifact name; higher-level frameworks such
    as NDNSF-DI decide whether the object is a model shard, runner, activation,
    or some other application object.
    """

    def __init__(
        self,
        base_name: str,
        payload: bytes,
        *,
        signing_identity: str = "",
        max_segment_size: int = 6000,
        freshness_ms: int = 60000,
    ) -> None:
        self._native = _ndnsf.SegmentedObjectProducer(
            base_name,
            bytes(payload),
            signing_identity,
            int(max_segment_size),
            int(freshness_ms),
        )

    @property
    def base_name(self) -> str:
        return str(self._native.base_name)

    @property
    def versioned_name(self) -> str:
        return str(self._native.versioned_name)

    @property
    def segment_count(self) -> int:
        return int(self._native.segment_count)

    @property
    def error(self) -> str:
        return str(self._native.error)

    def start(self) -> "SegmentedObjectProducer":
        self._native.start()
        return self

    def stop(self) -> None:
        self._native.stop()


@dataclass(frozen=True)
class DataPacket:
    """One immutable NDN Data packet encoded in wire format."""

    name: str
    segment: int
    wire: bytes


@dataclass(frozen=True)
class SegmentHintRange:
    """Forwarding hints that apply to a contiguous segment range."""

    start: int
    end: int
    forwarding_hints: tuple[str, ...]


class StoredDataProducer:
    """Serve already-signed NDN Data packets without rewriting them."""

    def __init__(
        self,
        base_name: str,
        packet_wires: list[bytes],
        *,
        signing_identity: str = "",
    ) -> None:
        self._native = _ndnsf.StoredDataProducer(
            base_name,
            [bytes(packet) for packet in packet_wires],
            signing_identity,
        )

    @property
    def segment_count(self) -> int:
        return int(self._native.segment_count)

    @property
    def error(self) -> str:
        return str(self._native.error)

    def start(self) -> "StoredDataProducer":
        self._native.start()
        return self

    def stop(self) -> None:
        self._native.stop()


def make_segmented_data_packets(
    base_name: str,
    payload: bytes,
    *,
    signing_identity: str = "",
    max_segment_size: int = 6000,
    freshness_ms: int = 60000,
) -> list[DataPacket]:
    """Create signed segmented NDN Data packets for direct packet storage."""

    packets = _ndnsf.make_segmented_data_packets(
        base_name,
        bytes(payload),
        signing_identity,
        int(max_segment_size),
        int(freshness_ms),
    )
    return [
        DataPacket(str(packet.name), int(packet.segment), bytes(packet.wire))
        for packet in packets
    ]


def fetch_segmented_data_packets(
    base_name: str,
    *,
    timeout_ms: int = 30000,
    interest_lifetime_ms: Optional[int] = None,
    forwarding_hints: Optional[list[str]] = None,
) -> list[DataPacket]:
    """Fetch segmented NDN Data and return the original Data wire packets."""

    packets = _ndnsf.fetch_segmented_data_packets(
        base_name,
        int(timeout_ms),
        int(interest_lifetime_ms or default_large_data_interest_lifetime_ms()),
        list(forwarding_hints or []),
    )
    return [
        DataPacket(str(packet.name), int(packet.segment), bytes(packet.wire))
        for packet in packets
    ]


def fetch_segmented_object(
    base_name: str,
    *,
    timeout_ms: int = 30000,
    interest_lifetime_ms: Optional[int] = None,
    init_cwnd: float = 8.0,
    forwarding_hints: Optional[list[str]] = None,
) -> bytes:
    """Fetch signed segmented NDN Data with ndn-cxx SegmentFetcher."""

    return bytes(_ndnsf.fetch_segmented_object(
        base_name,
        int(timeout_ms),
        int(interest_lifetime_ms or default_large_data_interest_lifetime_ms()),
        float(init_cwnd),
        list(forwarding_hints or []),
    ))


def fetch_segmented_object_with_segment_hints(
    base_name: str,
    *,
    timeout_ms: int = 30000,
    interest_lifetime_ms: Optional[int] = None,
    hint_ranges: Optional[list[SegmentHintRange]] = None,
) -> bytes:
    """Fetch segmented Data while allowing each segment range to use hints."""

    native_ranges = []
    for hint_range in hint_ranges or []:
        native = _ndnsf.SegmentHintRange()
        native.start = int(hint_range.start)
        native.end = int(hint_range.end)
        native.forwarding_hints = list(hint_range.forwarding_hints)
        native_ranges.append(native)
    return bytes(_ndnsf.fetch_segmented_object_with_segment_hints(
        base_name,
        int(timeout_ms),
        int(interest_lifetime_ms or default_large_data_interest_lifetime_ms()),
        native_ranges,
    ))


def fetch_known_segmented_object_with_segment_hints(
    versioned_name: str,
    segment_count: int,
    *,
    timeout_ms: int = 30000,
    interest_lifetime_ms: Optional[int] = None,
    hint_ranges: Optional[list[SegmentHintRange]] = None,
) -> bytes:
    """Fetch known signed segments with per-range forwarding hints."""

    native_ranges = []
    for hint_range in hint_ranges or []:
        native = _ndnsf.SegmentHintRange()
        native.start = int(hint_range.start)
        native.end = int(hint_range.end)
        native.forwarding_hints = list(hint_range.forwarding_hints)
        native_ranges.append(native)
    return bytes(_ndnsf.fetch_known_segmented_object_with_segment_hints(
        versioned_name,
        int(segment_count),
        int(timeout_ms),
        int(interest_lifetime_ms or default_large_data_interest_lifetime_ms()),
        native_ranges,
    ))


@dataclass(frozen=True)
class CollaborationRole:
    role: str
    service: str = ""
    artifact: str = ""
    allow_dynamic_provisioning: bool = False
    provisioning_timeout_ms: int = 30000
    app_requirement: bytes = b""
    min_providers: int = 1
    max_providers: int = 1


@dataclass(frozen=True)
class CollaborationDependency:
    producers: list[str]
    consumers: list[str]
    key_scope: str
    topic_prefix: str
    required: bool = True


@dataclass(frozen=True)
class CollaborationAssignment:
    role: str
    service: str
    assigned_artifact: str
    artifact_data_name: str = ""
    requires_provisioning: bool = False
    provisioning_timeout_ms: int = 0
    assignment_payload: bytes = b""
    role_providers: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class CollaborationData:
    session_id: str
    key_scope: str
    topic: str
    producer: str
    producer_role: str
    sequence: int
    payload: bytes


def _parse_assignment_fields(payload: bytes) -> dict[str, str]:
    text = bytes(payload or b"").decode("utf-8", errors="replace")
    fields: dict[str, str] = {}
    for item in text.split(";"):
        if not item or "=" not in item:
            continue
        key, value = item.split("=", 1)
        fields[key] = value
    return fields


def _parse_role_providers(payload: bytes) -> dict[str, str]:
    prefix = "roleProvider."
    return {
        key[len(prefix):]: value
        for key, value in _parse_assignment_fields(payload).items()
        if key.startswith(prefix) and value
    }


@dataclass(frozen=True)
class ExecutionArtifact:
    """One fetchable artifact needed by an execution backend.

    Examples include an ONNX model, TorchScript module, TensorRT engine,
    GGUF model, runtime bundle, or backend-specific configuration file.
    """

    name: str
    data_name: str
    filename: str
    sha256: str
    kind: str = "model"
    chunks: list[str] = None
    executable: bool = False
    cache_name: str = ""
    repo_manifest: dict = None
    large_data_reference: dict = None


@dataclass(frozen=True)
class ExecutionArtifactSpec:
    role: str
    backend: str
    entrypoint: str = ""
    artifacts: list[ExecutionArtifact] = None
    metadata: dict = None

    def to_bytes(self) -> bytes:
        return json.dumps({
            "role": self.role,
            "backend": self.backend,
            "entrypoint": self.entrypoint,
            "artifacts": [
                {
                    "name": artifact.name,
                    "dataName": artifact.data_name,
                    "filename": artifact.filename,
                    "sha256": artifact.sha256,
                    "kind": artifact.kind,
                    "chunks": list(artifact.chunks or []),
                    "executable": bool(artifact.executable),
                    "cacheName": artifact.cache_name,
                    "repoManifest": dict(artifact.repo_manifest or {}),
                    "largeDataReference": dict(artifact.large_data_reference or {}),
                }
                for artifact in (self.artifacts or [])
            ],
            "metadata": dict(self.metadata or {}),
        }, sort_keys=True).encode()

    @staticmethod
    def from_bytes(payload: bytes) -> "ExecutionArtifactSpec":
        obj = json.loads(payload.decode())
        return ExecutionArtifactSpec(
            role=str(obj["role"]),
            backend=str(obj["backend"]),
            entrypoint=str(obj.get("entrypoint", "")),
            artifacts=[
                ExecutionArtifact(
                    name=str(item["name"]),
                    data_name=str(item["dataName"]),
                    filename=str(item["filename"]),
                    sha256=str(item["sha256"]),
                    kind=str(item.get("kind", "model")),
                    chunks=[str(value) for value in item.get("chunks", [])],
                    executable=bool(item.get("executable", False)),
                    cache_name=str(item.get("cacheName", "")),
                    repo_manifest=dict(item.get("repoManifest", {})),
                    large_data_reference=dict(item.get("largeDataReference", {})),
                )
                for item in obj.get("artifacts", [])
            ],
            metadata=dict(obj.get("metadata", {})),
        )


@dataclass(frozen=True)
class ExecutionContext:
    spec: ExecutionArtifactSpec
    artifact_paths: dict[str, Path]
    work_dir: Path

    def path(self, artifact_name: str) -> Path:
        return self.artifact_paths[artifact_name]

    def executable(self, artifact_name: str) -> Path:
        path = self.path(artifact_name)
        artifact = next(
            (item for item in (self.spec.artifacts or []) if item.name == artifact_name),
            None,
        )
        if artifact is None or not artifact.executable:
            raise KeyError(f"artifact {artifact_name!r} is not declared executable")
        return path


class CollaborationContext:
    """Provider-side context for one collaborative service invocation.

    The object is valid only while the Python collaboration handler is running.
    Applications can publish scoped intermediate data, wait for peer data, fetch
    assigned artifacts, and publish the final response.
    """

    def __init__(self, native) -> None:
        self._native = native

    @property
    def session_id(self) -> str:
        return str(self._native.session_id)

    @property
    def role(self) -> str:
        return str(self._native.role)

    @property
    def local_provider(self) -> str:
        return str(self._native.local_provider)

    @property
    def assignment(self) -> CollaborationAssignment:
        native = self._native.assignment
        assignment_payload = bytes(native.assignment_payload)
        return CollaborationAssignment(
            role=str(native.role),
            service=str(native.service),
            assigned_artifact=str(native.assigned_artifact),
            artifact_data_name=str(native.artifact_data_name),
            requires_provisioning=bool(native.requires_provisioning),
            provisioning_timeout_ms=int(native.provisioning_timeout_ms),
            assignment_payload=assignment_payload,
            role_providers=_parse_role_providers(assignment_payload),
        )

    def fetch_artifact(self, artifact_name: str, timeout_ms: int = 5000) -> bool:
        return bool(self._native.fetch_artifact(artifact_name, timeout_ms))

    def get_artifact(self, artifact_name: str) -> Optional[bytes]:
        value = self._native.get_artifact(artifact_name)
        if value is None:
            return None
        return bytes(value)

    def fetch_encrypted_large_data(
        self,
        data_name: str,
        service: str = "",
    ) -> Optional[bytes]:
        value = self._native.fetch_encrypted_large_data(data_name, service)
        if value is None:
            return None
        return bytes(value)

    def prepare_execution(
        self,
        *,
        temp_root: Optional[str | Path] = None,
        allow_executables: bool = False,
    ) -> ExecutionContext:
        """Fetch and materialize the assigned execution artifacts.

        The assigned artifact is expected to be an ``ExecutionArtifactSpec``.
        Each referenced artifact is fetched as encrypted large Data, verified
        by SHA-256, and written under a provider-local temporary directory.
        Executable artifacts are never run by the framework. If
        ``allow_executables`` is true, they are marked owner-executable after
        hash verification so the application handler can invoke them with its
        own sandbox and argument policy.
        """

        assignment = self.assignment
        if not assignment.assigned_artifact:
            raise RuntimeError("collaboration assignment has no artifact name")
        if not self.fetch_artifact(
            assignment.assigned_artifact,
            assignment.provisioning_timeout_ms or 10000,
        ):
            raise RuntimeError(f"failed to fetch execution spec {assignment.assigned_artifact}")
        spec_payload = self.get_artifact(assignment.assigned_artifact)
        if spec_payload is None:
            raise RuntimeError("execution spec fetch returned no payload")
        spec = ExecutionArtifactSpec.from_bytes(spec_payload)

        root = Path(temp_root) if temp_root is not None else Path(tempfile.gettempdir())
        root.mkdir(parents=True, exist_ok=True)
        work_dir = Path(tempfile.mkdtemp(
            prefix=f"ndnsf-{_safe_file_token(spec.role)}-",
            dir=str(root)))
        artifact_paths: dict[str, Path] = {}
        for artifact in spec.artifacts or []:
            cached_payload = _read_cached_artifact(artifact)
            if cached_payload is not None:
                print(
                    "NDNSF_EXECUTION_ARTIFACT_CACHE_HIT "
                    f"role={spec.role} artifact={artifact.name} "
                    f"cacheName={artifact.cache_name}",
                    flush=True,
                )
                payload = cached_payload
            elif artifact.large_data_reference:
                print(
                    "NDNSF_EXECUTION_ARTIFACT_CACHE_MISS "
                    f"role={spec.role} artifact={artifact.name} "
                    f"cacheName={artifact.cache_name} source=reference",
                    flush=True,
                )
                payload = self._fetch_artifact_reference_payload(assignment.service, artifact)
            elif artifact.repo_manifest:
                print(
                    "NDNSF_EXECUTION_ARTIFACT_CACHE_MISS "
                    f"role={spec.role} artifact={artifact.name} "
                    f"cacheName={artifact.cache_name} source=repo",
                    flush=True,
                )
                payload = _fetch_repo_manifest_payload(artifact.repo_manifest)
            elif artifact.chunks:
                print(
                    "NDNSF_EXECUTION_ARTIFACT_CACHE_MISS "
                    f"role={spec.role} artifact={artifact.name} "
                    f"cacheName={artifact.cache_name}",
                    flush=True,
                )
                parts = []
                for index, chunk_name in enumerate(artifact.chunks):
                    part = self.fetch_encrypted_large_data(chunk_name, assignment.service)
                    if part is None:
                        raise RuntimeError(
                            f"failed to fetch execution artifact {artifact.name} chunk {index}")
                    parts.append(part)
                payload = b"".join(parts)
            else:
                print(
                    "NDNSF_EXECUTION_ARTIFACT_CACHE_MISS "
                    f"role={spec.role} artifact={artifact.name} "
                    f"cacheName={artifact.cache_name}",
                    flush=True,
                )
                payload = self.fetch_encrypted_large_data(artifact.data_name, assignment.service)
                if payload is None:
                    raise RuntimeError(f"failed to fetch execution artifact {artifact.name}")
            digest = hashlib.sha256(payload).hexdigest()
            if digest != artifact.sha256:
                raise RuntimeError(
                    f"artifact hash mismatch for {artifact.name}: "
                    f"expected {artifact.sha256}, got {digest}")
            artifact_path = Path(artifact.filename)
            if artifact_path.is_absolute() or ".." in artifact_path.parts:
                raise RuntimeError(f"unsafe artifact filename {artifact.filename!r}")
            _write_cached_artifact(artifact, payload)
            path = work_dir / artifact_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
            if artifact.executable:
                if not allow_executables:
                    raise RuntimeError(
                        f"artifact {artifact.name} is executable; "
                        "call prepare_execution(allow_executables=True) "
                        "only after application policy permits it")
                path.chmod(0o700)
            artifact_paths[artifact.name] = path

        return ExecutionContext(spec=spec,
                                artifact_paths=artifact_paths,
                                work_dir=work_dir)

    def _fetch_artifact_reference_payload(
        self,
        service: str,
        artifact: ExecutionArtifact,
    ) -> bytes:
        reference = dict(artifact.large_data_reference or {})
        source = str(reference.get("source", reference.get("sourceType", ""))).lower()
        data_name = str(reference.get("dataName", reference.get("data_name", "")))
        if source in {"repo", "repo-manifest", "repo_manifest"} or (
            not source and artifact.repo_manifest
        ):
            if not artifact.repo_manifest:
                raise RuntimeError(
                    f"artifact {artifact.name} reference source is repo but no repoManifest is present")
            return _fetch_repo_manifest_payload(artifact.repo_manifest)
        if not data_name:
            data_name = artifact.data_name
        if not data_name:
            raise RuntimeError(f"artifact {artifact.name} reference has no Data name")
        payload = self.fetch_encrypted_large_data(data_name, service)
        if payload is None:
            raise RuntimeError(
                f"failed to fetch execution artifact {artifact.name} from reference {data_name}")
        return payload

    def fail(self, reason: str) -> None:
        self._native.fail(reason)

    def publish(self, key_scope: str, topic: str, payload: bytes) -> None:
        self._native.publish(key_scope, topic, bytes(payload))

    def publish_large(
        self,
        key_scope: str,
        topic: str,
        payload: bytes,
        *,
        max_segment_size: int = 7000,
        freshness_ms: int = 60000,
    ) -> str:
        """Publish large scoped collaboration data as signed segments.

        The returned name is intended to be carried in a small collaboration
        message. Peers use :meth:`fetch_large` to retrieve, validate, reassemble,
        and decrypt the object.
        """

        return str(self._native.publish_large(
            key_scope,
            topic,
            bytes(payload),
            max_segment_size,
            freshness_ms,
        ))

    def publish_large_named(
        self,
        key_scope: str,
        data_name: str,
        payload: bytes,
        *,
        max_segment_size: int = 7000,
        freshness_ms: int = 60000,
    ) -> str:
        """Publish large collaboration data at a deterministic Data name.

        This keeps the same request-scoped hybrid encryption and segment
        retrieval semantics as :meth:`publish_large`, but lets a distributed
        plan make object names predictable for dataflow prefetch.
        """

        return str(self._native.publish_large_named(
            key_scope,
            data_name,
            bytes(payload),
            max_segment_size,
            freshness_ms,
        ))

    def publish_large_reference(
        self,
        key_scope: str,
        data_topic: str,
        ref_topic: str,
        payload: bytes,
        *,
        object_type: str = "",
        object_id: str = "",
        digest: str = "",
        data_name: str = "",
        max_segment_size: int = 7000,
        freshness_ms: int = 60000,
    ) -> str:
        """Publish a large collaboration object and advertise a standard reference."""

        payload_bytes = bytes(payload)
        data_name = (
            self.publish_large_named(
                key_scope,
                data_name,
                payload_bytes,
                max_segment_size=max_segment_size,
                freshness_ms=freshness_ms,
            )
            if data_name else
            self.publish_large(
                key_scope,
                data_topic,
                payload_bytes,
                max_segment_size=max_segment_size,
                freshness_ms=freshness_ms,
            )
        )
        effective_digest = digest or ("sha256:" + hashlib.sha256(payload_bytes).hexdigest())
        reference = encode_large_data_reference_payload(LargeDataReference(
            data_name=data_name,
            object_type=object_type,
            object_id=object_id,
            plaintext_size=len(payload_bytes),
            encrypted=True,
            digest=effective_digest,
        ))
        self.publish(key_scope, ref_topic, reference)
        return data_name

    def fetch_large(
        self,
        data_name: str,
        key_scope: str,
        timeout_ms: int = 5000,
    ) -> Optional[bytes]:
        value = self._native.fetch_large(data_name, key_scope, timeout_ms)
        if value is None:
            return None
        return bytes(value)

    def fetch_large_exact(
        self,
        data_name: str,
        key_scope: str,
        timeout_ms: int = 5000,
        expected_segments: int = 0,
    ) -> Optional[bytes]:
        if expected_segments <= 0:
            return self.fetch_large(data_name, key_scope, timeout_ms)
        value = self._native.fetch_large_exact(
            data_name,
            key_scope,
            timeout_ms,
            int(expected_segments),
        )
        if value is None:
            return None
        return bytes(value)

    def fetch_large_reference(
        self,
        reference_payload: bytes,
        key_scope: str,
        timeout_ms: int = 5000,
    ) -> Optional[bytes]:
        """Fetch a large collaboration object described by a standard reference.

        Older examples published only a naked Data name in the reference
        message. That form is accepted for migration, while new publishers use
        ``LargeDataReference``.
        """

        reference = parse_large_data_reference_payload(bytes(reference_payload))
        if reference is None:
            data_name = bytes(reference_payload).decode()
            expected_size = 0
            expected_digest = ""
        else:
            data_name = reference.data_name
            expected_size = reference.plaintext_size
            expected_digest = reference.digest
        payload = self.fetch_large(data_name, key_scope, timeout_ms)
        if payload is None:
            return None
        if expected_size and len(payload) != expected_size:
            raise ValueError(
                f"large reference size mismatch: expected={expected_size} actual={len(payload)}")
        if expected_digest:
            digest = expected_digest
            if digest.startswith("sha256:"):
                digest = digest[len("sha256:"):]
            if digest and hashlib.sha256(payload).hexdigest() != digest:
                raise ValueError("large reference SHA-256 mismatch")
        return payload

    def wait_one(
        self,
        key_scope: str,
        topic_prefix: str,
        timeout_ms: int = 5000,
    ) -> Optional[CollaborationData]:
        data = self._native.wait_one(key_scope, topic_prefix, timeout_ms)
        if data is None:
            return None
        return _from_native_collaboration_data(data)

    def wait_for(
        self,
        key_scope: str,
        topic_prefix: str,
        min_count: int,
        timeout_ms: int = 5000,
    ) -> list[CollaborationData]:
        return [
            _from_native_collaboration_data(data)
            for data in self._native.wait_for(key_scope, topic_prefix, min_count, timeout_ms)
        ]

    def publish_final_response(self, payload: bytes) -> None:
        self._native.publish_final_response(bytes(payload))


def _to_native_response(response: ServiceResponse) -> _ndnsf.ServiceResponse:
    native = _ndnsf.ServiceResponse()
    native.status = response.status
    native.payload = response.payload
    native.error = response.error
    return native


def _from_native_response(response: _ndnsf.ServiceResponse) -> ServiceResponse:
    return ServiceResponse(
        status=bool(response.status),
        payload=bytes(response.payload),
        error=str(response.error),
    )


def _to_native_ack(decision: AckDecision) -> _ndnsf.AckDecision:
    native = _ndnsf.AckDecision()
    native.status = decision.status
    native.payload = decision.payload
    native.message = decision.message
    native.suppress = decision.suppress
    return native


def _from_native_large_data_result(result) -> LargeDataPublishResult:
    return LargeDataPublishResult(
        success=bool(result.success),
        encrypted_data_name=str(result.encrypted_data_name),
        object_id=str(result.object_id),
        error=str(result.error),
    )


def _from_native_collaboration_data(data) -> CollaborationData:
    return CollaborationData(
        session_id=str(data.session_id),
        key_scope=str(data.key_scope),
        topic=str(data.topic),
        producer=str(data.producer),
        producer_role=str(data.producer_role),
        sequence=int(data.sequence),
        payload=bytes(data.payload),
    )


def _artifact_spec_parts(spec) -> tuple[bytes, str, str, bool, str]:
    if isinstance(spec, dict):
        return (
            bytes(spec["payload"]),
            str(spec["filename"]),
            str(spec.get("kind", "model")),
            bool(spec.get("executable", False)),
            str(spec.get("cache_name", spec.get("cacheName", ""))),
        )
    if len(spec) == 3:
        payload, filename, kind = spec
        executable = False
        cache_name = ""
    elif len(spec) == 4:
        payload, filename, kind, executable = spec
        cache_name = ""
    elif len(spec) == 5:
        payload, filename, kind, executable, cache_name = spec
    else:
        raise ValueError(
            "artifact spec must be (payload, filename, kind), "
            "(payload, filename, kind, executable), "
            "(payload, filename, kind, executable, cache_name), or a dict")
    return bytes(payload), str(filename), str(kind), bool(executable), str(cache_name)


def _artifact_from_repo_spec(name: str, spec: dict) -> Optional[ExecutionArtifact]:
    manifest = spec.get("repo_manifest", spec.get("repoManifest"))
    if not manifest:
        return None
    return ExecutionArtifact(
        name=name,
        data_name="",
        filename=str(spec["filename"]),
        sha256=str(manifest["sha256"]),
        kind=str(spec.get("kind", "model")),
        chunks=[],
        executable=bool(spec.get("executable", False)),
        cache_name=str(spec.get("cache_name", spec.get("cacheName", ""))),
        repo_manifest=dict(manifest),
        large_data_reference=_large_data_reference_dict_from_manifest(
            manifest,
            object_type=str(spec.get("kind", "model")),
            object_id=str(spec.get("cache_name", spec.get("cacheName", spec["filename"]))),
        ),
    )


def _large_data_reference_dict_from_manifest(
    manifest: dict,
    *,
    object_type: str = "",
    object_id: str = "",
) -> dict:
    """Represent a repo manifest as the generic large-object reference shape."""

    return {
        "source": "repo-manifest",
        "dataName": str(manifest.get("objectName", "")),
        "objectType": object_type,
        "objectId": object_id,
        "plaintextSize": int(manifest.get("size", 0)),
        "encrypted": bool(manifest.get("encrypted", False)),
        "digest": "sha256:" + str(manifest.get("sha256", "")),
    }


def _safe_file_token(value: str) -> str:
    token = "".join(
        ch if ch.isalnum() or ch in ("-", "_", ".") else "-"
        for ch in value
    ).strip("-")
    return token or "artifact"


def _fetch_repo_manifest_payload(manifest: dict) -> bytes:
    segment_count = int(manifest.get("segmentCount", 1))
    size = int(manifest["size"])
    expected_hash = str(manifest["sha256"])
    segment_locations = list(manifest.get("segmentLocations", []))
    if segment_locations:
        by_data_name: dict[str, list[dict]] = {}
        for location in segment_locations:
            by_data_name.setdefault(str(location["dataName"]), []).append(dict(location))
        last_error = None
        for data_name, locations in by_data_name.items():
            covered_segments = set()
            hint_ranges: list[SegmentHintRange] = []
            route_strategy = "hint-first"
            for location in locations:
                start = int(location.get("start", 0))
                end = int(location.get("end", start))
                covered_segments.update(range(start, end + 1))
                if str(location.get("routeStrategy", "")) == "direct-first":
                    route_strategy = "direct-first"
                hint_ranges.append(SegmentHintRange(
                    start=start,
                    end=end,
                    forwarding_hints=tuple(str(hint) for hint in location.get("hints", [])),
                ))
            if len(covered_segments) < segment_count:
                continue
            try:
                versioned_names = {
                    str(location.get("versionedDataName", ""))
                    for location in locations
                    if location.get("versionedDataName")
                }
                first_ranges = [] if route_strategy == "direct-first" else hint_ranges
                second_ranges = hint_ranges if route_strategy == "direct-first" else []
                if len(versioned_names) == 1:
                    versioned_name = next(iter(versioned_names))
                    try:
                        payload = fetch_known_segmented_object_with_segment_hints(
                            versioned_name,
                            segment_count,
                            timeout_ms=30000,
                            interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                            hint_ranges=first_ranges,
                        )
                    except Exception:
                        payload = fetch_known_segmented_object_with_segment_hints(
                            versioned_name,
                            segment_count,
                            timeout_ms=30000,
                            interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                            hint_ranges=second_ranges,
                        )
                else:
                    try:
                        payload = fetch_segmented_object_with_segment_hints(
                            data_name,
                            timeout_ms=30000,
                            interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                            hint_ranges=first_ranges,
                        )
                    except Exception:
                        payload = fetch_segmented_object_with_segment_hints(
                            data_name,
                            timeout_ms=30000,
                            interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                            hint_ranges=second_ranges,
                        )
                break
            except Exception as exc:
                last_error = exc
        else:
            raise RuntimeError(
                f"no repo segment location could serve {manifest.get('objectName')}: {last_error}"
            )
    else:
        replica_nodes = [str(value) for value in manifest.get("replicaNodes", [])]
        data_names = [str(value) for value in manifest.get("replicaDataNames", [])]
        if not data_names:
            data_names = [
                RepoDataName.data_name(repo_node, str(manifest["objectName"]))
                for repo_node in replica_nodes
            ]
        last_error = None
        for repo_node, data_name in zip(replica_nodes, data_names):
            try:
                hints = [] if data_name.startswith(repo_node.rstrip("/") + "/") else [repo_node]
                payload = fetch_segmented_object(
                    data_name,
                    timeout_ms=30000,
                    interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                    init_cwnd=8.0,
                    forwarding_hints=hints,
                )
                break
            except Exception as exc:
                last_error = exc
        else:
            raise RuntimeError(
                f"no repo replica could serve {manifest.get('objectName')}: {last_error}"
            )
    if len(payload) != size:
        raise RuntimeError(f"repo object size mismatch: {manifest.get('objectName')}")
    digest = hashlib.sha256(payload).hexdigest()
    if digest != expected_hash:
        raise RuntimeError(
            f"repo object hash mismatch: {manifest.get('objectName')}: "
            f"expected {expected_hash}, got {digest}"
        )
    return payload


class RepoDataName:
    @staticmethod
    def data_name(repo_node: str, object_name: str) -> str:
        digest = hashlib.sha256(object_name.encode()).hexdigest()
        return f"{repo_node.rstrip('/')}/NDNSF-DISTRIBUTED-REPO/DATA/{digest}"


def _artifact_cache_path(artifact: ExecutionArtifact) -> Optional[Path]:
    if not artifact.cache_name:
        return None
    filename = Path(artifact.filename).name
    root = os.environ.get("NDNSF_ARTIFACT_CACHE_DIR")
    cache_root = Path(root) if root else Path.home() / ".cache" / "ndnsf" / "artifacts"
    cache_dir = cache_root / f"{_safe_file_token(artifact.cache_name)}-{artifact.sha256[:16]}"
    return cache_dir / filename


def _read_cached_artifact(artifact: ExecutionArtifact) -> Optional[bytes]:
    path = _artifact_cache_path(artifact)
    if path is None or not path.exists():
        return None
    payload = path.read_bytes()
    if hashlib.sha256(payload).hexdigest() != artifact.sha256:
        return None
    return payload


def _write_cached_artifact(artifact: ExecutionArtifact, payload: bytes) -> None:
    path = _artifact_cache_path(artifact)
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    if artifact.executable:
        path.chmod(0o700)


def _role_to_dict(role: CollaborationRole | dict) -> dict:
    if isinstance(role, CollaborationRole):
        return {
            "role": role.role,
            "service": role.service,
            "artifact": role.artifact,
            "allow_dynamic_provisioning": role.allow_dynamic_provisioning,
            "provisioning_timeout_ms": role.provisioning_timeout_ms,
            "app_requirement": role.app_requirement,
            "min_providers": role.min_providers,
            "max_providers": role.max_providers,
        }
    return dict(role)


def _dependency_to_dict(dep: CollaborationDependency | dict) -> dict:
    if isinstance(dep, CollaborationDependency):
        return {
            "producers": list(dep.producers),
            "consumers": list(dep.consumers),
            "key_scope": dep.key_scope,
            "topic_prefix": dep.topic_prefix,
            "required": dep.required,
        }
    return dict(dep)


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


class ServiceProvider:
    """Python API for writing NDNSF provider business logic."""

    def __init__(
        self,
        *,
        provider_id: str = "",
        group: str = "/example/hello/group",
        controller: str = "/example/hello/controller",
        provider_prefix: str = "/example/hello/provider",
        trust_schema: str = "examples/trust-schema.conf",
        handler_threads: int = 4,
        ack_threads: int = 2,
        serve_certificates: bool = True,
        bootstrap_token: str = "",
        binary: str = "",
        binary_dir=None,
        library_dirs=None,
        cwd=None,
        env=None,
    ) -> None:
        # The last five parameters are accepted for source compatibility with
        # the previous subprocess bridge. pybind11 uses the loaded extension
        # module, not a separate host binary.
        del binary, binary_dir, library_dirs, cwd, env
        self._native = _ndnsf.NativeServiceProvider(
            provider_id=provider_id,
            group=group,
            controller=controller,
            provider_prefix=provider_prefix,
            trust_schema=trust_schema,
            handler_threads=handler_threads,
            ack_threads=ack_threads,
            serve_certificates=serve_certificates,
            bootstrap_token=bootstrap_token,
        )
        self._handlers: dict[str, Callable[[bytes], bytes | ServiceResponse]] = {}
        self._ack_handlers: dict[str, Callable[[bytes], bool | AckDecision]] = {}
        self._collaboration_services: set[str] = set()

    def add_handler(
        self,
        service: str,
        handler: Callable[[bytes], bytes | ServiceResponse],
    ) -> None:
        self._handlers[service] = handler

    def handler(self, service: str):
        def decorator(fn: Callable[[bytes], bytes | ServiceResponse]):
            self.add_handler(service, fn)
            return fn
        return decorator

    def set_ack_handler(
        self,
        service: str,
        handler: Callable[[bytes], bool | AckDecision],
    ) -> None:
        self._ack_handlers[service] = handler

    def ack_handler(self, service: str):
        def decorator(fn: Callable[[bytes], bool | AckDecision]):
            self.set_ack_handler(service, fn)
            return fn
        return decorator

    def _register_service(self, service: str) -> None:
        if service not in self._handlers:
            raise ValueError(f"no handler registered for {service}")

        def request_handler(payload: bytes):
            result = self._handlers[service](payload)
            if isinstance(result, ServiceResponse):
                return _to_native_response(result)
            return bytes(result)

        ack_handler = None
        if service in self._ack_handlers:
            def ack_handler(payload: bytes):
                result = self._ack_handlers[service](payload)
                if isinstance(result, AckDecision):
                    return _to_native_ack(result)
                return bool(result)

        self._native.add_service(service, request_handler, ack_handler)

    def add_collaboration_handler(
        self,
        service: str,
        allowed_roles: list[str],
        handler: Callable[[CollaborationContext, bytes], None],
        ack_handler: Optional[Callable[[bytes], bool | AckDecision]] = None,
    ) -> None:
        def request_handler(native_ctx, payload: bytes):
            handler(CollaborationContext(native_ctx), bytes(payload))

        native_ack = None
        if ack_handler is not None:
            def native_ack(payload: bytes):
                result = ack_handler(bytes(payload))
                if isinstance(result, AckDecision):
                    return _to_native_ack(result)
                return bool(result)

        self._native.add_collaboration_service(
            service,
            list(allowed_roles),
            request_handler,
            native_ack,
        )
        self._collaboration_services.add(service)

    def collaboration_handler(
        self,
        service: str,
        allowed_roles: list[str],
        ack_handler: Optional[Callable[[bytes], bool | AckDecision]] = None,
    ):
        def decorator(fn: Callable[[CollaborationContext, bytes], None]):
            self.add_collaboration_handler(service, allowed_roles, fn, ack_handler)
            return fn
        return decorator

    def run(self, service: Optional[str] = None) -> int:
        if service is None and not self._handlers and self._collaboration_services:
            self._native.run()
            return 0
        if service is None:
            if len(self._handlers) != 1:
                raise ValueError("service must be specified when multiple handlers are registered")
            service = next(iter(self._handlers))
        if service in self._handlers:
            self._register_service(service)
        self._native.run()
        return 0

    def publish_service_info(self,
                             service_name: str,
                             service_lifetime_seconds: int = 30,
                             meta_info: Optional[dict[str, str]] = None) -> None:
        """Publish service availability and capacity via NDNSD (requires NDNSF_ENABLE_NDNSD=1)."""
        self._native.publish_service_info(service_name, service_lifetime_seconds, meta_info or {})

    def update_ndnsd_meta(self, key: str, value: str) -> None:
        """Update one key in the internal NDNSD meta dict (thread-safe).

        Changes are picked up by the next periodic heartbeat.
        """
        self._native.update_ndnsd_meta(key, value)

    def set_ndnsd_meta(self, meta: dict[str, str]) -> None:
        """Replace the entire internal NDNSD meta dict (thread-safe)."""
        self._native.set_ndnsd_meta(meta)

    def start_ndnsd_heartbeat(self, interval_seconds: int = 10) -> None:
        """Start periodic NDNSD heartbeat using the C++ io_context scheduler.

        Reads the internal meta dict (updated via update_ndnsd_meta) each tick.
        Publishes for every registered service.
        """
        self._native.start_ndnsd_periodic_publish(interval_seconds)

    def start_background(self, service: Optional[str] = None) -> threading.Thread:
        thread = threading.Thread(target=self.run, args=(service,), daemon=True)
        thread.start()
        return thread

    def stop(self) -> int:
        self._native.stop()
        return 0


class ServiceController:
    """Python API for running the NDNSF ServiceController role."""

    def __init__(
        self,
        *,
        controller_prefix: str = "/example/hello/controller",
        policy_file: str = "examples/hello.policies",
        trust_schema: str = "examples/trust-schema.conf",
        bootstrap_identities: Optional[list[str]] = None,
        serve_certificates: bool = True,
        bootstrap_token_file: str = "",
        binary: str = "",
        binary_dir=None,
        library_dirs=None,
        cwd=None,
        env=None,
    ) -> None:
        del binary, binary_dir, library_dirs, cwd, env
        self._native = _ndnsf.NativeServiceController(
            controller_prefix=controller_prefix,
            policy_file=policy_file,
            trust_schema=trust_schema,
            bootstrap_identities=list(bootstrap_identities or []),
            serve_certificates=serve_certificates,
            bootstrap_token_file=bootstrap_token_file,
        )

    def start(self) -> None:
        self._native.start()

    def run(self) -> int:
        self._native.run()
        return 0

    def stop(self) -> int:
        self._native.stop()
        return 0

    def start_background(self) -> threading.Thread:
        thread = threading.Thread(target=self.run, daemon=True)
        thread.start()
        return thread


class ServiceUser:
    """Python API for issuing NDNSF service requests."""

    def __init__(
        self,
        *,
        group: str = "/example/hello/group",
        controller: str = "/example/hello/controller",
        user: str = "/example/hello/user",
        trust_schema: str = "examples/trust-schema.conf",
        permission_wait_ms: int = 1500,
        handler_threads: int = 2,
        ack_threads: int = 2,
        adaptive_admission: bool = False,
        serve_certificates: bool = True,
        bootstrap_token: str = "",
        binary: str = "",
        binary_dir=None,
        library_dirs=None,
        cwd=None,
        env=None,
    ) -> None:
        del binary, binary_dir, library_dirs, cwd, env
        self.group = group
        self.controller = controller
        self.user = user
        self.trust_schema = trust_schema
        self._native = _ndnsf.NativeServiceUser(
            group=group,
            controller=controller,
            user=user,
            trust_schema=trust_schema,
            permission_wait_ms=permission_wait_ms,
            handler_threads=handler_threads,
            ack_threads=ack_threads,
            adaptive_admission=adaptive_admission,
            serve_certificates=serve_certificates,
            bootstrap_token=bootstrap_token,
        )

    def request_service(
        self,
        service: str,
        payload: bytes,
        *,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 5000,
        strategy: str = "first-responding",
    ) -> ServiceResponse:
        response = self._native.request_service(
            service,
            bytes(payload),
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            strategy=strategy,
        )
        return _from_native_response(response)

    def request_service_select(
        self,
        service: str,
        payload: bytes,
        selector: Callable[[list[AckCandidate]], list[str]],
        *,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 5000,
        request_strategy: str = "first-responding",
    ) -> ServiceResponse:
        """Request a service using an application-defined ACK selector.

        ``selector`` receives all ACK candidates collected during
        ``ack_timeout_ms`` and returns provider names to select. This is the
        generic hook used by DistributedRepo to select exactly N repo replicas
        from one shared repo service name.
        """

        def native_selector(native_candidates) -> list[str]:
            return list(selector([
                AckCandidate(
                    provider_name=str(candidate.provider_name),
                    service_name=str(candidate.service_name),
                    request_id=str(candidate.request_id),
                    status=bool(candidate.status),
                    message=str(candidate.message),
                    payload=bytes(candidate.payload),
                    telemetry=(
                        None if candidate.telemetry is None
                        else dict(candidate.telemetry)
                    ),
                )
                for candidate in native_candidates
            ]))

        response = self._native.request_service_select(
            service,
            bytes(payload),
            native_selector,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            request_strategy=request_strategy,
        )
        return _from_native_response(response)

    def request_service_async(
        self,
        service: str,
        payload: bytes,
        *,
        on_response: Callable[[ServiceResponse], None],
        on_timeout: Callable[[str], None],
        ack_timeout_ms: int = 300,
        timeout_ms: int = 5000,
        strategy: str = "first-responding",
    ) -> None:
        """Submit a request and return immediately.

        The C++ runtime owns Face/SVS/NAC-ABE processing in a background event
        loop. Python only receives final response or timeout callbacks.
        """

        self._native.request_service_async(
            service,
            bytes(payload),
            lambda response: on_response(_from_native_response(response)),
            on_timeout,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            strategy=strategy,
        )

    def publish_encrypted_large_data(
        self,
        service: str,
        payload: bytes,
        *,
        object_label: str = "",
        freshness_ms: int = 60000,
    ) -> LargeDataPublishResult:
        result = self._native.publish_encrypted_large_data(
            service,
            bytes(payload),
            object_label,
            freshness_ms,
        )
        return _from_native_large_data_result(result)

    def publish_execution_artifact_spec(
        self,
        service: str,
        *,
        role: str,
        backend: str,
        artifacts: dict,
        entrypoint: str = "",
        metadata: Optional[dict] = None,
        object_label_prefix: str = "execution",
        max_artifact_chunk_size: int = 512 * 1024,
        freshness_ms: int = 60000,
    ) -> LargeDataPublishResult:
        """Publish artifacts and a provider-consumable execution spec.

        ``artifacts`` maps logical artifact names to either
        ``(payload, filename, kind)``, ``(payload, filename, kind, executable)``,
        ``(payload, filename, kind, executable, cache_name)``, or a dict
        containing ``payload``, ``filename``, ``kind``, and optional
        ``executable``/``cache_name``. The returned Data name should be used as the role's
        ``artifact_data_names[role]`` in ``request_collaboration``.
        """

        refs: list[ExecutionArtifact] = []
        for name, spec in artifacts.items():
            if isinstance(spec, dict):
                repo_artifact = _artifact_from_repo_spec(name, spec)
                if repo_artifact is not None:
                    refs.append(repo_artifact)
                    continue
            payload, filename, kind, executable, cache_name = _artifact_spec_parts(spec)
            payload = bytes(payload)
            chunks: list[str] = []
            data_name = ""
            if max_artifact_chunk_size > 0 and len(payload) > max_artifact_chunk_size:
                for offset in range(0, len(payload), max_artifact_chunk_size):
                    chunk = payload[offset:offset + max_artifact_chunk_size]
                    chunk_index = offset // max_artifact_chunk_size
                    result = self.publish_encrypted_large_data(
                        service,
                        chunk,
                        object_label=f"{object_label_prefix}-{role}-{name}-part{chunk_index:04d}",
                        freshness_ms=freshness_ms,
                    )
                    if not result.success:
                        return result
                    chunks.append(result.encrypted_data_name)
            else:
                result = self.publish_encrypted_large_data(
                    service,
                    payload,
                    object_label=f"{object_label_prefix}-{role}-{name}",
                    freshness_ms=freshness_ms,
                )
                if not result.success:
                    return result
                data_name = result.encrypted_data_name
            refs.append(ExecutionArtifact(
                name=name,
                data_name=data_name,
                filename=filename,
                sha256=sha256_bytes(bytes(payload)),
                kind=kind,
                chunks=chunks,
                executable=executable,
                cache_name=cache_name,
                large_data_reference=(
                    {
                        "source": "ndn-large-data",
                        "dataName": data_name,
                        "objectType": kind,
                        "objectId": cache_name or filename,
                        "plaintextSize": len(payload),
                        "encrypted": True,
                        "digest": "sha256:" + sha256_bytes(bytes(payload)),
                    }
                    if data_name else {}
                ),
            ))

        spec = ExecutionArtifactSpec(
            role=role,
            backend=backend,
            entrypoint=entrypoint,
            artifacts=refs,
            metadata=dict(metadata or {}),
        )
        return self.publish_encrypted_large_data(
            service,
            spec.to_bytes(),
            object_label=f"{object_label_prefix}-{role}-spec",
            freshness_ms=freshness_ms,
        )

    def request_collaboration(
        self,
        service: str,
        payload: bytes,
        *,
        roles: list[CollaborationRole | dict],
        key_scopes: dict[str, list[str]],
        dependencies: Optional[list[CollaborationDependency | dict]] = None,
        artifact_data_names: Optional[dict[str, str]] = None,
        scope_key_data_names: Optional[dict[str, str]] = None,
        role_scopes: Optional[dict[str, list[str]]] = None,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 10000,
        ack_observer: Optional[Callable[[list[AckCandidate]], None]] = None,
        deployment_id: Optional[str] = None,
    ) -> ServiceResponse:
        """Run a generic multi-provider collaboration.

        Provider ACK payloads should include ``role=<role>;``. The built-in
        Python selector assigns each requested role to the first successful ACK
        advertising that role, then sends per-role assignment metadata with
        artifact Data names and scope-key Data names.
        ``ack_observer`` receives the ACK candidates collected for the
        collaboration request before the built-in role selector chooses
        providers. It is observational only and must not return a value.
        ``deployment_id`` pre-fills role→provider from an existing deployment
        and reuses its scope keys (deployment-level, not request-level).
        """

        native_ack_observer = None
        if ack_observer is not None:
            def native_ack_observer(native_candidates) -> None:
                ack_observer([
                    AckCandidate(
                        provider_name=str(candidate.provider_name),
                        service_name=str(candidate.service_name),
                        request_id=str(candidate.request_id),
                        status=bool(candidate.status),
                        message=str(candidate.message),
                        payload=bytes(candidate.payload),
                        telemetry=(
                            None if candidate.telemetry is None
                            else dict(candidate.telemetry)
                        ),
                    )
                    for candidate in native_candidates
                ])

        # Pre-fill role→provider from deployment if specified
        role_provider_pref = ""
        if deployment_id:
            dep = self.get_deployment(deployment_id)
            if dep and dep.get("status") in {"ACTIVE", "DEGRADED"}:
                fm = dep.get("fragmentMap", dep.get("fragment_map", {}))
                prefs = []
                for role_id, providers in fm.items():
                    if providers:
                        provider = providers[0].get("provider", "") if isinstance(providers[0], dict) else str(providers[0])
                        if provider:
                            prefs.append(f"{role_id}=>{provider}")
                role_provider_pref = ";".join(prefs) + (";" if prefs else "")

        import os as _os
        prev_pref = _os.environ.get("NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE", "")
        if role_provider_pref:
            _os.environ["NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE"] = role_provider_pref
        try:
            response = self._native.request_collaboration(
                service,
                bytes(payload),
                [_role_to_dict(role) for role in roles],
                {str(scope): list(scope_roles) for scope, scope_roles in key_scopes.items()},
                [_dependency_to_dict(dep) for dep in (dependencies or [])],
                dict(artifact_data_names or {}),
                dict(scope_key_data_names or {}),
                {str(role): list(scopes) for role, scopes in (role_scopes or {}).items()},
                ack_timeout_ms,
                timeout_ms,
                native_ack_observer,
            )
            return _from_native_response(response)
        finally:
            if role_provider_pref:
                if prev_pref:
                    _os.environ["NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE"] = prev_pref
                else:
                    _os.environ.pop("NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE", None)

    def request_collaboration_async(
        self,
        service: str,
        payload: bytes,
        *,
        roles: list[CollaborationRole | dict],
        key_scopes: dict[str, list[str]],
        dependencies: Optional[list[CollaborationDependency | dict]] = None,
        artifact_data_names: Optional[dict[str, str]] = None,
        scope_key_data_names: Optional[dict[str, str]] = None,
        role_scopes: Optional[dict[str, list[str]]] = None,
        on_response: Callable[[ServiceResponse], None],
        on_timeout: Callable[[str], None],
        ack_timeout_ms: int = 300,
        timeout_ms: int = 10000,
    ) -> None:
        """Submit a generic multi-provider collaboration without blocking."""

        self._native.request_collaboration_async(
            service,
            bytes(payload),
            [_role_to_dict(role) for role in roles],
            {str(scope): list(scope_roles) for scope, scope_roles in key_scopes.items()},
            [_dependency_to_dict(dep) for dep in (dependencies or [])],
            dict(artifact_data_names or {}),
            dict(scope_key_data_names or {}),
            {str(role): list(scopes) for role, scopes in (role_scopes or {}).items()},
            on_response,
            on_timeout,
            ack_timeout_ms,
            timeout_ms,
        )

    def start(self) -> None:
        """Start the user's background Face event loop for async requests."""

        self._native.start()

    def stop(self) -> None:
        """Stop the user's background Face event loop."""

        self._native.stop()

    def get_allowed_services(self) -> list[AllowedService]:
        """Return the current permission snapshot fetched from ServiceController."""

        return [
            AllowedService(
                provider_service=str(provider_service),
                service=str(service),
                token=str(token),
            )
            for provider_service, service, token in self._native.get_allowed_services()
        ]

    def get_ndnsd_services(self) -> list[dict[str, Any]]:
        """Return received NDNSD service details from discovered providers.

        Requires NDNSF_ENABLE_NDNSD=1. Each entry contains provider, serviceName,
        serviceLifetime, publishTimestamp, and serviceMetaInfo dict.
        """
        return [
            {str(k): v for k, v in item.items()}
            for item in self._native.get_ndnsd_services()
        ]

    def discover_deployments(self, service: str = "") -> list[dict[str, Any]]:
        """Discover deployments, sorted by readiness (ACTIVE first).

        ACTIVE (GPU-resident, p50 < 10ms) → IDLE (memory-resident) →
        DISK_RESIDENT (35ms reload) → EVICTED (full redeploy).
        """
        import json as _json
        try:
            self._native.pump(50)
        except Exception:
            pass
        deployments: list[dict[str, Any]] = []
        seen: set[str] = set()
        for entry in self.get_ndnsd_services():
            meta = entry.get("serviceMetaInfo", {})
            if not isinstance(meta, dict):
                continue
            raw = meta.get("deployments", "")
            if not raw:
                continue
            try:
                deps = _json.loads(raw)
            except (_json.JSONDecodeError, TypeError):
                continue
            for dep in deps if isinstance(deps, list) else []:
                if not isinstance(dep, dict):
                    continue
                svc = str(dep.get("serviceName", dep.get("service_name", "")))
                if service and svc != service:
                    continue
                did = str(dep.get("deploymentId", dep.get("deployment_id", "")))
                if did and did not in seen:
                    seen.add(did)
                    deployments.append(dep)
        if not deployments:
            try:
                from ndnsf.coordination import CoordinationIntent, CoordinationServiceClient
                client = CoordinationServiceClient(
                    self, service_name=COORDINATION_ADVISORY_SERVICE,
                    ack_timeout_ms=1000, timeout_ms=5000)
                intent = CoordinationIntent(
                    intent_id=f"discover-{int(time.time()*1000)}",
                    request_id="discover-deployments",
                    requester_name=self.user,
                    service_name=COORDINATION_ADVISORY_SERVICE,
                    purpose="discover-deployments",
                    payload={"serviceName": service})
                response = client.request([intent])
                for s in response.suggestions:
                    dep = s.payload
                    if service and dep.get("serviceName", dep.get("service_name", "")) != service:
                        continue
                    did = str(dep.get("deploymentId", dep.get("deployment_id", "")))
                    if did and did not in seen:
                        seen.add(did)
                        deployments.append(dep)
            except Exception:
                pass
        deployments = [_with_deployment_operation_status(dep) for dep in deployments]
        deployments.sort(key=_deployment_sort_key)
        return deployments

    def get_deployment(self, deployment_id: str) -> dict[str, Any] | None:
        """Find a specific deployment by ID through NDNSD."""
        for dep in self.discover_deployments():
            if dep.get("deploymentId", dep.get("deployment_id", "")) == deployment_id:
                return dep
        return None

    def deploy_service(self,
                       service: str,
                       plan: dict[str, Any],
                       *,
                       roles: list[CollaborationRole | dict],
                       key_scopes: dict[str, list[str]],
                       artifact_data_names: dict[str, str] | None = None,
                       scope_key_data_names: dict[str, str] | None = None,
                       dependencies: list[CollaborationDependency | dict] | None = None,
                       ack_timeout_ms: int = 10000,
                       timeout_ms: int = 60000) -> dict[str, Any]:
        """Deploy a model across providers via the existing ACK/Selection/Lease flow.

        Reuses ``request_collaboration`` — providers ACK, user selects optimal
        providers, leases are reserved.  The deployer receives all provider
        responses directly and knows immediately when the deployment is ACTIVE
        (response.status=true).  No polling is needed for the deployer.

        Other users discover the deployment via ``discover_deployments()``
        (NDNSD/SVS gossip) or ``wait_deployment()``.

        The ``fragment_map`` is built from the ACK observer: the selected
        provider for each role is recorded during the selection phase.
        """
        import hashlib as _hashlib
        import json as _json
        import secrets as _secrets
        import time as _time

        deployment_id = "dep-" + _hashlib.sha256(
            _secrets.token_hex(16).encode()).hexdigest()[:12]
        started_ms = int(_time.time() * 1000)

        # Capture provider assignments per role.  Supports provisioning ACKs
        # (negative-ACK with role advert) and multi-replica.
        selected_providers: dict[str, list[str]] = {}

        def _deploy_observer(candidates: list[AckCandidate]) -> None:
            for c in candidates:
                for role in _deployment_roles_from_ack_candidate(c):
                    selected_providers.setdefault(role, []).append(c.provider_name)

        response = self.request_collaboration(
            service,
            _json.dumps({"purpose": "deploy", "deploymentId": deployment_id,
                          "plan": plan}).encode(),
            roles=roles,
            key_scopes=key_scopes,
            dependencies=dependencies,
            artifact_data_names=artifact_data_names or {},
            scope_key_data_names=scope_key_data_names or {},
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            ack_observer=_deploy_observer,
        )

        # Build fragment_map with multi-replica support (deduplicate)
        fragment_map: dict[str, list[dict[str, Any]]] = {}
        for role, providers in selected_providers.items():
            seen = set()
            entries = []
            for p in providers:
                if p not in seen:
                    seen.add(p)
                    entries.append({"provider": p, "role": role})
            fragment_map[role] = entries

        status = "ACTIVE" if response.status else "PROVISIONING"
        deployment = {
            "deploymentId": deployment_id,
            "planId": plan.get("planId", plan.get("plan_id", "")),
            "serviceName": service,
            "status": status,
            "fragmentMap": fragment_map,
            "scopeKeyDataNames": scope_key_data_names or {},
            "artifactDataNames": artifact_data_names or {},
            "refCount": 0,
            "createdAtMs": started_ms,
            "updatedAtMs": int(_time.time() * 1000),
        }
        deployment = _with_deployment_operation_status(deployment)

        self._publish_deployment_ndnsd(deployment)
        return deployment

    def _publish_deployment_ndnsd(self, deployment: dict[str, Any]) -> None:
        """Route deployment state change through the Merge Provider (coordinator).

        The coordinator is the sole authority for deployment state and NDNSD
        publishing.  Users never write NDNSD directly — they route through
        the coordinator service.
        """
        import json as _json
        from ndnsf.coordination import CoordinationIntent, CoordinationServiceClient
        try:
            client = CoordinationServiceClient(
                self, service_name=COORDINATION_ADVISORY_SERVICE,
                ack_timeout_ms=1000, timeout_ms=5000)
            intent = CoordinationIntent(
                intent_id=f"dep-publish-{deployment.get('deploymentId', '')}",
                request_id=f"ndnsd-{deployment.get('deploymentId', '')}",
                requester_name=self.user,
                service_name=COORDINATION_ADVISORY_SERVICE,
                purpose="deploy",
                payload=deployment)
            client.request([intent])
        except Exception:
            pass  # coordinator unreachable is recoverable; retry on next action

    def evict_deployment(self, deployment_id: str) -> dict[str, Any]:
        """Evict a deployment from all providers, freeing resources.

        Rejected if deployment has active execution leases (ref_count > 0).
        Sets status to DISK_RESIDENT or EVICTED and broadcasts via NDNSD.
        """
        import json as _json
        import time as _time
        dep = self.get_deployment(deployment_id)
        if dep is None:
            return _with_deployment_operation_status(
                {"status": "NOT_FOUND", "deploymentId": deployment_id},
                operation="EVICT_DEPLOYMENT")
        ref_count = int(dep.get("refCount", dep.get("ref_count", 0)))
        if ref_count > 0:
            return _with_deployment_operation_status(
                {"status": "REJECTED", "deploymentId": deployment_id,
                 "reason": f"DEPLOYMENT_IN_USE;ref_count={ref_count}",
                 "refCount": ref_count},
                operation="EVICT_DEPLOYMENT")
        dep["status"] = "DISK_RESIDENT"
        dep["updatedAtMs"] = int(_time.time() * 1000)
        dep = _with_deployment_operation_status(dep, operation="EVICT_DEPLOYMENT")
        self._publish_deployment_ndnsd(dep)
        return dep

    def acquire_execution_lease(self, deployment_id: str,
                                 ttl_ms: int = 30000) -> dict[str, Any]:
        """Acquire an execution lease through the Merge Provider (coordinator).

        The coordinator is the single authority for ref_count.
        """
        from ndnsf.coordination import CoordinationIntent, CoordinationServiceClient
        client = CoordinationServiceClient(self, service_name=COORDINATION_ADVISORY_SERVICE)
        intent = CoordinationIntent(
            intent_id=f"acquire-lease-{deployment_id}",
            request_id=deployment_id,
            requester_name=self.user,
            service_name=COORDINATION_ADVISORY_SERVICE,
            purpose="acquire-lease",
            payload={"deploymentId": deployment_id},
        )
        try:
            response = client.request([intent])
            if response.suggestions:
                return response.suggestions[0].payload
        except Exception:
            pass
        # Fallback: create a local lease (no ref_count tracking)
        from ndnsf.runtime_telemetry import ExecutionLease
        lease = ExecutionLease.create(deployment_id, user=self.user, ttl_ms=ttl_ms)
        return {
            "leaseId": lease.lease_id,
            "deploymentId": deployment_id,
            "acquiredAtMs": lease.acquired_at_ms,
            "expiresAtMs": lease.expires_at_ms,
            "status": "GRANTED_LOCAL",
        }

    def release_execution_lease(self, lease_id: str) -> dict[str, Any]:
        """Release an execution lease through the Merge Provider (coordinator)."""
        from ndnsf.coordination import CoordinationIntent, CoordinationServiceClient
        client = CoordinationServiceClient(self, service_name=COORDINATION_ADVISORY_SERVICE)
        intent = CoordinationIntent(
            intent_id=f"release-lease-{lease_id}",
            request_id=lease_id,
            requester_name=self.user,
            service_name=COORDINATION_ADVISORY_SERVICE,
            purpose="release-lease",
            payload={"leaseId": lease_id},
        )
        try:
            response = client.request([intent])
            if response.suggestions:
                return response.suggestions[0].payload
        except Exception:
            pass
        return {"status": "NOT_FOUND", "leaseId": lease_id}

    def wait_deployment(self, deployment_id: str,
                        timeout_ms: int = 60000,
                        target_status: str = "ACTIVE") -> dict[str, Any] | None:
        """Block until a deployment reaches target_status or timeout."""
        import time as _time
        deadline = _time.time() + timeout_ms / 1000.0
        while _time.time() < deadline:
            dep = self.get_deployment(deployment_id)
            if dep and dep.get("status") == target_status:
                return dep
            _time.sleep(0.5)
        return self.get_deployment(deployment_id)

    def pump(self, milliseconds: int) -> None:
        self._native.pump(milliseconds)

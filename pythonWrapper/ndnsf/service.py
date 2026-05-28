"""Python-facing NDNSF service API backed by a pybind11 extension.

Python application code defines request handlers and issues service requests in
Python. The NDNSF runtime itself stays in C++ through ``ndnsf._ndnsf``: Face,
SVS, NAC-ABE, signing, token checks, and worker threads are managed by the
framework rather than by Python.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import tempfile
import threading
from typing import Callable, Optional

from . import _ndnsf


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
    interest_lifetime_ms: int = 1000,
    forwarding_hints: Optional[list[str]] = None,
) -> list[DataPacket]:
    """Fetch segmented NDN Data and return the original Data wire packets."""

    packets = _ndnsf.fetch_segmented_data_packets(
        base_name,
        int(timeout_ms),
        int(interest_lifetime_ms),
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
    interest_lifetime_ms: int = 1000,
    init_cwnd: float = 8.0,
    forwarding_hints: Optional[list[str]] = None,
) -> bytes:
    """Fetch signed segmented NDN Data with ndn-cxx SegmentFetcher."""

    return bytes(_ndnsf.fetch_segmented_object(
        base_name,
        int(timeout_ms),
        int(interest_lifetime_ms),
        float(init_cwnd),
        list(forwarding_hints or []),
    ))


def fetch_segmented_object_with_segment_hints(
    base_name: str,
    *,
    timeout_ms: int = 30000,
    interest_lifetime_ms: int = 1000,
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
        int(interest_lifetime_ms),
        native_ranges,
    ))


def fetch_known_segmented_object_with_segment_hints(
    versioned_name: str,
    segment_count: int,
    *,
    timeout_ms: int = 30000,
    interest_lifetime_ms: int = 1000,
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
        int(interest_lifetime_ms),
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


@dataclass(frozen=True)
class CollaborationData:
    session_id: str
    key_scope: str
    topic: str
    producer: str
    producer_role: str
    sequence: int
    payload: bytes


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
        return CollaborationAssignment(
            role=str(native.role),
            service=str(native.service),
            assigned_artifact=str(native.assigned_artifact),
            artifact_data_name=str(native.artifact_data_name),
            requires_provisioning=bool(native.requires_provisioning),
            provisioning_timeout_ms=int(native.provisioning_timeout_ms),
            assignment_payload=bytes(native.assignment_payload),
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
                payload = cached_payload
            elif artifact.chunks:
                parts = []
                for index, chunk_name in enumerate(artifact.chunks):
                    part = self.fetch_encrypted_large_data(chunk_name, assignment.service)
                    if part is None:
                        raise RuntimeError(
                            f"failed to fetch execution artifact {artifact.name} chunk {index}")
                    parts.append(part)
                payload = b"".join(parts)
            else:
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


def _safe_file_token(value: str) -> str:
    token = "".join(
        ch if ch.isalnum() or ch in ("-", "_", ".") else "-"
        for ch in value
    ).strip("-")
    return token or "artifact"


def _artifact_cache_path(artifact: ExecutionArtifact) -> Optional[Path]:
    if not artifact.cache_name:
        return None
    filename = Path(artifact.filename).name
    cache_dir = (Path.home() / ".cache" / "ndnsf" / "artifacts" /
                 f"{_safe_file_token(artifact.cache_name)}-{artifact.sha256[:16]}")
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
    ) -> ServiceResponse:
        """Run a generic multi-provider collaboration.

        Provider ACK payloads should include ``role=<role>;``. The built-in
        Python selector assigns each requested role to the first successful ACK
        advertising that role, then sends per-role assignment metadata with
        artifact Data names and scope-key Data names.
        """

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
        )
        return _from_native_response(response)

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

    def pump(self, milliseconds: int) -> None:
        self._native.pump(milliseconds)

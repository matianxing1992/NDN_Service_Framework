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
import os
import threading
from typing import Any, Callable, Optional

from . import _ndnsf
from .runtime_telemetry import (
    ProviderCapabilityHint,
    parse_ack_metadata,
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


@dataclass(frozen=True)
class AllowedService:
    """A service permission entry visible to a Python NDNSF user.

    provider_service is the full permission namespace, typically
    /<provider>/<service>. service is the unified service name applications pass
    to request_service(), such as /HELLO. policy_epoch identifies the
    controller policy snapshot that authorized this record.
    """

    provider_service: str
    service: str
    policy_epoch: int = 0


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
        forwarding_route_prefixes: Optional[list[str]] = None,
    ) -> None:
        self._native = _ndnsf.StoredDataProducer(
            base_name,
            [bytes(packet) for packet in packet_wires],
            signing_identity,
            list(forwarding_route_prefixes or []),
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


def decode_data_packet(wire: bytes) -> DataPacket:
    """Decode one immutable NDN Data wire packet without rewriting it."""

    packet = _ndnsf.decode_data_packet(bytes(wire))
    return DataPacket(str(packet.name), int(packet.segment), bytes(packet.wire))


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


def fetch_exact_data_packet(
    data_name: str,
    *,
    timeout_ms: int = 30000,
    interest_lifetime_ms: Optional[int] = None,
    forwarding_hints: Optional[list[str]] = None,
) -> DataPacket:
    """Fetch one stored Data packet by its complete exact name."""

    packet = _ndnsf.fetch_exact_data_packet(
        data_name,
        int(timeout_ms),
        int(interest_lifetime_ms or default_large_data_interest_lifetime_ms()),
        list(forwarding_hints or []),
    )
    return DataPacket(str(packet.name), int(packet.segment), bytes(packet.wire))


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
        self._context_handlers: set[str] = set()
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

    def add_context_handler(
        self,
        service: str,
        handler: Callable[[dict[str, str], bytes], bytes | ServiceResponse],
    ) -> None:
        """Register a handler that receives authenticated invocation context."""

        self._handlers[service] = handler
        self._context_handlers.add(service)

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

        include_context = service in self._context_handlers

        def request_handler(*args):
            result = self._handlers[service](*args)
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

        self._native.add_service(
            service, request_handler, ack_handler, include_context)

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
            if not self._handlers:
                raise ValueError("at least one service handler must be registered")
            for registered_service in self._handlers:
                self._register_service(registered_service)
            self._native.run()
            return 0
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

    def request_service_targeted(
        self,
        provider: str,
        service: str,
        payload: bytes,
        *,
        timeout_ms: int = 5000,
    ) -> ServiceResponse:
        """Invoke a known provider through NDNSF's authenticated Targeted path."""

        response = self._native.request_service_targeted(
            provider,
            service,
            bytes(payload),
            timeout_ms=timeout_ms,
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

    def request_service_targeted_async(
        self,
        provider: str,
        service: str,
        payload: bytes,
        *,
        on_response: Callable[[ServiceResponse], None],
        on_timeout: Callable[[str], None],
        timeout_ms: int = 5000,
    ) -> None:
        """Submit a known-provider Targeted request and return immediately."""

        self._native.request_service_targeted_async(
            provider,
            service,
            bytes(payload),
            lambda response: on_response(_from_native_response(response)),
            on_timeout,
            timeout_ms=timeout_ms,
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
    ) -> ServiceResponse:
        """Run a generic multi-provider collaboration.

        Provider ACK payloads should include ``role=<role>;``. The built-in
        Python selector assigns each requested role to the first successful ACK
        advertising that role, then sends per-role assignment metadata with
        artifact Data names and scope-key Data names.
        ``ack_observer`` receives the ACK candidates collected for the
        collaboration request before the built-in role selector chooses
        providers. It is observational only and must not return a value.
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
                policy_epoch=int(policy_epoch),
            )
            for provider_service, service, policy_epoch in self._native.get_allowed_services()
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

    def pump(self, milliseconds: int) -> None:
        self._native.pump(milliseconds)

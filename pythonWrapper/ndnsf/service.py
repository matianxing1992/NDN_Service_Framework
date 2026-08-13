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
import time
from types import MappingProxyType
from typing import Any, Callable, Mapping, Optional, Union

from . import _ndnsf
from .runtime_telemetry import (
    CollaborationSelectionStatus,
    ProviderCapabilityHint,
    ServiceOperationStatus,
    parse_ack_metadata,
    to_plain,
)
from .service_discovery import ServiceDiscoveryRecord


class CollaborationDeadlineExceeded(TimeoutError):
    """Distinct terminal reason for a collaboration status watch."""

    def __init__(self, reason: str):
        if reason not in {"STALLED", "HARD_TIMEOUT"}:
            raise ValueError("invalid collaboration deadline reason")
        self.reason = reason
        super().__init__(reason)

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


def _native_deployment_intent(
    value: Optional[Union[Mapping[str, str], _ndnsf.NativeDeploymentIntent]],
):
    """Normalize the additive deployment intent without changing legacy calls."""
    if value is None or isinstance(value, _ndnsf.NativeDeploymentIntent):
        return value
    native = _ndnsf.NativeDeploymentIntent()
    for key, field_value in value.items():
        native.set_field(str(key), str(field_value))
    return native


def _native_request_capabilities(
    value: Optional[Union[Mapping[str, str], _ndnsf.NativeRequestCapabilities]],
):
    if value is None or isinstance(value, _ndnsf.NativeRequestCapabilities):
        return value
    native = _ndnsf.NativeRequestCapabilities()
    for key, field_value in value.items():
        native.set_field(str(key), str(field_value))
    return native


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
    request_id: str = ""
    data_name: str = ""
    signer_certificate: str = ""
    wire_digest: str = ""


@dataclass(frozen=True)
class ProviderSigningMetadata:
    """Public Provider signing names; no private key material is exposed."""

    provider_identity: str
    signing_key_name: str
    signing_certificate_name: str


@dataclass(frozen=True)
class AckDecision:
    status: bool = True
    payload: bytes = b""
    message: str = "ok"
    suppress: bool = False
    reservation_lease: Mapping[str, str] = field(default_factory=dict)
    selection_input_key_offer: Mapping[str, str] = field(default_factory=dict)
    pending_state_ttl_ms: int = 0


@dataclass(frozen=True)
class AckCandidate:
    provider_name: str
    service_name: str
    request_id: str
    status: bool
    message: str = ""
    payload: bytes = b""
    telemetry: Optional[Mapping[str, Any]] = None


@dataclass(frozen=True)
class CollaborationAckClosed:
    request_id: str
    candidates: tuple[AckCandidate, ...]
    digest: str
    closed_at_us: int
    request_deadline_us: int

    def __post_init__(self) -> None:
        if (not self.request_id or not self.digest.startswith("sha256:")
                or len(self.digest) != 71 or self.closed_at_us <= 0
                or self.request_deadline_us <= self.closed_at_us):
            raise ValueError("invalid immutable ACK_CLOSED snapshot")
        object.__setattr__(self, "candidates", tuple(self.candidates))


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
class SignedAppDataResult:
    """Result of exact-name, trust-schema-validated APP Data I/O."""

    success: bool
    data_name: str = ""
    signer_certificate: str = ""
    payload: bytes = b""
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


class FileSegmentedObjectProducer:
    """Serve a file as on-demand signed segments with bounded producer memory."""

    def __init__(
        self,
        base_name: str,
        file_path: str,
        *,
        signing_identity: str = "",
        max_segment_size: int = 6000,
        freshness_ms: int = 60000,
        digest_signing: bool = True,
    ) -> None:
        self._native = _ndnsf.FileSegmentedObjectProducer(
            base_name,
            str(file_path),
            signing_identity,
            int(max_segment_size),
            int(freshness_ms),
            bool(digest_signing),
        )

    def start(self) -> "FileSegmentedObjectProducer":
        self._native.start()
        return self

    def stop(self) -> None:
        self._native.stop()

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
    def file_size(self) -> int:
        return int(self._native.file_size)

    @property
    def data_count(self) -> int:
        return int(self._native.data_count)

    @property
    def wire_bytes(self) -> int:
        return int(self._native.wire_bytes)

    @property
    def signing_ms(self) -> float:
        return float(self._native.signing_ms)

    @property
    def public_key_der(self) -> bytes:
        return bytes(self._native.public_key_der)

    @property
    def error(self) -> str:
        return str(self._native.error)


@dataclass(frozen=True)
class DataPacket:
    """One immutable NDN Data packet encoded in wire format."""

    name: str
    segment: int
    wire: bytes
    content: bytes = b""


def verify_data_packet_signature(wire: bytes, public_key_der: bytes) -> bool:
    """Verify one RSA/ECDSA signed Data packet against a DER public key."""

    return bool(_ndnsf.verify_data_packet_signature(bytes(wire), bytes(public_key_der)))


def verify_detached_sha256_signature(
    payload: bytes, signature: bytes, public_key_der: bytes
) -> bool:
    """Verify one detached RSA/ECDSA SHA-256 signature in-process."""

    return bool(_ndnsf.verify_detached_sha256_signature(
        bytes(payload), bytes(signature), bytes(public_key_der)
    ))


def verify_data_packet_digest(wire: bytes) -> bool:
    """Verify one self-verifying DigestSha256 Data packet."""

    return bool(_ndnsf.verify_data_packet_digest(bytes(wire)))


@dataclass(frozen=True)
class AdaptiveSegmentFetchResult:
    """Metrics from callback-streamed adaptive segmented delivery."""

    total_segments: int
    delivered_segments: int
    interest_count: int
    retransmission_count: int
    duplicate_count: int
    timeout_count: int
    logical_bytes: int
    data_wire_bytes: int
    interest_wire_bytes: int
    wire_bytes: int
    retransmitted_bytes: int
    maximum_in_flight: int
    final_window: float


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
    return DataPacket(
        str(packet.name),
        int(packet.segment),
        bytes(packet.wire),
        bytes(packet.content),
    )


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
        DataPacket(
            str(packet.name),
            int(packet.segment),
            bytes(packet.wire),
            bytes(packet.content),
        )
        for packet in packets
    ]


def make_signed_data(
    name: str,
    content: bytes,
    *,
    signing_identity: str = "",
    freshness_ms: int = 300,
) -> bytes:
    """Make a signed exact-name Data packet for tests and convenience only.

    Production stream applications should sign with their own NDN keychain
    (for example ``python-ndn``) and pass that exact wire to
    :meth:`StreamPublisher.push`. This helper creates or uses an ndn-cxx
    identity in the process keychain and is not the Provider-bound production
    signing path.
    """

    return bytes(_ndnsf.make_signed_data(
        str(name), bytes(content), str(signing_identity), int(freshness_ms)))


def make_predictive_data_name(
    mapping_root: str, mapping_version: int, sequence: int
) -> str:
    """Return the canonical predictive source Data name URI."""

    return str(_ndnsf.make_predictive_data_name(
        str(mapping_root), int(mapping_version), int(sequence)))


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
        DataPacket(
            str(packet.name),
            int(packet.segment),
            bytes(packet.wire),
            bytes(packet.content),
        )
        for packet in packets
    ]


def fetch_adaptive_segmented_data_packets(
    base_name: str,
    on_packet: Callable[[DataPacket], None],
    *,
    timeout_ms: int = 30000,
    interest_lifetime_ms: Optional[int] = None,
    initial_window: int = 4,
    maximum_window: int = 64,
    maximum_retries: int = 5,
    persistence_backlog_limit: int = 16,
    forwarding_hints: Optional[list[str]] = None,
) -> AdaptiveSegmentFetchResult:
    """Stream adaptive NDN segments into a synchronous verifier/persistence sink.

    ``on_packet`` must return only after the packet is verified and persisted.
    Its execution is therefore the backpressure boundary; the native fetcher
    never assembles the full artifact in memory.
    """

    def deliver(native_packet) -> None:
        on_packet(DataPacket(
            str(native_packet.name),
            int(native_packet.segment),
            bytes(native_packet.wire),
            bytes(native_packet.content),
        ))

    result = _ndnsf.fetch_adaptive_segmented_data_packets(
        base_name,
        int(timeout_ms),
        int(interest_lifetime_ms or default_large_data_interest_lifetime_ms()),
        int(initial_window),
        int(maximum_window),
        int(maximum_retries),
        int(persistence_backlog_limit),
        list(forwarding_hints or []),
        deliver,
    )
    return AdaptiveSegmentFetchResult(
        total_segments=int(result.total_segments),
        delivered_segments=int(result.delivered_segments),
        interest_count=int(result.interest_count),
        retransmission_count=int(result.retransmission_count),
        duplicate_count=int(result.duplicate_count),
        timeout_count=int(result.timeout_count),
        logical_bytes=int(result.logical_bytes),
        data_wire_bytes=int(result.data_wire_bytes),
        interest_wire_bytes=int(result.interest_wire_bytes),
        wire_bytes=int(result.wire_bytes),
        retransmitted_bytes=int(result.retransmitted_bytes),
        maximum_in_flight=int(result.maximum_in_flight),
        final_window=float(result.final_window),
    )


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
    return DataPacket(
        str(packet.name),
        int(packet.segment),
        bytes(packet.wire),
        bytes(packet.content),
    )


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
    assignment_payload: bytes = b""
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
    selection_digest: str = ""
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
            selection_digest=str(native.selection_digest),
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

    def allow_data(self, key_scope: str, topic_prefix: str) -> None:
        """Allow one request-scoped collaboration binding before decryption.

        Providers receive a broad SVS collaboration subscription.  Registering
        the local role's scope/topic bindings lets the native layer discard
        other roles' ciphertext before trying authentication with a local key.
        """
        self._native.allow_data(key_scope, topic_prefix)

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
        if not data_name:
            raise RuntimeError(
                "large collaboration object publication returned no Data name")
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

    def report_operation_status(self, status: ServiceOperationStatus) -> None:
        """Report the latest observation through signed SELECTION-STATUS.

        This is generic progress only. Applications still own any exact
        readiness or completion certificate needed to authorize execution.
        """
        if not isinstance(status, ServiceOperationStatus):
            raise TypeError("status must be ServiceOperationStatus")
        payload = to_plain(status)
        payload["details_payload"] = bytes(status.details_payload)
        self._native.report_operation_status(payload)

    def publish_final_response(self, payload: bytes) -> None:
        self._native.publish_final_response(bytes(payload))


def _to_native_response(response: ServiceResponse) -> _ndnsf.ServiceResponse:
    native = _ndnsf.ServiceResponse()
    native.status = response.status
    native.payload = response.payload
    native.error = response.error
    native.request_id = response.request_id
    native.data_name = response.data_name
    native.signer_certificate = response.signer_certificate
    native.wire_digest = response.wire_digest
    return native


def _from_native_response(response: _ndnsf.ServiceResponse) -> ServiceResponse:
    return ServiceResponse(
        status=bool(response.status),
        payload=bytes(response.payload),
        error=str(response.error),
        request_id=str(response.request_id),
        data_name=str(response.data_name),
        signer_certificate=str(response.signer_certificate),
        wire_digest=str(response.wire_digest),
    )


def _to_native_ack(decision: AckDecision) -> _ndnsf.AckDecision:
    if int(decision.pending_state_ttl_ms) < 0:
        raise ValueError("pending_state_ttl_ms must be non-negative")
    native = _ndnsf.AckDecision()
    native.status = decision.status
    native.payload = decision.payload
    native.message = decision.message
    native.suppress = decision.suppress
    native.reservation_lease = dict(decision.reservation_lease)
    native.selection_input_key_offer = dict(decision.selection_input_key_offer)
    native.pending_state_ttl_ms = int(decision.pending_state_ttl_ms)
    return native


def _invoke_ack_handler_safely(
    service: str,
    handler: Callable[..., bool | AckDecision],
    args: tuple[Any, ...],
) -> bool | AckDecision:
    """Keep Python ACK failures observable without exposing request payloads.

    The native binding treats an escaping Python exception as a suppressed ACK.
    That preserves safety but makes a provider-side bug indistinguishable from
    packet loss at the requester. Convert ordinary application exceptions into
    a fail-closed, non-suppressed negative ACK and emit a bounded local marker.
    """
    try:
        return handler(*args)
    except Exception as exc:
        detail = " ".join(str(exc).split())[:240]
        print(
            "NDNSF_ACK_HANDLER_EXCEPTION "
            f"service={service} errorType={type(exc).__name__} "
            f"detail={detail or '-'}",
            flush=True,
        )
        return AckDecision(
            status=False,
            message=NEGATIVE_ACK_REASON_INTERNAL_ERROR,
            suppress=False,
        )


def _from_native_large_data_result(result) -> LargeDataPublishResult:
    return LargeDataPublishResult(
        success=bool(result.success),
        encrypted_data_name=str(result.encrypted_data_name),
        object_id=str(result.object_id),
        error=str(result.error),
    )


def _from_native_signed_app_data_result(result) -> SignedAppDataResult:
    return SignedAppDataResult(
        success=bool(result.success),
        data_name=str(result.data_name),
        signer_certificate=str(result.signer_certificate),
        payload=bytes(result.payload),
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
            "assignment_payload": role.assignment_payload,
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


def _freeze_collaboration_value(value: Any) -> Any:
    if isinstance(value, Mapping):
        return MappingProxyType({
            str(key): _freeze_collaboration_value(item)
            for key, item in value.items()
        })
    if isinstance(value, (tuple, list)):
        return tuple(_freeze_collaboration_value(item) for item in value)
    return value


def _from_native_ack_candidate(candidate) -> AckCandidate:
    telemetry = None
    if candidate.telemetry is not None:
        telemetry = _freeze_collaboration_value(dict(candidate.telemetry))
    return AckCandidate(
        provider_name=str(candidate.provider_name),
        service_name=str(candidate.service_name),
        request_id=str(candidate.request_id),
        status=bool(candidate.status),
        message=str(candidate.message),
        payload=bytes(candidate.payload),
        telemetry=telemetry,
    )


class _CollaborationInvocationState:
    def __init__(self) -> None:
        self.condition = threading.Condition()
        self.ack_closed: Optional[CollaborationAckClosed] = None
        self.response: Optional[ServiceResponse] = None
        self.timeout_reason = ""

    def set_ack_closed(self, value: CollaborationAckClosed) -> None:
        with self.condition:
            if self.ack_closed is not None and self.ack_closed != value:
                raise RuntimeError("conflicting ACK_CLOSED callback")
            self.ack_closed = value
            self.condition.notify_all()

    def set_response(self, value: ServiceResponse) -> None:
        with self.condition:
            if self.response is None:
                self.response = value
            self.condition.notify_all()

    def set_timeout(self, reason: str) -> None:
        with self.condition:
            if not self.timeout_reason:
                self.timeout_reason = str(reason)
            self.condition.notify_all()


class CollaborationInvocation:
    """One durable generic collaboration request with deferred plan commit."""

    _TERMINAL_SELECTION_STATES = frozenset({
        "REJECTED", "FAILED", "EXPIRED", "CANCELLED", "ABORTED",
    })

    def __init__(
        self,
        *,
        native,
        state: _CollaborationInvocationState,
        service: str,
        request_id: str,
        ack_timeout_ms: int,
        timeout_ms: int,
        fail_fast_terminal_selection: bool = False,
    ) -> None:
        self._native = native
        self._state = state
        self.service = str(service)
        self.request_id = str(request_id)
        self.ack_timeout_ms = int(ack_timeout_ms)
        self.timeout_ms = int(timeout_ms)
        # Generic collaborations include DistributedRepo control operations;
        # terminal Selection polling is opt-in for DI request handles only.
        self.fail_fast_terminal_selection = bool(fail_fast_terminal_selection)

    def acks_closed(self, timeout_ms: Optional[int] = None) -> CollaborationAckClosed:
        wait_ms = self.timeout_ms if timeout_ms is None else int(timeout_ms)
        with self._state.condition:
            ready = self._state.condition.wait_for(
                lambda: (
                    self._state.ack_closed is not None
                    or bool(self._state.timeout_reason)
                    or self._state.response is not None
                ),
                max(wait_ms, 0) / 1000.0,
            )
            if not ready:
                raise TimeoutError("local wait for ACK_CLOSED expired")
            if self._state.ack_closed is None:
                raise TimeoutError(
                    self._state.timeout_reason
                    or "collaboration ended before ACK_CLOSED"
                )
            return self._state.ack_closed

    def commit_plan(
        self,
        *,
        ack_closed_digest: str,
        roles: list[CollaborationRole | dict],
        key_scopes: dict[str, list[str]],
        dependencies: Optional[list[CollaborationDependency | dict]] = None,
        artifact_data_names: Optional[dict[str, str]] = None,
        scope_key_data_names: Optional[dict[str, str]] = None,
        role_scopes: Optional[dict[str, list[str]]] = None,
        role_provider_assignments: Optional[dict[str, str]] = None,
        assignment_payloads_by_role: Optional[Mapping[str, bytes]] = None,
    ) -> bool:
        closed = self.acks_closed()
        if ack_closed_digest != closed.digest:
            raise ValueError("commit does not bind this ACK_CLOSED snapshot")
        role_values = []
        assignment_payloads = dict(assignment_payloads_by_role or {})
        for role in roles:
            value = _role_to_dict(role)
            role_name = str(value.get("role", ""))
            if role_name in assignment_payloads:
                value["assignment_payload"] = bytes(
                    assignment_payloads[role_name])
            role_values.append(value)
        return bool(self._native.commit_collaboration_plan(
            self.service,
            self.request_id,
            ack_closed_digest,
            role_values,
            {str(scope): list(scope_roles)
             for scope, scope_roles in key_scopes.items()},
            [_dependency_to_dict(dep) for dep in (dependencies or [])],
            dict(artifact_data_names or {}),
            dict(scope_key_data_names or {}),
            {str(role): list(scopes)
             for role, scopes in (role_scopes or {}).items()},
            self.ack_timeout_ms,
            self.timeout_ms,
            dict(role_provider_assignments or {}),
        ))

    def result(self, timeout_ms: Optional[int] = None) -> ServiceResponse:
        wait_ms = self.timeout_ms if timeout_ms is None else int(timeout_ms)
        if wait_ms <= 0:
            raise TimeoutError("local wait for collaboration Response expired")
        deadline = time.monotonic() + wait_ms / 1000.0
        while True:
            with self._state.condition:
                if self._state.response is not None:
                    return self._state.response
                if self._state.timeout_reason:
                    raise TimeoutError(self._state.timeout_reason)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(
                        "local wait for collaboration Response expired")

            terminal_reason = self._terminal_selection_failure()
            if terminal_reason:
                self._state.set_timeout(terminal_reason)
                raise TimeoutError(terminal_reason)

            with self._state.condition:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(
                        "local wait for collaboration Response expired")
                self._state.condition.wait(timeout=min(0.25, remaining))

    def _terminal_selection_failure(self) -> str:
        """Return a fail-fast reason when every observed selection is terminal.

        Selection rejection is a durable protocol outcome, not transport
        silence.  Polling the generic request snapshot keeps this behavior in
        the framework collaboration handle while retaining the conservative
        rule that one rejected Provider does not abort other live Providers.
        """

        if not self.fail_fast_terminal_selection:
            return ""
        getter = getattr(self._native, "get_collaboration_status_snapshot", None)
        if getter is None:
            return ""
        try:
            snapshots = getter(self.request_id, 250)
        except (AttributeError, RuntimeError, TypeError, ValueError):
            return ""
        if not snapshots:
            return ""
        states = []
        reasons = []
        for raw in snapshots:
            if not isinstance(raw, Mapping):
                return ""
            state = str(raw.get("state", "")).strip().upper()
            if not state:
                return ""
            states.append(state)
            message = str(raw.get("message", "")).strip()
            if message:
                reasons.append(message)
        if not states or not all(
                state in self._TERMINAL_SELECTION_STATES for state in states):
            return ""
        if not any(state == "REJECTED" for state in states):
            return ""
        detail = "; ".join(dict.fromkeys(reasons))
        return "selection reached terminal rejection" + (
            f": {detail}" if detail else "")


class ServiceProvider:
    """Python API for writing NDNSF provider business logic."""

    @property
    def provider_identity(self) -> str:
        """Return the Provider identity used by the native runtime."""

        return str(self._native.provider_identity)

    @property
    def provider_signing_key_name(self) -> str:
        """Return the public NDN key name used for Provider-signed Data."""

        return str(self._native.provider_signing_key_name)

    @property
    def provider_signing_certificate_name(self) -> str:
        """Return the public certificate name used for Provider-signed Data."""

        return str(self._native.provider_signing_certificate_name)

    @property
    def signing_metadata(self) -> ProviderSigningMetadata:
        """Return public signing metadata for an external application signer."""

        return ProviderSigningMetadata(
            provider_identity=self.provider_identity,
            signing_key_name=self.provider_signing_key_name,
            signing_certificate_name=self.provider_signing_certificate_name,
        )

    @property
    def provider_boot_epoch(self) -> str:
        """Return Core's stable process-incarnation fence."""

        return str(self._native.provider_boot_epoch)

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
        self._ack_context_handlers: set[str] = set()
        self._collaboration_services: set[str] = set()

    def create_live_stream(self, definition):
        """Create the Core-owned publisher; application supplies opaque bytes only."""
        from .streaming import LiveStreamDefinition, LiveStreamPublisher

        if not isinstance(definition, LiveStreamDefinition):
            raise TypeError("definition must be LiveStreamDefinition")
        return LiveStreamPublisher(
            self._native.create_live_stream(definition._to_native()))

    def create_stream(self, config):
        """Create the high-level stream facade with Core-owned defaults."""
        from .streaming import StreamConfig, StreamPublisher

        if not isinstance(config, StreamConfig):
            raise TypeError("config must be StreamConfig")
        return StreamPublisher(
            self._native.create_stream(config._to_native()))

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
        self._ack_context_handlers.discard(service)

    def set_ack_context_handler(
        self,
        service: str,
        handler: Callable[[Mapping[str, Any], bytes], bool | AckDecision],
    ) -> None:
        """Register an ACK policy with authenticated Request metadata.

        The context exposes negotiated capabilities and opaque encrypted input;
        it never exposes decrypted application input.
        """
        self._ack_handlers[service] = handler
        self._ack_context_handlers.add(service)

    def ack_handler(self, service: str):
        def decorator(fn: Callable[[bytes], bool | AckDecision]):
            self.set_ack_handler(service, fn)
            return fn
        return decorator

    def configure_opaque_selection_store(
        self,
        *,
        wal_path: str,
        storage_key: bytes,
        storage_key_epoch: str,
        max_prepare_ms: int = 1000,
    ) -> None:
        """Configure Core's encrypted durable Selection transaction journal.

        ``storage_key`` must be an application-supplied 32-byte secret. Core
        never derives it from a token or writes it into the journal. The
        participant must be registered only after this store is configured.
        """
        key = bytes(storage_key)
        if len(key) != 32:
            raise ValueError("storage_key must contain exactly 32 bytes")
        if not wal_path or not storage_key_epoch:
            raise ValueError("wal_path and storage_key_epoch are required")
        if max_prepare_ms <= 0:
            raise ValueError("max_prepare_ms must be positive")
        self._native.configure_opaque_selection_store(
            str(wal_path), key, str(storage_key_epoch), int(max_prepare_ms),
        )

    def register_opaque_selection_participant(
        self,
        service: str,
        *,
        participant_id: str,
        participant_version: int,
        prepare: Callable[[Mapping[str, Any], bytes], Mapping[str, bytes]],
        on_committed: Callable[[Mapping[str, Any]], None],
        on_aborted: Callable[[str, str], None],
    ) -> None:
        """Register service-owned policy behind Core's generic Selection seam.

        ``prepare`` receives an immutable authenticated context and the exact
        opaque Selection payload. It must be side-effect free and return only
        ``commit_blob`` and ``acceptance_payload`` bytes. Core bounds its
        execution, computes both digests, fsyncs the encrypted commit record,
        and only then calls ``on_committed``. This API is model-agnostic.
        """
        if not service or not participant_id or participant_version <= 0:
            raise ValueError(
                "service, participant_id, and positive participant_version "
                "are required"
            )

        def native_prepare(context, payload):
            result = prepare(MappingProxyType(dict(context)), bytes(payload))
            if not isinstance(result, Mapping):
                raise TypeError("prepare must return a mapping")
            if set(result) != {"commit_blob", "acceptance_payload"}:
                raise ValueError(
                    "prepare result must contain exactly commit_blob and "
                    "acceptance_payload"
                )
            return {
                "commit_blob": bytes(result["commit_blob"]),
                "acceptance_payload": bytes(result["acceptance_payload"]),
            }

        def native_committed(view):
            on_committed(MappingProxyType(dict(view)))

        self._native.register_opaque_selection_participant(
            service,
            participant_id,
            int(participant_version),
            native_prepare,
            native_committed,
            on_aborted,
        )

    def set_deployment_prepare_handler(
        self, handler: Callable[[dict[str, Any]], Mapping[str, str]],
    ) -> None:
        """Register generic verify/load/warm preparation after Selection.

        The callback returns readiness-specific fields such as
        ``artifactDigest``, ``deploymentInstanceId``, and ``operationId``.
        Core overwrites all authority and identity bindings before sending the
        signed ProviderReadyMessage.
        """
        def native_handler(context):
            return {str(key): str(value)
                    for key, value in handler(dict(context)).items()}

        self._native.set_deployment_prepare_handler(native_handler)

    def set_r1_selection_decision_handler(
        self, service: str,
        handler: Callable[[Mapping[str, str]], Mapping[str, str]],
    ) -> None:
        """Apply a Core-authenticated R1 decision in application policy.

        The handler owns reservation commit/release semantics and must return
        a receipt bound to the supplied decision digest and reservation ID.
        """
        def native_handler(decision):
            return {str(key): str(value)
                    for key, value in handler(dict(decision)).items()}

        self._native.set_r1_selection_decision_handler(service, native_handler)

    def set_r1_reservation_terminal_handler(
        self, service: str, handler: Callable[[str, str], None],
    ) -> None:
        """Release a DI-owned committed reservation on local termination."""
        self._native.set_r1_reservation_terminal_handler(service, handler)

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
            def ack_handler(*args):
                result = _invoke_ack_handler_safely(
                    service, self._ack_handlers[service], args)
                if isinstance(result, AckDecision):
                    return _to_native_ack(result)
                return bool(result)

        self._native.add_service(
            service, request_handler, ack_handler, include_context,
            service in self._ack_context_handlers)

    def add_collaboration_handler(
        self,
        service: str,
        allowed_roles: list[str],
        handler: Callable[[CollaborationContext, bytes], None],
        ack_handler: Optional[Callable[[bytes], bool | AckDecision]] = None,
        *,
        include_ack_context: bool = False,
    ) -> None:
        def request_handler(native_ctx, payload: bytes):
            handler(CollaborationContext(native_ctx), bytes(payload))

        native_ack = None
        if ack_handler is not None:
            def native_ack(*args):
                result = _invoke_ack_handler_safely(service, ack_handler, args)
                if isinstance(result, AckDecision):
                    return _to_native_ack(result)
                return bool(result)

        self._native.add_collaboration_service(
            service,
            list(allowed_roles),
            request_handler,
            native_ack,
            include_ack_context,
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

    def open_live_stream(
        self,
        descriptor,
        *,
        start: str = "latest",
        prefetch_policy: str | None = None,
        aggregate_interest_limit: int = 64,
        enable_fec_recovery: bool = False,
        interest_lifetime_ms: int = 500,
        on_item,
        on_status=None,
    ):
        """Open semantic-name prefetch; callback receives validated opaque bytes."""
        from .streaming import (
            LiveStreamConsumerHandle,
            LiveStreamDescriptor,
            LiveStreamItemAdmission,
            VerifiedLiveStreamItem,
        )

        if not isinstance(descriptor, LiveStreamDescriptor):
            raise TypeError("descriptor must be LiveStreamDescriptor")
        if prefetch_policy is None:
            prefetch_policy = ("adaptive-sample-atomic"
                               if descriptor.contract_version == 2
                               else "mapped-pressure")

        def item_adapter(native_item):
            result = on_item(VerifiedLiveStreamItem._from_native(native_item))
            if isinstance(result, LiveStreamItemAdmission):
                return result._native
            return bool(result)

        native_status = None
        if on_status is not None:
            def native_status(value):
                on_status(value)

        native = self._native.open_live_stream(
            descriptor._native,
            item_adapter,
            start=str(start),
            prefetch_policy=str(prefetch_policy),
            aggregate_interest_limit=int(aggregate_interest_limit),
            enable_fec_recovery=bool(enable_fec_recovery),
            interest_lifetime_ms=int(interest_lifetime_ms),
            on_status=native_status,
        )
        return LiveStreamConsumerHandle(native)

    def subscribe_stream(self, descriptor, options):
        """Open and start one high-level stream subscription."""
        from .streaming import (
            LiveStreamItemAdmission,
            PredictiveStreamDescriptor,
            PredictiveStreamSubscriber,
            StreamSubscriptionOptions,
            VerifiedLiveStreamItem,
        )

        if not isinstance(descriptor, PredictiveStreamDescriptor):
            raise TypeError("descriptor must be PredictiveStreamDescriptor")
        if not isinstance(options, StreamSubscriptionOptions):
            raise TypeError("options must be StreamSubscriptionOptions")
        if not callable(options.on_item):
            raise TypeError("options.on_item must be callable")

        def item_adapter(native_item):
            result = options.on_item(
                VerifiedLiveStreamItem._from_native(native_item))
            if isinstance(result, LiveStreamItemAdmission):
                return result._native
            return bool(result)

        native_status = None
        if options.on_status is not None:
            if not callable(options.on_status):
                raise TypeError("options.on_status must be callable")

            def native_status(value):
                options.on_status(value)

        native = self._native.subscribe_stream(
            descriptor._native,
            item_adapter,
            start=str(options.start),
            prefetch_policy=options.prefetch_policy,
            aggregate_interest_limit=int(options.aggregate_interest_limit),
            enable_fec_recovery=bool(options.enable_fec_recovery),
            require_full_delivery=bool(options.require_full_delivery),
            interest_lifetime_ms=int(options.interest_lifetime_ms),
            on_status=native_status,
        )
        return PredictiveStreamSubscriber(native)

    def request_service(
        self,
        service: str,
        payload: bytes,
        *,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 5000,
        strategy: str = "first-responding",
        request_id: str = "",
        deployment_intent: Optional[
            Union[Mapping[str, str], _ndnsf.NativeDeploymentIntent]
        ] = None,
        request_capabilities: Optional[
            Union[Mapping[str, str], _ndnsf.NativeRequestCapabilities]
        ] = None,
    ) -> ServiceResponse:
        response = self._native.request_service(
            service,
            bytes(payload),
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            strategy=strategy,
            request_id=request_id,
            deployment_intent=_native_deployment_intent(deployment_intent),
            request_capabilities=_native_request_capabilities(request_capabilities),
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
        """Invoke a known provider through NDNSF's authenticated Targeted path.

        ``timeout_ms`` is one total deadline beginning when the request is
        submitted, including local admission, token bootstrap, and publication.
        """

        response = self._native.request_service_targeted(
            provider,
            service,
            bytes(payload),
            timeout_ms=timeout_ms,
        )
        return _from_native_response(response)

    def query_collaboration_status(
        self, *, provider: str, service: str, selection_digest: str,
        timeout_ms: int = 500,
    ) -> Optional[CollaborationSelectionStatus]:
        value = self._native.query_collaboration_status(
            provider, service, selection_digest, timeout_ms)
        if value is None:
            return None
        snapshot = CollaborationSelectionStatus.from_dict(dict(value))
        if (snapshot.provider_name != provider or
                snapshot.service_name != service or
                snapshot.selection_digest != selection_digest):
            raise ValueError("collaboration status binding mismatch")
        return snapshot

    def collaboration_status(
        self, request_id: str, *, timeout_ms: int = 500,
    ) -> tuple[CollaborationSelectionStatus, ...]:
        """Return the latest validated per-Provider snapshots for a request."""
        values = self._native.get_collaboration_status_snapshot(
            request_id, timeout_ms)
        snapshots = tuple(CollaborationSelectionStatus.from_dict(dict(value))
                          for value in values)
        if any(item.request_id != request_id for item in snapshots):
            raise ValueError("collaboration request status binding mismatch")
        return snapshots

    def watch_collaboration_request(
        self, request_id: str, *, timeout_ms: int = 5000,
        query_interval_ms: int = 250,
        idle_timeout_ms: Optional[int] = None,
        hard_timeout_ms: Optional[int] = None,
        _clock: Callable[[], float] = time.monotonic,
        _sleep: Callable[[float], None] = time.sleep,
    ):
        idle_ms = timeout_ms if idle_timeout_ms is None else idle_timeout_ms
        hard_ms = timeout_ms if hard_timeout_ms is None else hard_timeout_ms
        if idle_ms <= 0 or hard_ms <= 0 or idle_ms > hard_ms:
            raise ValueError(
                "collaboration deadlines require 0 < idle <= hard")
        started = _clock()
        idle_budget = idle_ms / 1000.0
        idle_deadline = started + idle_budget
        hard_deadline = started + hard_ms / 1000.0
        versions = {}
        observations = {}
        while True:
            now = _clock()
            # Hard timeout wins when both boundaries are reached.
            if now >= hard_deadline:
                raise CollaborationDeadlineExceeded("HARD_TIMEOUT")
            if now >= idle_deadline:
                raise CollaborationDeadlineExceeded("STALLED")
            snapshots = self.collaboration_status(
                request_id,
                timeout_ms=min(
                    query_interval_ms,
                    max(1, int((hard_deadline - now) * 1000)),
                ),
            )
            changed = False
            advanced = False
            for snapshot in snapshots:
                for member in snapshot.member_statuses:
                    key = (snapshot.provider_name, member.role,
                           member.operation_id)
                    version = (member.attempt, member.epoch, member.sequence)
                    if key in versions and version < versions[key]:
                        raise ValueError("stale/replayed collaboration status")
                    if versions.get(key) != version:
                        versions[key] = version
                        changed = True
                        state = getattr(member.state, "value", str(member.state))
                        observation = (
                            state,
                            bool(member.progress_known),
                            float(member.progress),
                        )
                        previous = observations.get(key)
                        if (
                            previous is None
                            or state != previous[0]
                            or (
                                member.progress_known
                                and (
                                    not previous[1]
                                    or float(member.progress) > previous[2]
                                )
                            )
                        ):
                            advanced = True
                        observations[key] = observation
                    else:
                        state = getattr(member.state, "value", str(member.state))
                        current = (
                            state,
                            bool(member.progress_known),
                            float(member.progress),
                        )
                        if observations.get(key) != current:
                            raise ValueError(
                                "same-version collaboration status equivocation")
            if advanced:
                idle_deadline = min(_clock() + idle_budget, hard_deadline)
            if changed or (snapshots and not versions):
                yield snapshots
            now = _clock()
            _sleep(min(
                query_interval_ms / 1000.0,
                max(0.0, min(idle_deadline, hard_deadline) - now),
            ))

    def wait_collaboration_request(
        self, request_id: str, *, predicate,
        timeout_ms: int = 5000, query_interval_ms: int = 250,
        idle_timeout_ms: Optional[int] = None,
        hard_timeout_ms: Optional[int] = None,
    ) -> tuple[CollaborationSelectionStatus, ...]:
        for snapshots in self.watch_collaboration_request(
                request_id, timeout_ms=timeout_ms,
                query_interval_ms=query_interval_ms,
                idle_timeout_ms=idle_timeout_ms,
                hard_timeout_ms=hard_timeout_ms):
            if predicate(snapshots):
                return snapshots
        raise TimeoutError("collaboration request status was not satisfied")

    def watch_collaboration_status(
        self, *, provider: str, service: str, selection_digest: str,
        timeout_ms: int = 5000, query_interval_ms: int = 250,
    ):
        """Yield only monotonic member snapshots until the local watch ends."""
        if timeout_ms <= 0 or query_interval_ms <= 0:
            raise ValueError("watch timing must be positive")
        deadline = time.monotonic() + timeout_ms / 1000.0
        observed: dict[tuple[str, str], tuple[int, int, int]] = {}
        while time.monotonic() < deadline:
            remaining = max(1, int((deadline - time.monotonic()) * 1000))
            snapshot = self.query_collaboration_status(
                provider=provider, service=service,
                selection_digest=selection_digest,
                timeout_ms=min(remaining, query_interval_ms))
            if snapshot is not None:
                changed = not snapshot.member_statuses
                for member in snapshot.member_statuses:
                    key = (member.role, member.operation_id)
                    version = (member.attempt, member.epoch, member.sequence)
                    previous = observed.get(key)
                    if previous is not None and version < previous:
                        raise ValueError("stale/replayed collaboration status")
                    if previous != version:
                        observed[key] = version
                        changed = True
                if changed:
                    yield snapshot
            time.sleep(min(query_interval_ms / 1000.0,
                           max(0.0, deadline - time.monotonic())))

    def wait_collaboration_status(
        self, *, provider: str, service: str, selection_digest: str,
        predicate: Callable[[CollaborationSelectionStatus], bool],
        timeout_ms: int = 5000, query_interval_ms: int = 250,
    ) -> CollaborationSelectionStatus:
        for snapshot in self.watch_collaboration_status(
                provider=provider, service=service,
                selection_digest=selection_digest,
                timeout_ms=timeout_ms, query_interval_ms=query_interval_ms):
            if predicate(snapshot):
                return snapshot
        raise TimeoutError("collaboration status predicate was not satisfied")

    def request_service_select(
        self,
        service: str,
        payload: bytes,
        selector: Callable[[list[AckCandidate]], list[str]],
        *,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 5000,
        request_strategy: str = "first-responding",
        deployment_intent: Optional[
            Union[Mapping[str, str], _ndnsf.NativeDeploymentIntent]
        ] = None,
        request_capabilities: Optional[
            Union[Mapping[str, str], _ndnsf.NativeRequestCapabilities]
        ] = None,
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
            deployment_intent=_native_deployment_intent(deployment_intent),
            request_capabilities=_native_request_capabilities(request_capabilities),
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
        """Submit a known-provider Targeted request and return immediately.

        Exactly one of ``on_response`` or ``on_timeout`` is delivered. The
        total deadline includes admission, token bootstrap, and publication.
        """

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

    def publish_signed_app_data(
        self,
        data_name: str,
        payload: bytes,
        *,
        freshness_ms: int = 60000,
    ) -> SignedAppDataResult:
        """Publish signed APP Data at an exact name below this identity's DI prefix."""

        return _from_native_signed_app_data_result(
            self._native.publish_signed_app_data(
                data_name, bytes(payload), freshness_ms))

    def fetch_signed_app_data(
        self,
        data_name: str,
        expected_signer: str,
        *,
        timeout_ms: int = 5000,
    ) -> SignedAppDataResult:
        """Fetch one exact APP record and validate its trust and signer identity."""

        return _from_native_signed_app_data_result(
            self._native.fetch_signed_app_data(
                data_name, expected_signer, timeout_ms))

    def begin_collaboration(
        self,
        service: str,
        payload: bytes,
        *,
        mode: str = "DEFERRED",
        ack_timeout_ms: int = 300,
        timeout_ms: int = 10000,
        request_id: str = "",
        fail_fast_terminal_selection: bool = False,
        ack_coverage_predicate: Optional[
            Callable[[tuple[AckCandidate, ...]], bool]
        ] = None,
    ) -> CollaborationInvocation:
        """Publish one generic Request and defer plan choice until ACK_CLOSED.

        ``ack_coverage_predicate`` is an application-owned early-close
        optimization. It may only report that the observed ACK candidates
        provide enough capability coverage; it cannot select providers or
        mutate the immutable ACK_CLOSED/commit_plan lifecycle.
        """

        if str(mode).upper() != "DEFERRED":
            raise ValueError(
                "begin_collaboration supports DEFERRED; use "
                "request_collaboration for PREPLANNED compatibility"
            )
        if ack_timeout_ms <= 0 or timeout_ms <= ack_timeout_ms:
            raise ValueError("invalid collaboration ACK/request deadline")
        state = _CollaborationInvocationState()

        def on_ack_closed(native) -> None:
            state.set_ack_closed(CollaborationAckClosed(
                request_id=str(native.request_id),
                candidates=tuple(
                    _from_native_ack_candidate(candidate)
                    for candidate in native.candidates
                ),
                digest=str(native.digest),
                closed_at_us=int(native.closed_at_us),
                request_deadline_us=int(native.request_deadline_us),
            ))

        def on_response(native) -> None:
            state.set_response(_from_native_response(native))

        def on_timeout(reason: str) -> None:
            state.set_timeout(reason)

        native_ack_coverage = None
        if ack_coverage_predicate is not None:
            def native_ack_coverage(native_candidates) -> bool:
                candidates = tuple(
                    _from_native_ack_candidate(candidate)
                    for candidate in native_candidates
                )
                return bool(ack_coverage_predicate(candidates))

        native_args = (
            service, bytes(payload), on_ack_closed, on_response, on_timeout,
            ack_timeout_ms, timeout_ms, request_id,
        )
        if native_ack_coverage is None:
            actual_request_id = self._native.begin_collaboration(*native_args)
        else:
            try:
                actual_request_id = self._native.begin_collaboration(
                    *native_args,
                    ack_coverage_predicate=native_ack_coverage,
                )
            except TypeError as exc:
                # A sealed runtime image may contain the pre-early-close
                # native binding.  Keep the durable FULL invocation usable
                # without weakening the new binding: the callback is an
                # optimization, while ACK_CLOSED remains the authority.
                if "ack_coverage_predicate" not in str(exc):
                    raise
                actual_request_id = self._native.begin_collaboration(*native_args)
        return CollaborationInvocation(
            native=self._native,
            state=state,
            service=service,
            request_id=str(actual_request_id),
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            fail_fast_terminal_selection=fail_fast_terminal_selection,
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
        assignment_context: Optional[object] = None,
        request_id: str = "",
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

        role_provider_assignments = (
            assignment_context.providers_by_role()
            if assignment_context is not None else {}
        )
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
            role_provider_assignments,
            request_id,
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
        assignment_context: Optional[object] = None,
        request_id: str = "",
    ) -> None:
        """Submit a generic multi-provider collaboration without blocking."""

        role_provider_assignments = (
            assignment_context.providers_by_role()
            if assignment_context is not None else {}
        )
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
            role_provider_assignments,
            request_id,
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

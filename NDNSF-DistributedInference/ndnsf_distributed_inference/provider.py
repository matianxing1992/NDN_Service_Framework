"""Provider-side high-level API for distributed inference over NDNSF."""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from concurrent.futures import Future, ThreadPoolExecutor
import base64
import json
import os
from pathlib import Path
import tempfile
from threading import RLock
from time import monotonic, perf_counter, sleep, time
from typing import Callable, Mapping, Sequence

from ndnsf import (
    AckDecision,
    CollaborationContext,
    GenericProviderRuntimeHint,
    NEGATIVE_ACK_REASON_GPU_BUSY,
    NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
    NEGATIVE_ACK_REASON_PROVIDER_BUSY,
    NEGATIVE_ACK_REASON_QUEUE_FULL,
    ProviderCapabilityHint,
    ServiceProvider,
    ServiceOperationState,
    ServiceOperationStatus,
    ServiceResponse,
    decode_provider_capability_ack,
    encode_provider_capability_ack,
    parse_ack_metadata,
    to_plain,
)

from .artifact_deployment import (
    ExecutionArtifact,
    ExecutionArtifactSpec,
    ExecutionContext,
    prepare_execution,
    prepare_runtime,
)
from .core import (
    LEGACY_READY_SET_V1,
    AssignmentContext, AtomicReservationBook, ProviderAssignment, ProviderProfileV1,
    RuntimeTelemetryV1, DeploymentIntent, DeploymentPlan,
    DIRequestEnvelopeV2, DISelectionAssignmentV2, DISelectionParticipant,
    GpuMiBAdmissionLedger,
    ExecutionActivateMessage, ProviderCapabilityOffer, PreparationCallbacks,
    ReservationDecisionAuthority, SelectionDecision, SelectionGatedProvider,
)
from .plan import DependencyEdge, RoleDependencyView
from .adapters.base import MAX_INLINE_INPUT_BYTES
from .repo_reference import LargeDataReference, _publication_manifest_digest
from .sdk.placement import (
    DIProviderOfferV2, ProviderOfferV3, DeviceTopologyProfile,
    DeviceResourceSnapshot, ExecutionDisposition, ResidencyProofV3,
    ProviderSelectionProjectionV3, UNBOUND_GRAPH_DIGEST_V3, canonical_digest,
)


@dataclass(frozen=True)
class LargePrefetchResult:
    payload: bytes
    ref_wait_ms: float
    fetch_ms: float
    total_ms: float
    expected_segments: int = 0
    expected_bytes: int = 0
    used_planned_name: bool = False


def validate_request_input_boundary(request: DIRequestEnvelopeV2) -> str:
    """Validate request input transport without fetching application data.

    ACK processing may inspect only the transport metadata.  A repository
    reference is intentionally returned as ``REPO_REF`` metadata; plaintext
    fetch/decrypt is owned by the selected ingress role after Selection.
    """
    if not isinstance(request, DIRequestEnvelopeV2):
        raise TypeError("request must be DIRequestEnvelopeV2")
    if request.input_transport == "REPO_REF":
        if request.input_payload_b64:
            raise ValueError("REPO_REF request contains inline payload")
        return "REPO_REF"
    if request.input_transport != "INLINE":
        raise ValueError("unsupported request input transport")
    return "INLINE"


def _dependency_view_from_assignment(role_assignment) -> RoleDependencyView:
    """Build the handler view from the exact dependency edges in Selection."""

    role = str(role_assignment.role)
    inputs: list[DependencyEdge] = []
    outputs: list[DependencyEdge] = []
    internal: list[DependencyEdge] = []
    for committed in role_assignment.dependencies:
        edge = DependencyEdge(
            producers=list(committed.producers),
            consumers=list(committed.consumers),
            key_scope=committed.key_scope,
            topic_prefix=committed.topic_prefix,
            required=committed.required,
            tensors=list(committed.tensors),
            object_name_template=committed.object_name_template,
            expected_segments=committed.expected_segments,
            expected_bytes=committed.expected_bytes,
        )
        is_producer = role in edge.producers
        is_consumer = role in edge.consumers
        if is_producer and is_consumer:
            internal.append(edge)
        elif is_producer:
            outputs.append(edge)
        elif is_consumer:
            inputs.append(edge)
        else:
            raise ValueError(
                f"committed dependency {edge.key_scope!r} excludes role {role!r}")
    return RoleDependencyView(
        role=role, inputs=inputs, outputs=outputs, internal=internal)


def _dependency_view_from_v3_projection(
    role: str, dependencies: Sequence[Mapping[str, object]],
) -> RoleDependencyView:
    """Build the handler view from the request's sealed V3 projection.

    V3 planning is intentionally request-first: the requester derives graph
    edges after ACK_CLOSED and seals their exact key scopes in the Selection
    projection.  A Provider's static service policy may describe an older
    model or split and therefore must not override those request-bound edges.
    """
    edges: list[DependencyEdge] = []
    for item in dependencies:
        if not isinstance(item, Mapping):
            raise ValueError("V3 projection dependency is not an object")
        producers = [str(value) for value in item.get("producers", ())]
        consumers = [str(value) for value in item.get("consumers", ())]
        key_scope = str(item.get("key_scope", ""))
        topic_prefix = str(item.get("topic_prefix", "/activation"))
        if not producers or not consumers or not key_scope or not topic_prefix:
            raise ValueError("V3 projection dependency binding is invalid")
        # A Selection projection is intentionally identical for every
        # Provider so its plan digest covers the complete graph.  Providers
        # must ignore edges owned by other roles and retain only their local
        # input/output/internal view; rejecting unrelated edges would make a
        # valid multi-role projection unusable.
        if role not in producers + consumers:
            continue
        edges.append(DependencyEdge(
            producers=producers,
            consumers=consumers,
            key_scope=key_scope,
            topic_prefix=topic_prefix,
            required=bool(item.get("required", True)),
            tensors=[str(value) for value in item.get("tensors", ())],
            object_name_template=str(item.get("object_name_template", "")),
            expected_segments=int(item.get("expected_segments", 0) or 0),
            expected_bytes=int(item.get("expected_bytes", 0) or 0),
        ))
    inputs = [edge for edge in edges
              if role in edge.consumers and role not in edge.producers]
    outputs = [edge for edge in edges
               if role in edge.producers and role not in edge.consumers]
    internal = [edge for edge in edges
                if role in edge.producers and role in edge.consumers]
    return RoleDependencyView(
        role=role, inputs=inputs, outputs=outputs, internal=internal)


@dataclass(frozen=True)
class ProviderAdmissionPolicy:
    """Optional provider-local policy for converting telemetry into negative ACKs."""

    max_queue: int | None = None
    max_active_workers: int | None = None
    min_free_memory_mb: float | None = None
    max_queue_wait_ewma_ms: float | None = None
    require_model_loaded: bool = False

    def evaluate(self, telemetry: RuntimeTelemetryV1) -> tuple[bool, str, dict[str, object]]:
        diagnostics: dict[str, object] = {
            "admissionPolicy": "provider-telemetry",
        }
        if self.require_model_loaded and not telemetry.model_loaded:
            diagnostics["admissionLimit"] = "modelLoaded"
            return False, NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE, diagnostics
        if self.min_free_memory_mb is not None and telemetry.free_memory_mb < self.min_free_memory_mb:
            diagnostics["admissionLimit"] = "freeMemoryMb"
            diagnostics["admissionThreshold"] = self.min_free_memory_mb
            return False, NEGATIVE_ACK_REASON_GPU_BUSY, diagnostics
        if self.max_active_workers is not None and telemetry.active_workers >= self.max_active_workers:
            diagnostics["admissionLimit"] = "activeWorkers"
            diagnostics["admissionThreshold"] = self.max_active_workers
            return False, NEGATIVE_ACK_REASON_PROVIDER_BUSY, diagnostics
        if self.max_queue is not None and telemetry.aggregate_queue >= self.max_queue:
            diagnostics["admissionLimit"] = "queue"
            diagnostics["admissionThreshold"] = self.max_queue
            return False, NEGATIVE_ACK_REASON_QUEUE_FULL, diagnostics
        if (self.max_queue_wait_ewma_ms is not None and
                telemetry.queue_wait_ewma_ms >= self.max_queue_wait_ewma_ms):
            diagnostics["admissionLimit"] = "queueWaitEwmaMs"
            diagnostics["admissionThreshold"] = self.max_queue_wait_ewma_ms
            return False, NEGATIVE_ACK_REASON_PROVIDER_BUSY, diagnostics
        return True, "", diagnostics


class DIProviderOfferIssuer:
    """Issue signed, capacity-held V2 offers for positive generic ACKs.

    The issuer sanitizes the ACK payload to the public V2 contract and reserves
    its GPU-MiB promise before returning a positive decision. Call
    ``release_unused`` when ACK closure identifies offers that were not
    selected.
    """

    def __init__(
        self, *, provider: str, service: str, boot_epoch: str,
        ledger: GpuMiBAdmissionLedger, offered_gpu_memory_mb: int,
        signer_key_id: str, sign_offer_digest: Callable[[str], str],
        devices: Sequence[str],
        offer_lease_ms: int = 60 * 60 * 1000,
        max_pending_state_ttl_ms: int = 60 * 60 * 1000,
        clock_ms: Callable[[], int] | None = None,
    ) -> None:
        if (ledger.provider != provider or ledger.boot_epoch != boot_epoch
                or offered_gpu_memory_mb <= 0 or not signer_key_id
                or not callable(sign_offer_digest) or offer_lease_ms <= 0
                or max_pending_state_ttl_ms <= 0
                or not devices
                or len(set(devices)) != len(tuple(devices))
                or any(not device for device in devices)):
            raise ValueError("invalid DI Provider offer issuer")
        self.provider = provider
        self.service = service
        self.boot_epoch = boot_epoch
        self.ledger = ledger
        self.offered_gpu_memory_mb = int(offered_gpu_memory_mb)
        self.signer_key_id = signer_key_id
        self.devices = tuple(devices)
        self._sign = sign_offer_digest
        self.offer_lease_ms = int(offer_lease_ms)
        self.max_pending_state_ttl_ms = int(max_pending_state_ttl_ms)
        self._clock_ms = clock_ms or (lambda: int(time() * 1000))
        self._sequence = 0
        self._offers: dict[str, DIProviderOfferV2] = {}
        self._lock = RLock()

    def _validated_cached_shards(
        self, cached_shards: Sequence[object], *, now_ms: int,
        backends: Sequence[str],
    ) -> tuple[dict, ...]:
        """Fail closed before signing malformed or stale residency claims."""

        required = {
            "artifact_digest", "model_content_digest", "semantics_digest",
            "graph_digest", "partition_digest", "backend", "precision",
            "tier", "boot_epoch", "cache_epoch", "captured_at_ms",
            "expires_at_ms",
        }
        result = []
        for raw in cached_shards:
            if not isinstance(raw, Mapping):
                raise ValueError("cached shard evidence must be a mapping")
            shard = {str(key): value for key, value in raw.items()}
            if not required.issubset(shard):
                raise ValueError("cached shard evidence is incomplete")
            for name in (
                    "artifact_digest", "model_content_digest",
                    "semantics_digest", "graph_digest", "partition_digest"):
                value = str(shard[name])
                if (len(value) != 71 or not value.startswith("sha256:")):
                    raise ValueError(
                        f"cached shard {name} is not a canonical digest")
                try:
                    int(value[7:], 16)
                except ValueError as exc:
                    raise ValueError(
                        f"cached shard {name} is not a canonical digest") from exc
            tier = str(shard["tier"]).upper()
            if tier not in {
                    "PINNED_GPU", "RELOAD_SAFE_GPU", "HOST_RAM", "DISK"}:
                raise ValueError("cached shard tier is not reusable")
            if (str(shard["boot_epoch"]) != self.boot_epoch
                    or str(shard["backend"]) not in tuple(backends)
                    or int(shard["cache_epoch"]) <= 0
                    or int(shard["captured_at_ms"]) > now_ms
                    or int(shard["expires_at_ms"]) <= now_ms):
                raise ValueError("cached shard evidence is stale or incompatible")
            if tier in {"PINNED_GPU", "RELOAD_SAFE_GPU"}:
                device = str(shard.get("device", ""))
                if device not in self.devices:
                    raise ValueError(
                        "GPU cached shard does not match an offered device")
            if (tier == "PINNED_GPU"
                    and int(shard.get("pin_until_ms", 0)) <= now_ms):
                raise ValueError("pinned GPU cached shard has no live pin")
            result.append(shard)
        return tuple(result)

    def issue(
        self, request_wire: bytes, *, accepted_roles: Sequence[str],
        backends: Sequence[str], queue_depth: int | None = None,
        estimated_wait_ms: float | None = None,
        rtt_ms: float | None = None, bandwidth_mbps: float | None = None,
        cached_shards: Sequence[object] = (),
        reusable_state: Sequence[object] = (),
    ) -> AckDecision:
        request = DIRequestEnvelopeV2.from_bytes(request_wire)
        # The ACK path validates only the transport boundary.  REPO_REF data
        # remains opaque until the selected ingress role receives Selection.
        validate_request_input_boundary(request)
        now_ms = self._clock_ms()
        if request.service != self.service or request.plan_deadline_ms <= now_ms:
            return AckDecision(
                status=False, message="DI_V2_REQUEST_NOT_ADMISSIBLE")
        pending_state_ttl_ms = request.plan_deadline_ms - now_ms
        if pending_state_ttl_ms > self.max_pending_state_ttl_ms:
            return AckDecision(
                status=False,
                message="DI_V2_REQUEST_DEADLINE_EXCEEDS_PROVIDER_LIMIT")
        if pending_state_ttl_ms > self.offer_lease_ms:
            # This attempt has no offer-renewal round.  Returning a positive
            # ACK whose signed offer expires before the accepted plan deadline
            # would make a request-first cold plan fail only after expensive
            # artifact publication.  Reject at admission instead.
            return AckDecision(
                status=False,
                message="DI_V2_REQUEST_DEADLINE_EXCEEDS_OFFER_LEASE")
        try:
            cached_shards = self._validated_cached_shards(
                cached_shards, now_ms=now_ms, backends=backends)
        except (TypeError, ValueError):
            return AckDecision(
                status=False, message="DI_RESIDENCY_EVIDENCE_INVALID")
        with self._lock:
            # Core may re-enter the collaboration ACK policy while the same
            # request is being admitted (for example after a Selection
            # retransmission or an ACK replay).  A positive DI ACK already
            # owns one ledger hold; issuing a second signed offer for the same
            # request would try to reserve the same GPU twice and incorrectly
            # turn a valid Selection into DI_GPU_CAPACITY_UNAVAILABLE.
            # Reuse only an unexpired offer with the complete immutable request
            # and capability binding.  A different request, attempt, model,
            # role set, or backend set still goes through fresh admission.
            accepted_roles_tuple = tuple(accepted_roles)
            backends_tuple = tuple(backends)
            for offer in tuple(self._offers.values()):
                if (
                    offer.request_id == request.request_id
                    and offer.attempt == request.attempt
                    and offer.service == request.service
                    and offer.model_intent_digest == request.model_identity_hash
                    and offer.provider == self.provider
                    and offer.boot_epoch == self.boot_epoch
                    and offer.accepted_roles == accepted_roles_tuple
                and offer.backends == backends_tuple
                    and offer.devices == self.devices
                    and offer.expires_at_ms > now_ms
                ):
                    return AckDecision(
                        status=True,
                        message="DI_SELECTION_DATAFLOW_V2_READY",
                        payload=offer.to_bytes(),
                        pending_state_ttl_ms=pending_state_ttl_ms)
            self._sequence += 1
            evidence = {
                "provider": self.provider,
                "boot_epoch": self.boot_epoch,
                "resource_sequence": self._sequence,
                "accepted_roles": accepted_roles_tuple,
                "backends": backends_tuple,
                "devices": self.devices,
                "gpu_mib": self.offered_gpu_memory_mb,
                "queue_depth": queue_depth,
                "estimated_wait_ms": estimated_wait_ms,
                "rtt_ms": rtt_ms,
                "bandwidth_mbps": bandwidth_mbps,
                "cached_shards": tuple(cached_shards),
                "reusable_state": tuple(reusable_state),
            }
            unsigned = DIProviderOfferV2(
                profile="ndnsf-di-provider-offer-v2",
                profile_version=2,
                request_id=request.request_id,
                attempt=request.attempt,
                service=request.service,
                provider=self.provider,
                model_intent_digest=request.model_identity_hash,
                boot_epoch=self.boot_epoch,
                resource_sequence=self._sequence,
                captured_at_ms=now_ms,
                expires_at_ms=min(
                    request.plan_deadline_ms, now_ms + self.offer_lease_ms),
                accepted_deadline_ms=request.plan_deadline_ms,
                accepted_roles=accepted_roles_tuple,
                backends=backends_tuple,
                devices=self.devices,
                offered_gpu_memory_mb=self.offered_gpu_memory_mb,
                queue_depth=queue_depth,
                estimated_wait_ms=estimated_wait_ms,
                rtt_ms=rtt_ms,
                bandwidth_mbps=bandwidth_mbps,
                capability_resource_digest=canonical_digest({
                    "provider": self.provider,
                    "boot_epoch": self.boot_epoch,
                    "capacity_mib": self.ledger.capacity_mib,
                }),
                acceptance_predicate_digest=canonical_digest({
                    "predicate": "DI_ACCEPTANCE_V2",
                    "service": self.service,
                }),
                evidence_digest=canonical_digest(evidence),
                signer_key_id=self.signer_key_id,
                signature="unsigned-placeholder",
                cached_shards=tuple(cached_shards),
                reusable_state=tuple(reusable_state),
            )
            signature = str(self._sign(unsigned.digest()))
            if not signature:
                raise ValueError("DI Provider offer signer returned no signature")
            offer = replace(unsigned, signature=signature)
            try:
                self.ledger.hold_offer(offer, now_ms=now_ms)
            except ValueError:
                return AckDecision(
                    status=False, message="DI_GPU_CAPACITY_UNAVAILABLE")
            self._offers[offer.digest()] = offer
            return AckDecision(
                status=True, message="DI_SELECTION_DATAFLOW_V2_READY",
                payload=offer.to_bytes(),
                pending_state_ttl_ms=pending_state_ttl_ms)

    def lookup(self, offer_digest: str) -> DIProviderOfferV2:
        with self._lock:
            return self._offers[offer_digest]

    def release_unused(self, *, request_id: str, attempt: int,
                       selected_offer_digest: str = "") -> None:
        with self._lock:
            for digest, offer in tuple(self._offers.items()):
                if (offer.request_id == request_id and offer.attempt == attempt
                        and digest != selected_offer_digest):
                    self.ledger.release_offer(digest, reason="not-selected")
                    self._offers.pop(digest, None)


class DIProviderOfferIssuerV3:
    """Issue observational V3 offers without touching a reservation ledger."""

    def __init__(self, *, provider: str, service: str, boot_epoch: str,
                 devices: Sequence[str], signer_key_id: str,
                 sign_offer_digest: Callable[[str], str],
                 clock_ms: Callable[[], int] | None = None) -> None:
        if (not provider or not service or not boot_epoch or not signer_key_id
                or not callable(sign_offer_digest)
                or len(set(devices)) != len(tuple(devices))):
            raise ValueError("invalid V3 Provider offer issuer")
        self.provider = provider
        self.service = service
        self.boot_epoch = boot_epoch
        self.devices = tuple(str(item) for item in devices)
        self.signer_key_id = signer_key_id
        self._sign = sign_offer_digest
        self._clock_ms = clock_ms or (lambda: int(time() * 1000))
        self._sequence = 0

    def issue(
        self, *, request_id: str, attempt: int, model_digest: str,
        graph_digest: str = "", deadline_ms: int = 0,
        accepted_roles: Sequence[str] = (),
        backends: Sequence[str], execution_disposition: ExecutionDisposition | str,
        preparation_accepted: bool, residency: Sequence[ResidencyProofV3] = (),
        resources: Sequence[DeviceResourceSnapshot] = (), queue_depth: int = 0,
        estimated_wait_ms: float = 0.0, rtt_ms: float = 0.0,
        bandwidth_mbps: float = 0.0, can_provision: bool = False,
        has_model: bool = False) -> AckDecision:
        now_ms = int(self._clock_ms())
        if int(deadline_ms) <= now_ms:
            return AckDecision(status=False, message="DI_V3_REQUEST_EXPIRED")
        graph_digest = str(graph_digest or UNBOUND_GRAPH_DIGEST_V3)
        disposition = ExecutionDisposition(execution_disposition)
        status = disposition != ExecutionDisposition.REJECT
        self._sequence += 1
        topology = DeviceTopologyProfile(
            self.provider, self.devices,
            "cuda" if any(item.startswith("cuda:") for item in self.devices) else "cpu")
        offer = ProviderOfferV3(
            request_id=str(request_id), attempt=int(attempt), service=self.service,
            provider=self.provider, model_digest=model_digest,
            graph_digest=graph_digest, status=status,
            execution_disposition=disposition,
            preparation_accepted=bool(preparation_accepted), topology=topology,
            resources=tuple(resources), residency=tuple(residency),
            accepted_roles=tuple(accepted_roles), backends=tuple(backends),
            queue_depth=int(queue_depth), estimated_wait_ms=float(estimated_wait_ms),
            rtt_ms=float(rtt_ms), bandwidth_mbps=float(bandwidth_mbps),
            boot_epoch=self.boot_epoch, captured_at_ms=now_ms,
            expires_at_ms=int(deadline_ms), signer_key_id=self.signer_key_id,
            can_provision=bool(can_provision), has_model=bool(has_model),
            signature="unsigned-placeholder")
        signature = str(self._sign(offer.digest()))
        if not signature:
            raise ValueError("V3 Provider offer signer returned no signature")
        offer = replace(offer, signature=signature)
        # No reservation, lease, queue entry, or device-state mutation occurs.
        return AckDecision(
            status=status, message="DI_PLACEMENT_V3_OFFER",
            payload=offer.to_bytes(),
            # V3 does not reserve a device at ACK time, but Core still needs
            # to retain the authenticated Request/token state until the
            # requester's post-ACK graph planning and canonical publication
            # can deliver Selection.  Leaving this at zero makes Core apply
            # its short provisional cleanup (30 s), so any legitimate cold
            # publication is rejected as "no pending request".  The horizon
            # is request-deadline bounded and carries no resource hold.
            pending_state_ttl_ms=max(1, int(deadline_ms) - now_ms))


def register_selection_dataflow_v2(
    network_provider: ServiceProvider,
    *,
    service: str,
    participant: DISelectionParticipant,
    wal_path: str | Path,
    storage_key: bytes,
    storage_key_epoch: str,
    max_prepare_ms: int = 1000,
) -> DISelectionParticipant:
    """Attach DI V2 to the generic Core opaque Selection transaction.

    Core owns authentication, token/lease disposition, encrypted WAL and
    replay. This function registers only DI-owned semantic validation and
    post-COMMITTED preparation callbacks.
    """
    network_provider.configure_opaque_selection_store(
        wal_path=str(wal_path),
        storage_key=bytes(storage_key),
        storage_key_epoch=storage_key_epoch,
        max_prepare_ms=max_prepare_ms,
    )
    network_provider.register_opaque_selection_participant(
        service,
        participant_id=participant.PARTICIPANT_ID,
        participant_version=participant.PARTICIPANT_VERSION,
        prepare=participant.prepare,
        on_committed=participant.on_committed,
        on_aborted=participant.on_aborted,
    )
    return participant


def make_selection_gated_provider(
    *,
    provider_name: str,
    provider_boot_epoch: str,
    capability: Callable[[DeploymentIntent], ProviderCapabilityOffer],
    verify: Callable[[DeploymentPlan, ProviderAssignment], None],
    load: Callable[[DeploymentPlan, ProviderAssignment], None],
    warm: Callable[[DeploymentPlan, ProviderAssignment], None],
    activation_verifier: Callable[[ExecutionActivateMessage], bool],
    release: Callable[[object], None] = lambda _instance: None,
    reservation_book: AtomicReservationBook | None = None,
    reservation_authorizer: Callable[[DeploymentIntent], bool] | None = None,
) -> SelectionGatedProvider:
    """Bind DI lifecycle callbacks to the canonical Spec 129 Core authority."""
    return SelectionGatedProvider(
        provider_name, provider_boot_epoch, capability,
        PreparationCallbacks(verify, load, warm, release),
        activation_verifier,
        reservation_book=reservation_book,
        reservation_authorizer=reservation_authorizer)


class DependencyPrefetcher:
    """Prefetch predictable dependency objects for one provider invocation.

    The prefetcher is intentionally model-agnostic. It only knows the current
    NDNSF collaboration context, a role-local dependency edge, and the planned
    dependency topic. Applications decide which edge/topic suffix is safe to
    prefetch based on their plan.
    """

    def __init__(self, ndnsf: CollaborationContext, *, max_workers: int = 4):
        self._ndnsf = ndnsf
        self._executor = ThreadPoolExecutor(
            max_workers=max(1, int(max_workers)),
            thread_name_prefix="ndnsf-di-prefetch",
        )

    def prefetch_large(self, edge, topic_suffix: str = "", *,
                       ref_timeout_ms: int = 10000,
                       fetch_timeout_ms: int = 10000,
                       data_name: str = "",
                       expected_segments: int = 0,
                       expected_bytes: int = 0) -> Future:
        topic = edge.topic(topic_suffix)

        def fetch() -> LargePrefetchResult:
            total_start = perf_counter()
            if data_name:
                fetch_start = perf_counter()
                if expected_segments > 0 and hasattr(self._ndnsf, "fetch_large_exact"):
                    payload = self._ndnsf.fetch_large_exact(
                        data_name,
                        edge.key_scope,
                        fetch_timeout_ms,
                        expected_segments,
                    )
                else:
                    payload = self._ndnsf.fetch_large(
                        data_name,
                        edge.key_scope,
                        fetch_timeout_ms,
                    )
                fetch_ms = _elapsed_ms(fetch_start)
                if payload is not None:
                    return LargePrefetchResult(
                        payload=payload,
                        ref_wait_ms=0.0,
                        fetch_ms=fetch_ms,
                        total_ms=_elapsed_ms(total_start),
                        expected_segments=expected_segments,
                        expected_bytes=expected_bytes,
                        used_planned_name=True,
                    )
            ref_start = perf_counter()
            ref = self._ndnsf.wait_one(edge.key_scope, topic, ref_timeout_ms)
            ref_wait_ms = _elapsed_ms(ref_start)
            if ref is None:
                raise TimeoutError(
                    f"timed out waiting for dependency ref "
                    f"scope={edge.key_scope} topic={topic}")
            fetch_start = perf_counter()
            payload = self._ndnsf.fetch_large_reference(
                ref.payload,
                edge.key_scope,
                fetch_timeout_ms,
            )
            fetch_ms = _elapsed_ms(fetch_start)
            if payload is None:
                raise TimeoutError(
                    f"timed out fetching dependency object "
                    f"scope={edge.key_scope} topic={topic}")
            return LargePrefetchResult(
                payload=payload,
                ref_wait_ms=ref_wait_ms,
                fetch_ms=fetch_ms,
                total_ms=_elapsed_ms(total_start),
                expected_segments=expected_segments,
                expected_bytes=expected_bytes,
                used_planned_name=False,
            )

        return self._executor.submit(fetch)

    def shutdown(self) -> None:
        self._executor.shutdown(wait=True)


def _assignment_deadline_ms(payload: bytes) -> int:
    """Return the signed execution deadline carried by a DI assignment.

    Selection Dataflow V2 assignments are canonical JSON contracts.  Older
    collaboration paths used semicolon-delimited metadata, so retain a small
    compatibility parser without treating unsigned application fields as an
    authority for the V2 path.
    """

    raw = bytes(payload or b"")
    if not raw:
        return 0
    try:
        assignment = DISelectionAssignmentV2.from_bytes(raw)
    except (TypeError, ValueError, KeyError, json.JSONDecodeError):
        assignment = None
    if assignment is not None:
        return max(0, int(assignment.deadline_ms))
    try:
        projection = ProviderSelectionProjectionV3.from_bytes(raw)
    except (TypeError, ValueError, KeyError, json.JSONDecodeError):
        projection = None
    if projection is not None:
        return max(0, int(projection.deadline_ms))
    try:
        text = raw.decode("utf-8", errors="replace")
    except Exception:
        return 0
    fields: dict[str, str] = {}
    for item in text.split(";"):
        if "=" in item:
            key, value = item.split("=", 1)
            fields[key] = value
    for key in ("executionDeadlineMs", "deadlineMs", "deadline_ms"):
        try:
            value = int(fields.get(key, "0"))
        except (TypeError, ValueError):
            value = 0
        if value > 0:
            return value
    return 0


@dataclass(frozen=True)
class _TerminalResponseGuard:
    lock: RLock = field(default_factory=RLock, compare=False, repr=False)
    published: list[bool] = field(
        default_factory=list, compare=False, repr=False)

    def claim(self) -> None:
        with self.lock:
            if self.published:
                raise RuntimeError(
                    "NDNSF-DI terminal Response was already published")
            self.published.append(True)


@dataclass(frozen=True)
class ProviderRuntimeContext:
    ndnsf: CollaborationContext
    execution: object
    request: bytes
    role: str
    dependencies: RoleDependencyView = field(
        default_factory=lambda: RoleDependencyView(role=""))
    prefetcher: DependencyPrefetcher | None = None
    assignment_context: AssignmentContext | None = None
    deadline_ms: int = 0
    on_dependency_ready: Callable[[object], None] | None = None
    # Present only for a streamed invocation.  The object is intentionally
    # duck-typed so the DI layer remains independent of the Python binding;
    # it exposes publish_event(), finish_stream(), and fail().
    stream_writer: object | None = None
    # V3 dataflow ownership is request-scoped.  Legacy/simple contexts leave
    # enforcement disabled for source compatibility; a decoded V3 Selection
    # must set these flags from its signed role/dataflow projection.
    input_ingress_owner: bool = False
    terminal_response_owner: bool = False
    enforce_dataflow_ownership: bool = False
    _terminal_response_guard: _TerminalResponseGuard = field(
        default_factory=_TerminalResponseGuard,
        compare=False, repr=False,
    )

    @property
    def request_id(self) -> str:
        """Return the immutable request identifier for this invocation.

        The wire/runtime context owns the identifier as ``session_id``.  DI
        application handlers use ``request_id`` in their evidence records, so
        expose the explicit alias at this boundary instead of making every
        adapter reach through ``ctx.ndnsf``.
        """

        return str(self.ndnsf.session_id)

    def _require_input_ingress(self) -> None:
        if (self.enforce_dataflow_ownership
                and not self.input_ingress_owner):
            raise PermissionError("DI_INPUT_FETCH_ROLE_MISMATCH")

    def _require_terminal_owner(self) -> None:
        if (self.enforce_dataflow_ownership
                and not self.terminal_response_owner):
            raise PermissionError("DI_TERMINAL_RESPONSE_ROLE_MISMATCH")

    def fetch_application_input(self, timeout_ms: int | None = None) -> bytes:
        """Resolve the request input only from the signed ingress role.

        INLINE bytes are decoded from the canonical request envelope.  A
        REPO_REF contains metadata only; the selected role must ask the
        reference-aware NDNSF large-data path to fetch/decrypt/verify it.
        A name-only or size-only fetch is rejected before plaintext is
        returned, and reference-integrity failures use a stable reason.
        No plaintext is logged or copied into an evidence record by this
        helper.
        """
        self._require_input_ingress()
        request = DIRequestEnvelopeV2.from_bytes(bytes(self.request))
        transport = validate_request_input_boundary(request)
        if transport == "INLINE":
            try:
                payload = base64.b64decode(
                    request.input_payload_b64, validate=True)
            except (ValueError, TypeError) as exc:
                raise ValueError("DI_INPUT_INLINE_BASE64_INVALID") from exc
            if len(payload) > MAX_INLINE_INPUT_BYTES:
                raise ValueError("DI_INPUT_INLINE_OVERSIZE")
            return bytes(payload)

        try:
            reference = LargeDataReference.from_mapping(request.input_reference)
            expected_manifest = _publication_manifest_digest(
                data_name=reference.data_name,
                object_id=reference.object_id,
                plaintext_size=reference.plaintext_size,
                content_digest=reference.ciphertext_digest,
                authorization_scope=reference.authorization_scope,
                protection_epoch=reference.protection_epoch,
            )
            if reference.manifest_digest != expected_manifest:
                raise ValueError("manifest digest mismatch")
        except (TypeError, ValueError, OverflowError) as exc:
            raise ValueError("DI_INPUT_REPO_DIGEST_UNVERIFIED") from exc
        fetch_reference = getattr(self.ndnsf, "fetch_large_reference", None)
        if not callable(fetch_reference):
            raise RuntimeError("DI_INPUT_REPO_DIGEST_UNVERIFIED")
        if timeout_ms is None:
            if self.deadline_ms > 0:
                timeout_ms = max(1, self.deadline_ms - int(time() * 1000))
            else:
                timeout_ms = 5000
        if int(timeout_ms) <= 0:
            raise TimeoutError("DI_INPUT_REPO_DEADLINE_EXPIRED")
        try:
            payload = fetch_reference(
                reference.to_dict(), reference.authorization_scope, int(timeout_ms))
        except TimeoutError:
            raise
        except PermissionError as exc:
            raise ValueError("DI_INPUT_REPO_DIGEST_UNVERIFIED") from exc
        except ValueError as exc:
            if "size mismatch" in str(exc):
                raise ValueError("DI_INPUT_REPO_SIZE_MISMATCH") from exc
            raise ValueError("DI_INPUT_REPO_DIGEST_UNVERIFIED") from exc
        if payload is None:
            raise TimeoutError("DI_INPUT_REPO_FETCH_TIMEOUT")
        payload = bytes(payload)
        return payload

    def publish_final_response(self, payload: bytes) -> None:
        """Publish exactly one complete authenticated terminal Response."""
        self._require_terminal_owner()
        self._terminal_response_guard.claim()
        self.ndnsf.publish_final_response(bytes(payload))

    def publish_terminal_result(self, payload: bytes) -> None:
        """Publish the one result owned by the candidate-declared egress role."""
        self.publish_final_response(bytes(payload))

    def publish_event(self, payload: bytes, *, event_type: str = "application") -> int:
        """Publish one ordered event and return its committed 1-based cursor.

        Invocation streams currently admit application events only.  Keeping
        the type explicit at this boundary prevents an adapter from silently
        manufacturing an End event or a second terminal state.
        """
        writer = self.stream_writer
        if writer is None or not (hasattr(writer, "publish_event") or
                                  hasattr(writer, "publish_stream_event")):
            raise RuntimeError("stream writer is not attached to this invocation")
        if str(event_type) != "application":
            raise ValueError("NDNSF-DI streamed events must use event_type=application")
        try:
            publish = getattr(writer, "publish_event", None)
            if publish is None:
                publish = writer.publish_stream_event
            cursor = publish(bytes(payload), event_type="application")
        except TypeError:
            # Keep small test doubles and pre-T010 adapters source-compatible;
            # the native binding accepts the keyword and returns the cursor.
            publish = getattr(writer, "publish_event", None)
            if publish is None:
                publish = writer.publish_stream_event
            cursor = publish(bytes(payload))
        if isinstance(cursor, bool):
            cursor = 1 if cursor else 0
        cursor = int(cursor)
        if cursor <= 0:
            raise RuntimeError("stream event was rejected by lifecycle or deadline")
        return cursor

    def finish_stream(
        self, payload: bytes = b"", *, finish_reason: str | int = "application_complete",
        reason: int | None = None,
    ) -> None:
        """Claim the streamed terminal and carry the complete final result."""
        self._require_terminal_owner()
        writer = self.stream_writer
        if writer is None or not hasattr(writer, "finish_stream"):
            # Preserve the unary context contract for legacy handlers.
            self.publish_final_response(bytes(payload))
            return
        # The DI context and the Core writer share one logical terminal claim.
        # Claim before crossing the binding so a concurrent legacy
        # publish_final_response cannot win after the stream has started.
        self._terminal_response_guard.claim()
        if reason is not None:
            finish_reason = reason
        if isinstance(finish_reason, str):
            reason_codes = {
                "eos": 1,
                "stop_sequence": 2,
                "max_tokens": 3,
                "application_complete": 4,
                "deadline": 5,
                "cancelled": 6,
                "failed": 7,
            }
            try:
                finish_reason = reason_codes[finish_reason.strip().lower()]
            except KeyError as exc:
                raise ValueError("unknown streamed finish reason") from exc
        try:
            accepted = writer.finish_stream(bytes(payload), reason=int(finish_reason))
        except TypeError:
            accepted = writer.finish_stream(bytes(payload), int(finish_reason))
        if not accepted:
            raise RuntimeError("stream terminal was already claimed or fenced")

    def stream_cancelled(self) -> bool:
        """Return the current Core-owned cancellation/fence state."""
        writer = self.stream_writer
        if writer is None:
            return False
        value = getattr(writer, "cancelled", False)
        if callable(value):
            value = value()
        return bool(value)

    def run_streamed_generation(self, loop, prompt):
        """Run one adapter-owned generation loop on this invocation.

        The loop must already be bound to this request ID and owns only the
        selected plan's prefill/decode callbacks.  This helper deliberately
        does not expose a request or provider list, so an adapter cannot turn
        one streamed generation into per-token discovery calls.
        """
        if self.stream_writer is None:
            raise RuntimeError("stream writer is not attached to this invocation")
        if str(getattr(loop, "request_id", "")) != self.request_id:
            raise ValueError("generation loop request binding mismatch")
        return loop.run(prompt)

    def remaining_deadline_ms(
        self,
        *,
        fallback_ms: int = 30000,
        safety_margin_ms: int = 1000,
        now_ms: int | None = None,
    ) -> int:
        """Return a bounded timeout derived from the signed request deadline.

        A missing deadline is only possible on legacy/non-DI local paths and
        keeps their historical bounded timeout.  A V2 assignment must never
        silently fall back after its deadline has expired.
        """

        deadline = int(self.deadline_ms or 0)
        if not deadline:
            metadata = getattr(getattr(self.execution, "spec", None), "metadata", {})
            if isinstance(metadata, Mapping):
                for key in ("executionDeadlineMs", "deadlineMs", "deadline_ms"):
                    try:
                        candidate = int(metadata.get(key, 0) or 0)
                    except (TypeError, ValueError):
                        candidate = 0
                    if candidate > 0:
                        deadline = candidate
                        break
        if deadline <= 0:
            return max(1, int(fallback_ms))
        current = int(time() * 1000) if now_ms is None else int(now_ms)
        remaining = deadline - current - max(0, int(safety_margin_ms))
        if remaining <= 0:
            raise TimeoutError("DI execution deadline expired before dependency fetch")
        return remaining

    def dependency_timeout_ms(self, *, fallback_ms: int = 30000) -> int:
        """Timeout for one dependency wait/fetch operation in this invocation."""

        return self.remaining_deadline_ms(fallback_ms=fallback_ms)

    def planned_large_data_name(self, edge, producer_role: str) -> str:
        template = str(getattr(edge, "object_name_template", "") or "")
        if not template:
            return ""
        assignment = self.ndnsf.assignment
        role_providers = getattr(assignment, "role_providers", {}) or {}
        producer_provider = (
            self.ndnsf.local_provider
            if producer_role == self.role else
            str(role_providers.get(producer_role, ""))
        )
        if not producer_provider:
            return ""
        values = {
            "producerProvider": producer_provider.rstrip("/"),
            "sessionId": self.ndnsf.session_id.strip("/"),
            "keyScope": str(getattr(edge, "key_scope", "")),
            "producerRole": str(producer_role).strip("/"),
            "role": str(self.role).strip("/"),
            "topicPrefix": str(getattr(edge, "topic_prefix", "")).strip("/"),
            "sequence": "0",
        }
        try:
            return template.format(**values)
        except Exception:
            return ""

    def publish_output(self, payload: bytes, *, key_scope: str = "",
                       topic_suffix: str = "") -> None:
        edge = self.dependencies.output(key_scope)
        self.ndnsf.publish(edge.key_scope, edge.topic(topic_suffix), payload)

    def publish_output_large(self, payload: bytes, *, key_scope: str = "",
                             topic_suffix: str = "",
                             max_segment_size: int = 7000,
                             freshness_ms: int = 60000) -> str:
        edge = self.dependencies.output(key_scope)
        return self.ndnsf.publish_large(
            edge.key_scope,
            edge.topic(topic_suffix),
            payload,
            max_segment_size=max_segment_size,
            freshness_ms=freshness_ms,
        )

    def publish_output_large_reference(self, payload: bytes, *,
                                       key_scope: str = "",
                                       data_topic_suffix: str = "",
                                       ref_topic_suffix: str = "",
                                       object_type: str = "",
                                       object_id: str = "",
                                       data_name: str = "",
                                       max_segment_size: int = 7000,
                                       freshness_ms: int = 60000) -> str:
        edge = self.dependencies.output(key_scope)
        return self.ndnsf.publish_large_reference(
            edge.key_scope,
            edge.topic(data_topic_suffix),
            edge.topic(ref_topic_suffix),
            payload,
            object_type=object_type,
            object_id=object_id,
            data_name=data_name,
            max_segment_size=max_segment_size,
            freshness_ms=freshness_ms,
        )

    def wait_input(self, *, key_scope: str = "", topic_suffix: str = "",
                   timeout_ms: int = 10000):
        edge = self.dependencies.input(key_scope)
        value = self.ndnsf.wait_one(
            edge.key_scope, edge.topic(topic_suffix), timeout_ms)
        if value is not None and self.on_dependency_ready is not None:
            self.on_dependency_ready(edge)
        return value

    def prefetch_input_large(self, *, key_scope: str = "",
                             topic_suffix: str = "",
                             ref_timeout_ms: int = 10000,
                             fetch_timeout_ms: int = 10000,
                             producer_role: str = "") -> Future:
        """Start fetching a planned input dependency in the background.

        This is useful when a distributed plan makes dependency names
        predictable. The method does not know model semantics; it simply waits
        for the dependency reference on the selected edge and fetches the large
        object named by that reference.
        """

        if self.prefetcher is None:
            raise RuntimeError("dependency prefetcher is not available")
        edge = self.dependencies.input(key_scope)
        data_name = self.planned_large_data_name(edge, producer_role) if producer_role else ""
        try:
            future = self.prefetcher.prefetch_large(
                edge,
                topic_suffix,
                ref_timeout_ms=ref_timeout_ms,
                fetch_timeout_ms=fetch_timeout_ms,
            data_name=data_name,
            expected_segments=int(getattr(edge, "expected_segments", 0) or 0),
            expected_bytes=int(getattr(edge, "expected_bytes", 0) or 0),
        )
        except TypeError:
            future = self.prefetcher.prefetch_large(
                edge,
                topic_suffix,
                ref_timeout_ms=ref_timeout_ms,
                fetch_timeout_ms=fetch_timeout_ms,
            )
        future._ndnsf_di_dependency_edge = edge
        return future

    def wait_prefetched_input_large(self, future: Future, *,
                                    timeout_ms: int | None = None) -> bytes:
        result = self.wait_prefetched_input_large_result(
            future, timeout_ms=timeout_ms)
        edge = getattr(future, "_ndnsf_di_dependency_edge", None)
        if edge is not None and self.on_dependency_ready is not None:
            self.on_dependency_ready(edge)
        return result.payload

    @staticmethod
    def wait_prefetched_input_large_result(future: Future, *,
                                           timeout_ms: int | None = None) -> LargePrefetchResult:
        timeout_s = None if timeout_ms is None else max(0, timeout_ms) / 1000.0
        return future.result(timeout=timeout_s)

    def publish_internal(self, payload: bytes, *, key_scope: str = "",
                         topic_suffix: str = "") -> None:
        edge = self.dependencies.internal_scope(key_scope)
        self.ndnsf.publish(edge.key_scope, edge.topic(topic_suffix), payload)

    def wait_internal(self, *, key_scope: str = "", topic_suffix: str = "",
                      timeout_ms: int = 10000):
        edge = self.dependencies.internal_scope(key_scope)
        return self.ndnsf.wait_one(edge.key_scope, edge.topic(topic_suffix), timeout_ms)


InferenceHandler = Callable[[ProviderRuntimeContext], None]


def _elapsed_ms(start: float) -> float:
    return (perf_counter() - start) * 1000.0


def _validate_metadata_token(value: str, field: str) -> str:
    text = str(value)
    if not text:
        raise ValueError(f"{field} must not be empty")
    if any(ch in text for ch in ";\r\n"):
        raise ValueError(f"{field} must not contain ';' or newlines: {text!r}")
    return text


def _validate_list_token(value: str, field: str) -> str:
    text = _validate_metadata_token(value, field)
    if "," in text:
        raise ValueError(f"{field} must not contain ',': {text!r}")
    return text


def _safe_path_token(value: str) -> str:
    token = str(value).strip("/").replace("/", "-")
    return token or "role"


# Fixed storage profile for the functional slice (spec181 FR-013): the
# assembled artifact is AEAD-sealed at rest in the Provider work directory
# under this profile's canonical digest.  Epoch rotation is out of scope.
_PROTECTED_ASSEMBLY_STORAGE_PROFILE = "ndnsf-di-provider-workdir-scratch-v1"


class DistributedInferenceProvider:
    """Register inference roles using the underlying NDNSF provider."""

    def __init__(self, provider: ServiceProvider, *, handler_workers: int = 0,
                 grant_authority_public_key=None,
                 grant_recipient_private_key=None,
                 grant_fetch_timeout_ms: int = 30000):
        self.provider = provider
        self._handler_executor = (
            ThreadPoolExecutor(
                max_workers=int(handler_workers),
                thread_name_prefix="ndnsf-di-provider",
            )
            if int(handler_workers) > 0 else None
        )
        # Protected-epoch grant qualification (spec181 T001).  Both keys are
        # operator configuration: the authority public key comes from the
        # Spec180 trust-root registry, the recipient key is the Provider's
        # own Ed25519/EC identity key.  None means protected assignments fail
        # closed with DI_PROTECTED_GRANT_REJECTED.
        self._grant_authority_public_key = grant_authority_public_key
        self._grant_recipient_private_key = grant_recipient_private_key
        self._grant_fetch_timeout_ms = int(grant_fetch_timeout_ms)

    @property
    def provider_boot_epoch(self) -> str:
        """Return NDNSF Core's authoritative process-incarnation fence."""

        return self.provider.provider_boot_epoch

    @classmethod
    def create(
        cls,
        *,
        provider_id: str = "",
        group: str,
        controller: str,
        provider_prefix: str,
        trust_schema: str,
        handler_threads: int = 4,
        ack_threads: int = 2,
        handler_workers: int = 0,
        serve_certificates: bool = True,
        bootstrap_token: str = "",
        grant_authority_public_key=None,
        grant_recipient_private_key=None,
        grant_fetch_timeout_ms: int = 30000,
    ) -> "DistributedInferenceProvider":
        """Create an inference provider without exposing NDNSF Core objects."""

        return cls(ServiceProvider(
            provider_id=provider_id,
            group=group,
            controller=controller,
            provider_prefix=provider_prefix,
            trust_schema=trust_schema,
            handler_threads=handler_threads,
            ack_threads=ack_threads,
            serve_certificates=serve_certificates,
            bootstrap_token=bootstrap_token,
        ), handler_workers=handler_workers,
            grant_authority_public_key=grant_authority_public_key,
            grant_recipient_private_key=grant_recipient_private_key,
            grant_fetch_timeout_ms=grant_fetch_timeout_ms)

    def _run_handler(self, handler: InferenceHandler,
                     context: ProviderRuntimeContext) -> None:
        trace_handler_timing = os.environ.get("NDNSF_DI_PROVIDER_TIMING", "1") != "0"
        submitted_at = perf_counter()
        submitted_epoch_ms = int(time() * 1000)

        def run() -> None:
            started_at = perf_counter()
            started_epoch_ms = int(time() * 1000)
            queue_wait_ms = _elapsed_ms(submitted_at)
            if trace_handler_timing:
                print(
                    "NDNSF_DI_PROVIDER_HANDLER_TIMING "
                    f"event=start "
                    f"session={context.ndnsf.session_id} "
                    f"role={context.role} "
                    f"queue_wait_ms={queue_wait_ms:.2f} "
                    f"submitted_epoch_ms={submitted_epoch_ms} "
                    f"start_epoch_ms={started_epoch_ms}",
                    flush=True,
                )
            try:
                handler(context)
            finally:
                ended_epoch_ms = int(time() * 1000)
                if trace_handler_timing:
                    print(
                        "NDNSF_DI_PROVIDER_HANDLER_TIMING "
                        f"event=end "
                        f"session={context.ndnsf.session_id} "
                        f"role={context.role} "
                        f"handler_ms={_elapsed_ms(started_at):.2f} "
                        f"start_epoch_ms={started_epoch_ms} "
                        f"end_epoch_ms={ended_epoch_ms}",
                        flush=True,
                    )

        if self._handler_executor is None:
            run()
            return
        # CollaborationContext is owned by the active NDNSF callback. Wait for
        # the Python worker to complete before returning to keep it valid.
        self._handler_executor.submit(run).result()

    def _local_execution(
        self,
        role: str,
        *,
        backend: str,
        temp_dir: str | None,
        local_artifacts: dict[str, dict],
    ) -> ExecutionContext:
        root = Path(temp_dir) if temp_dir is not None else Path(tempfile.gettempdir())
        root.mkdir(parents=True, exist_ok=True)
        artifact = dict(local_artifacts.get(role, {}))
        artifact_paths = {}
        spec_artifacts = []
        path = artifact.get("path", "")
        if path:
            artifact_paths["model"] = Path(path)
            spec_artifacts.append(ExecutionArtifact(
                name="model",
                data_name="",
                filename=str(artifact.get("filename") or Path(path).name),
                sha256="",
                kind=str(artifact.get("kind") or "model"),
                chunks=[],
                executable=False,
                cache_name="",
            ))
        return ExecutionContext(
            spec=ExecutionArtifactSpec(
                role=role,
                backend=str(artifact.get("backend") or backend),
                entrypoint="",
                artifacts=spec_artifacts,
                metadata={
                    "deployedModel": True,
                    **dict(artifact.get("metadata") or {}),
                },
            ),
            artifact_paths=artifact_paths,
            work_dir=Path(tempfile.mkdtemp(
                prefix=f"ndnsf-{_safe_path_token(_validate_list_token(role, 'role'))}-",
                dir=str(root))),
        )

    def _assemble_certified_role_execution(
        self,
        ctx: CollaborationContext,
        execution: ExecutionContext,
        v3_role_spec: Any,
        local_artifacts: dict[str, dict],
    ) -> ExecutionContext:
        """Assemble one certified component-set subgraph from the canonical root.

        The sealed V3 projection carries the digest-pinned recipe (node cover,
        interface contracts, canonical identities).  This Provider-local
        boundary extracts exactly the certified nodes, verifies graph and
        initializer identity, loads the assembled bytes through ONNX Runtime,
        and swaps them in for this role's execution only.
        """
        from .adapters.onnx.executor import (
            CertifiedOnnxAssemblyRecipe, assemble_certified_onnx_model)
        artifact = dict(local_artifacts.get(ctx.assignment.role, {}))
        model_path = str(artifact.get("path", ""))
        if not model_path or not Path(model_path).is_file():
            raise RuntimeError(
                "certified role assembly requires the canonical model")
        recipe = CertifiedOnnxAssemblyRecipe(
            model_manifest_digest=v3_role_spec.model_manifest_digest,
            artifact_profile_digest=v3_role_spec.artifact_profile_digest,
            graph_digest=v3_role_spec.graph_digest,
            canonical_initializer_digest=v3_role_spec.canonical_initializer_digest,
            adapter_descriptor_digest=v3_role_spec.adapter_descriptor_digest,
            assembler_descriptor_digest=v3_role_spec.assembler_descriptor_digest,
            backend_abi=v3_role_spec.backend_abi,
            role_kind=v3_role_spec.role_kind,
            layer_begin=v3_role_spec.layer_begin,
            layer_end=v3_role_spec.layer_end,
            node_indices=tuple(int(index) for index in v3_role_spec.node_indices),
            input_names=tuple(
                str(item["name"]) for item in v3_role_spec.expected_inputs),
            output_names=tuple(
                str(item["name"]) for item in v3_role_spec.expected_outputs),
            expected_inputs=tuple(
                dict(item) for item in v3_role_spec.expected_inputs),
            expected_outputs=tuple(
                dict(item) for item in v3_role_spec.expected_outputs),
            precision=str(v3_role_spec.precision or "float32"),
            max_source_bytes=int(
                v3_role_spec.resource_envelope.get(
                    "maxSourceBytes", 8 * 1024**3)),
            max_assembled_bytes=int(
                v3_role_spec.resource_envelope.get(
                    "maxAssembledBytes", 8 * 1024**3)),
            max_nodes=int(
                v3_role_spec.resource_envelope.get("maxNodes", 1_000_000)),
        )
        if len(recipe.node_indices) >= recipe.max_nodes:
            # Assembling the whole canonical graph is an identity operation:
            # the certified node set covers every node, so the canonical
            # model itself is the certified slice (and re-inlining every
            # initializer would only exceed the assembled-byte envelope).
            return execution
        canonical_initializer: bytes | None = None
        weights_path = Path(model_path).with_name(
            Path(model_path).stem + ".weights")
        if weights_path.is_file():
            canonical_initializer = weights_path.read_bytes()
        assembly = assemble_certified_onnx_model(
            Path(model_path).read_bytes(),
            canonical_initializer=canonical_initializer,
            role_spec=v3_role_spec,
            recipe=recipe,
        )
        assembled_path = execution.work_dir / "assembled-role.onnx"
        assembled_path.write_bytes(assembly.model_bytes)
        return replace(
            execution,
            artifact_paths={
                **dict(execution.artifact_paths),
                "model": assembled_path,
            },
            spec=replace(
                execution.spec,
                metadata={
                    **dict(execution.spec.metadata),
                    "assembledModelDigest": assembly.model_digest,
                    "assembledNodeCount": assembly.node_count,
                },
            ),
        )

    def _qualify_protected_assembly(
        self,
        ctx: CollaborationContext,
        execution: ExecutionContext,
        v3_projection: Any,
        v3_role_spec: Any,
        *,
        _fetch_grant_data=None,
    ) -> tuple[ExecutionContext, "PlaintextLeaseRegistry"]:
        """Fetch, verify, and unwrap the Provider-bound grant (spec181 T001),
        then seal the assembled artifact under the unwrapped content key
        (FR-013: the content key is consumed by a real AEAD operation, never
        left idle).

        Failure modes register DI_PROTECTED_GRANT_REJECTED and fail closed
        before the artifact is exposed to the adapter: a synthetic rejection
        may never stand in for a real verifier decision.
        """
        from ndnsf import fetch_exact_data_packet
        from .core import (
            PlaintextLeaseRegistry, ProtectedGrantRejected,
            decrypt_assembled_entry, encrypt_assembled_entry,
            grant_from_wire, verify_and_unwrap_grant)
        if (self._grant_authority_public_key is None
                or self._grant_recipient_private_key is None):
            raise ProtectedGrantRejected(
                "protected grant keys are not configured for this Provider")
        grant_binding = v3_projection.grant_binding
        if grant_binding is None:
            raise ProtectedGrantRejected(
                "protected V3 Selection carries no Provider grant binding")
        lease_registry = PlaintextLeaseRegistry()
        try:
            grant_hints = [
                hint for hint in os.environ.get(
                    "SPEC181_GRANT_FORWARDING_HINT", "").split(",")
                if hint.strip()]
            fetch_grant = _fetch_grant_data or (
                lambda name: fetch_exact_data_packet(
                    name, timeout_ms=self._grant_fetch_timeout_ms,
                    forwarding_hints=grant_hints))
            try:
                packet = fetch_grant(grant_binding.grant_name)
            except Exception as exc:
                raise ProtectedGrantRejected(
                    f"grant Data fetch failed at {grant_binding.grant_name}: "
                    f"{type(exc).__name__}: {exc}") from exc
            try:
                grant = grant_from_wire(packet.content)
            except ValueError as exc:
                raise ProtectedGrantRejected(
                    "grant Data payload is malformed") from exc
            # The native Merge role carries no assembly identity (no ONNX
            # layer); its grant still binds the same canonical manifest as
            # the component roles, so fall back to the grant's own manifest
            # for the binding comparison when the projection omits it.
            expected_manifest = (
                v3_role_spec.model_manifest_digest
                or grant.model_manifest_digest)
            try:
                content_key = verify_and_unwrap_grant(
                    grant,
                    authority_public_key=self._grant_authority_public_key,
                    recipient_private_key=self._grant_recipient_private_key,
                    expected_provider_identity=ctx.local_provider,
                    expected_request_id=v3_projection.request_id,
                    expected_attempt=v3_projection.attempt,
                    expected_plan_core_digest=v3_projection.plan_core_digest,
                    expected_model_manifest_digest=expected_manifest,
                    expected_protection_epoch=v3_role_spec.protection_epoch,
                    now_ms=int(time() * 1000),
                )
            except ValueError as exc:
                raise ProtectedGrantRejected(
                    f"grant verifier rejected: {exc}") from exc
            # FR-013: register the unwrapped content key as a zeroizable
            # plaintext lease, then seal the assembled artifact under a key
            # derived from it (DISK_CIPHERTEXT_ASSEMBLED semantics).
            role_assembly_spec_digest = canonical_digest(v3_role_spec)
            storage_profile_digest = canonical_digest(
                _PROTECTED_ASSEMBLY_STORAGE_PROFILE)
            model_path = Path(str(execution.artifact_paths.get("model", "")))
            if not model_path.is_file():
                raise ProtectedGrantRejected(
                    "protected assembly has no assembled artifact to seal")
            assembled_bytes = model_path.read_bytes()
            sealed = encrypt_assembled_entry(
                content_key, assembled_bytes, entry_kind="MODEL_PROTO",
                model_manifest_digest=expected_manifest,
                role_assembly_spec_digest=role_assembly_spec_digest,
                storage_profile_digest=storage_profile_digest,
            )
            cipher_path = execution.work_dir / "assembled-role.onnx.cipher"
            cipher_path.write_bytes(sealed.to_bytes())
            try:
                plaintext = decrypt_assembled_entry(content_key, sealed)
            except ValueError as exc:
                raise ProtectedGrantRejected(
                    "assembled artifact failed AEAD authentication") from exc
            lease_registry.register(
                "protected-content-key",
                execution.work_dir / "content-key.bin", content_key)
            lease_registry.register(
                "assembled-model-plaintext", model_path, plaintext)
            return execution, lease_registry
        except ProtectedGrantRejected:
            lease_registry.zeroize_all()
            raise
        except Exception as exc:
            lease_registry.zeroize_all()
            raise ProtectedGrantRejected(
                f"protected grant qualification failed: {exc}") from exc

    @staticmethod
    def _report_preparation(
        ctx: CollaborationContext,
        *,
        phase: str,
        sequence: int,
        progress: float,
        execution: ExecutionContext | None = None,
        reason: str = "",
        attempt: int = 1,
    ) -> None:
        """Publish bounded observational progress on the generic NDNSF channel.

        These snapshots are Provider-signed by the existing SELECTION-STATUS
        reply path.  They deliberately do not contain or replace the separate
        DI readiness certificate required to activate a deployment.
        """
        details = {"schema": "ndnsf-di-preparation-progress-v1", "phase": phase}
        if execution is not None:
            details.update({
                "adapter": execution.spec.backend,
                "artifactDigests": [
                    item.sha256 for item in execution.spec.artifacts or []
                    if item.sha256
                ],
                "planDigest": str(execution.spec.metadata.get(
                    "evidence.planDigest", execution.spec.metadata.get(
                        "planDigest", ""))),
                "deploymentRevision": str(execution.spec.metadata.get(
                    "deploymentRevision", "")),
            })
        ctx.report_operation_status(ServiceOperationStatus(
            operation_id=f"prepare:{ctx.assignment.role}",
            operation="prepare-inference-role",
            service_name=ctx.assignment.service,
            provider_name=ctx.local_provider,
            request_id=ctx.session_id,
            role=ctx.assignment.role,
            attempt=attempt,
            # Core keys this operation by Selection digest. Its local status
            # epoch is independent of the ACK attempt and Provider boot epoch.
            epoch=1,
            sequence=sequence,
            state=(ServiceOperationState.FAILED if phase == "FAILED"
                   else ServiceOperationState.DONE if phase == "READY"
                   else ServiceOperationState.RUNNING),
            reason_code=reason,
            message=reason or phase.lower(),
            progress_known=True,
            progress=progress,
            details_schema="ndnsf-di-preparation-progress-v1",
            details_payload=json.dumps(
                details, sort_keys=True, separators=(",", ":")).encode(),
        ))

    @staticmethod
    def _bind_assignment_metadata(
        ctx: CollaborationContext, execution: ExecutionContext,
    ) -> ExecutionContext:
        fields = {}
        for item in bytes(ctx.assignment.assignment_payload).decode(
                "utf-8", errors="replace").split(";"):
            if "=" in item:
                key, value = item.split("=", 1)
                fields[key] = value
        revision = fields.get("deploymentRevision", "")
        if not revision:
            return execution
        return replace(execution, spec=replace(
            execution.spec, metadata={
                **dict(execution.spec.metadata),
                "deploymentRevision": revision,
            }))

    @staticmethod
    def _await_all_role_readiness(
        ctx: CollaborationContext,
        execution: ExecutionContext,
        *,
        timeout_ms: int = 30_000,
    ) -> None:
        """Rollback-only V1 exact-member readiness barrier."""
        scope = "ndnsf-di-readiness-v1"
        topic = "/ndnsf-di/readiness"
        assignment = ctx.assignment
        expected = dict(assignment.role_providers)
        expected.setdefault(assignment.role, ctx.local_provider)
        assignment_fields = {}
        for item in bytes(assignment.assignment_payload).decode(
                "utf-8", errors="replace").split(";"):
            if "=" in item:
                key, value = item.split("=", 1)
                assignment_fields[key] = value
        if assignment_fields.get("executionPolicy") != LEGACY_READY_SET_V1:
            raise RuntimeError(
                "DI_LEGACY_READY_SET_REQUIRES_EXPLICIT_POLICY")
        activation_digest = assignment_fields.get(
            "executionActivationDigest", "")
        activation_members = {
            item for item in assignment_fields.get(
                "executionActivationMembers", "").split(",") if item
        }
        local_member = assignment_fields.get(
            "executionActivationLocalMember", "")
        activation_bound = bool(
            activation_digest and activation_members and local_member)
        if activation_bound and local_member not in activation_members:
            raise RuntimeError("DI_READINESS_ACTIVATION_MEMBER_MISMATCH")
        declared_roles = {
            item for item in assignment_fields.get(
                "readinessRoles", "").split(",") if item
        }
        try:
            declared_count = int(assignment_fields.get(
                "readinessRoleCount", "0"))
        except ValueError:
            declared_count = 0
        declared_binding = assignment_fields.get(
            "readinessBindingDigest", "")
        declared_bound = bool(
            not activation_bound and declared_binding and declared_roles and
            declared_count == len(declared_roles) and
            assignment.role in declared_roles)
        binding_digest = (
            activation_digest if activation_bound else
            declared_binding if declared_bound else assignment.selection_digest)
        expected_count = (
            len(activation_members) if activation_bound else
            len(declared_roles) if declared_bound else len(expected))
        metadata = dict(execution.spec.metadata)
        revision = str(metadata.get("deploymentRevision", ""))
        plan_digest = str(metadata.get(
            "evidence.planDigest", metadata.get("planDigest", revision)))
        payload_value = {
            "schema": "ndnsf-di-readiness-v1",
            "revision": revision,
            "planDigest": plan_digest,
            "bindingDigest": binding_digest,
            "memberId": local_member if activation_bound else assignment.role,
            "role": assignment.role,
            "provider": ctx.local_provider,
            "adapter": execution.spec.backend,
            "artifactDigests": sorted(
                item.sha256 for item in execution.spec.artifacts or []
                if item.sha256),
        }
        payload = json.dumps(
            payload_value, sort_keys=True, separators=(",", ":")).encode()
        ctx.publish(scope, topic, payload)

        observed = {
            local_member if activation_bound else assignment.role:
                (ctx.local_provider, payload)
        }
        role_owners = {assignment.role: ctx.local_provider}
        deadline = monotonic() + max(1, timeout_ms) / 1000.0
        next_publish = monotonic() + 0.25
        while len(observed) < expected_count and monotonic() < deadline:
            if monotonic() >= next_publish:
                ctx.publish(scope, topic, payload)
                next_publish = monotonic() + 0.25
            remaining_ms = max(1, min(100, int((deadline - monotonic()) * 1000)))
            for item in ctx.wait_for(scope, topic, 1, remaining_ms):
                try:
                    value = json.loads(bytes(item.payload).decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    raise RuntimeError("DI_READINESS_PAYLOAD_INVALID") from exc
                role = str(value.get("role", ""))
                provider = str(value.get("provider", ""))
                member_id = str(value.get("memberId", ""))
                exact_member = (
                    member_id in activation_members if activation_bound else
                    role in declared_roles if declared_bound else role in expected)
                exact_provider = (
                    bool(provider) and item.producer == provider
                    if activation_bound else
                    (provider == expected[role] and item.producer == provider
                     if role in expected else
                     declared_bound and bool(provider) and
                     item.producer == provider))
                if (value.get("schema") != "ndnsf-di-readiness-v1" or
                        not exact_member or not exact_provider or not role or
                        item.producer_role != role or item.producer != provider or
                        str(value.get("revision", "")) != revision or
                        str(value.get("planDigest", "")) != plan_digest or
                        str(value.get("bindingDigest", "")) != binding_digest or
                        not value.get("adapter")):
                    raise RuntimeError("DI_READINESS_BINDING_MISMATCH")
                encoded = json.dumps(
                    value, sort_keys=True, separators=(",", ":")).encode()
                previous_owner = role_owners.get(role)
                if previous_owner is not None and previous_owner != provider:
                    raise RuntimeError("DI_READINESS_ROLE_CONFLICT")
                previous = observed.get(member_id)
                if previous is not None and previous != (provider, encoded):
                    raise RuntimeError("DI_READINESS_REPLAY_CONFLICT")
                role_owners[role] = provider
                observed[member_id] = (provider, encoded)
            if len(observed) < expected_count:
                sleep(0.005)
        if len(observed) != expected_count:
            raise TimeoutError("DI_READINESS_BARRIER_TIMEOUT")

    def add_role(
        self,
        service: str,
        role: str,
        handler: InferenceHandler,
        *,
        temp_dir: str | None = None,
        queue_depth: int = 0,
        allow_executables: bool = False,
        dependency_graph=None,
    ) -> None:
        safe_role = _validate_list_token(role, "role")

        def ack(_payload: bytes) -> AckDecision:
            return AckDecision(
                status=True,
                message=f"inference role {safe_role} ready",
                payload=f"role={safe_role};queue={queue_depth};".encode(),
            )

        def wrapped(ctx: CollaborationContext, request: bytes) -> None:
            sequence = 1
            self._report_preparation(
                ctx, phase="ACCEPTED", sequence=sequence, progress=0.0)

            def report(phase: str, value: float) -> None:
                nonlocal sequence
                sequence += 1
                self._report_preparation(
                    ctx, phase=phase, sequence=sequence, progress=value)

            try:
                execution = prepare_execution(
                    ctx,
                    temp_root=temp_dir,
                    allow_executables=allow_executables,
                    progress=report,
                )
            except Exception as exc:
                sequence += 1
                self._report_preparation(
                    ctx, phase="FAILED", sequence=sequence, progress=0.0,
                    reason=type(exc).__name__)
                ctx.fail(f"failed to prepare inference execution: {exc}")
                return

            execution = self._bind_assignment_metadata(ctx, execution)

            sequence += 1
            self._report_preparation(
                ctx, phase="WARMING", sequence=sequence, progress=0.90,
                execution=execution)
            sequence += 1
            self._report_preparation(
                ctx, phase="READY", sequence=sequence, progress=1.0,
                execution=execution)

            # Core has already validated the exact signed READY set and the
            # requester activation before invoking this DI handler. Do not
            # create a second peer-to-peer readiness authority here.

            prefetcher = DependencyPrefetcher(ctx)
            try:
                self._run_handler(handler, ProviderRuntimeContext(
                    ndnsf=ctx,
                    execution=execution,
                    request=request,
                    role=ctx.assignment.role,
                    dependencies=(dependency_graph.for_role(ctx.assignment.role)
                                  if dependency_graph is not None
                                  else RoleDependencyView(ctx.assignment.role)),
                    prefetcher=prefetcher,
                    deadline_ms=_assignment_deadline_ms(
                        bytes(ctx.assignment.assignment_payload)),
                    stream_writer=(ctx if getattr(ctx, "is_streamed", False)
                                   else None),
                ))
            finally:
                prefetcher.shutdown()

        self.provider.add_collaboration_handler(service, [safe_role], wrapped, ack)

    def add_capability_handler(
        self,
        service: str,
        roles: Sequence[str],
        handler: InferenceHandler,
        *,
        backends: Sequence[str] = (),
        temp_dir: str | None = None,
        queue_depth: int = 0,
        has_model: bool = False,
        can_provision: bool = True,
        allow_executables: bool = False,
        dependency_graph=None,
        local_artifacts: dict[str, dict] | None = None,
        readiness_probe: Callable[[], AckDecision | bool] | None = None,
        provider_profile: ProviderProfileV1 | dict | None = None,
        runtime_telemetry: Callable[[], RuntimeTelemetryV1 | dict] | RuntimeTelemetryV1 | dict | None = None,
        admission_policy: ProviderAdmissionPolicy | None = None,
        reservation_book: AtomicReservationBook | None = None,
        reservation_authorizer: Callable[[Mapping[str, object]], bool] | None = None,
        conflict_admission_gate: Callable[[Mapping[str, object]], Mapping[str, object] | bool] | None = None,
        require_conflict_admission: bool = False,
        reservation_resource_id: str = "",
        reservation_resource_sequence: int = 0,
        reservation_units: int = 1,
        reservation_lease_ms: int = 5000,
        reservation_signature: str = "provider-reservation-signature",
        register_simple_service: bool = False,
        ready_without_model: bool = False,
        selection_offer_issuer: DIProviderOfferIssuer | None = None,
        selection_offer_issuer_v3: DIProviderOfferIssuerV3 | None = None,
        selection_participant: DISelectionParticipant | None = None,
        selection_wal_path: str | Path | None = None,
        selection_storage_key: bytes | None = None,
        selection_storage_key_epoch: str = "",
        selection_max_prepare_ms: int = 1000,
        selection_cached_shards: Callable[[], Sequence[object]] | None = None,
        selection_reusable_state: Callable[[], Sequence[object]] | None = None,
        runtime_preparer: Callable | None = None,
    ) -> None:
        """Register one provider as capable of serving multiple inference roles.

        Providers normally use locally deployed artifacts recorded in the
        service policy. If an assignment carries an artifact name, the provider
        can still fetch and materialize it for compatibility with older dynamic
        provisioning flows.
        """

        role_list = [_validate_list_token(str(role), "role") for role in roles]
        if not role_list:
            raise ValueError("at least one role capability is required")
        backend_list = [_validate_list_token(str(backend), "backend")
                        for backend in backends]
        if selection_offer_issuer is not None and not backend_list:
            raise ValueError("V2 Selection offers require at least one backend")
        if selection_participant is not None:
            if (selection_offer_issuer is None or selection_wal_path is None
                    or not selection_storage_key
                    or not selection_storage_key_epoch):
                raise ValueError(
                    "V2 Selection participant requires issuer, WAL, and storage key")
            register_selection_dataflow_v2(
                self.provider,
                service=service,
                participant=selection_participant,
                wal_path=selection_wal_path,
                storage_key=selection_storage_key,
                storage_key_epoch=selection_storage_key_epoch,
                max_prepare_ms=selection_max_prepare_ms,
            )
        local_artifacts = dict(local_artifacts or {})
        # A role-keyed, already validated local artifact is a cold-preparation
        # capability. It is distinct from ``has_model``: that flag is only a
        # coarse readiness hint and cannot authorize exact reuse. The role key
        # also prevents material for another role from becoming a capability
        # for this Provider's selected role.
        local_preparation_roles = frozenset(
            role for role in role_list
            if isinstance(local_artifacts.get(role), Mapping)
            and str(local_artifacts[role].get("path", "")).strip())
        local_preparation_available = bool(local_preparation_roles)

        def attach_negotiated_reservation(
            context: Mapping[str, object], decision: AckDecision,
        ) -> AckDecision:
            if context.get("_selected_execution") is True:
                return decision
            capabilities = dict(context.get("request_capabilities", {}) or {})
            if capabilities.get("DIReservationSelectionV1") != "required":
                return decision
            if not decision.status:
                return decision
            if reservation_book is None or reservation_authorizer is None:
                return AckDecision(status=False, message="DI_RESERVATION_UNAVAILABLE")
            intent = dict(context.get("deployment_intent", {}) or {})
            conflict_fields: dict[str, object] = {}
            conflict_required = capabilities.get("DIConflictAdmissionV1") == "required"
            if require_conflict_admission and not conflict_required:
                return AckDecision(
                    status=False, message="DI_CONFLICT_ADMISSION_REQUIRED")
            if conflict_required:
                if conflict_admission_gate is None:
                    return AckDecision(
                        status=False, message="DI_CONFLICT_ADMISSION_UNAVAILABLE")
                try:
                    gate_result = conflict_admission_gate(context)
                    if isinstance(gate_result, Mapping):
                        conflict_fields = dict(gate_result)
                        admitted = bool(conflict_fields.get("admitted", True))
                    else:
                        admitted = bool(gate_result)
                    if not admitted:
                        return AckDecision(
                            status=False, message="DI_CONFLICT_ADMISSION_REJECTED")
                    canonical_resource_id = str(
                        conflict_fields.get("canonicalResourceId", ""))
                    resource_sequence = int(
                        conflict_fields.get("resourceSequence", 0))
                    if not canonical_resource_id or resource_sequence <= 0:
                        return AckDecision(
                            status=False, message="DI_CONFLICT_ADMISSION_INVALID")
                    if (reservation_resource_id and
                            canonical_resource_id != reservation_resource_id):
                        return AckDecision(
                            status=False, message="DI_CONFLICT_RESOURCE_MISMATCH")
                except Exception as exc:  # fail closed before local reserve
                    return AckDecision(
                        status=False,
                        message=f"DI_CONFLICT_ADMISSION_REJECTED:{exc}")
            else:
                canonical_resource_id = str(reservation_resource_id)
                resource_sequence = int(reservation_resource_sequence)
            try:
                lease = reservation_book.reserve(
                    requester=str(intent["requesterIdentity"]), service=service,
                    request_id=str(intent["requestId"]),
                    attempt=int(intent.get("attempt", "1")),
                    units=int(reservation_units), now_ms=int(time() * 1000),
                    requested_lease_ms=int(reservation_lease_ms),
                    authorized=bool(reservation_authorizer(context)),
                    signature=reservation_signature,
                    canonical_resource_id=canonical_resource_id,
                    resource_sequence=resource_sequence)
            except PermissionError:
                return AckDecision(status=False, message="DI_RESERVATION_UNAUTHORIZED")
            except (KeyError, TypeError, ValueError, RuntimeError) as exc:
                return AckDecision(status=False, message=f"DI_RESERVATION_REJECTED:{exc}")
            lease_fields = dict(lease.fields)
            if conflict_fields:
                for target, source in (
                        ("conflictPermitId", "permitId"),
                        ("conflictAuthorityEpoch", "authorityEpoch"),
                        ("conflictAuthorityDigest", "authorityDigest")):
                    value = str(conflict_fields.get(source, ""))
                    if value:
                        lease_fields[target] = value
            return replace(decision, reservation_lease=lease_fields)

        def ack(context: Mapping[str, object], _payload: bytes) -> AckDecision:
            v2_request = False
            v3_request = False
            v2_envelope: DIRequestEnvelopeV2 | None = None
            v3_request_document: Mapping[str, object] | None = None
            try:
                request_document = json.loads(bytes(_payload).decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                request_document = None
            if (isinstance(request_document, dict)
                    and request_document.get("schema")
                    == "ndnsf-di-request-envelope-v2"):
                task_document = request_document.get("task", {})
                if not isinstance(task_document, Mapping):
                    task_document = {}
                placement_profile = (
                    request_document.get("placementProfile")
                    or task_document.get("placement_profile")
                    or task_document.get("placementProfile"))
                v3_request = placement_profile == "DI_PLACEMENT_V3"
                if v3_request:
                    v3_request_document = request_document
                if v3_request and selection_offer_issuer_v3 is None:
                    return AckDecision(
                        status=False, message="DI_PLACEMENT_V3_UNAVAILABLE")
                try:
                    v2_envelope = DIRequestEnvelopeV2.from_bytes(_payload)
                except ValueError as exc:
                    return AckDecision(
                        status=False,
                        message=f"DI_V2_REQUEST_REJECTED:{exc}")
                v2_request = True
                if not v3_request and selection_offer_issuer is None:
                    return AckDecision(
                        status=False,
                        message="DI_SELECTION_DATAFLOW_V2_UNAVAILABLE")
            readiness_fields: dict[str, object] = {}
            if readiness_probe is not None:
                readiness = readiness_probe()
                if isinstance(readiness, AckDecision):
                    if readiness.payload:
                        parsed = parse_ack_metadata(bytes(readiness.payload))
                        if "providerCapabilityHint" in parsed:
                            decoded = decode_provider_capability_ack(bytes(readiness.payload))
                            readiness_fields.update(decoded.hint.service_payload)
                        else:
                            readiness_fields.update(parsed)
                    if not readiness.status:
                        provider_name = getattr(self.provider, "provider", "")
                        reason = readiness.message or NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE
                        return AckDecision(
                            status=False,
                            message=reason,
                            payload=encode_provider_capability_ack(ProviderCapabilityHint(
                                provider_name=str(provider_name or "unknown-provider"),
                                service_name=service,
                                ready=False,
                                reason_code=reason,
                                message=reason,
                                runtime_hint=GenericProviderRuntimeHint(
                                    provider_name=str(provider_name or "unknown-provider")),
                                service_payload_schema="ndnsf-di-capability-v1",
                                service_payload=readiness_fields,
                            )),
                        )
                else:
                    if not bool(readiness):
                        provider_name = getattr(self.provider, "provider", "")
                        return AckDecision(
                            status=False,
                            message=NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
                            payload=encode_provider_capability_ack(ProviderCapabilityHint(
                                provider_name=str(provider_name or "unknown-provider"),
                                service_name=service,
                                ready=False,
                                reason_code=NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
                                message="model unavailable",
                                runtime_hint=GenericProviderRuntimeHint(
                                    provider_name=str(provider_name or "unknown-provider")),
                                service_payload_schema="ndnsf-di-capability-v1",
                                service_payload={"runtimeStatus": "installing"},
                            )),
                        )
            fields: dict[str, object] = {
                "roles": role_list,
                "queue": queue_depth,
                "hasModel": has_model,
                "canProvision": can_provision,
                "canPrepare": local_preparation_available,
                "readyWithoutModel": ready_without_model,
                **readiness_fields,
            }
            if len(role_list) == 1:
                fields["role"] = role_list[0]
            if backend_list:
                fields["backends"] = backend_list
            if provider_profile is not None:
                profile = (
                    provider_profile
                    if isinstance(provider_profile, ProviderProfileV1)
                    else ProviderProfileV1.from_dict(dict(provider_profile))
                )
                fields.update(profile.to_ack_fields())
            telemetry: RuntimeTelemetryV1 | None = None
            if runtime_telemetry is not None:
                telemetry_value = runtime_telemetry() if callable(runtime_telemetry) else runtime_telemetry
                telemetry = (
                    telemetry_value
                    if isinstance(telemetry_value, RuntimeTelemetryV1)
                    else RuntimeTelemetryV1.from_dict(dict(telemetry_value))
                )
                fields.update(telemetry.to_ack_fields())
            provider_name = getattr(self.provider, "provider", "") or str(fields.get("provider", ""))
            runtime_hint = GenericProviderRuntimeHint(
                provider_name=str(provider_name or "unknown-provider"),
                active_work_count=telemetry.active_workers if telemetry is not None else 0,
                queue_length=telemetry.aggregate_queue if telemetry is not None else queue_depth,
                estimated_queue_wait_ms=telemetry.queue_wait_ewma_ms if telemetry is not None else 0.0,
                capacity_hints={
                    "roles": role_list,
                    "backends": backend_list,
                    "hasModel": has_model,
                    "canProvision": can_provision,
                    "canPrepare": local_preparation_available,
                    **({
                        "freeMemoryMb": telemetry.free_memory_mb,
                        "runtimeBackend": telemetry.runtime_backend,
                        "modelLoaded": telemetry.model_loaded,
                    } if telemetry is not None else {}),
                },
            )
            if not (can_provision or has_model or ready_without_model
                    or local_preparation_available):
                fields["negativeAckReason"] = NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE
                fields["status"] = "model-unavailable"
                capability_hint = ProviderCapabilityHint(
                    provider_name=runtime_hint.provider_name,
                    service_name=service,
                    ready=False,
                    reason_code=NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
                    message="model unavailable",
                    runtime_hint=runtime_hint,
                    service_payload_schema="ndnsf-di-capability-v1",
                    service_payload={key: to_plain(value) for key, value in fields.items()},
                )
                return AckDecision(
                    status=False,
                    message=NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
                    payload=encode_provider_capability_ack(capability_hint),
                )
            if admission_policy is not None and telemetry is not None:
                accepted, reason, diagnostics = admission_policy.evaluate(telemetry)
                fields.update(diagnostics)
                if not accepted:
                    fields["negativeAckReason"] = reason
                    fields["status"] = "admission-rejected"
                    capability_hint = ProviderCapabilityHint(
                        provider_name=runtime_hint.provider_name,
                        service_name=service,
                        ready=False,
                        reason_code=reason,
                        message="admission rejected",
                        runtime_hint=runtime_hint,
                        service_payload_schema="ndnsf-di-capability-v1",
                        service_payload={key: to_plain(value) for key, value in fields.items()},
                    )
                    return AckDecision(
                        status=False,
                        message=reason,
                        payload=encode_provider_capability_ack(capability_hint),
                    )
            if v2_request and v3_request:
                assert selection_offer_issuer_v3 is not None
                assert v2_envelope is not None
                document = dict(v3_request_document or {})
                graph_digest = str(
                    document.get("graphDigest")
                    or dict(document.get("task", {}) or {}).get("graph_digest", ""))
                # The normal request deliberately reaches Providers before
                # graph inspection.  A missing graph digest therefore means a
                # signed wildcard offer, not a malformed request.  The
                # coordinator binds the actual digest after ACK_CLOSED.
                if not graph_digest.startswith("sha256:"):
                    graph_digest = UNBOUND_GRAPH_DIGEST_V3
                cached_state = tuple(
                    selection_reusable_state()
                    if selection_reusable_state is not None else ())
                cached_proofs = tuple(
                    item for item in cached_state
                    if isinstance(item, ResidencyProofV3))
                # ``has_model`` is only a coarse readiness hint.  It cannot
                # prove an exact post-ACK role/rank identity, so it must not
                # manufacture ACCEPT_IF_EXACT_REUSE.  Exact reuse is offered
                # only with explicit, signed residency proofs.
                exact_reuse_available = bool(cached_proofs)
                # Exact residency is the strongest admissible disposition.
                # A Provider may still be able to prepare a cold artifact, but
                # that must not downgrade a warm, exact hit to a rejection or
                # force an unnecessary preparation path.  Preparation is the
                # fallback only when no complete proof is available.
                if exact_reuse_available:
                    v3_disposition = ExecutionDisposition.ACCEPT_IF_EXACT_REUSE
                    v3_preparation_accepted = False
                elif (can_provision or ready_without_model
                      or local_preparation_available):
                    v3_disposition = ExecutionDisposition.ACCEPT_WITH_PREPARATION
                    v3_preparation_accepted = True
                else:
                    v3_disposition = ExecutionDisposition.REJECT
                    v3_preparation_accepted = False
                decision = selection_offer_issuer_v3.issue(
                    request_id=v2_envelope.request_id,
                    attempt=v2_envelope.attempt,
                    model_digest=v2_envelope.model_identity_hash,
                    graph_digest=graph_digest,
                    deadline_ms=v2_envelope.plan_deadline_ms,
                    accepted_roles=role_list, backends=backend_list,
                    execution_disposition=v3_disposition,
                    preparation_accepted=v3_preparation_accepted,
                    can_provision=bool(can_provision),
                    has_model=bool(has_model),
                    queue_depth=(telemetry.aggregate_queue
                                 if telemetry is not None else queue_depth),
                    estimated_wait_ms=(telemetry.queue_wait_ewma_ms
                                       if telemetry is not None else 0.0),
                    residency=cached_proofs,
                )
                print(
                    "NDNSF_DI_ACK_DECISION",
                    f"requestId={v2_envelope.request_id}",
                    f"attempt={v2_envelope.attempt}",
                    f"status={str(bool(decision.status)).lower()}",
                    "reservationHeld=false", "v3=true",
                    f"reason={decision.message or '-'}", flush=True)
                return decision
            if v2_request:
                assert selection_offer_issuer is not None
                assert v2_envelope is not None
                current_cached_shards = (
                    tuple(selection_cached_shards())
                    if selection_cached_shards is not None
                    else tuple(fields.get("cachedShards", ()) or ())
                )
                current_reusable_state = (
                    tuple(selection_reusable_state())
                    if selection_reusable_state is not None
                    else tuple(fields.get("reusableState", ()) or ())
                )
                decision = selection_offer_issuer.issue(
                    _payload, accepted_roles=role_list,
                    backends=backend_list,
                    queue_depth=(
                        telemetry.aggregate_queue
                        if telemetry is not None else queue_depth),
                    estimated_wait_ms=(
                        telemetry.queue_wait_ewma_ms
                        if telemetry is not None else None),
                    cached_shards=current_cached_shards,
                    reusable_state=current_reusable_state,
                )
                print(
                    "NDNSF_DI_ACK_DECISION",
                    f"requestId={v2_envelope.request_id}",
                    f"attempt={v2_envelope.attempt}",
                    f"status={str(bool(decision.status)).lower()}",
                    f"pendingStateTtlMs={decision.pending_state_ttl_ms}",
                    "reservationHeld="
                    f"{str(bool(decision.status)).lower()}",
                    f"reason={decision.message or '-'}",
                    flush=True,
                )
                return decision
            capability_hint = ProviderCapabilityHint(
                provider_name=runtime_hint.provider_name,
                service_name=service,
                ready=True,
                message="inference capability ready",
                runtime_hint=runtime_hint,
                service_payload_schema="ndnsf-di-capability-v1",
                service_payload={key: to_plain(value) for key, value in fields.items()},
            )
            return attach_negotiated_reservation(context, AckDecision(
                status=True,
                message="inference capability ready",
                payload=encode_provider_capability_ack(capability_hint),
            ))

        decision_authority = (ReservationDecisionAuthority(reservation_book)
                              if reservation_book is not None else None)

        def register_decision_authority() -> None:
            if decision_authority is None:
                return

            def apply_decision(fields: Mapping[str, str]) -> Mapping[str, str]:
                receipt = decision_authority.apply(
                    SelectionDecision(dict(fields)), now_ms=int(time() * 1000))
                return receipt.fields

            self.provider.set_r1_selection_decision_handler(
                service, apply_decision)
            self.provider.set_r1_reservation_terminal_handler(
                service,
                lambda reservation_id, cause: reservation_book.release(
                    reservation_id, reason=str(cause),
                    now_ms=int(time() * 1000)))

        class SimpleResponseContext:
            session_id = "simple-service"

            def __init__(self) -> None:
                self.response = ServiceResponse(status=False, error="no response published")

            def publish_final_response(self, payload: bytes) -> None:
                self.response = ServiceResponse(status=True, payload=bytes(payload))

            def fail(self, error: str) -> None:
                self.response = ServiceResponse(status=False, error=str(error))

        if register_simple_service:
            if len(role_list) != 1:
                raise ValueError("simple service mirror requires exactly one role")
            simple_role = role_list[0]

            def simple_handler(request: bytes) -> ServiceResponse:
                try:
                    readiness = ack({"_selected_execution": True}, request)
                    if not readiness.status:
                        return ServiceResponse(status=False, error=readiness.message)
                    execution = self._local_execution(
                        simple_role,
                        backend=backend_list[0] if backend_list else "",
                        temp_dir=temp_dir,
                        local_artifacts=local_artifacts,
                    )
                    simple_ctx = SimpleResponseContext()
                    self._run_handler(handler, ProviderRuntimeContext(
                        ndnsf=simple_ctx,
                        execution=execution,
                        request=request,
                        role=simple_role,
                        dependencies=RoleDependencyView(simple_role),
                        prefetcher=None,
                        deadline_ms=0,
                    ))
                    return simple_ctx.response
                except Exception as exc:  # noqa: BLE001
                    return ServiceResponse(status=False, error=str(exc))

            self.provider.add_handler(service, simple_handler)
            self.provider.set_ack_context_handler(service, ack)
            register_decision_authority()
            return

        def wrapped(ctx: CollaborationContext, request: bytes) -> None:
            from .core import ProtectedGrantRejected
            sequence = 1
            assignment_payload = bytes(ctx.assignment.assignment_payload or b"")
            terminal_released = False
            role_assignment = None
            committed_dependency_view = None
            v3_projection = None
            v3_role_spec = None
            v3_dependency_view = None
            protected_lease_registry = None
            # These flags are derived only from the authenticated V3
            # RoleDataflowContract.  Keep them false for legacy V2/simple
            # assignments so the compatibility path remains unchanged.
            v3_input_ingress_owner = False
            v3_terminal_response_owner = False
            if (selection_offer_issuer_v3 is not None
                    or assignment_payload.lstrip().startswith(b"{")):
                try:
                    decoded_projection = ProviderSelectionProjectionV3.from_bytes(
                        assignment_payload)
                except (ValueError, TypeError):
                    # A V3 Provider must never downgrade a rejected Selection
                    # into the legacy local-artifact path. Unconfigured legacy
                    # handlers may still receive a different JSON envelope,
                    # but a declared V3 schema retains the same strict boundary.
                    if (selection_offer_issuer_v3 is not None
                            or len(assignment_payload) > 1024 * 1024):
                        ctx.fail("DI_V3_SELECTION_INVALID")
                        return
                    try:
                        envelope = json.loads(assignment_payload)
                    except (ValueError, UnicodeError):
                        ctx.fail("DI_V3_SELECTION_INVALID")
                        return
                    if (not isinstance(envelope, dict)
                            or envelope.get("schema") == "ndnsf-di-selection-v3"):
                        ctx.fail("DI_V3_SELECTION_INVALID")
                        return
                    decoded_projection = None
                if decoded_projection is not None:
                    v3_projection = decoded_projection
                    if decoded_projection.provider != ctx.local_provider:
                        ctx.fail("V3 Selection Provider binding mismatch")
                        return
                    request_contract_digest = (
                        "sha256:" + __import__("hashlib").sha256(
                            bytes(request)).hexdigest())
                    if (decoded_projection.request_contract_digest
                            and decoded_projection.request_contract_digest
                            != request_contract_digest):
                        ctx.fail("V3 Selection request contract mismatch")
                        return
                    matching = [
                        item for item in decoded_projection.roles
                        if item.role == ctx.assignment.role
                    ]
                    if len(matching) != 1:
                        ctx.fail("V3 Selection role coverage mismatch")
                        return
                    v3_role_spec = matching[0]
                    v3_input_ingress_owner = any(
                        str(getattr(endpoint.source_kind, "value",
                                    endpoint.source_kind)) == "APPLICATION_INPUT"
                        and str(endpoint.consumer_role) == str(ctx.assignment.role)
                        for endpoint in decoded_projection.dataflow.must_fetch)
                    v3_terminal_response_owner = bool(
                        decoded_projection.dataflow.terminal_response_owner)
                    v3_dependency_view = _dependency_view_from_v3_projection(
                        ctx.assignment.role, decoded_projection.dependencies)
                    # SVS delivers the collaboration namespace broadly to all
                    # Providers.  Install this role's exact request-bound
                    # scope/topic bindings before artifact preparation so
                    # unrelated role traffic is filtered before native
                    # decryption (including packets that arrive while this
                    # role is still becoming ready).
                    for dependency in (
                            *v3_dependency_view.inputs,
                            *v3_dependency_view.outputs,
                            *v3_dependency_view.internal):
                        ctx.allow_data(dependency.key_scope,
                                       dependency.topic_prefix)

            def release_selection_reservation(reason: str) -> None:
                nonlocal terminal_released
                if (terminal_released or selection_participant is None
                        or not assignment_payload):
                    return
                assignment = DISelectionAssignmentV2.from_bytes(
                    assignment_payload)
                released = selection_participant.mark_role_terminal(
                    assignment_payload, ctx.assignment.role, reason=reason)
                terminal_released = True
                if released:
                    print(
                        "NDNSF_DI_SELECTION_RESERVATION_RELEASED",
                        f"requestId={assignment.request_id}",
                        f"attempt={assignment.attempt}",
                        f"role={ctx.assignment.role}",
                        f"reason={reason}",
                        flush=True,
                    )

            class TerminalAwareContext:
                """Delegate Core context while releasing DI capacity before Response."""

                def __getattr__(self, name):
                    return getattr(ctx, name)

                def publish_final_response(self, payload: bytes) -> None:
                    ctx.publish_final_response(payload)
                    release_selection_reservation("RESPONSE_PUBLISHED")

                def finish_stream(self, payload: bytes = b"", *, reason=4,
                                  **kwargs) -> bool:
                    # The Core writer remains the sole terminal authority.
                    # Release the selection reservation only after it accepts
                    # the terminal, so a rejected/fenced write is accounted
                    # for by the handler/failure path instead of being
                    # mislabeled as a published Response.
                    try:
                        accepted = ctx.finish_stream(
                            payload, reason=reason, **kwargs)
                    except TypeError:
                        accepted = ctx.finish_stream(payload, reason)
                    release_selection_reservation("RESPONSE_PUBLISHED")
                    return accepted

            terminal_context = TerminalAwareContext()

            preparation_attempt = v3_projection.attempt if v3_projection else 1
            self._report_preparation(
                ctx, phase="ACCEPTED", sequence=sequence, progress=0.0,
                attempt=preparation_attempt)

            def report(phase: str, value: float) -> None:
                nonlocal sequence
                sequence += 1
                self._report_preparation(
                    ctx, phase=phase, sequence=sequence, progress=value,
                    attempt=preparation_attempt)

            try:
                dependency_ready = None
                if v3_projection is not None and selection_participant is not None:
                    raise RuntimeError("V3 Selection cannot use the V2 participant")
                if selection_participant is not None:
                    if not assignment_payload:
                        raise RuntimeError(
                            "V2 Selection execution has no assignment payload")
                    transaction_id = selection_participant.wait_role_prepared(
                        assignment_payload,
                        ctx.assignment.role,
                        timeout=max(0.001, selection_max_prepare_ms / 1000.0),
                    )
                    assignment = DISelectionAssignmentV2.from_bytes(
                        assignment_payload)
                    role_assignment = next(
                        item for item in assignment.roles
                        if item.role == ctx.assignment.role)
                    committed_dependency_view = _dependency_view_from_assignment(
                        role_assignment)
                    if not role_assignment.required_input_scopes:
                        selection_participant.mark_input_ready(
                            ctx.assignment.role,
                            transaction_id=transaction_id)

                    def dependency_ready(edge) -> None:
                        selection_participant.mark_dependency_input_ready(
                            assignment_payload,
                            ctx.assignment.role,
                            str(edge.key_scope),
                        )
                assigned_artifact = str(ctx.assignment.assigned_artifact or "")
                if v3_role_spec is not None:
                    # The generic Role carries the canonical NDN artifact name;
                    # the opaque V3 projection independently binds its object
                    # digest, recipe, graph range, and Provider.
                    if not assigned_artifact or assigned_artifact == "/":
                        raise RuntimeError(
                            "V3 Selection has no canonical artifact identity")
                role_has_local_artifact = bool(local_artifacts.get(ctx.assignment.role, {}).get("path"))
                if selection_participant is not None:
                    # In V2, the committed NDNSF-DI participant owns artifact
                    # verification, materialization, and runtime preparation.
                    # ``assigned_artifact`` is the participant's immutable
                    # artifact identity (for example a DistributedRepo object),
                    # not the name of a legacy Core ExecutionArtifactSpec.
                    # Build only the adapter-facing execution shell here; the
                    # mandatory runtime_preparer below binds it to the exact
                    # role assignment and returns readiness evidence.
                    execution = self._local_execution(
                        ctx.assignment.role,
                        backend=backend_list[0] if backend_list else "",
                        temp_dir=temp_dir,
                        local_artifacts=local_artifacts,
                    )
                elif has_model and role_has_local_artifact:
                    execution = self._local_execution(
                        ctx.assignment.role,
                        backend=backend_list[0] if backend_list else "",
                        temp_dir=temp_dir,
                        local_artifacts=local_artifacts,
                    )
                elif assigned_artifact and assigned_artifact != "/":
                    execution = prepare_execution(
                        ctx,
                        temp_root=temp_dir,
                        allow_executables=allow_executables,
                        progress=report,
                    )
                elif has_model:
                    execution = self._local_execution(
                        ctx.assignment.role,
                        backend=backend_list[0] if backend_list else "",
                        temp_dir=temp_dir,
                        local_artifacts=local_artifacts,
                    )
                else:
                    raise RuntimeError(
                        "collaboration assignment has no artifact and provider "
                        "was not registered with has_model=True")
                if (v3_role_spec is not None
                        and v3_role_spec.role_kind == "COMPONENT_SET"
                        and v3_role_spec.node_indices
                        and role_has_local_artifact):
                    # A certified component-set role assembles its own
                    # subgraph from the validated canonical root; the
                    # assembled bytes replace the local canonical model for
                    # this role's execution only.
                    execution = self._assemble_certified_role_execution(
                        ctx, execution, v3_role_spec, local_artifacts)
                    if v3_role_spec.protection_epoch != "plaintext-v1":
                        # spec181 T001: fetch + verify + unwrap the
                        # Provider-bound grant at the authorization boundary
                        # (before the artifact is exposed), then AEAD-seal the
                        # assembled artifact under the unwrapped content key
                        # (FR-013).  Any verifier decision fails closed as
                        # DI_PROTECTED_GRANT_REJECTED.
                        try:
                            (execution,
                             protected_lease_registry) = (
                                self._qualify_protected_assembly(
                                    ctx, execution, v3_projection,
                                    v3_role_spec))
                        except ProtectedGrantRejected as exc:
                            sequence += 1
                            self._report_preparation(
                                ctx, phase="FAILED", sequence=sequence,
                                progress=0.0,
                                reason="DI_PROTECTED_GRANT_REJECTED",
                                attempt=preparation_attempt)
                            release_selection_reservation(
                                "PREPARATION_FAILED")
                            ctx.fail(
                                f"DI_PROTECTED_GRANT_REJECTED: {exc}")
                            return
                execution = self._bind_assignment_metadata(ctx, execution)
                if v3_role_spec is not None:
                    execution = replace(execution, spec=replace(
                        execution.spec,
                        backend=v3_role_spec.backend,
                        metadata={
                            **dict(execution.spec.metadata),
                            "placementProfile": "DI_PLACEMENT_V3",
                            "selectionRequestId": v3_projection.request_id,
                            "selectionAttempt": v3_projection.attempt,
                            "planDigest": v3_projection.plan_digest,
                            "selectionPlanDigest": v3_projection.plan_digest,
                            "planCoreDigest": v3_projection.plan_core_digest,
                            "selectionArtifactDigest": v3_role_spec.artifact_digest,
                            "selectionRecipeDigest": v3_role_spec.recipe_digest,
                            "selectionAdapterId": v3_role_spec.adapter_id,
                            "selectionAdapterVersion": v3_role_spec.adapter_version,
                            "selectionDevice": (
                                v3_role_spec.device_set[0]
                                if len(v3_role_spec.device_set) == 1 else ""),
                            "selectionLayerRange": (
                                v3_role_spec.layer_begin,
                                v3_role_spec.layer_end),
                            "selectionRank": v3_role_spec.rank,
                            # Conversation bindings are authenticated by the
                            # V3 Selection projection.  Keep the compact
                            # data-only forms in the adapter metadata so a
                            # Python Provider can implement the same receipt
                            # lifecycle as the native handler without
                            # re-parsing an untrusted side channel.
                            "conversationTurnBinding": (
                                v3_projection.conversation_turn_binding.to_dict()
                                if v3_projection.conversation_turn_binding
                                is not None else None),
                            "conversationStateReference": (
                                v3_projection.conversation_state_reference.to_dict()
                                if v3_projection.conversation_state_reference
                                is not None else None),
                            "generationId": (
                                v3_projection.generation_contract.generation_id
                                if v3_projection.generation_contract is not None
                                else ""),
                            "securityDomain": "requester-default",
                        },
                    ))
                if selection_participant is not None:
                    if role_assignment is None:
                        raise RuntimeError(
                            "V2 Selection has no local role assignment")
                    if runtime_preparer is None:
                        raise RuntimeError(
                            "V2 CUDA Selection requires a runtime preparer")
                    execution = replace(execution, spec=replace(
                        execution.spec,
                        backend=role_assignment.backend,
                        metadata={
                            **dict(execution.spec.metadata),
                            "selectionRequestId": assignment.request_id,
                            "selectionAttempt": assignment.attempt,
                            "selectionAdapterId": role_assignment.adapter_id,
                            "selectionAdapterVersion":
                                role_assignment.adapter_version,
                            "selectionBackend": role_assignment.backend,
                            "selectionDevice": role_assignment.device,
                            "selectionArtifactDigest":
                                role_assignment.artifact_digest,
                        },
                    ))
                    execution = prepare_runtime(
                        execution,
                        preparer=runtime_preparer,
                        expected_adapter_id=role_assignment.adapter_id,
                        expected_adapter_version=role_assignment.adapter_version,
                        expected_backend=role_assignment.backend,
                        expected_device=role_assignment.device,
                        expected_artifact_digest=role_assignment.artifact_digest,
                        progress=report,
                    )
                elif v3_role_spec is not None and runtime_preparer is not None:
                    if not v3_role_spec.adapter_id or not v3_role_spec.adapter_version:
                        raise RuntimeError(
                            "V3 Selection lacks adapter identity for runtime preparation")
                    execution = prepare_runtime(
                        execution,
                        preparer=runtime_preparer,
                        expected_adapter_id=v3_role_spec.adapter_id,
                        expected_adapter_version=v3_role_spec.adapter_version,
                        expected_backend=v3_role_spec.backend,
                        expected_device=(
                            v3_role_spec.device_set[0]
                            if len(v3_role_spec.device_set) == 1
                            else ""),
                        expected_artifact_digest=v3_role_spec.artifact_digest,
                        progress=report,
                    )
                elif v3_role_spec is not None:
                    # V3 remains selection-bound even for a provider that uses
                    # an already materialized local runtime.  The signed role
                    # metadata is attached above; only the adapter-specific
                    # preparer may add runtime readiness evidence.
                    if sequence == 1:
                        report("LOADING", 0.70)
                    report("WARMING", 0.90)
                else:
                    # Legacy/preplanned compatibility has no signed runtime
                    # binding. Keep it explicit and outside the V2 proof path.
                    if sequence == 1:
                        report("LOADING", 0.70)
                    report("WARMING", 0.90)
            except Exception as exc:
                sequence += 1
                self._report_preparation(
                    ctx, phase="FAILED", sequence=sequence, progress=0.0,
                    reason=type(exc).__name__, attempt=preparation_attempt)
                release_selection_reservation("PREPARATION_FAILED")
                ctx.fail(f"failed to prepare inference execution: {exc}")
                return

            sequence += 1
            self._report_preparation(
                ctx, phase="READY", sequence=sequence, progress=1.0,
                execution=execution, attempt=preparation_attempt)

            prefetcher = DependencyPrefetcher(ctx)
            try:
                self._run_handler(handler, ProviderRuntimeContext(
                    ndnsf=terminal_context,
                    execution=execution,
                    request=request,
                    role=ctx.assignment.role,
                    dependencies=(
                        committed_dependency_view
                        if selection_participant is not None
                        else v3_dependency_view
                        if v3_dependency_view is not None
                        else dependency_graph.for_role(ctx.assignment.role)
                        if dependency_graph is not None
                        else RoleDependencyView(ctx.assignment.role)
                    ),
                    prefetcher=prefetcher,
                    deadline_ms=_assignment_deadline_ms(assignment_payload),
                    on_dependency_ready=dependency_ready,
                    stream_writer=(terminal_context if getattr(ctx, "is_streamed", False)
                                   else None),
                    input_ingress_owner=v3_input_ingress_owner,
                    terminal_response_owner=v3_terminal_response_owner,
                    enforce_dataflow_ownership=(v3_projection is not None),
                ))
            finally:
                prefetcher.shutdown()
                release_selection_reservation("ROLE_HANDLER_RETURNED")
                if protected_lease_registry is not None:
                    # FR-013: every materialized plaintext (content key copy
                    # and assembled model) is zeroized and removed before the
                    # handler boundary closes.
                    protected_lease_registry.zeroize_all()

        try:
            self.provider.add_collaboration_handler(
                service, role_list, wrapped, ack, include_ack_context=True)
        except TypeError as exc:
            # Source-compatible adapter for pre-R1 provider facades and test
            # doubles. They cannot negotiate R1 because they provide no ACK
            # context, so preserve their ordinary non-reserving ACK behavior.
            if "include_ack_context" not in str(exc):
                raise
            self.provider.add_collaboration_handler(
                service, role_list, wrapped,
                lambda payload: ack({}, payload))
        register_decision_authority()

    def run(self) -> int:
        return self.provider.run()

    def start(self) -> None:
        """Start Core after all service registrations are installed."""
        self.provider.start()

    def stop(self) -> int:
        try:
            return self.provider.stop()
        finally:
            if self._handler_executor is not None:
                self._handler_executor.shutdown(wait=True)

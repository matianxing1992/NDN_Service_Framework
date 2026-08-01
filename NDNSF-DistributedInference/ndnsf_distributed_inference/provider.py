"""Provider-side high-level API for distributed inference over NDNSF."""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from concurrent.futures import Future, ThreadPoolExecutor
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
)
from .core import (
    AssignmentContext, AtomicReservationBook, ProviderAssignment, ProviderProfileV1,
    RuntimeTelemetryV1, DeploymentIntent, DeploymentPlan,
    DIRequestEnvelopeV2, DISelectionAssignmentV2, DISelectionParticipant,
    GpuMiBAdmissionLedger,
    ExecutionActivateMessage, ProviderCapabilityOffer, PreparationCallbacks,
    ReservationDecisionAuthority, SelectionDecision, SelectionGatedProvider,
)
from .plan import RoleDependencyView
from .sdk.placement import DIProviderOfferV2, canonical_digest


@dataclass(frozen=True)
class LargePrefetchResult:
    payload: bytes
    ref_wait_ms: float
    fetch_ms: float
    total_ms: float
    expected_segments: int = 0
    expected_bytes: int = 0
    used_planned_name: bool = False


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
        offer_lease_ms: int = 5000,
        max_pending_state_ttl_ms: int = 60 * 60 * 1000,
        clock_ms: Callable[[], int] | None = None,
    ) -> None:
        if (ledger.provider != provider or ledger.boot_epoch != boot_epoch
                or offered_gpu_memory_mb <= 0 or not signer_key_id
                or not callable(sign_offer_digest) or offer_lease_ms <= 0
                or max_pending_state_ttl_ms <= 0):
            raise ValueError("invalid DI Provider offer issuer")
        self.provider = provider
        self.service = service
        self.boot_epoch = boot_epoch
        self.ledger = ledger
        self.offered_gpu_memory_mb = int(offered_gpu_memory_mb)
        self.signer_key_id = signer_key_id
        self._sign = sign_offer_digest
        self.offer_lease_ms = int(offer_lease_ms)
        self.max_pending_state_ttl_ms = int(max_pending_state_ttl_ms)
        self._clock_ms = clock_ms or (lambda: int(time() * 1000))
        self._sequence = 0
        self._offers: dict[str, DIProviderOfferV2] = {}
        self._lock = RLock()

    def issue(
        self, request_wire: bytes, *, accepted_roles: Sequence[str],
        backends: Sequence[str], queue_depth: int | None = None,
        estimated_wait_ms: float | None = None,
        rtt_ms: float | None = None, bandwidth_mbps: float | None = None,
        cached_shards: Sequence[object] = (),
        reusable_state: Sequence[object] = (),
    ) -> AckDecision:
        request = DIRequestEnvelopeV2.from_bytes(request_wire)
        now_ms = self._clock_ms()
        if request.service != self.service or request.plan_deadline_ms <= now_ms:
            return AckDecision(
                status=False, message="DI_V2_REQUEST_NOT_ADMISSIBLE")
        pending_state_ttl_ms = request.plan_deadline_ms - now_ms
        if pending_state_ttl_ms > self.max_pending_state_ttl_ms:
            return AckDecision(
                status=False,
                message="DI_V2_REQUEST_DEADLINE_EXCEEDS_PROVIDER_LIMIT")
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
        return self.ndnsf.wait_one(edge.key_scope, edge.topic(topic_suffix), timeout_ms)

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
            return self.prefetcher.prefetch_large(
                edge,
                topic_suffix,
                ref_timeout_ms=ref_timeout_ms,
                fetch_timeout_ms=fetch_timeout_ms,
            data_name=data_name,
            expected_segments=int(getattr(edge, "expected_segments", 0) or 0),
            expected_bytes=int(getattr(edge, "expected_bytes", 0) or 0),
        )
        except TypeError:
            return self.prefetcher.prefetch_large(
                edge,
                topic_suffix,
                ref_timeout_ms=ref_timeout_ms,
                fetch_timeout_ms=fetch_timeout_ms,
            )

    @staticmethod
    def wait_prefetched_input_large(future: Future, *,
                                    timeout_ms: int | None = None) -> bytes:
        return ProviderRuntimeContext.wait_prefetched_input_large_result(
            future,
            timeout_ms=timeout_ms,
        ).payload

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


class DistributedInferenceProvider:
    """Register inference roles using the underlying NDNSF provider."""

    def __init__(self, provider: ServiceProvider, *, handler_workers: int = 0):
        self.provider = provider
        self._handler_executor = (
            ThreadPoolExecutor(
                max_workers=int(handler_workers),
                thread_name_prefix="ndnsf-di-provider",
            )
            if int(handler_workers) > 0 else None
        )

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
        ), handler_workers=handler_workers)

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

    @staticmethod
    def _report_preparation(
        ctx: CollaborationContext,
        *,
        phase: str,
        sequence: int,
        progress: float,
        execution: ExecutionContext | None = None,
        reason: str = "",
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
            attempt=1,
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
        """Gate execution on exact request-scoped readiness for every role."""
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
        selection_participant: DISelectionParticipant | None = None,
        selection_wal_path: str | Path | None = None,
        selection_storage_key: bytes | None = None,
        selection_storage_key_epoch: str = "",
        selection_max_prepare_ms: int = 1000,
        selection_cached_shards: Callable[[], Sequence[object]] | None = None,
        selection_reusable_state: Callable[[], Sequence[object]] | None = None,
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
            v2_envelope: DIRequestEnvelopeV2 | None = None
            try:
                request_document = json.loads(bytes(_payload).decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                request_document = None
            if (isinstance(request_document, dict)
                    and request_document.get("schema")
                    == "ndnsf-di-request-envelope-v2"):
                try:
                    v2_envelope = DIRequestEnvelopeV2.from_bytes(_payload)
                except ValueError as exc:
                    return AckDecision(
                        status=False,
                        message=f"DI_V2_REQUEST_REJECTED:{exc}")
                v2_request = True
                if selection_offer_issuer is None:
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
                    **({
                        "freeMemoryMb": telemetry.free_memory_mb,
                        "runtimeBackend": telemetry.runtime_backend,
                        "modelLoaded": telemetry.model_loaded,
                    } if telemetry is not None else {}),
                },
            )
            if not (can_provision or has_model or ready_without_model):
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
                    ))
                    return simple_ctx.response
                except Exception as exc:  # noqa: BLE001
                    return ServiceResponse(status=False, error=str(exc))

            self.provider.add_handler(service, simple_handler)
            self.provider.set_ack_context_handler(service, ack)
            register_decision_authority()
            return

        def wrapped(ctx: CollaborationContext, request: bytes) -> None:
            sequence = 1
            assignment_payload = bytes(ctx.assignment.assignment_payload or b"")
            terminal_released = False

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
                    release_selection_reservation("RESPONSE_PUBLISHED")
                    ctx.publish_final_response(payload)

            self._report_preparation(
                ctx, phase="ACCEPTED", sequence=sequence, progress=0.0)

            def report(phase: str, value: float) -> None:
                nonlocal sequence
                sequence += 1
                self._report_preparation(
                    ctx, phase=phase, sequence=sequence, progress=value)

            try:
                if selection_participant is not None:
                    if not assignment_payload:
                        raise RuntimeError(
                            "V2 Selection execution has no assignment payload")
                    transaction_id = selection_participant.wait_role_prepared(
                        assignment_payload,
                        ctx.assignment.role,
                        timeout=max(0.001, selection_max_prepare_ms / 1000.0),
                    )
                    selection_participant.mark_input_ready(
                        ctx.assignment.role, transaction_id=transaction_id)
                assigned_artifact = str(ctx.assignment.assigned_artifact or "")
                role_has_local_artifact = bool(local_artifacts.get(ctx.assignment.role, {}).get("path"))
                if has_model and role_has_local_artifact:
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
            except Exception as exc:
                sequence += 1
                self._report_preparation(
                    ctx, phase="FAILED", sequence=sequence, progress=0.0,
                    reason=type(exc).__name__)
                release_selection_reservation("PREPARATION_FAILED")
                ctx.fail(f"failed to prepare inference execution: {exc}")
                return

            execution = self._bind_assignment_metadata(ctx, execution)

            if sequence == 1:
                report("LOADING", 0.70)
            sequence += 1
            self._report_preparation(
                ctx, phase="WARMING", sequence=sequence, progress=0.90,
                execution=execution)
            sequence += 1
            self._report_preparation(
                ctx, phase="READY", sequence=sequence, progress=1.0,
                execution=execution)

            prefetcher = DependencyPrefetcher(ctx)
            try:
                self._run_handler(handler, ProviderRuntimeContext(
                    ndnsf=TerminalAwareContext(),
                    execution=execution,
                    request=request,
                    role=ctx.assignment.role,
                    dependencies=(dependency_graph.for_role(ctx.assignment.role)
                                  if dependency_graph is not None
                                  else RoleDependencyView(ctx.assignment.role)),
                    prefetcher=prefetcher,
                ))
            finally:
                prefetcher.shutdown()
                release_selection_reservation("ROLE_HANDLER_RETURNED")

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

    def stop(self) -> int:
        try:
            return self.provider.stop()
        finally:
            if self._handler_executor is not None:
                self._handler_executor.shutdown(wait=True)

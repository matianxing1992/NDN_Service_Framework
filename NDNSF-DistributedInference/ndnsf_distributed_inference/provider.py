"""Provider-side high-level API for distributed inference over NDNSF."""

from __future__ import annotations

from dataclasses import dataclass, field
from concurrent.futures import Future, ThreadPoolExecutor
import os
from pathlib import Path
import tempfile
from time import perf_counter, time
from typing import Callable, Sequence

from ndnsf import (
    AckDecision,
    CollaborationContext,
    ExecutionArtifact,
    ExecutionArtifactSpec,
    ExecutionContext,
    GenericProviderRuntimeHint,
    NEGATIVE_ACK_REASON_GPU_BUSY,
    NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
    NEGATIVE_ACK_REASON_PROVIDER_BUSY,
    NEGATIVE_ACK_REASON_QUEUE_FULL,
    ProviderCapabilityHint,
    ServiceProvider,
    ServiceResponse,
    to_plain,
)

from .plan import RoleDependencyView
from .runtime_v1 import (
    ProviderProfileV1,
    RuntimeTelemetryV1,
    encode_ack_metadata,
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
            try:
                execution = ctx.prepare_execution(
                    temp_root=temp_dir,
                    allow_executables=allow_executables,
                )
            except Exception as exc:
                ctx.fail(f"failed to prepare inference execution: {exc}")
                return

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
        register_simple_service: bool = False,
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
        local_artifacts = dict(local_artifacts or {})

        def ack(_payload: bytes) -> AckDecision:
            if readiness_probe is not None:
                readiness = readiness_probe()
                if isinstance(readiness, AckDecision):
                    if not readiness.status:
                        return readiness
                    readiness_payload = bytes(readiness.payload or b"")
                else:
                    if not bool(readiness):
                        return AckDecision(
                            status=False,
                            message=NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
                            payload=(
                                b"status=installing;"
                                b"negativeAckReason=MODEL_UNAVAILABLE;"
                            ),
                        )
                    readiness_payload = b""
            else:
                readiness_payload = b""
            fields: dict[str, object] = {
                "roles": role_list,
                "queue": queue_depth,
                "hasModel": has_model,
                "canProvision": can_provision,
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
            if not (can_provision or has_model):
                fields["negativeAckReason"] = NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE
                fields["status"] = "model-unavailable"
                fields.update(ProviderCapabilityHint(
                    provider_name=runtime_hint.provider_name,
                    service_name=service,
                    ready=False,
                    reason_code=NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
                    message="model unavailable",
                    runtime_hint=runtime_hint,
                    service_payload_schema="ndnsf-di-capability-v1",
                    service_payload={key: to_plain(value) for key, value in fields.items()},
                ).to_ack_fields())
                return AckDecision(
                    status=False,
                    message=NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
                    payload=encode_ack_metadata(fields) + readiness_payload,
                )
            if admission_policy is not None and telemetry is not None:
                accepted, reason, diagnostics = admission_policy.evaluate(telemetry)
                fields.update(diagnostics)
                if not accepted:
                    fields["negativeAckReason"] = reason
                    fields["status"] = "admission-rejected"
                    fields.update(ProviderCapabilityHint(
                        provider_name=runtime_hint.provider_name,
                        service_name=service,
                        ready=False,
                        reason_code=reason,
                        message="admission rejected",
                        runtime_hint=runtime_hint,
                        service_payload_schema="ndnsf-di-capability-v1",
                        service_payload={key: to_plain(value) for key, value in fields.items()},
                    ).to_ack_fields())
                    return AckDecision(
                        status=False,
                        message=reason,
                        payload=encode_ack_metadata(fields) + readiness_payload,
                    )
            fields.update(ProviderCapabilityHint(
                provider_name=runtime_hint.provider_name,
                service_name=service,
                ready=True,
                message="inference capability ready",
                runtime_hint=runtime_hint,
                service_payload_schema="ndnsf-di-capability-v1",
                service_payload={key: to_plain(value) for key, value in fields.items()},
            ).to_ack_fields())
            return AckDecision(
                status=True,
                message="inference capability ready",
                payload=encode_ack_metadata(fields) + readiness_payload,
            )

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
                    readiness = ack(request)
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
            self.provider.set_ack_handler(service, ack)
            return

        def wrapped(ctx: CollaborationContext, request: bytes) -> None:
            try:
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
                    execution = ctx.prepare_execution(
                        temp_root=temp_dir,
                        allow_executables=allow_executables,
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
                ctx.fail(f"failed to prepare inference execution: {exc}")
                return

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

        self.provider.add_collaboration_handler(service, role_list, wrapped, ack)

    def run(self) -> int:
        return self.provider.run()

    def stop(self) -> int:
        try:
            return self.provider.stop()
        finally:
            if self._handler_executor is not None:
                self._handler_executor.shutdown(wait=True)

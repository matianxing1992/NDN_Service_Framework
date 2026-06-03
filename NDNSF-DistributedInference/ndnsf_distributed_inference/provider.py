"""Provider-side high-level API for distributed inference over NDNSF."""

from __future__ import annotations

from dataclasses import dataclass, field
from concurrent.futures import Future, ThreadPoolExecutor
from pathlib import Path
import tempfile
from typing import Callable, Sequence

from ndnsf import (
    AckDecision,
    CollaborationContext,
    ExecutionArtifact,
    ExecutionArtifactSpec,
    ExecutionContext,
    ServiceProvider,
)

from .plan import RoleDependencyView


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
                       fetch_timeout_ms: int = 10000) -> Future:
        topic = edge.topic(topic_suffix)

        def fetch() -> bytes:
            ref = self._ndnsf.wait_one(edge.key_scope, topic, ref_timeout_ms)
            if ref is None:
                raise TimeoutError(
                    f"timed out waiting for dependency ref "
                    f"scope={edge.key_scope} topic={topic}")
            payload = self._ndnsf.fetch_large_reference(
                ref.payload,
                edge.key_scope,
                fetch_timeout_ms,
            )
            if payload is None:
                raise TimeoutError(
                    f"timed out fetching dependency object "
                    f"scope={edge.key_scope} topic={topic}")
            return payload

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
                             fetch_timeout_ms: int = 10000) -> Future:
        """Start fetching a planned input dependency in the background.

        This is useful when a distributed plan makes dependency names
        predictable. The method does not know model semantics; it simply waits
        for the dependency reference on the selected edge and fetches the large
        object named by that reference.
        """

        if self.prefetcher is None:
            raise RuntimeError("dependency prefetcher is not available")
        edge = self.dependencies.input(key_scope)
        return self.prefetcher.prefetch_large(
            edge,
            topic_suffix,
            ref_timeout_ms=ref_timeout_ms,
            fetch_timeout_ms=fetch_timeout_ms,
        )

    @staticmethod
    def wait_prefetched_input_large(future: Future, *,
                                    timeout_ms: int | None = None) -> bytes:
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
        if self._handler_executor is None:
            handler(context)
            return
        # CollaborationContext is owned by the active NDNSF callback. Wait for
        # the Python worker to complete before returning to keep it valid.
        self._handler_executor.submit(handler, context).result()

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
            fields = [
                "roles=" + ",".join(role_list),
                "queue=" + str(queue_depth),
                "hasModel=" + ("1" if has_model else "0"),
                "canProvision=" + ("1" if can_provision else "0"),
            ]
            if len(role_list) == 1:
                fields.append("role=" + role_list[0])
            if backend_list:
                fields.append("backends=" + ",".join(backend_list))
            return AckDecision(
                status=can_provision or has_model,
                message="inference capability ready",
                payload=(";".join(fields) + ";").encode(),
            )

        def wrapped(ctx: CollaborationContext, request: bytes) -> None:
            try:
                assigned_artifact = str(ctx.assignment.assigned_artifact or "")
                if assigned_artifact and assigned_artifact != "/":
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

"""Provider-side high-level API for distributed inference over NDNSF."""

from __future__ import annotations

from dataclasses import dataclass, field
from concurrent.futures import ThreadPoolExecutor
from typing import Callable, Sequence

from ndnsf import AckDecision, CollaborationContext, ServiceProvider

from .plan import RoleDependencyView


@dataclass(frozen=True)
class ProviderRuntimeContext:
    ndnsf: CollaborationContext
    execution: object
    request: bytes
    role: str
    dependencies: RoleDependencyView = field(
        default_factory=lambda: RoleDependencyView(role=""))

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

    def wait_input(self, *, key_scope: str = "", topic_suffix: str = "",
                   timeout_ms: int = 10000):
        edge = self.dependencies.input(key_scope)
        return self.ndnsf.wait_one(edge.key_scope, edge.topic(topic_suffix), timeout_ms)

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

            self._run_handler(handler, ProviderRuntimeContext(
                ndnsf=ctx,
                execution=execution,
                request=request,
                role=ctx.assignment.role,
                dependencies=(dependency_graph.for_role(ctx.assignment.role)
                              if dependency_graph is not None
                              else RoleDependencyView(ctx.assignment.role)),
            ))

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
    ) -> None:
        """Register one provider as capable of serving multiple inference roles.

        The provider may not have the model/runtime locally. The assignment
        carries encrypted artifacts published by the user; ``prepare_execution``
        downloads and verifies them before the handler runs.
        """

        role_list = [_validate_list_token(str(role), "role") for role in roles]
        if not role_list:
            raise ValueError("at least one role capability is required")
        backend_list = [_validate_list_token(str(backend), "backend")
                        for backend in backends]

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
                execution = ctx.prepare_execution(
                    temp_root=temp_dir,
                    allow_executables=allow_executables,
                )
            except Exception as exc:
                ctx.fail(f"failed to prepare inference execution: {exc}")
                return

            self._run_handler(handler, ProviderRuntimeContext(
                ndnsf=ctx,
                execution=execution,
                request=request,
                role=ctx.assignment.role,
                dependencies=(dependency_graph.for_role(ctx.assignment.role)
                              if dependency_graph is not None
                              else RoleDependencyView(ctx.assignment.role)),
            ))

        self.provider.add_collaboration_handler(service, role_list, wrapped, ack)

    def run(self) -> int:
        return self.provider.run()

    def stop(self) -> int:
        try:
            return self.provider.stop()
        finally:
            if self._handler_executor is not None:
                self._handler_executor.shutdown(wait=True)

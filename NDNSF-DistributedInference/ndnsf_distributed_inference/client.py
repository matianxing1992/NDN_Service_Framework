"""Client-side high-level API for distributed inference over NDNSF."""

from __future__ import annotations

import secrets
import threading
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from typing import Callable, Iterable

from ndnsf import ServiceResponse, ServiceUser

from .plan import DistributedInferencePlan


@dataclass(frozen=True)
class InferenceResult:
    status: bool
    payload: bytes = b""
    error: str = ""


class DistributedInferenceClient:
    """Compile an inference plan into NDNSF collaboration calls."""

    def __init__(self, user: ServiceUser, *, async_workers: int = 4):
        self.user = user
        self._executor = ThreadPoolExecutor(
            max_workers=max(1, int(async_workers)),
            thread_name_prefix="ndnsf-di-client",
        )
        self._user_lock = threading.Lock()

    @classmethod
    def connect(
        cls,
        *,
        group: str,
        controller: str,
        user: str,
        trust_schema: str,
        permission_wait_ms: int = 2500,
        handler_threads: int = 2,
        ack_threads: int = 2,
        async_workers: int = 4,
        adaptive_admission: bool = False,
        serve_certificates: bool = True,
    ) -> "DistributedInferenceClient":
        """Create an inference client without exposing NDNSF Core objects.

        AI applications should normally use this constructor instead of
        importing ``ndnsf.ServiceUser`` directly. The underlying NDNSF user is
        still used internally for permissions, SVS, NAC-ABE, signing, artifact
        publishing, and request coordination.
        """

        return cls(ServiceUser(
            group=group,
            controller=controller,
            user=user,
            trust_schema=trust_schema,
            permission_wait_ms=permission_wait_ms,
            handler_threads=handler_threads,
            ack_threads=ack_threads,
            adaptive_admission=adaptive_admission,
            serve_certificates=serve_certificates,
        ), async_workers=async_workers)

    def publish_plan_artifacts(
        self,
        plan: DistributedInferencePlan,
        *,
        object_label_prefix: str = "inference",
        freshness_ms: int = 60000,
    ) -> dict[str, str]:
        artifact_data_names: dict[str, str] = {}
        for role in plan.roles:
            published = self.user.publish_execution_artifact_spec(
                plan.service,
                role=role.role,
                backend=role.backend,
                entrypoint=role.runtime.entrypoint,
                artifacts=role.artifacts(),
                metadata={
                    "model": plan.model_name,
                    "role": role.role,
                    "runtime": role.runtime.name,
                    **dict(plan.metadata),
                    **dict(role.metadata),
                },
                object_label_prefix=object_label_prefix,
                freshness_ms=freshness_ms,
            )
            if not published.success:
                raise RuntimeError(
                    f"artifact publish failed for {role.role}: {published.error}")
            artifact_data_names[role.role] = published.encrypted_data_name
        return artifact_data_names

    def publish_scope_keys(
        self,
        plan: DistributedInferencePlan,
        *,
        object_label_prefix: str = "inference-scope-key",
        freshness_ms: int = 60000,
    ) -> dict[str, str]:
        scope_key_data_names: dict[str, str] = {}
        for scope in plan.key_scopes():
            result = self.user.publish_encrypted_large_data(
                plan.service,
                secrets.token_bytes(32),
                object_label=f"{object_label_prefix}-{scope}",
                freshness_ms=freshness_ms,
            )
            if not result.success:
                raise RuntimeError(f"scope key publish failed for {scope}: {result.error}")
            scope_key_data_names[scope] = result.encrypted_data_name
        return scope_key_data_names

    def infer(
        self,
        plan: DistributedInferencePlan,
        payload: bytes,
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        freshness_ms: int = 60000,
    ) -> InferenceResult:
        # The native ServiceUser owns one Face/SVS/NAC-ABE runtime. Some of its
        # publication and crypto paths are intentionally single-runtime state,
        # so concurrent Python requests must not enter them simultaneously.
        # Async callers still get Futures and queueing, while the native user
        # path remains serialized until NDNSF Core exposes a fully reentrant
        # multi-request publishing API.
        with self._user_lock:
            artifact_data_names = self.publish_plan_artifacts(
                plan,
                object_label_prefix="inference-artifact",
                freshness_ms=freshness_ms,
            )
            scope_key_data_names = self.publish_scope_keys(
                plan,
                object_label_prefix="inference-scope-key",
                freshness_ms=freshness_ms,
            )
            response: ServiceResponse = self.user.request_collaboration(
                plan.service,
                payload,
                roles=plan.ndnsf_roles(),
                key_scopes=plan.key_scopes(),
                dependencies=plan.ndnsf_dependencies(),
                artifact_data_names=artifact_data_names,
                scope_key_data_names=scope_key_data_names,
                role_scopes=plan.role_scopes(),
                ack_timeout_ms=ack_timeout_ms,
                timeout_ms=timeout_ms,
            )
        return InferenceResult(
            status=response.status,
            payload=response.payload,
            error=response.error,
        )

    def infer_async(
        self,
        plan: DistributedInferencePlan,
        payload: bytes,
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        freshness_ms: int = 60000,
        on_result: Callable[[InferenceResult], None] | None = None,
        on_error: Callable[[BaseException], None] | None = None,
    ) -> Future:
        """Submit one inference request and return immediately.

        Artifact publication, scope-key publication, provider selection, and
        response waiting run on the client worker pool. The underlying NDNSF
        Face/SVS operations remain owned by the NDNSF runtime.
        """

        future = self._executor.submit(
            self.infer,
            plan,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            freshness_ms=freshness_ms,
        )
        if on_result is not None or on_error is not None:
            def _done(done: Future) -> None:
                try:
                    result = done.result()
                except BaseException as exc:  # noqa: BLE001
                    if on_error is not None:
                        on_error(exc)
                    return
                if on_result is not None:
                    on_result(result)
            future.add_done_callback(_done)
        return future

    def infer_many_async(
        self,
        requests: Iterable[tuple[DistributedInferencePlan, bytes]],
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        freshness_ms: int = 60000,
    ) -> list[Future]:
        """Submit multiple independent inference requests."""

        return [
            self.infer_async(
                plan,
                payload,
                ack_timeout_ms=ack_timeout_ms,
                timeout_ms=timeout_ms,
                freshness_ms=freshness_ms,
            )
            for plan, payload in requests
        ]

    def shutdown(self, wait: bool = True) -> None:
        self._executor.shutdown(wait=wait)
        self.user.stop()

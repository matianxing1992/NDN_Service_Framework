"""Client-side high-level API for distributed inference over NDNSF."""

from __future__ import annotations

import hashlib
import json
import os
import secrets
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from enum import Enum
from threading import Lock
from time import perf_counter, time
from typing import Callable, Iterable
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from ndnsf import CollaborationDependency, CollaborationRole, ServiceResponse, ServiceUser

from .artifact_deployment import publish_execution_artifact_spec
from .core import (
    CoreAssignment,
    CoreExecutionPlan,
    CoreExecutor,
    ExecutionResult,
    DeploymentPlan,
    ProviderReadyMessage,
    ReadyAcknowledgement,
    ReadySetCoordinator,
    ExecutionActivateMessage,
)
from .plan import DistributedInferencePlan, InferenceDependency


class SelectionAcceptanceState(str, Enum):
    PENDING = "PENDING"
    ACCEPTED = "ACCEPTED"
    UNKNOWN = "UNKNOWN"
    FAILED = "FAILED"


class SelectionAcceptanceTracker:
    """Deadline-bounded encrypted requester control journal.

    Missing acceptance is UNKNOWN, never implicit rejection. The only retry
    payload is the authenticated byte-identical Selection originally stored.
    """

    def __init__(self, *, request_id: str, attempt: int, deadline_ms: int,
                 encryption_key: bytes,
                 clock_ms: Callable[[], int] | None = None) -> None:
        if (not request_id or attempt <= 0 or deadline_ms <= 0
                or len(bytes(encryption_key)) != 32):
            raise ValueError("invalid Selection acceptance tracker")
        self.request_id = request_id
        self.attempt = int(attempt)
        self.deadline_ms = int(deadline_ms)
        self._cipher = AESGCM(bytes(encryption_key))
        self._clock_ms = clock_ms or (lambda: int(time() * 1000))
        self._records: dict[str, dict[str, object]] = {}
        self._lock = Lock()

    def _aad(self, provider: str) -> bytes:
        return (
            f"{self.request_id}|{self.attempt}|{provider}|"
            f"{self.deadline_ms}"
        ).encode()

    def record_selection(self, provider: str, payload: bytes) -> None:
        wire = bytes(payload)
        value_digest = "sha256:" + hashlib.sha256(wire).hexdigest()
        with self._lock:
            current = self._records.get(provider)
            if current is not None:
                if current["digest"] != value_digest:
                    raise ValueError(
                        "conflicting Selection bytes for one Provider attempt")
                return
            nonce = secrets.token_bytes(12)
            self._records[provider] = {
                "digest": value_digest,
                "nonce": nonce,
                "ciphertext": self._cipher.encrypt(
                    nonce, wire, self._aad(provider)),
                "state": SelectionAcceptanceState.PENDING,
                "acceptance_digest": "",
            }

    def mark_delivery_timeout(self, provider: str) -> None:
        with self._lock:
            self._records[provider]["state"] = (
                SelectionAcceptanceState.UNKNOWN)

    def record_acceptance(self, provider: str, acceptance: bytes) -> None:
        value = bytes(acceptance)
        with self._lock:
            record = self._records[provider]
            record["acceptance_digest"] = (
                "sha256:" + hashlib.sha256(value).hexdigest())
            record["state"] = SelectionAcceptanceState.ACCEPTED

    def state(self, provider: str) -> SelectionAcceptanceState:
        with self._lock:
            return self._records[provider]["state"]

    def retry_payload(self, provider: str) -> bytes:
        if self._clock_ms() > self.deadline_ms:
            raise TimeoutError("Selection acceptance deadline expired")
        with self._lock:
            record = dict(self._records[provider])
        return self._cipher.decrypt(
            record["nonce"], record["ciphertext"], self._aad(provider))


@dataclass(frozen=True)
class InferenceResult:
    status: bool
    payload: bytes = b""
    error: str = ""
    request_id: str = ""
    data_name: str = ""
    signer_certificate: str = ""
    wire_digest: str = ""


def _inference_result(response) -> InferenceResult:
    return InferenceResult(
        status=response.status,
        payload=response.payload,
        error=response.error,
        request_id=response.request_id,
        data_name=str(getattr(response, "data_name", "")),
        signer_certificate=str(getattr(response, "signer_certificate", "")),
        wire_digest=str(getattr(response, "wire_digest", "")),
    )


@dataclass(frozen=True)
class PublishedPlanReferences:
    artifact_data_names: dict[str, str]
    scope_key_data_names: dict[str, str]


@dataclass(frozen=True)
class DeploymentSession:
    """Client-side handle for a deployed DI plan.

    The session separates deployment/static metadata from per-inference input:
    model artifacts, role mapping, dependency tensor lists, object-name
    templates, and scope keys are published or cached when the session is
    created; ``invoke_plan`` then only submits the changing input payload.
    """

    plan: DistributedInferencePlan
    fingerprint: str
    references: PublishedPlanReferences


def _to_collaboration_dependency(
    dep: CollaborationDependency | InferenceDependency | dict,
) -> CollaborationDependency | dict:
    if isinstance(dep, InferenceDependency):
        return dep.ndnsf_dependency()
    return dep


def _plan_fingerprint(plan: DistributedInferencePlan) -> str:
    payload = {
        "service": plan.service,
        "model": plan.model_name,
        "roles": [
            {
                "role": role.role,
                "artifact": role.artifact_name,
                "backend": role.backend,
                "service": role.service,
                "allowDynamicProvisioning": role.allow_dynamic_provisioning,
                "modelArtifact": _artifact_fingerprint(role.model_artifact),
                "runtime": {
                    "name": role.runtime.name,
                    "backend": role.runtime.backend,
                    "entrypoint": role.runtime.entrypoint,
                    "artifact": (
                        _artifact_fingerprint(role.runtime.artifact)
                        if role.runtime.artifact is not None else None
                    ),
                },
                "metadata": _jsonable(role.metadata),
            }
            for role in plan.roles
        ],
        "dependencies": [
            {
                "producers": list(dep.producers),
                "consumers": list(dep.consumers),
                "keyScope": dep.key_scope,
                "topicPrefix": dep.topic_prefix,
                "required": dep.required,
                "tensors": list(dep.tensors),
                "objectNameTemplate": dep.object_name_template,
                "expectedSegments": dep.expected_segments,
                "expectedBytes": dep.expected_bytes,
            }
            for dep in plan.dependencies
        ],
        "metadata": _jsonable(plan.metadata),
        "contentionRetry": {
            "maxAttempts": plan.contention_retry.max_attempts,
            "totalDeadlineMs": plan.contention_retry.total_deadline_ms,
            "baseBackoffMs": plan.contention_retry.base_backoff_ms,
            "maxBackoffMs": plan.contention_retry.max_backoff_ms,
        },
    }
    encoded = json.dumps(
        payload,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _artifact_fingerprint(artifact) -> dict:
    payload = bytes(artifact.payload or b"")
    return {
        "name": artifact.name,
        "filename": artifact.filename,
        "kind": artifact.kind,
        "executable": artifact.executable,
        "cacheName": artifact.cache_name,
        "payloadSha256": hashlib.sha256(payload).hexdigest() if payload else "",
        "payloadSize": len(payload),
        "largeDataReference": _jsonable(artifact.large_data_reference),
        "repoManifest": _jsonable(artifact.repo_manifest),
    }


def _jsonable(value):
    try:
        json.dumps(value, sort_keys=True)
        return value
    except TypeError:
        if isinstance(value, dict):
            return {str(key): _jsonable(item) for key, item in value.items()}
        if isinstance(value, (list, tuple)):
            return [_jsonable(item) for item in value]
        return repr(value)


def _elapsed_ms(start: float) -> float:
    return (perf_counter() - start) * 1000.0


def _client_timing_enabled() -> bool:
    return os.environ.get("NDNSF_DI_CLIENT_TIMING", "1") != "0"


class DistributedInferenceClient:
    """Compile an inference plan into NDNSF collaboration calls."""

    def __init__(self, user: ServiceUser, *, async_workers: int = 4,
                 optimization_suite=None):
        from .app_sdk.engine import DistributedInferenceEngine
        from .planner.defaults import DefaultOptimizationSuite
        self.user = user
        self.optimization_engine = DistributedInferenceEngine(
            optimization_suite or DefaultOptimizationSuite())
        self.user.start()
        self._executor = ThreadPoolExecutor(
            max_workers=max(1, int(async_workers)),
            thread_name_prefix="ndnsf-di-client",
        )
        self._published_plan_lock = Lock()
        self._published_plans: dict[str, PublishedPlanReferences] = {}

    @staticmethod
    def dependency_execution(plan: DistributedInferencePlan, *, request_id: str,
                             attempt: int, plan_digest: str,
                             terminal_role: str = "", evidence_verifier=lambda _fields: True):
        """Create the R1 authority; it does not aggregate Provider READY state."""
        return plan.dependency_execution(
            request_id=request_id, attempt=attempt, plan_digest=plan_digest,
            terminal_role=terminal_role, evidence_verifier=evidence_verifier)

    @staticmethod
    def contention_retry(plan: DistributedInferencePlan, *, started_at_ms: int,
                         seed: int | None = None):
        """Create bounded full-jitter retry state for this plan."""
        return plan.contention_retry.controller(
            started_at_ms=started_at_ms, seed=seed)

    def deployment_barrier(
        self,
        plan: DeploymentPlan,
        *,
        ready_verifier: Callable[[ProviderReadyMessage], bool],
    ) -> ReadySetCoordinator:
        """R0 compatibility only; R1 callers use dependency_execution()."""
        return ReadySetCoordinator(plan, verifier=ready_verifier)

    @staticmethod
    def accept_provider_ready(
        barrier: ReadySetCoordinator,
        message: ProviderReadyMessage,
        *,
        now_ms: int,
    ) -> ReadyAcknowledgement:
        return barrier.accept(message, now_ms=now_ms)

    @staticmethod
    def activate_deployment(
        barrier: ReadySetCoordinator,
        *,
        sequence: int,
        signature: str,
        now_ms: int,
    ) -> ExecutionActivateMessage:
        return barrier.activate(sequence=sequence, signature=signature, now_ms=now_ms)

    def execute_core(
        self,
        executor: CoreExecutor,
        plan: CoreExecutionPlan,
        assignment: CoreAssignment,
        payload: bytes,
        *,
        activation: ExecutionActivateMessage | None = None,
    ) -> ExecutionResult:
        """Execute through Core using the requester-authorized activation."""
        return executor.execute(
            plan, assignment, payload, activation=activation)

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
            published = publish_execution_artifact_spec(
                self.user,
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

    def publish_scope_keys_for_scopes(
        self,
        service: str,
        key_scopes: dict[str, list[str]],
        *,
        object_label_prefix: str = "inference-scope-key",
        freshness_ms: int = 60000,
    ) -> dict[str, str]:
        scope_key_data_names: dict[str, str] = {}
        for scope in key_scopes:
            result = self.user.publish_encrypted_large_data(
                service,
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
        request_id: str = "",
        freshness_ms: int = 60000,
    ) -> InferenceResult:
        total_start = perf_counter()
        plan_start = perf_counter()
        session = self.deploy_plan(plan, freshness_ms=freshness_ms)
        plan_ms = _elapsed_ms(plan_start)
        request_start = perf_counter()
        response = self._request_plan_session(
            session,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            request_id=request_id,
        )
        request_ms = _elapsed_ms(request_start)
        total_ms = _elapsed_ms(total_start)
        if _client_timing_enabled():
            print(
                "NDNSF_DI_CLIENT_INFERENCE_TIMING "
                f"service={plan.service} "
                f"mode=dynamic "
                f"plan_ms={plan_ms:.2f} "
                f"request_ms={request_ms:.2f} "
                f"total_ms={total_ms:.2f} "
                f"status={'true' if response.status else 'false'}",
                flush=True,
            )
        return _inference_result(response)

    def deploy_plan(
        self,
        plan: DistributedInferencePlan,
        *,
        freshness_ms: int = 60000,
    ) -> DeploymentSession:
        """Install/cache static plan metadata and return a session handle.

        This is the explicit high-level form of the DI deployment/session
        boundary.  It does not execute inference.  It only ensures that static
        model/runtime metadata and key-scope material referenced by the plan are
        available to providers.  Reusing the returned session avoids
        republishing those static references for each inference request.
        """

        fingerprint = _plan_fingerprint(plan)
        references = self._published_plan_references(
            plan,
            fingerprint=fingerprint,
            freshness_ms=freshness_ms,
        )
        return DeploymentSession(
            plan=plan,
            fingerprint=fingerprint,
            references=references,
        )

    def invoke_plan(
        self,
        session: DeploymentSession,
        payload: bytes,
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        assignment_context=None,
        request_id: str = "",
    ) -> InferenceResult:
        """Invoke one inference against an already deployed plan session."""

        request_start = perf_counter()
        response = self._request_plan_session(
            session,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            request_id=request_id,
            assignment_context=assignment_context,
        )
        request_ms = _elapsed_ms(request_start)
        if _client_timing_enabled():
            print(
                "NDNSF_DI_CLIENT_INFERENCE_TIMING "
                f"service={session.plan.service} "
                f"mode=plan-session "
                f"fingerprint={session.fingerprint[:16]} "
                f"request_ms={request_ms:.2f} "
                f"total_ms={request_ms:.2f} "
                f"status={'true' if response.status else 'false'}",
                flush=True,
            )
        return _inference_result(response)

    def preflight_plan(
        self,
        session: DeploymentSession,
        payload: bytes,
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
    ) -> InferenceResult:
        """Warm a deployed plan session without counting it as measured inference.

        The request uses the normal NDNSF collaboration path.  It is intended
        for deployment/session preflight so provider/user hybrid message keys,
        role assignment, and artifact/session caches are hot before the first
        measured inference.  The method deliberately logs
        NDNSF_DI_CLIENT_PREFLIGHT_TIMING instead of
        NDNSF_DI_CLIENT_INFERENCE_TIMING so benchmark summaries can keep
        preflight separate from measured requests.
        """

        request_start = perf_counter()
        response = self._request_plan_session(
            session,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
        )
        request_ms = _elapsed_ms(request_start)
        if _client_timing_enabled():
            print(
                "NDNSF_DI_CLIENT_PREFLIGHT_TIMING "
                f"service={session.plan.service} "
                f"mode=plan-session "
                f"fingerprint={session.fingerprint[:16]} "
                f"request_ms={request_ms:.2f} "
                f"status={'true' if response.status else 'false'}",
                flush=True,
            )
        return _inference_result(response)

    def invoke_plan_async(
        self,
        session: DeploymentSession,
        payload: bytes,
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        on_result: Callable[[InferenceResult], None] | None = None,
        on_error: Callable[[BaseException], None] | None = None,
        assignment_context=None,
        request_id: str = "",
    ) -> Future:
        """Submit one inference against an already deployed plan session."""

        future = self._executor.submit(
            self.invoke_plan,
            session,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            assignment_context=assignment_context,
            request_id=request_id,
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

    def _request_plan_session(
        self,
        session: DeploymentSession,
        payload: bytes,
        *,
        ack_timeout_ms: int,
        timeout_ms: int,
        assignment_context=None,
        request_id: str = "",
    ) -> ServiceResponse:
        return self.user.request_collaboration(
            session.plan.service,
            payload,
            roles=session.plan.ndnsf_roles(),
            key_scopes=session.plan.key_scopes(),
            dependencies=session.plan.ndnsf_dependencies(),
            artifact_data_names=session.references.artifact_data_names,
            scope_key_data_names=session.references.scope_key_data_names,
            role_scopes=session.plan.role_scopes(),
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            assignment_context=assignment_context,
            request_id=request_id,
        )

    def _published_plan_references(
        self,
        plan: DistributedInferencePlan,
        *,
        fingerprint: str | None = None,
        freshness_ms: int,
    ) -> PublishedPlanReferences:
        fingerprint = fingerprint or _plan_fingerprint(plan)
        with self._published_plan_lock:
            cached = self._published_plans.get(fingerprint)
            if cached is not None:
                print(
                    "NDNSF_DI_PLAN_CACHE "
                    f"service={plan.service} fingerprint={fingerprint[:16]} hit=true",
                    flush=True,
                )
                return cached
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
            published = PublishedPlanReferences(
                artifact_data_names=artifact_data_names,
                scope_key_data_names=scope_key_data_names,
            )
            self._published_plans[fingerprint] = published
            print(
                "NDNSF_DI_PLAN_CACHE "
                f"service={plan.service} fingerprint={fingerprint[:16]} hit=false "
                f"artifacts={len(artifact_data_names)} scopes={len(scope_key_data_names)}",
                flush=True,
            )
            return published

    def infer_deployed_service(
        self,
        service: str,
        payload: bytes,
        *,
        roles: list[CollaborationRole],
        key_scopes: dict[str, list[str]],
        dependencies: list[CollaborationDependency | InferenceDependency | dict],
        role_scopes: dict[str, list[str]],
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        freshness_ms: int = 60000,
        assignment_context=None,
        request_id: str = "",
    ) -> InferenceResult:
        """Request a service whose model layout/artifacts are already deployed."""

        total_start = perf_counter()
        scope_start = perf_counter()
        scope_key_data_names = self.publish_scope_keys_for_scopes(
            service,
            key_scopes,
            object_label_prefix="inference-scope-key",
            freshness_ms=freshness_ms,
        )
        scope_ms = _elapsed_ms(scope_start)
        request_start = perf_counter()
        response: ServiceResponse = self.user.request_collaboration(
            service,
            payload,
            roles=roles,
            key_scopes=key_scopes,
            dependencies=[
                _to_collaboration_dependency(dep)
                for dep in dependencies
            ],
            artifact_data_names={},
            scope_key_data_names=scope_key_data_names,
            role_scopes=role_scopes,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            assignment_context=assignment_context,
            request_id=request_id,
        )
        request_ms = _elapsed_ms(request_start)
        total_ms = _elapsed_ms(total_start)
        if _client_timing_enabled():
            print(
                "NDNSF_DI_CLIENT_INFERENCE_TIMING "
                f"service={service} "
                f"mode=deployed "
                f"scope_key_ms={scope_ms:.2f} "
                f"request_ms={request_ms:.2f} "
                f"total_ms={total_ms:.2f} "
                f"status={'true' if response.status else 'false'}",
                flush=True,
            )
        return _inference_result(response)

    def infer_simple_service(
        self,
        service: str,
        payload: bytes,
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        request_id: str = "",
    ) -> InferenceResult:
        """Request a predeployed single-role service without DI assignment.

        This is the low-overhead path for replicated provider serving, such as
        one llama-server provider selected for one chat completion. Multi-role
        dependency graphs still use ``infer_deployed_service``.
        """

        request_start = perf_counter()
        response = self.user.request_service(
            service,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            request_id=request_id,
        )
        request_ms = _elapsed_ms(request_start)
        if _client_timing_enabled():
            print(
                "NDNSF_DI_CLIENT_INFERENCE_TIMING "
                f"service={service} "
                f"mode=simple-service "
                f"request_ms={request_ms:.2f} "
                f"total_ms={request_ms:.2f} "
                f"status={'true' if response.status else 'false'}",
                flush=True,
            )
        return _inference_result(response)

    def infer_simple_service_async(
        self,
        service: str,
        payload: bytes,
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        on_result: Callable[[InferenceResult], None] | None = None,
        on_error: Callable[[BaseException], None] | None = None,
        request_id: str = "",
    ) -> Future:
        future = self._executor.submit(
            self.infer_simple_service,
            service,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            request_id=request_id,
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

    def infer_deployed_service_async(
        self,
        service: str,
        payload: bytes,
        *,
        roles: list[CollaborationRole],
        key_scopes: dict[str, list[str]],
        dependencies: list[CollaborationDependency],
        role_scopes: dict[str, list[str]],
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        freshness_ms: int = 60000,
        on_result: Callable[[InferenceResult], None] | None = None,
        on_error: Callable[[BaseException], None] | None = None,
        request_id: str = "",
    ) -> Future:
        future = self._executor.submit(
            self.infer_deployed_service,
            service,
            payload,
            roles=roles,
            key_scopes=key_scopes,
            dependencies=dependencies,
            role_scopes=role_scopes,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            freshness_ms=freshness_ms,
            request_id=request_id,
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
        request_id: str = "",
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
            request_id=request_id,
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

"""Application-facing API for NDNSF-DistributedInference.

This module is the API surface intended for AI application developers. It hides
NDNSF-specific user/provider/controller classes while keeping deployment,
artifact provisioning, and collaboration generic enough for non-YOLO models.
"""

from __future__ import annotations

import os

import json
from dataclasses import dataclass, field
from concurrent.futures import Future
import hashlib
import io
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

from ndnsf import (
    CollaborationRole,
    LargeDataReference,
    encode_large_data_reference_payload,
)

from .client import DistributedInferenceClient, InferenceResult
from .client import DeploymentSession
from .controller import DistributedInferenceController
from .plan import (
    ArtifactSpec,
    DistributedInferencePlan,
    DependencyGraph,
    InferenceDependency,
    InferenceRole,
    RuntimeSpec,
)
from .policy import DistributedInferenceDeployment, load_or_generate_deployment
from .provider import (
    DistributedInferenceProvider,
    InferenceHandler,
    ProviderAdmissionPolicy,
    ProviderRuntimeContext,
)
from .repo_reference import repo_manifest_from_large_data_reference


@dataclass(frozen=True)
class ModelPart:
    """One assignable model/runtime part in an application plan."""

    role: str
    model: bytes | str | Path
    artifact_name: str = ""
    filename: str = ""
    kind: str = "model"
    backend: str = ""
    cache_name: str = ""
    large_data_reference: dict = field(default_factory=dict)
    repo_manifest: dict = field(default_factory=dict)
    runtime: RuntimeSpec | None = None
    service: str = ""
    metadata: dict = field(default_factory=dict)
    allow_dynamic_provisioning: bool = True
    provisioning_timeout_ms: int = 60000
    min_providers: int = 1
    max_providers: int = 1

    def payload(self) -> bytes:
        if isinstance(self.model, bytes):
            return self.model
        return Path(self.model).read_bytes()

    def inferred_filename(self) -> str:
        if self.filename:
            return self.filename
        if isinstance(self.model, (str, Path)):
            return Path(self.model).name
        return self.role.strip("/").replace("/", "-") + ".bin"


class InferencePlanBuilder:
    """Build a distributed inference plan using model/stage/shard language."""

    def __init__(self, *, service: str, model_name: str,
                 runtime: RuntimeSpec | None = None,
                 backend: str = "onnxruntime"):
        self.service = service
        self.model_name = model_name
        self.runtime = runtime or RuntimeSpec(
            name=f"/Runtime/{backend}",
            backend=backend,
            entrypoint="runner",
        )
        self._parts: list[ModelPart] = []
        self._dependencies: list[InferenceDependency] = []
        self._metadata: dict = {}

    @classmethod
    def for_service(
        cls,
        deployment: DistributedInferenceDeployment,
        service: str,
        *,
        runtime: RuntimeSpec | None = None,
        backend: str = "onnxruntime",
    ) -> "InferencePlanBuilder":
        service_policy = deployment.service_policy(service)
        builder = cls(
            service=service_policy.name,
            model_name=service_policy.model_name or service_policy.name,
            runtime=runtime,
            backend=backend,
        )
        builder._dependencies = list(service_policy.dependencies)
        return builder

    def metadata(self, **values) -> "InferencePlanBuilder":
        self._metadata.update(values)
        return self

    def add_part(
        self,
        *,
        role: str,
        model: bytes | str | Path,
        artifact_name: str = "",
        filename: str = "",
        kind: str = "model",
        backend: str = "",
        cache_name: str = "",
        large_data_reference: dict | None = None,
        repo_manifest: dict | None = None,
        runtime: RuntimeSpec | None = None,
        service: str = "",
        metadata: dict | None = None,
        allow_dynamic_provisioning: bool = True,
        provisioning_timeout_ms: int = 60000,
        min_providers: int = 1,
        max_providers: int = 1,
    ) -> "InferencePlanBuilder":
        self._parts.append(ModelPart(
            role=role,
            model=model,
            artifact_name=artifact_name or self._default_artifact_name(role),
            filename=filename,
            kind=kind,
            backend=backend or self.runtime.backend,
            cache_name=cache_name,
            large_data_reference=dict(large_data_reference or {}),
            repo_manifest=dict(repo_manifest or {}),
            runtime=runtime or self.runtime,
            service=service,
            metadata=dict(metadata or {}),
            allow_dynamic_provisioning=allow_dynamic_provisioning,
            provisioning_timeout_ms=provisioning_timeout_ms,
            min_providers=min_providers,
            max_providers=max_providers,
        ))
        return self

    def add_grid_part(
        self,
        *,
        stage: int,
        shard: int,
        model: bytes | str | Path,
        artifact_name: str = "",
        filename: str = "",
        kind: str = "model",
        backend: str = "",
        runtime: RuntimeSpec | None = None,
        metadata: dict | None = None,
    ) -> "InferencePlanBuilder":
        """Add one role from a stage/shard split.

        The helper is only naming sugar. A model split can be horizontal,
        vertical, pipeline, tensor-parallel, or any application-defined layout
        as long as the APP gives each assignable unit a role name.
        """

        role = f"/Stage/{stage}/Shard/{shard}"
        return self.add_part(
            role=role,
            model=model,
            artifact_name=artifact_name,
            filename=filename,
            kind=kind,
            backend=backend,
            runtime=runtime,
            metadata={
                "stage": stage,
                "shard": shard,
                **dict(metadata or {}),
            },
        )

    def build(self) -> DistributedInferencePlan:
        if not self._parts:
            raise ValueError("distributed inference plan must contain at least one part")
        roles = []
        for part in self._parts:
            roles.append(InferenceRole(
                role=part.role,
                artifact_name=part.artifact_name,
                backend=part.backend,
                model_artifact=ArtifactSpec(
                    name="model",
                    payload=part.payload(),
                    filename=part.inferred_filename(),
                    kind=part.kind,
                    cache_name=part.cache_name,
                    large_data_reference=dict(part.large_data_reference or {}),
                    repo_manifest=dict(part.repo_manifest or {}),
                ),
                runtime=part.runtime or self.runtime,
                service=part.service,
                allow_dynamic_provisioning=part.allow_dynamic_provisioning,
                provisioning_timeout_ms=part.provisioning_timeout_ms,
                min_providers=part.min_providers,
                max_providers=part.max_providers,
                metadata=dict(part.metadata),
            ))
        return DistributedInferencePlan(
            service=self.service,
            model_name=self.model_name,
            roles=roles,
            dependencies=list(self._dependencies),
            metadata=dict(self._metadata),
        )

    def _default_artifact_name(self, role: str) -> str:
        model = self.model_name.strip("/") or "Model"
        if model.startswith("Model/"):
            model_prefix = "/" + model
        else:
            model_prefix = f"/Model/{model}"
        normalized_role = role.strip("/") or "Role"
        return f"{model_prefix}/{normalized_role}"


class APPClient:
    """Simple client facade for AI application code."""

    def __init__(self, deployment: DistributedInferenceDeployment,
                 client: DistributedInferenceClient):
        self.deployment = deployment
        self._client = client
        self._input_encoders: dict[str, Callable[[Any], bytes]] = {}
        self._service_runtimes: dict[str, RuntimeSpec] = {}

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        generated_policy_dir: str | Path = "/tmp/ndnsf-di-policy",
        group: str = "",
        permission_wait_ms: int = 2500,
        async_workers: int = 4,
        adaptive_admission: bool = False,
    ) -> "APPClient":
        trace_init = os.environ.get("NDNSF_DI_INIT_TRACE") == "1"
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=load_or_generate_deployment_start",
                  flush=True)
        deployment = load_or_generate_deployment(config, generated_policy_dir)
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=load_or_generate_deployment_done",
                  flush=True)
        client = DistributedInferenceClient.connect(
            group=group or deployment.group,
            controller=deployment.controller,
            user=deployment.user,
            trust_schema=deployment.trust_schema,
            permission_wait_ms=permission_wait_ms,
            async_workers=async_workers,
            adaptive_admission=adaptive_admission,
        )
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=client_connect_done", flush=True)
        return cls(deployment, client)

    def infer(self, plan: DistributedInferencePlan, payload: bytes, *,
              ack_timeout_ms: int = 500,
              timeout_ms: int = 30000,
              freshness_ms: int = 60000) -> InferenceResult:
        plan = self._with_service_dependencies(plan)
        return self._client.infer(
            plan,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            freshness_ms=freshness_ms,
        )

    def deploy_plan(self, plan: DistributedInferencePlan, *,
                    freshness_ms: int = 60000) -> DeploymentSession:
        """Install/cache static plan metadata and return a reusable session."""

        return self._client.deploy_plan(
            self._with_service_dependencies(plan),
            freshness_ms=freshness_ms,
        )

    def invoke_plan(self, session: DeploymentSession, payload: bytes, *,
                    ack_timeout_ms: int = 500,
                    timeout_ms: int = 30000) -> InferenceResult:
        """Invoke one inference against a previously deployed plan session."""

        return self._client.invoke_plan(
            session,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
        )

    def preflight_plan(self, session: DeploymentSession, payload: bytes, *,
                       ack_timeout_ms: int = 500,
                       timeout_ms: int = 30000) -> InferenceResult:
        """Warm one deployed plan session without marking it as measured inference."""

        return self._client.preflight_plan(
            session,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
        )

    def invoke_plan_async(self, session: DeploymentSession, payload: bytes, *,
                          ack_timeout_ms: int = 500,
                          timeout_ms: int = 30000,
                          on_result: Callable[[InferenceResult], None] | None = None,
                          on_error: Callable[[BaseException], None] | None = None) -> Future:
        """Submit one inference against a previously deployed plan session."""

        return self._client.invoke_plan_async(
            session,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            on_result=on_result,
            on_error=on_error,
        )

    def distributed_inference(self, service: str, value: Any, *,
                              ack_timeout_ms: int = 500,
                              timeout_ms: int = 30000,
                              freshness_ms: int = 60000,
                              dynamic_provisioning: bool | None = None,
                              runtime: RuntimeSpec | None = None,
                              artifact_references: dict | str | Path | None = None,
                              role_app_requirements: Mapping[str, bytes] | None = None) -> InferenceResult:
        """Run one distributed inference request for a deployed service.

        The normal application-facing path is service-level: the caller names
        the service and passes an application object, tensor bundle, or already
        encoded bytes. The service policy fixes the roles, dependency graph,
        input codec, and default artifacts. If artifacts are present, the
        client publishes an execution plan so homogeneous providers can be
        assigned roles and fetch the needed shard at request time. If no
        artifacts are present, this falls back to the pre-deployed model path.
        """

        service_policy = self.deployment.service_policy(service)
        payload = self.encode_input(service, value)
        if dynamic_provisioning is None:
            dynamic_provisioning = bool(service_policy.artifacts or artifact_references)
        if dynamic_provisioning:
            return self.infer(
                self.service_plan(
                    service,
                    runtime=runtime,
                    artifact_references=artifact_references,
                ),
                payload,
                ack_timeout_ms=ack_timeout_ms,
                timeout_ms=timeout_ms,
                freshness_ms=freshness_ms,
            )
        return self._infer_predeployed_service(
            service,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            freshness_ms=freshness_ms,
            role_app_requirements=role_app_requirements,
        )

    def service_plan(
        self,
        service: str,
        *,
        runtime: RuntimeSpec | None = None,
        artifact_references: dict | str | Path | None = None,
    ) -> DistributedInferencePlan:
        """Build a dynamic provisioning plan from a service policy.

        This is the service-level equivalent of manually constructing a
        ``DistributedInferencePlan``. It is useful when callers want to inspect
        or reuse the plan, but most applications can call
        ``distributed_inference`` directly.
        """

        service_policy = self.deployment.service_policy(service)
        if not service_policy.artifacts:
            raise ValueError(
                f"service {service} has no artifact descriptions; "
                "use dynamic_provisioning=False for pre-deployed providers")
        manifests = self._load_artifact_references(artifact_references)
        runtime = runtime or self._service_runtimes.get(service) or self._default_runtime(service)
        builder = self.plan_builder(
            service,
            runtime=runtime,
            backend=runtime.backend or self._default_backend(service),
        )
        for artifact in service_policy.artifacts:
            role_manifests = self._artifact_references_for_role(manifests, artifact.role)
            role_runtime = runtime
            if role_manifests and runtime.artifact is not None and "runner" in role_manifests:
                runner_reference = self._large_data_reference_for_entry(role_manifests["runner"])
                runner_manifest = repo_manifest_from_large_data_reference(role_manifests["runner"])
                role_runtime = RuntimeSpec(
                    name=runtime.name,
                    backend=runtime.backend,
                    entrypoint=runtime.entrypoint,
                    artifact=ArtifactSpec(
                        name=runtime.artifact.name,
                        payload=b"",
                        filename=runtime.artifact.filename,
                        kind=runtime.artifact.kind,
                        executable=runtime.artifact.executable,
                        cache_name=runtime.artifact.cache_name,
                        large_data_reference=runner_reference,
                        repo_manifest=runner_manifest,
                    ),
                )
            model_reference = (
                self._large_data_reference_for_entry(role_manifests["model"])
                if role_manifests and "model" in role_manifests else {}
            )
            model_manifest = (
                repo_manifest_from_large_data_reference(role_manifests["model"])
                if role_manifests and "model" in role_manifests else {}
            )
            builder.add_part(
                role=artifact.role,
                model=b"" if role_manifests else artifact.path,
                artifact_name=artifact.artifact_name,
                filename=artifact.filename,
                kind=artifact.kind,
                backend=artifact.backend or runtime.backend,
                cache_name=artifact.artifact_name,
                large_data_reference=model_reference,
                repo_manifest=model_manifest,
                runtime=role_runtime,
                metadata=dict(artifact.metadata or {}),
                allow_dynamic_provisioning=True,
            )
        return builder.build()

    def register_runtime(self, service: str, runtime: RuntimeSpec) -> None:
        """Register a default runtime artifact for service-level invocation."""

        self.deployment.service_policy(service)
        self._service_runtimes[service] = runtime

    def _infer_predeployed_service(
        self,
        service: str,
        payload: bytes,
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        freshness_ms: int = 60000,
        role_app_requirements: Mapping[str, bytes] | None = None,
    ) -> InferenceResult:
        service_policy = self.deployment.service_policy(service)
        role_names = list(service_policy.roles)
        dependencies = list(service_policy.dependencies)
        if len(role_names) == 1 and not dependencies:
            return self._client.infer_simple_service(
                service,
                payload,
                ack_timeout_ms=ack_timeout_ms,
                timeout_ms=timeout_ms,
            )
        key_scopes: dict[str, set[str]] = {}
        role_scopes: dict[str, list[str]] = {role: [] for role in role_names}
        for dep in dependencies:
            scope_roles = key_scopes.setdefault(dep.key_scope, set())
            scope_roles.update(dep.producers)
            scope_roles.update(dep.consumers)
            for role in dep.producers + dep.consumers:
                role_scopes.setdefault(role, []).append(dep.key_scope)
        return self._client.infer_deployed_service(
            service,
            payload,
            roles=[
                CollaborationRole(
                    role=role,
                    service=service,
                    allow_dynamic_provisioning=False,
                    app_requirement=bytes((role_app_requirements or {}).get(role, b"")),
                )
                for role in role_names
            ],
            key_scopes={scope: sorted(roles)
                        for scope, roles in key_scopes.items()},
            dependencies=dependencies,
            role_scopes=role_scopes,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            freshness_ms=freshness_ms,
        )

    @staticmethod
    def _load_artifact_references(
        artifact_references: dict | str | Path | None,
    ) -> dict:
        if artifact_references is None:
            return {}
        if isinstance(artifact_references, dict):
            return dict(artifact_references)
        return json.loads(Path(artifact_references).read_text(encoding="utf-8"))

    @staticmethod
    def _artifact_references_for_role(manifests: dict, role: str) -> dict:
        if not manifests:
            return {}
        roles = manifests.get("roles", manifests)
        value = roles.get(role, {})
        if not isinstance(value, dict):
            raise ValueError(f"repo manifest entry for role {role} must be a mapping")
        return dict(value)

    @staticmethod
    def _large_data_reference_for_entry(entry: dict) -> dict:
        if not isinstance(entry, dict):
            raise ValueError("artifact reference entry must be a mapping")
        reference = entry.get("largeDataReference", entry.get("large_data_reference", {}))
        if not reference:
            return {}
        if not isinstance(reference, dict):
            raise ValueError("largeDataReference must be a mapping")
        return dict(reference)

    def _default_backend(self, service: str) -> str:
        service_policy = self.deployment.service_policy(service)
        for artifact in service_policy.artifacts:
            if artifact.backend:
                return artifact.backend
        return "onnxruntime"

    def _default_runtime(self, service: str) -> RuntimeSpec:
        backend = self._default_backend(service)
        return RuntimeSpec(
            name=f"/Runtime/{backend}",
            backend=backend,
            entrypoint="runner",
        )

    def describe_input(self, service: str) -> dict[str, Any]:
        """Describe the input payload expected by a service."""

        return dict(self.deployment.service_policy(service).input_schema or {})

    def describe_output(self, service: str) -> dict[str, Any]:
        """Describe the output payload returned by a service."""

        return dict(self.deployment.service_policy(service).output_schema or {})

    def register_input_encoder(
        self,
        service: str,
        encoder: Callable[[Any], bytes],
    ) -> None:
        """Register application logic that converts objects to request bytes."""

        self.deployment.service_policy(service)
        self._input_encoders[service] = encoder

    def encode_input(self, service: str, value: Any) -> bytes:
        """Encode one application input according to a service contract."""

        if isinstance(value, bytes):
            return value
        if isinstance(value, (bytearray, memoryview)):
            return bytes(value)
        contract = self.describe_input(service)
        encoder = self._input_encoders.get(service)
        if encoder is None:
            payload = self._encode_default_input(value, contract)
            if payload is None:
                raise ValueError(
                    f"service {service} has no registered input encoder and "
                    f"no built-in encoder matches contract={contract!r}")
        else:
            payload = encoder(value)
        if not isinstance(payload, bytes):
            raise TypeError("input encoder must return bytes")
        return payload

    @staticmethod
    def _encode_default_input(value: Any, contract: dict[str, Any]) -> bytes | None:
        codec = str(contract.get("codec", "")).lower()
        if codec != "npz":
            return None
        try:
            import numpy as np  # type: ignore
        except ImportError as exc:
            raise RuntimeError("built-in NPZ input encoding requires numpy") from exc

        fields = contract.get("fields", {})
        if isinstance(fields, Mapping):
            field_names = [str(name) for name in fields.keys()]
        else:
            field_names = []

        if isinstance(value, Mapping):
            values = {str(key): np.asarray(item) for key, item in value.items()}
        elif isinstance(value, tuple) and len(field_names) == len(value):
            values = {name: np.asarray(item) for name, item in zip(field_names, value)}
        elif isinstance(value, list) and len(field_names) == len(value):
            values = {name: np.asarray(item) for name, item in zip(field_names, value)}
        else:
            field_name = field_names[0] if len(field_names) == 1 else "input"
            values = {field_name: np.asarray(value)}

        buffer = io.BytesIO()
        np.savez(buffer, **values)
        return buffer.getvalue()

    def publish_large_payload(
        self,
        service: str,
        payload: bytes,
        *,
        object_label: str = "input",
        freshness_ms: int = 60000,
    ):
        """Publish a large application payload and return its NDNSF Data reference."""

        return self._client.user.publish_encrypted_large_data(
            service,
            bytes(payload),
            object_label=object_label,
            freshness_ms=freshness_ms,
        )

    def publish_large_payload_reference(
        self,
        service: str,
        payload: bytes,
        *,
        object_label: str = "input",
        object_type: str = "",
        freshness_ms: int = 60000,
        digest: str = "",
    ) -> bytes:
        """Publish a large payload and return a standard NDNSF reference payload.

        This is the preferred request payload for large application inputs:
        callers pass the returned bytes to distributed_inference(), while
        providers parse the reference and fetch the encrypted segmented Data.
        """

        published = self.publish_large_payload(
            service,
            payload,
            object_label=object_label,
            freshness_ms=freshness_ms,
        )
        if not published.success:
            raise RuntimeError(f"large payload publish failed: {published.error}")
        effective_digest = digest or ("sha256:" + hashlib.sha256(payload).hexdigest())
        return encode_large_data_reference_payload(LargeDataReference(
            data_name=published.encrypted_data_name,
            object_type=object_type,
            object_id=published.object_id,
            plaintext_size=len(payload),
            encrypted=True,
            digest=effective_digest,
        ))

    def async_distributed_inference(
        self,
        service: str,
        value: Any,
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        freshness_ms: int = 60000,
        dynamic_provisioning: bool | None = None,
        runtime: RuntimeSpec | None = None,
        artifact_references: dict | str | Path | None = None,
        on_result: Callable[[InferenceResult], None] | None = None,
        on_error: Callable[[BaseException], None] | None = None,
    ) -> Future:
        service_policy = self.deployment.service_policy(service)
        payload = self.encode_input(service, value)
        if dynamic_provisioning is None:
            dynamic_provisioning = bool(service_policy.artifacts or artifact_references)
        if dynamic_provisioning:
            return self.infer_async(
                self.service_plan(
                    service,
                    runtime=runtime,
                    artifact_references=artifact_references,
                ),
                payload,
                ack_timeout_ms=ack_timeout_ms,
                timeout_ms=timeout_ms,
                freshness_ms=freshness_ms,
                on_result=on_result,
                on_error=on_error,
            )
        service_policy = self.deployment.service_policy(service)
        role_names = list(service_policy.roles)
        dependencies = list(service_policy.dependencies)
        if len(role_names) == 1 and not dependencies:
            return self._client.infer_simple_service_async(
                service,
                payload,
                ack_timeout_ms=ack_timeout_ms,
                timeout_ms=timeout_ms,
                on_result=on_result,
                on_error=on_error,
            )
        key_scopes: dict[str, set[str]] = {}
        role_scopes: dict[str, list[str]] = {role: [] for role in role_names}
        for dep in dependencies:
            scope_roles = key_scopes.setdefault(dep.key_scope, set())
            scope_roles.update(dep.producers)
            scope_roles.update(dep.consumers)
            for role in dep.producers + dep.consumers:
                role_scopes.setdefault(role, []).append(dep.key_scope)
        return self._client.infer_deployed_service_async(
            service,
            payload,
            roles=[
                CollaborationRole(
                    role=role,
                    service=service,
                    allow_dynamic_provisioning=False,
                )
                for role in role_names
            ],
            key_scopes={scope: sorted(roles)
                        for scope, roles in key_scopes.items()},
            dependencies=dependencies,
            role_scopes=role_scopes,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            freshness_ms=freshness_ms,
            on_result=on_result,
            on_error=on_error,
        )

    def infer_async(self, plan: DistributedInferencePlan, payload: bytes, *,
                    ack_timeout_ms: int = 500,
                    timeout_ms: int = 30000,
                    freshness_ms: int = 60000,
                    on_result: Callable[[InferenceResult], None] | None = None,
                    on_error: Callable[[BaseException], None] | None = None) -> Future:
        """Submit one inference request on the client worker pool."""

        plan = self._with_service_dependencies(plan)
        return self._client.infer_async(
            plan,
            payload,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            freshness_ms=freshness_ms,
            on_result=on_result,
            on_error=on_error,
        )

    def infer_many_async(
        self,
        requests: Iterable[tuple[DistributedInferencePlan, bytes]],
        *,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30000,
        freshness_ms: int = 60000,
    ) -> list[Future]:
        """Submit multiple inference requests concurrently."""

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
        self._client.shutdown(wait=wait)

    def dependency_graph(self, service: str) -> DependencyGraph:
        return self.deployment.dependency_graph_for_service(service)

    def model_name_for_service(self, service: str) -> str:
        return self.deployment.service_policy(service).model_name

    def publish_service_manifest(
        self,
        service: str = "",
        *,
        object_label: str = "service-manifest",
        freshness_ms: int = 60000,
    ):
        """Publish the service manifest as signed NDN Data.

        The Python wrapper uses NDNSF encrypted large Data, which stores
        encrypted content in NDN Data packets signed by the local identity. The
        local ``.sha256`` file is only a fingerprint for deployment tooling; it
        is not a replacement for NDN Data signatures.
        """

        if not service:
            if len(self.deployment.services) != 1:
                raise ValueError(
                    "service must be specified when deployment has multiple services")
            service = self.deployment.services[0].name
        return self._client.user.publish_encrypted_large_data(
            service,
            self.deployment.service_manifest_payload(),
            object_label=object_label,
            freshness_ms=freshness_ms,
        )

    def plan_builder(self, service: str, *,
                     runtime: RuntimeSpec | None = None,
                     backend: str = "onnxruntime") -> InferencePlanBuilder:
        return InferencePlanBuilder.for_service(
            self.deployment,
            service,
            runtime=runtime,
            backend=backend,
        )

    def _with_service_dependencies(
        self,
        plan: DistributedInferencePlan,
    ) -> DistributedInferencePlan:
        service_policy = self.deployment.service_policy(plan.service)
        return DistributedInferencePlan(
            service=plan.service,
            model_name=plan.model_name or service_policy.model_name,
            roles=list(plan.roles),
            dependencies=list(service_policy.dependencies),
            metadata=dict(plan.metadata),
        )


class APPDeployment:
    """Generate and inspect deployment files without starting NDNSF runtime."""

    def __init__(self, deployment: DistributedInferenceDeployment):
        self.deployment = deployment

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        generated_policy_dir: str | Path = "/tmp/ndnsf-di-policy",
    ) -> "APPDeployment":
        return cls(load_or_generate_deployment(config, generated_policy_dir))

    @property
    def trust_schema(self) -> str:
        return self.deployment.trust_schema

    @property
    def policy_file(self) -> str:
        return self.deployment.policy_file

    def roles_for_service(self, service: str) -> list[str]:
        return list(self.deployment.service_policy(service).roles)

    def dependency_graph(self, service: str) -> DependencyGraph:
        return self.deployment.dependency_graph_for_service(service)

    def model_name_for_service(self, service: str) -> str:
        return self.deployment.service_policy(service).model_name


class APPProvider:
    """Simple provider facade for AI application code."""

    def __init__(self, deployment: DistributedInferenceDeployment,
                 provider: DistributedInferenceProvider):
        self.deployment = deployment
        self._provider = provider

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        generated_policy_dir: str | Path = "/tmp/ndnsf-di-policy",
        provider_id: str = "",
        group: str = "",
        handler_threads: int = 4,
        ack_threads: int = 2,
        handler_workers: int = 0,
    ) -> "APPProvider":
        deployment = load_or_generate_deployment(config, generated_policy_dir)
        provider = DistributedInferenceProvider.create(
            provider_id=provider_id,
            group=group or deployment.group,
            controller=deployment.controller,
            provider_prefix=deployment.provider_prefix,
            trust_schema=deployment.trust_schema,
            handler_threads=handler_threads,
            ack_threads=ack_threads,
            handler_workers=handler_workers,
        )
        return cls(deployment, provider)

    def serve_service(
        self,
        *,
        service: str,
        roles: Sequence[str] | str,
        handler: InferenceHandler,
        backends: Sequence[str] = (),
        temp_dir: str | None = None,
        queue_depth: int = 0,
        has_model: bool = False,
        can_provision: bool = True,
        allow_executables: bool = False,
        readiness_probe: Callable[[], Any] | None = None,
        local_artifacts: dict[str, dict] | None = None,
        admission_policy: ProviderAdmissionPolicy | None = None,
    ) -> None:
        if allow_executables:
            self.deployment.require_executable_artifacts_allowed()
        if isinstance(roles, str):
            if roles.lower() == "all":
                roles = self.roles_for_service(service)
            else:
                roles = [part.strip() for part in roles.split(",") if part.strip()]
        service_policy = self.deployment.service_policy(service)
        policy_artifacts = {
            artifact.role: {
                "path": artifact.path,
                "artifact": artifact.artifact_name,
                "filename": artifact.filename,
                "kind": artifact.kind,
                "backend": artifact.backend,
                "metadata": dict(artifact.metadata or {}),
            }
            for artifact in service_policy.artifacts
        }
        if local_artifacts is not None:
            for role, artifact in policy_artifacts.items():
                local_artifacts.setdefault(role, artifact)
            policy_artifacts = local_artifacts
        self._provider.add_capability_handler(
            service,
            list(roles),
            handler,
            backends=backends,
            temp_dir=temp_dir,
            queue_depth=queue_depth,
            has_model=has_model,
            can_provision=can_provision,
            allow_executables=allow_executables,
            dependency_graph=self.deployment.dependency_graph_for_service(service),
            local_artifacts=policy_artifacts,
            readiness_probe=readiness_probe,
            admission_policy=admission_policy,
            register_simple_service=(
                len(list(roles)) == 1 and
                not list(self.deployment.service_policy(service).dependencies)
            ),
        )

    def serve(
        self,
        *,
        service: str,
        roles: Sequence[str] | str,
        handler: InferenceHandler,
        backends: Sequence[str] = (),
        temp_dir: str | None = None,
        queue_depth: int = 0,
        has_model: bool = False,
        can_provision: bool = True,
        allow_executables: bool = False,
        readiness_probe: Callable[[], Any] | None = None,
        local_artifacts: dict[str, dict] | None = None,
        admission_policy: ProviderAdmissionPolicy | None = None,
    ) -> None:
        self.serve_service(
            service=service,
            roles=roles,
            handler=handler,
            backends=backends,
            temp_dir=temp_dir,
            queue_depth=queue_depth,
            has_model=has_model,
            can_provision=can_provision,
            allow_executables=allow_executables,
            readiness_probe=readiness_probe,
            local_artifacts=local_artifacts,
            admission_policy=admission_policy,
        )

    def roles_for_service(self, service: str) -> list[str]:
        return list(self.deployment.service_policy(service).roles)

    def dependency_graph(self, service: str) -> DependencyGraph:
        return self.deployment.dependency_graph_for_service(service)

    def model_name_for_service(self, service: str) -> str:
        return self.deployment.service_policy(service).model_name

    def run(self) -> int:
        return self._provider.run()

    def stop(self) -> int:
        return self._provider.stop()


class APPController:
    """Simple deployment/controller facade for application code."""

    def __init__(self, deployment: DistributedInferenceDeployment,
                 controller: DistributedInferenceController):
        self.deployment = deployment
        self._controller = controller

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        generated_policy_dir: str | Path = "/tmp/ndnsf-di-policy",
    ) -> "APPController":
        deployment = load_or_generate_deployment(config, generated_policy_dir)
        controller = DistributedInferenceController.create(
            controller_prefix=deployment.controller,
            policy_file=deployment.policy_file,
            trust_schema=deployment.trust_schema,
            bootstrap_identities=deployment.bootstrap_identities,
        )
        return cls(deployment, controller)

    def run(self) -> int:
        return self._controller.run()

    def stop(self) -> int:
        return self._controller.stop()

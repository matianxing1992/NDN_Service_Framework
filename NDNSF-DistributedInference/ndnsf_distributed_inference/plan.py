"""Generic distributed-inference execution-plan objects.

This layer is intentionally above NDNSF Core. It understands model/runtime
artifacts, roles, stages, and data dependencies, then compiles them into the
generic NDNSF Python collaboration API.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from enum import Enum
import hashlib
import json
from types import MappingProxyType
from typing import Mapping, Optional

from ndnsf import CollaborationDependency, CollaborationRole
from .core.contracts import DATA_DRIVEN_V2
from .repo_reference import large_data_reference_from_repo_manifest


# Rollback-only V1 rendezvous. DATA_DRIVEN_V2 never emits or waits on this
# all-member scope; it uses local preparation plus direct predecessor data.
NDNSF_DI_READINESS_SCOPE = "ndnsf-di-readiness-v1"


class ModelFamily(str, Enum):
    """Model/application family understood by DI planners.

    This is planning metadata, not an execution backend.  It lets the same
    deployment/runtime code dispatch to YOLO-, generic ONNX-, or future
    LLM-specific planners without encoding model-family assumptions into the
    native execution-plan schema.
    """

    GENERIC_ONNX = "generic-onnx"
    YOLO_ONNX = "yolo-onnx"
    LLM = "llm"


class ModelFormat(str, Enum):
    """Artifact/model container format consumed by a planner/runtime."""

    UNKNOWN = "unknown"
    ONNX = "onnx"
    GGUF = "gguf"
    SAFETENSORS = "safetensors"
    HF_TRANSFORMERS = "hf-transformers"
    CUSTOM = "custom"


class PlannerKind(str, Enum):
    """Planner strategy used to produce role/dependency metadata."""

    ONNX_DAG = "onnx-dag"
    YOLO_SEQUENTIAL_CHUNKS = "yolo-sequential-chunks"
    YOLO_DETECT_AUTO = "yolo-detect-auto"
    YOLO_DETECT_SHARED_BACKBONE = "yolo-detect-shared-backbone"
    YOLO_DETECT_REPLICATED_BACKBONE = "yolo-detect-replicated-backbone"
    YOLO_OUTPUT_CHANNEL_SHARDS = "yolo-output-channel-shards"
    LLM_PIPELINE = "llm-pipeline"
    LLM_PREFILL_DECODE = "llm-prefill-decode"
    LLM_TENSOR_PARALLEL = "llm-tensor-parallel"


def _enum_value(value: str | Enum | None, default: str) -> str:
    if value is None:
        return default
    if isinstance(value, Enum):
        return str(value.value)
    text = str(value).strip()
    return text or default


def normalize_model_family(value: str | ModelFamily | None) -> str:
    return _enum_value(value, ModelFamily.GENERIC_ONNX.value)


def normalize_model_format(value: str | ModelFormat | None) -> str:
    return _enum_value(value, ModelFormat.UNKNOWN.value)


def normalize_planner_kind(value: str | PlannerKind | None) -> str:
    return _enum_value(value, PlannerKind.ONNX_DAG.value)


@dataclass(frozen=True)
class PlannerDescriptor:
    """Stable planner identity embedded into generated execution plans."""

    model_family: str | ModelFamily = ModelFamily.GENERIC_ONNX
    model_format: str | ModelFormat = ModelFormat.UNKNOWN
    planner_kind: str | PlannerKind = PlannerKind.ONNX_DAG
    schema_version: int = 2
    metadata: dict = field(default_factory=dict)

    def normalized_model_family(self) -> str:
        return normalize_model_family(self.model_family)

    def normalized_model_format(self) -> str:
        return normalize_model_format(self.model_format)

    def normalized_planner_kind(self) -> str:
        return normalize_planner_kind(self.planner_kind)

    def to_metadata(self) -> dict:
        return {
            "modelFamily": self.normalized_model_family(),
            "modelFormat": self.normalized_model_format(),
            "plannerKind": self.normalized_planner_kind(),
            "schemaVersion": int(self.schema_version),
            **dict(self.metadata or {}),
        }


def stage_shard_role(stage: int, shard: int) -> str:
    """Return the canonical role name for an NxM stage/shard layout."""

    if stage < 0 or shard < 0:
        raise ValueError("stage and shard indices must be non-negative")
    return f"/Stage/{stage}/Shard/{shard}"


def nxm_stage_roles(stages: int, shards_per_stage: int) -> list[str]:
    """Return roles for true NxM semantics.

    ``stages`` is the number of vertical model stages. ``shards_per_stage`` is
    the number of parallel shards inside every stage.
    """

    if stages <= 0 or shards_per_stage <= 0:
        raise ValueError("stages and shards_per_stage must be positive")
    return [
        stage_shard_role(stage, shard)
        for stage in range(stages)
        for shard in range(shards_per_stage)
    ]


def nxm_stage_frontier_dependencies(
    stages: int,
    shards_per_stage: int,
    *,
    topic_prefix: str = "/activation",
    tensors_by_stage: dict[int, list[str]] | None = None,
) -> list["InferenceDependency"]:
    """Build stage-frontier dependencies for true NxM parallel sharding.

    Each edge connects all shards in stage ``s`` to all shards in stage
    ``s + 1``. A model-specific splitter still decides the actual tensor names,
    tensor partitioning, merge operators, and ONNX artifacts.
    """

    if stages <= 0 or shards_per_stage <= 0:
        raise ValueError("stages and shards_per_stage must be positive")
    dependencies: list[InferenceDependency] = []
    for stage in range(stages - 1):
        dependencies.append(InferenceDependency(
            producers=[
                stage_shard_role(stage, shard)
                for shard in range(shards_per_stage)
            ],
            consumers=[
                stage_shard_role(stage + 1, shard)
                for shard in range(shards_per_stage)
            ],
            key_scope=f"stage{stage}-to-stage{stage + 1}",
            topic_prefix=topic_prefix,
            tensors=list((tensors_by_stage or {}).get(stage, [])),
        ))
    return dependencies


@dataclass(frozen=True)
class ArtifactSpec:
    """A model, runtime, executable, config, or auxiliary artifact."""

    name: str
    payload: bytes
    filename: str
    kind: str = "model"
    executable: bool = False
    cache_name: str = ""
    large_data_reference: dict = field(default_factory=dict)
    repo_manifest: dict = field(default_factory=dict)

    def to_ndnsf_artifact(self) -> dict:
        repo_manifest = dict(self.repo_manifest or {})
        large_data_reference = dict(self.large_data_reference or {})
        artifact = {
            "payload": self.payload,
            "filename": self.filename,
            "kind": self.kind,
            "executable": self.executable,
            "cache_name": self.cache_name,
            "repo_manifest": repo_manifest,
            "repoManifest": repo_manifest,
            "large_data_reference": large_data_reference,
            "largeDataReference": large_data_reference,
        }
        if not large_data_reference and repo_manifest:
            large_data_reference = large_data_reference_from_repo_manifest(
                repo_manifest,
                object_type=self.kind,
                object_id=self.name,
            )
            artifact["large_data_reference"] = large_data_reference
            artifact["largeDataReference"] = large_data_reference
        return artifact


@dataclass(frozen=True)
class RuntimeSpec:
    """Runtime/backend requirement for a role.

    The runtime may be a local provider capability or a downloadable artifact.
    ``artifact`` is optional because some providers may already have the runtime
    installed and only need the model shard.
    """

    name: str
    backend: str
    entrypoint: str = "runner"
    artifact: Optional[ArtifactSpec] = None


@dataclass(frozen=True)
class InferenceRole:
    """One assignable unit in a distributed inference plan."""

    role: str
    artifact_name: str
    backend: str
    model_artifact: ArtifactSpec
    runtime: RuntimeSpec
    service: str = ""
    allow_dynamic_provisioning: bool = True
    provisioning_timeout_ms: int = 60000
    min_providers: int = 1
    max_providers: int = 1
    metadata: dict = field(default_factory=dict)

    def ndnsf_role(self, default_service: str) -> CollaborationRole:
        revision = str(self.metadata.get("deploymentRevision", ""))
        requirement = (
            f"deploymentRevision={revision};".encode("utf-8")
            if revision else b"")
        return CollaborationRole(
            role=self.role,
            service=self.service or default_service,
            artifact=self.artifact_name,
            allow_dynamic_provisioning=self.allow_dynamic_provisioning,
            provisioning_timeout_ms=self.provisioning_timeout_ms,
            min_providers=self.min_providers,
            max_providers=self.max_providers,
            app_requirement=requirement,
        )

    def artifacts(self) -> dict[str, dict]:
        artifacts = {
            "model": self.model_artifact.to_ndnsf_artifact(),
        }
        if self.runtime.artifact is not None:
            artifacts["runner"] = self.runtime.artifact.to_ndnsf_artifact()
        return artifacts


@dataclass(frozen=True)
class InferenceDependency:
    producers: list[str]
    consumers: list[str]
    key_scope: str
    topic_prefix: str
    required: bool = True
    tensors: list[str] = field(default_factory=list)
    object_name_template: str = ""
    expected_segments: int = 0
    expected_bytes: int = 0

    def ndnsf_dependency(self) -> CollaborationDependency:
        return CollaborationDependency(
            producers=list(self.producers),
            consumers=list(self.consumers),
            key_scope=self.key_scope,
            topic_prefix=self.topic_prefix,
            required=self.required,
        )


@dataclass(frozen=True)
class DependencyEdge:
    """One dependency edge visible to application role handlers."""

    producers: list[str]
    consumers: list[str]
    key_scope: str
    topic_prefix: str
    required: bool = True
    tensors: list[str] = field(default_factory=list)
    object_name_template: str = ""
    expected_segments: int = 0
    expected_bytes: int = 0

    def topic(self, suffix: str = "") -> str:
        if not suffix:
            return self.topic_prefix
        if suffix.startswith("/"):
            return self.topic_prefix.rstrip("/") + suffix
        return self.topic_prefix.rstrip("/") + "/" + suffix


@dataclass(frozen=True)
class RoleDependencyView:
    """Dependency view for a single assigned role."""

    role: str
    inputs: list[DependencyEdge] = field(default_factory=list)
    outputs: list[DependencyEdge] = field(default_factory=list)
    internal: list[DependencyEdge] = field(default_factory=list)

    def input(self, key_scope: str = "") -> DependencyEdge:
        return self._select(self.inputs, key_scope, "input")

    def output(self, key_scope: str = "") -> DependencyEdge:
        return self._select(self.outputs, key_scope, "output")

    def internal_scope(self, key_scope: str = "") -> DependencyEdge:
        return self._select(self.internal, key_scope, "internal")

    def _select(self, edges: list[DependencyEdge], key_scope: str,
                label: str) -> DependencyEdge:
        if key_scope:
            for edge in edges:
                if edge.key_scope == key_scope:
                    return edge
            raise KeyError(f"role {self.role} has no {label} edge {key_scope!r}")
        if len(edges) != 1:
            raise KeyError(
                f"role {self.role} has {len(edges)} {label} edges; "
                "pass key_scope explicitly")
        return edges[0]


@dataclass(frozen=True)
class DependencyGraph:
    """Dependency graph supplied by the model splitter or application.

    NDNSF-DistributedInference carries dependencies supplied by a splitter,
    application planner, or optional analyzer, then materializes role-local
    views for provider handlers.
    """

    roles: list[str]
    dependencies: list[DependencyEdge]

    @classmethod
    def from_dependencies(
        cls,
        roles: list[str],
        dependencies: list[InferenceDependency],
    ) -> "DependencyGraph":
        return cls(
            roles=list(roles),
            dependencies=[
                DependencyEdge(
                    producers=list(dep.producers),
                    consumers=list(dep.consumers),
                    key_scope=dep.key_scope,
                    topic_prefix=dep.topic_prefix,
                    required=dep.required,
                    tensors=list(dep.tensors),
                    object_name_template=dep.object_name_template,
                    expected_segments=dep.expected_segments,
                    expected_bytes=dep.expected_bytes,
                )
                for dep in dependencies
            ],
        )

    def for_role(self, role: str) -> RoleDependencyView:
        inputs: list[DependencyEdge] = []
        outputs: list[DependencyEdge] = []
        internal: list[DependencyEdge] = []
        for edge in self.dependencies:
            is_producer = role in edge.producers
            is_consumer = role in edge.consumers
            if is_producer and is_consumer:
                internal.append(edge)
            elif is_producer:
                outputs.append(edge)
            elif is_consumer:
                inputs.append(edge)
        return RoleDependencyView(
            role=role,
            inputs=inputs,
            outputs=outputs,
            internal=internal,
        )

    def key_scopes(self) -> dict[str, list[str]]:
        scopes: dict[str, set[str]] = {}
        for edge in self.dependencies:
            roles = scopes.setdefault(edge.key_scope, set())
            roles.update(edge.producers)
            roles.update(edge.consumers)
        return {scope: sorted(roles) for scope, roles in scopes.items()}

    def role_scopes(self) -> dict[str, list[str]]:
        mapping: dict[str, list[str]] = {role: [] for role in self.roles}
        for edge in self.dependencies:
            for role in edge.producers + edge.consumers:
                mapping.setdefault(role, []).append(edge.key_scope)
        return mapping


@dataclass(frozen=True)
class ContentionRetryPolicy:
    """Bounded probabilistic retry policy for partial DI reservations."""

    max_attempts: int = 3
    total_deadline_ms: int = 30_000
    base_backoff_ms: int = 100
    max_backoff_ms: int = 2_000

    def controller(self, *, started_at_ms: int, seed: int | None = None):
        from .core import ContentionRetryController
        return ContentionRetryController(
            max_attempts=self.max_attempts,
            total_deadline_ms=self.total_deadline_ms,
            base_backoff_ms=self.base_backoff_ms,
            max_backoff_ms=self.max_backoff_ms,
            seed=seed,
            started_at_ms=started_at_ms,
        )


@dataclass(frozen=True)
class DistributedInferencePlan:
    """APP/model-publisher supplied execution plan."""

    service: str
    model_name: str
    roles: list[InferenceRole]
    dependencies: list[InferenceDependency] = field(default_factory=list)
    metadata: dict = field(default_factory=dict)
    planner: PlannerDescriptor = field(default_factory=PlannerDescriptor)
    contention_retry: ContentionRetryPolicy = field(default_factory=ContentionRetryPolicy)
    result_contract: object | None = None

    def dependency_execution(self, *, request_id: str, attempt: int,
                             plan_digest: str, terminal_role: str = "",
                             evidence_verifier=lambda _fields: True,
                             role_bindings=None, generation: int = 1,
                             deadline_ms: int = (1 << 63) - 1,
                             result_contract=None,
                             start_callback=None, response_callback=None):
        """Create the maintained R1 local/direct-predecessor authority."""
        from .core import DependencyDrivenExecution
        roles = {role.role for role in self.roles}
        edges = {(source, target) for dep in self.dependencies
                 for source in dep.producers for target in dep.consumers}
        sinks = roles - {source for source, _target in edges}
        terminal = terminal_role or (next(iter(sinks)) if len(sinks) == 1 else "")
        if terminal not in roles:
            raise ValueError("R1 plan requires one explicit terminal role")
        sealed_result = (
            result_contract if result_contract is not None
            else self.result_contract)
        if role_bindings is not None and sealed_result is None:
            raise ValueError(
                "V2 dependency execution requires one sealed ResultContract")
        return DependencyDrivenExecution(
            request_id=request_id, attempt=attempt, plan_digest=plan_digest,
            roles=roles, edges=edges, terminal_role=terminal,
            evidence_verifier=evidence_verifier,
            role_bindings=role_bindings, generation=generation,
            deadline_ms=deadline_ms, result_contract=sealed_result,
            start_callback=start_callback,
            response_callback=response_callback)

    def role_map(self) -> dict[str, InferenceRole]:
        return {role.role: role for role in self.roles}

    def dependency_graph(self) -> DependencyGraph:
        return DependencyGraph.from_dependencies(
            [role.role for role in self.roles],
            self.dependencies,
        )

    def dependency_view_for_role(self, role: str) -> RoleDependencyView:
        return self.dependency_graph().for_role(role)

    def key_scopes(self) -> dict[str, list[str]]:
        scopes: dict[str, set[str]] = {}
        for dep in self.dependencies:
            roles = scopes.setdefault(dep.key_scope, set())
            roles.update(dep.producers)
            roles.update(dep.consumers)
        scopes[NDNSF_DI_READINESS_SCOPE] = {
            role.role for role in self.roles
        }
        return {scope: sorted(roles) for scope, roles in scopes.items()}

    def role_scopes(self) -> dict[str, list[str]]:
        mapping: dict[str, list[str]] = {
            role.role: [NDNSF_DI_READINESS_SCOPE] for role in self.roles
        }
        for dep in self.dependencies:
            for role in dep.producers + dep.consumers:
                mapping.setdefault(role, []).append(dep.key_scope)
        return mapping

    def ndnsf_roles(self) -> list[CollaborationRole]:
        role_names = sorted(role.role for role in self.roles)
        binding_payload = {
            "service": self.service,
            "model": self.model_name,
            "roles": role_names,
            "dependencies": [{
                "producers": sorted(dep.producers),
                "consumers": sorted(dep.consumers),
                "scope": dep.key_scope,
                "topic": dep.topic_prefix,
                "required": dep.required,
            } for dep in self.dependencies],
        }
        binding = str(self.metadata.get("deploymentRevision", "")) or (
            "sha256:" + hashlib.sha256(json.dumps(
                binding_payload, sort_keys=True,
                separators=(",", ":")).encode()).hexdigest())
        common = (
            "executionPolicy=LEGACY_READY_SET_V1;"
            f"readinessRoleCount={len(role_names)};"
            f"readinessRoles={','.join(role_names)};"
            f"readinessBindingDigest={binding};"
        ).encode()
        result = []
        for role in self.roles:
            value = role.ndnsf_role(self.service)
            result.append(replace(
                value, app_requirement=bytes(value.app_requirement) + common))
        return result

    def ndnsf_dependencies(self) -> list[CollaborationDependency]:
        return [dep.ndnsf_dependency() for dep in self.dependencies]


@dataclass(frozen=True)
class SealedCollaborationPlan:
    """Validated DI plan projected onto the generic Collaboration carrier."""

    ack_closed_digest: str
    placement_input_digest: str
    placement_decision_digest: str
    strategy_identity_digest: str
    execution_policy: str
    roles: tuple[CollaborationRole, ...]
    dependencies: tuple[CollaborationDependency, ...]
    key_scopes: Mapping[str, tuple[str, ...]]
    role_scopes: Mapping[str, tuple[str, ...]]
    providers_by_role: Mapping[str, str]
    artifact_data_names: Mapping[str, str]
    scope_key_data_names: Mapping[str, str]
    assignment_payloads_by_role: Mapping[str, bytes]

    def __post_init__(self) -> None:
        for name in (
                "ack_closed_digest", "placement_input_digest",
                "placement_decision_digest", "strategy_identity_digest"):
            value = getattr(self, name)
            if (not isinstance(value, str) or len(value) != 71
                    or not value.startswith("sha256:")):
                raise ValueError(f"{name} must be a canonical sha256 digest")
        if self.execution_policy != DATA_DRIVEN_V2:
            raise ValueError(
                "sealed collaboration plan requires DATA_DRIVEN_V2")
        roles = tuple(self.roles)
        role_names = tuple(role.role for role in roles)
        if (not roles or len(set(role_names)) != len(role_names)
                or set(self.providers_by_role) != set(role_names)
                or set(self.assignment_payloads_by_role) != set(role_names)):
            raise ValueError("sealed collaboration plan does not cover every role")
        for dependency in self.dependencies:
            if (not set(dependency.producers).issubset(set(role_names))
                    or not set(dependency.consumers).issubset(set(role_names))):
                raise ValueError("sealed dependency references an unknown role")
        object.__setattr__(self, "roles", roles)
        object.__setattr__(self, "dependencies", tuple(self.dependencies))
        for name in (
                "key_scopes", "role_scopes", "providers_by_role",
                "artifact_data_names", "scope_key_data_names",
                "assignment_payloads_by_role"):
            value = getattr(self, name)
            if name in ("key_scopes", "role_scopes"):
                normalized = {
                    str(key): tuple(items) for key, items in value.items()
                }
            elif name == "assignment_payloads_by_role":
                normalized = {
                    str(key): bytes(item) for key, item in value.items()
                }
            else:
                normalized = {
                    str(key): str(item) for key, item in value.items()
                }
            object.__setattr__(self, name, MappingProxyType(normalized))

    @property
    def plan_digest(self) -> str:
        plain = {
            "ack_closed_digest": self.ack_closed_digest,
            "placement_input_digest": self.placement_input_digest,
            "placement_decision_digest": self.placement_decision_digest,
            "strategy_identity_digest": self.strategy_identity_digest,
            "execution_policy": self.execution_policy,
            "roles": [{
                "role": role.role,
                "service": role.service,
                "artifact": role.artifact,
                "dynamic": role.allow_dynamic_provisioning,
                "timeout_ms": role.provisioning_timeout_ms,
                "min": role.min_providers,
                "max": role.max_providers,
            } for role in self.roles],
            "dependencies": [{
                "producers": list(dep.producers),
                "consumers": list(dep.consumers),
                "key_scope": dep.key_scope,
                "topic_prefix": dep.topic_prefix,
                "required": dep.required,
            } for dep in self.dependencies],
            "key_scopes": {
                key: list(value) for key, value in self.key_scopes.items()
            },
            "role_scopes": {
                key: list(value) for key, value in self.role_scopes.items()
            },
            "providers_by_role": dict(self.providers_by_role),
            "artifact_data_names": dict(self.artifact_data_names),
            "scope_key_data_names": dict(self.scope_key_data_names),
            "assignment_payloads": {
                key: value.hex()
                for key, value in self.assignment_payloads_by_role.items()
            },
        }
        wire = json.dumps(
            plain, sort_keys=True, separators=(",", ":"),
        ).encode("utf-8")
        return "sha256:" + hashlib.sha256(wire).hexdigest()

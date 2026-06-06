"""Generic distributed-inference execution-plan objects.

This layer is intentionally above NDNSF Core. It understands model/runtime
artifacts, roles, stages, and data dependencies, then compiles them into the
generic NDNSF Python collaboration API.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

from ndnsf import CollaborationDependency, CollaborationRole
from .repo import large_data_reference_from_repo_manifest


@dataclass(frozen=True)
class ArtifactSpec:
    """A model, runtime, executable, config, or auxiliary artifact."""

    name: str
    payload: bytes
    filename: str
    kind: str = "model"
    executable: bool = False
    cache_name: str = ""
    repo_manifest: dict = field(default_factory=dict)

    def to_ndnsf_artifact(self) -> dict:
        artifact = {
            "payload": self.payload,
            "filename": self.filename,
            "kind": self.kind,
            "executable": self.executable,
            "cache_name": self.cache_name,
            "repo_manifest": dict(self.repo_manifest or {}),
        }
        if self.repo_manifest:
            artifact["large_data_reference"] = large_data_reference_from_repo_manifest(
                self.repo_manifest,
                object_type=self.kind,
                object_id=self.name,
            )
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
        return CollaborationRole(
            role=self.role,
            service=self.service or default_service,
            artifact=self.artifact_name,
            allow_dynamic_provisioning=self.allow_dynamic_provisioning,
            provisioning_timeout_ms=self.provisioning_timeout_ms,
            min_providers=self.min_providers,
            max_providers=self.max_providers,
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
class DistributedInferencePlan:
    """APP/model-publisher supplied execution plan."""

    service: str
    model_name: str
    roles: list[InferenceRole]
    dependencies: list[InferenceDependency] = field(default_factory=list)
    metadata: dict = field(default_factory=dict)

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
        return {scope: sorted(roles) for scope, roles in scopes.items()}

    def role_scopes(self) -> dict[str, list[str]]:
        mapping: dict[str, list[str]] = {role.role: [] for role in self.roles}
        for dep in self.dependencies:
            for role in dep.producers + dep.consumers:
                mapping.setdefault(role, []).append(dep.key_scope)
        return mapping

    def ndnsf_roles(self) -> list[CollaborationRole]:
        return [role.ndnsf_role(self.service) for role in self.roles]

    def ndnsf_dependencies(self) -> list[CollaborationDependency]:
        return [dep.ndnsf_dependency() for dep in self.dependencies]

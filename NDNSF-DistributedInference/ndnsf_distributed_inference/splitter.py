"""Standard output objects for model-specific splitters.

NDNSF-DistributedInference intentionally does not try to infer model
dependencies. A model-specific splitter should produce these objects after it
has split the model into deployable parts.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .plan import InferenceDependency


@dataclass(frozen=True)
class SplitArtifact:
    """One artifact produced by a model splitter."""

    role: str
    path: str
    artifact_name: str
    filename: str = ""
    kind: str = "model"
    backend: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)

    def resolved_filename(self) -> str:
        return self.filename or Path(self.path).name


@dataclass(frozen=True)
class SplitServiceSpec:
    """A service layout emitted by a splitter.

    The service name identifies exactly one model layout. Different splits of
    the same model should be represented as different services.
    """

    name: str
    model_name: str
    roles: list[str]
    dependencies: list[InferenceDependency]
    artifacts: list[SplitArtifact] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)

    def to_policy_service(
        self,
        *,
        users: list[str],
        providers: list[dict[str, Any]],
    ) -> dict[str, Any]:
        return {
            "name": self.name,
            "model": self.model_name,
            "roles": list(self.roles),
            "dependencies": [
                {
                    "producers": list(dep.producers),
                    "consumers": list(dep.consumers),
                    "key_scope": dep.key_scope,
                    "topic_prefix": dep.topic_prefix,
                    "required": dep.required,
                }
                for dep in self.dependencies
            ],
            "users": list(users),
            "providers": list(providers),
        }

    def artifact_for_role(self, role: str) -> SplitArtifact:
        for artifact in self.artifacts:
            if artifact.role == role:
                return artifact
        raise KeyError(f"split service {self.name} has no artifact for role {role}")


@dataclass(frozen=True)
class SplitterOutput:
    """Complete deployment-facing output from a model splitter."""

    application: str
    controller: str
    group: str
    user: str
    provider_prefix: str
    services: list[SplitServiceSpec]
    trust_app_roots: list[str] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)

    def service(self, name: str) -> SplitServiceSpec:
        for service in self.services:
            if service.name == name:
                return service
        raise KeyError(f"splitter output has no service {name}")

    def to_policy_config(self) -> dict[str, Any]:
        providers = [
            {"identity": self.provider_prefix, "roles": "all"},
            {"identity": self.provider_prefix.rstrip("/") + "/A", "roles": "all"},
        ]
        return {
            "application": self.application,
            "controller": self.controller,
            "group": self.group,
            "identities": {
                "user": self.user,
                "provider_prefix": self.provider_prefix,
            },
            "trust": {
                "app_roots": list(self.trust_app_roots),
            },
            "services": [
                service.to_policy_service(users=[self.user], providers=providers)
                for service in self.services
            ],
        }

    def write_policy_config(self, path: str | Path) -> None:
        """Write the generated service policy YAML.

        The dependency graph in this file is splitter output, not handwritten
        NDNSF logic.
        """

        try:
            import yaml  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "Writing splitter policy YAML requires PyYAML"
            ) from exc
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(
            yaml.safe_dump(self.to_policy_config(), sort_keys=False),
            encoding="utf-8",
        )

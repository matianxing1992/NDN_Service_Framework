"""Standard output objects for model splitters and deployment planners.

NDNSF-DistributedInference accepts this output regardless of whether it came
from an ONNX analyzer, a PyTorch/model-specific splitter, a handwritten
application planner, or a future optimizer.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .plan import InferenceDependency


def _append_unique(mapping: dict[str, list[Any]], key: str, value: Any) -> None:
    values = mapping.setdefault(key, [])
    if value not in values:
        values.append(value)


def authorization_summary(config: dict[str, Any]) -> dict[str, Any]:
    """Return a deployment-review summary derived from service permissions."""

    user_services: dict[str, list[Any]] = {}
    provider_services: dict[str, list[Any]] = {}
    for service in config.get("services", []):
        if not isinstance(service, dict):
            continue
        service_name = str(service.get("name", ""))
        if not service_name:
            continue
        for user in service.get("users", []) or []:
            _append_unique(user_services, str(user), service_name)
        for provider in service.get("providers", []) or []:
            if not isinstance(provider, dict):
                continue
            identity = str(provider.get("identity", ""))
            if not identity:
                continue
            _append_unique(provider_services, identity, {
                "service": service_name,
                "roles": provider.get("roles", []),
            })
    return {
        "users": [
            {"identity": identity, "services": services}
            for identity, services in user_services.items()
        ],
        "providers": [
            {"identity": identity, "services": services}
            for identity, services in provider_services.items()
        ],
    }


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
    input_schema: dict[str, Any] = field(default_factory=dict)
    output_schema: dict[str, Any] = field(default_factory=dict)
    users: list[str] = field(default_factory=list)
    providers: list[dict[str, Any]] = field(default_factory=list)
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
            "users": list(self.users or users),
            "providers": list(self.providers or providers),
            "roles": list(self.roles),
            "dependencies": [
                {
                    "producers": list(dep.producers),
                    "consumers": list(dep.consumers),
                    "key_scope": dep.key_scope,
                    "topic_prefix": dep.topic_prefix,
                    "required": dep.required,
                    **({"tensors": list(dep.tensors)} if dep.tensors else {}),
                }
                for dep in self.dependencies
            ],
            "artifacts": [
                {
                    "role": artifact.role,
                    "path": artifact.path,
                    "artifact": artifact.artifact_name,
                    "filename": artifact.resolved_filename(),
                    "kind": artifact.kind,
                    "backend": artifact.backend,
                    "metadata": dict(artifact.metadata),
                }
                for artifact in self.artifacts
            ],
            "input": dict(self.input_schema),
            "output": dict(self.output_schema),
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
    provider_identities: list[str] = field(default_factory=list)
    trust_app_roots: list[str] = field(default_factory=list)
    trust_anchor_file: str = ""
    artifact_allowlist: list[str] = field(default_factory=list)
    artifact_sandbox: dict[str, Any] = field(default_factory=dict)
    metadata: dict[str, Any] = field(default_factory=dict)

    def service(self, name: str) -> SplitServiceSpec:
        for service in self.services:
            if service.name == name:
                return service
        raise KeyError(f"splitter output has no service {name}")

    def to_policy_config(self) -> dict[str, Any]:
        provider_identities = list(self.provider_identities) or [
            self.provider_prefix,
            self.provider_prefix.rstrip("/") + "/A",
            self.provider_prefix.rstrip("/") + "/B",
            self.provider_prefix.rstrip("/") + "/C",
        ]
        providers = [
            {"identity": identity, "roles": "all"}
            for identity in provider_identities
        ]
        return {
            "application": self.application,
            "controller": self.controller,
            "group": self.group,
            "runtime": {
                "user_identity": self.user,
                "provider_prefix": self.provider_prefix,
            },
            "trust": {
                "app_roots": list(self.trust_app_roots),
            },
            "artifact_security": {
                **({"anchor_file": self.trust_anchor_file}
                   if self.trust_anchor_file else {}),
                "allowlist": list(self.artifact_allowlist),
                "sandbox": dict(self.artifact_sandbox),
            },
            "services": [
                service.to_policy_service(
                    users=[self.user],
                    providers=providers,
                )
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
        config = self.to_policy_config()
        editable_keys = (
            "application",
            "controller",
            "group",
            "runtime",
            "trust",
            "artifact_security",
        )
        editable_section = {
            key: config[key]
            for key in editable_keys
            if key in config
        }
        generated_section = {
            "services": config.get("services", []),
        }
        text = (
            "# Generated by NDNSF-DistributedInference.\n"
            "# editable deployment section\n"
            "# Edit these fields when moving this deployment to a new\n"
            "# namespace, controller, trust root, runtime identity, or\n"
            "# artifact security policy. runtime.user_identity must also\n"
            "# appear in at least one service users list below.\n\n"
            + yaml.safe_dump(editable_section, sort_keys=False)
            + "\n"
            "# generated authorization summary\n"
            "# Read-only review aid derived from services[].users/providers.\n"
            "# Do not treat this as a second permission source; edit exact\n"
            "# service users/providers in the model-plan section below.\n\n"
            + yaml.safe_dump(
                {"authorization_summary": authorization_summary(config)},
                sort_keys=False,
            )
            + "\n"
            "# generated model-plan section\n"
            "# For each service, edit only exact users/providers for deployment.\n"
            "# roles/dependencies/artifacts/input/output are splitter or planner\n"
            "# output; regenerate this section when the model split changes.\n\n"
            + yaml.safe_dump(generated_section, sort_keys=False)
        )
        target.write_text(
            text,
            encoding="utf-8",
        )

"""Generate NDNSF security files from user-facing inference policy config."""

from __future__ import annotations

import json
import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .plan import DependencyGraph, InferenceDependency


@dataclass(frozen=True)
class ProviderPolicy:
    identity: str
    roles: tuple[str, ...] = ()


@dataclass(frozen=True)
class ArtifactPolicy:
    role: str
    path: str
    artifact_name: str = ""
    filename: str = ""
    kind: str = "model"
    backend: str = ""
    metadata: dict[str, Any] = None


@dataclass(frozen=True)
class ServicePolicy:
    name: str
    model_name: str
    roles: tuple[str, ...]
    dependencies: tuple[InferenceDependency, ...]
    users: tuple[str, ...]
    providers: tuple[ProviderPolicy, ...]
    artifacts: tuple[ArtifactPolicy, ...] = ()
    input_schema: dict[str, Any] = None
    output_schema: dict[str, Any] = None


@dataclass(frozen=True)
class SandboxPolicy:
    kind: str = ""
    image: str = ""
    command: tuple[str, ...] = ()
    workdir: str = ""

    @property
    def configured(self) -> bool:
        return bool(self.kind and (self.image or self.command))


@dataclass(frozen=True)
class ArtifactSecurityPolicy:
    anchor_file: str = ""
    allowlist: tuple[str, ...] = ()
    sandbox: SandboxPolicy = SandboxPolicy()

    @property
    def has_allowlist(self) -> bool:
        return bool(self.allowlist)


@dataclass(frozen=True)
class DistributedInferenceDeployment:
    application: str
    controller: str
    group: str
    user: str
    provider_prefix: str
    services: tuple[ServicePolicy, ...]
    trust_schema: str
    policy_file: str
    service_manifest_file: str = ""
    service_manifest_sha256: str = ""
    trust_anchor_file: str = ""
    artifact_security: ArtifactSecurityPolicy = ArtifactSecurityPolicy()

    @property
    def bootstrap_identities(self) -> list[str]:
        identities: list[str] = []
        for service in self.services:
            identities.extend(service.users)
            identities.extend(provider.identity for provider in service.providers)
        seen = set()
        return [name for name in identities if not (name in seen or seen.add(name))]

    def provider_identity_for_role(self, role: str, service: str = "") -> str:
        for service_policy in self.services:
            if service and service_policy.name != service:
                continue
            for provider in service_policy.providers:
                if role in provider.roles:
                    return provider.identity
        return self.provider_prefix

    def provider_id_for_role(self, role: str, service: str = "") -> str:
        identity = self.provider_identity_for_role(role, service)
        prefix = self.provider_prefix.rstrip("/")
        if identity == prefix:
            return ""
        marker = prefix + "/"
        if identity.startswith(marker):
            return identity[len(marker):]
        return identity.strip("/").replace("/", "-")

    def service_policy(self, service: str) -> ServicePolicy:
        for service_policy in self.services:
            if service_policy.name == service:
                return service_policy
        raise ValueError(f"service {service} is not defined in deployment policy")

    def dependency_graph_for_service(self, service: str) -> DependencyGraph:
        service_policy = self.service_policy(service)
        return DependencyGraph.from_dependencies(
            list(service_policy.roles),
            list(service_policy.dependencies),
        )

    def service_manifest_payload(self) -> bytes:
        return Path(self.service_manifest_file).read_bytes()

    def allow_executable_artifacts(self) -> bool:
        return bool(
            (self.trust_anchor_file or self.artifact_security.anchor_file) and
            self.artifact_security.has_allowlist and
            self.artifact_security.sandbox.configured
        )

    def require_executable_artifacts_allowed(self) -> None:
        missing = []
        if not (self.trust_anchor_file or self.artifact_security.anchor_file):
            missing.append("trust.anchor_file or artifact_security.anchor_file")
        if not self.artifact_security.has_allowlist:
            missing.append("artifact_security.allowlist")
        if not self.artifact_security.sandbox.configured:
            missing.append("artifact_security.sandbox")
        if missing:
            raise RuntimeError(
                "Executable artifacts are disabled. Configure " +
                ", ".join(missing) +
                " before using allow_executables=True.")


def load_config(path: str | Path) -> dict[str, Any]:
    """Load a user-facing policy config from JSON or YAML."""

    config_path = Path(path)
    text = config_path.read_text()
    if config_path.suffix.lower() == ".json":
        return json.loads(text)
    try:
        import yaml  # type: ignore
    except ImportError as exc:
        raise RuntimeError(
            "YAML policy configs require PyYAML; use JSON or install pyyaml"
        ) from exc
    loaded = yaml.safe_load(text)
    if not isinstance(loaded, dict):
        raise ValueError(f"policy config {config_path} must contain a mapping")
    return loaded


def trust_anchor_file(config: dict[str, Any]) -> str:
    trust_config = config.get("trust", {})
    if isinstance(trust_config, dict):
        return str(trust_config.get("anchor_file", ""))
    return ""


def parse_artifact_security(config: dict[str, Any]) -> ArtifactSecurityPolicy:
    raw = config.get("artifact_security", {})
    if not isinstance(raw, dict):
        raw = {}
    raw_sandbox = raw.get("sandbox", {})
    if not isinstance(raw_sandbox, dict):
        raw_sandbox = {}
    return ArtifactSecurityPolicy(
        anchor_file=str(raw.get("anchor_file", "")),
        allowlist=_as_tuple(raw.get("allowlist")),
        sandbox=SandboxPolicy(
            kind=str(raw_sandbox.get("kind", "")),
            image=str(raw_sandbox.get("image", "")),
            command=_as_tuple(raw_sandbox.get("command")),
            workdir=str(raw_sandbox.get("workdir", "")),
        ),
    )


def _as_tuple(value: Any) -> tuple[str, ...]:
    if value is None:
        return ()
    if isinstance(value, str):
        return (value,)
    return tuple(str(item) for item in value)


def _role_permission(service: str, role: str) -> str:
    name = service.rstrip("/") + "/ROLE"
    role = role.strip("/")
    return name if not role else f"{name}/{role}"


def _parse_dependencies(raw: Any) -> tuple[InferenceDependency, ...]:
    dependencies = []
    for item in raw or []:
        if not isinstance(item, dict):
            raise ValueError("service dependencies must be mappings")
        dependencies.append(InferenceDependency(
            producers=list(_as_tuple(item.get("producers"))),
            consumers=list(_as_tuple(item.get("consumers"))),
            key_scope=str(item["key_scope"]),
            topic_prefix=str(item["topic_prefix"]),
            required=bool(item.get("required", True)),
        ))
    return tuple(dependencies)


def _parse_artifacts(raw: Any) -> tuple[ArtifactPolicy, ...]:
    artifacts = []
    for item in raw or []:
        if not isinstance(item, dict):
            raise ValueError("service artifacts must be mappings")
        artifacts.append(ArtifactPolicy(
            role=str(item["role"]),
            path=str(item["path"]),
            artifact_name=str(item.get("artifact", item.get("artifact_name", ""))),
            filename=str(item.get("filename", "")),
            kind=str(item.get("kind", "model")),
            backend=str(item.get("backend", "")),
            metadata=dict(item.get("metadata", {}) or {}),
        ))
    return tuple(artifacts)


def parse_services(config: dict[str, Any]) -> tuple[ServicePolicy, ...]:
    services = []
    for item in config.get("services", []):
        service_roles = _as_tuple(item.get("roles"))
        providers = []
        for provider in item.get("providers", []):
            roles = _as_tuple(provider.get("roles"))
            if len(roles) == 1 and roles[0].lower() == "all":
                if not service_roles:
                    raise ValueError(
                        f"provider {provider['identity']} uses roles=all but "
                        f"service {item['name']} does not define service roles")
                roles = service_roles
            providers.append(ProviderPolicy(
                identity=str(provider["identity"]),
                roles=roles,
            ))
        services.append(ServicePolicy(
            name=str(item["name"]),
            model_name=str(item.get("model", item.get("model_name", ""))),
            roles=service_roles,
            dependencies=_parse_dependencies(item.get("dependencies")),
            users=_as_tuple(item.get("users")),
            providers=tuple(providers),
            artifacts=_parse_artifacts(item.get("artifacts")),
            input_schema=dict(item.get("input", item.get("input_schema", {})) or {}),
            output_schema=dict(item.get("output", item.get("output_schema", {})) or {}),
        ))
    if not services:
        raise ValueError("policy config must define at least one service")
    return tuple(services)


def _first_component_root(name: str) -> str:
    parts = [part for part in name.split("/") if part]
    return "/" + parts[0] if parts else "/"


def app_roots(config: dict[str, Any], services: tuple[ServicePolicy, ...]) -> list[str]:
    explicit = config.get("trust", {}).get("app_roots") if isinstance(config.get("trust"), dict) else None
    if explicit:
        return list(_as_tuple(explicit))
    roots = {
        _first_component_root(str(config.get("controller", ""))),
        _first_component_root(str(config.get("group", ""))),
        _first_component_root(str(config.get("identities", {}).get("user", ""))),
    }
    for service in services:
        for user in service.users:
            roots.add(_first_component_root(user))
        for provider in service.providers:
            roots.add(_first_component_root(provider.identity))
    return sorted(root for root in roots if root and root != "/")


def _name_to_regex_components(name: str) -> str:
    return "".join(f"<{part}>" for part in name.strip("/").split("/") if part)


def _root_key_locator_regex(root_regex: str) -> str:
    return f'"^{root_regex}[^<KEY>]*<KEY><>{{1,3}}$"'


def _hierarchical_checkers(indent: str = "  ") -> str:
    return f"""{indent}checker
{indent}{{
{indent}  type hierarchical
{indent}  sig-type rsa-sha256
{indent}}}
{indent}checker
{indent}{{
{indent}  type hierarchical
{indent}  sig-type ecdsa-sha256
{indent}}}"""


def generate_trust_schema(config: dict[str, Any], services: tuple[ServicePolicy, ...]) -> str:
    roots = app_roots(config, services)
    anchor_file = trust_anchor_file(config)
    blocks = [
        """; Generated by NDNSF-DistributedInference.
; Application developers should edit the high-level policy config instead.

rule
{
  id "NDN certificates"
  for data
  filter
  {
    type name
    regex ^<>+<KEY><><><>$
  }
""" + _hierarchical_checkers() + """
}
"""
    ]
    for root in roots:
        root_regex = _name_to_regex_components(root)
        root_label = root.strip("/") or "root"
        blocks.append(f"""
rule
{{
  id "NDNSF runtime data {root}"
  for data
  filter
  {{
    type name
    regex ^{root_regex}<>*<NDNSF><>*$
  }}
  checker
  {{
    type customized
    sig-type rsa-sha256
    key-locator
    {{
      type name
      regex {_root_key_locator_regex(root_regex)}
    }}
  }}
  checker
  {{
    type customized
    sig-type ecdsa-sha256
    key-locator
    {{
      type name
      regex {_root_key_locator_regex(root_regex)}
    }}
  }}
}}
""")
        blocks.append(f"""
rule
{{
  id "NDN-SVS sync data {root}"
  for data
  filter
  {{
    type name
    regex ^{root_regex}<>*{root_regex}<group><>*$
  }}
  checker
  {{
    type customized
    sig-type rsa-sha256
    key-locator
    {{
      type name
      regex {_root_key_locator_regex(root_regex)}
    }}
  }}
  checker
  {{
    type customized
    sig-type ecdsa-sha256
    key-locator
    {{
      type name
      regex {_root_key_locator_regex(root_regex)}
    }}
  }}
}}
""")
        blocks.append(f"""
rule
{{
  id "NDN-SVS sync interest {root}"
  for interest
  filter
  {{
    type name
    regex ^{root_regex}<group><>*$
  }}
  checker
  {{
    type customized
    sig-type rsa-sha256
    key-locator
    {{
      type name
      regex ^<>*$
    }}
  }}
  checker
  {{
    type customized
    sig-type ecdsa-sha256
    key-locator
    {{
      type name
      regex ^<>*$
    }}
  }}
}}
""")
        blocks.append(f"""
rule
{{
  id "Application data {root}"
  for data
  filter
  {{
    type name
    regex ^{root_regex}<>*$
  }}
{_hierarchical_checkers()}
}}
""")
    if anchor_file:
        blocks.append(f"""
trust-anchor
{{
  type file
  file-name "{anchor_file}"
}}
""")
    else:
        blocks.append("""
; Demo/local bootstrap default for examples that create ephemeral self-signed
; identities. Production deployments must set trust.anchor_file to the trust
; root certificate and issue child certificates under parent namespaces.
trust-anchor
{
  type any
}
""")
    return "\n".join(blocks)


def generate_controller_policy(
    config: dict[str, Any],
    services: tuple[ServicePolicy, ...],
) -> str:
    controller = str(config["controller"]).rstrip("/")
    policy_name = f"{controller}/NDNSF/ControllerPolicy/v1"
    provider_allows: dict[str, list[str]] = {}
    user_allows: dict[str, list[str]] = {}
    for service in services:
        for user in service.users:
            user_allows.setdefault(user, []).append(service.name)
        for provider in service.providers:
            allows = provider_allows.setdefault(provider.identity, [])
            allows.append(service.name)
            for role in provider.roles:
                allows.append(_role_permission(service.name, role))

    def unique(values: list[str]) -> list[str]:
        seen = set()
        return [value for value in values if not (value in seen or seen.add(value))]

    lines = [f"name {policy_name}", "", "provider-policies", "{"]
    for identity, allows in provider_allows.items():
        lines.extend([
            "    provider-policy",
            "    {",
            f"        for {identity}",
            "        allow",
            "        {",
        ])
        lines.extend(f"            {value}" for value in unique(allows))
        lines.extend(["        }", "    }"])
    lines.extend(["}", "", "user-policies", "{"])
    for identity, allows in user_allows.items():
        lines.extend([
            "    user-policy",
            "    {",
            f"        for {identity}",
            "        allow",
            "        {",
        ])
        lines.extend(f"            {value}" for value in unique(allows))
        lines.extend(["        }", "    }"])
    lines.append("}")
    return "\n".join(lines) + "\n"


def service_manifest(services: tuple[ServicePolicy, ...]) -> dict[str, Any]:
    return {
        "services": [
            {
                "name": service.name,
                "model": service.model_name,
                "roles": list(service.roles),
                "dependencies": [
                    {
                        "producers": list(dep.producers),
                        "consumers": list(dep.consumers),
                        "key_scope": dep.key_scope,
                        "topic_prefix": dep.topic_prefix,
                        "required": dep.required,
                    }
                    for dep in service.dependencies
                ],
                "artifacts": [
                    {
                        "role": artifact.role,
                        "path": artifact.path,
                        "artifact": artifact.artifact_name,
                        "filename": artifact.filename,
                        "kind": artifact.kind,
                        "backend": artifact.backend,
                        "metadata": dict(artifact.metadata or {}),
                    }
                    for artifact in service.artifacts
                ],
                "input": dict(service.input_schema or {}),
                "output": dict(service.output_schema or {}),
            }
            for service in services
        ],
    }


def canonical_service_manifest_payload(services: tuple[ServicePolicy, ...]) -> bytes:
    return json.dumps(
        service_manifest(services),
        sort_keys=True,
        separators=(",", ":"),
    ).encode()


def write_service_manifest(
    services: tuple[ServicePolicy, ...],
    output_file: Path,
) -> str:
    payload = canonical_service_manifest_payload(services)
    output_file.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    output_file.with_suffix(output_file.suffix + ".sha256").write_text(digest + "\n")
    return digest


def write_policy_bundle(
    config_path: str | Path,
    output_dir: str | Path,
) -> DistributedInferenceDeployment:
    config = load_config(config_path)
    services = parse_services(config)
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    trust_schema = out / "trust-schema.conf"
    policy_file = out / "controller.policies"
    service_manifest_file = out / "service-manifest.json"
    trust_schema.write_text(generate_trust_schema(config, services))
    policy_file.write_text(generate_controller_policy(config, services))
    manifest_sha256 = write_service_manifest(services, service_manifest_file)
    identities = config.get("identities", {})
    return DistributedInferenceDeployment(
        application=str(config.get("application", "distributed-inference")),
        controller=str(config["controller"]),
        group=str(config.get("group", "/example/hello/group")),
        user=str(identities.get("user", "")),
        provider_prefix=str(identities.get("provider_prefix", "")),
        services=services,
        trust_schema=str(trust_schema),
        policy_file=str(policy_file),
        service_manifest_file=str(service_manifest_file),
        service_manifest_sha256=manifest_sha256,
        trust_anchor_file=trust_anchor_file(config),
        artifact_security=parse_artifact_security(config),
    )


def load_or_generate_deployment(
    config_path: str | Path,
    output_dir: str | Path | None = None,
) -> DistributedInferenceDeployment:
    if output_dir is None:
        output_dir = Path("/tmp") / "ndnsf-distributed-inference-policy"
    return write_policy_bundle(config_path, output_dir)


def main(argv: list[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args(argv)
    deployment = write_policy_bundle(args.config, args.out_dir)
    print("Generated trust schema:", deployment.trust_schema)
    print("Generated controller policy:", deployment.policy_file)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

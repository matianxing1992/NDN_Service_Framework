#!/usr/bin/env python3
"""Build a candidate-local Qwen policy with the current Repo services."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil


QWEN_SERVICE = "/AI/LLM/Pipeline/Fake"
QWEN_ROLES = tuple(f"/LLM/Pipeline/Stage/{index}" for index in range(3))
QWEN_CUT_EDGES = (
    "hidden-layer-20-to-21",
    "hidden-layer-41-to-42",
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _automatic_scope(index: int, producer: str, consumer: str,
                     tensor_edge: str) -> str:
    """Match app-sdk placement's request-scoped tensor key identity.

    The runtime plan derives each scope from the immutable RoleDependency
    tuple.  The provider policy is only a compatibility graph for the Core
    callback, so it must describe the same edge without reintroducing the old
    symbolic ``pipeline-stage-*`` names.
    """
    payload = json.dumps({
        "consumer": consumer,
        "producer": producer,
        "tensor_edges": [tensor_edge],
    }, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    return f"tensor-{index}-{hashlib.sha256(payload).hexdigest()[:16]}"


def _align_qwen_dynamic_dependencies(path: Path) -> None:
    """Align the provider-side fallback graph with deferred Qwen planning."""
    document = __import__("yaml").safe_load(path.read_text(encoding="utf-8"))
    changed = False
    for service in document.get("services", []):
        if str(service.get("name", "")) != QWEN_SERVICE:
            continue
        dependencies = []
        for index, tensor_edge in enumerate(QWEN_CUT_EDGES):
            producer = QWEN_ROLES[index]
            consumer = QWEN_ROLES[index + 1]
            dependencies.append({
                "producers": [producer],
                "consumers": [consumer],
                "key_scope": _automatic_scope(
                    index, producer, consumer, tensor_edge),
                "topic_prefix": "/activation",
                "required": True,
                "tensors": [tensor_edge],
                "object_name_template": "",
            })
        if service.get("dependencies") != dependencies:
            service["dependencies"] = dependencies
            changed = True
    if changed:
        path.write_text(
            __import__("yaml").safe_dump(document, sort_keys=False),
            encoding="utf-8",
        )


def _add_distributed_repo_services(
    path: Path,
    *,
    user: str,
    provider_prefix: str,
    repo_provider_prefix: str | None = None,
) -> None:
    import yaml
    from py_repoclient.service_names import repo_versioned_services

    document = yaml.safe_load(path.read_text(encoding="utf-8"))
    compute_provider_identities = [
        provider_prefix.rstrip("/"),
        f"{provider_prefix.rstrip('/')}/1",
        f"{provider_prefix.rstrip('/')}/2",
    ]
    repo_prefix = (repo_provider_prefix or provider_prefix).rstrip("/")
    repo_provider_identities = [
        repo_prefix,
        f"{repo_prefix}/1",
        f"{repo_prefix}/2",
    ]
    # Compute Providers are Repo clients; Repo Providers may also execute peer
    # repair/catalog operations. Keep both sets authorized while assigning the
    # storage services only to the Repo process identities.
    repo_users = list(dict.fromkeys([
        user, *compute_provider_identities, *repo_provider_identities,
    ]))
    services_by_name = {
        str(service.get("name", "")): service
        for service in document.get("services", [])
    }
    artifact_service = "/NDNSF/DistributedRepo/Artifact/v2/STORE"
    for service_name in repo_versioned_services():
        roles = ["artifact-replica-0"] if service_name == artifact_service else []
        providers = [
            {
                "identity": identity,
                "roles": list(roles),
            }
            for identity in repo_provider_identities
        ]
        service = services_by_name.get(service_name)
        if service is None:
            service = {"name": service_name, "model": service_name}
            document.setdefault("services", []).append(service)
            services_by_name[service_name] = service
        # This is intentionally an idempotent reconciliation, not an
        # append-only migration. Older promoted policies already contain Repo
        # services owned by compute Provider identities; skipping them leaves
        # the dedicated /repo identities absent from Controller bootstrap.
        service.update({
            "users": list(repo_users),
            "providers": providers,
            "roles": list(roles),
            "dependencies": [],
        })
    artifact = services_by_name.get(artifact_service)
    if artifact is None:
        artifact = {"name": artifact_service, "model": artifact_service}
        document.setdefault("services", []).append(artifact)
        services_by_name[artifact_service] = artifact
    # STORE is not listed by every Repo client version. Reconcile it
    # unconditionally so an already-present legacy service cannot retain the
    # compute Provider identities.
    artifact.update({
            "users": list(repo_users),
            "providers": [
                {
                    "identity": identity,
                    "roles": ["artifact-replica-0"],
                }
                for identity in repo_provider_identities
            ],
            "roles": ["artifact-replica-0"],
            "dependencies": [],
        })
    path.write_text(
        yaml.safe_dump(document, sort_keys=False),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--provider-prefix", required=True)
    parser.add_argument("--repo-provider-prefix", required=True)
    args = parser.parse_args()
    if args.output.exists() or args.provenance.exists():
        raise FileExistsError("candidate policy output already exists")

    source_digest = sha256_file(args.input)
    shutil.copyfile(args.input, args.output)
    _add_distributed_repo_services(
        args.output,
        user=args.user,
        provider_prefix=args.provider_prefix,
        repo_provider_prefix=args.repo_provider_prefix,
    )
    _align_qwen_dynamic_dependencies(args.output)
    output_digest = sha256_file(args.output)
    provenance = {
        "schemaVersion": "spec162-candidate-policy-provenance-v1",
        "input": str(args.input),
        "inputSha256": source_digest,
        "output": str(args.output),
        "outputSha256": output_digest,
        "transform": (
            "_add_distributed_repo_services;"
            "_align_qwen_dynamic_dependencies"
        ),
    }
    args.provenance.write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        "SPEC162_CANDIDATE_POLICY_READY",
        f"inputSha256={source_digest}",
        f"outputSha256={output_digest}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

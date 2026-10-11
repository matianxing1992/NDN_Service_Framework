"""llama-server example helpers for NDNSF-DistributedInference.

This example treats ``llama-server`` as a pre-deployed runtime backend.  It is
not tensor-parallel LLM execution: multiple NDNSF providers may advertise the
same LLM service, and NDNSF selects an available provider that proxies the
request to its local OpenAI-compatible llama-server endpoint.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

from ndnsf_distributed_inference.plan import ModelFamily, PlannerKind
from ndnsf_distributed_inference.app_sdk.contracts import ArtifactReference as DIArtifactReference
from ndnsf_distributed_inference.repo_artifact_resolver import binding_from_native_handle
from ndnsf_distributed_inference.splitter import (
    SplitArtifact, SplitServiceSpec, SplitterOutput,
)
from ndnsf_distributed_inference.adapters.llama.runtime import (
    call_openai_chat_runtime,
    decode_openai_chat_response,
    encode_openai_chat_request,
)


SERVICE = "/AI/LLM/Qwen2.5-0.5B/LlamaServer"
ROLE = "/LLM/LlamaServer"
MODEL_NAME = "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf"
MODEL_ARTIFACT_NAME = "/Model/Qwen2.5-0.5B-Instruct-Q4_K_M/GGUF"
RUNTIME_ARTIFACT_NAME = "/Runtime/llama.cpp/llama-server"
DEFAULT_CONTROLLER = "/NDNSF-DistributeInference/example/controller"
DEFAULT_GROUP = "/NDNSF-DistributeInference/example/group"
DEFAULT_USER = "/NDNSF-DistributeInference/example/user"
DEFAULT_PROVIDER_PREFIX = "/NDNSF-DistributeInference/example/provider"


def llama_server_splitter_output(
    *,
    model_path: str = MODEL_NAME,
    llama_server_path: str = "llama-server",
    service: str = SERVICE,
    controller: str = DEFAULT_CONTROLLER,
    group: str = DEFAULT_GROUP,
    user: str = DEFAULT_USER,
    provider_prefix: str = DEFAULT_PROVIDER_PREFIX,
    provider_count: int = 2,
    include_artifacts: bool = True,
) -> SplitterOutput:
    """Return a pre-deployed llama-server service policy.

    ``model_path`` and ``llama_server_path`` are metadata/artifact paths for
    review and future repo-backed deployment.  Providers in this first example
    are expected to have the executable and GGUF model already installed.
    """

    service_spec = SplitServiceSpec(
        name=service,
        model_name=model_path,
        roles=[ROLE],
        dependencies=[],
        input_schema={
            "codec": "openai-chat-json",
            "endpoint": "/v1/chat/completions",
            "stream": False,
        },
        output_schema={
            "codec": "openai-chat-json",
        },
        artifacts=[
            SplitArtifact(
                role=ROLE,
                path=model_path,
                artifact_name=MODEL_ARTIFACT_NAME,
                filename=Path(model_path).name or MODEL_NAME,
                kind="model",
                backend="llama.cpp",
                metadata={
                    "modelFamily": ModelFamily.LLM.value,
                    "modelFormat": "gguf",
                    "runtimeBackend": "llama.cpp",
                },
            )
        ] if include_artifacts else [],
        metadata={
            "model_family": ModelFamily.LLM.value,
            "model_format": "gguf",
            "runtime_backend": "llama.cpp",
            "planner_kind": PlannerKind.LLM_PIPELINE.value,
            "execution_plan_schema_version": 2,
            "llmServingMode": "replicated-provider-serving",
            "planner": {
                "modelFamily": ModelFamily.LLM.value,
                "modelFormat": "gguf",
                "runtimeBackend": "llama.cpp",
                "plannerKind": PlannerKind.LLM_PIPELINE.value,
                "schemaVersion": 2,
                "scoreSummary": {
                    "roleCount": 1,
                    "dependencyCount": 0,
                    "executionImplemented": True,
                    "modelParallel": False,
                },
                "selectedCandidate": {
                    "mode": "replicated-provider-serving",
                    "selected": True,
                },
            },
            "runtime": {
                "backend": "llama.cpp",
                "entrypoint": "llama-server",
                "artifact": RUNTIME_ARTIFACT_NAME,
                "path": llama_server_path,
                "kind": "runtime",
                "executable": True,
                "deploymentScope": "session",
            },
            "modelArtifact": {
                "artifact": MODEL_ARTIFACT_NAME,
                "path": model_path,
                "format": "gguf",
                "deploymentScope": "session",
            },
        },
    )
    providers = [
        provider_prefix if index == 0 else f"{provider_prefix}/{index}"
        for index in range(max(1, int(provider_count)))
    ]
    return SplitterOutput(
        application="llama-server-qwen-demo",
        controller=controller,
        group=group,
        user=user,
        provider_prefix=provider_prefix,
        services=[service_spec],
        provider_identities=providers,
        trust_app_roots=["/example"],
        metadata=dict(service_spec.metadata),
    )


def encode_chat_request(
    prompt: str,
    *,
    model: str = "qwen2.5-0.5b",
    system: str = "",
    max_tokens: int = 64,
    temperature: float = 0.2,
) -> bytes:
    return encode_openai_chat_request(
        prompt,
        model=model,
        system=system,
        max_tokens=max_tokens,
        temperature=temperature,
    )


def decode_chat_response(payload: bytes) -> dict[str, Any]:
    return decode_openai_chat_response(payload)


def call_llama_server_chat(
    payload: bytes,
    *,
    base_url: str = "http://127.0.0.1:8080",
    timeout_s: float = 60.0,
) -> bytes:
    """POST an OpenAI-compatible chat-completion request to llama-server."""

    return call_openai_chat_runtime(
        payload,
        base_url=base_url,
        timeout_s=timeout_s,
    )


def write_policy(path: str | Path, **kwargs: Any) -> Path:
    output = llama_server_splitter_output(**kwargs)
    policy = Path(path)
    output.write_policy_config(policy)
    return policy


def build_llama_server_artifact_references(
    repo,
    *,
    model_path: str | Path,
    llama_server_path: str | Path,
    role: str = ROLE,
    object_prefix: str = "NDNSF-DI/ARTIFACT/AI/LLM/Qwen2.5-0.5B",
    replication_factor: int = 1,
    include_local_payload_paths: bool = False,
    publisher_identity: str = "",
) -> dict[str, Any]:
    """Publish native Repo bindings or return explicit local DI references."""

    model_file = Path(model_path)
    runtime_file = Path(llama_server_path)
    if repo is None or include_local_payload_paths:
        model_entry = _local_artifact_entry(
            model_file, kind="model", executable=False,
            metadata={
                "modelFamily": ModelFamily.LLM.value,
                "modelFormat": "gguf",
                "runtimeBackend": "llama.cpp",
            },
        )
        runtime_entry = _local_artifact_entry(
            runtime_file, kind="runtime", executable=True,
            metadata={"runtimeBackend": "llama.cpp", "entrypoint": "llama-server"},
        )
    else:
        model_entry = _publish_repo_artifact(
            repo,
            model_file,
            logical_name=f"/{object_prefix.strip('/')}/model/{_sha256_file(model_file)}",
            publisher_identity=publisher_identity,
            role=role,
            use="model",
            kind="model",
            executable=False,
            metadata={
                "modelFamily": ModelFamily.LLM.value,
                "modelFormat": "gguf",
                "runtimeBackend": "llama.cpp",
            },
            replication_factor=replication_factor,
        )
        runtime_entry = _publish_repo_artifact(
            repo,
            runtime_file,
            logical_name=(
                f"/{object_prefix.strip('/')}/runtime/llama-server/"
                f"{_sha256_file(runtime_file)}"
            ),
            publisher_identity=publisher_identity,
            role=role,
            use="runtime",
            kind="runtime",
            executable=True,
            metadata={"runtimeBackend": "llama.cpp", "entrypoint": "llama-server"},
            replication_factor=replication_factor,
        )
    return {
        "schemaVersion": 1,
        "roles": {
            role: {
                "model": model_entry,
                "runner": runtime_entry,
            },
        },
    }


def write_artifact_references(path: str | Path, references: dict[str, Any]) -> Path:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(references, indent=2, sort_keys=True), encoding="utf-8")
    return target


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _local_artifact_entry(
    path: Path, *, kind: str, executable: bool, metadata: dict,
) -> dict:
    digest = _sha256_file(path)
    reference = DIArtifactReference(
        uri=f"urn:ndnsf:local-artifact:sha256:{digest}",
        digest="sha256:" + digest,
        size_bytes=path.stat().st_size,
        mount_path=path.name,
    )
    return {
        "artifactReference": {
            "uri": reference.uri,
            "digest": reference.digest,
            "size_bytes": reference.size_bytes,
            "mount_path": reference.mount_path,
        },
        "localPayloadPath": str(path),
        "filename": path.name,
        "kind": kind,
        "executable": executable,
        "metadata": dict(metadata),
    }


def _publish_repo_artifact(
    repo_client,
    path: Path,
    *,
    logical_name: str,
    publisher_identity: str,
    role: str,
    use: str,
    kind: str,
    executable: bool,
    metadata: dict,
    replication_factor: int,
) -> dict:
    from py_repoclient import (
        ArtifactCapabilityRequirements,
        ArtifactReference,
        RepoWriteConsistency,
        SelectedRepoSet,
        SelectedRepoTarget,
    )

    if not publisher_identity.startswith("/"):
        raise ValueError("native Repo publication requires publisher identity")
    if replication_factor <= 0:
        raise ValueError("replication_factor must be positive")
    payload_digest = _sha256_file(path)
    reference = ArtifactReference(
        logical_name=logical_name,
        content_digest=payload_digest,
        size_bytes=path.stat().st_size,
        root_manifest_name=logical_name + "/root",
        publisher_identity=publisher_identity,
        policy_epoch="/Policy/llama-server-qwen/v1",
    )
    api = repo_client.artifact_api
    requirements = ArtifactCapabilityRequirements()
    requirements.artifact = reference
    candidates = api.discover_repo_candidates(requirements, 60000).candidates
    if len(candidates) < replication_factor:
        raise RuntimeError(
            f"Repo discovery returned {len(candidates)} candidates; "
            f"need {replication_factor}"
        )
    selected = SelectedRepoSet()
    selected.targets = []
    for candidate in candidates[:replication_factor]:
        target = SelectedRepoTarget()
        target.repo_node = candidate.repo_node
        target.provider_identity = candidate.provider_identity
        target.service_prefix = candidate.service_prefix
        selected.targets.append(target)
    selected.seed_repo_id = selected.targets[0].repo_node
    selected.consistency = RepoWriteConsistency.ALL
    result = repo_client.publish_file(path, reference, selected, timeout_ms=60000)
    if not result.success or result.handle is None:
        raise RuntimeError(f"native Repo publish failed for {logical_name}")
    uri = "urn:ndnsf:repo-artifact:" + hashlib.sha256(
        result.handle.to_json().encode("utf-8")).hexdigest()
    projection = DIArtifactReference(
        uri=uri,
        digest="sha256:" + payload_digest,
        size_bytes=path.stat().st_size,
        mount_path=path.name,
    )
    binding = binding_from_native_handle(
        result.handle, projection, role=role, use=use)
    return {
        "repoBinding": binding.to_dict(),
        "filename": path.name,
        "kind": kind,
        "executable": executable,
        "metadata": {**metadata, "role": role, "use": use},
    }

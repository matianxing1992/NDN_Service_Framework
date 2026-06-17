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

from ndnsf_distributed_inference import (
    ModelFamily,
    PlannerKind,
    SplitArtifact,
    SplitServiceSpec,
    SplitterOutput,
    repo_artifact_reference,
)
from ndnsf_distributed_inference.llm_runtime import (
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
) -> dict[str, Any]:
    """Store llama-server runtime/model artifacts and return DI references.

    ``repo`` may be a real ``NetworkDistributedRepoClient`` or a smoke-test
    ``LocalDistributedRepo``. The returned structure is intentionally the same
    ``roles -> role -> {model, runner}`` artifact-reference shape already used
    by DI dynamic provisioning.
    """

    model_file = Path(model_path)
    runtime_file = Path(llama_server_path)
    model_payload = model_file.read_bytes()
    runtime_payload = runtime_file.read_bytes()
    model_digest = hashlib.sha256(model_payload).hexdigest()[:16]
    runtime_digest = hashlib.sha256(runtime_payload).hexdigest()[:16]
    model_manifest = _store_repo_artifact(
        repo,
        object_suffix=f"{object_prefix}/model/{model_digest}",
        payload=model_payload,
        object_type="model-artifact",
        replication_factor=replication_factor,
    )
    runtime_manifest = _store_repo_artifact(
        repo,
        object_suffix=f"{object_prefix}/runtime/llama-server/{runtime_digest}",
        payload=runtime_payload,
        object_type="runtime-executable",
        replication_factor=replication_factor,
    )
    model_entry = repo_artifact_reference(
        model_manifest,
        object_type="model-artifact",
        object_id=MODEL_ARTIFACT_NAME,
    )
    model_entry.update({
        "filename": model_file.name or MODEL_NAME,
        "kind": "model",
        "executable": False,
        "metadata": {
            "modelFamily": ModelFamily.LLM.value,
            "modelFormat": "gguf",
            "runtimeBackend": "llama.cpp",
            "filename": model_file.name or MODEL_NAME,
        },
    })
    runtime_entry = repo_artifact_reference(
        runtime_manifest,
        object_type="runtime-executable",
        object_id=RUNTIME_ARTIFACT_NAME,
    )
    runtime_entry.update({
        "filename": runtime_file.name or "llama-server",
        "kind": "runtime",
        "executable": True,
        "metadata": {
            "runtimeBackend": "llama.cpp",
            "entrypoint": "llama-server",
            "filename": runtime_file.name or "llama-server",
        },
    })
    if include_local_payload_paths:
        model_entry["localPayloadPath"] = str(model_file)
        runtime_entry["localPayloadPath"] = str(runtime_file)
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


def _store_repo_artifact(
    repo,
    *,
    object_suffix: str,
    payload: bytes,
    object_type: str,
    replication_factor: int,
):
    if hasattr(repo, "publisher_object_name") and hasattr(repo, "store_object"):
        object_name = repo.publisher_object_name(object_suffix)
        return repo.store_object(
            object_name=object_name,
            payload=payload,
            object_type=object_type,
            replication_factor=replication_factor,
            policy_epoch="/Policy/llama-server-qwen/v1",
        )
    object_name = "/" + object_suffix.strip("/")
    if hasattr(repo, "put"):
        return repo.put(
            object_name=object_name,
            payload=payload,
            object_type=object_type,
        )
    raise TypeError("repo must provide store_object(...) or put(...)")

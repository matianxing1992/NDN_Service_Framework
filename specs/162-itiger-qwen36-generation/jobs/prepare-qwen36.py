#!/usr/bin/env python3
"""Prepare frozen Qwen3.6 references and three RTX-validated stage packages."""

from __future__ import annotations

import argparse
import gc
import hashlib
import json
import os
from pathlib import Path
import time
from typing import Any, Iterable


MODEL_REPOSITORY = "Qwen/Qwen3.6-27B"
MODEL_REVISION = "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9"
MODEL_TYPE = "qwen3_5"
EXPECTED_LAYER_COUNT = 64
STAGE_COUNT = 3
STAGE_RANGES = ((0, 21), (21, 42), (42, 64))
MODEL_PROFILE = "qwen36-27b"
REFERENCE_DEVICE_MAP = "stage-ranges"
MODEL_PROFILES = {
    "qwen36-27b": {
        "repository": "Qwen/Qwen3.6-27B",
        "revision": "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        "modelType": "qwen3_5",
        "layerCount": 64,
        # The reference model must execute the same layer boundaries as the
        # exported pipeline stages.  ``balanced`` is capacity-aware but may
        # choose a different boundary (as v94 did: 0-18/18-42/42-64), which
        # makes an exact token comparison compare different computations.
        "referenceDeviceMap": "stage-ranges",
    },
    "qwen3-0.6b": {
        "repository": "Qwen/Qwen3-0.6B",
        "revision": "e6de91484c29aa9480d55605af694f39b081c455",
        "modelType": "qwen3",
        "layerCount": 28,
        "referenceDeviceMap": "cuda:0",
    },
}
MAX_NEW_TOKENS = 64
DTYPE = "bfloat16"
PROMPT_SCHEMA = "ndnsf-di-qwen-prompt-set-v1"
MIN_GPU_HEADROOM_BYTES = 1024 ** 3


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_sha256(value: Any) -> str:
    payload = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def tree_manifest(
    root: Path,
    *,
    excluded_names: Iterable[str] = (),
) -> list[dict[str, Any]]:
    excluded = frozenset(excluded_names)
    rows: list[dict[str, Any]] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or any(part in excluded for part in path.parts):
            continue
        rows.append({
            "path": path.relative_to(root).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        })
    if not rows:
        raise RuntimeError(f"manifest root has no files: {root}")
    return rows


def normalize_eos_ids(value: Any) -> list[int]:
    values = value if isinstance(value, (list, tuple)) else [value]
    result = sorted({int(item) for item in values if item is not None})
    if not result or any(item < 0 for item in result):
        raise RuntimeError(f"invalid EOS token IDs: {value!r}")
    return result


def validate_reference_gpus(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    if len(rows) != 3:
        raise RuntimeError(
            f"reference requires exactly three visible GPUs, found {len(rows)}")
    indices = [int(row["index"]) for row in rows]
    if indices != [0, 1, 2]:
        raise RuntimeError(f"reference GPU indices must be 0,1,2: {indices}")
    for row in rows:
        if "RTX 5000" not in str(row["name"]).upper():
            raise RuntimeError(f"reference GPU is not RTX 5000: {row['name']}")
        if int(row["totalBytes"]) <= MIN_GPU_HEADROOM_BYTES:
            raise RuntimeError(f"reference GPU capacity invalid: {row}")
    return rows


def validate_stage_acceptance(
    rows: list[dict[str, Any]], *, validation_input_token_count: int,
) -> None:
    """Reject stage packages that were not exercised at campaign scale.

    A short smoke tensor is not an acceptable CUDA admission test: Qwen3.6's
    hybrid attention/linear-attention path can have a materially different
    workspace footprint at the longest registered prompt.  The preparation
    job therefore records and checks the exact maximum prompt length used by
    the campaign.
    """
    if validation_input_token_count <= 0:
        raise RuntimeError("validation input token count must be positive")
    if len(rows) != STAGE_COUNT:
        raise RuntimeError(f"expected three stage rows, found {len(rows)}")
    for index, (row, expected_range) in enumerate(zip(rows, STAGE_RANGES)):
        observed = (
            int(row["layerRange"]["start"]),
            int(row["layerRange"]["endExclusive"]),
        )
        if int(row["stageIndex"]) != index or observed != expected_range:
            raise RuntimeError(
                f"stage {index} range mismatch: {observed} != {expected_range}")
        if not row.get("cudaValidated") or not row.get("forwardValidated"):
            raise RuntimeError(f"stage {index} CUDA/forward validation missing")
        if row.get("cpuFallback") is not False:
            raise RuntimeError(f"stage {index} CPU fallback is not disabled")
        if int(row.get("validationInputTokenCount", 0)) != validation_input_token_count:
            raise RuntimeError(
                f"stage {index} was validated with {row.get('validationInputTokenCount')} "
                f"tokens, expected {validation_input_token_count}")
        total = int(row["gpuTotalBytes"])
        peak_allocated = int(row["peakAllocatedBytes"])
        peak_reserved = int(row["peakReservedBytes"])
        if min(total, peak_allocated, peak_reserved) < 0:
            raise RuntimeError(f"stage {index} invalid CUDA memory metrics")
        if peak_reserved > total - MIN_GPU_HEADROOM_BYTES:
            raise RuntimeError(
                f"stage {index} has less than one GiB CUDA headroom")


def _visible_gpu_rows(torch: Any) -> list[dict[str, Any]]:
    return [
        {
            "index": index,
            "name": torch.cuda.get_device_name(index),
            "uuid": str(torch.cuda.get_device_properties(index).uuid),
            "totalBytes": int(
                torch.cuda.get_device_properties(index).total_memory),
        }
        for index in range(torch.cuda.device_count())
    ]


def _assert_reference_device_map(
    model: Any, *, expected_devices: set[int] | None = None,
) -> dict[str, int]:
    mapping = dict(getattr(model, "hf_device_map", {}))
    normalized: dict[str, int] = {}
    if mapping:
        for key, value in mapping.items():
            if isinstance(value, str) and value.startswith("cuda:"):
                device = int(value.split(":", 1)[1])
            elif isinstance(value, int):
                device = value
            elif (
                getattr(value, "type", None) == "cuda"
                and getattr(value, "index", None) is not None
            ):
                device = int(value.index)
            else:
                raise RuntimeError(
                    "reference device map contains CPU/disk target: "
                    f"{key}={value}")
            if device not in (0, 1, 2):
                raise RuntimeError(
                    f"reference device map target invalid: {key}={value}")
            normalized[str(key)] = device
    else:
        actual_devices: set[int] = set()
        tensors = list(model.parameters()) + list(model.buffers())
        if not tensors:
            raise RuntimeError("reference model has no parameters or buffers")
        for tensor in tensors:
            device = getattr(tensor, "device", None)
            if (
                getattr(device, "type", None) != "cuda"
                or getattr(device, "index", None) is None
            ):
                raise RuntimeError(
                    "reference model contains a non-CUDA parameter or buffer: "
                    f"{device}")
            actual_devices.add(int(device.index))
        normalized = {
            f"<actual-model-device:{device}>": device
            for device in sorted(actual_devices)
        }
    required = {0, 1, 2} if expected_devices is None else set(expected_devices)
    if set(normalized.values()) != required:
        raise RuntimeError(
            "reference model device map mismatch: "
            f"expected={sorted(required)} observed={normalized}")
    return normalized


def _stage_reference_device_map(
    layer_ranges: tuple[tuple[int, int], ...],
) -> dict[str, int]:
    """Build an explicit full-model map matching exported stage boundaries."""
    if len(layer_ranges) != STAGE_COUNT:
        raise RuntimeError(
            f"reference stage map requires {STAGE_COUNT} ranges, "
            f"found {len(layer_ranges)}")
    mapping: dict[str, int] = {
        "model.embed_tokens": 0,
        "model.rotary_emb": STAGE_COUNT - 1,
        "model.norm": STAGE_COUNT - 1,
        "lm_head": STAGE_COUNT - 1,
    }
    for stage_index, (start, end) in enumerate(layer_ranges):
        if start < 0 or end <= start:
            raise RuntimeError(
                f"invalid reference stage range {stage_index}: {(start, end)}")
        for layer_index in range(start, end):
            mapping[f"model.layers.{layer_index}"] = stage_index
    return mapping


def _assert_reference_layer_ranges(
    device_map: dict[str, int],
    expected_ranges: tuple[tuple[int, int], ...],
) -> None:
    """Reject a reference placement whose layer cuts differ from stage files."""
    expected: dict[int, int] = {}
    for stage_index, (start, end) in enumerate(expected_ranges):
        for layer_index in range(start, end):
            expected[layer_index] = stage_index
    observed: dict[int, int] = {}
    for key, device in device_map.items():
        if key.startswith("model.layers."):
            suffix = key[len("model.layers."):]
            if suffix.isdigit():
                observed[int(suffix)] = int(device)
    if observed != expected:
        raise RuntimeError(
            "reference layer ranges differ from exported stage ranges: "
            f"expected={sorted(expected.items())} "
            f"observed={sorted(observed.items())}")


def _input_device(model: Any) -> str:
    mapping = dict(getattr(model, "hf_device_map", {}))
    for key in ("model.embed_tokens", "model.language_model.embed_tokens"):
        if key in mapping:
            value = mapping[key]
            return f"cuda:{value}" if isinstance(value, int) else str(value)
    return "cuda:0"


def _memory_snapshot(torch: Any, index: int) -> dict[str, int]:
    return {
        "peakAllocatedBytes": int(torch.cuda.max_memory_allocated(index)),
        "peakReservedBytes": int(torch.cuda.max_memory_reserved(index)),
        "gpuTotalBytes": int(
            torch.cuda.get_device_properties(index).total_memory),
    }


def _build_policy(
    *,
    output_path: Path,
    stage_paths: list[Path],
    promoted_artifact_dir: Path,
    model_digest: str,
) -> None:
    from ndnsf_distributed_inference.plan import PlannerKind
    from ndnsf_distributed_inference.splitter import (
        SplitArtifact,
        SplitServiceSpec,
        SplitterOutput,
    )
    from ndnsf_distributed_inference.llm_stub_planner import (
        llm_planner_registry,
        llm_planner_request,
        llm_splitter_output_from_result,
    )
    from llm_pipeline_lib import (
        DEFAULT_CONTROLLER,
        DEFAULT_GROUP,
        DEFAULT_PROVIDER_PREFIX,
        DEFAULT_USER,
        MODEL_NAME,
        QWEN_TRANSFORMERS_RUNTIME,
        SERVICE,
        _pin_stage_providers,
        qwen_transformer_stage_spec,
    )

    request = llm_planner_request(
        planner_kind=PlannerKind.LLM_PIPELINE,
        model_path=MODEL_NAME,
        output_dir=output_path.parent,
        model_format="custom",
        runtime_backend="custom",
        service=SERVICE,
        stages=STAGE_COUNT,
        layers=EXPECTED_LAYER_COUNT,
    )
    result = llm_planner_registry().plan(request)
    splitter = llm_splitter_output_from_result(
        result,
        application=f"llm-pipeline-{MODEL_PROFILE}-spec162",
        controller=DEFAULT_CONTROLLER,
        group=DEFAULT_GROUP,
        user=DEFAULT_USER,
        provider_prefix=DEFAULT_PROVIDER_PREFIX,
    )
    services: list[SplitServiceSpec] = []
    for service in splitter.services:
        if service.name != SERVICE:
            services.append(service)
            continue
        artifacts: list[SplitArtifact] = []
        for role, scratch_path in zip(service.roles, stage_paths):
            spec = qwen_transformer_stage_spec(
                role=role,
                stages=STAGE_COUNT,
                layer_count=EXPECTED_LAYER_COUNT,
                model_name=MODEL_REPOSITORY,
            )
            promoted_path = (
                promoted_artifact_dir
                / "qwen-transformers-stage-artifacts"
                / scratch_path.name
            )
            artifacts.append(SplitArtifact(
                role=role,
                path=str(promoted_path),
                artifact_name=(
                    f"/Model/LLM/Pipeline/{MODEL_PROFILE}/{role.strip('/')}"
                ),
                filename=scratch_path.name,
                kind="llm-stage-weights",
                backend="transformers",
                metadata={
                    "runtime": QWEN_TRANSFORMERS_RUNTIME,
                    "stageIndex": spec["stageIndex"],
                    "stageCount": spec["stageCount"],
                    "layerCount": spec["layerCount"],
                    "layerRange": dict(spec["layerRange"]),
                    "modelFamily": "llm",
                    "runtimeBackend": "transformers",
                    "modelRevision": MODEL_REVISION,
                    "modelDigest": model_digest,
                    "dtype": DTYPE,
                    "cpuFallbackAllowed": False,
                },
            ))
        services.append(SplitServiceSpec(
            name=service.name,
            model_name=MODEL_REPOSITORY,
            roles=list(service.roles),
            dependencies=list(service.dependencies),
            artifacts=artifacts,
            input_schema=dict(service.input_schema),
            output_schema=dict(service.output_schema),
            users=list(service.users),
            providers=list(service.providers),
            metadata={
                **dict(service.metadata),
                "execution_implemented": True,
                "runtime": QWEN_TRANSFORMERS_RUNTIME,
                "model": MODEL_REPOSITORY,
                "modelRevision": MODEL_REVISION,
                "modelDigest": model_digest,
                "dtype": DTYPE,
                "cpuFallbackAllowed": False,
            },
        ))
    output = SplitterOutput(
        application=splitter.application,
        controller=splitter.controller,
        group=splitter.group,
        user=splitter.user,
        provider_prefix=splitter.provider_prefix,
        services=services,
        provider_identities=list(splitter.provider_identities),
        trust_app_roots=list(splitter.trust_app_roots),
        trust_anchor_file=splitter.trust_anchor_file,
        artifact_allowlist=list(splitter.artifact_allowlist),
        artifact_sandbox=dict(splitter.artifact_sandbox),
        metadata=dict(splitter.metadata),
    )
    output.write_policy_config(output_path)
    _pin_stage_providers(
        output_path,
        service=SERVICE,
        provider_prefix=DEFAULT_PROVIDER_PREFIX,
        stages=STAGE_COUNT,
    )
    _add_distributed_repo_services(
        output_path,
        user=DEFAULT_USER,
        provider_prefix=DEFAULT_PROVIDER_PREFIX,
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
    repo_providers = [
        {"identity": identity, "roles": []}
        for identity in repo_provider_identities
    ]
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
    parser.add_argument("--work-root", required=True)
    parser.add_argument("--prompt-set", required=True)
    parser.add_argument("--promoted-artifact-dir", required=True)
    parser.add_argument("--submission-id", required=True)
    parser.add_argument("--capacity-decision-sha256", required=True)
    parser.add_argument("--runtime-sif-sha256", required=True)
    parser.add_argument("--source-bundle-sha256", required=True)
    parser.add_argument(
        "--model-profile",
        choices=sorted(MODEL_PROFILES),
        default="qwen36-27b",
    )
    args = parser.parse_args()

    global MODEL_PROFILE, MODEL_REPOSITORY, MODEL_REVISION, MODEL_TYPE
    global EXPECTED_LAYER_COUNT, STAGE_RANGES, REFERENCE_DEVICE_MAP
    MODEL_PROFILE = args.model_profile
    profile = MODEL_PROFILES[MODEL_PROFILE]
    MODEL_REPOSITORY = str(profile["repository"])
    MODEL_REVISION = str(profile["revision"])
    MODEL_TYPE = str(profile["modelType"])
    EXPECTED_LAYER_COUNT = int(profile["layerCount"])
    REFERENCE_DEVICE_MAP = str(profile["referenceDeviceMap"])
    base_layers = EXPECTED_LAYER_COUNT // STAGE_COUNT
    STAGE_RANGES = tuple(
        (index * base_layers,
         EXPECTED_LAYER_COUNT if index == STAGE_COUNT - 1
         else (index + 1) * base_layers)
        for index in range(STAGE_COUNT)
    )

    work_root = Path(args.work_root).resolve()
    output_root = work_root / "output"
    stage_root = output_root / "qwen-transformers-stage-artifacts"
    tokenizer_root = output_root / "tokenizer"
    prompt_set_path = Path(args.prompt_set).resolve()
    promoted_artifact_dir = Path(args.promoted_artifact_dir)
    output_root.mkdir(parents=True, exist_ok=False)
    stage_root.mkdir()
    tokenizer_root.mkdir()

    prompt_set = json.loads(prompt_set_path.read_text(encoding="utf-8"))
    if prompt_set.get("schemaVersion") != PROMPT_SCHEMA:
        raise RuntimeError("prompt set schema mismatch")
    prompts = list(prompt_set.get("prompts", []))
    if len(prompts) != 5:
        raise RuntimeError(f"expected five prompts, found {len(prompts)}")
    prompt_ids = [str(item.get("promptId", "")) for item in prompts]
    if any(not item for item in prompt_ids) or len(set(prompt_ids)) != 5:
        raise RuntimeError("prompt IDs must be non-empty and unique")
    prompt_set_sha = sha256_file(prompt_set_path)

    import torch
    from importlib.metadata import version as package_version
    from huggingface_hub import snapshot_download
    from transformers import AutoTokenizer
    from llm_pipeline_lib import (
        _safe_torch_save,
        _stage_state_dict,
        format_qwen_chat_prompt,
        qwen_transformer_model_from_stage_package,
        qwen_transformer_stage_spec,
        run_qwen_transformer_stage,
        split_layer_ranges,
    )

    if not torch.cuda.is_available():
        raise RuntimeError("RTX 5000 CUDA devices are unavailable")
    gpu_rows = validate_reference_gpus(_visible_gpu_rows(torch))
    for index in range(3):
        torch.cuda.reset_peak_memory_stats(index)

    cache_root = work_root / "huggingface-cache"
    print("SPEC162_DOWNLOAD_BEGIN", MODEL_REPOSITORY, MODEL_REVISION, flush=True)
    snapshot = Path(snapshot_download(
        repo_id=MODEL_REPOSITORY,
        revision=MODEL_REVISION,
        cache_dir=cache_root,
    )).resolve()
    if snapshot.name != MODEL_REVISION:
        raise RuntimeError(
            f"snapshot revision mismatch: {snapshot.name} != {MODEL_REVISION}")
    print("SPEC162_DOWNLOAD_COMPLETE", snapshot, flush=True)

    source_rows = tree_manifest(snapshot)
    source_manifest = {
        "schemaVersion": "ndnsf-di-qwen36-source-manifest-v1",
        "repository": MODEL_REPOSITORY,
        "revision": MODEL_REVISION,
        "files": source_rows,
        "totalBytes": sum(row["bytes"] for row in source_rows),
    }
    model_digest = canonical_sha256(source_manifest)
    source_manifest["modelDigest"] = f"sha256:{model_digest}"
    write_json(output_root / "model-source-manifest.json", source_manifest)

    tokenizer = AutoTokenizer.from_pretrained(
        snapshot,
        local_files_only=True,
        trust_remote_code=False,
    )
    if not tokenizer.chat_template:
        raise RuntimeError("frozen tokenizer has no chat template")
    tokenizer.save_pretrained(tokenizer_root)
    tokenizer_rows = tree_manifest(tokenizer_root)
    tokenizer_digest = canonical_sha256(tokenizer_rows)
    chat_template_digest = hashlib.sha256(
        tokenizer.chat_template.encode("utf-8")).hexdigest()

    print("SPEC162_REFERENCE_MODEL_LOAD_BEGIN", flush=True)
    if MODEL_TYPE == "qwen3_5":
        from transformers.models.qwen3_5.configuration_qwen3_5 import (
            Qwen3_5TextConfig as ModelConfig,
        )
        from transformers.models.qwen3_5.modeling_qwen3_5 import (
            Qwen3_5ForCausalLM as ModelForCausalLM,
        )
        expected_runtime_model_type = "qwen3_5_text"
    elif MODEL_TYPE == "qwen3":
        from transformers.models.qwen3.configuration_qwen3 import (
            Qwen3Config as ModelConfig,
        )
        from transformers.models.qwen3.modeling_qwen3 import (
            Qwen3ForCausalLM as ModelForCausalLM,
        )
        expected_runtime_model_type = "qwen3"
    else:
        raise RuntimeError(f"unsupported frozen model type: {MODEL_TYPE}")
    text_config = ModelConfig.from_pretrained(
        snapshot,
        local_files_only=True,
        trust_remote_code=False,
    )
    reference_device_map: Any
    reference_max_memory: dict[int, str] | None
    if REFERENCE_DEVICE_MAP == "stage-ranges":
        reference_device_map = _stage_reference_device_map(STAGE_RANGES)
        reference_max_memory = {index: "30GiB" for index in range(3)}
        expected_reference_devices = {0, 1, 2}
    elif REFERENCE_DEVICE_MAP == "balanced":
        reference_device_map = "balanced"
        reference_max_memory = {index: "30GiB" for index in range(3)}
        expected_reference_devices = {0, 1, 2}
    elif REFERENCE_DEVICE_MAP == "cuda:0":
        reference_device_map = {"": 0}
        reference_max_memory = {0: "30GiB"}
        expected_reference_devices = {0}
    else:
        raise RuntimeError(
            f"unsupported reference device map: {REFERENCE_DEVICE_MAP}")
    model = ModelForCausalLM.from_pretrained(
        snapshot,
        config=text_config,
        local_files_only=True,
        trust_remote_code=False,
        dtype=torch.bfloat16,
        attn_implementation="sdpa",
        device_map=reference_device_map,
        max_memory=reference_max_memory,
        low_cpu_mem_usage=True,
    )
    model.eval()
    if str(getattr(model.config, "model_type", "")) != expected_runtime_model_type:
        raise RuntimeError(
            f"unexpected model type: {getattr(model.config, 'model_type', '')}")
    if int(model.config.num_hidden_layers) != EXPECTED_LAYER_COUNT:
        raise RuntimeError(
            f"expected {EXPECTED_LAYER_COUNT} layers, found "
            f"{model.config.num_hidden_layers}")
    device_map = _assert_reference_device_map(
        model, expected_devices=expected_reference_devices)
    if REFERENCE_DEVICE_MAP == "stage-ranges":
        _assert_reference_layer_ranges(device_map, STAGE_RANGES)
    input_device = _input_device(model)
    print("SPEC162_REFERENCE_MODEL_LOAD_COMPLETE", device_map, flush=True)

    eos_ids = normalize_eos_ids(model.generation_config.eos_token_id)
    reference_cases: list[dict[str, Any]] = []
    for prompt in prompts:
        prompt_id = str(prompt["promptId"])
        text = str(prompt["text"])
        formatted_ids = format_qwen_chat_prompt(tokenizer, text)
        if not formatted_ids or len(formatted_ids) > 512:
            raise RuntimeError(
                f"prompt {prompt_id} input tokens invalid: {len(formatted_ids)}")
        context = torch.tensor(
            [formatted_ids], dtype=torch.long, device=input_device)
        generated: list[int] = []
        token_ms: list[float] = []
        for _ in range(MAX_NEW_TOKENS):
            started = time.perf_counter()
            with torch.inference_mode():
                output = model(
                    input_ids=context,
                    attention_mask=torch.ones_like(context),
                    use_cache=False,
                )
                token = int(torch.argmax(
                    output.logits[:, -1, :], dim=-1).item())
            for index in range(3):
                torch.cuda.synchronize(index)
            token_ms.append((time.perf_counter() - started) * 1000.0)
            generated.append(token)
            context = torch.cat([
                context,
                torch.tensor([[token]], dtype=torch.long, device=input_device),
            ], dim=1)
            del output
            if token in eos_ids:
                break
        if not generated:
            raise RuntimeError(f"reference prompt {prompt_id} generated no tokens")
        reached_eos = generated[-1] in eos_ids
        decoded = tokenizer.decode(generated, skip_special_tokens=True)
        if not decoded.strip():
            raise RuntimeError(f"reference prompt {prompt_id} decoded empty output")
        reference_cases.append({
            "promptId": prompt_id,
            "language": str(prompt.get("language", "")),
            "text": text,
            "textSha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
            "formattedInputIds": formatted_ids,
            "formattedInputSha256": canonical_sha256(formatted_ids),
            "inputTokenCount": len(formatted_ids),
            "eosTokenIds": eos_ids,
            "referenceGeneratedTokenIds": generated,
            "referenceGeneratedTokenSha256": canonical_sha256(generated),
            "referenceDecodedText": decoded,
            "referenceStopReason":
                "EOS" if reached_eos else "MAX_NEW_TOKENS",
            "referenceTokenMs": token_ms,
            "referenceTotalMs": sum(token_ms),
        })
        print(
            "SPEC162_REFERENCE_OK",
            f"promptId={prompt_id}",
            f"generatedTokens={len(generated)}",
            flush=True,
        )

    reference_memory = [
        {"index": index, **_memory_snapshot(torch, index)}
        for index in range(3)
    ]
    full_state = model.state_dict()
    roles = [f"/LLM/Pipeline/Stage/{index}" for index in range(STAGE_COUNT)]
    ranges = split_layer_ranges(EXPECTED_LAYER_COUNT, STAGE_COUNT)
    if tuple(ranges) != STAGE_RANGES:
        raise RuntimeError(f"unexpected layer ranges: {ranges}")
    stage_paths: list[Path] = []
    stage_rows: list[dict[str, Any]] = []
    for role in roles:
        spec = qwen_transformer_stage_spec(
            role=role,
            stages=STAGE_COUNT,
            layer_count=EXPECTED_LAYER_COUNT,
            model_name=MODEL_REPOSITORY,
        )
        path = stage_root / (
            f"stage-{spec['stageIndex']}-qwen-transformers.pt")
        package = {
            "schema": "ndnsf-di-qwen-stage-weights-v1",
            "spec": spec,
            "config": model.config.to_dict(),
            "attnImplementation": getattr(
                model.config, "_attn_implementation", "sdpa"),
            "state_dict": _stage_state_dict(full_state, spec),
        }
        if not package["state_dict"]:
            raise RuntimeError(f"stage {spec['stageIndex']} has no weights")
        _safe_torch_save(torch, package, path)
        del package
        stage_paths.append(path)
        stage_rows.append({
            "filename": path.name,
            "path": str(
                promoted_artifact_dir
                / "qwen-transformers-stage-artifacts"
                / path.name),
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
            "role": role,
            "stageIndex": int(spec["stageIndex"]),
            "stageCount": int(spec["stageCount"]),
            "layerCount": int(spec["layerCount"]),
            "layerRange": dict(spec["layerRange"]),
            "runtime": "qwen-transformers",
            "dtype": DTYPE,
            "cpuFallback": False,
        })
        print(
            "SPEC162_STAGE_WRITTEN",
            f"stage={spec['stageIndex']}",
            f"bytes={path.stat().st_size}",
            flush=True,
        )

    del full_state
    del model
    del context
    gc.collect()
    for index in range(3):
        torch.cuda.empty_cache()
    reference_release_allocated = [
        int(torch.cuda.memory_allocated(index))
        for index in range(3)
    ]
    if any(value > 512 * 1024**2 for value in reference_release_allocated):
        raise RuntimeError(
            "reference model CUDA release incomplete: "
            f"{reference_release_allocated}")
    print(
        "SPEC162_REFERENCE_CUDA_RELEASED",
        reference_release_allocated,
        flush=True,
    )

    validation_case = max(
        reference_cases,
        key=lambda item: int(item["inputTokenCount"]),
    )
    validation_ids = list(validation_case["formattedInputIds"])
    validation_input_token_count = len(validation_ids)
    payload = json.dumps({
        "runtime": "qwen-transformers",
        "inputIds": [validation_ids],
        "inputShape": [1, len(validation_ids)],
        "inputDtype": "int64",
        "isFirst": True,
        "isLast": False,
    }, sort_keys=True).encode("utf-8")
    for index, (path, row) in enumerate(zip(stage_paths, stage_rows)):
        torch.cuda.reset_peak_memory_stats(index)
        stage_model = qwen_transformer_model_from_stage_package(
            path, device=f"cuda:{index}", require_cuda=True)
        parameter = next(stage_model.parameters())
        if parameter.device.type != "cuda":
            raise RuntimeError(f"stage {index} CUDA validation failed")
        output = run_qwen_transformer_stage(
            payload,
            role=str(row["role"]),
            stages=STAGE_COUNT,
            model=stage_model,
            compute_delay_ms=0,
        )
        torch.cuda.synchronize(index)
        row.update({
            "cudaValidated": True,
            "forwardValidated": True,
            "validationPromptId": validation_case["promptId"],
            "validationInputTokenCount": validation_input_token_count,
            "cudaDevice": str(parameter.device),
            "validationGpuName": torch.cuda.get_device_name(index),
            **_memory_snapshot(torch, index),
        })
        payload = output
        del output
        del stage_model
        torch.cuda.empty_cache()
        print(
            "SPEC162_STAGE_CUDA_OK",
            path.name,
            f"peakReservedBytes={row['peakReservedBytes']}",
            flush=True,
        )
    validate_stage_acceptance(
        stage_rows,
        validation_input_token_count=validation_input_token_count,
    )

    policy_path = output_root / "policy.yaml"
    _build_policy(
        output_path=policy_path,
        stage_paths=stage_paths,
        promoted_artifact_dir=promoted_artifact_dir,
        model_digest=f"sha256:{model_digest}",
    )
    reference = {
        "schemaVersion": "ndnsf-di-qwen36-reference-set-v1",
        "modelProfile": MODEL_PROFILE,
        "repository": MODEL_REPOSITORY,
        "revision": MODEL_REVISION,
        "modelDigest": f"sha256:{model_digest}",
        "dtype": DTYPE,
        "quantization": "none",
        "attentionImplementation": "sdpa",
        "textOnly": True,
        "enableThinking": False,
        "generation": {
            "strategy": "greedy",
            "maxNewTokens": MAX_NEW_TOKENS,
            "requireEos": False,
            "useCache": False,
        },
        "promptSetSha256": prompt_set_sha,
        "tokenizerDigest": f"sha256:{tokenizer_digest}",
        "chatTemplateDigest": f"sha256:{chat_template_digest}",
        "eosTokenIds": eos_ids,
        "gpus": gpu_rows,
        "deviceMap": device_map,
        "runtime": {
            "sifSha256": args.runtime_sif_sha256,
            "torch": str(torch.__version__),
            "transformers": package_version("transformers"),
            "attentionImplementation": "sdpa",
        },
        "cudaMemory": reference_memory,
        "prompts": reference_cases,
    }
    reference_path = output_root / "reference.json"
    write_json(reference_path, reference)

    tokenizer_manifest = {
        "schemaVersion": "ndnsf-di-qwen36-tokenizer-manifest-v1",
        "digest": f"sha256:{tokenizer_digest}",
        "chatTemplateDigest": f"sha256:{chat_template_digest}",
        "eosTokenIds": eos_ids,
        "enableThinking": False,
        "files": tokenizer_rows,
    }
    tokenizer_manifest_path = output_root / "tokenizer-manifest.json"
    write_json(tokenizer_manifest_path, tokenizer_manifest)

    stage_manifest = {
        "schemaVersion": "ndnsf-di-qwen36-stage-manifest-v1",
        "modelProfile": MODEL_PROFILE,
        "submissionId": args.submission_id,
        "repository": MODEL_REPOSITORY,
        "revision": MODEL_REVISION,
        "modelDigest": f"sha256:{model_digest}",
        "dtype": DTYPE,
        "quantization": "none",
        "layerCount": EXPECTED_LAYER_COUNT,
        "layerRanges": [list(item) for item in STAGE_RANGES],
        "cpuFallbackAllowed": False,
        "minimumGpuHeadroomBytes": MIN_GPU_HEADROOM_BYTES,
        "validation": {
            "promptId": validation_case["promptId"],
            "inputTokenCount": validation_input_token_count,
            "policy": "maximum-registered-prompt",
        },
        "policy": {
            "path": str(promoted_artifact_dir / "policy.yaml"),
            "sha256": sha256_file(policy_path),
            "bytes": policy_path.stat().st_size,
        },
        "reference": {
            "path": str(promoted_artifact_dir / "reference.json"),
            "sha256": sha256_file(reference_path),
            "bytes": reference_path.stat().st_size,
        },
        "tokenizer": {
            "path": str(promoted_artifact_dir / "tokenizer"),
            "manifestSha256": sha256_file(tokenizer_manifest_path),
            "digest": f"sha256:{tokenizer_digest}",
        },
        "stages": stage_rows,
        "runtimeSifSha256": args.runtime_sif_sha256,
        "runtime": {
            "torch": str(torch.__version__),
            "transformers": package_version("transformers"),
        },
        "sourceBundleSha256": args.source_bundle_sha256,
        "capacityDecisionSha256": args.capacity_decision_sha256,
    }
    stage_manifest_path = output_root / "stage-manifest.json"
    write_json(stage_manifest_path, stage_manifest)
    stage_manifest_sha = sha256_file(stage_manifest_path)
    candidate_digest = canonical_sha256({
        "modelDigest": f"sha256:{model_digest}",
        "stageManifestSha256": stage_manifest_sha,
        "runtimeSifSha256": args.runtime_sif_sha256,
        "sourceBundleSha256": args.source_bundle_sha256,
        "capacityDecisionSha256": args.capacity_decision_sha256,
    })
    smoke_campaign = {
        "schemaVersion": "ndnsf-di-qwen-generation-campaign-v1",
        "campaignId": f"{args.submission_id}-smoke",
        "candidateId": f"sha256:{candidate_digest}",
        "model": {
            "repository": MODEL_REPOSITORY,
            "revision": MODEL_REVISION,
            "dtype": "bf16",
            "quantization": "none",
            "digest": f"sha256:{model_digest}",
        },
        "generation": {
            "strategy": "greedy",
            "maxNewTokens": MAX_NEW_TOKENS,
            "requireEos": False,
            "useCache": False,
            "enableThinking": False,
        },
        "repetitions": {
            "warmupPerPrompt": 0,
            "measuredPerPrompt": 1,
            "sequential": True,
        },
        "prompts": [reference_cases[0]],
        "stageManifestSha256": f"sha256:{stage_manifest_sha}",
        "capacityDecisionSha256": f"sha256:{args.capacity_decision_sha256}",
    }
    write_json(output_root / "smoke-campaign.json", smoke_campaign)
    formal_campaign = {
        **smoke_campaign,
        "campaignId": f"{args.submission_id}-formal",
        "repetitions": {
            "warmupPerPrompt": 1,
            "measuredPerPrompt": 5,
            "sequential": True,
        },
        "prompts": reference_cases,
    }
    write_json(output_root / "formal-campaign.json", formal_campaign)
    print(
        "SPEC162_PREPARATION_OK",
        f"modelDigest=sha256:{model_digest}",
        f"stageManifestSha256=sha256:{stage_manifest_sha}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Prepare frozen Qwen2.5-32B references and three CUDA stage packages."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import time
from pathlib import Path
from typing import Any, Iterable


MODEL_REPOSITORY = "Qwen/Qwen2.5-32B-Instruct"
MODEL_REVISION = "5ede1c97bbab6ce5cda5812749b4c0bdf79b18dd"
EXPECTED_LAYER_COUNT = 64
STAGE_COUNT = 3
MAX_NEW_TOKENS = 64
PROMPT_SCHEMA = "ndnsf-di-qwen-prompt-set-v1"


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


def tree_manifest(root: Path, *, excluded_names: Iterable[str] = ()) -> list[dict[str, Any]]:
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


def build_policy(
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
        application="llm-pipeline-qwen32b-spec161",
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
                artifact_name=f"/Model/LLM/Pipeline/Qwen/{role.strip('/')}",
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
                    "dtype": "float16",
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
                "dtype": "float16",
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-root", required=True)
    parser.add_argument("--prompt-set", required=True)
    parser.add_argument("--promoted-artifact-dir", required=True)
    parser.add_argument("--submission-id", required=True)
    parser.add_argument("--capacity-decision-sha256", required=True)
    parser.add_argument("--runtime-sif-sha256", required=True)
    parser.add_argument("--source-bundle-sha256", required=True)
    args = parser.parse_args()

    work_root = Path(args.work_root).resolve()
    output_root = work_root / "output"
    stage_root = output_root / "qwen-transformers-stage-artifacts"
    tokenizer_root = output_root / "tokenizer"
    source_manifest_path = output_root / "model-source-manifest.json"
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
    from huggingface_hub import snapshot_download
    from transformers import AutoModelForCausalLM, AutoTokenizer
    from llm_pipeline_lib import (
        _safe_torch_save,
        _stage_state_dict,
        qwen_transformer_model_from_stage_package,
        qwen_transformer_stage_spec,
        split_layer_ranges,
    )

    if not torch.cuda.is_available():
        raise RuntimeError("H100 CUDA device is unavailable")
    if torch.cuda.device_count() != 1:
        raise RuntimeError(
            f"reference job requires exactly one visible GPU, found {torch.cuda.device_count()}")
    device = torch.device("cuda:0")
    gpu_name = torch.cuda.get_device_name(device)
    if "H100" not in gpu_name.upper():
        raise RuntimeError(f"reference GPU is not H100: {gpu_name}")

    cache_root = work_root / "huggingface-cache"
    print("SPEC161_DOWNLOAD_BEGIN", MODEL_REPOSITORY, MODEL_REVISION, flush=True)
    snapshot = Path(snapshot_download(
        repo_id=MODEL_REPOSITORY,
        revision=MODEL_REVISION,
        cache_dir=cache_root,
    )).resolve()
    if snapshot.name != MODEL_REVISION:
        raise RuntimeError(
            f"snapshot revision mismatch: {snapshot.name} != {MODEL_REVISION}")
    print("SPEC161_DOWNLOAD_COMPLETE", snapshot, flush=True)

    source_rows = tree_manifest(snapshot)
    source_manifest = {
        "schemaVersion": "ndnsf-di-qwen-source-manifest-v1",
        "repository": MODEL_REPOSITORY,
        "revision": MODEL_REVISION,
        "files": source_rows,
        "totalBytes": sum(row["bytes"] for row in source_rows),
    }
    model_digest = canonical_sha256(source_manifest)
    source_manifest["modelDigest"] = f"sha256:{model_digest}"
    write_json(source_manifest_path, source_manifest)

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

    print("SPEC161_MODEL_LOAD_BEGIN", flush=True)
    model = AutoModelForCausalLM.from_pretrained(
        snapshot,
        local_files_only=True,
        trust_remote_code=False,
        torch_dtype=torch.float16,
        attn_implementation="sdpa",
    )
    model.eval()
    layer_count = len(model.model.layers)
    if layer_count != EXPECTED_LAYER_COUNT:
        raise RuntimeError(
            f"expected {EXPECTED_LAYER_COUNT} layers, found {layer_count}")
    model.to(device)
    if next(model.parameters()).device.type != "cuda":
        raise RuntimeError("reference model did not move to CUDA")
    print("SPEC161_MODEL_LOAD_COMPLETE", gpu_name, flush=True)

    eos_ids = normalize_eos_ids(model.generation_config.eos_token_id)
    reference_cases: list[dict[str, Any]] = []
    for prompt in prompts:
        prompt_id = str(prompt["promptId"])
        text = str(prompt["text"])
        formatted = tokenizer.apply_chat_template(
            [{"role": "user", "content": text}],
            tokenize=True,
            add_generation_prompt=True,
        )
        formatted_ids = [int(item) for item in formatted]
        if not formatted_ids or len(formatted_ids) > 512:
            raise RuntimeError(
                f"prompt {prompt_id} input tokens invalid: {len(formatted_ids)}")
        context = torch.tensor(
            [formatted_ids], dtype=torch.long, device=device)
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
            torch.cuda.synchronize(device)
            token_ms.append((time.perf_counter() - started) * 1000.0)
            generated.append(token)
            context = torch.cat([
                context,
                torch.tensor([[token]], dtype=torch.long, device=device),
            ], dim=1)
            del output
            if token in eos_ids:
                break
        if not generated or generated[-1] not in eos_ids:
            raise RuntimeError(
                f"reference prompt {prompt_id} did not reach EOS within "
                f"{MAX_NEW_TOKENS} tokens")
        decoded = tokenizer.decode(generated, skip_special_tokens=True)
        if not decoded.strip():
            raise RuntimeError(f"reference prompt {prompt_id} decoded empty output")
        row = {
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
            "referenceStopReason": "EOS",
            "referenceTokenMs": token_ms,
            "referenceTotalMs": sum(token_ms),
        }
        reference_cases.append(row)
        print(
            "SPEC161_REFERENCE_OK",
            f"promptId={prompt_id}",
            f"inputTokens={len(formatted_ids)}",
            f"generatedTokens={len(generated)}",
            f"totalMs={sum(token_ms):.3f}",
            flush=True,
        )

    full_state = model.state_dict()
    roles = [f"/LLM/Pipeline/Stage/{index}" for index in range(STAGE_COUNT)]
    ranges = split_layer_ranges(layer_count, STAGE_COUNT)
    if ranges != [(0, 21), (21, 42), (42, 64)]:
        raise RuntimeError(f"unexpected layer ranges: {ranges}")
    stage_paths: list[Path] = []
    stage_rows: list[dict[str, Any]] = []
    for role in roles:
        spec = qwen_transformer_stage_spec(
            role=role,
            stages=STAGE_COUNT,
            layer_count=layer_count,
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
        _safe_torch_save(torch, package, path)
        stage_paths.append(path)
        stage_rows.append({
            "filename": path.name,
            "path": str(
                promoted_artifact_dir
                / "qwen-transformers-stage-artifacts"
                / path.name
            ),
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
            "role": role,
            "stageIndex": int(spec["stageIndex"]),
            "stageCount": int(spec["stageCount"]),
            "layerCount": int(spec["layerCount"]),
            "layerRange": dict(spec["layerRange"]),
            "runtime": "qwen-transformers",
            "dtype": "float16",
        })
        print(
            "SPEC161_STAGE_WRITTEN",
            f"stage={spec['stageIndex']}",
            f"bytes={path.stat().st_size}",
            flush=True,
        )

    del full_state
    del model
    torch.cuda.empty_cache()
    for path, row in zip(stage_paths, stage_rows):
        stage_model = qwen_transformer_model_from_stage_package(
            path, device="cuda:0", require_cuda=True)
        parameter = next(stage_model.parameters())
        if parameter.device.type != "cuda":
            raise RuntimeError(f"stage {path.name} CUDA validation failed")
        row["cudaValidated"] = True
        row["cudaDevice"] = str(parameter.device)
        row["validationGpuName"] = gpu_name
        del stage_model
        torch.cuda.empty_cache()
        print("SPEC161_STAGE_CUDA_OK", path.name, flush=True)

    policy_path = output_root / "policy.yaml"
    build_policy(
        output_path=policy_path,
        stage_paths=stage_paths,
        promoted_artifact_dir=promoted_artifact_dir,
        model_digest=f"sha256:{model_digest}",
    )
    reference = {
        "schemaVersion": "ndnsf-di-qwen-reference-set-v1",
        "repository": MODEL_REPOSITORY,
        "revision": MODEL_REVISION,
        "modelDigest": f"sha256:{model_digest}",
        "dtype": "float16",
        "quantization": "none",
        "attentionImplementation": "sdpa",
        "generation": {
            "strategy": "greedy",
            "maxNewTokens": MAX_NEW_TOKENS,
            "requireEos": True,
            "useCache": False,
        },
        "promptSetSha256": prompt_set_sha,
        "tokenizerDigest": f"sha256:{tokenizer_digest}",
        "chatTemplateDigest": f"sha256:{chat_template_digest}",
        "eosTokenIds": eos_ids,
        "gpu": {
            "name": gpu_name,
            "device": "cuda:0",
        },
        "prompts": reference_cases,
    }
    reference_path = output_root / "reference.json"
    write_json(reference_path, reference)

    tokenizer_manifest = {
        "schemaVersion": "ndnsf-di-qwen-tokenizer-manifest-v1",
        "digest": f"sha256:{tokenizer_digest}",
        "chatTemplateDigest": f"sha256:{chat_template_digest}",
        "eosTokenIds": eos_ids,
        "files": tokenizer_rows,
    }
    tokenizer_manifest_path = output_root / "tokenizer-manifest.json"
    write_json(tokenizer_manifest_path, tokenizer_manifest)

    stage_manifest = {
        "schemaVersion": "ndnsf-di-qwen32b-stage-manifest-v1",
        "submissionId": args.submission_id,
        "repository": MODEL_REPOSITORY,
        "revision": MODEL_REVISION,
        "modelDigest": f"sha256:{model_digest}",
        "dtype": "float16",
        "quantization": "none",
        "layerCount": layer_count,
        "layerRanges": [list(item) for item in ranges],
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
            "dtype": "fp16",
            "quantization": "none",
            "digest": f"sha256:{model_digest}",
        },
        "generation": {
            "strategy": "greedy",
            "maxNewTokens": MAX_NEW_TOKENS,
            "requireEos": True,
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
    print(
        "SPEC161_PREPARATION_OK",
        f"modelDigest=sha256:{model_digest}",
        f"stageManifestSha256=sha256:{stage_manifest_sha}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

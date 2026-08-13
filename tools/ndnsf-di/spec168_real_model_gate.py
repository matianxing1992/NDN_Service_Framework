"""Resource-independent real-model admission checks for Spec 168."""

from __future__ import annotations

import json
from pathlib import Path
import re


def validate_real_model_binding(args) -> None:
    """Bind the real-runtime gate to the supplied immutable model manifest."""
    if args.runtime not in {
        "qwen-transformers", "qwen-onnx", "qwen-onnx-cpu-native"
    }:
        raise ValueError(
            "--require-real-model rejects simulated runtime: "
            f"{args.runtime}"
        )
    required = {
        "--qwen-stage-manifest": args.qwen_stage_manifest,
        "--generation-campaign-manifest": args.generation_campaign_manifest,
        "--generation-jsonl": args.generation_jsonl,
        "--qwen-tokenizer-dir": args.qwen_tokenizer_dir,
        "--workload-digest": args.workload_digest,
        "--model-identity-digest": args.model_identity_digest,
    }
    missing = [name for name, value in required.items() if not str(value).strip()]
    if missing:
        raise ValueError(
            "--require-real-model requires: " + ", ".join(missing)
        )
    tokenizer_dir = Path(args.qwen_tokenizer_dir).expanduser()
    if not all((tokenizer_dir / name).is_file() for name in (
            "tokenizer.json", "tokenizer_config.json")):
        raise ValueError(
            "--require-real-model tokenizer snapshot is not resolvable: "
            f"{tokenizer_dir}"
        )
    if not re.fullmatch(r"sha256:[0-9a-f]{64}",
                        str(args.model_identity_digest)):
        raise ValueError(
            "--require-real-model requires a sha256 model identity digest"
        )
    manifest_path = Path(args.qwen_stage_manifest).expanduser()
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        raise ValueError(
            f"--require-real-model stage manifest is invalid: {manifest_path}"
        ) from error
    expected_stage_runtime = (
        "qwen-onnx" if args.runtime == "qwen-onnx-cpu-native"
        else args.runtime
    )
    stages = manifest.get("stages")
    mismatches = []
    if manifest.get("repository") != args.qwen_model:
        mismatches.append("repository")
    if manifest.get("revision") != args.qwen_revision:
        mismatches.append("revision")
    if manifest.get("modelDigest") != args.model_identity_digest:
        mismatches.append("modelDigest")
    if not isinstance(stages, list) or len(stages) != args.stages:
        mismatches.append("stages")
    elif any(item.get("runtime") != expected_stage_runtime for item in stages):
        mismatches.append("runtime")
    if mismatches:
        raise ValueError(
            "--require-real-model immutable model binding mismatch: "
            + ", ".join(mismatches)
        )


def real_model_artifact_marker(runtime: str) -> str:
    if runtime == "qwen-transformers":
        return "LLM_PIPELINE_QWEN_STAGE_ARTIFACT_READY"
    return "LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY"


def real_model_readiness_marker(runtime: str, *, deferred_selection: bool) -> str:
    if deferred_selection:
        return "LLM_PIPELINE_QWEN_SELECTION_PREPARE"
    return real_model_artifact_marker(runtime)


def expected_stage_completion_marker(
    runtime: str,
    *,
    stage_index: int,
    stages: int,
    full_generation: bool,
) -> str:
    if full_generation and runtime in {"qwen-transformers", "qwen-onnx"}:
        if stage_index == 0:
            return "LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL"
        if stage_index == stages - 1:
            return "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED"
        return "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED"
    if runtime == "qwen-onnx-cpu-native":
        return "NDNSF_DI_ONNX_TIMING"
    if runtime == "qwen-onnx":
        suffix = "FINAL" if stage_index == stages - 1 else "OUTPUT"
        return f"LLM_PIPELINE_QWEN_ONNX_STAGE_{suffix}"
    if runtime == "qwen-transformers":
        suffix = "FINAL" if stage_index == stages - 1 else "OUTPUT"
        return f"LLM_PIPELINE_QWEN_STAGE_{suffix}"
    if runtime == "tiny-transformers":
        suffix = "FINAL" if stage_index == stages - 1 else "OUTPUT"
        return f"LLM_PIPELINE_TRANSFORMER_STAGE_{suffix}"
    suffix = "FINAL" if stage_index == stages - 1 else "OUTPUT"
    return f"LLM_PIPELINE_STAGE_{suffix}"

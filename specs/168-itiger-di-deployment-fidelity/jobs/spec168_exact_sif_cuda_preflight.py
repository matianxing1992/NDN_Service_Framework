#!/usr/bin/env python3
"""Bounded single-GPU exact-SIF preflight for the frozen Qwen3 control.

The preflight never prepares, copies, or publishes model weights. It verifies
the existing immutable artifacts, then loads and executes the three stages one
at a time on one allocated CUDA device. This is Gate C environment evidence,
not a distributed-inference result.
"""

from __future__ import annotations

import argparse
import gc
import hashlib
import json
import os
from pathlib import Path
import time
from typing import Any


SCHEMA = "ndnsf-di.spec168-exact-sif-cuda-preflight.v1"


class PreflightError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalize_digest(value: object) -> str:
    text = str(value)
    return text if text.startswith("sha256:") else "sha256:" + text


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise PreflightError(f"JSON_OBJECT_REQUIRED:{path}")
    return value


def verify_static_inputs(args: argparse.Namespace) -> dict[str, Any]:
    manifest_path = args.stage_manifest.resolve()
    reference_path = args.reference.resolve()
    manifest_digest = normalize_digest(sha256_file(manifest_path))
    if manifest_digest != normalize_digest(args.expected_stage_manifest_digest):
        raise PreflightError("STAGE_MANIFEST_DIGEST_MISMATCH")

    manifest = load_json(manifest_path)
    reference = load_json(reference_path)
    if normalize_digest(manifest.get("modelDigest", "")) != normalize_digest(
        args.expected_model_digest
    ):
        raise PreflightError("MODEL_DIGEST_MISMATCH")
    if str(manifest.get("revision", "")) != str(args.expected_revision):
        raise PreflightError("MODEL_REVISION_MISMATCH")
    if normalize_digest(manifest.get("runtimeSifSha256", "")) != normalize_digest(
        args.expected_sif_digest
    ):
        raise PreflightError("MANIFEST_SIF_DIGEST_MISMATCH")
    if normalize_digest(os.environ.get("SPEC168_RUNTIME_SIF_SHA256", "")) != normalize_digest(
        args.expected_sif_digest
    ):
        raise PreflightError("RUNTIME_SIF_DIGEST_MISMATCH")
    if normalize_digest(os.environ.get("SPEC168_SOURCE_DIGEST", "")) != normalize_digest(
        args.expected_source_digest
    ):
        raise PreflightError("SOURCE_DIGEST_MISMATCH")
    if normalize_digest(os.environ.get("SPEC168_SOURCE_BUNDLE_DIGEST", "")) != normalize_digest(
        args.expected_source_bundle_digest
    ):
        raise PreflightError("SOURCE_BUNDLE_DIGEST_MISMATCH")

    stages = manifest.get("stages")
    if not isinstance(stages, list) or len(stages) != 3:
        raise PreflightError("THREE_STAGES_REQUIRED")
    verified_stages = []
    for expected_index, row in enumerate(stages):
        if not isinstance(row, dict) or int(row.get("stageIndex", -1)) != expected_index:
            raise PreflightError("STAGE_ORDER_INVALID")
        if int(row.get("stageCount", 0)) != 3:
            raise PreflightError("STAGE_COUNT_INVALID")
        path = Path(str(row.get("path", ""))).resolve()
        if not path.is_file():
            raise PreflightError(f"STAGE_ARTIFACT_MISSING:{expected_index}")
        if path.stat().st_size != int(row.get("bytes", -1)):
            raise PreflightError(f"STAGE_ARTIFACT_SIZE_MISMATCH:{expected_index}")
        observed = sha256_file(path)
        if observed != str(row.get("sha256", "")):
            raise PreflightError(f"STAGE_ARTIFACT_DIGEST_MISMATCH:{expected_index}")
        verified_stages.append({
            "stageIndex": expected_index,
            "role": str(row.get("role", "")),
            "path": str(path),
            "bytes": path.stat().st_size,
            "sha256": observed,
            "layerRange": dict(row.get("layerRange", {})),
        })

    prompts = reference.get("prompts")
    if not isinstance(prompts, list) or not prompts or not isinstance(prompts[0], dict):
        raise PreflightError("REFERENCE_PROMPT_MISSING")
    prompt = prompts[0]
    input_ids = prompt.get("formattedInputIds")
    generated_ids = prompt.get("referenceGeneratedTokenIds")
    if not isinstance(input_ids, list) or not input_ids:
        raise PreflightError("REFERENCE_INPUT_IDS_MISSING")
    if not isinstance(generated_ids, list) or not generated_ids:
        raise PreflightError("REFERENCE_OUTPUT_IDS_MISSING")
    return {
        "manifest": manifest,
        "reference": reference,
        "stageManifestDigest": manifest_digest,
        "stages": verified_stages,
        "prompt": prompt,
    }


def run_cuda_preflight(static: dict[str, Any]) -> dict[str, Any]:
    import torch
    from llm_pipeline_lib import (
        encode_qwen_input_ids,
        qwen_transformer_model_from_stage_package,
        run_qwen_transformer_stage,
        warm_qwen_transformer_stage,
    )

    if not torch.cuda.is_available() or torch.cuda.device_count() < 1:
        raise PreflightError("CUDA_DEVICE_UNAVAILABLE")
    device = torch.device("cuda:0")
    properties = torch.cuda.get_device_properties(device)
    prompt = static["prompt"]
    payload = encode_qwen_input_ids(
        [prompt["formattedInputIds"]],
        request_id="/spec168/gate-c/qwen3-0.6b",
    )
    stage_results = []
    for row in static["stages"]:
        torch.cuda.empty_cache()
        torch.cuda.reset_peak_memory_stats(device)
        load_started = time.perf_counter()
        model = qwen_transformer_model_from_stage_package(
            row["path"], device="cuda:0", require_cuda=True)
        load_ms = (time.perf_counter() - load_started) * 1000.0
        warm_started = time.perf_counter()
        if warm_qwen_transformer_stage(model, "cuda:0") is not True:
            raise PreflightError(f"STAGE_WARMUP_FAILED:{row['stageIndex']}")
        warmup_ms = (time.perf_counter() - warm_started) * 1000.0
        timing: dict[str, float | int | str] = {}
        payload = run_qwen_transformer_stage(
            payload,
            role=row["role"],
            stages=3,
            model=model,
            timing=timing,
        )
        stage_results.append({
            "stageIndex": row["stageIndex"],
            "role": row["role"],
            "device": str(next(model.parameters()).device),
            "cpuFallback": bool(getattr(model, "ndnsf_cpu_fallback", True)),
            "loadMs": round(load_ms, 3),
            "warmupMs": round(warmup_ms, 3),
            "forwardMs": round(float(timing.get("total_ms", 0.0)), 3),
            "peakAllocatedBytes": int(torch.cuda.max_memory_allocated(device)),
            "peakReservedBytes": int(torch.cuda.max_memory_reserved(device)),
        })
        if stage_results[-1]["device"] != "cuda:0" or stage_results[-1]["cpuFallback"]:
            raise PreflightError(f"STAGE_CPU_FALLBACK:{row['stageIndex']}")
        del model
        gc.collect()
        torch.cuda.empty_cache()

    response = json.loads(payload.decode("utf-8"))
    expected_top_token = int(prompt["referenceGeneratedTokenIds"][0])
    actual_top_token = int(response.get("topToken", -1))
    if actual_top_token != expected_top_token:
        raise PreflightError(
            f"TOP_TOKEN_MISMATCH:expected={expected_top_token}:actual={actual_top_token}"
        )
    return {
        "gpuName": str(properties.name),
        "gpuTotalBytes": int(properties.total_memory),
        "torchVersion": str(torch.__version__),
        "cudaVersion": str(torch.version.cuda),
        "stageResults": stage_results,
        "expectedTopToken": expected_top_token,
        "actualTopToken": actual_top_token,
        "response": response,
    }


def write_result(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".partial.{os.getpid()}")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage-manifest", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-stage-manifest-digest", required=True)
    parser.add_argument("--expected-model-digest", required=True)
    parser.add_argument("--expected-revision", required=True)
    parser.add_argument("--expected-sif-digest", required=True)
    parser.add_argument("--expected-source-digest", required=True)
    parser.add_argument("--expected-source-bundle-digest", required=True)
    parser.add_argument("--validate-only", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    started = time.time()
    result: dict[str, Any] = {
        "schema": SCHEMA,
        "status": "FAIL",
        "startedEpochMs": int(started * 1000),
        "sourceDigest": normalize_digest(args.expected_source_digest),
        "sourceBundleDigest": normalize_digest(args.expected_source_bundle_digest),
        "runtimeSifDigest": normalize_digest(args.expected_sif_digest),
        "modelDigest": normalize_digest(args.expected_model_digest),
        "revision": str(args.expected_revision),
        "claimBoundary": "single-node exact-SIF CUDA environment preflight only",
    }
    try:
        static = verify_static_inputs(args)
        result["stageManifestDigest"] = static["stageManifestDigest"]
        result["verifiedStages"] = static["stages"]
        if args.validate_only:
            result["validationMode"] = "STATIC_ONLY"
        else:
            result["cuda"] = run_cuda_preflight(static)
            result["validationMode"] = "EXACT_SIF_CUDA"
        result["status"] = "PASS"
    except Exception as error:
        result["errorType"] = type(error).__name__
        result["error"] = str(error)
        result["completedEpochMs"] = int(time.time() * 1000)
        write_result(args.output, result)
        print(f"SPEC168_EXACT_SIF_CUDA_PREFLIGHT_FAIL error={type(error).__name__}:{error}")
        return 1
    result["completedEpochMs"] = int(time.time() * 1000)
    write_result(args.output, result)
    print(
        "SPEC168_EXACT_SIF_CUDA_PREFLIGHT_PASS "
        f"mode={result['validationMode']} output={args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Run the frozen Spec 165 Qwen3 ONNX workload on one TigerCluster GPU."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from pathlib import Path

from llm_pipeline_lib import (
    decode_payload,
    encode_qwen_pipeline_context,
    run_qwen_onnx_stage,
)
from provider import (
    _qwen_onnx_profile_summary,
    _qwen_onnx_session,
    _qwen_onnx_session_placement,
)


EXPECTED_WORKLOAD = (
    "sha256:2d2aac62e9340ed401b7e7579d992f7fe652de06ee0829e0d8d0ea4c653a7ae9"
)
EXPECTED_MODEL = (
    "sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a"
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--workload", type=Path, required=True)
    parser.add_argument("--service-manifest", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    args = parser.parse_args()

    campaign = json.loads(args.campaign.read_text(encoding="utf-8"))
    workload = json.loads(args.workload.read_text(encoding="utf-8"))
    manifest = json.loads(args.service_manifest.read_text(encoding="utf-8"))
    if campaign.get("workloadDigest") != EXPECTED_WORKLOAD:
        raise SystemExit("FROZEN_CAMPAIGN_WORKLOAD_DIGEST_MISMATCH")
    if workload.get("workloadDigest") != EXPECTED_WORKLOAD:
        raise SystemExit("FROZEN_WORKLOAD_DIGEST_MISMATCH")
    if workload.get("modelIdentity", {}).get("contentDigest") != EXPECTED_MODEL:
        raise SystemExit("FROZEN_MODEL_IDENTITY_DIGEST_MISMATCH")

    expected_artifacts = (
        ("stage-0-qwen.onnx",
         "6f02058ba2cc420b4f11c6e7ab391451c3fbdb9bc4ca4ba8adebd633de60ec06"),
        ("stage-1-qwen.onnx",
         "b9e4acb15b5ea2ba009f4bd6a132ce7efb90c18b5062aadc6769bd5514f3e501"),
        ("stage-2-qwen.onnx",
         "b2cb724a2f4638fee6c008c0f657ce687a1de262b67b8cbaf351aca6033ac841"),
    )
    for filename, expected in expected_artifacts:
        if _sha256(args.artifacts / filename) != expected:
            raise SystemExit(f"FROZEN_ARTIFACT_DIGEST_MISMATCH:{filename}")

    stages = list(manifest["stages"])
    if len(stages) != 3:
        raise SystemExit("FROZEN_STAGE_COUNT_MISMATCH")
    sessions = []
    placements = []
    load_started = time.perf_counter()
    for index, stage in enumerate(stages):
        path = args.artifacts / f"stage-{index}-qwen.onnx"
        session = _qwen_onnx_session(
            str(path),
            device="cuda:0",
            require_cuda=True,
            profile_prefix=str(
                args.output.parent / f"ort-stage-{index}-profile"),
        )
        device, cpu_fallback = _qwen_onnx_session_placement(session)
        if device != "cuda:0" or cpu_fallback:
            raise SystemExit(f"CUDA_PLACEMENT_REQUIRED:stage-{index}")
        sessions.append(session)
        placements.append({
            "stageIndex": index,
            "device": device,
            "cpuFallback": int(cpu_fallback),
            "providers": list(session.get_providers()),
        })
    model_load_ms = (time.perf_counter() - load_started) * 1000.0

    rows = []
    for prompt in campaign["prompts"]:
        prompt_id = str(prompt["promptId"])
        reference = [int(value)
                     for value in prompt["referenceGeneratedTokenIds"]]
        if len(reference) != 8:
            raise SystemExit(f"FROZEN_TOKEN_COUNT_MISMATCH:{prompt_id}")
        for phase, repetitions in (("warmup", 1), ("measured", 3)):
            for repetition in range(repetitions):
                request_id = (
                    f"{args.run_id}:{prompt_id}:{phase}:{repetition}")
                context = [int(value) for value in prompt["formattedInputIds"]]
                generated = []
                token_ms = []
                request_started = time.perf_counter()
                for token_index in range(8):
                    token_started = time.perf_counter()
                    payload = encode_qwen_pipeline_context(
                        [context],
                        request_id=request_id,
                        session_id=request_id,
                        context_epoch=token_index,
                    )
                    for index, (session, stage) in enumerate(
                            zip(sessions, stages)):
                        payload = run_qwen_onnx_stage(
                            payload,
                            role=str(stage["role"]),
                            stages=3,
                            session=session,
                            metadata=stage,
                        )
                    token = int(decode_payload(payload)["topToken"])
                    generated.append(token)
                    context.append(token)
                    token_ms.append(
                        (time.perf_counter() - token_started) * 1000.0)
                total_ms = (time.perf_counter() - request_started) * 1000.0
                row = {
                    "schemaVersion": "spec166-standalone-gpu-request-v1",
                    "runId": args.run_id,
                    "requestId": request_id,
                    "promptId": prompt_id,
                    "phase": phase,
                    "repetition": repetition,
                    "generatedTokenIds": generated,
                    "expectedTokenIds": reference,
                    "tokenCount": len(generated),
                    "exactMatch": generated == reference,
                    "ttftMs": token_ms[0],
                    "perTokenLatencyMs": token_ms,
                    "totalLatencyMs": total_ms,
                    "tokensPerSecond": 8.0 / (total_ms / 1000.0),
                    "workloadDigest": EXPECTED_WORKLOAD,
                    "modelIdentityDigest": EXPECTED_MODEL,
                }
                print(json.dumps(row, sort_keys=True), flush=True)
                rows.append(row)

    for placement, session in zip(placements, sessions):
        profile = _qwen_onnx_profile_summary(session)
        placement["profilePolicy"] = profile["state"]
        placement["profile"] = profile

    exact_match = len(rows) == 8 and all(row["exactMatch"] for row in rows)
    profile_match = all(
        placement["profilePolicy"] == "PASS"
        for placement in placements
    )
    result = {
        "schemaVersion": "spec166-standalone-gpu-result-v1",
        "state": "PASS" if exact_match and profile_match else "FAIL",
        "runId": args.run_id,
        "requestCount": len(rows),
        "warmupCount": 2,
        "measuredCount": 6,
        "tokensPerRequest": 8,
        "modelLoadMs": model_load_ms,
        "placements": placements,
        "requests": rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if not exact_match:
        raise SystemExit("STANDALONE_REFERENCE_MISMATCH")
    if not profile_match:
        raise SystemExit("STANDALONE_EP_PROFILE_POLICY_MISMATCH")
    print("SPEC166_STANDALONE_GPU_PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

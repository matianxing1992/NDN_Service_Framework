#!/usr/bin/env python3
"""Fail-closed analyzer for the no-model three-node Selection canary."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _complete_response(user_log: str) -> dict:
    """Return the real three-stage response payload emitted by the DI user."""
    for line in user_log.splitlines():
        if "LLM_PIPELINE_USER_RESPONSE" not in line:
            continue
        payload_start = line.find("{")
        if payload_start < 0:
            continue
        try:
            payload = json.loads(line[payload_start:])
        except json.JSONDecodeError:
            continue
        if (
            payload.get("schema") == "ndnsf-di-llm-pipeline-response-v1"
            and payload.get("stageCount") == 3
            and payload.get("finalRole") == "/LLM/Pipeline/Stage/2"
            and payload.get("lineage") == [
                "prompt",
                "/LLM/Pipeline/Stage/0",
                "/LLM/Pipeline/Stage/1",
                "/LLM/Pipeline/Stage/2",
            ]
        ):
            return payload
    raise RuntimeError("CANARY_COMPLETE_RESPONSE_MISSING")


def analyze(root: Path, request_id: str, *, expected_core_digest: str = "",
            expected_extension_digest: str = "") -> dict:
    user_log = (root / "node-0/user.log").read_text(
        encoding="utf-8", errors="replace")
    if user_log.count("NDNSF_SELECTION_PROVIDER_PROJECTION") != 3:
        raise RuntimeError("CANARY_PROVIDER_PROJECTION_COUNT_MISMATCH")
    if request_id not in user_log:
        raise RuntimeError("CANARY_REQUEST_ID_MISSING_FROM_USER_LOG")
    response = _complete_response(user_log)
    forbidden = (
        "LLM_PIPELINE_QWEN_REPO_FETCH",
        "LLM_PIPELINE_QWEN_RUNTIME_READY",
        "LLM_PIPELINE_QWEN_SELECTION_PREPARE",
    )
    if any(marker in user_log for marker in forbidden):
        raise RuntimeError("CANARY_MODEL_WORK_DETECTED_IN_USER_LOG")

    providers = []
    for rank in range(3):
        path = root / f"node-{rank}/provider-{rank}.log"
        log = path.read_text(encoding="utf-8", errors="replace")
        if request_id not in log:
            raise RuntimeError(f"CANARY_PROVIDER_REQUEST_ID_MISSING:rank={rank}")
        if "message=selection received" not in log:
            raise RuntimeError(f"CANARY_SELECTION_NOT_ACCEPTED:rank={rank}")
        if "message=collaboration handler running" not in log:
            raise RuntimeError(f"CANARY_HANDLER_NOT_RUNNING:rank={rank}")
        if any(marker in log for marker in forbidden):
            raise RuntimeError(f"CANARY_MODEL_WORK_DETECTED:rank={rank}")
        providers.append({
            "rank": rank,
            "log": str(path),
            "logDigest": _sha256(path),
            "selectionAccepted": True,
            "handlerRunning": True,
        })

    rank_log = (root / "rank-step.log").read_text(
        encoding="utf-8", errors="replace")
    abi_lines = [line for line in rank_log.splitlines()
                 if "SPEC168_SELECTION_FANOUT_ABI_PASS" in line]
    if len(abi_lines) != 3:
        raise RuntimeError("CANARY_NATIVE_FANOUT_ABI_COUNT_MISMATCH")
    if expected_core_digest and any(
            f"coreSha256={expected_core_digest}" not in line for line in abi_lines):
        raise RuntimeError("CANARY_NATIVE_CORE_DIGEST_MISMATCH")
    if expected_extension_digest and any(
            f"extensionSha256={expected_extension_digest}" not in line
            for line in abi_lines):
        raise RuntimeError("CANARY_NATIVE_EXTENSION_DIGEST_MISMATCH")
    child_lines = [line for line in rank_log.splitlines()
                   if "SPEC168_COMPAT_CHILD_NATIVE_ABI_PASS" in line]
    if len(child_lines) != 3:
        raise RuntimeError("CANARY_COMPAT_CHILD_NATIVE_ABI_COUNT_MISMATCH")
    if expected_core_digest and any(
            f"coreSha256={expected_core_digest}" not in line
            for line in child_lines):
        raise RuntimeError("CANARY_COMPAT_CHILD_CORE_DIGEST_MISMATCH")
    if expected_extension_digest and any(
            f"extensionSha256={expected_extension_digest}" not in line
            for line in child_lines):
        raise RuntimeError("CANARY_COMPAT_CHILD_EXTENSION_DIGEST_MISMATCH")
    return {
        "schema": "ndnsf-di.spec168-control-plane-canary.v1",
        "status": "PASS",
        "requestId": request_id,
        "providerProjectionCount": 3,
        "providerCount": 3,
        "modelWorkCount": 0,
        "globalPreparationBarrier": False,
        "nativeCoreDigest": expected_core_digest,
        "nativeExtensionDigest": expected_extension_digest,
        "compatChildNativeEvidenceCount": len(child_lines),
        "responseSchema": response["schema"],
        "responseStageCount": response["stageCount"],
        "providers": providers,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--request-id", required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--expected-native-core-digest", required=True)
    parser.add_argument("--expected-native-extension-digest", required=True)
    args = parser.parse_args()
    result = analyze(
        args.root, args.request_id,
        expected_core_digest=args.expected_native_core_digest,
        expected_extension_digest=args.expected_native_extension_digest,
    )
    args.output_json.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("SPEC168_CONTROL_PLANE_CANARY_PASS", json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

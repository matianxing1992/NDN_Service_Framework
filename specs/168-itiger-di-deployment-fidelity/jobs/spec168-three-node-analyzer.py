#!/usr/bin/env python3
"""Fail-closed acceptance analyzer for one Spec 168 three-node control."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
from typing import Any


DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")


def require(value: object, code: str) -> None:
    if not value:
        raise RuntimeError(code)


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), f"SPEC168_JSON_OBJECT_REQUIRED:{path.name}")
    return value


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 << 20), b""):
            value.update(block)
    return "sha256:" + value.hexdigest()


def canonical_request_id(value: object) -> str:
    text = str(value or "").strip()
    return "/" + text.lstrip("/") if text else ""


def marker_count(text: str, marker: str) -> int:
    return sum(marker in line for line in text.splitlines())


def ordered_once(text: str, markers: tuple[str, ...]) -> None:
    offsets = []
    for marker in markers:
        require(marker_count(text, marker) == 1,
                f"SPEC168_USER_MARKER_CARDINALITY:{marker}")
        offsets.append(text.index(marker))
    require(offsets == sorted(offsets), "SPEC168_USER_LIFECYCLE_ORDER_INVALID")


def request_lines(text: str, marker: str, request_id: str) -> list[str]:
    expected = canonical_request_id(request_id)
    rows = []
    for line in text.splitlines():
        if marker not in line:
            continue
        match = re.search(r"\brequestId=([^ ]+)", line)
        if match and canonical_request_id(match.group(1)) == expected:
            rows.append(line)
    return rows


def request_epochs(text: str, marker: str, request_id: str) -> set[int]:
    epochs: set[int] = set()
    for line in request_lines(text, marker, request_id):
        match = re.search(r"\bepoch=([0-9]+)\b", line)
        require(match is not None,
                f"SPEC168_DEPENDENCY_EPOCH_MISSING:{marker}")
        epochs.add(int(match.group(1)))
    return epochs


def native_closure_evidence(args: argparse.Namespace, root: Path) -> dict[str, Any]:
    for label, value in (
        ("nativeCoreDigest", args.expected_native_core_digest),
        ("nativeExtensionDigest", args.expected_native_extension_digest),
    ):
        require(DIGEST.fullmatch(str(value)), f"SPEC168_DIGEST_INVALID:{label}")
    text = (root / "rank-step.log").read_text(encoding="utf-8", errors="replace")
    lines = [line for line in text.splitlines()
             if "SPEC168_SELECTION_FANOUT_ABI_PASS" in line]
    require(len(lines) == 3, "SPEC168_NATIVE_FANOUT_ABI_COUNT_MISMATCH")
    for line in lines:
        require(f"coreSha256={args.expected_native_core_digest}" in line,
                "SPEC168_NATIVE_CORE_DIGEST_MISMATCH")
        require(f"extensionSha256={args.expected_native_extension_digest}" in line,
                "SPEC168_NATIVE_EXTENSION_DIGEST_MISMATCH")
    child_lines = [line for line in text.splitlines()
                   if "SPEC168_COMPAT_CHILD_NATIVE_ABI_PASS" in line]
    require(len(child_lines) == 3,
            "SPEC168_COMPAT_CHILD_NATIVE_ABI_COUNT_MISMATCH")
    for line in child_lines:
        require(f"coreSha256={args.expected_native_core_digest}" in line,
                "SPEC168_COMPAT_CHILD_NATIVE_CORE_DIGEST_MISMATCH")
        require(f"extensionSha256={args.expected_native_extension_digest}" in line,
                "SPEC168_COMPAT_CHILD_NATIVE_EXTENSION_DIGEST_MISMATCH")
    return {
        "coreDigest": args.expected_native_core_digest,
        "extensionDigest": args.expected_native_extension_digest,
        "rankEvidenceCount": len(lines),
        "compatChildEvidenceCount": len(child_lines),
    }


def analyze(args: argparse.Namespace) -> dict[str, Any]:
    root = Path(args.root)
    stage_path = Path(args.stage_manifest)
    campaign_path = Path(args.campaign_manifest)
    campaign = read_json(campaign_path)
    bindings = campaign.get("bindingDigests", {})
    require(campaign.get("schema") == "ndnsf-di.spec168-campaign.v3",
            "SPEC168_CAMPAIGN_SCHEMA_INVALID")
    require(campaign.get("state") == "FROZEN", "SPEC168_CAMPAIGN_NOT_FROZEN")
    stage_binding_key = getattr(
        args, "stage_binding_key", "remoteSmallStageManifestDigest")
    for key, observed in (
        ("sourceDigest", args.expected_source_digest),
        ("sourceBundleDigest", args.expected_source_bundle_digest),
        ("runtimeSifDigest", args.expected_sif_digest),
        (stage_binding_key, sha256(stage_path)),
    ):
        require(DIGEST.fullmatch(str(observed)), f"SPEC168_DIGEST_INVALID:{key}")
        require(bindings.get(key) == observed, f"SPEC168_BINDING_MISMATCH:{key}")

    stage_manifest = read_json(stage_path)
    stages = list(stage_manifest.get("stages", []))
    require(len(stages) == 3, "SPEC168_STAGE_COVERAGE_INVALID")
    layer_ranges = stage_manifest.get("layerRanges", [])
    require(len(layer_ranges) == 3, "SPEC168_LAYER_COVERAGE_INVALID")
    model_digest = str(stage_manifest.get("modelDigest", ""))
    if not model_digest.startswith("sha256:"):
        model_digest = "sha256:" + model_digest
    require(DIGEST.fullmatch(model_digest), "SPEC168_MODEL_DIGEST_INVALID")
    workload_digest = str(bindings.get("promptSetDigest", ""))
    native_closure = native_closure_evidence(args, root)

    rows = [json.loads(line) for line in
            (root / "node-0/generation-raw.jsonl").read_text(
                encoding="utf-8").splitlines() if line.strip()]
    require(len(rows) == 1, "SPEC168_SINGLE_INVOCATION_REQUIRED")
    row = rows[0]
    require(row.get("phase") == "measured" and row.get("status") == "OK",
            "SPEC168_GENERATION_FAILED")
    require(row.get("exactReferenceMatch") is True,
            "SPEC168_REFERENCE_MISMATCH")
    require(str(row.get("decodedText", "")).strip(), "SPEC168_ANSWER_MISSING")
    tokens = list(row.get("generatedTokenIds", []))
    require(len(tokens) >= 2, "SPEC168_MULTI_TOKEN_REQUIRED")
    steps = list(row.get("tokenSteps", []))
    require(len(steps) == 1 and steps[0].get("mode") == "FULL",
            "SPEC168_DURABLE_INVOCATION_FRAGMENTED")
    metadata = steps[0].get("metadata", {})
    require(isinstance(metadata, dict), "SPEC168_RESPONSE_METADATA_MISSING")
    request_id = canonical_request_id(args.expected_request_id)
    require(canonical_request_id(metadata.get("requestId")) == request_id,
            "SPEC168_RESPONSE_REQUEST_ID_MISMATCH")
    require(int(metadata.get("wireRequestCount", -1)) == 1,
            "SPEC168_WIRE_REQUEST_COUNT_INVALID")
    require(int(metadata.get("tokenRequestCount", -1)) == 0,
            "SPEC168_TOKEN_REQUEST_COUNT_INVALID")
    require(row.get("modelIdentityDigest") == model_digest,
            "SPEC168_MODEL_IDENTITY_MISMATCH")
    require(row.get("workloadDigest") == workload_digest,
            "SPEC168_WORKLOAD_IDENTITY_MISMATCH")

    user_text = (root / "node-0/user.log").read_text(errors="replace")
    ordered_once(user_text, (
        "SPEC162_REQUEST_GATE_OPEN",
        "NDNSF_DI_AUTOPLANNING_REQUEST_SENT",
        "NDNSF_DI_AUTOPLANNING_ACK_CLOSED",
        "NDNSF_DI_AUTOPLANNING_GRAPH_READY",
        "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED",
        "LLM_PIPELINE_GENERATION_FINAL_RESPONSE",
    ))
    for marker in (
        "SPEC162_REQUEST_GATE_OPEN",
        "NDNSF_DI_AUTOPLANNING_REQUEST_SENT",
        "NDNSF_DI_AUTOPLANNING_ACK_CLOSED",
        "NDNSF_DI_AUTOPLANNING_GRAPH_READY",
        "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED",
    ):
        require(len(request_lines(user_text, marker, request_id)) == 1,
                f"SPEC168_REQUEST_ID_LINEAGE:{marker}")
    require(marker_count(user_text, "LLM_PIPELINE_GENERATION_FINAL_RESPONSE") == 1,
            "SPEC168_RESPONSE_CARDINALITY_INVALID")
    require("UserToken/ProviderToken runtime mode: enabled" in user_text,
            "SPEC168_USER_TOKEN_EVIDENCE_MISSING")
    require("Installed user permission" in user_text,
            "SPEC168_USER_PERMISSION_EVIDENCE_MISSING")

    forbidden = (
        "CPUExecutionProvider", "cpuFallback=true", "LEGACY_READY_SET_V1",
        "ExecutionActivateMessage", "SPEC162_PROVIDER_SETTLE_SECONDS",
        "SPEC162_USER_STARTUP_SETTLE_MS",
    )
    require(not any(item in user_text for item in forbidden),
            "SPEC168_FORBIDDEN_USER_RUNTIME_MARKER")

    nodes: list[str] = []
    gpus: list[dict[str, Any]] = []
    providers: list[dict[str, Any]] = []
    for rank in range(3):
        node_root = root / f"node-{rank}"
        node = (node_root / "hostname.txt").read_text().strip()
        nodes.append(node)
        gpu_row = next(csv.reader([
            (node_root / "gpu.csv").read_text().splitlines()[0]]))
        require(len(gpu_row) >= 4 and "RTX 5000" in gpu_row[1],
                f"SPEC168_GPU_INVALID:rank={rank}")
        gpus.append({"rank": rank, "uuid": gpu_row[0].strip(),
                     "name": gpu_row[1].strip(), "node": node})
        route_text = (node_root / "route-list-after-routes.txt").read_text(
            errors="replace")
        face_text = (node_root / "face-list-after-routes.txt").read_text(
            errors="replace")
        for prefix in ("/NDNSF-DistributeInference/example",
                       "/NDNSF/DistributedRepo", "/activation/llm"):
            require(prefix in route_text,
                    f"SPEC168_ROUTE_MISSING:rank={rank}:{prefix}")
        require("remote=" in face_text, f"SPEC168_FACE_EVIDENCE_MISSING:rank={rank}")

        log_path = node_root / f"provider-{rank}.log"
        text = log_path.read_text(errors="replace")
        marker_path = node_root / f"provider-markers-{rank}.log"
        marker_text = (marker_path.read_text(errors="replace")
                       if marker_path.is_file() else text)
        for marker in (
            "LLM_PIPELINE_PROVIDER_READY",
            "NDNSF_DI_ACK_DECISION",
            "LLM_PIPELINE_QWEN_SELECTION_PREPARE",
            "LLM_PIPELINE_QWEN_FULL_STAGE_START",
            "NDNSF_DI_SELECTION_RESERVATION_RELEASED",
        ):
            require(request_lines(text, marker, request_id) or
                    marker == "LLM_PIPELINE_PROVIDER_READY",
                    f"SPEC168_PROVIDER_MARKER_MISSING:rank={rank}:{marker}")
        require("status=true" in request_lines(
            text, "NDNSF_DI_ACK_DECISION", request_id)[0],
            f"SPEC168_ACK_REJECTED:rank={rank}")
        require("device=cuda:0" in text and "cpuFallback=false" in text,
                f"SPEC168_CUDA_EVIDENCE_MISSING:rank={rank}")
        require(not any(item in text for item in forbidden),
                f"SPEC168_FORBIDDEN_PROVIDER_MARKER:rank={rank}")
        require("NAC_ABE_BOOTSTRAP" in text,
                f"SPEC168_NAC_ABE_EVIDENCE_MISSING:rank={rank}")
        require("Installed provider permission" in text,
                f"SPEC168_PROVIDER_PERMISSION_MISSING:rank={rank}")
        if rank == 0:
            terminal = "LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL"
        elif rank == 1:
            terminal = "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED"
        else:
            terminal = "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED"
        require(terminal in text,
                f"SPEC168_STAGE_TERMINAL_MISSING:rank={rank}")
        if rank > 0:
            received = request_epochs(
                marker_text, "LLM_PIPELINE_QWEN_FULL_HIDDEN_RECEIVED", request_id)
            output_marker = (
                "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED" if rank == 1 else
                "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED")
            produced = request_epochs(marker_text, output_marker, request_id)
            expected_epochs = set(range(len(tokens)))
            require(produced == expected_epochs and produced <= received,
                    f"SPEC168_DEPENDENCY_EVIDENCE_MISSING:rank={rank}")
        providers.append({
            "rank": rank,
            "node": node,
            "role": f"/LLM/Pipeline/Stage/{rank}",
            "gpuUuid": gpu_row[0].strip(),
            "logDigest": sha256(log_path),
            "routeDigest": sha256(node_root / "route-list-after-routes.txt"),
        })

    require(len(set(nodes)) == 3, "SPEC168_DISTINCT_NODES_REQUIRED")
    require(len({item["uuid"] for item in gpus}) == 3,
            "SPEC168_DISTINCT_GPUS_REQUIRED")
    require(not list(root.rglob("*.token")) and not list(root.rglob("*.key")),
            "SPEC168_SECRET_RETAINED")

    planning = read_json(root / "automatic-planning.json")
    plan_digest = str(planning.get("candidateDigest", ""))
    require(DIGEST.fullmatch(plan_digest), "SPEC168_PLAN_DIGEST_INVALID")
    result = {
        "schema": "ndnsf-di.spec168-three-node-analysis.v1",
        "status": "PASS",
        "campaignId": campaign["campaignId"],
        "requestId": request_id,
        "sourceDigest": args.expected_source_digest,
        "sourceBundleDigest": args.expected_source_bundle_digest,
        "runtimeSifDigest": args.expected_sif_digest,
        "nativeClosure": native_closure,
        "stageManifestDigest": sha256(stage_path),
        "modelIdentityDigest": model_digest,
        "workloadDigest": workload_digest,
        "planDigest": plan_digest,
        "wireRequestCount": 1,
        "tokenRequestCount": 0,
        "responseCount": 1,
        "generatedTokenCount": len(tokens),
        "stopReason": row.get("stopReason"),
        "answer": row["decodedText"],
        "generatedTokenIds": tokens,
        "cpuFallbackCount": 0,
        "readiness": "REQUEST_FIRST_DATA_DRIVEN_V2",
        "nodes": nodes,
        "gpus": gpus,
        "providers": providers,
        "securityVerdict": "PASS",
    }
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--stage-manifest", required=True)
    parser.add_argument("--campaign-manifest", required=True)
    parser.add_argument("--expected-source-digest", required=True)
    parser.add_argument("--expected-source-bundle-digest", required=True)
    parser.add_argument("--expected-sif-digest", required=True)
    parser.add_argument("--expected-native-core-digest", required=True)
    parser.add_argument("--expected-native-extension-digest", required=True)
    parser.add_argument("--expected-request-id", required=True)
    parser.add_argument(
        "--stage-binding-key",
        choices=("remoteSmallStageManifestDigest", "remoteLargeStageManifestDigest"),
        default="remoteSmallStageManifestDigest",
    )
    parser.add_argument("--output-json", required=True)
    args = parser.parse_args()
    try:
        result = analyze(args)
    except Exception as error:
        failure = {
            "schema": "ndnsf-di.spec168-three-node-analysis.v1",
            "status": "FAIL",
            "failure": f"{type(error).__name__}:{error}",
        }
        Path(args.output_json).write_text(
            json.dumps(failure, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        print(json.dumps(failure, sort_keys=True))
        return 1
    Path(args.output_json).write_text(
        json.dumps(result, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8")
    print(json.dumps(result, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

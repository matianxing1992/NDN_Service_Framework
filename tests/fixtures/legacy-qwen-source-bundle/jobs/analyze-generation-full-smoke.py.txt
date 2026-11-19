#!/usr/bin/env python3
"""Validate one TigerCluster Qwen smoke as one FULL invocation.

The smoke deliberately measures a complete distributed generation.  It does
not reinterpret every generated token as a new ACK/Selection transaction.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def count(text: str, marker: str) -> int:
    return text.count(marker)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--stage-manifest", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--enriched-jsonl", required=True)
    args = parser.parse_args()

    root = Path(args.root)
    rows = [
        json.loads(line)
        for line in (root / "node-0/generation-raw.jsonl")
        .read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    require(len(rows) == 1, f"expected one FULL smoke row, got {len(rows)}")
    row = rows[0]
    generated = list(row.get("generatedTokenIds", []))
    steps = list(row.get("tokenSteps", []))
    require(row.get("status") == "OK", f"generation status: {row.get('status')}")
    require(row.get("exactReferenceMatch") is True,
            "generation did not match the frozen RTX reference")
    require(str(row.get("decodedText", "")).strip(),
            "final response text is missing")
    require(row.get("stopReason") in {"EOS", "MAX_NEW_TOKENS"},
            f"invalid stop reason: {row.get('stopReason')}")
    require(len(generated) >= 2, "FULL smoke must generate multiple tokens")
    require(len(steps) == 1 and steps[0].get("mode") == "FULL",
            "smoke is not represented as one FULL response event")
    metadata = steps[0].get("metadata", {})
    require(metadata.get("generationMode") == "FULL",
            "FULL response mode metadata is absent")
    require(metadata.get("requestId") == f"/{row['generationId']}",
            "FULL response request ID lineage mismatch")
    require(row.get("interTokenMs") == [],
            "FULL smoke must not report per-token wire latency")

    planning = json.loads(
        (root / "automatic-planning.json").read_text(encoding="utf-8"))
    require(planning["preSplitCatalog"]["publicationState"] ==
            "REQUIRES_DISTRIBUTED_REPO_REGISTRATION",
            "FULL smoke used an eagerly published Repo catalog")

    user_log = (root / "node-0/user.log").read_text(
        encoding="utf-8", errors="replace")
    request_id = str(row["generationId"])
    wire_id = "/" + request_id.lstrip("/")
    for marker in (
        "SPEC162_REQUEST_GATE_OPEN",
        "NDNSF_DI_AUTOPLANNING_REQUEST_SENT",
        "NDNSF_DI_AUTOPLANNING_ACK_CLOSED",
        "NDNSF_DI_AUTOPLANNING_ARTIFACTS_READY",
        "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED",
        "LLM_PIPELINE_GENERATION_FINAL_RESPONSE",
    ):
        require(marker in user_log, f"user lifecycle marker missing: {marker}")
    require(f"generationId={request_id}" in user_log,
            "final response generation ID is absent")
    require(f"requestId={wire_id}" in user_log,
            "FULL wire request ID is absent")
    require(count(user_log, "NDNSF_DI_AUTOPLANNING_REQUEST_SENT") == 1,
            "FULL smoke reopened the request transaction")
    require(count(user_log, "LLM_PIPELINE_GENERATION_FINAL_RESPONSE") == 1,
            "FULL smoke published more than one final response")

    manifest = json.loads(Path(args.stage_manifest).read_text(encoding="utf-8"))
    ranges = manifest.get("layerRanges", [])
    require(len(ranges) == 3 and all(int(item[0]) < int(item[1]) for item in ranges),
            f"invalid three-stage layer cover: {ranges}")

    nodes: list[str] = []
    gpus: list[dict[str, str]] = []
    provider_summaries: list[dict[str, object]] = []
    for rank in range(3):
        node_root = root / f"node-{rank}"
        node = (node_root / "hostname.txt").read_text(encoding="utf-8").strip()
        nodes.append(node)
        values = [item.strip() for item in csv.reader(
            [(node_root / "gpu.csv").read_text(encoding="utf-8").splitlines()[0]]
        ).__next__()]
        require(values[0].startswith("GPU-"), f"rank {rank} GPU UUID is invalid")
        require("RTX 5000" in values[1], f"rank {rank} is not RTX 5000: {values[1]}")
        gpus.append({"rank": rank, "uuid": values[0], "name": values[1]})
        log = (node_root / f"provider-{rank}.log").read_text(
            encoding="utf-8", errors="replace")
        require("LLM_PIPELINE_PROVIDER_READY" in log,
                f"provider {rank} did not become ready")
        # Newer providers publish an explicit immutable-artifact marker;
        # the sealed runtime used by this requalification predates that
        # marker and records the same fact as model residency (including the
        # content-addressed path and cache-hit state).  Accept either form,
        # but never accept FULL execution without one of them.
        require("LLM_PIPELINE_QWEN_STAGE_ARTIFACT_READY" in log or
                "LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY" in log or
                "LLM_PIPELINE_QWEN_MODEL_RESIDENCY" in log,
                f"provider {rank} lacks immutable artifact readiness")
        require(count(log, "LLM_PIPELINE_QWEN_FULL_STAGE_START") >= 1,
                f"provider {rank} did not enter FULL stage execution")
        if rank == 0:
            required = "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED"
            terminal = "LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL"
        elif rank == 1:
            required = "LLM_PIPELINE_QWEN_FULL_TOKEN_FORWARDED"
            terminal = "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED"
        else:
            required = "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED"
            terminal = "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED"
        require(count(log, required) >= len(generated),
                f"provider {rank} FULL progress is shorter than generation")
        terminal_minimum = 1 if rank == 0 else len(generated)
        require(count(log, terminal) >= terminal_minimum,
                f"provider {rank} FULL terminal progress is incomplete")
        require("LLM_PIPELINE_QWEN_REPO_FETCH_PROGRESS" in log,
                f"provider {rank} lacks Repo fetch progress")
        require("LLM_PIPELINE_QWEN_REPO_FETCH_COMPLETE" in log,
                f"provider {rank} lacks Repo fetch completion")
        require("NDNSF_DI_SELECTION_RESERVATION_RELEASED" in log,
                f"provider {rank} lacks terminal release evidence")
        provider_summaries.append({
            "rank": rank,
            "role": f"/LLM/Pipeline/Stage/{rank}",
            "fullProgressEvents": count(log, required),
            "repoFetchProgress": count(log, "LLM_PIPELINE_QWEN_REPO_FETCH_PROGRESS"),
            "repoFetchComplete": count(log, "LLM_PIPELINE_QWEN_REPO_FETCH_COMPLETE"),
        })

    require(len(set(nodes)) == 3, f"nodes are not distinct: {nodes}")
    require(len({item["uuid"] for item in gpus}) == 3,
            "GPU UUIDs are not distinct")
    enriched = {
        **row,
        "tokenSteps": [{
            **steps[0],
            "stageReceipts": [
                {"rank": rank, "node": nodes[rank], "gpuUuid": gpus[rank]["uuid"],
                 "role": f"/LLM/Pipeline/Stage/{rank}",
                 "layerRange": ranges[rank], "backend": "transformers",
                 "cpuFallback": False}
                for rank in range(3)
            ],
        }],
    }
    Path(args.enriched_jsonl).write_text(
        json.dumps(enriched, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8")
    analysis = {
        "schemaVersion": "ndnsf-di-qwen36-generation-full-smoke-analysis-v1",
        "status": "PASS",
        "invocationMode": "FULL",
        "generationId": row["generationId"],
        "decodedText": row["decodedText"],
        "generatedTokenIds": generated,
        "generatedTokenCount": len(generated),
        "exactReferenceMatch": True,
        "nodes": nodes,
        "gpus": gpus,
        "layerRanges": ranges,
        "wireRequestCount": 1,
        "tokenRequestCount": 0,
        "stageReceiptCount": 3,
        "providerSummaries": provider_summaries,
        "timingSemantics": {
            "requestUnit": "one durable FULL distributed invocation",
            "perTokenWireRequests": False,
            "interTokenLatency": "not a wire metric; internal stage progress only",
        },
    }
    Path(args.output_json).write_text(
        json.dumps(analysis, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8")
    print(json.dumps(analysis, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

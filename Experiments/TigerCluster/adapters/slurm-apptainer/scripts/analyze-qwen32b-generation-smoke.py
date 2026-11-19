#!/usr/bin/env python3
"""Correlate one Spec 161 generation with three stages and two dependencies."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path
from typing import Any


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def marker_fields(text: str, marker: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for line in text.splitlines():
        if marker not in line:
            continue
        tail = line.split(marker, 1)[1].strip()
        rows.append(dict(
            item.split("=", 1) for item in tail.split() if "=" in item
        ))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--stage-manifest", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--enriched-jsonl", required=True)
    args = parser.parse_args()

    root = Path(args.root)
    stage_manifest = json.loads(
        Path(args.stage_manifest).read_text(encoding="utf-8"))
    ranges = [list(item) for item in stage_manifest["layerRanges"]]
    require(ranges == [[0, 21], [21, 42], [42, 64]],
            f"unexpected layer ranges: {ranges}")
    raw_rows = [
        json.loads(line)
        for line in (root / "node-0/generation-raw.jsonl")
        .read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    require(len(raw_rows) == 1, f"expected one smoke row, got {len(raw_rows)}")
    row = raw_rows[0]
    require(row.get("status") == "OK", f"generation status: {row.get('status')}")
    require(row.get("stopReason") == "EOS",
            f"generation stop reason: {row.get('stopReason')}")
    require(row.get("exactReferenceMatch") is True,
            "generation did not match the H100 reference")
    steps = list(row.get("tokenSteps", []))
    generated = list(row.get("generatedTokenIds", []))
    require(steps and len(steps) == len(generated),
            "token step count does not match generated tokens")
    request_ids = [str(step.get("requestId", "")) for step in steps]
    require(all(request_ids) and len(set(request_ids)) == len(request_ids),
            "token request IDs are missing or duplicated")

    nodes = [
        (root / f"node-{rank}/hostname.txt").read_text().strip()
        for rank in range(3)
    ]
    require(len(set(nodes)) == 3, f"nodes are not distinct: {nodes}")
    gpus = []
    for rank in range(3):
        values = next(csv.reader([
            (root / f"node-{rank}/gpu.csv").read_text().splitlines()[0]
        ]))
        values = [item.strip() for item in values]
        require(values[0].startswith("GPU-"),
                f"rank {rank} GPU UUID is invalid")
        require("RTX 5000" in values[1],
                f"rank {rank} is not RTX 5000: {values[1]}")
        gpus.append({"rank": rank, "uuid": values[0], "name": values[1]})
    require(len({item["uuid"] for item in gpus}) == 3,
            "GPU UUIDs are not distinct")

    timings_by_rank: list[dict[str, dict[str, str]]] = []
    fetches_by_rank: list[list[dict[str, str]]] = []
    for rank in range(3):
        log = (root / f"node-{rank}/provider-{rank}.log").read_text(
            encoding="utf-8", errors="replace")
        require("LLM_PIPELINE_PROVIDER_READY" in log,
                f"provider {rank} did not become ready")
        timings = marker_fields(log, "LLM_PIPELINE_QWEN_STAGE_TIMING")
        require(len(timings) == len(steps),
                f"provider {rank} timing count {len(timings)} != {len(steps)}")
        mapping = {item.get("requestId", ""): item for item in timings}
        require(set(mapping) == set(request_ids),
                f"provider {rank} request IDs do not match generation")
        for item in timings:
            require(item.get("role") == f"/LLM/Pipeline/Stage/{rank}",
                    f"provider {rank} role mismatch")
            require(item.get("stage") == str(rank),
                    f"provider {rank} stage mismatch")
            require(item.get("device", "").startswith("cuda"),
                    f"provider {rank} did not execute on CUDA")
            require(item.get("cpuFallback", "").lower() in {"0", "false"},
                    f"provider {rank} used CPU fallback")
            for key in ("input_sha256", "output_sha256"):
                require(re.fullmatch(r"[0-9a-f]{64}", item.get(key, "")) is not None,
                        f"provider {rank} missing {key}")
        timings_by_rank.append(mapping)
        fetches_by_rank.append(marker_fields(
            log, "NDNSF_COLLAB_LARGE_FETCH_TIMING event=complete"))

    enriched_steps = []
    for step in steps:
        request_id = str(step["requestId"])
        timings = [mapping[request_id] for mapping in timings_by_rank]
        require(timings[0]["output_sha256"] == timings[1]["input_sha256"],
                f"stage 0/1 digest mismatch for {request_id}")
        require(timings[1]["output_sha256"] == timings[2]["input_sha256"],
                f"stage 1/2 digest mismatch for {request_id}")
        require(timings[0]["dataName"] not in {"", "-"},
                f"stage 0 data name missing for {request_id}")
        require(timings[1]["dataName"] not in {"", "-"},
                f"stage 1 data name missing for {request_id}")
        require(timings[0]["dataName"] != timings[1]["dataName"],
                f"dependency names collide for {request_id}")
        require(timings[2]["dataName"] == "-",
                f"final stage unexpectedly published dependency for {request_id}")
        dependencies = []
        for producer, consumer in ((0, 1), (1, 2)):
            data_name = timings[producer]["dataName"]
            matches = [
                item for item in fetches_by_rank[consumer]
                if item.get("dataName") == data_name
            ]
            require(len(matches) == 1,
                    f"dependency fetch evidence missing for {request_id} "
                    f"{producer}->{consumer}")
            dependencies.append({
                "producerStage": producer,
                "consumerStage": consumer,
                "dataName": data_name,
                "sha256": timings[producer]["output_sha256"],
                "encodedBytes": int(matches[0].get("encoded_bytes", "0")),
                "receivedSegments": int(
                    matches[0].get("received_segments", "0")),
                "validatedSegments": int(
                    matches[0].get("validated_segments", "0")),
                "receivedWireBytes": int(
                    matches[0].get("received_wire_bytes", "0")),
            })
        enriched_steps.append({
            **step,
            "stageReceipts": [
                {
                    "rank": rank,
                    "node": nodes[rank],
                    "gpuUuid": gpus[rank]["uuid"],
                    "role": f"/LLM/Pipeline/Stage/{rank}",
                    "layerRange": ranges[rank],
                    "backend": "transformers",
                    "device": timings[rank]["device"],
                    "cpuFallback": False,
                    "inputSha256": timings[rank]["input_sha256"],
                    "outputSha256": timings[rank]["output_sha256"],
                }
                for rank in range(3)
            ],
            "dependencyReceipts": dependencies,
        })

    enriched = {**row, "tokenSteps": enriched_steps}
    Path(args.enriched_jsonl).write_text(
        json.dumps(enriched, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    analysis = {
        "schemaVersion": "ndnsf-di-qwen32b-generation-smoke-analysis-v1",
        "status": "PASS",
        "promptId": row["promptId"],
        "decodedText": row["decodedText"],
        "generatedTokenIds": generated,
        "generatedTokenCount": len(generated),
        "exactReferenceMatch": True,
        "stopReason": "EOS",
        "nodes": nodes,
        "gpus": gpus,
        "layerRanges": ranges,
        "tokenRequestCount": len(request_ids),
        "stageReceiptCount": len(request_ids) * 3,
        "dependencyReceiptCount": len(request_ids) * 2,
        "cpuFallbackCount": 0,
    }
    Path(args.output_json).write_text(
        json.dumps(analysis, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(analysis, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

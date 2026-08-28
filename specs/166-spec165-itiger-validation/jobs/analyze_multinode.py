#!/usr/bin/env python3
"""Fail-closed validator for the Spec 166 three-node Qwen campaign."""

from __future__ import annotations

import csv
import json
import re
import sys
from pathlib import Path


EXPECTED_WORKLOAD = (
    "sha256:2d2aac62e9340ed401b7e7579d992f7fe652de06ee0829e0d8d0ea4c653a7ae9"
)
EXPECTED_MODEL = (
    "sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def marker_fields(log: str, marker: str) -> list[dict[str, str]]:
    rows = []
    for line in log.splitlines():
        if marker not in line:
            continue
        tail = line.split(marker, 1)[1].strip()
        rows.append(dict(
            item.split("=", 1) for item in tail.split() if "=" in item))
    return rows


def main() -> int:
    root = Path(sys.argv[1])
    output = Path(sys.argv[2])
    nodes = [
        line.strip() for line in (root / "nodes.txt").read_text().splitlines()
        if line.strip()
    ]
    require(nodes == ["itiger07", "itiger08", "itiger09"],
            f"unexpected node allocation: {nodes}")
    gpus = []
    timing_by_rank = []
    expected_roles = [f"/LLM/Pipeline/Stage/{rank}" for rank in range(3)]
    forbidden = re.compile(
        r"double free|invalid pointer|munmap_chunk|Aborted|core dumped|"
        r"QWEN_STAGE_CPU_FALLBACK_FORBIDDEN|QWEN_STAGE_CUDA_UNAVAILABLE|"
        r"reportOperationStatus.*incompatible function arguments",
        re.IGNORECASE,
    )
    for rank in range(3):
        gpu_row = next(csv.reader([
            (root / f"node-{rank}/gpu.csv").read_text().splitlines()[0]]))
        gpu_row = [value.strip() for value in gpu_row]
        require(len(gpu_row) >= 2 and gpu_row[0].startswith("GPU-"),
                f"rank {rank} invalid GPU evidence")
        require("RTX 5000" in gpu_row[1],
                f"rank {rank} is not RTX 5000: {gpu_row[1]}")
        gpus.append({"rank": rank, "uuid": gpu_row[0], "name": gpu_row[1]})
        log_path = root / f"node-{rank}/provider-{rank}.log"
        log = log_path.read_text(encoding="utf-8", errors="replace")
        require(not forbidden.search(log),
                f"rank {rank} contains forbidden runtime marker")
        require("LLM_PIPELINE_PROVIDER_READY" in log,
                f"rank {rank} provider did not become ready")
        timings = marker_fields(log, "LLM_PIPELINE_QWEN_STAGE_TIMING")
        require(len(timings) == 64,
                f"rank {rank} expected 64 token executions, got {len(timings)}")
        for timing in timings:
            require(timing.get("role") == expected_roles[rank],
                    f"rank {rank} role mismatch")
            require(int(timing.get("stage", "-1")) == rank,
                    f"rank {rank} stage mismatch")
            require(timing.get("device") == "cuda:0",
                    f"rank {rank} did not execute on CUDA")
            require(timing.get("cpuFallback", "").lower() in {"0", "false"},
                    f"rank {rank} used CPU fallback")
            require(re.fullmatch(r"[0-9a-f]{64}",
                                 timing.get("input_sha256", "")) is not None,
                    f"rank {rank} missing input digest")
            require(re.fullmatch(r"[0-9a-f]{64}",
                                 timing.get("output_sha256", "")) is not None,
                    f"rank {rank} missing output digest")
        timing_by_rank.append({
            row["requestId"]: row for row in timings
        })
    require(len({gpu["uuid"] for gpu in gpus}) == 3,
            "three ranks did not use three distinct GPUs")
    request_sets = [set(rows) for rows in timing_by_rank]
    require(all(items == request_sets[0] for items in request_sets[1:]),
            "request IDs differ across stages")
    require(len(request_sets[0]) == 64,
            "distributed campaign does not contain 64 unique token requests")
    for request_id in sorted(request_sets[0]):
        stage0 = timing_by_rank[0][request_id]
        stage1 = timing_by_rank[1][request_id]
        stage2 = timing_by_rank[2][request_id]
        require(stage0["output_sha256"] == stage1["input_sha256"],
                f"stage 0->1 digest mismatch for {request_id}")
        require(stage1["output_sha256"] == stage2["input_sha256"],
                f"stage 1->2 digest mismatch for {request_id}")
        require(stage0.get("dataName", "-") != "-",
                f"stage 0 missing Data name for {request_id}")
        require(stage1.get("dataName", "-") != "-",
                f"stage 1 missing Data name for {request_id}")

    generation_path = root / "node-0/generation.jsonl"
    samples = [
        json.loads(line) for line in generation_path.read_text(
            encoding="utf-8").splitlines() if line.strip()
    ]
    require(len(samples) == 8, f"expected 8 generation rows, got {len(samples)}")
    require(sum(row["phase"] == "warmup" for row in samples) == 2,
            "warmup row count mismatch")
    require(sum(row["phase"] == "measured" for row in samples) == 6,
            "measured row count mismatch")
    lineage = []
    for row in samples:
        require(row["status"] == "OK" and row["exactReferenceMatch"],
                f"generation mismatch: {row['generationId']}")
        require(len(row["generatedTokenIds"]) == 8,
                f"token count mismatch: {row['generationId']}")
        require(len(row["tokenSteps"]) == 8,
                f"token-step count mismatch: {row['generationId']}")
        require(row["workloadDigest"] == EXPECTED_WORKLOAD,
                "workload digest mismatch")
        require(row["modelIdentityDigest"] == EXPECTED_MODEL,
                "model identity digest mismatch")
        for step in row["tokenSteps"]:
            transport = step.get("transport", {})
            require(step["status"] == "OK", "token step failed")
            require(transport.get("wireRequestId") == step["requestId"],
                    "wire request ID mismatch")
            require(int(transport.get("attempt", 0)) == 1,
                    "token request did not use attempt 1")
            lineage.append(step["requestId"])
    require(len(lineage) == 64 and len(set(lineage)) == 64,
            "request lineage is not exactly-once")
    require(set(lineage) == request_sets[0],
            "user and provider request-ID sets differ")
    require((root / "node-0/user-exit.txt").read_text().strip() == "0",
            "user process exit status is nonzero")

    result = {
        "schemaVersion": "spec166-multinode-analysis-v1",
        "state": "PASS",
        "nodes": nodes,
        "gpus": gpus,
        "requestCount": 8,
        "tokenRequestCount": 64,
        "warmupCount": 2,
        "measuredCount": 6,
        "tokensPerRequest": 8,
        "workloadDigest": EXPECTED_WORKLOAD,
        "modelIdentityDigest": EXPECTED_MODEL,
        "generationJsonl": "node-0/generation.jsonl",
        "requestIds": sorted(lineage),
    }
    output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print("SPEC166_MULTINODE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

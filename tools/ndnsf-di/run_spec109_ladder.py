#!/usr/bin/env python3
"""Build a complete keyed Qwen ladder ledger and apply scoped gates."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Iterable

from spec109_matrix import apply_gate, validate_matrix


CELL_MODES = (
    "transfer", "oracle-token-1", "oracle-token-2", "oracle-token-32",
    "artifact-validation", "staged-baseline-perf-1", "candidate-perf-1",
    "staged-baseline-perf-2", "candidate-perf-2",
    "staged-baseline-perf-3", "candidate-perf-3",
    "candidate-token-1", "candidate-token-2", "candidate-token-32", "verdict",
)


def digest(value: object) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def build_matrix(sizes: Iterable[str], campaign_gate_digest: str) -> dict:
    sizes = list(sizes)
    cells = {}
    for size in sizes:
        for mode in CELL_MODES:
            cell_id = f"{size}:{mode}"
            repetition = next((int(mode[-1]) for suffix in ("-1", "-2", "-3")
                               if mode.endswith(suffix)), 0)
            cells[cell_id] = {
                "candidateId": f"spec109-{size.lower()}-pending",
                "modelSize": size, "mode": mode, "repetition": repetition,
                "state": "NOT_STARTED", "reasonCode": "", "runId": None,
                "evidenceDigest": None, "gateScope": "none", "gateId": None,
                "gateDigest": None,
            }
    value = {
        "schemaVersion": "2.0", "campaignId": "spec109-qwen-ladder-v1",
        "campaignGateDigest": campaign_gate_digest, "locked": True,
        "finalized": False, "models": sizes, "cells": cells, "runs": {},
        "physicalProduction": "DEFERRED",
    }
    validate_matrix(value)
    return value


def block_matrix(value: dict, gate_digest: str, reason: str) -> dict:
    first = next(iter(value["cells"]))
    result = apply_gate(value, source_cell=first, scope="systemic",
                        gate_id=reason, gate_digest=gate_digest)
    result["finalized"] = True
    validate_matrix(result)
    result["matrixDigest"] = digest({k: v for k, v in result.items()
                                      if k != "matrixDigest"})
    return result


def partition_layers(layer_count: int, gpu_bytes: list[int], bytes_per_layer: int) -> list[dict]:
    if layer_count <= 0 or bytes_per_layer <= 0 or not gpu_bytes or any(v <= 0 for v in gpu_bytes):
        raise ValueError("PARTITION_INPUT_INVALID")
    capacities = [value // bytes_per_layer for value in gpu_bytes]
    if sum(capacities) < layer_count:
        raise ValueError("PARTITION_MEMORY_INSUFFICIENT")
    weights = [value / sum(gpu_bytes) for value in gpu_bytes]
    counts = [min(capacities[i], int(layer_count * weights[i])) for i in range(len(gpu_bytes))]
    while sum(counts) < layer_count:
        choices = [i for i in range(len(counts)) if counts[i] < capacities[i]]
        if not choices:
            raise ValueError("PARTITION_MEMORY_INSUFFICIENT")
        index = min(choices, key=lambda i: counts[i] / max(weights[i], 1e-9))
        counts[index] += 1
    rows = []
    start = 0
    for index, count in enumerate(counts):
        rows.append({"gpu": index, "startLayer": start, "endLayer": start + count,
                     "layerCount": count, "estimatedBytes": count * bytes_per_layer})
        start += count
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate", required=True)
    parser.add_argument("--sizes", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    gate = json.loads(Path(args.gate).read_text(encoding="utf-8"))
    gate_digest = gate.get("observationDigest") or gate.get("gateDigest")
    matrix = build_matrix(args.sizes.split(","), gate_digest)
    if gate.get("status") != "PASS":
        matrix = block_matrix(matrix, gate_digest, str(gate.get("reasonCode")))
    target = Path(args.output)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(matrix, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"status": "PASS", "cells": len(matrix["cells"]),
                      "finalized": matrix["finalized"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

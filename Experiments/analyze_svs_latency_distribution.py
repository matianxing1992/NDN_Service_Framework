#!/usr/bin/env python3
"""Verify and analyze Spec 140 raw delivery-latency distributions."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any, Iterable


PEERS = ("peer-a", "peer-b")
MODES = ("face-inline-rsa", "worker-rsa")
EXPECTED_SCHEMA = "spec140.peer-summary.v1"


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def nearest_rank(values: Iterable[int], percentile: int) -> int:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("latency sample population is empty")
    rank = (len(ordered) * percentile + 99) // 100
    return ordered[max(1, rank) - 1]


def distribution(values: Iterable[int]) -> dict[str, int]:
    samples = list(values)
    if not samples:
        raise ValueError("latency sample population is empty")
    if any(value < 0 for value in samples):
        raise ValueError("latency samples must be non-negative")
    return {
        "deliverySamples": len(samples),
        "deliveryMeanNs": sum(samples) // len(samples),
        "deliveryP50Ns": nearest_rank(samples, 50),
        "deliveryP95Ns": nearest_rank(samples, 95),
        "deliveryP99Ns": nearest_rank(samples, 99),
    }


def load_samples(path: Path) -> list[int]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["latencyNs"]:
            raise ValueError(f"{path}: expected one latencyNs column")
        values = []
        for line, row in enumerate(reader, 2):
            raw = row.get("latencyNs", "")
            if raw is None or not raw.isdigit():
                raise ValueError(f"{path}:{line}: invalid latencyNs")
            values.append(int(raw))
    if not values:
        raise ValueError(f"{path}: no delivery samples")
    return values


def validate_peer(
    summary: dict[str, Any], samples: list[int], *, peer: str
) -> tuple[dict[str, int], list[str]]:
    errors = []
    if summary.get("schema") != EXPECTED_SCHEMA:
        errors.append(f"{peer}:legacy or unexpected peer-summary schema")
    recomputed = distribution(samples)
    if summary.get("deliveredMeasured") != len(samples):
        errors.append(
            f"{peer}:deliveredMeasured={summary.get('deliveredMeasured')} "
            f"but raw sample count={len(samples)}"
        )
    for key, expected in recomputed.items():
        if summary.get(key) != expected:
            errors.append(
                f"{peer}:{key}={summary.get(key)!r}, recomputed={expected}"
            )
    return recomputed, errors


def analyze(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    manifest = load_json(campaign / "campaign-manifest.json")
    terminals = load_json(campaign / "campaign-terminals.json")
    if manifest.get("schema") != "spec140.campaign.v1":
        raise ValueError("legacy or unexpected campaign schema")
    expected_matrix = [
        {"ordinal": 1, "mode": "face-inline-rsa", "ratePerPeer": 400},
        {"ordinal": 2, "mode": "worker-rsa", "ratePerPeer": 400},
    ]
    if manifest.get("matrix") != expected_matrix:
        raise ValueError("Spec 140 matrix is not the exact two-cell contract")
    if len(terminals) != 2:
        raise ValueError("Spec 140 requires exactly two terminal receipts")

    errors: list[str] = []
    cells = []
    for terminal in terminals:
        cell = campaign / terminal["cellId"]
        mode = terminal["mode"]
        if terminal.get("schema") != "spec140.cell-terminal.v1":
            errors.append(f"{cell.name}:unexpected terminal schema")
        combined: list[int] = []
        peers = []
        for peer in PEERS:
            summary_path = cell / f"{peer}-summary.json"
            sample_path = cell / f"{peer}-delivery-latency.csv"
            summary = load_json(summary_path)
            samples = load_samples(sample_path)
            stats, peer_errors = validate_peer(summary, samples, peer=peer)
            errors.extend(f"{cell.name}:{error}" for error in peer_errors)
            attempted = int(summary.get("attemptedMeasured", 0))
            attempted_pps = attempted / 60.0
            if not 392.0 <= attempted_pps <= 408.0:
                errors.append(
                    f"{cell.name}:{peer}:attempted pps={attempted_pps:.3f}"
                )
            combined.extend(samples)
            peers.append(
                {
                    "peer": peer,
                    **stats,
                    "attemptedMeasured": attempted,
                    "deliveredMeasured": int(summary.get("deliveredMeasured", 0)),
                    "samplePath": str(sample_path),
                    "sampleSha256": sha256(sample_path),
                    "summaryPath": str(summary_path),
                    "summarySha256": sha256(summary_path),
                }
            )
        combined_stats = distribution(combined)
        attempted_total = sum(row["attemptedMeasured"] for row in peers)
        delivered_total = sum(row["deliveredMeasured"] for row in peers)
        cells.append(
            {
                "cellId": terminal["cellId"],
                "mode": mode,
                "terminalStatus": terminal["status"],
                "attemptedPpsPerPeer": attempted_total / 120.0,
                "deliveredPpsPerPeer": delivered_total / 120.0,
                "deliveryRatio": (
                    delivered_total / attempted_total if attempted_total else 0.0
                ),
                **combined_stats,
                "peers": peers,
            }
        )

    result = {
        "schema": "spec140.latency-distribution-analysis.v1",
        "campaign": str(campaign),
        "campaignManifestSha256": sha256(campaign / "campaign-manifest.json"),
        "status": "PASS" if not errors else "FAIL",
        "errors": errors,
        "cells": cells,
    }
    return result


def write_outputs(campaign: Path, result: dict[str, Any]) -> None:
    output_json = campaign / "latency-distribution.json"
    output_json.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    fields = [
        "cellId",
        "mode",
        "attemptedPpsPerPeer",
        "deliveredPpsPerPeer",
        "deliveryRatio",
        "deliverySamples",
        "deliveryMeanMs",
        "deliveryP50Ms",
        "deliveryP95Ms",
        "deliveryP99Ms",
    ]
    rows = []
    for cell in result["cells"]:
        rows.append(
            {
                "cellId": cell["cellId"],
                "mode": cell["mode"],
                "attemptedPpsPerPeer": cell["attemptedPpsPerPeer"],
                "deliveredPpsPerPeer": cell["deliveredPpsPerPeer"],
                "deliveryRatio": cell["deliveryRatio"],
                "deliverySamples": cell["deliverySamples"],
                "deliveryMeanMs": cell["deliveryMeanNs"] / 1_000_000,
                "deliveryP50Ms": cell["deliveryP50Ns"] / 1_000_000,
                "deliveryP95Ms": cell["deliveryP95Ns"] / 1_000_000,
                "deliveryP99Ms": cell["deliveryP99Ns"] / 1_000_000,
            }
        )
    with (campaign / "latency-distribution.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    lines = [
        "# Spec 140 Delivery-Latency Distribution",
        "",
        f"- Status: `{result['status']}`",
        "- Evidence class: fresh two-cell descriptive diagnostic",
        "- Percentiles: nearest-rank over concatenated raw peer samples",
        "",
        "| Mode | Attempted pps/peer | Delivered pps/peer | Delivery | Samples | Mean ms | p50 ms | p95 ms | p99 ms |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['mode']} | {row['attemptedPpsPerPeer']:.2f} | "
            f"{row['deliveredPpsPerPeer']:.2f} | "
            f"{row['deliveryRatio']:.4f} | {row['deliverySamples']} | "
            f"{row['deliveryMeanMs']:.3f} | {row['deliveryP50Ms']:.3f} | "
            f"{row['deliveryP95Ms']:.3f} | {row['deliveryP99Ms']:.3f} |"
        )
    if result["errors"]:
        lines.extend(["", "## Validation errors", ""])
        lines.extend(f"- {error}" for error in result["errors"])
    (campaign / "latency-distribution.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    args = parser.parse_args()
    result = analyze(args.campaign)
    write_outputs(args.campaign.resolve(), result)
    print(args.campaign.resolve() / "latency-distribution.md")
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Analyze the Spec 141 600/800 pps raw latency distributions."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any

try:
    from . import analyze_svs_latency_distribution as base
    from . import NDN_SVS_Latency_Rate_Extension_Minindn as runner
except ImportError:
    import analyze_svs_latency_distribution as base
    import NDN_SVS_Latency_Rate_Extension_Minindn as runner


def load_samples_allow_empty(path: Path) -> list[int]:
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
    return values


def zero_distribution() -> dict[str, Any]:
    return {
        "deliverySamples": 0,
        "deliveryMeanNs": None,
        "deliveryP50Ns": None,
        "deliveryP95Ns": None,
        "deliveryP99Ns": None,
    }


def analyze(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    manifest = base.load_json(campaign / "campaign-manifest.json")
    terminals = base.load_json(campaign / "campaign-terminals.json")
    expected_matrix = [
        {"ordinal": ordinal, "mode": mode, "ratePerPeer": rate}
        for ordinal, (mode, rate) in enumerate(runner.MATRIX, 1)
    ]
    if manifest.get("schema") != "spec141.campaign.v1":
        raise ValueError("unexpected campaign schema")
    if manifest.get("matrix") != expected_matrix or len(terminals) != 4:
        raise ValueError("Spec 141 matrix is not the exact four-cell contract")
    if manifest.get("binarySha256") != runner.EXPECTED_BINARY_SHA256:
        raise ValueError("campaign did not use the frozen Spec 140 binary")

    errors: list[str] = []
    cells = []
    for terminal in terminals:
        cell_dir = campaign / terminal["cellId"]
        rate = int(terminal["ratePerPeer"])
        status = terminal.get("status")
        if status not in runner.ACCEPTED_TERMINALS:
            errors.append(f"{cell_dir.name}:invalid terminal status={status}")
        if terminal.get("admissionErrors"):
            errors.append(
                f"{cell_dir.name}:harness errors={terminal['admissionErrors']}"
            )
        combined: list[int] = []
        peers = []
        for peer in base.PEERS:
            summary_path = cell_dir / f"{peer}-summary.json"
            sample_path = cell_dir / f"{peer}-delivery-latency.csv"
            summary = base.load_json(summary_path)
            samples = load_samples_allow_empty(sample_path)
            delivered = int(summary.get("deliveredMeasured", 0))
            attempted = int(summary.get("attemptedMeasured", 0))
            if summary.get("schema") != base.EXPECTED_SCHEMA:
                errors.append(f"{cell_dir.name}:{peer}:unexpected summary schema")
            if delivered != len(samples):
                errors.append(
                    f"{cell_dir.name}:{peer}:delivery/sample mismatch "
                    f"{delivered}!={len(samples)}"
                )
            stats = base.distribution(samples) if samples else zero_distribution()
            for key, expected in stats.items():
                summary_value = summary.get(key)
                if expected is None:
                    if summary_value != 0:
                        errors.append(
                            f"{cell_dir.name}:{peer}:{key}={summary_value}, expected 0"
                        )
                elif summary_value != expected:
                    errors.append(
                        f"{cell_dir.name}:{peer}:{key}={summary_value}, "
                        f"recomputed={expected}"
                    )
            attempted_pps = attempted / 60.0
            if not 0.98 * rate <= attempted_pps <= 1.02 * rate:
                errors.append(
                    f"{cell_dir.name}:{peer}:attempted pps={attempted_pps:.3f}"
                )
            combined.extend(samples)
            peers.append(
                {
                    "peer": peer,
                    "attemptedMeasured": attempted,
                    "deliveredMeasured": delivered,
                    **stats,
                    "sampleSha256": base.sha256(sample_path),
                    "summarySha256": base.sha256(summary_path),
                }
            )
        combined_stats = (
            base.distribution(combined) if combined else zero_distribution()
        )
        attempted_total = sum(row["attemptedMeasured"] for row in peers)
        delivered_total = sum(row["deliveredMeasured"] for row in peers)
        cells.append(
            {
                "cellId": terminal["cellId"],
                "mode": terminal["mode"],
                "ratePerPeer": rate,
                "terminalStatus": status,
                "attemptedPpsPerPeer": attempted_total / 120.0,
                "deliveredPpsPerPeer": delivered_total / 120.0,
                "deliveryRatio": (
                    delivered_total / attempted_total if attempted_total else 0.0
                ),
                "latencyPopulation": (
                    "full-delivery"
                    if delivered_total == attempted_total
                    else "delivered-survivors"
                ),
                **combined_stats,
                "peers": peers,
            }
        )
    return {
        "schema": "spec141.rate-extension-analysis.v1",
        "campaign": str(campaign),
        "status": "PASS" if not errors else "FAIL",
        "errors": errors,
        "cells": cells,
    }


def milliseconds(value: Any) -> str:
    return "N/A" if value is None else f"{value / 1_000_000:.3f}"


def write_outputs(campaign: Path, result: dict[str, Any]) -> None:
    (campaign / "rate-extension-analysis.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    lines = [
        "# Spec 141 600/800 pps Rate Extension",
        "",
        f"- Status: `{result['status']}`",
        "- Same frozen Spec 140 binary and configuration; only rate changed.",
        "- Partial-delivery latency describes delivered survivors only.",
        "",
        "| Rate | Mode | Terminal | Attempted pps/peer | Delivered pps/peer | "
        "Delivery | Population | Samples | Mean ms | p50 ms | p95 ms | p99 ms |",
        "|---:|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|",
    ]
    for cell in result["cells"]:
        lines.append(
            f"| {cell['ratePerPeer']} | {cell['mode']} | "
            f"{cell['terminalStatus']} | {cell['attemptedPpsPerPeer']:.2f} | "
            f"{cell['deliveredPpsPerPeer']:.2f} | "
            f"{cell['deliveryRatio']:.4f} | {cell['latencyPopulation']} | "
            f"{cell['deliverySamples']} | "
            f"{milliseconds(cell['deliveryMeanNs'])} | "
            f"{milliseconds(cell['deliveryP50Ns'])} | "
            f"{milliseconds(cell['deliveryP95Ns'])} | "
            f"{milliseconds(cell['deliveryP99Ns'])} |"
        )
    if result["errors"]:
        lines.extend(["", "## Validation errors", ""])
        lines.extend(f"- {error}" for error in result["errors"])
    (campaign / "rate-extension-analysis.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    args = parser.parse_args()
    result = analyze(args.campaign)
    write_outputs(args.campaign.resolve(), result)
    print(args.campaign.resolve() / "rate-extension-analysis.md")
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

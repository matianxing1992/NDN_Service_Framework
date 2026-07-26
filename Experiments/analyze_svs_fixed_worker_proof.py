#!/usr/bin/env python3
"""Offline verifier and report generator for Spec 139."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any


EXPERIMENTS = Path(__file__).resolve().parent
if str(EXPERIMENTS) not in sys.path:
    sys.path.insert(0, str(EXPERIMENTS))
import NDN_SVS_Fixed_Worker_Proof_Minindn as runner  # noqa: E402
import analyze_svs_worker_necessity as base  # noqa: E402


def analyze_campaign(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    manifest = runner.verify_seal(campaign)
    receipts = sorted((campaign / "receipts").glob("*.json"))
    if len(receipts) != 6:
        raise RuntimeError(f"expected six receipts, got {len(receipts)}")
    run_rows: list[dict[str, Any]] = []
    traffic_rows: list[dict[str, Any]] = []
    host_rows: list[dict[str, Any]] = []
    for config, receipt_path in zip(manifest["cells"], receipts):
        receipt = runner.load_json(receipt_path)
        if (
            receipt.get("schema") != "spec139.receipt.v1"
            or receipt.get("ordinal") != config["ordinal"]
            or receipt.get("mode") != config["mode"]
            or receipt.get("retryCount") != 0
        ):
            raise RuntimeError(f"receipt mismatch: {receipt_path}")
        cell = campaign / "formal/cells" / config["cellId"]
        base.verify_artifact_records(cell)
        row = base.cell_metrics(cell, campaign.name, config)
        row.update({"ordinal": config["ordinal"], "pair": config["pair"]})
        if bool(receipt["admissible"]) != bool(row["admissible"]):
            raise RuntimeError(f"receipt admission mismatch: {receipt_path}")
        run_rows.append(row)
        traffic_rows.append(base.traffic_metrics(cell, config))
        qvalue = runner.load_json(Path(receipt["quiescenceRecord"]))
        host_rows.append(
            {
                "ordinal": config["ordinal"],
                "cellId": config["cellId"],
                "mode": config["mode"],
                "quiescencePassed": qvalue["passed"],
                "maxSelectedBusyRatio": max(
                    qvalue["final"]["busyRatios"].values()
                ),
                "attemptCount": len(qvalue["attempts"]),
            }
        )
    pairs: list[dict[str, Any]] = []
    for pair in (1, 2, 3):
        values = [row for row in run_rows if row["pair"] == pair]
        face = next(row for row in values if row["mode"] == "face-serial")
        worker = next(row for row in values if row["mode"] == "worker-serial")
        pairs.append(base.paired_contrast(pair, face, worker))
    conclusion = base.classify(run_rows, pairs)
    conclusion["schema"] = "spec139.conclusion.v1"
    output = campaign / "analysis"
    base.write_csv(output / "run-metrics.csv", run_rows)
    base.write_csv(output / "paired-contrasts.csv", pairs)
    base.write_csv(
        output / "stage-breakdown.csv",
        [
            {
                key: row[key]
                for key in (
                    "ordinal", "pair", "cellId", "mode", "faceCpuNs",
                    "workerCpuNs", "serialCpuNs", "queueWaitNs",
                    "workerServiceNs", "extraBuildNs", "encodeNs", "signNs",
                    "faceQueueWaitNs", "faceFinalizeNs",
                )
            }
            for row in run_rows
        ],
    )
    base.write_csv(output / "traffic-breakdown.csv", traffic_rows)
    base.write_csv(output / "host-load.csv", host_rows)
    base.write_json(output / "conclusion.json", conclusion)
    return {
        "schema": "spec139.analysis.v1",
        "campaign": str(campaign),
        "runs": run_rows,
        "pairs": pairs,
        "traffic": traffic_rows,
        "host": host_rows,
        "conclusion": conclusion,
    }


def write_report(path: Path, result: dict[str, Any]) -> None:
    base.write_report(path, result)
    text = path.read_text(encoding="utf-8")
    path.write_text(
        text.replace(
            "# Spec 138 Single-Worker Necessity Report",
            "# Spec 139 Fixed-Rate Single-Worker Proof",
            1,
        ),
        encoding="utf-8",
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args(argv)
    result = analyze_campaign(args.campaign)
    write_report(args.report, result)
    print(json.dumps(result["conclusion"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

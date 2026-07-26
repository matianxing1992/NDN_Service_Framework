#!/usr/bin/env python3
"""Analyze one Spec 149 predictive-recovery MiniNDN cell."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "Experiments/analyze_spec148_predictive_uav.py"


def load_base() -> Any:
    spec = importlib.util.spec_from_file_location("spec148_analyzer_base", BASE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load analyzer base: {BASE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASE = load_base()
RECOVERY_FIELDS = (
    "recoveryControlInterests",
    "recoveryFrontierInterests",
    "recoveryGroupInterests",
    "recoveryCoalescedWaiters",
    "recoveryMetadataCacheHits",
)


def analyze(cell_dir: Path, manifest: dict[str, Any],
            return_code: int) -> dict[str, Any]:
    summary = BASE.analyze(cell_dir, manifest, return_code)
    ground = (cell_dir / "ground-station.log").read_text(
        encoding="utf-8", errors="replace"
    )
    core = BASE.last_fields(BASE.CORE_RE, ground)
    metrics = summary["metrics"]
    metrics.update({
        "recoveryControlInterests": BASE.integer(
            core, "recovery_control_interests"
        ),
        "recoveryFrontierInterests": BASE.integer(
            core, "recovery_frontier_interests"
        ),
        "recoveryGroupInterests": BASE.integer(
            core, "recovery_group_interests"
        ),
        "recoveryCoalescedWaiters": BASE.integer(
            core, "recovery_coalesced_waiters"
        ),
        "recoveryMetadataCacheHits": BASE.integer(
            core, "recovery_metadata_cache_hits"
        ),
    })

    checks = summary["checks"]
    checks["recoveryControlAccountingExact"] = (
        metrics["recoveryControlInterests"]
        == metrics["recoveryFrontierInterests"]
        + metrics["recoveryGroupInterests"]
    )
    checks["frontierFanoutBounded"] = (
        metrics["recoveryFrontierInterests"] <= metrics["repairAttempts"]
    )
    checks["recoveryControlDoesNotDominatePayload"] = (
        metrics["recoveryControlInterests"] <= metrics["payloadInterests"]
    )
    if manifest["profile"]["lossPercent"] > 0:
        checks["impairedDeliveryAtLeast98Percent"] = (
            metrics["deliveryRatio"] >= 0.98
        )
        checks["recoveryCoalescingObserved"] = (
            metrics["recoveryCoalescedWaiters"] > 0
        )

    summary["schemaVersion"] = "spec149-predictive-recovery-cell-v1"
    summary["accepted"] = all(checks.values())
    return summary


def write_outputs(cell_dir: Path, summary: dict[str, Any]) -> None:
    BASE.write_outputs(cell_dir, summary)
    metrics = summary["metrics"]
    recovery = {key: metrics[key] for key in RECOVERY_FIELDS}
    (cell_dir / "recovery-control.json").write_text(
        json.dumps(recovery, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    with (cell_dir / "recovery-control.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(RECOVERY_FIELDS))
        writer.writeheader()
        writer.writerow(recovery)
    with (cell_dir / "analysis.md").open("a", encoding="utf-8") as stream:
        stream.write("\n## Recovery control\n\n")
        stream.write("| Metric | Value |\n|---|---:|\n")
        for key in RECOVERY_FIELDS:
            stream.write(f"| {key} | {metrics[key]} |\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cell-dir", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--return-code", required=True, type=int)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    summary = analyze(args.cell_dir, manifest, args.return_code)
    write_outputs(args.cell_dir, summary)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

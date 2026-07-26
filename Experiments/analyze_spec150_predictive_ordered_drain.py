#!/usr/bin/env python3
"""Analyze one Spec 150 predictive ordered-drain MiniNDN cell."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "Experiments/analyze_spec149_predictive_recovery.py"


def load_base() -> Any:
    spec = importlib.util.spec_from_file_location("spec149_analyzer_base", BASE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load analyzer base: {BASE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASE = load_base()
ORDERED_FIELDS = (
    "nextDeliverCursor",
    "readyQueueDepth",
    "oldestReadyCursor",
    "terminalGapQueueDepth",
    "drainWakeCount",
    "staleReadyDrops",
    "terminalGapSuperseded",
)


def analyze(cell_dir: Path, manifest: dict[str, Any],
            return_code: int) -> dict[str, Any]:
    summary = BASE.analyze(cell_dir, manifest, return_code)
    ground = (cell_dir / "ground-station.log").read_text(
        encoding="utf-8", errors="replace"
    )
    core = BASE.BASE.last_fields(BASE.BASE.CORE_RE, ground)
    metrics = summary["metrics"]
    metrics.update({
        "nextDeliverCursor": BASE.BASE.integer(core, "next_deliver_cursor"),
        "readyQueueDepth": BASE.BASE.integer(core, "ready_queue_depth"),
        "oldestReadyCursor": BASE.BASE.integer(core, "oldest_ready_cursor"),
        "terminalGapQueueDepth": BASE.BASE.integer(
            core, "terminal_gap_queue_depth"
        ),
        "drainWakeCount": BASE.BASE.integer(core, "drain_wake_count"),
        "staleReadyDrops": BASE.BASE.integer(core, "stale_ready_drops"),
        "terminalGapSuperseded": BASE.BASE.integer(
            core, "terminal_gap_superseded"
        ),
    })

    checks = summary["checks"]
    checks["directRecoveryGroupLookupBounded"] = (
        metrics["recoveryGroupInterests"] <= metrics["repairAttempts"]
    )
    checks["orderedCursorAccountsForDeliveryAndGaps"] = (
        metrics["nextDeliverCursor"]
        == metrics["delivered"] + metrics["terminalGaps"]
    )
    checks["noPersistentOrderedDrainBacklog"] = (
        metrics["readyQueueDepth"] == 0
        and metrics["terminalGapQueueDepth"] == 0
    )
    checks["drainWasExercised"] = metrics["drainWakeCount"] > 0

    summary["schemaVersion"] = "spec150-predictive-ordered-drain-cell-v1"
    summary["accepted"] = all(checks.values())
    return summary


def write_outputs(cell_dir: Path, summary: dict[str, Any]) -> None:
    BASE.write_outputs(cell_dir, summary)
    metrics = summary["metrics"]
    ordered = {key: metrics[key] for key in ORDERED_FIELDS}
    (cell_dir / "ordered-drain.json").write_text(
        json.dumps(ordered, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    with (cell_dir / "ordered-drain.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(ORDERED_FIELDS))
        writer.writeheader()
        writer.writerow(ordered)
    with (cell_dir / "analysis.md").open("a", encoding="utf-8") as stream:
        stream.write("\n## Ordered drain\n\n")
        stream.write("| Metric | Value |\n|---|---:|\n")
        for key in ORDERED_FIELDS:
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

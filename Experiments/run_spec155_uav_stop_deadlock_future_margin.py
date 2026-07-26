#!/usr/bin/env python3
"""Freeze and execute the immutable Spec 155 six-rate confirmation."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "Experiments/run_spec154_uav_stop_process_map.py"
RUNNER = Path(__file__).resolve()


def load_base() -> Any:
    spec = importlib.util.spec_from_file_location("spec154_runner_base", BASE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load runner base: {BASE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASE = load_base()
BASE.BASE.BASE.ENGINE.RUNNER = RUNNER
BASE.BASE.BASE.ENGINE.SOURCE_PATHS = BASE.BASE.BASE.ENGINE.SOURCE_PATHS + tuple(
    Path(value) for value in (
        "Experiments/run_spec155_uav_stop_deadlock_future_margin.py",
        "tests/python/test_spec155_uav_stop_deadlock_future_margin.py",
        "specs/155-uav-stop-deadlock-future-margin/spec.md",
        "specs/155-uav-stop-deadlock-future-margin/plan.md",
        "specs/155-uav-stop-deadlock-future-margin/tasks.md",
        "specs/155-uav-stop-deadlock-future-margin/contracts/formal-matrix.md",
    )
)


def replace_schema(value: Any) -> Any:
    if isinstance(value, str):
        return value.replace(
            "spec154-uav-stop-process-map",
            "spec155-uav-stop-deadlock-future-margin",
        )
    if isinstance(value, list):
        return [replace_schema(item) for item in value]
    if isinstance(value, dict):
        return {key: replace_schema(item) for key, item in value.items()}
    return value


def apply_efficiency_gates(cell: dict[str, Any]) -> dict[str, Any]:
    metrics = cell.setdefault("metrics", {})
    checks = cell.setdefault("checks", {})
    payload = max(1, int(metrics.get("payloadInterests") or 0))
    retry_ratio = int(metrics.get("retryAttempts") or 0) / payload
    timeout_ratio = int(metrics.get("timeouts") or 0) / payload
    metrics["retryToPayloadRatio"] = retry_ratio
    metrics["timeoutToPayloadRatio"] = timeout_ratio
    checks["retryAtMostTwoPercentOfPayload"] = retry_ratio <= 0.02
    checks["timeoutAtMostTwoPercentOfPayload"] = timeout_ratio <= 0.02
    cell["accepted"] = bool(checks) and all(bool(value) for value in checks.values())
    return cell


def prepare(output_root: Path) -> None:
    BASE.prepare(output_root)
    path = output_root / "frozen-campaign.json"
    campaign = replace_schema(json.loads(path.read_text(encoding="utf-8")))
    path.write_text(
        json.dumps(campaign, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def execute(output_root: Path) -> int:
    # The base return code is intentionally not used to skip aggregation:
    # every failed formal cell remains terminal evidence.
    BASE.execute(output_root)
    cells: list[dict[str, Any]] = []
    for cell_dir in sorted(output_root.glob("fps-*")):
        path = cell_dir / "cell-summary.json"
        if not path.is_file():
            continue
        cell = apply_efficiency_gates(
            replace_schema(json.loads(path.read_text(encoding="utf-8")))
        )
        path.write_text(
            json.dumps(cell, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        cells.append(cell)

    summary_path = output_root / "campaign-summary.json"
    if not summary_path.is_file():
        return 1
    summary = replace_schema(
        json.loads(summary_path.read_text(encoding="utf-8"))
    )
    summary["cells"] = cells
    summary["formalCellCount"] = 6
    summary["acceptedCellCount"] = sum(
        bool(cell.get("accepted")) for cell in cells
    )
    summary["status"] = (
        "PASS"
        if len(cells) == 6 and summary["acceptedCellCount"] == 6
        else "FAIL"
    )
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    BASE.BASE.BASE.write_comparison(output_root, summary)
    comparison = output_root / "rate-comparison.md"
    comparison.write_text(
        comparison.read_text(encoding="utf-8").replace(
            "Spec 152 UAV predictive rate comparison",
            "Spec 155 UAV predictive rate comparison",
        ),
        encoding="utf-8",
    )
    return 0 if summary["status"] == "PASS" else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True, type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--prepare-only", action="store_true")
    action.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    output_root = args.output_root.resolve()
    if args.prepare_only:
        prepare(output_root)
        return 0
    return execute(output_root)


if __name__ == "__main__":
    raise SystemExit(main())

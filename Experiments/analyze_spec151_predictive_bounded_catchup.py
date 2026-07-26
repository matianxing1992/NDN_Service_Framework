#!/usr/bin/env python3
"""Analyze one Spec 151 bounded-catch-up MiniNDN cell."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "Experiments/analyze_spec150_predictive_ordered_drain.py"


def load_base() -> Any:
    spec = importlib.util.spec_from_file_location("spec150_analyzer_base", BASE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load analyzer base: {BASE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASE = load_base()
CATCHUP_FIELDS = (
    "futureCursorHorizon",
    "adaptiveWindow",
    "adaptiveLookahead",
)


def analyze(cell_dir: Path, manifest: dict[str, Any],
            return_code: int) -> dict[str, Any]:
    summary = BASE.analyze(cell_dir, manifest, return_code)
    ground = (cell_dir / "ground-station.log").read_text(
        encoding="utf-8", errors="replace"
    )
    core = BASE.BASE.BASE.last_fields(BASE.BASE.BASE.CORE_RE, ground)
    metrics = summary["metrics"]
    metrics.update({
        "futureCursorHorizon": BASE.BASE.BASE.integer(
            core, "future_cursor_horizon"
        ),
        "adaptiveWindow": BASE.BASE.BASE.integer(core, "window"),
        "adaptiveLookahead": BASE.BASE.BASE.integer(core, "lookahead"),
    })

    checks = summary["checks"]
    checks["futureCursorHorizonExposed"] = (
        metrics["futureCursorHorizon"] > 0
    )
    checks["futureCursorHorizonWithinLookahead"] = (
        metrics["adaptiveLookahead"] > 0
        and metrics["futureCursorHorizon"] <= metrics["adaptiveLookahead"]
    )
    checks["futureCursorHorizonWithinWindow"] = (
        metrics["adaptiveWindow"] > 0
        and metrics["futureCursorHorizon"] <= metrics["adaptiveWindow"]
    )

    summary["schemaVersion"] = "spec151-predictive-bounded-catchup-cell-v1"
    summary["accepted"] = all(checks.values())
    return summary


def write_outputs(cell_dir: Path, summary: dict[str, Any]) -> None:
    BASE.write_outputs(cell_dir, summary)
    metrics = summary["metrics"]
    catchup = {key: metrics[key] for key in CATCHUP_FIELDS}
    (cell_dir / "bounded-catchup.json").write_text(
        json.dumps(catchup, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    with (cell_dir / "bounded-catchup.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(CATCHUP_FIELDS))
        writer.writeheader()
        writer.writerow(catchup)
    with (cell_dir / "analysis.md").open("a", encoding="utf-8") as stream:
        stream.write("\n## Bounded catch-up\n\n")
        stream.write("| Metric | Value |\n|---|---:|\n")
        for key in CATCHUP_FIELDS:
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

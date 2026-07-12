#!/usr/bin/env python3
"""Compare measured NativeTracer layout runs from MiniNDN result directories."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import sys
from typing import Any


def load_summary(path: Path) -> dict[str, Any]:
    summary_path = path / "summary.json"
    if not summary_path.exists():
        raise RuntimeError(f"missing summary.json: {summary_path}")
    return json.loads(summary_path.read_text(encoding="utf-8"))


def row_for(label: str, path: Path) -> dict[str, Any]:
    summary = load_summary(path)
    user = summary.get("userExecution", {})
    optimization = summary.get("optimizationEvidence", {})
    return {
        "label": label,
        "resultDir": str(path),
        "status": summary.get("status", ""),
        "assignment": summary.get("assignment", ""),
        "runnerClassification": summary.get("runnerClassification", "invalid-evidence"),
        "executionEvidence": summary.get("executionEvidence", []),
        "selectedCandidate": optimization.get("selectedCandidate", ""),
        "runtimeCandidate": optimization.get("runtimeCandidate", ""),
        "elapsedMs": float(user.get("elapsedMs", 0.0) or 0.0),
        "payloadBytes": int(user.get("payloadBytes", 0) or 0),
        "userExecution": user.get("status", ""),
        "dependencyExecution": summary.get("dependencyExecution", {}).get("status", ""),
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--alternative", required=True)
    parser.add_argument("--out-json", required=True)
    parser.add_argument("--out-csv", required=True)
    args = parser.parse_args(argv)

    rows = [
        row_for("baseline", Path(args.baseline)),
        row_for("alternative", Path(args.alternative)),
    ]
    baseline_ms = rows[0]["elapsedMs"]
    alternative_ms = rows[1]["elapsedMs"]
    delta_ms = alternative_ms - baseline_ms
    comparison = {
        "baseline": rows[0],
        "alternative": rows[1],
        "deltaMs": round(delta_ms, 3),
        "alternativeOverBaselineRatio": (
            round(alternative_ms / baseline_ms, 4)
            if baseline_ms > 0 else None
        ),
    }
    Path(args.out_json).write_text(
        json.dumps(comparison, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    write_csv(Path(args.out_csv), rows)
    print("NDNSF_DI_NATIVE_TRACER_LAYOUT_COMPARISON_OK")
    print("baseline:", rows[0]["selectedCandidate"], rows[0]["elapsedMs"])
    print("alternative:", rows[1]["selectedCandidate"], rows[1]["elapsedMs"])
    print("deltaMs:", comparison["deltaMs"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

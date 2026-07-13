#!/usr/bin/env python3
"""Aggregate every Spec 109 cell; never filter to successful runs."""
from __future__ import annotations

import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Mapping


def aggregate_matrix(matrix: Mapping[str, object]) -> dict:
    cells = matrix.get("cells")
    if not isinstance(cells, Mapping) or not cells:
        raise ValueError("AGGREGATE_MATRIX_EMPTY")
    states = Counter()
    models = defaultdict(Counter)
    rows = []
    for cell_id, raw in sorted(cells.items()):
        if not isinstance(raw, Mapping):
            raise ValueError("AGGREGATE_CELL_INVALID")
        state = str(raw.get("state", ""))
        if state not in {"PASS", "FAIL", "BLOCKED", "DEFERRED", "CANCELLED"}:
            raise ValueError("AGGREGATE_CELL_NONTERMINAL")
        size = str(raw.get("modelSize", ""))
        states[state] += 1
        models[size][state] += 1
        rows.append({"cellId": cell_id, "modelSize": size,
                     "mode": raw.get("mode"), "repetition": raw.get("repetition"),
                     "state": state, "reasonCode": raw.get("reasonCode"),
                     "runId": raw.get("runId"), "evidenceDigest": raw.get("evidenceDigest"),
                     "comparisonEligible": state == "PASS" and
                       raw.get("mode") in {"staged-baseline", "candidate-performance"}})
    return {
        "schemaVersion": "1.0", "plannedCellCount": len(cells),
        "representedCellCount": len(rows), "stateCounts": dict(states),
        "perModel": {size: dict(counts) for size, counts in sorted(models.items())},
        "successfulOnlyFiltering": False, "rows": rows,
    }


def reconcile_critical_path(end_to_end_ms: float, components: Mapping[str, float]) -> dict:
    if end_to_end_ms <= 0 or any(value < 0 for value in components.values()):
        raise ValueError("CRITICAL_PATH_INPUT_INVALID")
    accounted = sum(components.values())
    coverage = accounted / end_to_end_ms
    residual = end_to_end_ms - accounted
    return {"endToEndMs": end_to_end_ms, "accountedMs": accounted,
            "residualMs": residual, "coverage": coverage,
            "status": "PASS" if 0.99 <= coverage <= 1.01 else "FAIL"}


def engineering_equivalence(reference: float, ci_low: float, ci_high: float,
                            tolerance: float = 0.10) -> str:
    if reference <= 0 or ci_low > ci_high or tolerance <= 0:
        raise ValueError("REPRODUCTION_INPUT_INVALID")
    lower, upper = reference * (1 - tolerance), reference * (1 + tolerance)
    if ci_low >= lower and ci_high <= upper:
        return "PASS"
    if ci_high < lower or ci_low > upper:
        return "FAIL"
    return "INCONCLUSIVE"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--matrix", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    matrix = json.loads(Path(args.matrix).read_text(encoding="utf-8"))
    report = aggregate_matrix(matrix)
    output = Path(args.output_dir); output.mkdir(parents=True, exist_ok=True)
    (output / "matrix-summary.json").write_text(
        json.dumps({k: v for k, v in report.items() if k != "rows"}, indent=2,
                   sort_keys=True) + "\n", encoding="utf-8")
    fields = ("cellId", "modelSize", "mode", "repetition", "state", "reasonCode",
              "runId", "evidenceDigest", "comparisonEligible")
    with (output / "cells.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields); writer.writeheader()
        writer.writerows(report["rows"])
    (output / "matched-overhead.json").write_text(json.dumps({
        "schemaVersion": "1.0", "status": "UNAVAILABLE",
        "reasonCode": "NO_ACCEPTED_MATCHED_PAIRS", "pairs": [],
        "fullModelOracleTimingIncluded": False, "successfulOnlyFiltering": False,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"status": "PASS", "cells": report["representedCellCount"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

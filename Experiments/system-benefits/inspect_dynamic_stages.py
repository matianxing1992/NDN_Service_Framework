#!/usr/bin/env python3
"""Exploratory timing breakdown after qualification of the complete matrix.

Admission after a previous rejection is an observed timing category, not proof
of a particular internal bootstrap mode. All arms retain failed submissions.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
from analyze_credentials import estimate
from analyze_dynamic import percentile, require, rows
from summarize_dynamic import summarize


def inspect(directory):
    qualified = summarize(directory)
    manifest = json.loads((directory / "manifest.json").read_text())
    blocks = []
    for cell in manifest["cells"]:
        path = directory / cell["id"]
        own = json.loads((path / "manifest.json").read_text())
        for run in own["runs"]:
            label = run["id"]
            user = rows(path / (label + "-user.jsonl"))
            submitted = [r for r in user if r["event"] == "attempt_submitted"]
            by_id = {r["request_id"]: r for r in submitted}
            by_attempt = {(r["sequence"], r["attempt"]): r for r in submitted}
            require(len(by_id) == len(submitted), "ambiguous native submission identity")
            executions = []
            for peer in ("drone1", "drone2"):
                executions.extend(r for r in rows(path / (label + "-" + peer + ".jsonl"))
                                  if r["event"] == "execution" and not r["command"].startswith("w" * 64))
            rejected = defaultdict(list)
            for execution in executions:
                require(execution["request_id"] in by_id, "admission without measured submission")
                if not execution["eligible"]:
                    rejected[execution["provider"]].append(execution["end_ns"])
            samples = defaultdict(list)
            for execution in executions:
                submission = by_id[execution["request_id"]]
                delay = (execution["begin_ns"] - submission["begin_ns"]) / 1e6
                require(delay >= 0, "admission precedes submission")
                samples["submit_to_admission_ms"].append(delay)
                previous = any(t < submission["begin_ns"] for t in rejected[execution["provider"]])
                samples["after_prior_rejection_ms" if previous else "before_prior_rejection_ms"].append(delay)
                samples["handler_ms"].append((execution["end_ns"] - execution["begin_ns"]) / 1e6)
            for selection in user:
                if selection["event"] == "advertisement_selection":
                    key = (selection["sequence"], selection["attempt"])
                    require(key in by_attempt, "snapshot without measured submission")
                    samples["selected_snapshot_age_ms"].append(
                        (by_attempt[key]["begin_ns"] - selection["snapshot"]["published_ns"]) / 1e6)
            blocks.append({"cell": cell["id"], "arm": run["arm"], "period_ms": own["period_ms"],
                           "dwell_ms": own["dwell_ms"], "submissions": len(submitted),
                           "admissions": len(executions), "metrics": {
                               name: {"observations": len(values), "median_ms": percentile(values, .5)}
                               for name, values in samples.items()}})
    groups = defaultdict(list)
    for block in blocks:
        groups[(block["period_ms"], block["dwell_ms"], block["arm"])].append(block)
    result = []
    names = ("submit_to_admission_ms", "before_prior_rejection_ms", "after_prior_rejection_ms",
             "handler_ms", "selected_snapshot_age_ms")
    for (period, dwell, arm), group in sorted(groups.items()):
        metrics = {}
        for name in names:
            values = [b["metrics"][name]["median_ms"] for b in group if name in b["metrics"]]
            metrics[name] = estimate(values) if values else {"n": 0, "mean": None, "ci95_half_width": None}
        result.append({"period_ms": period, "dwell_ms": dwell, "arm": arm, "metrics": metrics})
    return {"qualified": True, "scope": "exploratory observed intervals; no internal bootstrap-mode count",
            "statistical_unit": "equal-weight block medians; missing categories remain undefined",
            "candidate": qualified["candidate"], "raw_sha256": qualified["raw_sha256"],
            "postprocessing_sha256": {**qualified["postprocessing_sha256"],
                                      str(Path(__file__).resolve()): hashlib.sha256(Path(__file__).read_bytes()).hexdigest()},
            "blocks": blocks, "results": result}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.directory), indent=2))

#!/usr/bin/env python3
"""Run the frozen three-policy Spec 119 MiniNDN acceptance matrix."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import random
import statistics
import subprocess
import sys
from typing import Any


REPO = Path(__file__).resolve().parents[1]
CELL_RUNNER = REPO / "Experiments/NDNSF_UAV_Stream_Parity_Campaign.py"
VIDEO = REPO / "NDNSF-UAV-APP/videos/drone.mp4"
RUNTIME_ARTIFACTS = (
    REPO / "build/examples/UavDroneApp",
    REPO / "build/examples/UavGroundStationApp",
    REPO / "build/examples/App_ServiceController",
)
POLICIES = (
    "mapped-pressure",
    "mapped-live-v1-future-on",
    "mapped-live-v1-future-off",
)


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def relative_improvement(baseline: float, candidate: float) -> float:
    if baseline > 0:
        return (baseline - candidate) / baseline
    return 0.0 if candidate == 0 else -1.0


def analyze(runs: list[dict[str, Any]]) -> dict[str, Any]:
    by_key = {
        (int(run["lossPercent"]), int(run["pairId"]), str(run["prefetchPolicy"])): run
        for run in runs
    }
    paired: list[dict[str, Any]] = []
    for loss in sorted({int(run["lossPercent"]) for run in runs}):
        for pair_id in sorted({int(run["pairId"]) for run in runs
                               if int(run["lossPercent"]) == loss}):
            pressure = by_key.get((loss, pair_id, "mapped-pressure"))
            candidate = by_key.get((loss, pair_id, "mapped-live-v1-future-on"))
            future_off = by_key.get((loss, pair_id, "mapped-live-v1-future-off"))
            if not pressure or not candidate or not future_off:
                continue
            lag_improvement = relative_improvement(
                float(pressure["captureToDecodeP95Ms"]),
                float(candidate["captureToDecodeP95Ms"]))
            pressure_load = int(pressure["maxTimeouts"]) + int(pressure["maxNacks"])
            candidate_load = int(candidate["maxTimeouts"]) + int(candidate["maxNacks"])
            load_improvement = relative_improvement(pressure_load, candidate_load)
            best = max(lag_improvement, load_improvement)
            other = min(lag_improvement, load_improvement)
            paired.append({
                "lossPercent": loss,
                "pairId": pair_id,
                "lagImprovement": lag_improvement,
                "timeoutNackImprovement": load_improvement,
                "bestImprovement": best,
                "otherWorsening": min(0.0, other),
                "favorable": best > 0.0 and other >= -0.05,
                "futureOnMaxInFlight": int(candidate["maxCoreInFlight"]),
                "futureOffMaxInFlight": int(future_off["maxCoreInFlight"]),
            })

    loss_decisions: list[dict[str, Any]] = []
    for loss in sorted({row["lossPercent"] for row in paired}):
        rows = [row for row in paired if row["lossPercent"] == loss]
        loss_decisions.append({
            "lossPercent": loss,
            "pairCount": len(rows),
            "favorablePairs": sum(bool(row["favorable"]) for row in rows),
            "medianBestImprovement": statistics.median(
                row["bestImprovement"] for row in rows) if rows else 0.0,
            "medianLagImprovement": statistics.median(
                row["lagImprovement"] for row in rows) if rows else 0.0,
            "medianTimeoutNackImprovement": statistics.median(
                row["timeoutNackImprovement"] for row in rows) if rows else 0.0,
        })

    future_runs = [run for run in runs
                   if run["prefetchPolicy"] == "mapped-live-v1-future-on"]
    future_gate = bool(future_runs) and all(
        int(run.get("futurePayloadInterests", 0)) > 0 and
        int(run.get("providerFutureEligible", 0)) > 0 and
        float(run.get("providerFutureHitRatio", 0.0)) >= 0.99
        for run in future_runs
    )
    completion_gate = bool(runs) and all(bool(run.get("accepted")) for run in runs)
    adoption_gate = bool(loss_decisions) and all(
        row["pairCount"] >= 5 and row["favorablePairs"] >= 4 and
        row["medianBestImprovement"] >= 0.10
        for row in loss_decisions
    )
    selected = "mapped-live-v1-future-on" if (
        completion_gate and future_gate and adoption_gate
    ) else "mapped-pressure"
    return {
        "completionGate": completion_gate,
        "futureHitGate": future_gate,
        "adoptionGate": adoption_gate,
        "selectedDefault": selected,
        "pairedEffects": paired,
        "lossDecisions": loss_decisions,
        "negativeResultPreserved": selected == "mapped-pressure",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--duration-seconds", type=int, default=60)
    parser.add_argument("--loss-percentages", default="0,5")
    parser.add_argument("--order-seed", type=int, default=11920260718)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    losses = [int(value) for value in args.loss_percentages.split(",")]
    if args.runs < 1 or args.duration_seconds < 1 or any(loss < 0 or loss > 100 for loss in losses):
        raise SystemExit("invalid campaign dimensions")

    out = args.out.expanduser().resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_digest = file_digest(VIDEO)
    runtime_digests = {str(path.relative_to(REPO)): file_digest(path)
                       for path in RUNTIME_ARTIFACTS}
    plan: list[dict[str, Any]] = []
    for pair_id in range(1, args.runs + 1):
        for loss in losses:
            order = list(POLICIES)
            random.Random(args.order_seed + pair_id * 1000 + loss).shuffle(order)
            for order_index, policy in enumerate(order):
                plan.append({
                    "pairId": pair_id,
                    "lossPercent": loss,
                    "policy": policy,
                    "orderIndex": order_index,
                })
    (out / "campaign-plan.json").write_text(json.dumps({
        "schemaVersion": "spec119-prefetch-campaign-v1",
        "automaticRetry": False,
        "orderSeed": args.order_seed,
        "sourceTrace": str(VIDEO),
        "sourceTraceSha256": source_digest,
        "runtimeArtifactSha256": runtime_digests,
        "netemSeedAvailable": False,
        "plan": plan,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.dry_run:
        print(json.dumps({"status": "DRY_RUN", "cellCount": len(plan)}))
        return 0

    runs: list[dict[str, Any]] = []
    for cell in plan:
        cell_root = out / f"pair-{cell['pairId']:02d}" / f"loss-{cell['lossPercent']:02d}" / cell["policy"]
        summary_path = cell_root / "campaign-summary.json"
        if summary_path.exists():
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
        else:
            if cell_root.exists() and any(cell_root.iterdir()):
                raise SystemExit(f"partial cell exists; refusing automatic retry: {cell_root}")
            command = [
                sys.executable, str(CELL_RUNNER),
                "--out", str(cell_root),
                "--runs", "1",
                "--loss-percentages", str(cell["lossPercent"]),
                "--fec-parity-shards", "1",
                "--auto-stop-seconds", str(args.duration_seconds),
                "--live-stream-prefetch-policy", cell["policy"],
            ]
            completed = subprocess.run(command, cwd=REPO, check=False)
            if not summary_path.exists():
                raise SystemExit(f"cell produced no summary (rc={completed.returncode}): {cell_root}")
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
        run = dict(summary["runs"][0])
        run.update({
            "pairId": cell["pairId"],
            "orderIndex": cell["orderIndex"],
            "sourceTraceSha256": source_digest,
        })
        runs.append(run)
        print(json.dumps({
            "pairId": cell["pairId"], "loss": cell["lossPercent"],
            "policy": cell["policy"], "accepted": run.get("accepted", False),
        }), flush=True)

    decision = analyze(runs)
    result = {
        "schemaVersion": "spec119-prefetch-campaign-v1",
        "status": "SUCCESS" if decision["completionGate"] else "FAILURE",
        "runCount": len(runs),
        "acceptedRuns": sum(bool(run.get("accepted")) for run in runs),
        "constants": {
            "runsPerTreatment": args.runs,
            "durationSeconds": args.duration_seconds,
            "lossPercentages": losses,
            "policies": POLICIES,
            "orderSeed": args.order_seed,
            "sourceTraceSha256": source_digest,
            "runtimeArtifactSha256": runtime_digests,
            "automaticRetry": False,
            "netemSeedAvailable": False,
        },
        "decision": decision,
        "runs": runs,
    }
    (out / "campaign-summary.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if runs:
        fields = sorted({key for run in runs for key in run})
        with (out / "campaign-runs.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(runs)
    print(json.dumps({"status": result["status"], "decision": decision}, sort_keys=True))
    return 0 if result["status"] == "SUCCESS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

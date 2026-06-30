#!/usr/bin/env python3
"""Compare greedy and proportional LLM planning across an RPS sweep.

This script is planner-derived. It can optionally run the existing NativeTracer
MiniNDN harness separately, but the current harness does not execute LLM roles.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent
PLANNER = ROOT / "plan_llm_resource_aware.py"


def parse_float_list(raw: str) -> list[float]:
    values = []
    for item in raw.split(","):
        text = item.strip()
        if not text:
            continue
        value = float(text)
        if value < 0:
            raise SystemExit("RPS values must be non-negative")
        values.append(value)
    return values


def run_plan(mode: str,
             model_spec: Path,
             provider_profiles: Path,
             out_dir: Path) -> dict[str, Any]:
    out = out_dir / f"plan-{mode}.json"
    subprocess.run([
        "python3", str(PLANNER),
        "--model-spec", str(model_spec),
        "--provider-profiles", str(provider_profiles),
        "--out", str(out),
        "--mode", mode,
        "--validate",
        "--expect-shards", "no",
    ], check=True)
    return json.loads(out.read_text(encoding="utf-8"))


def provider_utils(plan: dict[str, Any], target_rps: float) -> dict[str, float]:
    utils = {}
    for stage in plan.get("stages", []):
        provider = str(stage["provider"])
        compute_ms = float(stage.get("estimatedComputeMs", 0.0))
        utils[provider] = utils.get(provider, 0.0) + (compute_ms * target_rps / 1000.0)
    return utils


def estimate_latency(plan: dict[str, Any], target_rps: float) -> dict[str, float]:
    stages = list(plan.get("stages", []))
    max_compute = float(plan.get("summary", {}).get("maxStageComputeMs", 0.0))
    transfer_ms = 2.0 * max(0, len(stages) - 1)
    base = max_compute + transfer_ms
    utils = provider_utils(plan, target_rps)
    max_util = max(utils.values() or [0.0])
    if max_util >= 1.0:
        queue_penalty = 1000.0 * (max_util - 1.0)
        failure_rate = min(1.0, max_util - 1.0)
    else:
        queue_penalty = base * (max_util / max(0.05, 1.0 - max_util)) * 0.10
        failure_rate = 0.0
    p50 = base + queue_penalty
    p95 = p50 * (1.0 + min(2.0, max_util) * 0.5)
    return {
        "baseLatencyMs": round(base, 3),
        "p50Ms": round(p50, 3),
        "p95Ms": round(p95, 3),
        "failureRate": round(failure_rate, 6),
        "maxProviderUtilization": round(max_util, 6),
    }


def max_stable_rps(plan: dict[str, Any], max_utilization: float) -> float:
    stage_ms_by_provider = {}
    for stage in plan.get("stages", []):
        provider = str(stage["provider"])
        stage_ms_by_provider[provider] = (
            stage_ms_by_provider.get(provider, 0.0) +
            float(stage.get("estimatedComputeMs", 0.0)))
    bottleneck = max(stage_ms_by_provider.values() or [0.0])
    if bottleneck <= 0:
        return 0.0
    return round(max_utilization * 1000.0 / bottleneck, 3)


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-spec", type=Path, default=ROOT / "llm_model_spec_qwen_tiny_proportional.json")
    parser.add_argument("--provider-profiles", type=Path, default=ROOT / "llm_provider_profiles_2_4_8.json")
    parser.add_argument("--out-root", type=Path, required=True)
    parser.add_argument("--target-rps-list", default="1,5,10,20,30,40")
    parser.add_argument("--stable-utilization", type=float, default=0.85)
    parser.add_argument("--attempt-minindn", action="store_true")
    args = parser.parse_args()

    out_root = args.out_root.resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    plans_dir = out_root / "plans"
    plans_dir.mkdir(exist_ok=True)
    plans = {
        mode: run_plan(mode, args.model_spec, args.provider_profiles, plans_dir)
        for mode in ("greedy", "proportional")
    }
    target_rps_values = parse_float_list(args.target_rps_list)
    rows = []
    for mode, plan in plans.items():
        stable = max_stable_rps(plan, args.stable_utilization)
        allocation = json.dumps(plan.get("summary", {}).get("layerAllocation", {}),
                                sort_keys=True, separators=(",", ":"))
        for target_rps in target_rps_values:
            estimate = estimate_latency(plan, target_rps)
            rows.append({
                "mode": mode,
                "targetRps": target_rps,
                "maxStableRps": stable,
                "stageCount": int(plan.get("summary", {}).get("stageCount", 0)),
                "layerAllocation": allocation,
                "p50Ms": estimate["p50Ms"],
                "p95Ms": estimate["p95Ms"],
                "failureRate": estimate["failureRate"],
                "maxProviderUtilization": estimate["maxProviderUtilization"],
            })
    csv_path = out_root / "llm-proportional-rps-search.csv"
    write_csv(csv_path, rows)
    summary = {
        "modelSpec": str(args.model_spec),
        "providerProfiles": str(args.provider_profiles),
        "csv": str(csv_path),
        "plans": {mode: str(plans_dir / f"plan-{mode}.json") for mode in plans},
        "maxStableRps": {
            mode: max_stable_rps(plan, args.stable_utilization)
            for mode, plan in plans.items()
        },
        "layerAllocation": {
            mode: plan.get("summary", {}).get("layerAllocation", {})
            for mode, plan in plans.items()
        },
        "minindn": {
            "attempted": bool(args.attempt_minindn),
            "status": "not-run",
            "reason": (
                "Current MiniNDN full-network harness executes /Inference/NativeTracer; "
                "it does not yet execute LLM proportional roles."
            ),
        },
        "rows": rows,
    }
    (out_root / "llm-proportional-rps-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print("NDNSF_DI_LLM_PROPORTIONAL_RPS_SEARCH_OK")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

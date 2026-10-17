#!/usr/bin/env python3
"""Recompute the frozen Spec206 matrix and paired block summaries."""

import argparse
import hashlib
import json
from pathlib import Path
import random
import statistics
import subprocess
import sys


REPO = Path(__file__).resolve().parents[1]
ANALYZER = REPO / "Experiments/system-benefits/analyze_spec206.py"
ARMS = ("SOLICIT", "NSC", "GRPC")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require(value, reason):
    if not value:
        raise ValueError(reason)


def interval(values, seed):
    rng = random.Random(seed)
    count = len(values)
    samples = sorted(statistics.mean(values[rng.randrange(count)] for _ in range(count))
                     for _ in range(10000))
    return [samples[250], samples[9749]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root")
    parser.add_argument("--spec207-coarse", action="store_true")
    args = parser.parse_args()
    arms = ("SOLICIT", "ADS_COARSE") if args.spec207_coarse else ARMS
    root = Path(args.root).resolve()
    freeze = json.loads((root / "freeze.json").read_text())
    contract = freeze["contract"]
    require(freeze["provenance"]["analyzer_sha256"] == digest(ANALYZER),
            "analyzer differs from frozen source")
    require(contract["schema"] in (("ndnsf-spec207-coarse-matrix-v1",) if args.spec207_coarse
                                    else ("ndnsf-spec206-formal-matrix-v2",
                                          "ndnsf-spec206-task-busy-v1")),
            "wrong matrix design")
    result = {"schema": ("ndnsf-spec207-matrix-summary-v1" if args.spec207_coarse else
                          "ndnsf-spec206-matrix-summary-v2"), "contract": contract,
              "freeze_sha256": digest(root / "freeze.json"), "conditions": []}
    for condition in contract["conditions"]:
        blocks = []
        for index, seed in enumerate(contract["seeds"]):
            archive = root / f"block-{index + 1}-{condition['name']}"
            manifest_path = archive / "manifest.json"
            manifest = json.loads(manifest_path.read_text())
            expected_order = list(arms[seed % len(arms):] + arms[:seed % len(arms)])
            require(manifest["source_sha256"] == freeze["provenance"]["source_sha256"]
                    and manifest.get("baseline_sources", {}) == freeze["provenance"]["baseline_sources"]
                    and manifest["binaries"] == freeze["provenance"]["binaries"]
                    and manifest["seed"] == seed
                    and manifest["condition"] == condition["condition"]
                    and manifest["arm_order"] == expected_order
                    and manifest["baseline_attempt_ms"] == contract["baseline_attempt_ms"],
                    "formal provenance or condition mismatch: " + str(archive))
            completed = subprocess.run(
                [sys.executable, str(ANALYZER), str(archive)],
                check=True, capture_output=True, text=True)
            receipt = json.loads(completed.stdout)
            require(receipt == json.loads((archive / "reanalysis.json").read_text())
                    and receipt["manifest_sha256"] == digest(manifest_path)
                    and {run["arm"] for run in receipt["runs"]} == set(arms),
                    "formal receipt mismatch: " + str(archive))
            runs = {run["arm"]: run for run in receipt["runs"]}
            task_count = len(runs["SOLICIT"]["outcomes"])
            require(task_count == 13 and all(run["tasks"] == task_count for run in runs.values()),
                    "formal task cohort mismatch")
            block = {"seed": seed, "arms": {}}
            for arm in arms:
                run = runs[arm]
                outcomes = {row["task"]: row for row in run["outcomes"]}
                require(set(outcomes) == set(range(1, task_count + 1)),
                        "task IDs missing or duplicated")
                success = [row["latency_ms"] for row in outcomes.values() if row["success"]]
                block["arms"][arm] = {
                    "successes": run["successes"],
                    "client_successes": run["client_successes"],
                    "duplicate_executions": run["duplicate_executions"],
                    "late_executions": run["late_executions"],
                    "success_latency_ms": success,
                    "penalty_latency_ms": [row["latency_ms"] if row["success"] else 2000.0
                                           for row in outcomes.values()],
                    "ip_bytes": run["traffic"]["ip_bytes"],
                    "attempts": run["attempts"],
                    "health_checks": run["health_checks"],
                    "diagnostics": run.get("diagnostics"),
                    "pcap_sha256": run["traffic"]["pcap_sha256"],
                }
            blocks.append(block)
        arm_summary = {}
        for arm in arms:
            rows = [block["arms"][arm] for block in blocks]
            successes = [ms for row in rows for ms in row["success_latency_ms"]]
            arm_summary[arm] = {
                "successes": sum(row["successes"] for row in rows),
                "client_successes": sum(row["client_successes"] for row in rows),
                "duplicate_executions": sum(row["duplicate_executions"] for row in rows),
                "late_executions": sum(row["late_executions"] for row in rows),
                "tasks": 13 * len(rows),
                "success_rate": sum(row["successes"] for row in rows) / (13 * len(rows)),
                "success_latency_mean_ms": statistics.mean(successes) if successes else None,
                "failure_penalty_mean_ms": statistics.mean(
                    ms for row in rows for ms in row["penalty_latency_ms"]),
                "gross_ip_kib_per_planned_task":
                    sum(row["ip_bytes"] for row in rows) / (1024 * 13 * len(rows)),
                "attempts_per_planned_task": sum(row["attempts"] for row in rows) / (13 * len(rows)),
                "health_checks_per_window":
                    statistics.mean(row["health_checks"] for row in rows)
                    if arm == "GRPC" else None,
            }
            if args.spec207_coarse:
                ages = [age for row in rows
                        for age in row["diagnostics"]["selected_advertisement_age_ms"]]
                arm_summary[arm]["selected_snapshot_age_mean_ms"] = (
                    statistics.mean(ages) if ages else None)
                for field in ("failed_invocations", "status_queries", "accepted_initial_ads",
                              "selected_long_advertisements", "selected_short_statuses"):
                    arm_summary[arm][field] = sum(row["diagnostics"][field] for row in rows)
        contrasts = {}
        for other in arms[1:]:
            differences = {
                "success_rate": [
                    (block["arms"]["SOLICIT"]["successes"] -
                     block["arms"][other]["successes"]) / 13 for block in blocks],
                "failure_penalty_ms": [statistics.mean(block["arms"]["SOLICIT"]["penalty_latency_ms"])
                                       - statistics.mean(block["arms"][other]["penalty_latency_ms"])
                                       for block in blocks],
                "gross_ip_kib_per_task": [
                    (block["arms"]["SOLICIT"]["ip_bytes"] - block["arms"][other]["ip_bytes"])
                    / (1024 * 13) for block in blocks],
                "attempts_per_task": [
                    (block["arms"]["SOLICIT"]["attempts"] - block["arms"][other]["attempts"])
                    / 13 for block in blocks],
            }
            contrasts["SOLICIT_minus_" + other] = {
                metric: {"mean": statistics.mean(values),
                         "block_bootstrap_95_ci": interval(values, 20600 + len(contrasts) * 10 + n)}
                for n, (metric, values) in enumerate(differences.items())
            }
        result["conditions"].append({"name": condition["name"], "blocks": blocks,
                                     "arms": arm_summary, "contrasts": contrasts})
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()

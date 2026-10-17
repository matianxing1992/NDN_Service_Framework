#!/usr/bin/env python3
"""Recount qualified Spec 207 completions at exploratory delivery-time cutoffs."""

import argparse
import hashlib
import itertools
import json
from pathlib import Path
import statistics


CUTOFFS_MS = (100, 150, 200, 250, 300, 350, 400, 450, 500)
ARMS = ("SOLICIT", "ADS_COARSE")


def read_json(path):
    return json.loads(path.read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def timings(directory, arm):
    files = list(directory.glob(f"*-{arm}-user.jsonl"))
    if len(files) != 1:
        raise ValueError(f"expected one {arm} user record in {directory}")
    events = (json.loads(line) for line in files[0].read_text().splitlines())
    terminals = [row for row in events if row.get("event") == "terminal"]
    if len(terminals) != 13 or {row["sequence"] for row in terminals} != set(range(1, 14)):
        raise ValueError(f"incomplete task cohort: {files[0]}")
    if any(row["terminal"] != "success" or row["success_response_ns"] <= 0
           or row["success_response_ns"] >= row["deadline_ns"]
           for row in terminals):
        raise ValueError(f"unqualified terminal: {files[0]}")
    return [(row["success_response_ns"] - row["begin_ns"]) / 1_000_000
            for row in sorted(terminals, key=lambda row: row["sequence"])]


def paired_interval(differences):
    means = sorted(statistics.mean(differences[index] for index in sample)
                   for sample in itertools.product(range(len(differences)),
                                                   repeat=len(differences)))
    return [means[int(0.025 * len(means))], means[int(0.975 * len(means)) - 1]]


def load_block(block, receipt_path):
    receipt = read_json(receipt_path)
    manifest_path = block / "manifest.json"
    manifest = read_json(manifest_path)
    if (receipt["schema"] != "ndnsf-spec207-reanalysis-v1"
            or receipt["manifest_sha256"] != digest(manifest_path)
            or manifest["schema"] != "ndnsf-spec207-coarse-v1"
            or manifest["logical_deadline_ms"] != 2000
            or manifest["period_ms"] != 4700
            or manifest["window_ms"] != 60000
            or manifest["condition"] != "task-busy"):
        raise ValueError(f"unexpected evidence contract: {block}")
    runs = {run["arm"]: run for run in receipt["runs"]}
    if set(runs) != set(ARMS):
        raise ValueError(f"unpaired arms: {block}")
    for arm in ARMS:
        run = runs[arm]
        if (not run["native_receipt"]["qualified"] or run["tasks"] != 13
                or run["successes"] != 13 or run["duplicate_executions"]
                or run["late_executions"]):
            raise ValueError(f"failed native qualification: {block}/{arm}")
    return {arm: timings(block, arm) for arm in ARMS}, runs, manifest


def summarize_batch(root, blocks, receipt_path):
    samples, runs, manifests = [], [], []
    for block in blocks:
        sample, paired, manifest = load_block(block, receipt_path(block))
        samples.append(sample)
        runs.append(paired)
        manifests.append(manifest)
    rows = []
    for cutoff in CUTOFFS_MS:
        counts = {arm: sum(value < cutoff for block in samples for value in block[arm])
                  for arm in ARMS}
        differences = [(sum(value < cutoff for value in block["SOLICIT"])
                        - sum(value < cutoff for value in block["ADS_COARSE"])) / 13
                       for block in samples]
        rows.append({"cutoff_ms": cutoff, "timely_results": counts,
                     "paired_difference_percentage_points": statistics.mean(differences) * 100,
                     "paired_block_bootstrap_95_pct_percentage_points":
                         [value * 100 for value in paired_interval(differences)]})
    aggregate = {}
    for arm in ARMS:
        arm_runs = [pair[arm] for pair in runs]
        aggregate[arm] = {
            "correct_tasks": sum(run["successes"] for run in arm_runs),
            "attempts": sum(run["attempts"] for run in arm_runs),
            "failed_invocations": sum(run["diagnostics"]["failed_invocations"]
                                      for run in arm_runs),
            "mean_success_latency_ms": statistics.mean(
                outcome["latency_ms"] for run in arm_runs for outcome in run["outcomes"]),
            "gross_relay_ip_KiB_per_task": sum(run["traffic"]["ip_bytes"]
                                               for run in arm_runs) / (len(blocks) * 13 * 1024),
        }
    return {"source": str(root), "blocks": len(blocks), "tasks_per_arm": len(blocks) * 13,
            "metrics": aggregate, "cutoffs": rows}, manifests


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--validation", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.evidence.resolve()
    summary = read_json(root / "summary.json")
    contract = summary["contract"]
    if (summary["schema"] != "ndnsf-spec207-matrix-summary-v1"
            or contract["logical_deadline_ms"] != 2000
            or contract["seeds"] != [20621, 20622, 20623, 20624, 20625]):
        raise ValueError("unexpected formal evidence contract")
    blocks = sorted(root.glob("block-*-task-busy-fast"))
    if len(blocks) != 5:
        raise ValueError("expected five paired blocks")
    original, original_manifests = summarize_batch(
        root, blocks, lambda block: block / "reanalysis.json")
    result = {"schema": "ndnsf-spec207-delivery-thresholds-v2",
              "interpretation": "Exploratory post-hoc delivery-time cutoffs on "
                                "2-second-deadline runs; not deadline-enforced trials.",
              "original": original}
    if args.validation:
        validation = args.validation.resolve()
        new_blocks = [validation / f"seed{seed}" for seed in range(20631, 20636)]
        if any(not block.is_dir() for block in new_blocks):
            raise ValueError("missing validation seed")
        replication, new_manifests = summarize_batch(
            validation, new_blocks,
            lambda block: validation / (block.name + "-reanalysis.json"))
        reference = original_manifests[0]
        for manifest in new_manifests:
            if manifest["binaries"] != reference["binaries"] or manifest["libraries"] != reference["libraries"]:
                raise ValueError("validation binary/library drift")
            for name in ("dynamic-user.cpp", "dynamic-provider.cpp", "dynamic-oracle.cpp",
                         "run_dynamic.py"):
                if (manifest["source_sha256"][name]
                        != reference["source_sha256"][name]):
                    raise ValueError(f"validation source drift: {name}")
        result["validation"] = replication
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()

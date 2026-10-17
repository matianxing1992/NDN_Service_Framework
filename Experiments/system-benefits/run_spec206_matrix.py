#!/usr/bin/env python3
"""Run the frozen, sequential Spec206 comparison using maintained actors."""

import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import subprocess
import sys


HERE = Path(__file__).resolve().parent
CONTRACT = {
    "schema": "ndnsf-spec206-formal-matrix-v2",
    "seeds": [20621, 20622, 20623, 20624, 20625],
    "conditions": [
        {"name": "stable", "condition": "stable", "profile": "legacy"},
        {"name": "offline-slow", "condition": "offline", "profile": "slow"},
        {"name": "offline-fast", "condition": "offline", "profile": "fast"},
        {"name": "busy-fast", "condition": "busy", "profile": "fast"},
    ],
    "window_ms": 60000,
    "period_ms": 4700,
    "baseline_attempt_ms": 250,
    "logical_deadline_ms": 2000,
    "arm_rotation": "seed modulo three over SOLICIT, NSC, GRPC",
    "analysis": "paired blocks; all planned tasks; duplicate/late execution incorrect; gross relay IP bytes",
}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--task-busy-followup", action="store_true")
    parser.add_argument("--spec207-coarse", action="store_true")
    args = parser.parse_args()
    if args.task_busy_followup and args.spec207_coarse:
        parser.error("choose one follow-up")
    contract = dict(CONTRACT)
    if args.task_busy_followup or args.spec207_coarse:
        contract["schema"] = ("ndnsf-spec207-coarse-matrix-v1" if args.spec207_coarse else
                              "ndnsf-spec206-task-busy-v1")
        contract["conditions"] = [{"name": "task-busy-fast", "condition": "task-busy",
                                   "profile": "fast"}]
    if args.spec207_coarse:
        contract["arm_rotation"] = "seed modulo two over SOLICIT, ADS_COARSE"
    root = Path(args.output_root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    with (root / ".matrix.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        freeze_path = root / "freeze.json"
        if freeze_path.exists():
            freeze = json.loads(freeze_path.read_text())
            if freeze["contract"] != contract:
                raise RuntimeError("formal matrix contract changed")
        else:
            freeze = {"contract": contract, "provenance": None}
            write_json(freeze_path, freeze)
        analyzer = HERE / "analyze_spec206.py"
        for block, seed in enumerate(contract["seeds"]):
            conditions = contract["conditions"]
            conditions = conditions[block % len(conditions):] + conditions[:block % len(conditions)]
            for condition in conditions:
                name = f"block-{block + 1}-{condition['name']}"
                archive = root / name
                receipt = archive / "reanalysis.json"
                if not archive.exists():
                    print("MATRIX_START", name, flush=True)
                    subprocess.run([
                        sys.executable, str(HERE / "run_dynamic.py"),
                        "--comparison-condition", condition["condition"],
                        "--availability-profile", condition["profile"],
                        "--comparison-attempt-ms", str(contract["baseline_attempt_ms"]),
                        "--seed", str(seed),
                        "--window-ms", str(contract["window_ms"]),
                        "--period-ms", str(contract["period_ms"]),
                        "--output-dir", str(archive),
                        *(["--spec207-coarse"] if args.spec207_coarse else []),
                    ], check=True)
                manifest_path = archive / "manifest.json"
                manifest = json.loads(manifest_path.read_text())
                if (manifest["schema"] != ("ndnsf-spec207-coarse-v1" if args.spec207_coarse
                                          else "ndnsf-spec206-comparison-v1") or
                        manifest["status"] != "ACTORS_COMPLETE_UNQUALIFIED" or
                        manifest["condition"] != condition["condition"] or
                        manifest["availability_trace"]["profile"] !=
                        ("stable" if condition["condition"] == "stable" else condition["profile"]) or
                        manifest["baseline_attempt_ms"] != contract["baseline_attempt_ms"] or
                        manifest["seed"] != seed or len(manifest["runs"]) !=
                        (2 if args.spec207_coarse else 3)):
                    raise RuntimeError("formal cell incomplete or mismatched: " + name)
                provenance = {
                    "source_sha256": manifest["source_sha256"],
                    "baseline_sources": manifest.get("baseline_sources", {}),
                    "binaries": manifest["binaries"],
                    "analyzer_sha256": sha256(analyzer),
                }
                if freeze["provenance"] is None:
                    freeze["provenance"] = provenance
                    write_json(freeze_path, freeze)
                elif freeze["provenance"] != provenance:
                    raise RuntimeError("formal source/binary identity changed: " + name)
                result = subprocess.run(
                    [sys.executable, str(analyzer), str(archive)],
                    capture_output=True, text=True)
                if result.returncode != 0:
                    raise RuntimeError("formal analyzer rejected " + name + ": " +
                                       result.stderr.strip())
                analyzed = json.loads(result.stdout)
                if (analyzed["manifest_sha256"] != sha256(manifest_path) or
                        analyzed["analyzer_sha256"] != provenance["analyzer_sha256"] or
                        {run["arm"] for run in analyzed["runs"]} !=
                        ({"SOLICIT", "ADS_COARSE"} if args.spec207_coarse else
                         {"SOLICIT", "NSC", "GRPC"})):
                    raise RuntimeError("formal reanalysis incomplete: " + name)
                if receipt.exists() and json.loads(receipt.read_text()) != analyzed:
                    raise RuntimeError("formal receipt changed: " + name)
                if not receipt.exists():
                    write_json(receipt, analyzed)
                print("MATRIX_QUALIFIED", name,
                      " ".join(f"{run['arm']}={run['successes']}/{run['tasks']}"
                               for run in analyzed["runs"]), flush=True)
        print("MATRIX_COMPLETE", root, flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run the single, fixed Spec175 host/CPU MiniNDN matrix.

This is the only G3 orchestration entry point.  It deliberately has no seed,
case, timeout, admission-control, or repetition knobs: the case runner and
the G3 manifest own those values.  A non-empty output root is rejected so a
second invocation cannot silently mix evidence with a previous subject.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CASE_RUNNER = ROOT / "Experiments/NDNSF_DI_StreamedGeneration_Minindn.py"
MANIFEST_DRIVER = ROOT / "scripts/spec175_g3_manifest.py"
FIXTURE_ROOT = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"
TOPOLOGY = ROOT / "Experiments/Topology/spec175-host-gate.conf"
CASES = (
    "M01", "M02", "M03", "M04", "M05", "M06", "M07",
    "M08", "M09", "M10", "M11", "M12", "M13", "M14",
)
WORKLOAD_SEED = 1750001
FAULT_SEED = 1750002
FAULT_CASES = {"M05", "M06", "M07", "M08", "M09"}
REPETITIONS = 3


class MatrixError(ValueError):
    """Raised before or during the fixed matrix when a gate invariant fails."""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", default=str(ROOT))
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--manifest-output", required=True)
    parser.add_argument("--source-seal", required=True)
    parser.add_argument("--fixture-manifest", default=str(FIXTURE_ROOT / "manifest.json"))
    parser.add_argument("--tiny-fixture-root", default=str(FIXTURE_ROOT))
    parser.add_argument("--topology-file", default=str(TOPOLOGY))
    parser.add_argument("--dry-run", action="store_true")
    return parser


def _resolve(root: Path, value: str) -> Path:
    path = Path(value).expanduser()
    return path.resolve() if path.is_absolute() else (root / path).resolve()


def _validate_inputs(args: argparse.Namespace) -> tuple[Path, Path, Path, Path, Path, Path]:
    root = _resolve(Path.cwd(), args.project_root)
    output_root = _resolve(root, args.output_root)
    manifest_output = _resolve(root, args.manifest_output)
    source_seal = _resolve(root, args.source_seal)
    fixture_manifest = _resolve(root, args.fixture_manifest)
    fixture_root = _resolve(root, args.tiny_fixture_root)
    topology = _resolve(root, args.topology_file)
    if root != ROOT:
        raise MatrixError(f"project root must be the repository root: {ROOT}")
    if not args.dry_run and os.geteuid() != 0:
        raise MatrixError(
            "MiniNDN/Mininet requires root; rerun the same command with sudo -E")
    for label, path in (
        ("case runner", CASE_RUNNER),
        ("manifest driver", MANIFEST_DRIVER),
        ("fixture root", fixture_root),
        ("fixture manifest", fixture_manifest),
        ("topology", topology),
        ("source seal", source_seal),
    ):
        if not path.exists():
            raise MatrixError(f"missing {label}: {path}")
    if topology != TOPOLOGY:
        raise MatrixError(f"G3 requires the frozen topology: {TOPOLOGY}")
    if output_root.exists():
        raise MatrixError(f"output root must not already exist: {output_root}")
    if manifest_output.exists():
        raise MatrixError(f"manifest output already exists: {manifest_output}")
    return root, output_root, manifest_output, source_seal, fixture_manifest, fixture_root


def case_seed(case: str) -> int:
    return FAULT_SEED if case in FAULT_CASES else WORKLOAD_SEED


def case_command(case: str, seed: int, output_dir: Path, fixture_root: Path,
                 topology: Path) -> list[str]:
    return [
        sys.executable,
        str(CASE_RUNNER),
        "--case", case,
        "--seed", str(seed),
        "--output-dir", str(output_dir),
        "--tiny-fixture-root", str(fixture_root),
        "--topology-file", str(topology),
    ]


def _write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run_matrix(args: argparse.Namespace) -> int:
    root, output_root, manifest_output, source_seal, fixture_manifest, fixture_root = _validate_inputs(args)
    topology = TOPOLOGY
    entries: list[dict[str, Any]] = []
    for case in CASES:
        seed = case_seed(case)
        for repetition in range(1, REPETITIONS + 1):
            output_dir = output_root / case / f"r{repetition}"
            command = case_command(case, seed, output_dir, fixture_root, topology)
            entries.append({
                "case": case,
                "repetition": repetition,
                "seed": seed,
                "outputDir": str(output_dir),
                "command": command,
            })
    if args.dry_run:
        print(json.dumps({
            "schema": "ndnsf-di-spec175-g3-matrix-plan-v1",
            "status": "DRY_RUN",
            "cases": list(CASES),
            "repetitionsPerCase": REPETITIONS,
            "total": len(entries),
            "entries": entries,
        }, indent=2, sort_keys=True))
        return 0

    output_root.mkdir(parents=True, exist_ok=False)
    summary = {
        "schema": "ndnsf-di-spec175-g3-matrix-run-v1",
        "startedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "status": "RUNNING",
        "total": len(entries),
        "completed": 0,
        "entries": [],
    }
    summary_path = output_root / "matrix-run.json"
    _write_json(summary_path, summary)
    for entry in entries:
        output_dir = Path(entry["outputDir"])
        output_dir.mkdir(parents=True, exist_ok=False)
        log_path = output_dir.parent / f"{output_dir.name}.driver.log"
        with log_path.open("w", encoding="utf-8") as log:
            log.write("COMMAND " + json.dumps(entry["command"]) + "\n")
            completed = subprocess.run(
                entry["command"], cwd=root,
                env={**os.environ, "SPEC175_RUN_REAL_MININDN": "1"},
                stdout=log, stderr=subprocess.STDOUT, check=False,
            )
        entry_result = {**entry, "returnCode": completed.returncode, "log": str(log_path)}
        summary["entries"].append(entry_result)
        summary["completed"] = len(summary["entries"])
        if completed.returncode != 0:
            summary["status"] = "FAIL"
            summary["failedEntry"] = entry_result
            summary["finishedAt"] = dt.datetime.now(dt.timezone.utc).isoformat()
            _write_json(summary_path, summary)
            return completed.returncode or 1
        _write_json(summary_path, summary)

    manifest_command = [
        sys.executable, str(MANIFEST_DRIVER),
        "--project-root", str(root),
        "--output", str(manifest_output),
        "--source-seal", str(source_seal),
        "--fixture-manifest", str(fixture_manifest),
        "--topology", str(topology),
    ]
    for case in CASES:
        manifest_command += ["--run-root", f"{case}={output_root / case}"]
    manifest_log = output_root / "manifest-driver.log"
    with manifest_log.open("w", encoding="utf-8") as log:
        log.write("COMMAND " + json.dumps(manifest_command) + "\n")
        completed = subprocess.run(manifest_command, cwd=root, stdout=log,
                                   stderr=subprocess.STDOUT, check=False)
    summary["manifestDriverReturnCode"] = completed.returncode
    summary["manifestOutput"] = str(manifest_output)
    summary["finishedAt"] = dt.datetime.now(dt.timezone.utc).isoformat()
    summary["status"] = "PASS" if completed.returncode == 0 else "FAIL"
    _write_json(summary_path, summary)
    return completed.returncode


def main() -> int:
    try:
        return run_matrix(build_parser().parse_args())
    except MatrixError as exc:
        print(f"SPEC175_G3_MATRIX_REJECTED:{exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

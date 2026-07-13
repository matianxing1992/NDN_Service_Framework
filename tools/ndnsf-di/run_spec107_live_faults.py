#!/usr/bin/env python3
"""Immutable once-only orchestration primitives for Spec 107 live faults."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Mapping

from spec107_identity import (
    digest_object,
    validate_campaign_set,
    validate_candidate_identity,
)
from spec107_lineage import assert_mutation_allowed

import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.runtime_v1_evidence import LiveFaultRecordV1  # noqa: E402


FAULT_CELLS = (
    "positive-control", "provider-kill-restart", "straggler",
    "missing-segment", "dependency-digest-mismatch", "stale-telemetry",
    "kv-eviction", "provider-boot-change", "late-old-output",
)
FAULT_PROVIDER_CELLS = frozenset({
    "straggler", "missing-segment", "dependency-digest-mismatch",
    "stale-telemetry", "kv-eviction", "late-old-output",
})


class LiveFaultOrchestrationError(RuntimeError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise LiveFaultOrchestrationError(code + (f":{detail}" if detail else ""))


class LiveFaultOrchestrator:
    def __init__(self, *, candidate: Mapping[str, object],
                 campaign: Mapping[str, object], repo_root: Path | str) -> None:
        self.candidate = validate_candidate_identity(candidate)
        self.campaign = validate_campaign_set(
            [campaign], candidate_id=str(self.candidate["candidateId"]),
            candidate_digest=digest_object(self.candidate))[0]
        if self.campaign["kind"] != "fault" or self.campaign["releaseEligible"] is not True:
            _fail("FAULT_CAMPAIGN_INVALID")
        self.repo_root = Path(repo_root).resolve()
        self.output_root = assert_mutation_allowed(
            str(self.campaign["outputRoot"]), repo_root=self.repo_root)
        self.cleanup_failed = False

    def preregistration(self) -> dict[str, object]:
        return {
            "schema": "ndnsf-di-spec107-live-fault-matrix-v1",
            "candidateId": self.candidate["candidateId"],
            "campaignId": self.campaign["campaignId"],
            "outputRoot": str(self.campaign["outputRoot"]),
            "onceOnly": True,
            "orderedCells": [
                {"ordinal": index + 1, "cellId": cell,
                 "providerClass": "fault" if cell in FAULT_PROVIDER_CELLS else "normal",
                 "state": "LOCKED"}
                for index, cell in enumerate(FAULT_CELLS)
            ],
        }

    def lock(self, path: Path | str) -> Path:
        destination = assert_mutation_allowed(path, repo_root=self.repo_root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        try:
            with destination.open("x", encoding="utf-8") as stream:
                json.dump(self.preregistration(), stream, indent=2, sort_keys=True)
                stream.write("\n")
        except FileExistsError:
            _fail("FAULT_MATRIX_ALREADY_LOCKED", str(destination))
        return destination

    def claim_cell(self, cell_id: str) -> Path:
        if self.cleanup_failed:
            _fail("FAULT_MATRIX_STOPPED_BY_CLEANUP")
        if cell_id not in FAULT_CELLS:
            _fail("FAULT_CELL_UNKNOWN", cell_id)
        path = self.output_root / f"{FAULT_CELLS.index(cell_id) + 1:02d}-{cell_id}"
        marker = path.with_name(path.name + ".claim.json")
        marker.parent.mkdir(parents=True, exist_ok=True)
        try:
            with marker.open("x", encoding="utf-8") as stream:
                json.dump({
                    "schema": "ndnsf-di-spec107-cell-claim-v1",
                    "candidateId": self.candidate["candidateId"],
                    "campaignId": self.campaign["campaignId"],
                    "cellId": cell_id,
                    "pid": os.getpid(),
                }, stream, sort_keys=True)
                stream.write("\n")
        except FileExistsError:
            _fail("FAULT_CELL_ALREADY_CONSUMED", cell_id)
        return path

    def retain_record(self, cell_id: str, record: Mapping[str, object],
                      cell_root: Path | str) -> Path:
        parsed = LiveFaultRecordV1.from_dict(record)
        if (parsed.cell_id != cell_id or
                parsed.candidate_id != self.candidate["candidateId"] or
                parsed.campaign_id != self.campaign["campaignId"]):
            _fail("FAULT_RECORD_IDENTITY_MISMATCH")
        root = Path(cell_root).resolve()
        try:
            root.relative_to(self.output_root.resolve())
        except ValueError:
            _fail("FAULT_CELL_OUTPUT_ESCAPE")
        destination = root / "live-fault-record.json"
        try:
            with destination.open("x", encoding="utf-8") as stream:
                json.dump(parsed.to_dict(), stream, indent=2, sort_keys=True)
                stream.write("\n")
        except FileExistsError:
            _fail("FAULT_RECORD_ALREADY_EXISTS", cell_id)
        if not parsed.cleanup.proven:
            self.cleanup_failed = True
        return destination


def validate_cell_claim(*, cell_id: str, candidate_id: str,
                        campaign_id: str, output_root: Path | str) -> dict[str, object]:
    output = Path(output_root).resolve()
    claim = output.with_name(output.name + ".claim.json")
    try:
        value = json.loads(claim.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        _fail("FAULT_CELL_CLAIM_MISSING_OR_INVALID", str(exc))
    if not isinstance(value, dict) or value.get("schema") != "ndnsf-di-spec107-cell-claim-v1":
        _fail("FAULT_CELL_CLAIM_INVALID")
    if (value.get("cellId") != cell_id or
            value.get("candidateId") != candidate_id or
            value.get("campaignId") != campaign_id):
        _fail("FAULT_CELL_CLAIM_IDENTITY_MISMATCH")
    if output.exists():
        _fail("FAULT_CELL_ALREADY_CONSUMED", cell_id)
    return value


def derive_fault_provider_control(*, cell_id: str,
                                  marker_observed: bool) -> dict[str, object]:
    if cell_id == "positive-control":
        return {
            "injectionApplied": False,
            "networkInjection": False,
            "providerFaultMarkerObserved": False,
        }
    if cell_id not in FAULT_PROVIDER_CELLS:
        _fail("FAULT_EXTERNAL_CONTROL_REQUIRED", cell_id)
    if marker_observed is not True:
        _fail("FAULT_PROVIDER_MARKER_NOT_OBSERVED", cell_id)
    return {
        "injectionApplied": True,
        "networkInjection": True,
        "providerFaultMarkerObserved": True,
    }


def _load_object(path: Path | str) -> dict[str, object]:
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        _fail("FAULT_INPUT_INVALID", str(exc))
    if not isinstance(value, dict):
        _fail("FAULT_INPUT_INVALID")
    return value


def _orchestrator(args: argparse.Namespace) -> LiveFaultOrchestrator:
    return LiveFaultOrchestrator(
        candidate=_load_object(args.candidate),
        campaign=_load_object(args.campaign),
        repo_root=args.repo_root)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=REPO)
    commands = parser.add_subparsers(dest="command", required=True)
    preregister = commands.add_parser("preregister")
    run_cell = commands.add_parser("run-cell")
    for command in (preregister, run_cell):
        command.add_argument("--candidate", type=Path, required=True)
        command.add_argument("--campaign", type=Path, required=True)
    preregister.add_argument("--output", type=Path, required=True)
    run_cell.add_argument("--matrix-lock", type=Path, required=True)
    run_cell.add_argument("--cell", choices=FAULT_CELLS, required=True)
    run_cell.add_argument("--timing-sample-rate", type=int, default=1)
    args = parser.parse_args(argv)
    try:
        orchestrator = _orchestrator(args)
        if args.command == "preregister":
            path = orchestrator.lock(args.output)
            print(json.dumps({"status": "LOCKED", "path": str(path)}, sort_keys=True))
            return 0
        expected_lock = orchestrator.preregistration()
        actual_lock = _load_object(args.matrix_lock)
        if actual_lock != expected_lock:
            _fail("FAULT_MATRIX_LOCK_MISMATCH")
        output = orchestrator.claim_cell(args.cell)
        claim = validate_cell_claim(
            cell_id=args.cell,
            candidate_id=str(orchestrator.candidate["candidateId"]),
            campaign_id=str(orchestrator.campaign["campaignId"]),
            output_root=output)
        command = [
            sys.executable,
            str(orchestrator.repo_root / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"),
            "--runtime", "qwen-onnx-cpu-native",
            "--spec107-live-fault-cell", args.cell,
            "--candidate-manifest", str(Path(args.candidate).resolve()),
            "--campaign-manifest", str(Path(args.campaign).resolve()),
            "--campaign-id", str(orchestrator.campaign["campaignId"]),
            "--output-dir", str(output),
            "--max-new-tokens", "32",
            "--measured-requests", "1",
            "--spec107-timing-sample-rate", str(args.timing_sample_rate),
        ]
        completed = subprocess.run(command, cwd=orchestrator.repo_root, check=False)
        if not output.is_dir():
            _fail("FAULT_CELL_OUTPUT_MISSING_AFTER_EXECUTION")
        execution_path = output / "cell-execution.json"
        with execution_path.open("x", encoding="utf-8") as stream:
            json.dump({
                "schema": "ndnsf-di-spec107-cell-execution-v1",
                "candidateId": orchestrator.candidate["candidateId"],
                "campaignId": orchestrator.campaign["campaignId"],
                "cellId": args.cell,
                "claim": claim,
                "command": command,
                "returnCode": completed.returncode,
                "preserved": True,
            }, stream, indent=2, sort_keys=True)
            stream.write("\n")
        return completed.returncode
    except (LiveFaultOrchestrationError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


__all__ = [
    "FAULT_CELLS", "FAULT_PROVIDER_CELLS", "LiveFaultOrchestrationError",
    "LiveFaultOrchestrator", "derive_fault_provider_control",
    "validate_cell_claim",
]


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Produce the source-bound Spec183 host gate from retained MiniNDN runs.

The command only joins already completed, bounded run directories.  It never
turns a return code or a hand-written status into a qualification; the shared
validator re-reads every lifecycle, execution, numeric, failure and cleanup
record before the receipt is published.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
GATE = ROOT / "packaging/ndnsf-di-container/lib/spec183_yolo_host_gate.py"
sys.path.insert(0, str(GATE.parent))
from spec183_yolo_host_gate import (  # noqa: E402
    CASES, CASE_EVIDENCE, GRAPH, SCHEMA, WORKLOAD, sha256,
    validate_yolo_host_gate,
)


def _digest(path: Path) -> str:
    return sha256(path)


def _json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def _relative(path: Path, root: Path) -> str:
    try:
        relative = path.resolve().relative_to(root.resolve())
    except ValueError as exc:
        raise ValueError("HOST_GATE_EVIDENCE_OUTSIDE_RECEIPT") from exc
    if not relative.parts or any(part in ("", ".", "..") for part in relative.parts):
        raise ValueError("HOST_GATE_EVIDENCE_PATH")
    return relative.as_posix()


def _record(path: Path, root: Path, kind: str) -> dict:
    if not path.is_file() or path.is_symlink():
        raise ValueError("HOST_GATE_EVIDENCE_MISSING:" + kind)
    return {"kind": kind, "path": _relative(path, root),
            "bytes": path.stat().st_size, "sha256": _digest(path)}


def _find(output: Path, name: str) -> Path:
    path = output / name
    if not path.is_file() or path.is_symlink():
        raise ValueError("HOST_GATE_OUTPUT_MISSING:" + name)
    return path


def _run_id_for_output(output: Path) -> str:
    """Recover the prepared run directory owning one host output tree."""
    for ancestor in (output.resolve(), *output.resolve().parents):
        if (ancestor / "public" / "preparation.json").is_file():
            return ancestor.name
    raise ValueError("HOST_GATE_RUN_OWNER_MISSING")


def _normal_case(output: Path, root: Path, *, application_name: str) -> dict:
    lifecycle = _find(output, "lifecycle.jsonl")
    logs = sorted(output.glob("provider-*.log"))
    if len(logs) != 4:
        raise ValueError("HOST_GATE_EXECUTION_MISSING")
    execution = root / "spec183-host-gate-execution.log"
    if execution.exists() or execution.is_symlink():
        raise ValueError("HOST_GATE_EXECUTION_OUTPUT_EXISTS")
    with execution.open("wb") as stream:
        for log in logs:
            if log.is_symlink() or not log.is_file():
                raise ValueError("HOST_GATE_EXECUTION_MISSING")
            stream.write(log.read_bytes())
            stream.write(b"\n")
    numeric = _find(output, "yolo-numerical.json")
    cleanup = _find(output, "cleanup-attempt-001.json")
    rows = [json.loads(line) for line in lifecycle.read_text(encoding="utf-8").splitlines()]
    terminal = [row for row in rows if row.get("milestone") == "TERMINAL_RESPONSE"]
    if len(terminal) != 1 or terminal[0].get("status") is not True:
        raise ValueError("HOST_GATE_NORMAL_TERMINAL")
    return {
        "case": "normal", "runId": _run_id_for_output(output), "status": "PASS",
        "requestCount": terminal[0].get("requestCount", 0),
        "responseStatus": "PASS", "success": True,
        "failureBoundary": None, "reselectionCount": 0,
        "evidence": [_record(lifecycle, root, "lifecycle"),
                     _record(execution, root, "execution"),
                     _record(numeric, root, "numeric"),
                     _record(cleanup, root, "cleanup")],
    }


def _negative_case(case: str, output: Path, root: Path) -> dict:
    lifecycle = _find(output, "lifecycle.jsonl")
    failure = _find(output, "negative-evidence.json")
    cleanup = _find(output, "cleanup-attempt-001.json")
    body = _json(failure)
    boundary = body.get("boundary")
    if case == "permission-rejection":
        expected = "PROVIDER_GRANT_VERIFICATION"
    else:
        expected = {"DEPENDENCY_DATA_MISSING", "PEER_FAILURE"}
    if boundary != expected and not (isinstance(expected, set) and boundary in expected):
        raise ValueError("HOST_GATE_FAILURE_BOUNDARY")
    return {
        "case": case, "runId": _run_id_for_output(output), "status": "PASS", "requestCount": 1,
        "responseStatus": "REJECTED", "success": False,
        "failureBoundary": boundary, "reselectionCount": 0,
        "evidence": [_record(lifecycle, root, "lifecycle"),
                     _record(failure, root, "failure"),
                     _record(cleanup, root, "cleanup")],
    }


def build_manifest(*, receipt: Path, source_seal: Path, base_sif: Path,
                   application_manifest: Path, run_id: str, application_name: str,
                   normal: Path, permission: Path, negative: Path) -> dict:
    receipt = receipt.expanduser().resolve()
    root = receipt.parent
    # Validate all negative selectors before materializing the combined normal
    # execution log.  A rejected dependency case must leave no partial
    # producer artifact that would make a later retry look like a duplicate.
    permission_case = _negative_case("permission-rejection", permission, root)
    negative_case = _negative_case("negative-dependency", negative, root)
    normal_case = _normal_case(normal, root, application_name=application_name)
    value = {
        "schema": SCHEMA, "status": "PASS", "workload": WORKLOAD,
        "sourceSeal": {"path": str(source_seal.resolve()),
                        "sha256": _digest(source_seal),
                        "sealDigest": _json(source_seal)["sealDigest"],
                        "sourceRevision": _json(source_seal)["sourceRevision"]},
        "runId": run_id, "baseSifSha256": _digest(base_sif),
        "applicationManifestSha256": _digest(application_manifest),
        "applicationName": application_name, "providerCount": 4,
        "graph": GRAPH,
        "cases": [normal_case, permission_case, negative_case],
    }
    receipt.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    validate_yolo_host_gate(receipt, source_seal_path=source_seal,
                            expected_base_sif_sha256=_digest(base_sif),
                            expected_application_manifest_sha256=_digest(application_manifest))
    return value


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--source-seal", required=True, type=Path)
    parser.add_argument("--base-sif", required=True, type=Path)
    parser.add_argument("--application-manifest", required=True, type=Path)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--application-name", required=True)
    parser.add_argument("--normal", required=True, type=Path)
    parser.add_argument("--permission", required=True, type=Path)
    parser.add_argument("--negative", required=True, type=Path)
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", args.run_id):
        parser.error("invalid --run-id")
    value = build_manifest(receipt=args.receipt, source_seal=args.source_seal,
                           base_sif=args.base_sif,
                           application_manifest=args.application_manifest,
                           run_id=args.run_id, application_name=args.application_name,
                           normal=args.normal, permission=args.permission,
                           negative=args.negative)
    print(json.dumps({"status": "PASS", "schema": value["schema"],
                      "receipt": str(args.receipt.resolve())}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

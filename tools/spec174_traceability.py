#!/usr/bin/env python3
"""Validate and materialize the Spec174 requirement traceability map.

The map is planning/evidence metadata.  It does not infer that a symbol is
wired or executed merely because it appears in a document; execution status
must be supplied explicitly by the gate that ran it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


SCHEMA = "spec174-traceability-v1"
ROW = re.compile(r"^\|\s*(FR-\d{3}|SC-\d{3})\s*\|(.*)\|\s*$")


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def revision(repo: Path) -> str:
    return subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=repo, text=True
    ).strip()


def parse_rows(path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = ROW.match(line)
        if not match:
            continue
        fields = [field.strip() for field in match.group(2).split("|")]
        expected_fields = 4 if match.group(1).startswith("FR-") else 2
        if len(fields) != expected_fields:
            raise ValueError(f"SPEC174_TRACEABILITY_ROW_FIELDS:{match.group(1)}")
        if expected_fields == 4:
            requirement, owner, regression, task, proof = (match.group(1), *fields)
            rows.append({
                "requirement": requirement,
                "owner": owner,
                "regression": regression,
                "tasks": task,
                "proof": proof,
            })
        else:
            requirement, task, proof = (match.group(1), *fields)
            rows.append({
                "requirement": requirement,
                "owner": "gate acceptance criteria",
                "regression": "success-criteria gate",
                "tasks": task,
                "proof": proof,
            })
    return rows


def build(repo: Path, status: str, executed: list[str]) -> dict[str, object]:
    path = repo / "specs/174-ndnsf-di-verified-delivery/traceability.md"
    spec = repo / "specs/174-ndnsf-di-verified-delivery/spec.md"
    rows = parse_rows(path)
    by_id = {row["requirement"]: row for row in rows}
    if len(rows) != len(by_id):
        raise ValueError("SPEC174_TRACEABILITY_DUPLICATE_REQUIREMENT")
    fr = sorted(key for key in by_id if key.startswith("FR-"))
    sc = sorted(key for key in by_id if key.startswith("SC-"))
    if fr != [f"FR-{index:03d}" for index in range(1, 30)]:
        raise ValueError("SPEC174_TRACEABILITY_FR_COUNT_OR_ID_GAP")
    if sc != [f"SC-{index:03d}" for index in range(1, 12)]:
        raise ValueError("SPEC174_TRACEABILITY_SC_COUNT_OR_ID_GAP")
    for row in rows:
        if not row["owner"] or not row["regression"] or not row["tasks"] or not row["proof"]:
            raise ValueError(f"SPEC174_TRACEABILITY_EMPTY_FIELD:{row['requirement']}")
    executed_set = sorted(set(executed))
    unknown = sorted(set(executed_set) - set(by_id))
    if unknown:
        raise ValueError("SPEC174_TRACEABILITY_UNKNOWN_EXECUTED:" + ",".join(unknown))
    for row in rows:
        row["currentSessionStatus"] = "executed" if row["requirement"] in executed_set else status
    payload: dict[str, object] = {
        "schemaVersion": SCHEMA,
        "sourceRevision": revision(repo),
        "traceabilitySha256": digest(path),
        "specSha256": digest(spec),
        "counts": {"functionalRequirements": len(fr), "successCriteria": len(sc)},
        "executedRequirements": executed_set,
        "rows": rows,
    }
    body = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    payload["manifestSha256"] = "sha256:" + hashlib.sha256(body).hexdigest()
    return payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--status", default="not-executed")
    parser.add_argument("--executed", action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.status not in {"not-executed", "unverified", "partial", "conforming"}:
        raise SystemExit("SPEC174_TRACEABILITY_STATUS_INVALID")
    repo = args.repo.resolve()
    result = build(repo, args.status, args.executed)
    output = args.output if args.output.is_absolute() else repo / args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({
        "status": "PASS",
        "schemaVersion": SCHEMA,
        "counts": result["counts"],
        "manifestSha256": result["manifestSha256"],
        "output": str(output),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

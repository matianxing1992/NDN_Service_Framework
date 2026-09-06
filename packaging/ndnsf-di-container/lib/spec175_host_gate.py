"""Fail-closed validation for the Spec175 host/CPU manifest.

The historical v1 manifest describes the old M01--M14 matrix.  The current
qualification route uses v2: one registered M01 production-path run.  Both
schemas remain readable so historical evidence can still be audited, but only
the manifest supplied by the current candidate is allowed to drive a replay.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any


CASES = (
    "M01", "M02", "M03", "M04", "M05", "M06", "M07", "M08", "M09",
    "M10", "M11", "M12", "M13", "M14",
)
CURRENT_SCHEMA = "spec175-host-minindn-manifest-v2"
CURRENT_CASES = ("M01",)
WORKLOAD_SEED = 1750001
FAULT_SEED = 1750002
FAULT_CASES = {"M05", "M06", "M07", "M08", "M09"}


class HostGateError(ValueError):
    """The supplied host qualification is not an accepted G3 subject."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def validate_host_gate(path: Path, repository_root: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HostGateError(f"HOST_GATE_MANIFEST_INVALID:{type(exc).__name__}") from exc

    schema = value.get("schema")
    if schema not in {"spec175-g3-host-minindn-manifest-v1", CURRENT_SCHEMA}:
        raise HostGateError("HOST_GATE_SCHEMA_MISMATCH")
    if value.get("status") != "PASS":
        raise HostGateError("HOST_GATE_STATUS_NOT_PASS")
    subject = value.get("subject")
    matrix = value.get("matrix")
    if not isinstance(subject, dict) or not isinstance(matrix, dict):
        raise HostGateError("HOST_GATE_SUBJECT_OR_MATRIX_MISSING")
    required_subject = {
        "runtime": "tiny-onnx",
        "providerCount": 4,
        "admissionControl": False,
        "targetedPrefetch": False,
        "workloadSeed": WORKLOAD_SEED,
    }
    for key, expected in required_subject.items():
        if subject.get(key) != expected:
            raise HostGateError(f"HOST_GATE_SUBJECT_MISMATCH:{key}")
    if schema == CURRENT_SCHEMA:
        expected_cases = list(CURRENT_CASES)
        expected_repetitions = 1
        expected_entries = 1
    else:
        expected_cases = list(CASES)
        expected_repetitions = 3
        expected_entries = 42
    if matrix.get("cases") != expected_cases:
        raise HostGateError("HOST_GATE_CASE_SET_MISMATCH")
    if matrix.get("repetitionsPerCase") != expected_repetitions:
        raise HostGateError("HOST_GATE_REPETITION_MISMATCH")
    entries = matrix.get("entries")
    if not isinstance(entries, list) or len(entries) != expected_entries:
        raise HostGateError("HOST_GATE_ENTRY_COUNT_MISMATCH")
    counts = {case: 0 for case in expected_cases}
    for entry in entries:
        case = entry.get("case") if isinstance(entry, dict) else None
        if case not in counts:
            raise HostGateError("HOST_GATE_ENTRY_CASE_INVALID")
        counts[case] += 1
        expected_seed = (WORKLOAD_SEED if schema == CURRENT_SCHEMA else
                         (FAULT_SEED if case in FAULT_CASES else WORKLOAD_SEED))
        expected_campaign = f"spec175-{case}-{expected_seed}"
        if entry.get("campaignId") != expected_campaign:
            raise HostGateError("HOST_GATE_WORKLOAD_SEED_MISMATCH")
        if entry.get("workloadSeed") != WORKLOAD_SEED:
            raise HostGateError("HOST_GATE_WORKLOAD_SEED_FIELD_MISMATCH")
        if schema == CURRENT_SCHEMA:
            if entry.get("repetition") != 1:
                raise HostGateError("HOST_GATE_REPETITION_INDEX_MISMATCH")
            if entry.get("status", "PASS") != "PASS":
                raise HostGateError("HOST_GATE_ENTRY_STATUS_NOT_PASS")
            if entry.get("expectedTerminal") is not False:
                raise HostGateError("HOST_GATE_EXPECTED_TERMINAL_MISMATCH")
    if counts != {case: expected_repetitions for case in expected_cases}:
        raise HostGateError("HOST_GATE_CASE_REPETITIONS_INCOMPLETE")
    for key in ("sourceSealPath", "sourceSealSha256", "fixtureManifestPath", "fixtureManifestSha256"):
        if not isinstance(subject.get(key), str) or not subject[key]:
            raise HostGateError(f"HOST_GATE_SUBJECT_DIGEST_MISSING:{key}")

    def resolve_subject(path_value: str) -> Path:
        candidate = (repository_root / path_value).resolve()
        try:
            candidate.relative_to(repository_root.resolve())
        except ValueError as exc:
            raise HostGateError("HOST_GATE_SUBJECT_PATH_ESCAPES_REPOSITORY") from exc
        return candidate

    for path_key, digest_key in (
        ("sourceSealPath", "sourceSealSha256"),
        ("fixtureManifestPath", "fixtureManifestSha256"),
    ):
        candidate = resolve_subject(subject[path_key])
        if not candidate.is_file():
            raise HostGateError(f"HOST_GATE_SUBJECT_FILE_MISSING:{path_key}")
        if sha256(candidate) != subject[digest_key]:
            raise HostGateError(f"HOST_GATE_SUBJECT_DIGEST_MISMATCH:{path_key}")

    return {
        "path": str(path.resolve()),
        "sha256": sha256(path),
        "schema": schema,
        "status": value["status"],
        "total": len(entries),
        "passed": len(entries),
        "cases": expected_cases,
        "repetitionsPerCase": expected_repetitions,
        "entries": entries,
        "subject": {
            key: subject[key]
            for key in required_subject
        },
        "sourceSealPath": subject["sourceSealPath"],
        "sourceSealSha256": subject["sourceSealSha256"],
        "fixtureManifestPath": subject["fixtureManifestPath"],
        "fixtureManifestSha256": subject["fixtureManifestSha256"],
    }

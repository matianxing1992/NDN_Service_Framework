#!/usr/bin/env python3
"""Spec 110 distributed execution lifecycle."""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import re
from typing import Any, Mapping


CELL_RE = re.compile(r"^spec110-cell-[0-9a-f]{20}$")
DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
FAILURE_BOUNDARIES = {
    "stage-load", "stage-execution", "dependency-transfer", "terminal-response"
}
TRANSITIONS = {
    "PLANNED": {"PREFLIGHT_BLOCKED", "READY_TO_SUBMIT"},
    "PREFLIGHT_BLOCKED": {"READY_TO_SUBMIT"},
    "READY_TO_SUBMIT": {"SUBMITTED_NOT_STARTED"},
    "SUBMITTED_NOT_STARTED": {"CANDIDATE_EXECUTION_STARTED", "EVIDENCE_INCOMPLETE"},
    "CANDIDATE_EXECUTION_STARTED": {"EXECUTED_PASS", "EXECUTED_FAIL", "EVIDENCE_INCOMPLETE"},
    "EXECUTED_PASS": set(),
    "EXECUTED_FAIL": set(),
    "EVIDENCE_INCOMPLETE": set(),
}


class ExecutionStateError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise ExecutionStateError(code + (f":{detail}" if detail else ""))


def _timestamp() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def new_execution_state(cell_id: str) -> dict[str, Any]:
    if not isinstance(cell_id, str) or CELL_RE.fullmatch(cell_id) is None:
        _fail("EXECUTION_CELL_ID_INVALID")
    return {
        "schemaVersion": "spec110-execution-state-v1",
        "cellId": cell_id,
        "state": "PLANNED",
        "executionBoundaryReached": False,
        "history": [{"state": "PLANNED", "evidence": {}, "observedAt": _timestamp()}],
    }


def transition(
    record: Mapping[str, object], target: str, evidence: Mapping[str, object]
) -> dict[str, Any]:
    if record.get("schemaVersion") != "spec110-execution-state-v1":
        _fail("EXECUTION_RECORD_INVALID")
    current = record.get("state")
    if current not in TRANSITIONS or target not in TRANSITIONS[current]:
        _fail("EXECUTION_TRANSITION_INVALID", f"{current}->{target}")
    if not isinstance(evidence, Mapping):
        _fail("EXECUTION_EVIDENCE_INVALID")
    if target == "PREFLIGHT_BLOCKED" and not evidence.get("reasonCode"):
        _fail("PREFLIGHT_REASON_REQUIRED")
    if target == "READY_TO_SUBMIT" and (
        not isinstance(evidence.get("preflightDigest"), str)
        or DIGEST_RE.fullmatch(str(evidence["preflightDigest"])) is None
    ):
        _fail("PREFLIGHT_DIGEST_REQUIRED")
    if target == "SUBMITTED_NOT_STARTED" and not evidence.get("jobId"):
        _fail("SUBMISSION_JOB_ID_REQUIRED")
    if target == "CANDIDATE_EXECUTION_STARTED" and (
        not isinstance(evidence.get("executionProofDigest"), str)
        or DIGEST_RE.fullmatch(str(evidence["executionProofDigest"])) is None
    ):
        _fail("EXECUTION_PROOF_REQUIRED")
    if target == "EXECUTED_FAIL" and evidence.get("failureBoundary") not in FAILURE_BOUNDARIES:
        _fail("FAILURE_BOUNDARY_REQUIRED")
    if target == "EXECUTED_PASS" and evidence.get("failureBoundary") is not None:
        _fail("PASS_FAILURE_BOUNDARY_FORBIDDEN")
    result = deepcopy(dict(record))
    result["state"] = target
    if target == "CANDIDATE_EXECUTION_STARTED":
        result["executionBoundaryReached"] = True
    result["history"].append({
        "state": target, "evidence": deepcopy(dict(evidence)), "observedAt": _timestamp()
    })
    return result


def can_close_live_task(record: Mapping[str, object]) -> bool:
    state = record.get("state")
    if state not in {"EXECUTED_PASS", "EXECUTED_FAIL"}:
        return False
    if record.get("executionBoundaryReached") is not True:
        return False
    history = record.get("history")
    if not isinstance(history, list) or not history:
        return False
    final_evidence = history[-1].get("evidence", {})
    if state == "EXECUTED_FAIL":
        return final_evidence.get("failureBoundary") in FAILURE_BOUNDARIES
    return final_evidence.get("placementSemanticsValid") is True


__all__ = [
    "ExecutionStateError", "FAILURE_BOUNDARIES", "TRANSITIONS",
    "can_close_live_task", "new_execution_state", "transition",
]

#!/usr/bin/env python3
"""Candidate-bound GPU stage and dependency execution proof for Spec 110."""

from __future__ import annotations

import hashlib
import json
from typing import Any, Mapping


FAILURE_BOUNDARIES = {
    "stage-load", "stage-execution", "dependency-transfer", "terminal-response"
}


class ExecutionProofError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise ExecutionProofError(code + (f":{detail}" if detail else ""))


def _digest(value: object) -> str:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def validate_execution_start(value: Mapping[str, object]) -> dict[str, Any]:
    required = {
        "candidateId", "runId", "cellId", "jobId", "placementClass", "plane",
        "readinessAtNs", "request", "security", "allocatedGpus", "nfds",
        "providerProcesses", "stageStarts", "dependencies", "promotion", "generation",
    }
    if not isinstance(value, Mapping) or set(value) != required:
        _fail("EXECUTION_PROOF_FIELDS_INVALID")
    if value["plane"] != "distributed-candidate":
        _fail("EXECUTION_BOUNDARY_STANDALONE_ONLY")
    request = value["request"]
    if not isinstance(request, Mapping) or request.get("accepted") is not True:
        _fail("EXECUTION_BOUNDARY_REQUEST_NOT_ACCEPTED")
    for field in ("requestId", "sessionId", "acceptedAtNs"):
        if field not in request:
            _fail("EXECUTION_BOUNDARY_REQUEST_INCOMPLETE", field)
    security = value["security"]
    security_fields = (
        "permissionEncrypted", "nacAbeVerified", "userTokenVerified",
        "providerTokenVerified", "providerPermissionVerified",
    )
    if not isinstance(security, Mapping) or any(security.get(field) is not True for field in security_fields):
        _fail("EXECUTION_BOUNDARY_SECURITY_INCOMPLETE")
    starts = value["stageStarts"]
    if not isinstance(starts, list) or not starts:
        _fail("EXECUTION_BOUNDARY_NOT_REACHED")
    allocated = {
        row["uuid"]: row
        for row in value["allocatedGpus"]
        if isinstance(row, Mapping) and row.get("uuid")
    }
    processes = {
        row["identity"]: row
        for row in value["providerProcesses"]
        if isinstance(row, Mapping) and row.get("identity")
    }
    if not allocated:
        _fail("EXECUTION_PROOF_GPU_ALLOCATION_MISSING")
    if not processes:
        _fail("EXECUTION_PROOF_PROVIDER_PROCESS_MISSING")
    readiness = value["readinessAtNs"]
    if not isinstance(readiness, int) or isinstance(readiness, bool):
        _fail("EXECUTION_PROOF_TIME_INVALID")
    for start in starts:
        if not isinstance(start, Mapping):
            _fail("EXECUTION_PROOF_STAGE_INVALID")
        process = processes.get(start.get("providerIdentity"))
        if process is None:
            _fail("EXECUTION_PROOF_PROVIDER_STALE")
        if (
            process.get("pid") != start.get("providerPid")
            or process.get("role") != start.get("role")
            or process.get("node") != start.get("node")
            or process.get("gpuUuid") != start.get("gpuUuid")
        ):
            _fail("EXECUTION_PROOF_PROCESS_MISMATCH", str(start.get("role")))
        gpu = allocated.get(start.get("gpuUuid"))
        if gpu is None or gpu.get("node") != start.get("node"):
            _fail("EXECUTION_PROOF_GPU_NOT_ALLOCATED", str(start.get("role")))
        if start.get("backend") != "onnxruntime-cuda":
            _fail("EXECUTION_PROOF_CPU_FALLBACK", str(start.get("role")))
        started = start.get("startedAtNs")
        if not isinstance(started, int) or started < readiness or started < request["acceptedAtNs"]:
            _fail("EXECUTION_PROOF_TIME_ORDER_INVALID", str(start.get("role")))
    nodes = {row["node"] for row in value["allocatedGpus"]}
    nfd_nodes = {row["node"] for row in value["nfds"]}
    if nodes != nfd_nodes:
        _fail("EXECUTION_PROOF_NFD_PER_NODE_INVALID")
    providers = {row["providerIdentity"] for row in starts}
    for edge in value["dependencies"]:
        if edge.get("fromProvider") not in providers or edge.get("toProvider") not in providers:
            _fail("EXECUTION_PROOF_DEPENDENCY_PROVIDER_INVALID")
        if edge.get("fromNode") == edge.get("toNode") and edge.get("crossNode") is True:
            _fail("EXECUTION_PROOF_DEPENDENCY_NODE_INVALID")
        if edge.get("fromNode") != edge.get("toNode") and edge.get("crossNode") is not True:
            _fail("EXECUTION_PROOF_DEPENDENCY_NODE_INVALID")
    return {
        "status": "PASS",
        "executionBoundaryReached": True,
        "startedStageCount": len(starts),
        "proofDigest": _digest(value),
    }


def validate_complete_dataflow(value: Mapping[str, object]) -> dict[str, Any]:
    report = validate_execution_start(value)
    starts = value["stageStarts"]
    if (
        {row["role"] for row in starts} != {"/LLM/Stage/0", "/LLM/Stage/1", "/LLM/Stage/2"}
        or len({row["providerIdentity"] for row in starts}) != 3
        or len({row["providerPid"] for row in starts}) != 3
        or len({row["gpuUuid"] for row in starts}) != 3
    ):
        _fail("EXECUTION_PROOF_DATAFLOW_INCOMPLETE")
    if value["placementClass"] == "multi-node":
        if not any(edge.get("crossNode") is True for edge in value["dependencies"]):
            _fail("EXECUTION_PROOF_CROSS_NODE_EDGE_MISSING")
    elif value["placementClass"] != "single-node-multi-gpu":
        _fail("EXECUTION_PROOF_PLACEMENT_INVALID")
    generation = value["generation"]
    if (
        generation.get("terminalResponseCount") != 1
        or generation.get("outputTokenIds") != generation.get("oracleTokenIds")
    ):
        _fail("EXECUTION_PROOF_TERMINAL_INVALID")
    return {**report, "completeDataflow": True}


def closure_decision(
    value: Mapping[str, object], state: str, failure_boundary: str | None = None
) -> dict[str, Any]:
    try:
        start = validate_execution_start(value)
    except ExecutionProofError as error:
        return {
            "canCloseLiveTask": False,
            "state": "PREFLIGHT_BLOCKED",
            "reasonCode": str(error).split(":", 1)[0],
        }
    promotion = value.get("promotion")
    if not isinstance(promotion, Mapping) or promotion.get("complete") is not True:
        return {
            "canCloseLiveTask": False,
            "state": "EVIDENCE_INCOMPLETE",
            "reasonCode": "EXECUTION_EVIDENCE_PROMOTION_INCOMPLETE",
            **start,
        }
    if state == "EXECUTED_FAIL":
        if failure_boundary not in FAILURE_BOUNDARIES:
            _fail("EXECUTION_PROOF_FAILURE_BOUNDARY_INVALID")
        return {"canCloseLiveTask": True, "state": state, "failureBoundary": failure_boundary, **start}
    if state == "EXECUTED_PASS":
        complete = validate_complete_dataflow(value)
        return {"canCloseLiveTask": True, "state": state, **complete}
    return {"canCloseLiveTask": False, "state": state, "reasonCode": "EXECUTION_STATE_NOT_TERMINAL", **start}


__all__ = [
    "ExecutionProofError", "closure_decision", "validate_complete_dataflow",
    "validate_execution_start",
]

#!/usr/bin/env python3
"""Schema and semantic validation for Spec 110 distributed evidence."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
from typing import Any, Mapping

from jsonschema import Draft202012Validator


ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = ROOT / "specs/110-itiger-qwen-live-inference/contracts/run-evidence.schema.json"


class EvidenceValidationError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise EvidenceValidationError(code + (f":{detail}" if detail else ""))


def _digest(value: object) -> str:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def _schema_validator() -> Draft202012Validator:
    return Draft202012Validator(json.loads(SCHEMA_PATH.read_text(encoding="utf-8")))


def _validate_schema(value: Mapping[str, object]) -> None:
    errors = sorted(_schema_validator().iter_errors(value), key=lambda item: list(item.path))
    if errors:
        error = errors[0]
        path = ".".join(str(item) for item in error.path) or "$"
        _fail("SCHEMA_INVALID", f"{path}:{error.message}")


def _reject_legacy(value: object, field: str) -> None:
    if isinstance(value, str) and value.lower().startswith("spec109"):
        _fail("EVIDENCE_LEGACY_IDENTITY", field)


def validate_evidence(
    value: Mapping[str, object], *, expected_sif_sha256: str | None = None
) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        _fail("EVIDENCE_NOT_OBJECT")
    _validate_schema(value)
    for field in ("runId", "cellId", "candidateId"):
        _reject_legacy(value.get(field), field)
    submission = value["submission"]
    _reject_legacy(submission.get("submissionId"), "submissionId")
    release = value["release"]
    if expected_sif_sha256 is not None and release.get("sifSha256") != expected_sif_sha256:
        _fail("EVIDENCE_SIF_MISMATCH")

    allocation = value["allocation"]
    topology = value["topology"]
    stages = value["stages"]
    nodes = set(allocation["nodes"])
    gpu_rows = {row["uuid"]: row for row in allocation["allocatedGpus"]}
    if len(gpu_rows) != len(allocation["allocatedGpus"]):
        _fail("EVIDENCE_GPU_UUID_DISTINCTNESS")

    provider_processes = [row for row in topology["processes"] if row["kind"] == "provider"]
    if len(provider_processes) != 3:
        _fail("EVIDENCE_PROVIDER_PROCESS_COUNT")
    process_by_identity = {row["identityRef"]: row for row in provider_processes}
    if len(process_by_identity) != 3:
        _fail("EVIDENCE_PROVIDER_IDENTITY_DISTINCTNESS")

    roles = [row["role"] for row in stages]
    providers = [row["provider"] for row in stages]
    pids = [row["providerPid"] for row in stages]
    gpu_uuids = [row["gpuUuid"] for row in stages]
    if len(set(roles)) != 3:
        _fail("EVIDENCE_STAGE_ROLE_DISTINCTNESS")
    if len(set(providers)) != 3:
        _fail("EVIDENCE_PROVIDER_DISTINCTNESS")
    if len(set(pids)) != 3:
        _fail("EVIDENCE_PROVIDER_PID_DISTINCTNESS")
    if len(set(gpu_uuids)) != 3:
        _fail("EVIDENCE_STAGE_GPU_DISTINCTNESS")
    for stage in stages:
        process = process_by_identity.get(stage["provider"])
        if process is None:
            _fail("EVIDENCE_PROVIDER_IDENTITY_STALE", stage["provider"])
        if (
            process["pid"] != stage["providerPid"]
            or process["role"] != stage["role"]
            or process["node"] != stage["node"]
            or process["gpuUuid"] != stage["gpuUuid"]
        ):
            _fail("EVIDENCE_STAGE_PROCESS_MISMATCH", stage["role"])
        gpu = gpu_rows.get(stage["gpuUuid"])
        if gpu is None or gpu["node"] != stage["node"]:
            _fail("EVIDENCE_STAGE_GPU_NOT_ALLOCATED", stage["role"])
        if stage["node"] not in nodes:
            _fail("EVIDENCE_STAGE_NODE_NOT_ALLOCATED", stage["role"])

    nfd_nodes = [row["node"] for row in topology["nfds"]]
    if len(nfd_nodes) != len(nodes) or set(nfd_nodes) != nodes:
        _fail("EVIDENCE_NFD_PER_NODE_INVALID")
    placement = value["placementClass"]
    edges = topology["crossNodeEdges"]
    if placement == "single-node-multi-gpu":
        if len(nodes) != 1 or edges:
            _fail("EVIDENCE_SINGLE_NODE_TOPOLOGY_INVALID")
    else:
        if len(nodes) < 2 or not edges:
            _fail("EVIDENCE_MULTI_NODE_TOPOLOGY_INVALID")
        if not any(edge["fromNode"] != edge["toNode"] for edge in edges):
            _fail("EVIDENCE_CROSS_NODE_EDGE_INVALID")

    generation = value["generation"]
    state = value["state"]
    if state == "EXECUTED_PASS":
        if generation["outputTokenIds"] != generation["oracleTokenIds"]:
            _fail("EVIDENCE_TOKEN_MISMATCH")
        if any(stage["outcome"] != "PASS" or stage["executionStarted"] is not True for stage in stages):
            _fail("EVIDENCE_PASS_STAGE_INCOMPLETE")
    elif state == "EXECUTED_FAIL":
        all_stages_pass = all(stage["outcome"] == "PASS" for stage in stages)
        exact_terminal = (
            generation["terminalResponseCount"] == 1
            and generation["outputTokenIds"] == generation["oracleTokenIds"]
        )
        if all_stages_pass and exact_terminal:
            _fail("EVIDENCE_FAILURE_NARRATION_CONTRADICTORY")
    if value["authority"]["physicalProduction"] != "DEFERRED":
        _fail("EVIDENCE_FALSE_PHYSICAL_AUTHORITY")

    return {
        "status": "PASS",
        "runId": value["runId"],
        "cellId": value["cellId"],
        "candidateId": value["candidateId"],
        "placementClass": placement,
        "stageCount": len(stages),
        "nodeCount": len(nodes),
        "evidenceDigest": _digest(value),
    }


def load_and_validate(
    path: Path | str, *, expected_sif_sha256: str | None = None
) -> dict[str, Any]:
    value = json.loads(Path(path).read_text(encoding="utf-8"))
    return validate_evidence(value, expected_sif_sha256=expected_sif_sha256)


__all__ = ["EvidenceValidationError", "load_and_validate", "validate_evidence"]

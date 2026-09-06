#!/usr/bin/env python3
"""Collect the candidate-bound Spec180 terminal result from raw run evidence.

The collector is deliberately downstream of process cleanup.  It does not
decide that a run succeeded from a marker or from a response alone: it binds
the lifecycle journal, numerical oracle, observed provider records, actual
ORT profiles, and supervisor record before producing the result oracles.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
from typing import Any, Mapping


RESULT_SCHEMA = "spec180-result-v1"
ORACLE_SCHEMA = "spec180-oracle-v1"
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_GPU_RE = re.compile(r"^GPU-[A-Za-z0-9._:-]+$")
_MILESTONES = (
    "INPUT_REFERENCE_PUBLISHED", "REQUEST_SENT", "ACK_CLOSED",
    "GRAPH_READY", "PLACEMENT_DECISION", "ARTIFACTS_READY", "PLAN_SEALED",
    "SELECTION_COMMITTED", "PROVIDER_EXECUTION_STARTED", "TERMINAL_RESPONSE",
)
_MILESTONE_FIELDS = {
    "INPUT_REFERENCE_PUBLISHED": {"referenceDigest"},
    "REQUEST_SENT": {"requestDigest"},
    "ACK_CLOSED": {"ackSnapshotDigest", "ackCount"},
    "GRAPH_READY": {"graphDigest", "catalogueDigest"},
    "PLACEMENT_DECISION": {
        "candidateId", "candidateDigest", "candidatePriority", "providerCount",
    },
    "ARTIFACTS_READY": {"artifactDigest", "artifactCount"},
    "PLAN_SEALED": {"planDigest"},
    "SELECTION_COMMITTED": {"selectionDigest", "selectedRoleCount"},
    "PROVIDER_EXECUTION_STARTED": {"roleDigest", "providerCount"},
    "TERMINAL_RESPONSE": {"resultDigest", "requestCount", "status"},
}
_COMMON_EVENT_FIELDS = {
    "schema", "caseId", "requestId", "attemptId", "sequence",
    "timestampUnix", "milestone",
}
_ROLES = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
_SECRET_RE = re.compile(
    r"(?i)(BEGIN[ _-]+(?:RSA |EC |OPENSSH )?PRIVATE KEY|"
    r"password\s*[=:]|secret\s*[=:]|providerToken\s*[=:]|"
    r"userToken\s*[=:]|plaintext|input_bytes|result_bytes)"
)
_OBSERVED_PREFIX = "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED "
REPO_ROOT = Path(__file__).resolve().parents[4]


class CollectionError(ValueError):
    """Raised when raw evidence cannot prove the fixed Spec180 contract."""


def _digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _DIGEST_RE.fullmatch(value):
        raise CollectionError("INVALID_DIGEST:" + label)
    return value


def _object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CollectionError("DUPLICATE_JSON_FIELD:" + key)
        result[key] = value
    return result


def _json_bytes(path: Path, label: str) -> tuple[Any, bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise CollectionError(label + "_READ_FAILED") from exc
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_object_pairs)
    except (UnicodeError, json.JSONDecodeError, CollectionError) as exc:
        raise CollectionError(label + "_JSON_INVALID") from exc
    return value, raw


def _require_object(value: Any, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise CollectionError(label + "_NOT_OBJECT")
    return value


def _secret_scan(value: Any, path: str = "") -> list[str]:
    findings: list[str] = []
    if isinstance(value, Mapping):
        for key, child in value.items():
            key_path = f"{path}.{key}" if path else str(key)
            if _SECRET_RE.search(str(key)):
                findings.append(key_path)
            findings.extend(_secret_scan(child, key_path))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            findings.extend(_secret_scan(child, f"{path}[{index}]"))
    elif isinstance(value, str) and _SECRET_RE.search(value):
        findings.append(path)
    return findings


def _read_lifecycle(root: Path, candidate_id: str, candidate_digest: str) -> dict[str, Any]:
    path = root / "lifecycle.jsonl"
    try:
        lines = [line for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    except (OSError, UnicodeError) as exc:
        raise CollectionError("LIFECYCLE_READ_FAILED") from exc
    if len(lines) != len(_MILESTONES):
        raise CollectionError("LIFECYCLE_EVENT_COUNT_MISMATCH")
    events: list[Mapping[str, Any]] = []
    for index, line in enumerate(lines):
        try:
            event = json.loads(line, object_pairs_hook=_object_pairs)
        except (UnicodeError, json.JSONDecodeError, CollectionError) as exc:
            raise CollectionError("LIFECYCLE_JSON_INVALID:" + str(index)) from exc
        event = _require_object(event, "LIFECYCLE_EVENT")
        milestone = event.get("milestone")
        if milestone != _MILESTONES[index]:
            raise CollectionError("LIFECYCLE_ORDER_MISMATCH:" + str(index))
        expected = _COMMON_EVENT_FIELDS | _MILESTONE_FIELDS[milestone]
        if set(event) != expected:
            raise CollectionError("LIFECYCLE_FIELDS_MISMATCH:" + milestone)
        if event["schema"] != "spec180-yolo-lifecycle-event-v1" or event["caseId"] != "Y-B":
            raise CollectionError("LIFECYCLE_IDENTITY_MISMATCH:" + milestone)
        if not isinstance(event["requestId"], str) or not event["requestId"]:
            raise CollectionError("LIFECYCLE_REQUEST_ID_MISSING")
        if not isinstance(event["attemptId"], str) or not event["attemptId"]:
            raise CollectionError("LIFECYCLE_ATTEMPT_ID_MISSING")
        if event["sequence"] != index or not isinstance(event["timestampUnix"], (int, float)):
            raise CollectionError("LIFECYCLE_SEQUENCE_INVALID:" + milestone)
        events.append(event)
    request_id = events[0]["requestId"]
    attempt_id = events[0]["attemptId"]
    if any(event["requestId"] != request_id or event["attemptId"] != attempt_id
           for event in events):
        raise CollectionError("LIFECYCLE_PROTOCOL_IDENTITY_MISMATCH")
    placement = events[4]
    if placement["candidateId"] != candidate_id or placement["candidateDigest"] != candidate_digest:
        raise CollectionError("LIFECYCLE_CANDIDATE_MISMATCH")
    plan_digest = _digest(events[6]["planDigest"], "lifecycle.planDigest")
    terminal = events[-1]
    if terminal["status"] is not True or terminal["requestCount"] != 1:
        raise CollectionError("LIFECYCLE_TERMINAL_NOT_PASS")
    _digest(terminal["resultDigest"], "lifecycle.resultDigest")
    findings = _secret_scan(events)
    if findings:
        raise CollectionError("REDACTION_SECRET_FINDING:" + findings[0])
    return {
        "events": events,
        "requestId": request_id,
        "attemptId": attempt_id,
        "planDigest": plan_digest,
        "resultDigest": terminal["resultDigest"],
    }


def _read_numerical(root: Path, lifecycle: Mapping[str, Any], candidate_id: str,
                    candidate_digest: str) -> Mapping[str, Any]:
    value, _ = _json_bytes(root / "yolo-numerical.json", "NUMERICAL")
    value = _require_object(value, "NUMERICAL")
    if value.get("schemaVersion") != "spec180-yolo-numerical-v1":
        raise CollectionError("NUMERICAL_SCHEMA_UNSUPPORTED")
    if value.get("case") != "Y-B" or value.get("requestId") != lifecycle["requestId"]:
        raise CollectionError("NUMERICAL_REQUEST_MISMATCH")
    if value.get("attemptId") != lifecycle["attemptId"] or value.get("planDigest") != lifecycle["planDigest"]:
        raise CollectionError("NUMERICAL_LINEAGE_MISMATCH")
    if value.get("candidateId") != candidate_id or value.get("candidateDigest") != candidate_digest:
        raise CollectionError("NUMERICAL_CANDIDATE_MISMATCH")
    if value.get("matched") is not True:
        raise CollectionError("NUMERICAL_ORACLE_NOT_PASS")
    _digest(value.get("responseDigest"), "numerical.responseDigest")
    if value["responseDigest"] != lifecycle["resultDigest"]:
        raise CollectionError("NUMERICAL_RESPONSE_DIGEST_MISMATCH")
    if _secret_scan(value):
        raise CollectionError("REDACTION_SECRET_FINDING: numerical")
    return value


def _relative_file(root: Path, raw_path: Any, label: str) -> tuple[str, Path]:
    if not isinstance(raw_path, str) or not raw_path:
        raise CollectionError(label + "_PATH_MISSING")
    root_resolved = root.resolve()
    candidate = Path(raw_path)
    resolved = (candidate if candidate.is_absolute() else root / candidate).resolve()
    try:
        relative = resolved.relative_to(root_resolved)
    except ValueError as exc:
        raise CollectionError(label + "_PATH_ESCAPES_EVIDENCE") from exc
    if not resolved.is_file() or any(part in {"", ".", ".."} for part in relative.parts):
        raise CollectionError(label + "_FILE_MISSING")
    return relative.as_posix(), resolved


def _read_observation(log: Path, role: str, request_id: str, plan_digest: str,
                      expected_pid: int, root: Path) -> dict[str, Any]:
    try:
        lines = log.read_text(encoding="utf-8", errors="strict").splitlines()
    except (OSError, UnicodeError) as exc:
        raise CollectionError("EXECUTION_LOG_READ_FAILED:" + role) from exc
    observations: list[Mapping[str, Any]] = []
    for line in lines:
        if not line.startswith(_OBSERVED_PREFIX):
            continue
        try:
            value = json.loads(line[len(_OBSERVED_PREFIX):], object_pairs_hook=_object_pairs)
        except (UnicodeError, json.JSONDecodeError, CollectionError) as exc:
            raise CollectionError("EXECUTION_OBSERVATION_JSON_INVALID:" + role) from exc
        value = _require_object(value, "EXECUTION_OBSERVATION")
        if value.get("roles") == [role] and value.get("requestId") == request_id:
            observations.append(value)
    if len(observations) != 1:
        raise CollectionError("EXECUTION_OBSERVATION_COUNT_MISMATCH:" + role)
    value = observations[0]
    if value.get("schema") != "ndnsf-di-execution-evidence-v1":
        raise CollectionError("EXECUTION_SCHEMA_UNSUPPORTED:" + role)
    if value.get("providerName") != "/example/provider/" + role:
        raise CollectionError("EXECUTION_PROVIDER_MISMATCH:" + role)
    if value.get("processId") != expected_pid or type(expected_pid) is not int or expected_pid <= 0:
        raise CollectionError("EXECUTION_PID_MISMATCH:" + role)
    if value.get("attemptEpoch", 0) <= 0 or value.get("executionCompleted") is not True:
        raise CollectionError("EXECUTION_NOT_COMPLETED:" + role)
    if value.get("exactForwardCacheHit") is not False:
        raise CollectionError("EXECUTION_CACHE_HIT:" + role)
    if value.get("planDigest") != plan_digest or value.get("loadCompleted") is not True or value.get("warmupCompleted") is not True:
        raise CollectionError("EXECUTION_PLAN_OR_WARMUP_MISMATCH:" + role)
    if value.get("realCompute") is not (role != "Merge"):
        raise CollectionError("EXECUTION_COMPUTE_CLASSIFICATION:" + role)
    if role == "Merge":
        if value.get("runnerKind") != "native-yolo-postprocess" or value.get("gpuUuid") != "" or value.get("cudaVisibleDevices") != "":
            raise CollectionError("EXECUTION_MERGE_DEVICE_MISMATCH")
        if value.get("nodeProviderAssignments") not in ([], None):
            raise CollectionError("EXECUTION_MERGE_NODE_ASSIGNMENT")
        profile_path = ""
        profile_digest = ""
        gpu_uuid = ""
        visible = ""
        identity_source = ""
        node_providers: list[str] = []
    else:
        gpu_uuid = value.get("gpuUuid")
        visible = value.get("cudaVisibleDevices")
        identity_source = value.get("gpuIdentitySource")
        if value.get("runnerKind") != "onnxruntime-cuda" or value.get("realCompute") is not True:
            raise CollectionError("EXECUTION_CUDA_RUNNER_MISMATCH:" + role)
        if not isinstance(gpu_uuid, str) or not _GPU_RE.fullmatch(gpu_uuid) or visible != "0":
            raise CollectionError("EXECUTION_CUDA_DEVICE_MISMATCH:" + role)
        if identity_source != "cuda-runtime-pci+driver-uuid" or value.get("cpuFallbackUsed") is not False:
            raise CollectionError("EXECUTION_CUDA_IDENTITY_MISMATCH:" + role)
        assignments = value.get("nodeProviderAssignments")
        if not isinstance(assignments, list) or not assignments:
            raise CollectionError("EXECUTION_NODE_ASSIGNMENTS_MISSING:" + role)
        node_providers = []
        for assignment in assignments:
            if (not isinstance(assignment, Mapping) or assignment.get("role") != role or
                    assignment.get("modelNode") is not True or
                    assignment.get("provider") != "CUDAExecutionProvider"):
                raise CollectionError("EXECUTION_NODE_PROVIDER_MISMATCH:" + role)
            node_providers.append(assignment["provider"])
        profile_path, profile = _relative_file(root, value.get("providerProfilePath"), "PROFILE_" + role)
        profile_digest = "sha256:" + hashlib.sha256(profile.read_bytes()).hexdigest()
        if value.get("profileRequestId") != request_id or value.get("profileAttemptEpoch") != value.get("attemptEpoch"):
            raise CollectionError("PROFILE_LINEAGE_MISMATCH:" + role)
    if _secret_scan(value):
        raise CollectionError("REDACTION_SECRET_FINDING: execution-" + role)
    return {
        "role": role,
        "provider": value["providerName"],
        "pid": expected_pid,
        "planDigest": plan_digest,
        "requestId": request_id,
        "attemptEpoch": value["attemptEpoch"],
        "runnerKind": value["runnerKind"],
        "realCompute": value["realCompute"],
        "executionCompleted": True,
        "exactForwardCacheHit": False,
        "physicalDeviceUuid": gpu_uuid,
        "cudaVisibleDevices": visible,
        "gpuIdentitySource": identity_source,
        "profileRequestId": value.get("profileRequestId", ""),
        "profileAttemptEpoch": value.get("profileAttemptEpoch", 0),
        "profilePath": profile_path,
        "profileSha256": profile_digest,
        "nodeProviders": node_providers,
    }


def _oracle(name: str, candidate_id: str, fields: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "schema": ORACLE_SCHEMA, "version": 1, "status": "PASS",
        "candidateId": candidate_id, "path": name + ".json", "sha256": "",
        "fields": dict(fields),
    }


def _write_json_exclusive(path: Path, value: Any) -> bytes:
    raw = json.dumps(value, ensure_ascii=False, sort_keys=True,
                     separators=(",", ":")).encode("utf-8")
    with path.open("xb") as stream:
        stream.write(raw)
    return raw


def collect_terminal_result(evidence_root: Path | str, candidate_id: str,
                            candidate_digest: str, supervision: Mapping[str, Any]) -> dict[str, Any]:
    """Collect and atomically publish the terminal result for one run."""
    root = Path(evidence_root).resolve()
    if not root.is_dir() or (root / "spec180-result.json").exists():
        raise CollectionError("EVIDENCE_NOT_FRESH")
    if not isinstance(candidate_id, str) or not candidate_id:
        raise CollectionError("CANDIDATE_ID_REQUIRED")
    candidate_digest = _digest(candidate_digest, "candidateDigest")
    if not isinstance(supervision, Mapping):
        raise CollectionError("SUPERVISION_NOT_OBJECT")
    children = supervision.get("children")
    pids = supervision.get("pids")
    if not isinstance(children, list) or not isinstance(pids, Mapping):
        raise CollectionError("SUPERVISION_RECORD_INCOMPLETE")
    if any(row.get("exitStatus") != 0 or row.get("timedOut") is not False
           for row in children if isinstance(row, Mapping)):
        raise CollectionError("SUPERVISION_CHILD_FAILURE")
    lifecycle = _read_lifecycle(root, candidate_id, candidate_digest)
    numerical = _read_numerical(root, lifecycle, candidate_id, candidate_digest)
    observations = [
        _read_observation(root / "runtime" / "log" / ("provider-" + role + ".log"),
                          role, lifecycle["requestId"], lifecycle["planDigest"],
                          pids.get("provider-" + role), root)
        for role in _ROLES
    ]
    provider_fields = [{
        "role": item["role"], "provider": item["provider"], "pid": item["pid"],
        "executionProvider": ("native-yolo-postprocess" if item["role"] == "Merge"
                               else "CUDAExecutionProvider"),
        "physicalDeviceUuid": item["physicalDeviceUuid"],
        "cudaVisibleDevices": item["cudaVisibleDevices"],
    } for item in observations]
    gpu_ids = sorted({item["physicalDeviceUuid"] for item in observations if item["physicalDeviceUuid"]})
    if len(gpu_ids) != 1:
        raise CollectionError("RUNTIME_GPU_INVENTORY_MISMATCH")
    protocol_fields = {
        "lifecyclePath": "lifecycle.jsonl", "requestCount": 1,
        "terminalResponseCount": 1,
    }
    result_fields = {
        "resultPath": "yolo-numerical.json", "requestCount": 1, "matched": True,
    }
    runtime_fields = {
        "backend": "cuda-onnxruntime", "deviceIds": gpu_ids,
        "cpuFallback": False, "providers": provider_fields,
        "planDigest": lifecycle["planDigest"],
        "executionEvidence": observations,
    }
    redaction_fields = {"secretFindings": 0, "redacted": False}
    cleanup_fields = {
        "childExitCount": supervision.get("childExitCount"),
        "allExited": supervision.get("allExited"),
        "outputClosed": supervision.get("outputClosed"),
    }
    if (cleanup_fields["childExitCount"] != len(children) or
            cleanup_fields["allExited"] is not True or cleanup_fields["outputClosed"] is not True):
        raise CollectionError("SUPERVISION_CLEANUP_INCOMPLETE")
    result = {
        "schema": RESULT_SCHEMA, "candidateId": candidate_id,
        "candidateDigest": candidate_digest, "gate": "yolo-functional", "status": "PASS",
        "requests": {"expected": 1, "completed": 1}, "children": children,
        "protocolOracle": _oracle("protocolOracle", candidate_id, protocol_fields),
        "resultOracle": _oracle("resultOracle", candidate_id, result_fields),
        "runtimeOracle": _oracle("runtimeOracle", candidate_id, runtime_fields),
        "redaction": _oracle("redaction", candidate_id, redaction_fields),
        "cleanup": _oracle("cleanup", candidate_id, cleanup_fields),
    }
    # Validate the staged result with the repository validator before anything
    # becomes visible as the terminal artifact.
    validator_path = REPO_ROOT / "scripts" / "validate_spec180_results.py"
    if not validator_path.is_file():
        raise CollectionError("RESULT_VALIDATOR_MISSING")
    import importlib.util
    spec = importlib.util.spec_from_file_location("spec180_result_validator", validator_path)
    validator = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(validator)
    stage = Path(tempfile.mkdtemp(prefix=".spec180-collect-", dir=root))
    try:
        for observation in observations:
            if observation["profilePath"]:
                source = root / observation["profilePath"]
                target = stage / observation["profilePath"]
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
        for name in ("protocolOracle", "resultOracle", "runtimeOracle", "redaction", "cleanup"):
            oracle = result[name]
            payload = {key: value for key, value in oracle.items() if key not in {"path", "sha256"}}
            raw = _write_json_exclusive(stage / oracle["path"], payload)
            oracle["sha256"] = "sha256:" + hashlib.sha256(raw).hexdigest()
        _write_json_exclusive(stage / "spec180-result.json", result)
        staged_result, _ = _json_bytes(stage / "spec180-result.json", "STAGED_RESULT")
        validator.validate_result(staged_result, candidate_digest, stage)
        for name in ("protocolOracle", "resultOracle", "runtimeOracle", "redaction", "cleanup", "spec180-result"):
            os.replace(stage / (name + ".json"), root / (name + ".json"))
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--candidate-id", required=True)
    parser.add_argument("--candidate-digest", required=True)
    parser.add_argument("--supervision", type=Path, required=True)
    args = parser.parse_args()
    try:
        supervision, _ = _json_bytes(args.supervision, "SUPERVISION")
        collect_terminal_result(args.evidence_root, args.candidate_id,
                                args.candidate_digest, _require_object(supervision, "SUPERVISION"))
    except (CollectionError, OSError, UnicodeError) as exc:
        print(str(exc), file=__import__("sys").stderr)
        return 2
    print("SPEC180_RESULT_COLLECTED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

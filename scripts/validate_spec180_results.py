#!/usr/bin/env python3
"""Validate one candidate-bound Spec180 terminal result manifest.

This validator does not infer success from a Response or a scheduler marker.
It requires protocol, result, runtime, redaction, child-exit, and cleanup
oracles to agree for the one-cold-request, one-GPU YOLO functional gate.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Any, Mapping


RESULT_SCHEMA = "spec180-result-v1"
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_REQUIRED = {
    "schema", "candidateId", "candidateDigest", "gate", "status", "requests",
    "children", "protocolOracle", "resultOracle", "runtimeOracle",
    "redaction", "cleanup",
}
_REQUEST_KEYS = {"expected", "completed"}
_CHILD_KEYS = {"id", "exitStatus", "timedOut"}
_ORACLE_KEYS = {"schema", "version", "status", "candidateId", "path", "sha256", "fields"}
_ORACLE_FIELDS = {
    "protocolOracle": {"lifecyclePath", "requestCount", "terminalResponseCount"},
    "resultOracle": {"resultPath", "requestCount", "matched"},
    "runtimeOracle": {"backend", "deviceIds", "cpuFallback", "providers",
                       "executionEvidence", "planDigest"},
    "redaction": {"secretFindings", "redacted"},
    "cleanup": {"childExitCount", "allExited", "outputClosed"},
}
_ROLES = {"BackboneNeck", "DetectShard0", "DetectShard1", "Merge"}
_CHILD_IDS = {"nfd", "controller", "repo", "user"} | {"provider-" + role for role in _ROLES}
_PROVIDER_KEYS = {"role", "provider", "pid", "executionProvider",
                  "physicalDeviceUuid", "cudaVisibleDevices"}
_EXECUTION_KEYS = {
    "role", "provider", "pid", "requestId", "attemptEpoch", "runnerKind",
    "realCompute", "executionCompleted", "exactForwardCacheHit",
    "physicalDeviceUuid", "cudaVisibleDevices", "gpuIdentitySource",
    "profileRequestId", "profileAttemptEpoch", "profilePath", "profileSha256",
    "nodeProviders", "planDigest",
}


class ResultError(ValueError):
    """Raised when a terminal result cannot prove functional qualification."""


def _keys(value: Mapping[str, Any], expected: set[str], label: str) -> None:
    unknown = sorted(set(value) - expected)
    if unknown:
        raise ResultError("UNKNOWN_" + label + "_FIELD:" + ",".join(unknown))
    missing = sorted(expected - set(value))
    if missing:
        raise ResultError("MISSING_" + label + "_FIELD:" + ",".join(missing))


def _digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _DIGEST_RE.fullmatch(value):
        raise ResultError("INVALID_DIGEST:" + label)
    return value


def _count(value: Any, expected: int) -> bool:
    return type(value) is int and value == expected


def _validate_oracle(name: str, value: Any, candidate_id: str,
                     evidence_root: Path | None = None) -> None:
    if not isinstance(value, Mapping):
        raise ResultError("ORACLE_NOT_OBJECT:" + name)
    _keys(value, _ORACLE_KEYS, "ORACLE")
    if value["schema"] != "spec180-oracle-v1" or value["version"] != 1:
        raise ResultError("ORACLE_SCHEMA_UNSUPPORTED:" + name)
    if value["status"] != "PASS":
        raise ResultError("ORACLE_NOT_PASS:" + name)
    if value["candidateId"] != candidate_id:
        raise ResultError("ORACLE_CANDIDATE_MISMATCH:" + name)
    path = value["path"]
    if (not isinstance(path, str) or not path or path.startswith("/") or
            any(part in {"", ".", ".."} for part in Path(path).parts)):
        raise ResultError("ORACLE_PATH_INVALID:" + name)
    digest = _digest(value["sha256"], "oracle." + name)
    fields = value["fields"]
    if not isinstance(fields, Mapping):
        raise ResultError("ORACLE_FIELDS_NOT_OBJECT:" + name)
    required = _ORACLE_FIELDS[name]
    _keys(fields, required, "ORACLE_FIELDS_" + name)
    if any(isinstance(item, (bytes, bytearray)) for item in fields.values()):
        raise ResultError("ORACLE_FIELDS_BINARY:" + name)
    if evidence_root is not None:
        root = evidence_root.resolve()
        evidence = (root / path).resolve()
        try:
            evidence.relative_to(root)
        except ValueError as exc:
            raise ResultError("ORACLE_PATH_ESCAPES_ROOT:" + name) from exc
        if not evidence.is_file():
            raise ResultError("ORACLE_FILE_MISSING:" + name)
        raw = evidence.read_bytes()
        actual = hashlib.sha256(raw).hexdigest()
        if digest != "sha256:" + actual:
            raise ResultError("ORACLE_DIGEST_MISMATCH:" + name)
        try:
            payload = json.loads(raw)
        except (ValueError, UnicodeError) as exc:
            raise ResultError("ORACLE_CONTENT_INVALID:" + name) from exc
        expected = {key: item for key, item in value.items()
                    if key not in {"path", "sha256"}}
        # Hashing an unrelated file cannot attest the manifest's PASS fields.
        if json.dumps(payload, sort_keys=True) != json.dumps(expected, sort_keys=True):
            raise ResultError("ORACLE_CONTENT_MISMATCH:" + name)


def validate_result(result: Mapping[str, Any], candidate_digest: str,
                    evidence_root: Path) -> bool:
    if not isinstance(result, Mapping):
        raise ResultError("RESULT_NOT_OBJECT")
    _keys(result, _REQUIRED, "RESULT")
    if result["schema"] != RESULT_SCHEMA:
        raise ResultError("RESULT_SCHEMA_UNSUPPORTED")
    expected_digest = _digest(candidate_digest, "expectedCandidateDigest")
    if _digest(result["candidateDigest"], "candidateDigest") != expected_digest:
        raise ResultError("CANDIDATE_DIGEST_MISMATCH")
    if not isinstance(result["candidateId"], str) or not result["candidateId"]:
        raise ResultError("INVALID_CANDIDATE_ID")
    if result["gate"] != "yolo-functional":
        raise ResultError("UNKNOWN_GATE")
    if result["status"] != "PASS":
        raise ResultError("RESULT_NOT_PASS")

    requests = result["requests"]
    if not isinstance(requests, Mapping):
        raise ResultError("REQUEST_ORACLE_NOT_OBJECT")
    _keys(requests, _REQUEST_KEYS, "REQUEST_ORACLE")
    if not _count(requests["expected"], 1) or not _count(requests["completed"], 1):
        raise ResultError("REQUEST_COUNT_MISMATCH")

    children = result["children"]
    if not isinstance(children, list) or not children:
        raise ResultError("CHILD_RECORDS_MISSING")
    child_ids: set[str] = set()
    for index, child in enumerate(children):
        if not isinstance(child, Mapping):
            raise ResultError("CHILD_RECORD_NOT_OBJECT:" + str(index))
        _keys(child, _CHILD_KEYS, "CHILD")
        if not isinstance(child["id"], str) or not child["id"]:
            raise ResultError("CHILD_ID_MISSING:" + str(index))
        if child["id"] in child_ids:
            raise ResultError("CHILD_ID_DUPLICATE:" + str(child["id"]))
        child_ids.add(child["id"])
        if not _count(child["exitStatus"], 0) or child["timedOut"] is not False:
            raise ResultError("CHILD_FAILURE:" + child["id"])
    if child_ids != _CHILD_IDS:
        raise ResultError("CHILD_INVENTORY_MISMATCH")

    for key in ("protocolOracle", "resultOracle", "runtimeOracle", "redaction", "cleanup"):
        _validate_oracle(key, result[key], result["candidateId"], evidence_root)

    # Structural records are not sufficient evidence.  Bind the scalar
    # assertions to the fixed one-request functional contract and to the
    # selected gate's required execution backend.
    protocol_fields = result["protocolOracle"]["fields"]
    if (not _count(protocol_fields["requestCount"], 1) or
            not _count(protocol_fields["terminalResponseCount"], 1)):
        raise ResultError("PROTOCOL_COUNT_MISMATCH")
    result_fields = result["resultOracle"]["fields"]
    if not _count(result_fields["requestCount"], 1) or result_fields["matched"] is not True:
        raise ResultError("RESULT_ORACLE_MISMATCH")
    runtime_fields = result["runtimeOracle"]["fields"]
    if runtime_fields["backend"] != "cuda-onnxruntime":
        raise ResultError("RUNTIME_BACKEND_MISMATCH")
    device_ids = runtime_fields["deviceIds"]
    if (not isinstance(device_ids, list) or not device_ids or
            any(not isinstance(item, str) or not item for item in device_ids) or
            len(set(device_ids)) != len(device_ids) or
            len(device_ids) != 1 or not device_ids[0].startswith("GPU-")):
        raise ResultError("RUNTIME_DEVICE_MAP_MISMATCH")
    if runtime_fields["cpuFallback"] is not False:
        raise ResultError("RUNTIME_CPU_FALLBACK")
    runtime_plan_digest = _digest(runtime_fields["planDigest"], "runtime.planDigest")
    providers = runtime_fields["providers"]
    if not isinstance(providers, list) or len(providers) != 4:
        raise ResultError("RUNTIME_PROVIDER_INVENTORY_MISMATCH")
    roles: set[str] = set()
    pids: set[int] = set()
    for provider in providers:
        if not isinstance(provider, Mapping):
            raise ResultError("RUNTIME_PROVIDER_NOT_OBJECT")
        _keys(provider, _PROVIDER_KEYS, "RUNTIME_PROVIDER")
        role = provider["role"]
        if (not isinstance(role, str) or role not in _ROLES or role in roles or
                provider["provider"] != "/example/provider/" + role):
            raise ResultError("RUNTIME_PROVIDER_IDENTITY_MISMATCH")
        roles.add(role)
        pid = provider["pid"]
        if type(pid) is not int or pid <= 0 or pid in pids:
            raise ResultError("RUNTIME_PROVIDER_PID_INVALID")
        pids.add(pid)
        if role == "Merge":
            if (provider["cudaVisibleDevices"] != "" or
                    provider["physicalDeviceUuid"] != ""):
                raise ResultError("RUNTIME_MERGE_DEVICE_MISMATCH")
            backend = "native-yolo-postprocess"
        else:
            if (provider["cudaVisibleDevices"] != "0" or
                    provider["physicalDeviceUuid"] != device_ids[0]):
                raise ResultError("RUNTIME_PROVIDER_DEVICE_MISMATCH")
            backend = "CUDAExecutionProvider"
        if provider["executionProvider"] != backend:
            raise ResultError("RUNTIME_PROVIDER_BACKEND_MISMATCH")
    observations = runtime_fields["executionEvidence"]
    if not isinstance(observations, list) or len(observations) != 4:
        raise ResultError("RUNTIME_EXECUTION_EVIDENCE_INVENTORY_MISMATCH")
    observed_roles: set[str] = set()
    provider_by_role = {item["role"]: item for item in providers}
    for observation in observations:
        if not isinstance(observation, Mapping):
            raise ResultError("RUNTIME_EXECUTION_EVIDENCE_NOT_OBJECT")
        _keys(observation, _EXECUTION_KEYS, "RUNTIME_EXECUTION_EVIDENCE")
        role = observation["role"]
        if (not isinstance(role, str) or role not in _ROLES or role in observed_roles or
                observation["provider"] != "/example/provider/" + role):
            raise ResultError("RUNTIME_EXECUTION_EVIDENCE_IDENTITY_MISMATCH")
        observed_roles.add(role)
        provider = provider_by_role[role]
        if observation["pid"] != provider["pid"]:
            raise ResultError("RUNTIME_EXECUTION_EVIDENCE_PID_MISMATCH")
        if (not isinstance(observation["requestId"], str) or not observation["requestId"] or
                type(observation["attemptEpoch"]) is not int or observation["attemptEpoch"] <= 0 or
                observation["planDigest"] != runtime_plan_digest):
            raise ResultError("RUNTIME_EXECUTION_EVIDENCE_LINEAGE_INVALID")
        if observation["executionCompleted"] is not True or observation["exactForwardCacheHit"] is not False:
            raise ResultError("RUNTIME_EXECUTION_EVIDENCE_NOT_FRESH")
        if observation["realCompute"] is not (role != "Merge"):
            raise ResultError("RUNTIME_EXECUTION_EVIDENCE_COMPUTE_MISMATCH")
        if role == "Merge":
            if (observation["runnerKind"] != "native-yolo-postprocess" or
                    observation["physicalDeviceUuid"] != "" or
                    observation["cudaVisibleDevices"] != "" or
                    observation["gpuIdentitySource"] != "" or
                    observation["profileRequestId"] != "" or
                    observation["profileAttemptEpoch"] != 0 or
                    observation["profilePath"] != "" or
                    observation["profileSha256"] != "" or
                    observation["nodeProviders"] != []):
                raise ResultError("RUNTIME_MERGE_EXECUTION_EVIDENCE_MISMATCH")
        else:
            if (observation["runnerKind"] != "onnxruntime-cuda" or
                    observation["realCompute"] is not True or
                    observation["physicalDeviceUuid"] != device_ids[0] or
                    observation["cudaVisibleDevices"] != "0" or
                    observation["gpuIdentitySource"] != "cuda-runtime-pci+driver-uuid" or
                    observation["profileRequestId"] != observation["requestId"] or
                    observation["profileAttemptEpoch"] != observation["attemptEpoch"] or
                    observation["profileSha256"] == "" or
                    not isinstance(observation["nodeProviders"], list) or
                    not observation["nodeProviders"] or
                    any(item != "CUDAExecutionProvider" for item in observation["nodeProviders"])):
                raise ResultError("RUNTIME_CUDA_EXECUTION_EVIDENCE_MISMATCH")
            profile_path = observation["profilePath"]
            if (not isinstance(profile_path, str) or not profile_path or
                    profile_path.startswith("/") or
                    any(part in {"", ".", ".."} for part in Path(profile_path).parts)):
                raise ResultError("RUNTIME_PROFILE_PATH_INVALID")
            profile = (evidence_root / profile_path).resolve()
            try:
                profile.relative_to(evidence_root.resolve())
            except ValueError as exc:
                raise ResultError("RUNTIME_PROFILE_PATH_ESCAPES_ROOT") from exc
            if not profile.is_file():
                raise ResultError("RUNTIME_PROFILE_MISSING")
            if _digest(observation["profileSha256"], "execution.profileSha256") != (
                    "sha256:" + hashlib.sha256(profile.read_bytes()).hexdigest()):
                raise ResultError("RUNTIME_PROFILE_DIGEST_MISMATCH")
    if observed_roles != _ROLES:
        raise ResultError("RUNTIME_EXECUTION_EVIDENCE_ROLE_MISMATCH")
    redaction_fields = result["redaction"]["fields"]
    if (not _count(redaction_fields["secretFindings"], 0) or
            not isinstance(redaction_fields["redacted"], bool)):
        raise ResultError("REDACTION_ORACLE_MISMATCH")
    cleanup_fields = result["cleanup"]["fields"]
    if (not _count(cleanup_fields["childExitCount"], len(children)) or
            cleanup_fields["allExited"] is not True or
            cleanup_fields["outputClosed"] is not True):
        raise ResultError("CLEANUP_ORACLE_MISMATCH")
    return True


def _main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result", type=Path)
    parser.add_argument("candidate_digest")
    parser.add_argument("--evidence-root", type=Path, required=True,
                        help="directory against which oracle paths and digests are verified")
    args = parser.parse_args()
    try:
        value = json.loads(args.result.read_text(encoding="utf-8"))
        validate_result(value, args.candidate_digest, args.evidence_root)
    except (OSError, UnicodeError, json.JSONDecodeError, ResultError) as exc:
        print(str(exc), file=__import__("sys").stderr)
        return 2
    print("SPEC180_RESULT_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())

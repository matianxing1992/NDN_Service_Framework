"""Fail-closed validation for the Spec183 YOLO host qualification receipt.

The receipt is produced by the bounded CPU/MiniNDN qualification owner.  This
module verifies source/build/run binding and the semantics of retained
lifecycle, execution, numerical, failure, and cleanup evidence; it never
launches a process, invokes Apptainer, or turns a synthetic fixture into
runtime qualification.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
from typing import Any


SCHEMA = "tiger-yolo-host-minindn-manifest-v2"
WORKLOAD = "ndnsf-di-yolo"
GRAPH = "shared-backbone-two-shard-v1"
CASES = ("normal", "permission-rejection", "negative-dependency")
SHA256 = re.compile(r"sha256:[0-9a-f]{64}\Z")
REVISION = re.compile(r"[0-9a-f]{40}\Z")
CASE_EVIDENCE = {
    "normal": {"lifecycle", "execution", "numeric", "cleanup"},
    "permission-rejection": {"lifecycle", "failure", "cleanup"},
    "negative-dependency": {"lifecycle", "failure", "cleanup"},
}
_RUN_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\Z")
_NORMAL_MILESTONES = (
    "INPUT_REFERENCE_PUBLISHED", "REQUEST_SENT", "ACK_CLOSED", "GRAPH_READY",
    "PLACEMENT_DECISION", "ARTIFACTS_READY", "PLAN_SEALED",
    "SELECTION_COMMITTED", "PROVIDER_EXECUTION_STARTED", "TERMINAL_RESPONSE",
)
_NEGATIVE_BOUNDARIES = {"DEPENDENCY_DATA_MISSING", "PEER_FAILURE"}


class YoloHostGateError(ValueError):
    """The supplied Spec183 host receipt is not build-authorizing evidence."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return "sha256:" + digest.hexdigest()


def _pairs(items):
    value = {}
    for key, item in items:
        if key in value:
            raise YoloHostGateError("YOLO_HOST_GATE_DUPLICATE_JSON_KEY")
        value[key] = item
    return value


def _load(path: Path) -> dict[str, Any]:
    try:
        payload = path.read_bytes()
        value = json.loads(payload, object_pairs_hook=_pairs,
                           parse_constant=lambda _: (_ for _ in ()).throw(
                               YoloHostGateError("YOLO_HOST_GATE_NONFINITE")))
    except YoloHostGateError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError) as exc:
        raise YoloHostGateError("YOLO_HOST_GATE_DOCUMENT") from exc
    if not isinstance(value, dict):
        raise YoloHostGateError("YOLO_HOST_GATE_ROOT")
    return value


def _digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise YoloHostGateError("YOLO_HOST_GATE_DIGEST:" + label)
    return value


def _safe_relative(value: Any, label: str) -> PurePosixPath:
    if (not isinstance(value, str) or not value or "\\" in value
            or any(ord(char) < 32 or ord(char) == 127 for char in value)):
        raise YoloHostGateError("YOLO_HOST_GATE_PATH:" + label)
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise YoloHostGateError("YOLO_HOST_GATE_PATH:" + label)
    return path


def _bound_file(root: Path, record: Any, label: str, *, forbidden: set[Path]) -> dict[str, Any]:
    if (not isinstance(record, dict)
            or set(record) != {"path", "bytes", "sha256"}
            or type(record["bytes"]) is not int
            or not 1 <= record["bytes"] <= 4 * 1024 * 1024):
        raise YoloHostGateError("YOLO_HOST_GATE_FILE_RECORD:" + label)
    relative = _safe_relative(record["path"], label)
    path = (root / relative).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError as exc:
        raise YoloHostGateError("YOLO_HOST_GATE_PATH:" + label) from exc
    if path in forbidden or any(parent in forbidden for parent in path.parents):
        raise YoloHostGateError("YOLO_HOST_GATE_SELF_REFERENCE:" + label)
    # Check every component before opening the final file.  A qualification
    # receipt must not hide mutable evidence behind a symlink.
    current = root
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            raise YoloHostGateError("YOLO_HOST_GATE_SYMLINK:" + label)
    try:
        fd = path.open("rb")
        with fd:
            info = path.stat()
            if not stat.S_ISREG(info.st_mode) or info.st_size != record["bytes"]:
                raise YoloHostGateError("YOLO_HOST_GATE_FILE_SIZE:" + label)
            observed = sha256(path)
    except OSError as exc:
        raise YoloHostGateError("YOLO_HOST_GATE_FILE_MISSING:" + label) from exc
    if observed != record["sha256"]:
        raise YoloHostGateError("YOLO_HOST_GATE_FILE_DIGEST:" + label)
    return {"path": str(path), "bytes": record["bytes"], "sha256": observed}


def _read_bound_json(path: Path, label: str) -> Any:
    try:
        return _load(path)
    except YoloHostGateError as exc:
        raise YoloHostGateError("YOLO_HOST_GATE_SEMANTIC:" + label) from exc


def _read_lifecycle(path: Path, *, case: str) -> tuple[str, list[dict[str, Any]]]:
    """Read the driver's append-only lifecycle, never infer it from exit code."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise YoloHostGateError("YOLO_HOST_GATE_LIFECYCLE_READ") from exc
    if not lines or len(lines) > 256:
        raise YoloHostGateError("YOLO_HOST_GATE_LIFECYCLE_EMPTY")
    rows = []
    for line in lines:
        try:
            row = json.loads(line, object_pairs_hook=_pairs,
                             parse_constant=lambda _: (_ for _ in ()).throw(
                                 YoloHostGateError("YOLO_HOST_GATE_NONFINITE")))
        except (UnicodeError, json.JSONDecodeError, RecursionError, YoloHostGateError) as exc:
            raise YoloHostGateError("YOLO_HOST_GATE_LIFECYCLE_JSON") from exc
        if (not isinstance(row, dict)
                or row.get("schema") != "spec180-yolo-lifecycle-event-v1"
                or row.get("caseId") not in {"Y-A", "Y-B", "Y-N", "Y-N-O", "Y-N-E", "Y-N-C"}
                or not isinstance(row.get("requestId"), str)
                or not row["requestId"].startswith("/")
                or not isinstance(row.get("attemptId"), str)
                or row["attemptId"] != "attempt-1"
                or type(row.get("sequence")) is not int
                or row["sequence"] != len(rows)
                or not isinstance(row.get("milestone"), str)):
            raise YoloHostGateError("YOLO_HOST_GATE_LIFECYCLE_FIELDS")
        rows.append(row)
    request_ids = {row["requestId"] for row in rows}
    if len(request_ids) != 1:
        raise YoloHostGateError("YOLO_HOST_GATE_LIFECYCLE_REQUEST")
    milestones = [row["milestone"] for row in rows]
    if len(set(milestones)) != len(milestones):
        raise YoloHostGateError("YOLO_HOST_GATE_LIFECYCLE_DUPLICATE")
    terminal = [row for row in rows if row["milestone"] == "TERMINAL_RESPONSE"]
    if case == "normal":
        if (milestones != list(_NORMAL_MILESTONES) or len(terminal) != 1
                or terminal[0].get("status") is not True
                or type(terminal[0].get("requestCount")) is not int
                or terminal[0]["requestCount"] < 1):
            raise YoloHostGateError("YOLO_HOST_GATE_NORMAL_LIFECYCLE")
    elif terminal:
        raise YoloHostGateError("YOLO_HOST_GATE_NEGATIVE_TERMINAL")
    elif "SELECTION_COMMITTED" not in milestones and case == "negative-dependency":
        raise YoloHostGateError("YOLO_HOST_GATE_NEGATIVE_SELECTION")
    return next(iter(request_ids)), rows


def _validate_semantic_evidence(case: str, evidence: dict[str, Path], *, application_name: str,
                                request_count: int, provider_count: int,
                                case_run_id: str) -> dict[str, Any]:
    """Validate retained records from the real MiniNDN owner.

    A hash-valid JSON object containing only ``{case, kind}`` is deliberately
    rejected.  The checks are intentionally bounded and consume the same
    records produced by the driver; they do not launch a process or accept a
    claim authored by the builder.
    """
    lifecycle_id, lifecycle = _read_lifecycle(evidence["lifecycle"], case=case)
    if case == "normal":
        if request_count != lifecycle[-1].get("requestCount"):
            raise YoloHostGateError("YOLO_HOST_GATE_REQUEST_COUNT_BINDING")
        numeric = _read_bound_json(evidence["numeric"], "numeric")
        if (numeric.get("schemaVersion") != "spec180-yolo-numerical-v1"
                or numeric.get("matched") is not True
                or numeric.get("requestId") != lifecycle_id
                or numeric.get("shape") != [1, 50, 6]
                or not isinstance(numeric.get("responseDigest"), str)
                or SHA256.fullmatch(numeric["responseDigest"]) is None):
            raise YoloHostGateError("YOLO_HOST_GATE_NUMERICAL")
        try:
            log = evidence["execution"].read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise YoloHostGateError("YOLO_HOST_GATE_EXECUTION_READ") from exc
        updates = []
        marker = "NDNSF_DI_EXECUTION_EVIDENCE_UPDATE "
        for line in log.splitlines():
            if marker not in line:
                continue
            try:
                row = json.loads(line.split(marker, 1)[1], object_pairs_hook=_pairs)
            except (UnicodeError, json.JSONDecodeError, RecursionError) as exc:
                raise YoloHostGateError("YOLO_HOST_GATE_EXECUTION_JSON") from exc
            if isinstance(row, dict):
                updates.append(row)
        if not updates:
            raise YoloHostGateError("YOLO_HOST_GATE_EXECUTION_EMPTY")
        expected_roles = {"BackboneNeck", "DetectShard0", "DetectShard1", "Merge"}
        provider_names = {row.get("providerName") for row in updates}
        if (len(updates) != provider_count or len(provider_names) != provider_count
                or {name.rsplit("/", 1)[-1] for name in provider_names
                    if isinstance(name, str)} != expected_roles
                or any(row.get("requestId") != lifecycle_id
                       or row.get("cpuFallbackUsed") != "false"
                       or row.get("executionCompleted") != "true"
                       or not isinstance(row.get("providerName"), str)
                       or not row["providerName"].startswith(application_name + "/")
                       or ("/" + case_run_id + "/") not in row["providerName"]
                       or ((row["providerName"].rsplit("/", 1)[-1] != "Merge"
                            and (row.get("runnerKind") != "onnxruntime-cpu"
                                 or row.get("realCompute") != "true"
                                 or row.get("loadCompleted") != "true"
                                 or row.get("warmupCompleted") != "true"))
                           or (row["providerName"].rsplit("/", 1)[-1] == "Merge"
                               and (row.get("runnerKind") != "native-yolo-postprocess"
                                    or row.get("realCompute") != "false"
                                    or row.get("loadCompleted") != "false"
                                    or row.get("warmupCompleted") != "false")))
                       for row in updates)):
            raise YoloHostGateError("YOLO_HOST_GATE_EXECUTION_BINDING")
    else:
        failure = _read_bound_json(evidence["failure"], "failure")
        if (failure.get("status") != "PASS"
                or failure.get("schema") != "spec180-negative-evidence-v1"
                or failure.get("requestId") != lifecycle_id
                or not isinstance(failure.get("reason"), str)
                or not isinstance(failure.get("provider"), str)
                or not failure["provider"].startswith(application_name + "/")
                or ("/" + case_run_id + "/") not in failure["provider"]):
            raise YoloHostGateError("YOLO_HOST_GATE_FAILURE_RECORD")
        boundary = failure.get("boundary")
        if case == "permission-rejection":
            if boundary != "PROVIDER_GRANT_VERIFICATION" or not failure["reason"].startswith("DI_PROTECTED_GRANT_REJECTED"):
                raise YoloHostGateError("YOLO_HOST_GATE_PERMISSION_BOUNDARY")
        elif boundary not in _NEGATIVE_BOUNDARIES:
            raise YoloHostGateError("YOLO_HOST_GATE_DEPENDENCY_BOUNDARY")
    cleanup = _read_bound_json(evidence["cleanup"], "cleanup")
    children = cleanup.get("children")
    observations = cleanup.get("networkResourceObservations")
    if (cleanup.get("schema") != "minindn-owned-cleanup-v1"
            or cleanup.get("errors") != [] or cleanup.get("networkStopped") is not True
            or not isinstance(children, list) or not children
            or any(not isinstance(row, dict) or row.get("reaped") is not True
                   or row.get("forced") is not False for row in children)
            or not isinstance(observations, list) or not observations
            or any(not isinstance(row, dict)
                   or row.get("observation", {}).get("clean") is not True
                   for row in observations)):
        raise YoloHostGateError("YOLO_HOST_GATE_CLEANUP")
    return {"requestId": lifecycle_id, "lifecycle": lifecycle}


def validate_yolo_host_gate(path: Path, *, source_seal_path: Path,
                            expected_base_sif_sha256: str | None = None,
                            expected_application_manifest_sha256: str | None = None) -> dict[str, Any]:
    """Validate a source-bound Spec183 host receipt without side effects."""
    path = Path(path).expanduser().resolve()
    source_seal_path = Path(source_seal_path).expanduser().resolve()
    value = _load(path)
    required = {"schema", "status", "workload", "sourceSeal", "runId",
                "baseSifSha256", "applicationManifestSha256", "applicationName",
                "providerCount", "graph", "cases"}
    if set(value) != required:
        raise YoloHostGateError("YOLO_HOST_GATE_FIELDS")
    if value["schema"] != SCHEMA or value["status"] != "PASS":
        raise YoloHostGateError("YOLO_HOST_GATE_SCHEMA_OR_STATUS")
    if value["workload"] != WORKLOAD or value["graph"] != GRAPH:
        raise YoloHostGateError("YOLO_HOST_GATE_WORKLOAD")
    if (not isinstance(value["runId"], str) or _RUN_ID.fullmatch(value["runId"]) is None):
        raise YoloHostGateError("YOLO_HOST_GATE_RUNTIME_BINDING")
    _digest(value["baseSifSha256"], "baseSifSha256")
    _digest(value["applicationManifestSha256"], "applicationManifestSha256")
    if (expected_base_sif_sha256 is not None
            and value["baseSifSha256"] != expected_base_sif_sha256):
        raise YoloHostGateError("YOLO_HOST_GATE_BASE_BINDING")
    if (expected_application_manifest_sha256 is not None
            and value["applicationManifestSha256"] != expected_application_manifest_sha256):
        raise YoloHostGateError("YOLO_HOST_GATE_APPLICATION_BINDING")
    if (not isinstance(value["applicationName"], str)
            or not value["applicationName"].startswith("/")
            or value["applicationName"] == "/"
            or any(ord(char) < 33 or ord(char) == 127 for char in value["applicationName"])):
        raise YoloHostGateError("YOLO_HOST_GATE_APPLICATION")
    if value["providerCount"] != 4:
        raise YoloHostGateError("YOLO_HOST_GATE_PROVIDER_COUNT")

    source = value["sourceSeal"]
    if (not isinstance(source, dict)
            or set(source) != {"path", "sha256", "sealDigest", "sourceRevision"}
            or not isinstance(source["path"], str)
            or Path(source["path"]).expanduser().resolve() != source_seal_path
            or _digest(source["sha256"], "sourceSeal") != sha256(source_seal_path)
            or _digest(source["sealDigest"], "sealDigest") is None
            or not isinstance(source["sourceRevision"], str)
            or REVISION.fullmatch(source["sourceRevision"]) is None):
        raise YoloHostGateError("YOLO_HOST_GATE_SOURCE_BINDING")
    source_value = _load(source_seal_path)
    if (source_value.get("sealDigest") != source["sealDigest"]
            or source_value.get("sourceRevision") != source["sourceRevision"]):
        raise YoloHostGateError("YOLO_HOST_GATE_SOURCE_CONTENT")

    cases = value["cases"]
    if (not isinstance(cases, list) or len(cases) != len(CASES)
            or {row.get("case") for row in cases if isinstance(row, dict)} != set(CASES)):
        raise YoloHostGateError("YOLO_HOST_GATE_CASE_SET")
    observed = {}
    for row in cases:
        if not isinstance(row, dict) or set(row) != {
                "case", "runId", "status", "requestCount", "responseStatus",
                "success", "failureBoundary", "reselectionCount", "evidence"}:
            raise YoloHostGateError("YOLO_HOST_GATE_CASE_FIELDS")
        case = row["case"]
        if (case not in CASES or case in observed or row["status"] != "PASS"
                or not isinstance(row["runId"], str)
                or _RUN_ID.fullmatch(row["runId"]) is None):
            raise YoloHostGateError("YOLO_HOST_GATE_CASE_STATUS")
        if (type(row["requestCount"]) is not int or row["requestCount"] < 1
                or type(row["reselectionCount"]) is not int
                or row["reselectionCount"] != 0
                or type(row["success"]) is not bool):
            raise YoloHostGateError("YOLO_HOST_GATE_CASE_COUNTS")
        if case == "normal":
            if (row["responseStatus"] != "PASS" or row["success"] is not True
                    or row["failureBoundary"] is not None):
                raise YoloHostGateError("YOLO_HOST_GATE_NORMAL_CASE")
        else:
            if (row["responseStatus"] != "REJECTED" or row["success"] is not False
                    or not isinstance(row["failureBoundary"], str)
                    or not row["failureBoundary"]):
                raise YoloHostGateError("YOLO_HOST_GATE_NEGATIVE_CASE")
        evidence = row["evidence"]
        if (not isinstance(evidence, list)
                or {item.get("kind") for item in evidence if isinstance(item, dict)}
                != CASE_EVIDENCE[case]):
            raise YoloHostGateError("YOLO_HOST_GATE_EVIDENCE_SET")
        bound = {}
        for item in evidence:
            kind = item.get("kind") if isinstance(item, dict) else None
            if kind in bound:
                raise YoloHostGateError("YOLO_HOST_GATE_EVIDENCE_DUPLICATE")
            file_record = dict(item)
            file_record.pop("kind", None)
            bound[kind] = _bound_file(path.parent, file_record, "evidence." + str(kind),
                                      forbidden={path})
        semantic = _validate_semantic_evidence(
            case, {kind: Path(record["path"]) for kind, record in bound.items()},
            application_name=value["applicationName"], request_count=row["requestCount"],
            provider_count=value["providerCount"], case_run_id=row["runId"])
        observed[case] = dict(row, evidence=bound,
                              requestId=semantic["requestId"])
    return {
        "schema": SCHEMA,
        "status": "PASS",
        "workload": WORKLOAD,
        "applicationName": value["applicationName"],
        "runId": value["runId"],
        "baseSifSha256": value["baseSifSha256"],
        "applicationManifestSha256": value["applicationManifestSha256"],
        "providerCount": 4,
        "graph": GRAPH,
        "cases": observed,
        "sourceSeal": {
            "path": str(source_seal_path),
            "sha256": source["sha256"],
            "sealDigest": source["sealDigest"],
            "sourceRevision": source["sourceRevision"],
        },
        "qualification": "YOLO_HOST_GATE_COMPONENT_ONLY",
    }


__all__ = ["GRAPH", "SCHEMA", "WORKLOAD", "YoloHostGateError",
           "validate_yolo_host_gate"]

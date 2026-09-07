"""Fail-closed validation for the Spec183 YOLO host qualification receipt.

The receipt is produced by the bounded CPU/MiniNDN qualification owner.  This
module only verifies its binding and the existence/content identity of the
retained evidence; it never launches a process, invokes Apptainer, or turns a
synthetic fixture into runtime qualification.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
from typing import Any


SCHEMA = "tiger-yolo-host-minindn-manifest-v1"
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


def validate_yolo_host_gate(path: Path, *, source_seal_path: Path) -> dict[str, Any]:
    """Validate a source-bound Spec183 host receipt without side effects."""
    path = Path(path).expanduser().resolve()
    source_seal_path = Path(source_seal_path).expanduser().resolve()
    value = _load(path)
    required = {"schema", "status", "workload", "sourceSeal", "applicationName",
                "providerCount", "graph", "cases"}
    if set(value) != required:
        raise YoloHostGateError("YOLO_HOST_GATE_FIELDS")
    if value["schema"] != SCHEMA or value["status"] != "PASS":
        raise YoloHostGateError("YOLO_HOST_GATE_SCHEMA_OR_STATUS")
    if value["workload"] != WORKLOAD or value["graph"] != GRAPH:
        raise YoloHostGateError("YOLO_HOST_GATE_WORKLOAD")
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
                "case", "status", "requestCount", "responseStatus",
                "success", "failureBoundary", "reselectionCount", "evidence"}:
            raise YoloHostGateError("YOLO_HOST_GATE_CASE_FIELDS")
        case = row["case"]
        if case not in CASES or case in observed or row["status"] != "PASS":
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
        observed[case] = dict(row, evidence=bound)
    return {
        "schema": SCHEMA,
        "status": "PASS",
        "workload": WORKLOAD,
        "applicationName": value["applicationName"],
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

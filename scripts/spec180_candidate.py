#!/usr/bin/env python3
"""Legacy Spec180 candidate identity and Spec181 local development delivery.

The module is deliberately independent of the runtime and release adapters.
Planning, local qualification, SIF sealing, and Tiger tooling all consume the
same canonical record and digest; none of them may redefine candidate
identity locally.
"""

from __future__ import annotations

from dataclasses import dataclass
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Any, Mapping


SCHEMA = "spec180-candidate-v1"

# These names mirror the candidate contract and are intentionally ordered. The
# order is part of the serialized schema, while JSON key ordering is canonical.
REQUIRED_PLANES = (
    "design",
    "source",
    "toolchain",
    "runtime",
    "sif",
    "harness",
    "profiles",
    "models",
    "invocation",
    "workloads",
)

# A change to a plane invalidates that plane and every later qualification
# state. The values are the earliest state at which construction may resume.
EARLIEST_RESTART = {
    "design": "IMPLEMENTATION",
    "source": "CONVERGENCE",
    "toolchain": "CONVERGENCE",
    "runtime": "CONVERGENCE",
    "sif": "SIF_BUILD",
    "harness": "LOCAL_QUALIFICATION",
    "profiles": "REMOTE_READINESS",
    "models": "MODEL_PREFLIGHT",
    "invocation": "LOCAL_QUALIFICATION",
    "workloads": "LOCAL_QUALIFICATION",
}


class CandidateIdentityError(ValueError):
    """Raised when a candidate/evidence record violates the shared contract."""


def canonical_bytes(value: Any) -> bytes:
    """Return the deterministic JSON representation used for identity hashes."""
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def sha256_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _require_digest(value: Any, label: str) -> None:
    if not isinstance(value, str) or len(value) != 71 or not value.startswith("sha256:"):
        raise CandidateIdentityError(f"{label} must be a sha256:<64 hex> digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise CandidateIdentityError(f"{label} must be a sha256:<64 hex> digest") from exc


def _validate_planes(planes: Mapping[str, Any]) -> None:
    missing = [name for name in REQUIRED_PLANES if name not in planes]
    if missing:
        raise CandidateIdentityError("missing candidate planes: " + ",".join(missing))
    extra = sorted(set(planes) - set(REQUIRED_PLANES))
    if extra:
        raise CandidateIdentityError("unknown candidate planes: " + ",".join(extra))
    for name in REQUIRED_PLANES:
        plane = planes[name]
        if not isinstance(plane, Mapping):
            raise CandidateIdentityError(f"candidate plane {name} must be an object")
        if not plane:
            raise CandidateIdentityError(f"candidate plane {name} must not be empty")
        _require_digest(plane.get("digest"), f"candidate plane {name}.digest")


def _validate_source_delta(source_delta: Mapping[str, Any]) -> None:
    if not isinstance(source_delta, Mapping):
        raise CandidateIdentityError("sourceDelta must be an object")
    if not isinstance(source_delta.get("dirty"), bool):
        raise CandidateIdentityError("sourceDelta.dirty must be explicit")
    paths = source_delta.get("paths")
    if not isinstance(paths, list) or not all(isinstance(path, str) and path for path in paths):
        raise CandidateIdentityError("sourceDelta.paths must be a list of non-empty paths")
    if source_delta["dirty"] and not paths:
        raise CandidateIdentityError("dirty sourceDelta must name modified/untracked paths")
    if not source_delta["dirty"] and paths:
        raise CandidateIdentityError("clean sourceDelta cannot name dirty paths")


@dataclass(frozen=True)
class CandidateRecord:
    """Canonical identity consumed by all Spec180 promotion stages."""

    candidate_id: str
    planes: Mapping[str, Any]
    source_delta: Mapping[str, Any]
    state: str = "DRAFT"
    schema: str = SCHEMA

    def __post_init__(self) -> None:
        if self.schema != SCHEMA:
            raise CandidateIdentityError(f"unsupported candidate schema: {self.schema}")
        if not self.candidate_id or not isinstance(self.candidate_id, str):
            raise CandidateIdentityError("candidate_id must be non-empty")
        _validate_planes(self.planes)
        _validate_source_delta(self.source_delta)

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "candidateId": self.candidate_id,
            "planes": {name: self.planes[name] for name in REQUIRED_PLANES},
            "sourceDelta": self.source_delta,
            "state": self.state,
        }

    def canonical_bytes(self) -> bytes:
        return canonical_bytes(self.to_dict())

    @property
    def digest(self) -> str:
        return sha256_digest(self.canonical_bytes())


def candidate_from_dict(value: Mapping[str, Any]) -> CandidateRecord:
    """Validate and load an on-disk candidate record."""
    if not isinstance(value, Mapping):
        raise CandidateIdentityError("candidate record must be an object")
    return CandidateRecord(
        candidate_id=value.get("candidateId", ""),
        planes=value.get("planes", {}),
        source_delta=value.get("sourceDelta", {}),
        state=value.get("state", "DRAFT"),
        schema=value.get("schema", ""),
    )


def evidence_candidate_digest(evidence: Mapping[str, Any]) -> str:
    """Return the candidate digest from an evidence record or reject it."""
    if not isinstance(evidence, Mapping):
        raise CandidateIdentityError("evidence record must be an object")
    digest = evidence.get("candidateDigest")
    _require_digest(digest, "evidence.candidateDigest")
    return digest


def validate_evidence_for_candidate(
    candidate: CandidateRecord,
    evidence: Mapping[str, Any],
) -> None:
    """Reject evidence produced by another candidate identity."""
    digest = evidence_candidate_digest(evidence)
    if digest != candidate.digest:
        raise CandidateIdentityError(
            f"evidence candidateDigest {digest} does not match {candidate.digest}")
    evidence_id = evidence.get("candidateId")
    if evidence_id is not None and evidence_id != candidate.candidate_id:
        raise CandidateIdentityError("evidence candidateId does not match candidate")


def invalidation_for_change(
    previous: CandidateRecord,
    current: CandidateRecord,
) -> dict[str, Any]:
    """Identify changed planes and the earliest safe restart gate.

    Candidate IDs may remain stable across a rebuild, but a changed canonical
    plane always creates a new digest and invalidates downstream evidence.
    """
    changed = [
        name for name in REQUIRED_PLANES
        if previous.planes[name] != current.planes[name]
    ]
    if previous.source_delta != current.source_delta:
        changed.append("source")
    changed = sorted(set(changed), key=REQUIRED_PLANES.index)
    if not changed:
        return {
            "changedPlanes": [],
            "earliestRestart": None,
            "candidateChanged": previous.digest != current.digest,
        }
    earliest = min((EARLIEST_RESTART[name] for name in changed),
                   key=("IMPLEMENTATION", "CONVERGENCE", "MODEL_PREFLIGHT",
                        "LOCAL_QUALIFICATION", "SIF_BUILD", "REMOTE_READINESS").index)
    return {
        "changedPlanes": changed,
        "earliestRestart": earliest,
        "candidateChanged": previous.digest != current.digest,
    }


def bind_evidence(candidate: CandidateRecord, evidence: Mapping[str, Any]) -> dict[str, Any]:
    """Return an evidence envelope with the candidate identity attached."""
    validate_evidence_for_candidate(candidate, evidence)
    return dict(evidence)


__all__ = [
    "CandidateIdentityError",
    "CandidateRecord",
    "EARLIEST_RESTART",
    "REQUIRED_PLANES",
    "SCHEMA",
    "bind_evidence",
    "candidate_from_dict",
    "canonical_bytes",
    "evidence_candidate_digest",
    "invalidation_for_change",
    "sha256_digest",
    "validate_evidence_for_candidate",
]


# The legacy candidate API above remains stable. Local delivery is a separate
# post-qualification record and never requires a SIF or remote experiment.
DELIVERY_SCHEMA = "spec181-development-delivery-v1"
LOCAL_INPUT_GROUPS = ("contracts", "registries", "publicKeys", "models", "oracle",
                      "runners", "parityVectors", "caseEvidence")
_RECIPE_FIELDS = {"sourceRevision", "inputs", "localEnvironment", "validation",
                  "reproduction", "limitations", "experimentTransfer"}


def _delivery_reference(root: Path, value: Any) -> dict[str, Any]:
    expected = value if isinstance(value, Mapping) else None
    name = expected.get("path") if expected is not None else value
    if not isinstance(name, str) or not name:
        raise CandidateIdentityError("DELIVERY_REFERENCE_PATH_INVALID")
    if expected is not None and set(expected) != {"path", "sha256", "bytes"}:
        raise CandidateIdentityError("DELIVERY_REFERENCE_FIELDS_INVALID")
    path = root / name
    before = path.stat()
    if not path.is_file():
        raise CandidateIdentityError("DELIVERY_REFERENCE_NOT_FILE:" + name)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    after = path.stat()
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
        raise CandidateIdentityError("DELIVERY_REFERENCE_CHANGED:" + name)
    actual = {"path": name, "sha256": "sha256:" + digest.hexdigest(), "bytes": after.st_size}
    if expected is not None and dict(expected) != actual:
        raise CandidateIdentityError("DELIVERY_REFERENCE_MISMATCH:" + name)
    return actual


def _delivery_json(root: Path, reference: Mapping[str, Any]) -> dict[str, Any]:
    raw = (root / reference["path"]).read_bytes()
    if sha256_digest(raw) != reference["sha256"] or len(raw) != reference["bytes"]:
        raise CandidateIdentityError("DELIVERY_REFERENCE_CHANGED:" + reference["path"])
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise CandidateIdentityError("DELIVERY_JSON_NOT_OBJECT:" + reference["path"])
    return value


def _delivery_context(root: Path, revision: str, validation: Mapping[str, Any],
                      environment: Mapping[str, str]) -> dict[str, Any]:
    # Use the maintained owners rather than a second source/config validator.
    import run_spec180_local_gate as gate
    from spec180_inventory import local_input_identity, local_launch_configuration

    gate._validate_source_checkout(root, revision)
    inventory = _delivery_json(root, validation["inventory"])
    result = _delivery_json(root, validation["qualification"])
    audit = _delivery_json(root, validation["audit"])
    gate._validate_complete_inventory(inventory)
    if inventory["sourceRevision"] != revision:
        raise CandidateIdentityError("DELIVERY_SOURCE_REVISION_MISMATCH")
    if (result.get("schema") != gate.RESULT_SCHEMA or result.get("status") != "PASS"
            or result.get("backend") != inventory["backend"]):
        raise CandidateIdentityError("DELIVERY_QUALIFICATION_NOT_PASS")
    for name in ("candidateId", "candidateDigest", "sourceRevision", "effectiveConfigDigest",
                 "inputDigest", "inventoryDigest"):
        if result.get(name) != inventory[name]:
            raise CandidateIdentityError("DELIVERY_QUALIFICATION_IDENTITY_MISMATCH:" + name)
    for name in ("sourceIdentity", "configurationIdentity", "inputIdentity"):
        if not isinstance(result.get(name), dict) or result[name].get("status") != "PASS":
            raise CandidateIdentityError("DELIVERY_QUALIFICATION_IDENTITY_NOT_PASS:" + name)
    if (result.get("failedEntryIds") != [] or result.get("unexecutedEntryIds") != []
            or result.get("cleanup") != "PASS" or result.get("redaction") != "PASS"):
        raise CandidateIdentityError("DELIVERY_QUALIFICATION_INCOMPLETE")
    snapshot = _delivery_reference(root, result.get("inventoryPath"))
    if (snapshot["sha256"] != result.get("inventoryFileSha256")
            or snapshot["sha256"] != validation["inventory"]["sha256"]):
        raise CandidateIdentityError("DELIVERY_INVENTORY_SNAPSHOT_MISMATCH")
    configuration = local_launch_configuration(root, environment, inventory["timeoutSeconds"])
    inputs = local_input_identity(root, environment)
    if (sha256_digest(canonical_bytes(configuration)) != inventory["effectiveConfigDigest"]
            or result.get("effectiveConfiguration") != configuration):
        raise CandidateIdentityError("DELIVERY_CONFIGURATION_MISMATCH")
    if (sha256_digest(canonical_bytes(inputs)) != inventory["inputDigest"]
            or inputs != inventory["inputIdentity"]):
        raise CandidateIdentityError("DELIVERY_INPUT_MISMATCH")
    if (audit.get("schema") != "spec181-convergence-verdict-v1"
            or audit.get("verdict") != "PASS"):
        raise CandidateIdentityError("DELIVERY_AUDIT_NOT_PASS")
    for name in ("sourceRevision", "effectiveConfigDigest", "inputDigest"):
        if audit.get(name) != inventory[name]:
            raise CandidateIdentityError("DELIVERY_AUDIT_IDENTITY_MISMATCH:" + name)
    audit_evidence = audit.get("evidence")
    if not isinstance(audit_evidence, list) or not audit_evidence:
        raise CandidateIdentityError("DELIVERY_AUDIT_EVIDENCE_MISSING")
    for reference in audit_evidence:
        if not isinstance(reference, dict):
            raise CandidateIdentityError("DELIVERY_AUDIT_REFERENCE_INVALID")
        _delivery_reference(root, reference)
    reviewed = audit.get("reviewedAtUnix")
    if (type(reviewed) not in (int, float) or not math.isfinite(reviewed) or reviewed <= 0):
        raise CandidateIdentityError("DELIVERY_AUDIT_TIME_INVALID")
    entries = result.get("entries")
    if (not isinstance(entries, list) or len(entries) != len(inventory["entries"])
            or result.get("entryCount") != len(entries)):
        raise CandidateIdentityError("DELIVERY_ENTRIES_INCOMPLETE")
    logs = []
    for expected, actual in zip(inventory["entries"], entries):
        if not isinstance(actual, dict):
            raise CandidateIdentityError("DELIVERY_ENTRY_INVALID")
        if not {"signal", "case"}.issubset(actual):
            raise CandidateIdentityError("DELIVERY_ENTRY_FIELDS_MISSING")
        started = actual.get("startedAtUnix")
        if (type(started) not in (int, float) or not math.isfinite(started) or started < reviewed):
            raise CandidateIdentityError("DELIVERY_AUDIT_AFTER_QUALIFICATION")
        gate._validate_entry_command(root, expected)
        gate._entry_digest_matches(root, expected)
        for name in ("id", "kind", "case", "command", "commandDigest", "timeoutSeconds"):
            if actual.get(name) != expected.get(name):
                raise CandidateIdentityError("DELIVERY_ENTRY_MISMATCH:" + name)
        if actual.get("workingDirectory") != str(root):
            raise CandidateIdentityError("DELIVERY_ENTRY_CWD_MISMATCH")
        if (type(actual.get("pid")) is not int or actual["pid"] <= 0
                or type(actual.get("exitCode")) is not int or actual["exitCode"] != 0
                or actual.get("signal") is not None or actual.get("timedOut") is not False
                or any(actual.get(k) != "PASS" for k in ("status", "cleanup", "redaction"))):
            raise CandidateIdentityError("DELIVERY_ENTRY_NOT_PASS:" + expected["id"])
        child_environment = dict(environment)
        evidence_dir = Path(result["inventoryPath"]).parent / expected["id"]
        if expected["kind"] == "minindn-case":
            variable = gate.CASE_CONFIG_ENV[expected["case"]]
            if variable in environment:
                child_environment["SPEC180_YOLO_CONFIG"] = environment[variable]
            child_environment["SPEC180_CASE_OUTPUT_DIR"] = str(evidence_dir / "case-output")
        if actual.get("environmentDigest") != sha256_digest(canonical_bytes(child_environment)):
            raise CandidateIdentityError("DELIVERY_CHILD_ENVIRONMENT_MISMATCH")
        output = []
        for stream in ("stdout", "stderr"):
            if actual.get(stream + "Path") != str(evidence_dir / (stream + ".log")):
                raise CandidateIdentityError("DELIVERY_LOG_PATH_MISMATCH")
            reference = _delivery_reference(root, actual[stream + "Path"])
            if reference["sha256"] != actual.get(stream + "Sha256"):
                raise CandidateIdentityError("DELIVERY_LOG_DIGEST_MISMATCH")
            logs.append(reference)
            if expected["kind"] == "minindn-case":
                output.append((root / reference["path"]).read_text(encoding="utf-8"))
        oracle = (gate._case_oracle_marker(expected["case"])
                  if expected["kind"] == "minindn-case" else "exit-code-zero")
        if actual.get("oracle") != oracle or (expected["kind"] == "minindn-case"
                                               and not any(oracle in text for text in output)):
            raise CandidateIdentityError("DELIVERY_CASE_ORACLE_MISSING")
    gate._validate_source_checkout(root, revision)
    if local_input_identity(root, environment) != inputs:
        raise CandidateIdentityError("DELIVERY_INPUT_CHANGED_DURING_CHECK")
    return {"inventory": inventory, "configuration": configuration, "inputs": inputs,
            "logs": logs, "auditEvidence": audit_evidence}


def _build_development_delivery(recipe: Mapping[str, Any], *, root: Path,
                                environment: Mapping[str, str]) -> dict[str, Any]:
    if not isinstance(recipe, Mapping) or set(recipe) != _RECIPE_FIELDS:
        raise CandidateIdentityError("DELIVERY_RECIPE_FIELDS_INVALID")
    revision = recipe["sourceRevision"]
    if not isinstance(revision, str) or re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise CandidateIdentityError("DELIVERY_SOURCE_REVISION_INVALID")
    def reference_list(value: Any) -> list[dict[str, Any]]:
        if not isinstance(value, list) or not value:
            raise CandidateIdentityError("DELIVERY_REFERENCE_LIST_EMPTY")
        return [_delivery_reference(root, item) for item in value]
    groups = recipe["inputs"]
    if not isinstance(groups, Mapping) or set(groups) != set(LOCAL_INPUT_GROUPS):
        raise CandidateIdentityError("DELIVERY_INPUT_GROUPS_INVALID")
    inputs = {name: reference_list(groups[name]) for name in LOCAL_INPUT_GROUPS}
    native = recipe["localEnvironment"]
    if not isinstance(native, Mapping) or set(native) != {"nativeArtifacts"}:
        raise CandidateIdentityError("DELIVERY_ENVIRONMENT_FIELDS_INVALID")
    artifacts = reference_list(native["nativeArtifacts"])
    validation = recipe["validation"]
    if not isinstance(validation, Mapping) or set(validation) != {"audit", "inventory", "qualification"}:
        raise CandidateIdentityError("DELIVERY_VALIDATION_FIELDS_INVALID")
    validation = {k: _delivery_reference(root, v) for k, v in validation.items()}
    transfer = recipe["experimentTransfer"]
    if (not isinstance(transfer, Mapping) or set(transfer) != {"contract", "owner", "T010", "T011"}
            or transfer["owner"] != "experiment-machine"
            or transfer["T010"] != "TRANSFERRED" or transfer["T011"] != "TRANSFERRED"):
        raise CandidateIdentityError("DELIVERY_TRANSFER_INVALID")
    context = _delivery_context(root, revision, validation, environment)
    validation.update(logs=context["logs"], auditEvidence=context["auditEvidence"])
    record = {
        "schema": DELIVERY_SCHEMA, "sourceRevision": revision,
        "candidateId": context["inventory"]["candidateId"],
        "candidateDigest": context["inventory"]["candidateDigest"],
        "inputs": inputs,
        "localEnvironment": {"nativeArtifacts": artifacts,
                             "effectiveConfiguration": context["configuration"],
                             "inputIdentity": context["inputs"]},
        "validation": validation,
        "reproduction": _delivery_reference(root, recipe["reproduction"]),
        "limitations": _delivery_reference(root, recipe["limitations"]),
        "experimentTransfer": dict(transfer, contract=_delivery_reference(root, transfer["contract"])),
        "handoffStatus": "READY_FOR_EXPERIMENT_MACHINE",
    }
    record["deliveryDigest"] = sha256_digest(canonical_bytes(record))
    return record


def seal_development_delivery(recipe: Mapping[str, Any], *, root: Path | str,
                              environment: Mapping[str, str]) -> dict[str, Any]:
    """Bind accepted local evidence; never build SIFs or perform experiments."""
    record = _build_development_delivery(recipe, root=Path(root).resolve(), environment=environment)
    verify_development_delivery(record, root=root, environment=environment)
    return record


def verify_development_delivery(record: Mapping[str, Any], *, root: Path | str,
                                environment: Mapping[str, str]) -> None:
    """Recheck actual dependencies and evidence without updating the old seal."""
    root = Path(root).resolve()
    if not isinstance(record, Mapping) or record.get("schema") != DELIVERY_SCHEMA:
        raise CandidateIdentityError("DELIVERY_SCHEMA_INVALID")
    body = dict(record)
    expected_digest = body.pop("deliveryDigest", None)
    if expected_digest != sha256_digest(canonical_bytes(body)):
        raise CandidateIdentityError("DELIVERY_DIGEST_MISMATCH")
    try:
        recipe = {name: record[name] for name in _RECIPE_FIELDS}
        recipe["localEnvironment"] = {"nativeArtifacts": record["localEnvironment"]["nativeArtifacts"]}
        recipe["validation"] = {k: record["validation"][k] for k in ("audit", "inventory", "qualification")}
        rebuilt = _build_development_delivery(recipe, root=root, environment=environment)
    except (KeyError, TypeError) as exc:
        raise CandidateIdentityError("DELIVERY_RECORD_FIELDS_INVALID") from exc
    if canonical_bytes(rebuilt) != canonical_bytes(record):
        raise CandidateIdentityError("DELIVERY_RECORD_MISMATCH")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Seal or verify a Spec181 local development delivery.")
    parser.add_argument("operation", choices=("seal", "verify"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True, help="recipe for seal; delivery record for verify")
    parser.add_argument("--environment-json", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if (args.operation == "seal") != (args.output is not None):
        parser.error("--output is required only for seal")
    try:
        environment = json.loads(args.environment_json.read_text(encoding="utf-8"))
        if not isinstance(environment, dict) or any(
                not isinstance(k, str) or not isinstance(v, str) for k, v in environment.items()):
            raise CandidateIdentityError("DELIVERY_ENVIRONMENT_INVALID")
        value = json.loads(args.input.read_text(encoding="utf-8"))
        if args.operation == "verify":
            verify_development_delivery(value, root=args.root, environment=environment)
            digest = value["deliveryDigest"]
        else:
            # A seal is an output artifact, never an input to its own source check.
            try:
                args.output.resolve().relative_to(args.root.resolve())
            except ValueError:
                pass
            else:
                raise CandidateIdentityError("DELIVERY_OUTPUT_INSIDE_SOURCE")
            record = seal_development_delivery(value, root=args.root, environment=environment)
            encoded = json.dumps(record, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", delete=False,
                                                 dir=args.output.parent, prefix=".delivery-") as stream:
                    temporary = Path(stream.name)
                    stream.write(encoded)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.link(temporary, args.output)  # Atomic publication; refuses existing output.
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
            digest = record["deliveryDigest"]
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(str(exc), file=sys.stderr)
        return 78
    print("SPEC181_DEVELOPMENT_DELIVERY_OK " + digest)
    return 0


__all__ += ["DELIVERY_SCHEMA", "LOCAL_INPUT_GROUPS", "seal_development_delivery",
            "verify_development_delivery"]


if __name__ == "__main__":
    raise SystemExit(main())

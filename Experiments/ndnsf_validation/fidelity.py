"""Versioned fidelity records and fail-closed aggregate admission."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import Any, Iterable


class FidelityTier(IntEnum):
    STATIC = 1
    UNIT = 2
    FIXTURE = 3
    HOST_PROCESS = 4
    REAL_MININDN_MODEL = 5
    REAL_CANDIDATE_CONTAINER_MODEL = 6
    REMOTE_MULTI_NODE = 7


VALID_STATUSES = {"PASS", "FAIL", "SKIP", "ERROR"}
REQUIRED_FIELDS = {
    "schemaVersion",
    "caseId",
    "gateId",
    "runId",
    "startedAt",
    "completedAt",
    "status",
    "failureReason",
    "exactCommand",
    "sourceRevision",
    "fidelityTier",
    "realComponents",
    "simulatedComponents",
    "networkMode",
    "containerMode",
    "modelIdentity",
    "hardwareProfile",
    "skipIsFailure",
    "evidencePaths",
}


class FidelityError(ValueError):
    pass


@dataclass(frozen=True)
class GatePolicy:
    schema_version: int
    source_revision: str
    run_id: str
    mandatory_cases: dict[str, FidelityTier]
    model_identity_digest: str = ""
    workload_digest: str = ""
    authorization_case_ids: frozenset[str] = frozenset()


def _nonempty_string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise FidelityError(f"{field} must be a non-empty string")
    return value


def validate_fidelity_record(record: dict[str, Any]) -> dict[str, Any]:
    missing = sorted(REQUIRED_FIELDS - record.keys())
    if missing:
        raise FidelityError(f"missing fidelity fields: {', '.join(missing)}")
    if record["schemaVersion"] != 1:
        raise FidelityError("unsupported fidelity schemaVersion")
    for field in (
        "caseId",
        "gateId",
        "runId",
        "startedAt",
        "completedAt",
        "exactCommand",
        "sourceRevision",
        "networkMode",
        "containerMode",
    ):
        _nonempty_string(record[field], field)
    if record["status"] not in VALID_STATUSES:
        raise FidelityError("invalid status")
    if record["status"] != "PASS":
        _nonempty_string(record["failureReason"], "failureReason")
    if not isinstance(record["skipIsFailure"], bool):
        raise FidelityError("skipIsFailure must be boolean")
    for field in ("realComponents", "simulatedComponents", "evidencePaths"):
        if not isinstance(record[field], list):
            raise FidelityError(f"{field} must be a list")
    real = set(record["realComponents"])
    simulated = set(record["simulatedComponents"])
    if real & simulated:
        raise FidelityError("component cannot be both real and simulated")
    if not real:
        raise FidelityError("realComponents must not be empty")
    try:
        tier = FidelityTier[record["fidelityTier"]]
    except (KeyError, TypeError) as exc:
        raise FidelityError("unknown fidelityTier") from exc
    if tier >= FidelityTier.REAL_MININDN_MODEL:
        model = record["modelIdentity"]
        if not isinstance(model, dict):
            raise FidelityError("real-model tier requires modelIdentity")
        for field in ("name", "revision", "contentDigest"):
            _nonempty_string(model.get(field), f"modelIdentity.{field}")
    if not isinstance(record["hardwareProfile"], dict):
        raise FidelityError("hardwareProfile must be an object")
    return record


def aggregate_records(
    records: Iterable[dict[str, Any]], policy: GatePolicy
) -> dict[str, Any]:
    admitted: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    for raw in records:
        try:
            record = validate_fidelity_record(raw)
        except FidelityError as exc:
            errors.append(str(exc))
            continue
        case_id = record["caseId"]
        if case_id in admitted:
            errors.append(f"duplicate caseId: {case_id}")
            continue
        if record["runId"] != policy.run_id:
            errors.append(f"{case_id}: cross-run evidence")
            continue
        if record["sourceRevision"] != policy.source_revision:
            errors.append(f"{case_id}: cross-revision evidence")
            continue
        admitted[case_id] = record

    case_results: list[dict[str, Any]] = []
    for case_id, minimum_tier in policy.mandatory_cases.items():
        record = admitted.get(case_id)
        reasons: list[str] = []
        if record is None:
            reasons.append("missing mandatory evidence")
        else:
            tier = FidelityTier[record["fidelityTier"]]
            if tier < minimum_tier:
                reasons.append(
                    f"fidelity {tier.name} below required {minimum_tier.name}"
                )
            if record["status"] != "PASS":
                reasons.append(f"mandatory status is {record['status']}")
            identity = record.get("modelIdentity") or {}
            if (
                policy.model_identity_digest
                and identity.get("contentDigest") != policy.model_identity_digest
            ):
                reasons.append("model identity mismatch")
            if (
                policy.workload_digest
                and record.get("workloadDigest") != policy.workload_digest
            ):
                reasons.append("workload identity mismatch")
        case_results.append(
            {
                "caseId": case_id,
                "requiredTier": minimum_tier.name,
                "passed": not reasons,
                "reasons": reasons,
                "status": record["status"] if record else "MISSING",
            }
        )

    passed = not errors and all(item["passed"] for item in case_results)
    authorization_cases = (
        policy.authorization_case_ids
        or frozenset(policy.mandatory_cases)
    )
    covered_cases = frozenset(
        item["caseId"] for item in case_results if item["passed"]
    )
    external_authorized = passed and authorization_cases.issubset(covered_cases)
    return {
        "schemaVersion": policy.schema_version,
        "runId": policy.run_id,
        "sourceRevision": policy.source_revision,
        "mandatoryCaseIds": list(policy.mandatory_cases),
        "caseResults": case_results,
        "errors": errors,
        "passed": passed,
        "externalValidationAuthorized": external_authorized,
        "passCount": sum(item["passed"] for item in case_results),
        "failCount": sum(not item["passed"] for item in case_results),
    }

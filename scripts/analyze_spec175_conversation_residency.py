#!/usr/bin/env python3
"""Validate the real CUDA/host/CUDA Spec175 G6C evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
from typing import Any


SCHEMA = "ndnsf-di-spec175-g6c-analysis-v1"
ROLES = (
    "/LLM/Pipeline/Stage/0",
    "/LLM/Pipeline/Stage/1",
    "/LLM/Pipeline/Stage/2",
)


def sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def load_json(path: Path, label: str, errors: list[str]) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"{label} cannot be read: {exc}")
        return {}
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return {}
    return value


def bound_file(
    bundle: Path,
    record: object,
    label: str,
    errors: list[str],
) -> Path | None:
    if (not isinstance(record, dict)
            or set(record) != {"path", "sha256"}
            or not isinstance(record.get("path"), str)
            or not isinstance(record.get("sha256"), str)):
        errors.append(f"{label}: expected path/sha256 object")
        return None
    root = bundle.resolve()
    path = (root / record["path"]).resolve()
    try:
        path.relative_to(root)
    except ValueError:
        errors.append(f"{label}: path escapes the sealed bundle")
        return None
    if not path.is_file():
        errors.append(f"{label}: file is missing")
        return None
    if sha256(path) != record["sha256"]:
        errors.append(f"{label}: sha256 mismatch")
        return None
    return path


def token_digest(tokens: object) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(tuple(tokens), separators=(",", ":")).encode()).hexdigest()


def validate_result(
    result: object,
    expected_tokens: object,
    label: str,
    errors: list[str],
) -> None:
    if not isinstance(result, dict):
        errors.append(f"{label}: result is missing")
        return
    if (not isinstance(expected_tokens, list) or not expected_tokens
            or any(isinstance(value, bool) or not isinstance(value, int)
                   for value in expected_tokens)):
        errors.append(f"{label}: sealed token oracle is invalid")
        return
    if (result.get("status") != "OK"
            or result.get("exactReferenceMatch") is not True
            or result.get("tokenCount") != len(expected_tokens)
            or result.get("tokenSha256") != token_digest(expected_tokens)):
        errors.append(f"{label}: generation does not match the sealed oracle")
    if finite_nonnegative(result.get("totalMs")) is None:
        errors.append(f"{label}: totalMs is not finite")


def valid_digest(value: object) -> bool:
    if not isinstance(value, str) or not value.startswith("sha256:"):
        return False
    suffix = value[len("sha256:"):]
    return len(suffix) == 64 and all(char in "0123456789abcdef" for char in suffix)


def marker_rows(text: str, marker: str) -> list[dict[str, str]]:
    rows = []
    for line in text.splitlines():
        offset = line.find(marker)
        if offset < 0:
            continue
        fields: dict[str, str] = {}
        for token in line[offset + len(marker):].strip().split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        fields["__line__"] = line
        fields["__offset__"] = str(text.find(line))
        rows.append(fields)
    return rows


def positive_int(value: object) -> int | None:
    try:
        parsed = int(str(value))
    except (TypeError, ValueError):
        return None
    return parsed if parsed > 0 else None


def finite_nonnegative(value: object) -> float | None:
    try:
        parsed = float(str(value))
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) and parsed >= 0.0 else None


def analyze(evidence_root: Path, bundle: Path) -> dict[str, Any]:
    errors: list[str] = []
    manifest = load_json(
        bundle / "spec175-functional-manifest.json", "functional manifest", errors)
    if (manifest.get("schemaVersion")
            != "ndnsf-di-spec175-functional-bundle-v2"
            or manifest.get("gate") != "conversation-residency"):
        errors.append("functional manifest is not the frozen G6C bundle")
    oracle_path = bound_file(
        bundle, manifest.get("conversationOracle"),
        "conversationOracle", errors)
    oracle = load_json(
        oracle_path if oracle_path is not None else Path("/missing-g6c-oracle"),
        "conversation oracle", errors)
    if oracle.get("schemaVersion") != "ndnsf-di-spec175-g6c-oracle-v1":
        errors.append("conversation oracle schema mismatch")
    groups = manifest.get("processGroups")
    campaign_path = None
    if not isinstance(groups, list) or len(groups) != 1 \
            or not isinstance(groups[0], dict):
        errors.append("G6C bundle must contain exactly one process group")
    else:
        campaign_path = bound_file(
            bundle, groups[0].get("conversationCampaign"),
            "conversationCampaign", errors)
    campaign = load_json(
        campaign_path if campaign_path is not None else Path("/missing-g6c-campaign"),
        "conversation campaign", errors)
    if (campaign.get("schemaVersion")
            != "ndnsf-di-spec175-conversation-residency-campaign-v1"
            or campaign.get("conversationOracleSha256")
            != (sha256(oracle_path) if oracle_path is not None else "")
            or campaign.get("turnPairs") != oracle.get("turnPairs")
            or campaign.get("unavailableRole") != oracle.get("unavailableRole")):
        errors.append("conversation campaign differs from the sealed G6C oracle")
    pairs = oracle.get("turnPairs", [])
    pressure = next((item for item in pairs if isinstance(item, dict)
                     and item.get("caseId") == "paused-pressure"), {})
    pressure_id = str(pressure.get("conversationId", ""))
    unavailable = oracle.get("unavailableRole", {})
    unavailable_id = str(unavailable.get("conversationId", "")) \
        if isinstance(unavailable, dict) else ""
    unavailable_role = str(unavailable.get("role", "")) \
        if isinstance(unavailable, dict) else ""
    if not pressure_id or unavailable_role not in ROLES or not unavailable_id:
        errors.append("sealed G6C pressure/unavailable controls are incomplete")

    user_path = evidence_root / "conversation-residency.json"
    user = load_json(user_path, "User G6C evidence", errors)
    if user.get("schemaVersion") != "ndnsf-di-spec175-g6c-evidence-v1" \
            or user.get("status") != "PASS":
        errors.append("User G6C evidence is not PASS")
    if user.get("campaignId") != campaign.get("campaignId"):
        errors.append("User G6C evidence campaign binding mismatch")
    rows = user.get("turnPairs")
    if not isinstance(rows, list) or {
            item.get("caseId") for item in rows if isinstance(item, dict)} \
            != {"two-turn", "paused-pressure"}:
        errors.append("User G6C evidence lacks both two-turn controls")
    rows_by_case = {
        str(item.get("caseId")): item for item in rows
        if isinstance(item, dict)
    } if isinstance(rows, list) else {}
    request_ids: list[str] = []
    generation_ids: list[str] = []
    for sealed in pairs if isinstance(pairs, list) else []:
        if not isinstance(sealed, dict):
            errors.append("sealed G6C turn pair is malformed")
            continue
        case_id = str(sealed.get("caseId", ""))
        row = rows_by_case.get(case_id)
        if not isinstance(row, dict):
            continue
        if row.get("conversationId") != sealed.get("conversationId"):
            errors.append(f"{case_id}: conversation binding mismatch")
        first_request = str(row.get("firstRequestId", ""))
        second_request = str(row.get("secondRequestId", ""))
        first_generation = str(row.get("firstGenerationId", ""))
        second_generation = str(row.get("secondGenerationId", ""))
        if (not first_request or not second_request
                or first_request == second_request):
            errors.append(f"{case_id}: request identities are not fresh")
        if (not first_generation or not second_generation
                or first_generation == second_generation):
            errors.append(f"{case_id}: generation identities are not fresh")
        request_ids.extend((first_request, second_request))
        generation_ids.extend((first_generation, second_generation))
        if (row.get("firstCheckpointEpoch") != 1
                or row.get("successorCheckpointEpoch") != 2
                or not valid_digest(row.get("firstCheckpointDigest"))
                or not valid_digest(row.get("successorCheckpointDigest"))
                or row.get("firstCheckpointDigest")
                == row.get("successorCheckpointDigest")):
            errors.append(f"{case_id}: successor checkpoint evidence is invalid")
        second_turn = sealed.get("secondTurn", {})
        appended = second_turn.get("appendedInputTokenIds", []) \
            if isinstance(second_turn, dict) else []
        if row.get("deltaPrefillInputTokens") != len(appended):
            errors.append(f"{case_id}: delta-prefill token count mismatch")
        first_turn = sealed.get("firstTurn", {})
        validate_result(
            row.get("firstResult"),
            first_turn.get("referenceGeneratedTokenIds", [])
            if isinstance(first_turn, dict) else [],
            f"{case_id} first turn", errors)
        validate_result(
            row.get("secondResult"),
            second_turn.get("referenceGeneratedTokenIds", [])
            if isinstance(second_turn, dict) else [],
            f"{case_id} second turn", errors)
    negative = user.get("unavailableRole")
    if (not isinstance(negative, dict)
            or negative.get("role") != unavailable_role
            or negative.get("conversationId") != unavailable_id
            or negative.get("negativeError") != "CONVERSATION_STATE_UNAVAILABLE"
            or negative.get("fallbackAuthorized") is not True
            or negative.get("fallbackMode") != "FULL_CONTEXT"
            or negative.get("checkpointPreservedAtEpoch") != 1):
        errors.append("User unavailable-role/fallback evidence is incomplete")
    if isinstance(negative, dict) and isinstance(unavailable, dict):
        request_ids.extend(str(negative.get(field, "")) for field in (
            "firstRequestId", "negativeRequestId", "fallbackRequestId"))
        generation_ids.extend(str(negative.get(field, "")) for field in (
            "firstGenerationId", "negativeGenerationId",
            "fallbackGenerationId"))
        if (not valid_digest(negative.get("checkpointDigest"))
                or any(not value for value in request_ids[-3:])
                or any(not value for value in generation_ids[-3:])):
            errors.append("User unavailable-role identity/checkpoint evidence is incomplete")
        validate_result(
            negative.get("firstResult"),
            unavailable.get("firstTurn", {}).get(
                "referenceGeneratedTokenIds", []),
            "unavailable-role first turn", errors)
        validate_result(
            negative.get("fallbackResult"),
            unavailable.get("fallback", {}).get(
                "referenceGeneratedTokenIds", []),
            "unavailable-role fallback", errors)
    if (len(request_ids) != 7 or len(set(request_ids)) != 7
            or len(generation_ids) != 7 or len(set(generation_ids)) != 7):
        errors.append("G6C does not prove seven fresh request/generation identities")
    for field in (
            "stateTensorBytesOnNdn", "cpuModelFallbackCount",
            "incompatibleRunnerCalls"):
        if user.get(field) != 0:
            errors.append(f"User G6C evidence {field} must be zero")

    provider_logs = sorted(evidence_root.glob("provider-*.log"))
    if len(provider_logs) != 3:
        errors.append("G6C requires exactly three Provider logs")
    role_records: dict[str, dict[str, object]] = {}
    invalidation_roles: set[str] = set()
    for path in provider_logs:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            errors.append(f"cannot read {path}: {exc}")
            continue
        ready = marker_rows(text, "LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY")
        if len(ready) != 1:
            errors.append(f"{path.name}: expected one resident ONNX artifact marker")
            continue
        role = ready[0].get("role", "")
        if role not in ROLES or role in role_records:
            errors.append(f"{path.name}: duplicate or invalid role marker")
            continue
        if not ready[0].get("device", "").startswith("cuda") \
                or ready[0].get("cpuFallback") != "false":
            errors.append(f"{path.name}: model is not CUDA-resident")
        if "cpuFallback=true" in text:
            errors.append(f"{path.name}: CPU model-compute fallback observed")
        if "LLM_PIPELINE_SPEC175_M13_PROVIDER_RESTARTED" in text:
            errors.append(f"{path.name}: G6C used a Provider-wide restart")
        state_rows = marker_rows(text, "LLM_PIPELINE_QWEN_STATE_RESIDENCY")
        if not state_rows or any(
                item.get("storage") != "device"
                or item.get("hostRoundTripBytes") != "0"
                for item in state_rows):
            errors.append(f"{path.name}: decode state did not remain device-resident")
        hosted = [item for item in marker_rows(
            text, "LLM_PIPELINE_CONVERSATION_HOST_PAUSED")
            if item.get("conversationId") == pressure_id]
        prefetched = [item for item in marker_rows(
            text, "LLM_PIPELINE_CONVERSATION_PREFETCHED")
            if item.get("conversationId") == pressure_id]
        if not hosted or not prefetched:
            errors.append(f"{path.name}: pressure HOST/PREFETCH transition is missing")
            continue
        host_bytes = positive_int(hosted[0].get("transferBytes"))
        prefetch_bytes = positive_int(prefetched[0].get("transferBytes"))
        latency = finite_nonnegative(prefetched[0].get("transferLatencyMs"))
        if host_bytes is None or prefetch_bytes is None or host_bytes != prefetch_bytes:
            errors.append(f"{path.name}: state transfer byte accounting mismatch")
        if latency is None:
            errors.append(f"{path.name}: prefetch latency is not finite")
        if text.find(hosted[0]["__line__"]) >= text.find(prefetched[0]["__line__"]):
            errors.append(f"{path.name}: PREFETCH preceded HOST residency")
        prepared = [item for item in marker_rows(
            text, "LLM_PIPELINE_CONVERSATION_PREPARED")
            if item.get("requestId") == rows_by_case.get(
                "paused-pressure", {}).get("secondRequestId")]
        if (not prepared or prepared[0].get("mode") != "append-delta"
                or text.find(prefetched[0]["__line__"])
                >= text.find(prepared[0]["__line__"])):
            errors.append(f"{path.name}: all-role delta readiness is not proven")
        invalidations = [item for item in marker_rows(
            text, "LLM_PIPELINE_CONVERSATION_STATE_INVALIDATED")
            if item.get("conversationId") == unavailable_id]
        if invalidations:
            if (len(invalidations) != 1
                    or invalidations[0].get("invalidatedEntries") != "1"):
                errors.append(f"{path.name}: exact invalidation count is not one")
            invalidation_roles.add(role)
        role_records[role] = {
            "providerLog": path.name,
            "providerLogSha256": sha256(path),
            "modelDevice": ready[0].get("device"),
            "stateTransferBytes": host_bytes or 0,
            "prefetchLatencyMs": latency if latency is not None else -1,
            "stateEpochMarkers": len(state_rows),
            "unavailableStateInvalidated": bool(invalidations),
        }
    if set(role_records) != set(ROLES):
        errors.append("G6C does not cover all three placement roles")
    if invalidation_roles != {unavailable_role}:
        errors.append("exactly the registered unavailable role must invalidate state")

    return {
        "schemaVersion": SCHEMA,
        "status": "PASS" if not errors else "FAIL",
        "gate": "G6C",
        "candidateId": manifest.get("candidateId"),
        "conversationOracleSha256": (
            sha256(oracle_path) if oracle_path is not None and oracle_path.is_file()
            else ""),
        "userEvidenceSha256": sha256(user_path) if user_path.is_file() else "",
        "roleRecords": role_records,
        "stateTensorBytesOnNdn": user.get("stateTensorBytesOnNdn"),
        "cpuModelFallbackCount": user.get("cpuModelFallbackCount"),
        "incompatibleRunnerCalls": user.get("incompatibleRunnerCalls"),
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = analyze(
        args.evidence_root.expanduser().resolve(),
        args.bundle.expanduser().resolve())
    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(result, sort_keys=True))
    return 0 if result["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())

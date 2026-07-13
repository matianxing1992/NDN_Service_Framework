#!/usr/bin/env python3
"""Immutable identity derivation for Spec 110 campaigns and live runs."""

from __future__ import annotations

import hashlib
import json
import re
from typing import Any, Iterable, Mapping


DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
CANDIDATE_ID_RE = re.compile(r"^spec110-c1(?:-[0-9a-f]{12}){6}$")
CAMPAIGN_ID_RE = re.compile(r"^spec110-campaign-v1-[0-9a-f]{20}$")
CELL_ID_RE = re.compile(r"^spec110-cell-[0-9a-f]{20}$")
RUN_ID_RE = re.compile(r"^spec110-run-[0-9a-f]{20}$")
SUBMISSION_ID_RE = re.compile(r"^spec110-submission-[0-9a-f]{20}$")

CANDIDATE_BINDING_FIELDS = (
    "sourceDigest",
    "runtimeReleaseDigest",
    "modelArtifactSetDigest",
    "identitySetDigest",
    "topologyPlacementDigest",
    "workloadDigest",
)
CAMPAIGN_BINDING_FIELDS = (
    "sourceBaselineDigest",
    "modelLadderDigest",
    "workloadDigest",
    "identityContractDigest",
    "clusterContractDigest",
    "evidenceContractDigest",
)
CELL_MODES = {"ORACLE", "STAGED_BASELINE", "DISTRIBUTED_CANDIDATE", "DIAGNOSTIC"}
DEFAULT_CAMPAIGN_PROTOCOL = {
    "modelSizes": ["0.5B", "1.5B", "3B", "7B", "14B", "32B", "72B"],
    "primaryPlacement": "single-node-multi-gpu",
    "extensionPlacement": "multi-node",
    "correctnessTokenCounts": [1, 2, 32],
    "candidateRepetitions": 3,
    "matchedBaselineRepetitions": 3,
    "warmupExcluded": True,
    "measuredWindowSeconds": 60,
    "submissionPolicy": "at-most-once-no-auto-resubmit",
    "replacementPolicy": "human-authorized-new-run-and-submission-identities",
    "liveSubmissionGate": "foundation-gate-PASS",
    "physicalProduction": "DEFERRED",
    "physicalProductionOwner": "Spec 106",
}


class IdentityError(ValueError):
    """Stable fail-closed Spec 110 identity error."""


def _fail(code: str, detail: str = "") -> None:
    raise IdentityError(code + (f":{detail}" if detail else ""))


def canonical_json(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")


def digest_object(value: object) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def _digest_map(value: object, fields: tuple[str, ...], code: str) -> dict[str, str]:
    if not isinstance(value, Mapping) or set(value) != set(fields):
        _fail(code, "FIELDS")
    result: dict[str, str] = {}
    for field in fields:
        item = value.get(field)
        if not isinstance(item, str) or DIGEST_RE.fullmatch(item) is None:
            _fail(code, field)
        result[field] = item
    return result


def reject_legacy_identity(identity: str) -> str:
    if not isinstance(identity, str) or not identity:
        _fail("IDENTITY_INVALID")
    if identity.lower().startswith("spec109"):
        _fail("LEGACY_IDENTITY_FORBIDDEN", identity)
    return identity


def _candidate_id(bindings: Mapping[str, str]) -> str:
    return "spec110-c1" + "".join(
        "-" + bindings[field][7:19] for field in CANDIDATE_BINDING_FIELDS
    )


def freeze_candidate(value: Mapping[str, object]) -> dict[str, Any]:
    if not isinstance(value, Mapping) or "bindingDigests" not in value:
        _fail("CANDIDATE_PROFILE_INVALID")
    unexpected = set(value) - {"bindingDigests"}
    if unexpected:
        _fail("CANDIDATE_PROFILE_FIELDS_INVALID", ",".join(sorted(unexpected)))
    bindings = _digest_map(
        value["bindingDigests"], CANDIDATE_BINDING_FIELDS, "CANDIDATE_BINDING_INVALID"
    )
    identity = _candidate_id(bindings)
    body = {
        "schemaVersion": "spec110-candidate-v1",
        "state": "FROZEN",
        "candidateId": identity,
        "bindingDigests": bindings,
    }
    body["candidateDigest"] = digest_object(body)
    return body


def validate_frozen_candidate(value: Mapping[str, object]) -> dict[str, Any]:
    fields = {
        "schemaVersion", "state", "candidateId", "bindingDigests", "candidateDigest"
    }
    if not isinstance(value, Mapping) or set(value) != fields:
        _fail("FROZEN_CANDIDATE_FIELDS_INVALID")
    if value.get("schemaVersion") != "spec110-candidate-v1" or value.get("state") != "FROZEN":
        _fail("FROZEN_CANDIDATE_STATE_INVALID")
    bindings = _digest_map(
        value.get("bindingDigests"), CANDIDATE_BINDING_FIELDS,
        "FROZEN_CANDIDATE_BINDING_INVALID",
    )
    expected_id = _candidate_id(bindings)
    body = {
        "schemaVersion": "spec110-candidate-v1",
        "state": "FROZEN",
        "candidateId": expected_id,
        "bindingDigests": bindings,
    }
    expected_digest = digest_object(body)
    if value.get("candidateId") != expected_id or value.get("candidateDigest") != expected_digest:
        _fail("FROZEN_CANDIDATE_MUTATED")
    return dict(value)


def assert_no_identity_collision(
    existing: Iterable[Mapping[str, object]], proposed: Mapping[str, object]
) -> None:
    validated = validate_frozen_candidate(proposed)
    for record in existing:
        current = validate_frozen_candidate(record)
        if (
            current["candidateId"] == validated["candidateId"]
            and current["candidateDigest"] != validated["candidateDigest"]
        ):
            _fail("IDENTITY_PREFIX_COLLISION", str(validated["candidateId"]))


def freeze_campaign(value: Mapping[str, object]) -> dict[str, Any]:
    if not isinstance(value, Mapping) or set(value) - {"bindingDigests", "protocol"}:
        _fail("CAMPAIGN_PROFILE_INVALID")
    bindings = _digest_map(
        value["bindingDigests"], CAMPAIGN_BINDING_FIELDS, "CAMPAIGN_BINDING_INVALID"
    )
    protocol = value.get("protocol", DEFAULT_CAMPAIGN_PROTOCOL)
    if protocol != DEFAULT_CAMPAIGN_PROTOCOL:
        _fail("CAMPAIGN_PROTOCOL_INVALID")
    namespace = {
        "candidate": "spec110-c1",
        "cell": "spec110-cell",
        "run": "spec110-run",
        "submission": "spec110-submission",
        "legacyPrefixForbidden": "spec109",
    }
    digest_body = {
        "schemaVersion": "spec110-campaign-bindings-v1",
        "bindingDigests": bindings,
        "identityNamespace": namespace,
        "protocol": protocol,
    }
    campaign_digest = digest_object(digest_body)
    return {
        "schemaVersion": "spec110-campaign-v1",
        "state": "FROZEN",
        "campaignId": "spec110-campaign-v1-" + campaign_digest[7:27],
        "campaignDigest": campaign_digest,
        "bindingDigests": bindings,
        "identityNamespace": namespace,
        "protocol": protocol,
    }


def derive_cell_identity(
    candidate_id: str,
    mode: str,
    token_length: int,
    repetition: int,
    placement_id: str,
) -> str:
    reject_legacy_identity(candidate_id)
    if CANDIDATE_ID_RE.fullmatch(candidate_id) is None:
        _fail("CELL_CANDIDATE_ID_INVALID")
    if mode not in CELL_MODES:
        _fail("CELL_MODE_INVALID", str(mode))
    if not isinstance(token_length, int) or isinstance(token_length, bool) or token_length < 1:
        _fail("CELL_TOKEN_LENGTH_INVALID")
    if not isinstance(repetition, int) or isinstance(repetition, bool) or repetition < 0:
        _fail("CELL_REPETITION_INVALID")
    if not isinstance(placement_id, str) or not placement_id:
        _fail("CELL_PLACEMENT_INVALID")
    value = {
        "candidateId": candidate_id,
        "mode": mode,
        "tokenLength": token_length,
        "repetition": repetition,
        "placementId": placement_id,
    }
    return "spec110-cell-" + digest_object(value)[7:27]


def derive_run_identity(
    cell_id: str,
    run_ordinal: int,
    *,
    replaces_run_id: str | None = None,
    replacement_authorization_digest: str | None = None,
) -> dict[str, Any]:
    reject_legacy_identity(cell_id)
    if CELL_ID_RE.fullmatch(cell_id) is None:
        _fail("RUN_CELL_ID_INVALID")
    if not isinstance(run_ordinal, int) or isinstance(run_ordinal, bool) or run_ordinal < 1:
        _fail("RUN_ORDINAL_INVALID")
    if run_ordinal == 1:
        if replaces_run_id is not None or replacement_authorization_digest is not None:
            _fail("ORIGINAL_RUN_REPLACEMENT_FIELDS_FORBIDDEN")
    else:
        if not isinstance(replaces_run_id, str) or RUN_ID_RE.fullmatch(replaces_run_id) is None:
            if isinstance(replaces_run_id, str):
                reject_legacy_identity(replaces_run_id)
            _fail("REPLACEMENT_LINK_REQUIRED")
        if (
            not isinstance(replacement_authorization_digest, str)
            or DIGEST_RE.fullmatch(replacement_authorization_digest) is None
        ):
            _fail("REPLACEMENT_AUTHORIZATION_REQUIRED")
    binding = {
        "cellId": cell_id,
        "runOrdinal": run_ordinal,
        "replacesRunId": replaces_run_id,
        "replacementAuthorizationDigest": replacement_authorization_digest,
    }
    return {
        "schemaVersion": "spec110-run-identity-v1",
        "runId": "spec110-run-" + digest_object(binding)[7:27],
        **binding,
    }


def derive_submission_identity(run_id: str, rendered_script_digest: str) -> str:
    reject_legacy_identity(run_id)
    if RUN_ID_RE.fullmatch(run_id) is None:
        _fail("SUBMISSION_RUN_ID_INVALID")
    if not isinstance(rendered_script_digest, str) or DIGEST_RE.fullmatch(rendered_script_digest) is None:
        _fail("SUBMISSION_SCRIPT_DIGEST_INVALID")
    binding = {"runId": run_id, "renderedScriptDigest": rendered_script_digest}
    return "spec110-submission-" + digest_object(binding)[7:27]


__all__ = [
    "CAMPAIGN_BINDING_FIELDS", "CANDIDATE_BINDING_FIELDS", "DEFAULT_CAMPAIGN_PROTOCOL",
    "IdentityError",
    "assert_no_identity_collision", "canonical_json", "derive_cell_identity",
    "derive_run_identity", "derive_submission_identity", "digest_object",
    "freeze_campaign", "freeze_candidate", "reject_legacy_identity",
    "validate_frozen_candidate",
]

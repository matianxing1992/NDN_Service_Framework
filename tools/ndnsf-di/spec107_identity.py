#!/usr/bin/env python3
"""Canonical Spec 107 candidate and campaign identity construction."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from pathlib import PurePosixPath
import re
import subprocess
from typing import Any, Iterable, Mapping


NAMESPACE = "spec107-c1"
SCHEMA = "ndnsf-di-spec107-candidate-v1"
CAMPAIGN_SCHEMA = "ndnsf-di-spec107-campaign-v1"
ID_DIGEST_KEYS = ("source", "profile", "model", "plan", "artifact", "lineage")
REQUIRED_DIGEST_KEYS = frozenset({
    *ID_DIGEST_KEYS,
    "workload",
    "tokenizer",
    "trustPolicy",
    "command",
})
CAMPAIGN_KINDS = frozenset({
    "diagnostic",
    "correctness",
    "performance",
    "fault",
    "canary",
    "operations",
    "soak",
    "release-gate",
})
SHA256_RE = re.compile(r"^sha256:([0-9a-f]{64})$")
CANDIDATE_ID_RE = re.compile(
    r"^spec107-c1(?:-[0-9a-f]{12}){6}$")
CANDIDATE_FIELDS = frozenset({
    "schema", "namespace", "candidateId", "state", "digests",
    "createdAt", "generatorVersion",
})
CAMPAIGN_FIELDS = frozenset({
    "schema", "campaignId", "candidateId", "candidateDigest", "kind",
    "ordinal", "commandDigest", "outputRoot", "eligibility",
    "releaseEligible",
})


class IdentityError(ValueError):
    """Stable fail-closed candidate/campaign identity error."""


def _fail(code: str, detail: str = "") -> None:
    suffix = f":{detail}" if detail else ""
    raise IdentityError(code + suffix)


def _canonical_json(value: object) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("utf-8")


def digest_object(value: object) -> str:
    return "sha256:" + hashlib.sha256(_canonical_json(value)).hexdigest()


def _validated_timestamp(value: object) -> str:
    if not isinstance(value, str):
        _fail("CANDIDATE_TIMESTAMP_INVALID")
    try:
        datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError as exc:
        raise IdentityError("CANDIDATE_TIMESTAMP_INVALID") from exc
    return value


def committed_source_digest(repo_root: Path | str) -> str:
    """Hash Git's canonical committed-tree representation, excluding worktree state."""

    try:
        result = subprocess.run(
            ["git", "ls-tree", "-r", "-z", "--full-tree", "HEAD"],
            cwd=Path(repo_root), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        _fail("CANDIDATE_SOURCE_GIT_UNAVAILABLE", str(exc))
    if result.returncode != 0:
        _fail(
            "CANDIDATE_SOURCE_GIT_INVALID",
            result.stderr.decode("utf-8", "replace").strip())
    return "sha256:" + hashlib.sha256(result.stdout).hexdigest()


def _validated_digests(digests: Mapping[str, object]) -> dict[str, str]:
    keys = set(digests)
    missing = REQUIRED_DIGEST_KEYS - keys
    if missing:
        _fail("CANDIDATE_DIGEST_MISSING", ",".join(sorted(missing)))
    unknown = keys - REQUIRED_DIGEST_KEYS
    if unknown:
        _fail("CANDIDATE_DIGEST_UNKNOWN", ",".join(sorted(unknown)))
    result: dict[str, str] = {}
    for key in sorted(REQUIRED_DIGEST_KEYS):
        value = digests[key]
        if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
            _fail("CANDIDATE_DIGEST_INVALID", key)
        result[key] = value
    return result


def expected_candidate_id(
    digests: Mapping[str, object],
    *,
    namespace: str = NAMESPACE,
) -> str:
    if namespace != NAMESPACE or "spec105" in namespace.lower():
        _fail("SPEC105_IDENTITY_REJECTED", namespace)
    validated = _validated_digests(digests)
    suffixes = [validated[key].split(":", 1)[1][:12] for key in ID_DIGEST_KEYS]
    return namespace + "-" + "-".join(suffixes)


def build_candidate_identity(
    digests: Mapping[str, object],
    *,
    namespace: str = NAMESPACE,
    created_at: str | None = None,
    generator_version: str = "spec107-tools-v1",
) -> dict[str, Any]:
    validated = _validated_digests(digests)
    candidate_id = expected_candidate_id(validated, namespace=namespace)
    if not isinstance(generator_version, str) or not generator_version.strip():
        _fail("CANDIDATE_GENERATOR_INVALID")
    timestamp = created_at or datetime.now(timezone.utc).replace(
        microsecond=0).isoformat().replace("+00:00", "Z")
    timestamp = _validated_timestamp(timestamp)
    return {
        "schema": SCHEMA,
        "namespace": namespace,
        "candidateId": candidate_id,
        "state": "FROZEN",
        "digests": validated,
        "createdAt": timestamp,
        "generatorVersion": generator_version,
    }


def validate_candidate_identity(candidate: Mapping[str, object]) -> dict[str, Any]:
    if not isinstance(candidate, Mapping):
        _fail("CANDIDATE_OBJECT_INVALID")
    fields = set(candidate)
    missing = CANDIDATE_FIELDS - fields
    if missing:
        _fail("CANDIDATE_FIELD_MISSING", ",".join(sorted(missing)))
    unknown = fields - CANDIDATE_FIELDS
    if unknown:
        _fail("CANDIDATE_FIELD_UNKNOWN", ",".join(sorted(unknown)))
    if candidate.get("schema") != SCHEMA:
        _fail("CANDIDATE_SCHEMA_INVALID")
    namespace = candidate.get("namespace")
    if namespace != NAMESPACE:
        _fail("SPEC105_IDENTITY_REJECTED", repr(namespace))
    digests = candidate.get("digests")
    if not isinstance(digests, Mapping):
        _fail("CANDIDATE_DIGESTS_INVALID")
    expected = expected_candidate_id(digests, namespace=str(namespace))
    actual = candidate.get("candidateId")
    if actual != expected or not isinstance(actual, str) or CANDIDATE_ID_RE.fullmatch(actual) is None:
        _fail("CANDIDATE_ID_MISMATCH", repr(actual))
    if candidate.get("state") != "FROZEN":
        _fail("CANDIDATE_STATE_INVALID", repr(candidate.get("state")))
    _validated_timestamp(candidate.get("createdAt"))
    generator = candidate.get("generatorVersion")
    if not isinstance(generator, str) or not generator.strip():
        _fail("CANDIDATE_GENERATOR_INVALID")
    return dict(candidate)


def _validated_output_root(value: object) -> str:
    if not isinstance(value, str) or not value or "\\" in value:
        _fail("CAMPAIGN_OUTPUT_INVALID", repr(value))
    lowered = value.lower()
    if "spec105" in lowered:
        _fail("SPEC105_IDENTITY_REJECTED", value)
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        _fail("CAMPAIGN_OUTPUT_INVALID", value)
    if (
        len(path.parts) < 2
        or path.parts[0] != "results"
        or not (
            path.parts[1].startswith("spec107-c1-")
            or path.parts[1].startswith("spec107-attribution-")
        )
    ):
        _fail("CAMPAIGN_OUTPUT_INVALID", value)
    return path.as_posix().rstrip("/")


def build_campaign_identity(
    candidate: Mapping[str, object],
    *,
    kind: str,
    ordinal: int,
    command_digest: str,
    output_root: str,
) -> dict[str, Any]:
    validated_candidate = validate_candidate_identity(candidate)
    candidate_id = str(validated_candidate["candidateId"])
    if "spec105" in candidate_id.lower():
        _fail("SPEC105_IDENTITY_REJECTED", candidate_id)
    if kind not in CAMPAIGN_KINDS:
        _fail("CAMPAIGN_KIND_INVALID", repr(kind))
    if isinstance(ordinal, bool) or not isinstance(ordinal, int) or ordinal < 1:
        _fail("CAMPAIGN_ORDINAL_INVALID", repr(ordinal))
    if not isinstance(command_digest, str) or SHA256_RE.fullmatch(command_digest) is None:
        _fail("CAMPAIGN_COMMAND_DIGEST_INVALID")
    normalized_output = _validated_output_root(output_root)
    candidate_digest = digest_object(validated_candidate)
    binding = {
        "candidateId": candidate_id,
        "candidateDigest": candidate_digest,
        "kind": kind,
        "ordinal": ordinal,
        "commandDigest": command_digest,
        "outputRoot": normalized_output,
    }
    suffix = digest_object(binding).split(":", 1)[1][:12]
    campaign_id = f"{NAMESPACE}-{kind}-r{ordinal}-{suffix}"
    diagnostic = kind == "diagnostic"
    return {
        "schema": CAMPAIGN_SCHEMA,
        "campaignId": campaign_id,
        "candidateId": candidate_id,
        "candidateDigest": candidate_digest,
        "kind": kind,
        "ordinal": ordinal,
        "commandDigest": command_digest,
        "outputRoot": normalized_output,
        "eligibility": (
            "DIAGNOSTIC_INELIGIBLE" if diagnostic else "EVIDENCE_ELIGIBLE"),
        "releaseEligible": not diagnostic,
    }


def validate_campaign_set(
    campaigns: Iterable[Mapping[str, object]],
    *,
    candidate_id: str,
    candidate_digest: str,
) -> list[dict[str, Any]]:
    if not isinstance(candidate_id, str) or CANDIDATE_ID_RE.fullmatch(candidate_id) is None:
        _fail("CANDIDATE_ID_MISMATCH", repr(candidate_id))
    if not isinstance(candidate_digest, str) or SHA256_RE.fullmatch(candidate_digest) is None:
        _fail("CANDIDATE_DIGEST_INVALID", "candidate")
    seen_ids: set[str] = set()
    seen_outputs: set[str] = set()
    result = []
    for index, campaign in enumerate(campaigns):
        if not isinstance(campaign, Mapping):
            _fail("CAMPAIGN_OBJECT_INVALID", str(index))
        fields = set(campaign)
        missing = CAMPAIGN_FIELDS - fields
        if missing:
            _fail("CAMPAIGN_FIELD_MISSING", ",".join(sorted(missing)))
        unknown = fields - CAMPAIGN_FIELDS
        if unknown:
            _fail("CAMPAIGN_FIELD_UNKNOWN", ",".join(sorted(unknown)))
        if campaign.get("schema") != CAMPAIGN_SCHEMA:
            _fail("CAMPAIGN_SCHEMA_INVALID", str(index))
        if campaign.get("candidateId") != candidate_id:
            _fail("CAMPAIGN_CANDIDATE_MISMATCH", str(index))
        if campaign.get("candidateDigest") != candidate_digest:
            _fail("CAMPAIGN_CANDIDATE_DIGEST_MISMATCH", str(index))
        kind = campaign.get("kind")
        if kind not in CAMPAIGN_KINDS:
            _fail("CAMPAIGN_KIND_INVALID", repr(kind))
        ordinal = campaign.get("ordinal")
        if isinstance(ordinal, bool) or not isinstance(ordinal, int) or ordinal < 1:
            _fail("CAMPAIGN_ORDINAL_INVALID", repr(ordinal))
        command_digest = campaign.get("commandDigest")
        if not isinstance(command_digest, str) or SHA256_RE.fullmatch(command_digest) is None:
            _fail("CAMPAIGN_COMMAND_DIGEST_INVALID")
        output = _validated_output_root(campaign.get("outputRoot"))
        campaign_id = campaign.get("campaignId")
        if not isinstance(campaign_id, str) or "spec105" in campaign_id.lower():
            _fail("SPEC105_IDENTITY_REJECTED", repr(campaign_id))
        binding = {
            "candidateId": candidate_id,
            "candidateDigest": candidate_digest,
            "kind": kind,
            "ordinal": ordinal,
            "commandDigest": command_digest,
            "outputRoot": output,
        }
        expected_id = (
            f"{NAMESPACE}-{kind}-r{ordinal}-"
            f"{digest_object(binding).split(':', 1)[1][:12]}")
        if campaign_id != expected_id:
            _fail("CAMPAIGN_ID_MISMATCH", str(index))
        diagnostic = kind == "diagnostic"
        expected_eligibility = (
            "DIAGNOSTIC_INELIGIBLE" if diagnostic else "EVIDENCE_ELIGIBLE")
        if (
            campaign.get("eligibility") != expected_eligibility
            or campaign.get("releaseEligible") is not (not diagnostic)
        ):
            _fail("CAMPAIGN_ELIGIBILITY_INVALID", str(index))
        if campaign_id in seen_ids:
            _fail("CAMPAIGN_ID_DUPLICATE", campaign_id)
        seen_ids.add(campaign_id)
        if output in seen_outputs:
            _fail("CAMPAIGN_OUTPUT_DUPLICATE", output)
        seen_outputs.add(output)
        result.append(dict(campaign))
    return result


__all__ = [
    "CAMPAIGN_KINDS",
    "IdentityError",
    "NAMESPACE",
    "build_candidate_identity",
    "build_campaign_identity",
    "committed_source_digest",
    "digest_object",
    "expected_candidate_id",
    "validate_campaign_set",
    "validate_candidate_identity",
]

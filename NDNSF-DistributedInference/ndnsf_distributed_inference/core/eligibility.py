"""Immutable candidate normalization and fail-closed Core eligibility."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterable, Mapping


@dataclass(frozen=True, order=True)
class CandidateFacts:
    provider: str
    provider_boot_epoch: str
    observed_at_ms: int
    fragment_digests: tuple[str, ...] = ()
    lease_id: str = ""
    lease_expires_at_ms: int = 0
    feasible: bool = True
    rejection_reason: str = ""
    attributes: Mapping[str, Any] = field(default_factory=dict, compare=False)

    def __post_init__(self) -> None:
        if not self.provider or not self.provider_boot_epoch or self.observed_at_ms <= 0:
            raise ValueError("candidate facts require provider, boot epoch and observation")


@dataclass(frozen=True)
class EligibilityRequirements:
    at_ms: int
    maximum_age_ms: int
    required_fragment_digest: str = ""
    lease_required: bool = False
    expected_provider_boot_epoch: str = ""
    excluded_providers: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if self.at_ms <= 0 or self.maximum_age_ms < 0:
            raise ValueError("eligibility time bounds are invalid")


@dataclass(frozen=True)
class EligibilityDecision:
    eligible: bool
    reason: str
    candidate: CandidateFacts


def normalize_candidate(payload: CandidateFacts | Mapping[str, Any]) -> CandidateFacts:
    if isinstance(payload, CandidateFacts):
        return payload
    return CandidateFacts(
        provider=str(payload.get("provider", payload.get("providerName", ""))),
        provider_boot_epoch=str(payload.get(
            "provider_boot_epoch", payload.get("providerBootEpoch", ""))),
        observed_at_ms=int(payload.get("observed_at_ms", payload.get("observedAtMs", 0))),
        fragment_digests=tuple(sorted(str(item) for item in payload.get(
            "fragment_digests", payload.get("fragmentDigests", [])))),
        lease_id=str(payload.get("lease_id", payload.get("leaseId", ""))),
        lease_expires_at_ms=int(payload.get(
            "lease_expires_at_ms", payload.get("leaseExpiresAtMs", 0))),
        feasible=bool(payload.get("feasible", True)),
        rejection_reason=str(payload.get("rejection_reason", payload.get("rejectionReason", ""))),
        attributes=dict(payload.get("attributes", {})),
    )


def evaluate_candidate(
    candidate: CandidateFacts | Mapping[str, Any],
    requirements: EligibilityRequirements,
) -> EligibilityDecision:
    facts = normalize_candidate(candidate)
    if facts.provider in requirements.excluded_providers:
        return EligibilityDecision(False, "PROVIDER_EXCLUDED", facts)
    if (requirements.expected_provider_boot_epoch
            and facts.provider_boot_epoch != requirements.expected_provider_boot_epoch):
        return EligibilityDecision(False, "PROVIDER_BOOT_EPOCH_MISMATCH", facts)
    age = requirements.at_ms - facts.observed_at_ms
    if age < 0 or age > requirements.maximum_age_ms:
        return EligibilityDecision(False, "CANDIDATE_STALE", facts)
    if not facts.feasible:
        return EligibilityDecision(False, facts.rejection_reason or "CANDIDATE_INFEASIBLE", facts)
    if (requirements.required_fragment_digest
            and requirements.required_fragment_digest not in facts.fragment_digests):
        return EligibilityDecision(False, "FRAGMENT_UNAVAILABLE", facts)
    if requirements.lease_required:
        if not facts.lease_id:
            return EligibilityDecision(False, "LEASE_REQUIRED", facts)
        if facts.lease_expires_at_ms <= requirements.at_ms:
            return EligibilityDecision(False, "LEASE_EXPIRED", facts)
    return EligibilityDecision(True, "ELIGIBLE", facts)


def eligible_candidates(
    candidates: Iterable[CandidateFacts | Mapping[str, Any]],
    requirements: EligibilityRequirements,
) -> tuple[CandidateFacts, ...]:
    decisions = (evaluate_candidate(candidate, requirements) for candidate in candidates)
    return tuple(sorted((item.candidate for item in decisions if item.eligible),
                        key=lambda item: (item.provider, item.provider_boot_epoch)))

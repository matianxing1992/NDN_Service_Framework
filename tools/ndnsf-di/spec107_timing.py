#!/usr/bin/env python3
"""Stable sampling and critical-path reconciliation for Spec 107."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Iterable, Mapping


COMPONENTS = (
    "admission",
    "ack-selection",
    "plan-lease",
    "queue",
    "compute",
    "encode-decode",
    "dependency-fetch",
    "dependency-publish",
    "response",
    "inter-token",
)
FORBIDDEN_FIELDS = frozenset({
    "prompt", "payload", "tensor", "kv", "kvValue", "token", "tokenValue",
    "secret", "privateKey", "userToken", "providerToken",
})
IDENTITY_FIELDS = (
    "candidateId", "campaignId", "generationId", "tokenEpoch", "requestId",
    "attemptEpoch",
)


class TimingError(ValueError):
    """Stable fail-closed timing schema error."""


def _fail(code: str, detail: str = "") -> None:
    suffix = f":{detail}" if detail else ""
    raise TimingError(code + suffix)


def _required_string(payload: Mapping[str, object], key: str) -> str:
    value = payload.get(key)
    if not isinstance(value, str) or not value:
        _fail("TIMING_FIELD_INVALID", key)
    return value


def _required_nonnegative_int(payload: Mapping[str, object], key: str) -> int:
    value = payload.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        _fail("TIMING_FIELD_INVALID", key)
    return value


@dataclass(frozen=True)
class TimingSpan:
    candidate_id: str
    campaign_id: str
    generation_id: str
    token_epoch: int
    request_id: str
    attempt_epoch: int
    provider_name: str
    provider_boot_id: str
    role: str
    component: str
    start_ms: float
    end_ms: float
    status: str
    sampled: bool

    @classmethod
    def from_dict(cls, payload: Mapping[str, object]) -> "TimingSpan":
        forbidden = sorted(FORBIDDEN_FIELDS.intersection(payload))
        if forbidden:
            _fail("TIMING_FORBIDDEN_FIELD", forbidden[0])
        component = payload.get("component")
        if component not in COMPONENTS:
            _fail("TIMING_COMPONENT_INVALID", repr(component))
        start = payload.get("startMs")
        end = payload.get("endMs")
        if (
            isinstance(start, bool) or not isinstance(start, (int, float))
            or isinstance(end, bool) or not isinstance(end, (int, float))
            or float(start) < 0 or float(end) < float(start)
        ):
            _fail("TIMING_INTERVAL_INVALID")
        sampled = payload.get("sampled")
        if sampled is not True:
            _fail("TIMING_SAMPLE_DECISION_INVALID")
        status = _required_string(payload, "status")
        return cls(
            candidate_id=_required_string(payload, "candidateId"),
            campaign_id=_required_string(payload, "campaignId"),
            generation_id=_required_string(payload, "generationId"),
            token_epoch=_required_nonnegative_int(payload, "tokenEpoch"),
            request_id=_required_string(payload, "requestId"),
            attempt_epoch=_required_nonnegative_int(payload, "attemptEpoch"),
            provider_name=_required_string(payload, "providerName"),
            provider_boot_id=_required_string(payload, "providerBootId"),
            role=_required_string(payload, "role"),
            component=str(component),
            start_ms=float(start),
            end_ms=float(end),
            status=status,
            sampled=True,
        )

    @property
    def duration_ms(self) -> float:
        return self.end_ms - self.start_ms

    @property
    def step_key(self) -> tuple[str, int, str, int]:
        return (
            self.generation_id,
            self.token_epoch,
            self.request_id,
            self.attempt_epoch,
        )


def stable_sample_allows(request_id: str, sample_rate: int) -> bool:
    if not isinstance(request_id, str) or not request_id:
        _fail("TIMING_REQUEST_ID_INVALID")
    if isinstance(sample_rate, bool) or not isinstance(sample_rate, int) or sample_rate < 1:
        _fail("TIMING_SAMPLE_RATE_INVALID", repr(sample_rate))
    if sample_rate == 1:
        return True
    value = 1469598103934665603
    for byte in request_id.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xffffffffffffffff
    return value % sample_rate == 0


def _observed_identity(payload: Mapping[str, object]) -> tuple[str, str, tuple[str, int, str, int]]:
    candidate = _required_string(payload, "candidateId")
    campaign = _required_string(payload, "campaignId")
    generation = _required_string(payload, "generationId")
    token_epoch = _required_nonnegative_int(payload, "tokenEpoch")
    request = _required_string(payload, "requestId")
    attempt = _required_nonnegative_int(payload, "attemptEpoch")
    return candidate, campaign, (generation, token_epoch, request, attempt)


def reconcile_timing(
    spans: Iterable[Mapping[str, object] | TimingSpan],
    observed_steps: Iterable[Mapping[str, object]],
    *,
    minimum_coverage: float = 0.99,
) -> dict[str, Any]:
    observed = list(observed_steps)
    if not observed:
        _fail("TIMING_OBSERVED_STEPS_MISSING")
    if minimum_coverage <= 0 or minimum_coverage > 1:
        _fail("TIMING_COVERAGE_THRESHOLD_INVALID")

    observed_by_key: dict[tuple[str, int, str, int], Mapping[str, object]] = {}
    candidate_id = ""
    campaign_id = ""
    for payload in observed:
        candidate, campaign, key = _observed_identity(payload)
        if not candidate_id:
            candidate_id, campaign_id = candidate, campaign
        if candidate != candidate_id or campaign != campaign_id:
            _fail("TIMING_IDENTITY_MISMATCH", "observed")
        if key in observed_by_key:
            _fail("TIMING_OBSERVED_DUPLICATE", repr(key))
        end_to_end = payload.get("endToEndMs")
        if (
            isinstance(end_to_end, bool)
            or not isinstance(end_to_end, (int, float))
            or float(end_to_end) < 0
        ):
            _fail("TIMING_OBSERVED_DURATION_INVALID", repr(key))
        observed_by_key[key] = payload

    grouped: dict[tuple[str, int, str, int], list[TimingSpan]] = {}
    for raw in spans:
        item = raw if isinstance(raw, TimingSpan) else TimingSpan.from_dict(raw)
        if item.candidate_id != candidate_id or item.campaign_id != campaign_id:
            _fail("TIMING_IDENTITY_MISMATCH", item.step_key[0])
        if item.step_key not in observed_by_key:
            _fail("TIMING_STEP_UNOBSERVED", repr(item.step_key))
        grouped.setdefault(item.step_key, []).append(item)

    rows = []
    valid_count = 0
    any_reconciliation_failure = False
    for key, payload in observed_by_key.items():
        items = sorted(grouped.get(key, []), key=lambda item: (item.start_ms, item.end_ms))
        errors = []
        component_set = {item.component for item in items}
        missing = [component for component in COMPONENTS if component not in component_set]
        if missing:
            errors.append("MISSING_COMPONENTS:" + ",".join(missing))
        duplicates = sorted({
            component for component in component_set
            if sum(item.component == component for item in items) > 1
        })
        if duplicates:
            errors.append("DUPLICATE_COMPONENTS:" + ",".join(duplicates))
        for previous, current in zip(items, items[1:]):
            if current.start_ms < previous.end_ms:
                errors.append("OVERLAPPING_SPANS")
                break
        structural_errors = bool(errors)
        reconciled = sum(item.duration_ms for item in items)
        observed_ms = float(payload["endToEndMs"])
        unexplained = abs(observed_ms - reconciled)
        tolerance = max(observed_ms * 0.05, 10.0)
        if unexplained > tolerance:
            errors.append("UNEXPLAINED_TIME")
            if not structural_errors:
                any_reconciliation_failure = True
        errors = sorted(set(errors))
        if not errors:
            valid_count += 1
        rows.append({
            "generationId": key[0],
            "tokenEpoch": key[1],
            "requestId": key[2],
            "attemptEpoch": key[3],
            "observedMs": observed_ms,
            "reconciledMs": reconciled,
            "unexplainedMs": unexplained,
            "unexplainedRatio": unexplained / observed_ms if observed_ms else 0.0,
            "toleranceMs": tolerance,
            "errors": errors,
        })
    coverage = valid_count / len(observed_by_key)
    if any_reconciliation_failure:
        verdict = "BLOCK_RECONCILIATION"
    elif coverage < minimum_coverage:
        verdict = "BLOCK_COVERAGE"
    else:
        verdict = "PASS"
    return {
        "schema": "ndnsf-di-spec107-timing-reconciliation-v1",
        "candidateId": candidate_id,
        "campaignId": campaign_id,
        "observedStepCount": len(observed_by_key),
        "validStepCount": valid_count,
        "coverageRatio": coverage,
        "minimumCoverage": minimum_coverage,
        "verdict": verdict,
        "steps": rows,
    }


__all__ = [
    "COMPONENTS",
    "TimingError",
    "TimingSpan",
    "reconcile_timing",
    "stable_sample_allows",
]

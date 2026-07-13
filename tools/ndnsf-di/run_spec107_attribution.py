#!/usr/bin/env python3
"""Spec 107 diagnostic attribution and immutable bottleneck decision tools."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any, Iterable, Mapping, Sequence

from spec107_lineage import assert_mutation_allowed
from spec107_timing import COMPONENTS, reconcile_timing


class AttributionError(ValueError):
    """Stable fail-closed attribution error."""


def _fail(code: str, detail: str = "") -> None:
    suffix = f":{detail}" if detail else ""
    raise AttributionError(code + suffix)


def _canonical_digest(value: object) -> str:
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":"),
        ensure_ascii=True).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def _validate_diagnostic_campaign(campaign: Mapping[str, object]) -> tuple[str, str]:
    candidate = campaign.get("candidateId")
    campaign_id = campaign.get("campaignId")
    if (
        campaign.get("schema") != "ndnsf-di-spec107-campaign-v1"
        or campaign.get("kind") != "diagnostic"
        or campaign.get("eligibility") != "DIAGNOSTIC_INELIGIBLE"
        or campaign.get("releaseEligible") is not False
        or not isinstance(candidate, str)
        or not candidate.startswith("spec107-c1-")
        or not isinstance(campaign_id, str)
        or not campaign_id.startswith("spec107-c1-diagnostic-")
    ):
        _fail("ATTRIBUTION_CAMPAIGN_NOT_DIAGNOSTIC")
    return candidate, campaign_id


def _validated_hypotheses(
    hypotheses: Iterable[Mapping[str, object]],
    *,
    warm_token_step_ms: float,
) -> list[dict[str, object]]:
    rows = []
    seen: set[str] = set()
    for raw in hypotheses:
        branch = raw.get("branch")
        if not isinstance(branch, str) or not branch:
            _fail("ATTRIBUTION_BRANCH_INVALID")
        if branch in seen:
            _fail("ATTRIBUTION_BRANCH_DUPLICATE", branch)
        seen.add(branch)
        value = raw.get("avoidableMs")
        touchpoints = raw.get("sourceTouchpoints")
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or float(value) < 0
            or not isinstance(touchpoints, list)
            or not touchpoints
            or not all(isinstance(item, str) and item for item in touchpoints)
        ):
            _fail("ATTRIBUTION_MEASUREMENT_INVALID", branch)
        rows.append({
            "branch": branch,
            "avoidableMs": float(value),
            "dominanceRatio": float(value) / warm_token_step_ms,
            "sourceTouchpoints": list(touchpoints),
        })
    return rows


def build_bottleneck_decision(
    *,
    campaign: Mapping[str, object],
    reconciliation: Mapping[str, object],
    hypotheses: Iterable[Mapping[str, object]],
    minimum_dominance: float = 0.25,
) -> dict[str, Any]:
    """Select one measured branch or return an explicit replan decision."""

    candidate, campaign_id = _validate_diagnostic_campaign(campaign)
    if (
        reconciliation.get("schema") !=
            "ndnsf-di-spec107-timing-reconciliation-v1"
        or reconciliation.get("candidateId") != candidate
        or reconciliation.get("campaignId") != campaign_id
    ):
        _fail("ATTRIBUTION_IDENTITY_MISMATCH")
    coverage = reconciliation.get("coverageRatio")
    if (
        reconciliation.get("verdict") != "PASS"
        or isinstance(coverage, bool)
        or not isinstance(coverage, (int, float))
        or float(coverage) < 0.99
    ):
        _fail("ATTRIBUTION_TIMING_INVALID")
    if minimum_dominance <= 0 or minimum_dominance > 1:
        _fail("ATTRIBUTION_DOMINANCE_INVALID")
    steps = reconciliation.get("steps")
    if not isinstance(steps, list) or not steps:
        _fail("ATTRIBUTION_TIMING_INVALID")
    observed_values = []
    for row in steps:
        if not isinstance(row, dict):
            _fail("ATTRIBUTION_TIMING_INVALID")
        value = row.get("observedMs")
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
            _fail("ATTRIBUTION_TIMING_INVALID")
        observed_values.append(float(value))
    warm_token_step_ms = sum(observed_values) / len(observed_values)
    rows = _validated_hypotheses(
        hypotheses, warm_token_step_ms=warm_token_step_ms)
    ordered = sorted(rows, key=lambda row: (-float(row["avoidableMs"]), str(row["branch"])))

    selected = None
    reason = "NO_BRANCH_MEETS_DOMINANCE"
    if ordered and float(ordered[0]["dominanceRatio"]) >= minimum_dominance:
        tied = [
            row for row in ordered
            if float(row["avoidableMs"]) == float(ordered[0]["avoidableMs"])
        ]
        if len(tied) == 1:
            selected = ordered[0]
            reason = "UNIQUE_LARGEST_BRANCH_SELECTED"
        else:
            reason = "LARGEST_BRANCH_NOT_UNIQUE"

    decision: dict[str, Any] = {
        "schema": "ndnsf-di-spec107-bottleneck-decision-v1",
        "candidateId": candidate,
        "campaignId": campaign_id,
        "eligibility": "DIAGNOSTIC_INELIGIBLE",
        "releaseEligible": False,
        "timingVerdict": reconciliation["verdict"],
        "coverageRatio": float(coverage),
        "warmTokenStepMs": warm_token_step_ms,
        "minimumDominance": minimum_dominance,
        "verdict": "SELECTED" if selected else "BLOCK_REPLAN",
        "reason": reason,
        "selectedBranch": selected["branch"] if selected else None,
        "dominanceRatio": selected["dominanceRatio"] if selected else None,
        "selectedMeasurement": selected,
        "rejectedBranches": [
            row for row in ordered if selected is None or row["branch"] != selected["branch"]
        ],
        "falsificationCondition": (
            "timing coverage/reconciliation fails, selected branch is not unique, "
            "or dominance is below 0.25"),
    }
    decision["decisionDigest"] = _canonical_digest(decision)
    return decision


_TIMELINE_RE = re.compile(
    r"NDNSF_TIMELINE role=(?P<role>\S+) event=(?P<event>\S+) "
    r"steady_us=(?P<steady>\d+) timestamp_us=\d+ requestId=(?P<request>\S+)"
    r"(?P<fields>.*)$")


def parse_timeline_log(path: Path | str) -> list[dict[str, object]]:
    """Parse only content-free NDNSF_TIMELINE records from one log."""

    rows = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        match = _TIMELINE_RE.search(line)
        if not match:
            continue
        fields = {}
        for token in match.group("fields").strip().split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        rows.append({
            "role": match.group("role"),
            "event": match.group("event"),
            "steadyUs": int(match.group("steady")),
            "requestId": match.group("request"),
            "fields": fields,
        })
    return rows


def load_json_lines(path: Path | str) -> list[dict[str, object]]:
    rows = []
    for line_number, line in enumerate(
            Path(path).read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            _fail("ATTRIBUTION_JSONL_INVALID", f"{path}:{line_number}:{exc}")
        if not isinstance(row, dict):
            _fail("ATTRIBUTION_JSONL_INVALID", f"{path}:{line_number}")
        rows.append(row)
    return rows


_TIMELINE_COMPONENT_PAIRS = {
    "admission": ("request_created", "request_publish_done"),
    "ack-selection": ("ack_selection_start", "ack_selection_done"),
    "plan-lease": ("role_validation_start", "role_validation_done"),
    "queue": ("role_queue_enter", "role_queue_exit"),
    "compute": ("role_compute_start", "role_compute_done"),
    "dependency-fetch": ("dependency_fetch_start", "dependency_fetch_done"),
    "dependency-publish": ("dependency_publish_start", "dependency_publish_done"),
    "response": ("response_observed", "callback_done"),
}


def _pair_key(row: Mapping[str, object]) -> tuple[str, ...]:
    fields = row.get("fields")
    fields = fields if isinstance(fields, Mapping) else {}
    return tuple(str(fields.get(name, "")) for name in (
        "sessionId", "role", "scope", "attemptEpoch"))


def _paired_duration_ms(rows: Sequence[Mapping[str, object]],
                        start_event: str, end_event: str) -> float | None:
    pending: dict[tuple[str, ...], list[int]] = {}
    total_us = 0
    pair_count = 0
    for row in sorted(rows, key=lambda value: int(value.get("steadyUs", -1))):
        event = row.get("event")
        key = _pair_key(row)
        if event == start_event:
            pending.setdefault(key, []).append(int(row["steadyUs"]))
        elif event == end_event and pending.get(key):
            start = pending[key].pop(0)
            end = int(row["steadyUs"])
            if end < start:
                _fail("ATTRIBUTION_TIMELINE_ORDER_INVALID", end_event)
            total_us += end - start
            pair_count += 1
    return total_us / 1000.0 if pair_count else None


def derive_attribution_inputs(
    *,
    campaign: Mapping[str, object],
    client_events: Sequence[Mapping[str, object]],
    timeline_rows: Sequence[Mapping[str, object]],
) -> tuple[dict[str, Any], list[dict[str, object]]]:
    """Convert paired raw events into fail-closed reconciliation and branches."""

    candidate_id, campaign_id = _validate_diagnostic_campaign(campaign)
    metadata_by_request: dict[str, dict[str, object]] = {}
    client_by_request: dict[str, list[Mapping[str, object]]] = {}
    for row in client_events:
        if (row.get("schema") != "ndnsf-di-spec107-client-timing-event-v1" or
                row.get("candidateId") != candidate_id or
                row.get("campaignId") != campaign_id or
                row.get("sampled") is not True):
            _fail("ATTRIBUTION_CLIENT_EVENT_INVALID")
        request_id = row.get("requestId")
        if not isinstance(request_id, str) or not request_id:
            _fail("ATTRIBUTION_CLIENT_EVENT_INVALID")
        metadata = {
            "generationId": row.get("generationId"),
            "tokenEpoch": row.get("tokenEpoch"),
            "attemptEpoch": row.get("attemptEpoch"),
        }
        previous = metadata_by_request.setdefault(request_id, metadata)
        if previous != metadata:
            _fail("ATTRIBUTION_CLIENT_IDENTITY_MISMATCH", request_id)
        client_by_request.setdefault(request_id, []).append(row)

    timeline_by_request: dict[str, list[Mapping[str, object]]] = {}
    for row in timeline_rows:
        request_id = row.get("requestId")
        if isinstance(request_id, str) and request_id in client_by_request:
            timeline_by_request.setdefault(request_id, []).append(row)

    spans: list[dict[str, object]] = []
    observed_steps: list[dict[str, object]] = []
    component_totals = {component: 0.0 for component in COMPONENTS}
    for request_id, events in client_by_request.items():
        meta = metadata_by_request[request_id]
        observed = [row for row in events if row.get("component") == "observed-step"]
        if len(observed) != 1:
            continue
        observed_row = observed[0]
        observed_ms = float(observed_row["endMs"]) - float(observed_row["startMs"])
        observed_steps.append({
            "candidateId": candidate_id, "campaignId": campaign_id,
            **meta, "requestId": request_id, "endToEndMs": observed_ms,
        })
        durations: dict[str, float] = {}
        for component in ("encode-decode", "inter-token"):
            matching = [row for row in events if row.get("component") == component]
            if matching:
                durations[component] = sum(
                    float(row["endMs"]) - float(row["startMs"])
                    for row in matching)
        raw_timeline = timeline_by_request.get(request_id, [])
        for component, pair in _TIMELINE_COMPONENT_PAIRS.items():
            duration = _paired_duration_ms(raw_timeline, *pair)
            if duration is not None:
                durations[component] = duration
        cursor = 0.0
        for component in COMPONENTS:
            if component not in durations:
                continue
            duration = durations[component]
            spans.append({
                "candidateId": candidate_id, "campaignId": campaign_id,
                **meta, "requestId": request_id,
                "providerName": "diagnostic-local",
                "providerBootId": "diagnostic-local-boot",
                "role": "critical-path-aggregate", "component": component,
                "startMs": cursor, "endMs": cursor + duration,
                "status": "COMPLETED", "sampled": True,
            })
            cursor += duration
            component_totals[component] += duration
    if not observed_steps:
        _fail("ATTRIBUTION_OBSERVED_STEPS_MISSING")
    reconciliation = reconcile_timing(spans, observed_steps)
    hypotheses = [
        {
            "branch": "generation-scoped-collaboration",
            "avoidableMs": component_totals["admission"] +
                           component_totals["ack-selection"] +
                           component_totals["plan-lease"],
            "sourceTouchpoints": [
                "QwenGenerationSession.cpp", "ServiceUser.cpp"],
        },
        {
            "branch": "provider-execution",
            "avoidableMs": component_totals["queue"] + component_totals["compute"],
            "sourceTouchpoints": ["ProviderRoleWorker.cpp"],
        },
        {
            "branch": "dependency-transfer",
            "avoidableMs": component_totals["dependency-fetch"] +
                           component_totals["dependency-publish"],
            "sourceTouchpoints": ["NdnsfCollaborationDependencyIo.cpp"],
        },
        {
            "branch": "codec-response",
            "avoidableMs": component_totals["encode-decode"] +
                           component_totals["response"] +
                           component_totals["inter-token"],
            "sourceTouchpoints": ["llm_pipeline/user.py", "ServiceProvider.cpp"],
        },
    ]
    return reconciliation, hypotheses


def _load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _fail("ATTRIBUTION_INPUT_INVALID", f"{path}:{exc}")
    if not isinstance(value, dict):
        _fail("ATTRIBUTION_INPUT_INVALID", str(path))
    return value


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--reconciliation", type=Path)
    parser.add_argument("--hypotheses", type=Path)
    parser.add_argument("--client-events", type=Path)
    parser.add_argument("--timeline-log", type=Path, action="append", default=[])
    parser.add_argument("--reconciliation-output", type=Path)
    parser.add_argument("--hypotheses-output", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        campaign_value = _load_object(args.campaign)
        output = assert_mutation_allowed(args.output, repo_root=args.repo_root)
        if output.exists():
            _fail("ATTRIBUTION_OUTPUT_EXISTS", str(output))
        if args.client_events is not None or args.timeline_log:
            if args.client_events is None or not args.timeline_log:
                _fail("ATTRIBUTION_RAW_INPUT_INCOMPLETE")
            timeline_rows = [
                row for path in args.timeline_log for row in parse_timeline_log(path)]
            reconciliation_value, hypotheses_value = derive_attribution_inputs(
                campaign=campaign_value,
                client_events=load_json_lines(args.client_events),
                timeline_rows=timeline_rows)
            reconciliation_output = args.reconciliation_output or args.output.with_name(
                "timing-reconciliation.json")
            hypotheses_output = args.hypotheses_output or args.output.with_name(
                "bottleneck-hypotheses.json")
            derived_outputs = [
                (assert_mutation_allowed(path, repo_root=args.repo_root), value)
                for path, value in (
                (reconciliation_output, reconciliation_value),
                (hypotheses_output, hypotheses_value),
                )
            ]
            existing = [str(path) for path, _value in derived_outputs if path.exists()]
            if existing:
                _fail("ATTRIBUTION_OUTPUT_EXISTS", existing[0])
            for destination, value in derived_outputs:
                destination.parent.mkdir(parents=True, exist_ok=True)
                with destination.open("x", encoding="utf-8") as stream:
                    json.dump(value, stream, indent=2, sort_keys=True)
                    stream.write("\n")
        else:
            if args.reconciliation is None or args.hypotheses is None:
                _fail("ATTRIBUTION_DERIVED_INPUT_INCOMPLETE")
            reconciliation_value = _load_object(args.reconciliation)
            hypotheses_value = json.loads(args.hypotheses.read_text(encoding="utf-8"))
            if not isinstance(hypotheses_value, list):
                _fail("ATTRIBUTION_HYPOTHESES_INVALID")
        decision = build_bottleneck_decision(
            campaign=campaign_value,
            reconciliation=reconciliation_value,
            hypotheses=hypotheses_value,
        )
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("x", encoding="utf-8") as stream:
            json.dump(decision, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (AttributionError, FileExistsError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(json.dumps(decision, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


__all__ = [
    "AttributionError",
    "build_bottleneck_decision",
    "derive_attribution_inputs",
    "load_json_lines",
    "parse_timeline_log",
]

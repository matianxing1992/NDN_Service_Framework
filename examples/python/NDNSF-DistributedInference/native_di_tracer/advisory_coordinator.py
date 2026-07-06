#!/usr/bin/env python3
"""Run the NativeTracer advisory coordinator as a normal NDNSF service."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from dataclasses import replace
import threading
import time
from typing import Any

from ndnsf import (
    COORDINATION_ADVISORY_SERVICE,
    CoordinationRequest,
    CoordinationResponse,
    CoordinationServiceProvider,
    CoordinationSuggestion,
    NdnsdHealthTracker,
    RESIDENCY_READY_COST_MS,
    ServiceProvider,
    coordination_suggestion_proof,
)


GROUP = "/NDNSF-DI/Tracer/group"
CONTROLLER = "/NDNSF-DI/Tracer/controller"
COORDINATOR = "/NDNSF-DI/Tracer/provider/coordinator"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", default=GROUP)
    parser.add_argument("--controller", default=CONTROLLER)
    parser.add_argument("--provider", default=COORDINATOR)
    parser.add_argument("--service", default=COORDINATION_ADVISORY_SERVICE)
    parser.add_argument("--trust-schema", default="examples/trust-schema.conf")
    parser.add_argument("--proof-secret", default="")
    parser.add_argument("--suggestion-ttl-ms", type=int, default=5000)
    parser.add_argument("--default-role-duration-ms", type=float, default=500.0,
                        help="Default provider reservation duration when a candidate omits duration")
    parser.add_argument("--fairness-penalty-ms", type=float, default=25.0,
                        help="Penalty per previous provider use inside the rolling coordinator window")
    parser.add_argument("--busy-penalty-ms", type=float, default=500.0,
                        help="Penalty added when observed ACK shows provider has no idle workers")
    parser.add_argument("--queue-penalty-per-item-ms", type=float, default=50.0,
                        help="Penalty per queued item from observed ACK queue depth")
    parser.add_argument("--ack-state-ttl-ms", type=int, default=15000,
                        help="Forget observed ACK state older than this many milliseconds")
    parser.add_argument("--fragment-ready-default-ms", type=float,
                        default=RESIDENCY_READY_COST_MS["MISSING"],
                        help="Default fragment ready penalty when residency is unknown")
    parser.add_argument("--fragment-state-ttl-ms", type=int, default=30000,
                        help="Forget fragment state observations older than this many milliseconds")
    parser.add_argument("--enable-priority", action="store_true",
                        help="Sort intents by utility_weight descending before processing")
    parser.add_argument("--health-penalty-ms", type=float, default=200.0,
                        help="Penalty added per health score point below 1.0")
    parser.add_argument("--state-file", default="",
                        help="JSON file path for coordinator state persistence")
    parser.add_argument("--state-ttl-ms", type=int, default=60000,
                        help="Ignore persisted state entries older than this many milliseconds")
    parser.add_argument("--dry-run", action="store_true")
    return parser


def _as_float(value: Any, default: float = 0.0) -> float:
    try:
        if value is None or value == "":
            return default
        return float(value)
    except (TypeError, ValueError):
        return default


def _as_int_ms(value: Any, default: int = 0) -> int:
    return int(max(0.0, _as_float(value, float(default))))


def _runtime_hint(candidate: dict[str, Any]) -> dict[str, Any]:
    value = candidate.get("runtimeHint", candidate.get("runtime_hint", {}))
    return value if isinstance(value, dict) else {}


def _lease_offers(candidate: dict[str, Any]) -> list[dict[str, Any]]:
    value = candidate.get("leaseOffers", candidate.get("lease_offers", []))
    if isinstance(value, dict):
        return [value]
    if isinstance(value, list):
        return [item for item in value if isinstance(item, dict)]
    return []


def _lease_start_ms(candidate: dict[str, Any]) -> tuple[int, str]:
    offers = _lease_offers(candidate)
    if not offers:
        return 0, "NO_LEASE_HINT"
    best_start = 0
    best_reason = "NO_VALID_LEASE"
    for offer in offers:
        status = str(offer.get("status", "GRANTED")).upper()
        reason = str(offer.get("reasonCode", offer.get("reason_code", status)))
        if status in {"REJECTED", "EXPIRED", "CONSUMED"}:
            continue
        start = _as_int_ms(
            offer.get("estimatedStartMs", offer.get("estimated_start_ms", 0)),
            0,
        )
        if best_reason == "NO_VALID_LEASE" or start < best_start:
            best_start = start
            best_reason = reason if status != "GRANTED" else "LEASE_GRANTED"
    return best_start, best_reason


def _candidate_duration_ms(candidate: dict[str, Any], default_ms: float) -> float:
    return max(1.0, _as_float(
        candidate.get("estimatedDurationMs",
                      candidate.get("durationMs",
                                    candidate.get("estimated_duration_ms", default_ms))),
        default_ms,
    ))


def _candidate_ready_cost_ms(candidate: dict[str, Any]) -> float:
    return max(0.0, _as_float(
        candidate.get("readyCostMs",
                      candidate.get("estimatedReadyMs",
                                    candidate.get("ready_cost_ms", 0.0))),
        0.0,
    ))


def _candidate_queue_wait_ms(candidate: dict[str, Any]) -> float:
    hint = _runtime_hint(candidate)
    return max(0.0, _as_float(
        hint.get("estimatedQueueWaitMs",
                 hint.get("estimated_queue_wait_ms",
                          candidate.get("estimatedQueueWaitMs", 0.0))),
        0.0,
    ))


def _normalize_role_candidates(candidates: Any) -> list[dict[str, Any]]:
    if isinstance(candidates, list):
        return [item for item in candidates if isinstance(item, dict)]
    if isinstance(candidates, dict):
        return [candidates]
    return []


def _merge_observed_ack_runtime(observed: dict[str, dict[str, Any]],
                                ack_state: dict[str, dict[str, Any]],
                                current_ms: int,
                                ttl_ms: int) -> None:
    """Merge per-provider observed ACK runtime telemetry into the rolling state."""
    for provider, fields in observed.items():
        if not isinstance(fields, dict):
            continue
        entry = ack_state.get(provider, {})
        entry["provider"] = provider
        entry["observedAtMs"] = current_ms
        for key in (
            "queue", "readyQueue", "waitingInputs",
            "activeWorkers", "workers", "idleWorkers",
            "runtimeStatus", "negativeAckReason",
            "leaseId", "leaseExpiresAtMs",
        ):
            if key in fields:
                entry[key] = fields[key]
        ack_state[provider] = entry
    expired = [
        provider for provider, entry in ack_state.items()
        if current_ms - int(entry.get("observedAtMs", 0)) > ttl_ms
    ]
    for provider in expired:
        ack_state.pop(provider, None)


def _ack_state_busy_penalty(provider: str,
                            ack_state: dict[str, dict[str, Any]],
                            busy_penalty_ms: float,
                            queue_penalty_per_item_ms: float) -> float:
    """Return a busy penalty in ms based on observed ACK runtime telemetry."""
    entry = ack_state.get(provider)
    if not entry:
        return 0.0
    penalty = 0.0
    workers = int(entry.get("workers", 0))
    active = int(entry.get("activeWorkers", 0))
    idle = int(entry.get("idleWorkers", 0))
    if idle <= 0 and workers > 0:
        penalty += busy_penalty_ms
        if active >= workers:
            penalty += busy_penalty_ms * 0.5
    queue_length = int(entry.get("queue", 0))
    if queue_length > 0:
        penalty += float(queue_length) * queue_penalty_per_item_ms
    ready_queue = int(entry.get("readyQueue", 0))
    if ready_queue > 0:
        penalty += float(ready_queue) * queue_penalty_per_item_ms * 0.5
    runtime_status = str(entry.get("runtimeStatus", "")).lower()
    if runtime_status in {"overloaded", "degraded", "draining"}:
        penalty += busy_penalty_ms
    return penalty


def _merge_fragment_state(intent_payload: dict,
                          fragment_table: dict[str, dict[str, Any]],
                          current_ms: int,
                          ttl_ms: int) -> None:
    """Merge per-provider-per-role fragment residency into the rolling table."""
    fragment_state = intent_payload.get("fragmentState", {})
    if not isinstance(fragment_state, dict):
        return
    for provider, roles in fragment_state.items():
        if not isinstance(roles, dict):
            continue
        provider_table = fragment_table.setdefault(str(provider), {})
        for role, info in roles.items():
            if not isinstance(info, dict):
                continue
            entry = provider_table.get(str(role), {})
            entry["provider"] = str(provider)
            entry["role"] = str(role)
            entry["observedAtMs"] = current_ms
            for key in ("residency", "fragmentDigest", "readyCostMs", "backend", "path"):
                if key in info:
                    entry[key] = info[key]
            provider_table[str(role)] = entry
    # Expire old entries
    for provider in list(fragment_table):
        provider_table = fragment_table[provider]
        for role in list(provider_table):
            entry = provider_table[role]
            if current_ms - int(entry.get("observedAtMs", 0)) > ttl_ms:
                provider_table.pop(role, None)
        if not provider_table:
            fragment_table.pop(provider, None)


def _fragment_ready_penalty(provider: str,
                            role: str,
                            fragment_table: dict[str, dict[str, Any]],
                            default_ms: float) -> float:
    """Return fragment ready penalty based on observed residency."""
    provider_table = fragment_table.get(provider, {})
    entry = provider_table.get(role)
    if not entry:
        return default_ms
    residency = str(entry.get("residency", "MISSING")).upper()
    explicit_cost = entry.get("readyCostMs")
    if explicit_cost is not None:
        try:
            return float(explicit_cost)
        except (TypeError, ValueError):
            pass
    return RESIDENCY_READY_COST_MS.get(residency, default_ms)


def _load_state(path: str, ttl_ms: int) -> tuple[Counter, dict[str, float], int]:
    """Load persisted coordinator state from a JSON file."""
    if not path:
        return Counter(), {}, 0
    try:
        with open(path, encoding="utf-8") as input_file:
            payload = json.load(input_file)
    except (FileNotFoundError, json.JSONDecodeError):
        return Counter(), {}, 0
    saved_at_ms = int(payload.get("savedAtMs", 0))
    current_ms = int(time.time() * 1000)
    if ttl_ms > 0 and current_ms - saved_at_ms > ttl_ms:
        return Counter(), {}, 0
    provider_use = Counter()
    for provider, count in payload.get("providerUse", {}).items():
        provider_use[str(provider)] = int(count)
    provider_available = {}
    for provider, value in payload.get("providerAvailableAtMs", {}).items():
        provider_available[str(provider)] = float(value)
    window_version = int(payload.get("windowVersion", 0))
    return provider_use, provider_available, window_version


def _save_state(path: str,
                provider_use: Counter,
                provider_available_at_ms: dict[str, float],
                window_version: int) -> None:
    """Persist coordinator state to a JSON file."""
    if not path:
        return
    payload = {
        "providerUse": dict(provider_use),
        "providerAvailableAtMs": {
            provider: round(value, 3)
            for provider, value in sorted(provider_available_at_ms.items())
        },
        "windowVersion": window_version,
        "savedAtMs": int(time.time() * 1000),
    }
    try:
        with open(path, "w", encoding="utf-8") as output:
            json.dump(payload, output, indent=2, sort_keys=True)
            output.write("\n")
    except OSError:
        pass


def make_handler(args):
    state_ttl = int(getattr(args, "state_ttl_ms", 60000))
    state_path = str(getattr(args, "state_file", "") or "")
    provider_use, provider_available_at_ms, loaded_version = _load_state(
        state_path, state_ttl)
    if loaded_version > 0:
        print(
            "NDNSF_DI_ADVISORY_COORDINATOR_STATE_LOADED "
            + json.dumps({
                "providerUse": dict(provider_use),
                "providerCount": len(provider_available_at_ms),
                "windowVersion": loaded_version,
            }, sort_keys=True),
            flush=True,
        )
    provider_ack_state: dict[str, dict[str, Any]] = {}
    fragment_state_table: dict[str, dict[str, Any]] = {}
    health_tracker = NdnsdHealthTracker()
    state_lock = threading.Lock()
    window_version = loaded_version

    def choose_role_assignments(intent_payload: dict,
                                current_ms: int
                                ) -> tuple[dict[str, dict[str, Any]], dict[str, int], dict[str, Any]]:
        observed_ack = intent_payload.get("observedAckRuntime", {})
        if isinstance(observed_ack, dict) and observed_ack:
            _merge_observed_ack_runtime(
                observed_ack,
                provider_ack_state,
                current_ms,
                int(getattr(args, "ack_state_ttl_ms", 15000)),
            )
            print(
                "NDNSF_DI_ADVISORY_COORDINATOR_ACK_FEEDBACK "
                + json.dumps({
                    "observedProviderCount": len(observed_ack),
                    "providers": sorted(observed_ack.keys()),
                    "ackStateSize": len(provider_ack_state),
                }, sort_keys=True),
                flush=True,
            )
        fragment = intent_payload.get("fragmentState", {})
        if isinstance(fragment, dict) and fragment:
            _merge_fragment_state(
                intent_payload,
                fragment_state_table,
                current_ms,
                int(getattr(args, "fragment_state_ttl_ms", 30000)),
            )
            print(
                "NDNSF_DI_ADVISORY_COORDINATOR_FRAGMENT_STATE "
                + json.dumps({
                    "fragmentProviderCount": len(fragment),
                    "tableProviderCount": len(fragment_state_table),
                }, sort_keys=True),
                flush=True,
            )
        ndnsd_state = intent_payload.get("ndnsdProviderState", {})
        if isinstance(ndnsd_state, dict) and ndnsd_state.get("providers"):
            ndnsd_services = [
                {"provider": p, **info.get("capacity", {})}
                for p, info in ndnsd_state.get("providers", {}).items()
            ]
            if ndnsd_services:
                health_tracker.update_from_ndnsd(ndnsd_services)
                print(
                    "NDNSF_DI_ADVISORY_COORDINATOR_NDNSD_HEALTH "
                    + json.dumps({
                        "providerCount": len(ndnsd_services),
                        "openCircuits": sum(
                            1 for p in health_tracker.all_providers()
                            if health_tracker.circuit_breaker_state(p) == "OPEN"
                        ),
                    }, sort_keys=True),
                    flush=True,
                )
        role_assignments = dict(intent_payload.get("roleAssignments", {}))
        role_candidates = intent_payload.get("roleCandidates", {})
        selected: dict[str, dict[str, Any]] = {}
        local_use: Counter[str] = Counter()
        local_available_at: dict[str, float] = {}
        role_scores: dict[str, list[dict[str, Any]]] = {}
        rejected: list[dict[str, Any]] = []
        total_finish_ms = float(current_ms)
        if isinstance(role_candidates, dict):
            for role, candidates in sorted(role_candidates.items()):
                normalized = _normalize_role_candidates(candidates)
                best = None
                best_score = None
                for item in normalized:
                    provider = str(item.get("provider", item.get("providerName", ""))).strip()
                    if not provider:
                        continue
                    lease_start_ms, lease_reason = _lease_start_ms(item)
                    if lease_reason == "NO_VALID_LEASE":
                        rejected.append({
                            "role": str(role),
                            "provider": provider,
                            "reason": lease_reason,
                        })
                        continue
                    ready_cost_ms = _candidate_ready_cost_ms(item)
                    queue_wait_ms = _candidate_queue_wait_ms(item)
                    duration_ms = _candidate_duration_ms(
                        item,
                        getattr(args, "default_role_duration_ms", 500.0),
                    )
                    reserved_at_ms = max(
                        provider_available_at_ms.get(provider, float(current_ms)),
                        local_available_at.get(provider, float(current_ms)),
                    )
                    start_ms = max(
                        float(current_ms) + float(lease_start_ms),
                        reserved_at_ms,
                    )
                    finish_ms = start_ms + ready_cost_ms + queue_wait_ms + duration_ms
                    fairness_ms = (
                        provider_use[provider] + local_use[provider]
                    ) * max(0.0, float(getattr(args, "fairness_penalty_ms", 25.0)))
                    if not health_tracker.is_available(provider):
                        rejected.append({
                            "role": str(role),
                            "provider": provider,
                            "reason": "CIRCUIT_OPEN",
                        })
                        continue
                    ack_busy_ms = _ack_state_busy_penalty(
                        provider,
                        provider_ack_state,
                        float(getattr(args, "busy_penalty_ms", 500.0)),
                        float(getattr(args, "queue_penalty_per_item_ms", 50.0)),
                    )
                    fragment_ready_ms = _fragment_ready_penalty(
                        provider,
                        str(role),
                        fragment_state_table,
                        float(getattr(args, "fragment_ready_default_ms",
                                      RESIDENCY_READY_COST_MS["MISSING"])),
                    )
                    health_score = health_tracker.health_score(provider)
                    health_penalty_ms = max(0.0, (1.0 - health_score)) * float(
                        getattr(args, "health_penalty_ms", 200.0))
                    score = (finish_ms + fairness_ms + ack_busy_ms +
                             fragment_ready_ms + health_penalty_ms)
                    detail = {
                        "role": str(role),
                        "provider": provider,
                        "assignment": str(item.get("assignment", "")).strip(),
                        "scoreMs": round(score - float(current_ms), 3),
                        "startMs": round(start_ms, 3),
                        "finishMs": round(finish_ms, 3),
                        "durationMs": round(duration_ms, 3),
                        "readyCostMs": round(ready_cost_ms, 3),
                        "queueWaitMs": round(queue_wait_ms, 3),
                        "fairnessPenaltyMs": round(fairness_ms, 3),
                        "ackBusyPenaltyMs": round(ack_busy_ms, 3),
                        "fragmentReadyPenaltyMs": round(fragment_ready_ms, 3),
                        "healthPenaltyMs": round(health_penalty_ms, 3),
                        "healthScore": round(health_score, 3),
                        "leaseReason": lease_reason,
                    }
                    role_scores.setdefault(str(role), []).append(detail)
                    if best is None or (
                        score,
                        provider,
                    ) < (
                        best_score,
                        str(best.get("provider", "")),
                    ):
                        best = {
                            "provider": provider,
                            "assignment": str(item.get("assignment", "")).strip(),
                            "estimatedStartMs": round(start_ms, 3),
                            "estimatedFinishMs": round(finish_ms, 3),
                            "estimatedDurationMs": round(duration_ms, 3),
                            "leaseReason": lease_reason,
                        }
                        best_score = score
                        best_finish = finish_ms
                if best is not None:
                    selected[str(role)] = best
                    local_use[best["provider"]] += 1
                    local_available_at[best["provider"]] = best_finish
                    total_finish_ms = max(total_finish_ms, best_finish)
        for role, value in role_assignments.items():
            if role in selected:
                continue
            if isinstance(value, dict):
                provider = str(value.get("provider", value.get("providerName", ""))).strip()
                if provider:
                    selected[str(role)] = {
                        "provider": provider,
                        "assignment": str(value.get("assignment", "")).strip(),
                    }
                    local_use[provider] += 1
                    duration_ms = _candidate_duration_ms(
                        value,
                        getattr(args, "default_role_duration_ms", 500.0),
                    )
                    start_ms = max(
                        provider_available_at_ms.get(provider, float(current_ms)),
                        local_available_at.get(provider, float(current_ms)),
                    )
                    finish_ms = start_ms + duration_ms
                    local_available_at[provider] = finish_ms
                    total_finish_ms = max(total_finish_ms, finish_ms)
            elif value:
                provider = str(value).strip()
                selected[str(role)] = {"provider": provider, "assignment": ""}
                local_use[provider] += 1
                duration_ms = max(1.0, float(getattr(args, "default_role_duration_ms", 500.0)))
                start_ms = max(
                    provider_available_at_ms.get(provider, float(current_ms)),
                    local_available_at.get(provider, float(current_ms)),
                )
                finish_ms = start_ms + duration_ms
                local_available_at[provider] = finish_ms
                total_finish_ms = max(total_finish_ms, finish_ms)
        for provider, value in local_available_at.items():
            provider_available_at_ms[provider] = max(
                provider_available_at_ms.get(provider, 0.0),
                value,
            )
        return selected, dict(local_use), {
            "plannerMode": "lease-aware-rolling-window-with-ndnsd-health",
            "roleScores": role_scores,
            "rejectedCandidates": rejected,
            "ndnsdHealth": health_tracker.snapshot(),
            "providerReservations": {
                provider: round(value, 3)
                for provider, value in sorted(provider_available_at_ms.items())
            },
            "providerAckState": {
                provider: {
                    key: entry.get(key)
                    for key in ("queue", "readyQueue", "activeWorkers",
                                "workers", "idleWorkers", "runtimeStatus",
                                "observedAtMs")
                }
                for provider, entry in sorted(provider_ack_state.items())
            },
            "fragmentState": {
                provider: dict(provider_table)
                for provider, provider_table in sorted(fragment_state_table.items())
            },
            "estimatedWindowFinishMs": round(total_finish_ms, 3),
        }

    def handle(request: CoordinationRequest) -> CoordinationResponse:
        nonlocal window_version
        with state_lock:
            window_version += 1
            current_window_version = window_version
            current_ms = int(time.time() * 1000)
            suggestions = []
            intents = list(request.intents)
            if getattr(args, "enable_priority", False):
                intents.sort(
                    key=lambda item: (-float(getattr(item, "utility_weight", 1.0)),
                                      item.created_at_ms,
                                      item.intent_id))
                print(
                    "NDNSF_DI_ADVISORY_COORDINATOR_PRIORITY "
                    + json.dumps({
                        "enabled": True,
                        "intentOrder": [
                            {"intentId": item.intent_id,
                             "utilityWeight": getattr(item, "utility_weight", 1.0)}
                            for item in intents
                        ],
                        "windowVersion": current_window_version,
                    }, sort_keys=True),
                    flush=True,
                )
            for index, intent in enumerate(intents, start=1):
                if not intent.is_valid():
                    continue
                template_id = str(intent.payload.get("templateId", ""))
                role_assignments, local_use, score_breakdown = choose_role_assignments(
                    intent.payload,
                    current_ms,
                )
                provider_use.update(local_use)
                suggested = CoordinationSuggestion(
                    suggestion_id=f"{intent.intent_id or intent.request_id}-advisory-{index}",
                    intent_id=intent.intent_id,
                    request_id=intent.request_id,
                    service_name=intent.service_name,
                    coordinator_name=args.provider,
                    window_id=request.metadata.get("windowId", "native-tracer-advisory-window"),
                    expires_at_ms=(
                        0 if args.suggestion_ttl_ms <= 0 else
                        intent.created_at_ms + args.suggestion_ttl_ms
                    ),
                    payload_schema="ndnsf-di-assignment-suggestion-v1",
                    payload={
                        "templateId": template_id,
                        "roleAssignments": role_assignments,
                        "advisoryMode": "lease-aware-rolling-window-with-fragments",
                        "windowVersion": current_window_version,
                        "estimatedWindowFinishMs": score_breakdown["estimatedWindowFinishMs"],
                    },
                    score_breakdown={
                        **score_breakdown,
                        "intentDigest": intent.digest(),
                        "providerUse": dict(provider_use),
                        "windowVersion": current_window_version,
                    },
                )
                suggested = replace(
                    suggested,
                    proof=coordination_suggestion_proof(suggested, secret=args.proof_secret),
                )
                suggestions.append(suggested)
            _save_state(
                state_path,
                provider_use,
                provider_available_at_ms,
                current_window_version,
            )
        print(
            "NDNSF_DI_ADVISORY_COORDINATOR_REQUEST "
            + json.dumps({
                "intentCount": len(request.intents),
                "suggestionCount": len(suggestions),
                "service": args.service,
                "windowVersion": current_window_version,
                "fragmentStateSize": len(fragment_state_table),
            }, sort_keys=True),
            flush=True,
        )
        return CoordinationResponse(tuple(suggestions))

    return handle


def main() -> int:
    args = build_parser().parse_args()
    if args.dry_run:
        print(json.dumps({
            "service": args.service,
            "provider": args.provider,
            "group": args.group,
            "controller": args.controller,
            "trustSchema": args.trust_schema,
        }, indent=2, sort_keys=True))
        return 0
    provider = ServiceProvider(
        group=args.group,
        controller=args.controller,
        provider_prefix=args.provider,
        trust_schema=args.trust_schema,
        serve_certificates=True,
    )
    CoordinationServiceProvider(
        provider,
        make_handler(args),
        service_name=args.service,
    ).register()
    # Set initial NDNSD meta and start periodic heartbeat
    provider.set_ndnsd_meta({"role": "advisory-coordinator", "runtimeStatus": "ready"})
    provider.start_ndnsd_heartbeat(interval_seconds=10)
    return provider.run(args.service)


if __name__ == "__main__":
    raise SystemExit(main())

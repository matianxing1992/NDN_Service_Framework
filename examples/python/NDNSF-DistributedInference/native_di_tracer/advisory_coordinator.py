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
    PeerNetworkMetric,
    ProviderNetworkMatrix,
    RESIDENCY_READY_COST_MS,
    ServiceProvider,
    build_network_matrix_from_ndnsd,
    coordination_suggestion_proof,
    score_normalized,
)
from ndnsf_distributed_inference.runtime_v1 import (
    PLACEMENT_STRATEGY_PRESETS,
    DeploymentStatus,
    filter_feasible_providers,
    pick_optimal_placement,
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
    parser.add_argument("--edge-cost-factor", type=float, default=1.0,
                        help="Multiplier for inter-provider transfer costs in scoring")
    parser.add_argument("--score-weights", default="",
                        help="JSON file or string with per-dimension weights (fragment/queue/edge/health/fairness/compute)")
    parser.add_argument("--placement-strategy", default="best_score",
                        choices=["best_score", "pack", "spread", "min_replicas"],
                        help="Provider selection strategy (default: best_score)")
    parser.add_argument("--strategy-preset", default="gpu-cluster",
                        choices=["gpu-cluster", "edge-network", "multi-tenant", "high-availability"],
                        help="Pre-configured scoring weights for specific environments")
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


SCORE_WEIGHTS = {
    "fragment":    35,
    "queue":       20,
    "edge":        15,
    "health":      15,
    "fairness":    10,
    "compute":      5,
}


def _load_score_weights(raw: str) -> dict[str, int]:
    """Load score weights from JSON file or string."""
    import json as _json
    if not raw:
        return dict(SCORE_WEIGHTS)
    try:
        if raw.startswith("{") or raw.startswith("/"):
            loaded = _json.loads(raw) if raw.startswith("{") else _json.loads(
                __import__("pathlib").Path(raw).read_text(encoding="utf-8"))
        else:
            return dict(SCORE_WEIGHTS)
    except Exception:
        return dict(SCORE_WEIGHTS)
    result = dict(SCORE_WEIGHTS)
    for k in result:
        if k in loaded:
            result[k] = max(0, min(100, int(loaded[k])))
    return result


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
    deployments: dict[str, dict[str, Any]] = {}
    ref_count: dict[str, int] = {}  # deployment_id → active request count
    network_matrix = ProviderNetworkMatrix()
    preset = str(getattr(args, "strategy_preset", "gpu-cluster"))
    weights = dict(PLACEMENT_STRATEGY_PRESETS.get(preset, PLACEMENT_STRATEGY_PRESETS["gpu-cluster"]))
    user_weights = _load_score_weights(str(getattr(args, "score_weights", "")))
    weights.update(user_weights)  # user overrides take precedence
    placement_strategy = str(getattr(args, "placement_strategy", "best_score"))
    state_lock = threading.Lock()
    window_version = loaded_version

    def _refresh_network_matrix(ack_candidates: list[dict[str, Any]] | None = None) -> None:
        """Build ProviderNetworkMatrix from observed ACK RTT telemetry."""
        nonlocal network_matrix
        observed: list[PeerNetworkMetric] = []
        import json as _json
        # Try NDNSD-published metrics first
        try:
            ndnsd_raw = provider.get_ndnsd_services()
            if ndnsd_raw:
                ndnsd_list = [{str(k): v for k, v in item.items()} for item in ndnsd_raw]
                m = build_network_matrix_from_ndnsd(ndnsd_list)
                if m.metrics:
                    observed.extend(m.metrics.values())
        except Exception:
            pass
        # Also use ACK telemetry RTT as real-time metrics
        if ack_candidates:
            for c in ack_candidates:
                provider_name = str(c.get("provider", c.get("providerName", "")))
                telemetry = c.get("telemetry", c.get("_telemetry", {}))
                if isinstance(telemetry, dict):
                    rtt = float(telemetry.get("rtt_ms", telemetry.get("rttMs", 0)) or 0)
                    if rtt > 0:
                        observed.append(PeerNetworkMetric(
                            src_peer="user",
                            dst_peer=provider_name,
                            rtt_ms=rtt / 2.0,  # one-way ~ half of RTT
                            bandwidth_mbps=float(telemetry.get("goodput_mbps", 100)),
                            loss_rate=0.0,
                            confidence=0.8,
                        ))
        # For all known providers, add symmetric edges from ACK data
        known = list(set(
            m.dst_peer for m in observed
        ))
        for m in observed:
            for other in known:
                if m.dst_peer != other:
                    # If both reached user at similar RTT, they may be co-located
                    # Use observed RTT ratios as heuristic
                    observed.append(PeerNetworkMetric(
                        src_peer=m.dst_peer,
                        dst_peer=other,
                        rtt_ms=abs(m.rtt_ms - _get_ack_rtt(ack_candidates, other)) if ack_candidates else 5.0,
                        bandwidth_mbps=min(m.bandwidth_mbps, 1000),
                        confidence=0.3,
                    ))
        if observed:
            network_matrix = ProviderNetworkMatrix(observed, default_rtt_ms=5.0, unknown_penalty_ms=50.0)

    def _get_ack_rtt(candidates, provider_name):
        for c in (candidates or []):
            pn = str(c.get("provider", c.get("providerName", "")))
            if pn == provider_name:
                t = c.get("telemetry", c.get("_telemetry", {}))
                if isinstance(t, dict):
                    return float(t.get("rtt_ms", t.get("rttMs", 5.0)) or 5.0)
        return 5.0

    def choose_role_assignments(intent_payload: dict,
                                current_ms: int
                                ) -> tuple[dict[str, dict[str, Any]], dict[str, int], dict[str, Any]]:
        nonlocal network_matrix
        edge_factor = float(getattr(args, "edge_cost_factor", 1.0))
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
                best: dict[str, Any] | None = None
                best_score = -1.0
                best_finish = float(current_ms)
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
                    # === Phase 1: FILTER (hard constraints) ===
                    if not health_tracker.is_available(provider):
                        rejected.append({
                            "role": str(role), "provider": provider,
                            "reason": "CIRCUIT_OPEN",
                        })
                        continue

                    # === Phase 2: SCORE (normalized 0-100, weighted) ===
                    fragment_ready_ms = _fragment_ready_penalty(
                        provider, str(role), fragment_state_table,
                        float(getattr(args, "fragment_ready_default_ms",
                                      RESIDENCY_READY_COST_MS["MISSING"])),
                    )
                    ready_cost_ms = _candidate_ready_cost_ms(item)
                    queue_wait_ms = _candidate_queue_wait_ms(item)
                    duration_ms = _candidate_duration_ms(
                        item, getattr(args, "default_role_duration_ms", 500.0))
                    reserved_at_ms = max(
                        provider_available_at_ms.get(provider, float(current_ms)),
                        local_available_at.get(provider, float(current_ms)))
                    start_ms = max(float(current_ms) + float(lease_start_ms), reserved_at_ms)
                    finish_ms = start_ms + ready_cost_ms + queue_wait_ms + duration_ms
                    compute_ms = finish_ms - float(current_ms)

                    # Normalize each dimension to 0-100
                    health_val = 1.0 - health_tracker.health_score(provider)
                    fairness_count = provider_use[provider] + local_use[provider]
                    edge_cost_ms = 0.0
                    for prev_role, prev_info in selected.items():
                        prev_provider = str(prev_info.get("provider", ""))
                        if prev_provider and prev_provider != provider:
                            ems, _ = network_matrix.transfer_cost_ms(
                                prev_provider, provider,
                                int(item.get("_dependencyBytes", 100000)))
                            edge_cost_ms += ems * edge_factor

                    # All dimensions mapped to [0,100]; 100 = best
                    s_compute  = score_normalized(compute_ms,      worst=500,  best=0)
                    s_queue   = score_normalized(queue_wait_ms,    worst=200,  best=0)
                    s_fragment = score_normalized(
                        min(fragment_ready_ms, RESIDENCY_READY_COST_MS["MISSING"] * 2),
                        worst=RESIDENCY_READY_COST_MS["DISK_RESIDENT"] * 3, best=0)
                    s_edge    = score_normalized(edge_cost_ms,  worst=200,  best=0)
                    s_health  = (1.0 - health_val) * 100
                    s_fair    = score_normalized(float(fairness_count), worst=10, best=0)
                    # Affinity: bonus for co-locating with already-selected roles
                    s_affinity = 100.0 if any(
                        str(prev.get("provider", "")) == provider
                        for prev in selected.values()
                    ) else 0.0
                    s_anti = 100.0  # no anti-affinity conflict by default

                    total = (
                        weights.get("fragment", 35)   * s_fragment +
                        weights.get("queue", 20)       * s_queue +
                        weights.get("edge", 15)        * s_edge +
                        weights.get("health", 15)      * s_health +
                        weights.get("fairness", 10)    * s_fair +
                        weights.get("compute", 5)      * s_compute +
                        weights.get("affinity_bonus", 0) * s_affinity +
                        weights.get("anti_affinity", 0) * s_anti
                    ) / 100.0

                    detail = {
                        "role": str(role),
                        "provider": provider,
                        "assignment": str(item.get("assignment", "")).strip(),
                        "totalScore": round(total, 2),
                        "sCompute": round(s_compute, 1), "wCompute": weights.get("compute", 5),
                        "sQueue": round(s_queue, 1), "wQueue": weights.get("queue", 20),
                        "sFragment": round(s_fragment, 1), "wFragment": weights.get("fragment", 35),
                        "sEdge": round(s_edge, 1), "wEdge": weights.get("edge", 15),
                        "sHealth": round(s_health, 1), "wHealth": weights.get("health", 15),
                        "sFair": round(s_fair, 1), "wFairness": weights.get("fairness", 10),
                        "sAffinity": round(s_affinity, 1), "wAffinity": weights.get("affinity_bonus", 0),
                        "edgeCostMs": round(edge_cost_ms, 3),
                        "fragmentReadyMs": round(fragment_ready_ms, 3),
                        "queueWaitMs": round(queue_wait_ms, 3),
                        "computeMs": round(compute_ms, 3),
                        "startMs": round(start_ms, 3),
                        "finishMs": round(finish_ms, 3),
                        "leaseReason": lease_reason,
                    }
                    role_scores.setdefault(str(role), []).append(detail)
                    if best is None or total > best_score:
                        best = {
                            "provider": provider,
                            "assignment": str(item.get("assignment", "")).strip(),
                            "estimatedStartMs": round(start_ms, 3),
                            "estimatedFinishMs": round(finish_ms, 3),
                            "estimatedDurationMs": round(duration_ms, 3),
                            "leaseReason": lease_reason,
                        }
                        best_score = total
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
        import json as _json
        nonlocal window_version
        with state_lock:
            window_version += 1
            current_window_version = window_version
            current_ms = int(time.time() * 1000)
            _refresh_network_matrix()
            suggestions = []
            intents = list(request.intents)

            # --- Merge Provider: lease management ---
            for intent in intents[:]:
                purpose = str(intent.purpose).lower()
                if purpose in {"acquire-lease", "release-lease"}:
                    dep_id = str(intent.payload.get("deploymentId", intent.payload.get("deployment_id", "")))
                    if purpose == "acquire-lease":
                        lease = ExecutionLease.create(dep_id, user=intent.requester_name)
                        lease_table.grant(lease)
                        deployments.setdefault(dep_id, {
                            "deploymentId": dep_id, "status": "ACTIVE",
                            "fragmentMap": {}, "refCount": 0,
                        })
                        suggestions.append(CoordinationSuggestion(
                            suggestion_id=f"lease-{lease.lease_id}",
                            intent_id=intent.intent_id, request_id=intent.request_id,
                            service_name=intent.service_name,
                            coordinator_name=args.provider,
                            payload={"leaseId": lease.lease_id,
                                     "deploymentId": dep_id, "status": "GRANTED",
                                     "refCount": lease_table.active_count(dep_id)},
                        ))
                    else:
                        lease_id = str(intent.payload.get("leaseId", intent.payload.get("lease_id", "")))
                        released = lease_table.release(lease_id)
                        suggestions.append(CoordinationSuggestion(
                            suggestion_id=f"release-{lease_id}",
                            intent_id=intent.intent_id, request_id=intent.request_id,
                            service_name=intent.service_name,
                            coordinator_name=args.provider,
                            payload={"leaseId": lease_id,
                                     "status": "RELEASED" if released else "NOT_FOUND",
                                     "refCount": lease_table.active_count(dep_id) if dep_id else 0},
                        ))
                    intents.remove(intent)

            # --- Merge Provider: discover-deployments ---
            for intent in intents[:]:
                if intent.purpose == "discover-deployments":
                    svc = str(intent.payload.get("serviceName", intent.payload.get("service_name", "")))
                    dep_list = []
                    for did, dep in deployments.items():
                        ref = lease_table.active_count(did, now_ms_value=current_ms)
                        dep["refCount"] = ref
                        if svc and dep.get("serviceName", dep.get("service_name", "")) != svc:
                            continue
                        dep_list.append(dict(dep))
                    for dep_dict in dep_list:
                        suggestions.append(CoordinationSuggestion(
                            suggestion_id=f"discover-{dep_dict.get('deploymentId', '')}",
                            intent_id=intent.intent_id, request_id=intent.request_id,
                            service_name=intent.service_name,
                            coordinator_name=args.provider,
                            payload=dep_dict,
                        ))
                    intents.remove(intent)

            # --- Merge Provider: deploy intent ---
            for intent in intents[:]:
                if intent.purpose == "deploy":
                    dep_id = str(intent.payload.get("deploymentId", intent.payload.get("deployment_id", "")))
                    deployments[dep_id] = {
                        "deploymentId": dep_id, "planId": intent.payload.get("planId", ""),
                        "serviceName": intent.service_name, "status": "PROVISIONING",
                        "fragmentMap": intent.payload.get("roleAssignments", {}),
                        "refCount": 0, "createdAtMs": intent.created_at_ms,
                        "updatedAtMs": current_ms,
                    }
                    intents.remove(intent)

            # --- Publish deployments via NDNSD heartbeat ---
            dep_list = []
            for did, dep in deployments.items():
                ref = lease_table.active_count(did, now_ms_value=current_ms)
                dep["refCount"] = ref
                dep["updatedAtMs"] = current_ms
                dep_list.append(dep)
            try:
                provider.update_ndnsd_meta("deployments", _json.dumps(dep_list, sort_keys=True))
            except Exception:
                pass
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

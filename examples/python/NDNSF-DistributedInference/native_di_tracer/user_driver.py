#!/usr/bin/env python3
"""Submit a real NDNSF collaboration request for /Inference/NativeTracer."""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import csv
import json
import math
import os
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Optional

from ndnsf import (
    COORDINATION_ADVISORY_SERVICE,
    CoordinationIntent,
    CoordinationServiceClient,
    NdnMetrics,
    NdnsdHealthTracker,
    ProviderNetworkMatrix,
    RetryPolicy,
    ServiceUser,
    TokenBucket,
    TraceCollector,
    build_network_matrix_from_ndnsd,
    retry_call,
)
from ndnsf_distributed_inference.runtime_v1 import (
    PLACEMENT_STRATEGY_PRESETS,
    filter_feasible_providers,
    pick_optimal_placement,
)


SERVICE = "/Inference/NativeTracer"
GROUP = "/NDNSF-DI/Tracer/group"
CONTROLLER = "/NDNSF-DI/Tracer/controller"
USER = "/NDNSF-DI/Tracer/user"
ROLE_PROVIDER_PREFERENCE_ENV = "NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE"
ROLE_PROVIDER_PREFERENCE_LOCK = threading.Lock()


def encode_tensor_bundle() -> bytes:
    payload = bytearray(b"NDITB001")
    payload += struct.pack("<I", 1)
    name = b"images"
    payload += struct.pack("<I", len(name)) + name
    payload += struct.pack("<I", 1)  # Float32
    shape = [1, 3, 2, 2]
    payload += struct.pack("<I", len(shape))
    for dim in shape:
        payload += struct.pack("<q", dim)
    values = [float(i) / 10.0 for i in range(12)]
    data = struct.pack("<" + "f" * len(values), *values)
    payload += struct.pack("<Q", len(data)) + data
    return bytes(payload)


def load_service_plan(path: Path, service: str) -> dict:
    plan = json.loads(path.read_text(encoding="utf-8"))
    return next(item for item in plan["services"] if item["service"] == service)


def load_role_assignments(path: str) -> dict[str, dict[str, str]]:
    if not path:
        return {}
    assignments: dict[str, dict[str, str]] = {}
    with open(path, newline="", encoding="utf-8") as input_file:
        for row in csv.DictReader(input_file):
            role = str(row.get("role", "")).strip()
            provider = str(row.get("provider", "")).strip()
            if not role or not provider:
                continue
            assignments[role] = {
                "provider": provider,
                "assignment": str(row.get("assignment", "")).strip(),
            }
    return assignments


def load_role_assignment_candidates(path: str) -> dict[str, list[dict[str, Any]]]:
    if not path:
        return {}
    candidates: dict[str, list[dict[str, str]]] = {}
    with open(path, newline="", encoding="utf-8") as input_file:
        for row in csv.DictReader(input_file):
            role = str(row.get("role", "")).strip()
            provider = str(row.get("provider", "")).strip()
            if not role or not provider:
                continue
            candidates.setdefault(role, []).append({
                "provider": provider,
                "assignment": str(row.get("assignment", "")).strip(),
            })
    return candidates


def load_fragment_inventory(path: str) -> dict[str, dict[str, dict[str, Any]]]:
    """Load fragment inventory JSON and convert to fragmentState format.

    Reads ``latestByProviderRole`` from the inventory and builds a dict
    keyed by provider, then role, with residency/fragmentDigest/readyCostMs.
    """
    if not path:
        return {}
    try:
        with Path(path).expanduser().open(encoding="utf-8") as input_file:
            payload = json.load(input_file)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}
    if not isinstance(payload, dict):
        return {}
    latest = payload.get("latestByProviderRole", {})
    if not isinstance(latest, dict):
        return {}
    residency_cost = {
        "GPU_LOADED": 0.0,
        "CPU_RESIDENT": 8.0,
        "DISK_RESIDENT": 35.0,
        "REPO_AVAILABLE": 120.0,
        "MISSING": 1_000_000.0,
    }
    fragment_state: dict[str, dict[str, dict[str, Any]]] = {}
    for key, entry in latest.items():
        if not isinstance(entry, dict):
            continue
        provider = str(entry.get("provider", "")).strip()
        role = str(entry.get("role", "")).strip()
        if not provider or not role:
            continue
        residency = str(entry.get("residency", "MISSING"))
        fragment_state.setdefault(provider, {})[role] = {
            "residency": residency,
            "fragmentDigest": str(entry.get("fragmentDigest", "")),
            "readyCostMs": residency_cost.get(residency, residency_cost["MISSING"]),
            "backend": str(entry.get("backend", "")),
            "path": str(entry.get("path", "")),
            "observedAtMs": int(entry.get("epochMs", 0)),
        }
    return fragment_state


def load_runtime_hints(path: str) -> dict[str, Any]:
    if not path:
        return {}
    try:
        with Path(path).expanduser().open(encoding="utf-8") as input_file:
            payload = json.load(input_file)
    except FileNotFoundError:
        return {}
    return payload if isinstance(payload, dict) else {}


def _runtime_hint_for_candidate(runtime_hints: dict[str, Any],
                                role: str,
                                provider: str) -> dict[str, Any]:
    provider_roles = runtime_hints.get("providerRoles", {})
    if isinstance(provider_roles, dict):
        direct = provider_roles.get(f"{provider}|{role}")
        if isinstance(direct, dict):
            return direct
    providers = runtime_hints.get("providers", {})
    if isinstance(providers, dict):
        provider_payload = providers.get(provider, {})
        if isinstance(provider_payload, dict):
            roles = provider_payload.get("roles", {})
            role_payload = roles.get(role, {}) if isinstance(roles, dict) else {}
            if isinstance(role_payload, dict):
                merged = dict(provider_payload)
                merged.pop("roles", None)
                merged.update(role_payload)
                return merged
            merged = dict(provider_payload)
            merged.pop("roles", None)
            return merged
    return {}


def enrich_role_candidates_with_runtime_hints(
    candidates: dict[str, list[dict[str, Any]]],
    runtime_hints: dict[str, Any],
) -> dict[str, list[dict[str, Any]]]:
    if not runtime_hints:
        return candidates
    enriched: dict[str, list[dict[str, Any]]] = {}
    for role, items in candidates.items():
        enriched_items = []
        for item in items:
            provider = str(item.get("provider", item.get("providerName", ""))).strip()
            merged = dict(item)
            hint = _runtime_hint_for_candidate(runtime_hints, role, provider)
            runtime_hint = hint.get("runtimeHint", hint.get("runtime_hint", {}))
            if isinstance(runtime_hint, dict):
                merged["runtimeHint"] = runtime_hint
            lease_offers = hint.get("leaseOffers", hint.get("lease_offers", []))
            if isinstance(lease_offers, dict):
                merged["leaseOffers"] = [lease_offers]
            elif isinstance(lease_offers, list):
                merged["leaseOffers"] = [
                    offer for offer in lease_offers if isinstance(offer, dict)
                ]
            for key in (
                "estimatedDurationMs",
                "durationMs",
                "readyCostMs",
                "estimatedReadyMs",
                "fragmentDigest",
                "residency",
            ):
                if key in hint:
                    merged[key] = hint[key]
            enriched_items.append(merged)
        enriched[role] = enriched_items
    return enriched


def role_provider_preference_from_advisory(advisory: dict,
                                           roles: list[dict]) -> str:
    if not advisory.get("enabled") or advisory.get("status") != "executed":
        return ""
    allowed_roles = {
        str(item.get("role", "")).strip()
        for item in roles
        if str(item.get("role", "")).strip()
    }
    if not allowed_roles:
        return ""
    suggestions = advisory.get("suggestions", [])
    if not suggestions:
        return ""
    first = suggestions[0] if isinstance(suggestions[0], dict) else {}
    role_assignments = first.get("roleAssignments", {})
    if not isinstance(role_assignments, dict):
        return ""
    preferences = []
    for role in sorted(allowed_roles):
        value = role_assignments.get(role)
        provider = ""
        if isinstance(value, dict):
            provider = str(value.get("provider", value.get("providerName", ""))).strip()
        elif value is not None:
            provider = str(value).strip()
        if provider:
            preferences.append(f"{role}=>{provider}")
    return ";".join(preferences) + (";" if preferences else "")


@contextlib.contextmanager
def role_provider_preference_env(preference: str):
    if not preference:
        yield
        return
    with ROLE_PROVIDER_PREFERENCE_LOCK:
        previous = os.environ.get(ROLE_PROVIDER_PREFERENCE_ENV)
        os.environ[ROLE_PROVIDER_PREFERENCE_ENV] = preference
        try:
            yield
        finally:
            if previous is None:
                os.environ.pop(ROLE_PROVIDER_PREFERENCE_ENV, None)
            else:
                os.environ[ROLE_PROVIDER_PREFERENCE_ENV] = previous


def sample_service_plan(service: str) -> dict:
    return {
        "service": service,
        "roles": ["/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"],
        "dependencies": [
            {
                "producers": ["/Backbone"],
                "consumers": ["/Head/Shard/0"],
                "keyScope": "backbone-to-head0",
                "topicPrefix": "/activation",
                "required": True,
            },
            {
                "producers": ["/Backbone"],
                "consumers": ["/Head/Shard/1"],
                "keyScope": "backbone-to-head1",
                "topicPrefix": "/activation",
                "required": True,
            },
            {
                "producers": ["/Head/Shard/0"],
                "consumers": ["/Merge"],
                "keyScope": "head0-to-merge",
                "topicPrefix": "/activation",
                "required": True,
            },
            {
                "producers": ["/Head/Shard/1"],
                "consumers": ["/Merge"],
                "keyScope": "head1-to-merge",
                "topicPrefix": "/activation",
                "required": True,
            },
        ],
    }


def collaboration_roles(service_plan: dict, service: str) -> list[dict]:
    return [
        {
            "role": role,
            "service": service,
            "min_providers": 1,
            "max_providers": 1,
        }
        for role in service_plan["roles"]
    ]


def collaboration_dependencies(service_plan: dict) -> list[dict]:
    deps = []
    for dep in service_plan.get("dependencies", []):
        deps.append({
            "producers": list(dep.get("producers", [])),
            "consumers": list(dep.get("consumers", [])),
            "key_scope": str(dep["keyScope"]),
            "topic_prefix": str(dep.get("topicPrefix", "/activation")),
            "required": bool(dep.get("required", True)),
        })
    return deps


def key_scopes_and_role_scopes(service_plan: dict) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    key_scopes: dict[str, list[str]] = {}
    role_scopes: dict[str, list[str]] = {role: [] for role in service_plan["roles"]}
    for dep in service_plan.get("dependencies", []):
        scope = str(dep["keyScope"])
        roles = list(dep.get("producers", [])) + list(dep.get("consumers", []))
        key_scopes[scope] = roles
        for role in roles:
            role_scopes.setdefault(role, []).append(scope)
    return key_scopes, role_scopes


def publish_scope_keys(user: ServiceUser, service: str, key_scopes: dict[str, list[str]]) -> dict[str, str]:
    scope_key_data_names: dict[str, str] = {}
    for scope in key_scopes:
        result = user.publish_encrypted_large_data(
            service,
            secrets.token_bytes(32),
            object_label=f"native-tracer-scope-key-{scope}",
            freshness_ms=60000,
        )
        if not result.success:
            raise RuntimeError(f"scope key publish failed for {scope}: {result.error}")
        scope_key_data_names[scope] = result.encrypted_data_name
    return scope_key_data_names


def percentile_nearest_rank(values: list[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, math.ceil((percentile / 100.0) * len(ordered)))
    return float(ordered[min(rank, len(ordered)) - 1])


def open_loop_planned_requests(args) -> int:
    return max(1, min(
        args.requests,
        int(math.ceil(args.open_loop_duration_s * args.target_rps)),
    ))


def summarize_workload(results: list[dict],
                       makespan_ms: float,
                       service: str,
                       concurrency: int,
                       metadata: Optional[dict] = None) -> dict:
    latencies = [float(item.get("elapsedMs", 0.0)) for item in results]
    successes = [item for item in results if item.get("status") == "executed"]
    summary = {
        "status": "executed" if len(successes) == len(results) else "failed",
        "service": service,
        "requestCount": len(results),
        "concurrency": concurrency,
        "successCount": len(successes),
        "failureCount": len(results) - len(successes),
        "responseStatus": len(successes) == len(results),
        "payloadBytes": int(sum(int(item.get("payloadBytes", 0)) for item in results)),
        "elapsedMs": makespan_ms,
        "makespanMs": makespan_ms,
        "meanMs": (sum(latencies) / len(latencies)) if latencies else 0.0,
        "p50Ms": percentile_nearest_rank(latencies, 50.0),
        "p95Ms": percentile_nearest_rank(latencies, 95.0),
        "minMs": min(latencies) if latencies else 0.0,
        "maxMs": max(latencies) if latencies else 0.0,
        "throughputRps": (len(successes) / (makespan_ms / 1000.0)) if makespan_ms > 0 else 0.0,
        "overloadFastFailCount": sum(
            1 for item in results
            if bool(item.get("overloadFastFail", False))
        ),
        "error": "; ".join(
            str(item.get("error", ""))
            for item in results
            if item.get("status") != "executed" and item.get("error")
        ),
        "requests": results,
    }
    if metadata:
        summary.update(metadata)
    return summary


def runtime_replan_metadata(args) -> dict:
    max_replans = max(0, int(getattr(args, "runtime_aware_max_replans", 0)))
    reasons = [
        item.strip()
        for item in str(getattr(args, "runtime_aware_replan_reasons", "")).split(",")
        if item.strip()
    ]
    if max_replans == 0 and not reasons:
        return {}
    executed = min(max_replans, len(reasons))
    return {
        "runtimeAwareReplan": {
            "enabled": max_replans > 0,
            "maxReplans": max_replans,
            "requestedReplanReasons": reasons,
            "replanCount": executed,
            "status": (
                "max-attempts-exceeded"
                if len(reasons) > max_replans else
                "executed" if executed else "not-needed"),
        }
    }


def coordination_metadata(args) -> dict:
    if not args.coordination_service:
        return {"coordination": {"enabled": False}}
    return {
        "coordination": {
            "enabled": True,
            "service": args.coordination_service,
            "ackTimeoutMs": args.coordination_ack_timeout_ms,
            "timeoutMs": args.coordination_timeout_ms,
        }
    }


def effective_timeout_ms(args) -> int:
    fast_fail = int(getattr(args, "overload_fast_fail_timeout_ms", 0) or 0)
    if fast_fail <= 0:
        return int(args.timeout_ms)
    return max(1, min(int(args.timeout_ms), fast_fail))


def overload_fast_fail_metadata(args) -> dict:
    effective = effective_timeout_ms(args)
    enabled = int(getattr(args, "overload_fast_fail_timeout_ms", 0) or 0) > 0
    return {
        "overloadFastFail": {
            "enabled": enabled,
            "configuredTimeoutMs": int(getattr(args, "overload_fast_fail_timeout_ms", 0) or 0),
            "effectiveTimeoutMs": effective,
            "baseTimeoutMs": int(args.timeout_ms),
        }
    }


def is_overload_fast_fail_error(args, error: str, elapsed_ms: float) -> bool:
    if int(getattr(args, "overload_fast_fail_timeout_ms", 0) or 0) <= 0:
        return False
    if not error:
        return False
    normalized = error.lower()
    if "timeout" not in normalized and "local deadline" not in normalized:
        return False
    return elapsed_ms <= float(effective_timeout_ms(args) + 5000)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the NativeTracer user driver")
    parser.add_argument("--plan", default="")
    parser.add_argument("--service", default=SERVICE)
    parser.add_argument("--group", default=GROUP)
    parser.add_argument("--controller", default=CONTROLLER)
    parser.add_argument("--user", default=USER)
    parser.add_argument("--trust-schema", default="examples/trust-schema.conf")
    parser.add_argument("--ack-timeout-ms", type=int, default=1200)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--requests", type=int, default=1,
                        help="Number of closed-loop collaboration requests to submit")
    parser.add_argument("--concurrency", type=int, default=1,
                        help="Maximum outstanding collaboration requests")
    parser.add_argument("--submission-spacing-ms", type=int, default=0,
                        help="Delay between child request submissions in concurrent mode")
    parser.add_argument("--target-rps", type=float, default=0.0,
                        help="Open-loop offered request rate; requires --open-loop-duration-s")
    parser.add_argument("--open-loop-duration-s", type=float, default=0.0,
                        help="Submit requests on a fixed schedule for this many seconds")
    parser.add_argument("--open-loop-driver-mode",
                        choices=["child", "threaded", "process-pool"],
                        default="child",
                        help="Open-loop user driver implementation")
    parser.add_argument("--burst-admission-providers", default="",
                        help=("Comma-separated provider names used to seed "
                              "per-child burst admission bias"))
    parser.add_argument("--runtime-aware-max-replans", type=int, default=0,
                        help="Bounded runtime-aware planner replan attempt budget")
    parser.add_argument("--runtime-aware-replan-reasons", default="",
                        help="Comma-separated diagnostic reasons to record in replan metrics")
    parser.add_argument("--coordination-service", default="",
                        help="Optional NDNSF coordination service for advisory planning")
    parser.add_argument("--assignment-csv", default="",
                        help="Optional role/provider assignment CSV included in advisory intents")
    parser.add_argument("--runtime-hints-json", default="",
                        help=("Optional provider runtime/lease hint snapshot merged "
                              "into advisory role candidates"))
    parser.add_argument("--fragment-inventory-json", default="",
                        help=("Optional provider fragment inventory JSON used to "
                              "enrich coordination intents with live fragment residency"))
    parser.add_argument("--max-rps", type=float, default=0.0,
                        help="Per-user token-bucket rate limit (0 = unlimited)")
    parser.add_argument("--retry-max-attempts", type=int, default=0,
                        help="Max retry attempts per request (0 = no retry)")
    parser.add_argument("--wait-for-deployment", default="",
                        help="Wait for this deployment_id to become ACTIVE before starting requests")
    parser.add_argument("--coordination-ack-timeout-ms", type=int, default=800)
    parser.add_argument("--coordination-timeout-ms", type=int, default=5000)
    parser.add_argument("--overload-fast-fail-timeout-ms", type=int, default=0,
                        help=("Use this shorter collaboration timeout for overload "
                              "experiments and record fast-fail diagnostics"))
    parser.add_argument("--worker-child", action="store_true",
                        help=argparse.SUPPRESS)
    parser.add_argument("--request-index", type=int, default=1,
                        help=argparse.SUPPRESS)
    parser.add_argument("--worker-request-indices", default="",
                        help=argparse.SUPPRESS)
    parser.add_argument("--schedule-start-epoch", type=float, default=0.0,
                        help=argparse.SUPPRESS)
    parser.add_argument("--scope-key-data-names-json", default="",
                        help=argparse.SUPPRESS)
    parser.add_argument("--dry-run", action="store_true")
    return parser


def request_advisory_suggestion(user: ServiceUser,
                                args,
                                service_plan: dict,
                                index: int,
                                observed_ack_runtime: dict[str, dict[str, Any]] | None = None) -> dict:
    if not args.coordination_service:
        return {"enabled": False, "mode": "local-placement"}
    client = CoordinationServiceClient(
        user,
        service_name=args.coordination_service,
        ack_timeout_ms=args.coordination_ack_timeout_ms,
        timeout_ms=args.coordination_timeout_ms,
    )
    started = time.perf_counter()
    role_assignments = load_role_assignments(args.assignment_csv)
    role_candidates = load_role_assignment_candidates(args.assignment_csv)
    runtime_hints = load_runtime_hints(args.runtime_hints_json)
    role_candidates = enrich_role_candidates_with_runtime_hints(
        role_candidates,
        runtime_hints,
    )
    fragment_inventory = load_fragment_inventory(args.fragment_inventory_json)
    intent_payload = {
        "templateId": service_plan.get("planId", "native-tracer-plan"),
        "service": args.service,
        "roles": list(service_plan.get("roles", [])),
        "roleAssignments": role_assignments,
        "roleCandidates": role_candidates,
        "runtimeHintSnapshot": {
            "enabled": bool(runtime_hints),
            "schema": runtime_hints.get("schema", "") if runtime_hints else "",
            "generatedAtMs": runtime_hints.get("generatedAtMs", 0) if runtime_hints else 0,
        },
    }
    if observed_ack_runtime:
        intent_payload["observedAckRuntime"] = observed_ack_runtime
    if fragment_inventory:
        intent_payload["fragmentState"] = fragment_inventory
    ndnsd_services = user.get_ndnsd_services()
    if ndnsd_services:
        health = NdnsdHealthTracker()
        health.update_from_ndnsd(ndnsd_services)
        intent_payload["ndnsdProviderState"] = health.snapshot()
    intent = CoordinationIntent(
        intent_id=f"{args.user}-intent-{index}",
        request_id=f"native-tracer-{index}",
        requester_name=args.user,
        service_name=args.service,
        purpose="advisory-planning",
        nonce=secrets.token_hex(16),
        expires_at_ms=int(time.time() * 1000) + max(1000, int(args.coordination_timeout_ms)),
        payload_schema="ndnsf-di-plan-intent-v1",
        payload=intent_payload,
    )
    try:
        response = client.request([intent], metadata={"windowId": f"native-tracer-{index}"})
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        suggestions = [item.payload for item in response.suggestions]
        score_breakdowns = [
            item.score_breakdown for item in response.suggestions
            if item.score_breakdown
        ]
        result = {
            "enabled": True,
            "status": "executed",
            "service": args.coordination_service,
            "suggestionCount": len(response.suggestions),
            "elapsedMs": elapsed_ms,
            "suggestions": suggestions,
            "scoreBreakdowns": score_breakdowns,
        }
    except Exception as exc:
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        result = {
            "enabled": True,
            "status": "failed",
            "service": args.coordination_service,
            "suggestionCount": 0,
            "elapsedMs": elapsed_ms,
            "error": str(exc),
        }
    print("NDNSF_DI_ADVISORY_COORDINATION " + json.dumps(result, sort_keys=True), flush=True)
    return result


def parse_semicolon_fields(payload: bytes | str) -> dict[str, str]:
    text = payload.decode("utf-8", errors="replace") if isinstance(payload, bytes) else str(payload)
    fields: dict[str, str] = {}
    for item in text.split(";"):
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        if key:
            fields[key] = value
    return fields


def int_field(fields: dict[str, str], key: str, default: int = 0) -> int:
    try:
        return int(float(fields.get(key, default)))
    except (TypeError, ValueError):
        return default


def ack_candidates_snapshot(candidates) -> list[dict[str, Any]]:
    snapshot = []
    for candidate in candidates:
        payload = bytes(candidate.payload)
        fields = parse_semicolon_fields(payload)
        snapshot.append({
            "provider": candidate.provider_name,
            "service": candidate.service_name,
            "requestId": candidate.request_id,
            "status": bool(candidate.status),
            "message": candidate.message,
            "payloadText": payload.decode("utf-8", errors="replace"),
            "roles": fields.get("roles", fields.get("role", "")),
            "queue": int_field(fields, "queue"),
            "readyQueue": int_field(fields, "readyQueue"),
            "waitingInputs": int_field(fields, "waitingInputs"),
            "activeWorkers": int_field(fields, "activeWorkers"),
            "workers": int_field(fields, "workers"),
            "idleWorkers": int_field(fields, "idleWorkers"),
            "runtimeStatus": fields.get("runtimeStatus", ""),
            "negativeAckReason": fields.get("negativeAckReason", ""),
            "deploymentId": fields.get("deploymentId", ""),
            "provisioningRole": fields.get("provisioningRole", ""),
            "expectedReadyMs": fields.get("expectedReadyMs", ""),
            "leaseId": fields.get("leaseId", ""),
            "leaseExpiresAtMs": fields.get("leaseExpiresAtMs", ""),
            "telemetry": candidate.telemetry,
        })
    return snapshot


def observed_ack_runtime_from_snapshots(snapshots: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Convert ack candidate snapshots into per-provider observed runtime state.

    For each provider, keep the latest (highest-valued) runtime counters
    across all snapshots, so the advisory coordinator sees the worst-case
    observed load.
    """
    providers: dict[str, dict[str, Any]] = {}
    for snap in snapshots:
        provider = str(snap.get("provider", "")).strip()
        if not provider:
            continue
        entry = providers.get(provider, {})
        entry["provider"] = provider
        for key in (
            "queue", "readyQueue", "waitingInputs",
            "activeWorkers", "workers", "idleWorkers",
            "runtimeStatus", "negativeAckReason",
            "leaseId", "leaseExpiresAtMs",
        ):
            current = entry.get(key)
            value = snap.get(key)
            if current is None:
                entry[key] = value
            elif isinstance(current, int) and isinstance(value, int):
                entry[key] = max(current, value)
            elif value is not None and value != "":
                entry[key] = value
        providers[provider] = entry
    return providers


def run_one_request(user: ServiceUser,
                    args,
                    service_plan: dict,
                    roles: list[dict],
                    key_scopes: dict[str, list[str]],
                    dependencies: list[dict],
                    scope_key_data_names: dict[str, str],
                    role_scopes: dict[str, list[str]],
                    index: int,
                    observed_ack_runtime: dict[str, dict[str, Any]] | None = None) -> dict:
    start = time.perf_counter()
    try:
        advisory = request_advisory_suggestion(
            user, args, service_plan, index,
            observed_ack_runtime=observed_ack_runtime,
        )
        preference = role_provider_preference_from_advisory(advisory, roles)
        if preference:
            advisory["appliedRoleProviderPreference"] = preference
        ack_snapshots: list[dict[str, Any]] = []

        def observe_ack_candidates(candidates) -> None:
            ack_snapshots.extend(ack_candidates_snapshot(candidates))

        with role_provider_preference_env(preference):
            response = user.request_collaboration(
                args.service,
                encode_tensor_bundle(),
                roles=roles,
                key_scopes=key_scopes,
                dependencies=dependencies,
                scope_key_data_names=scope_key_data_names,
                role_scopes=role_scopes,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=effective_timeout_ms(args),
                ack_observer=observe_ack_candidates,
            )
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        result = {
            "status": "executed" if response.status else "failed",
            "service": args.service,
            "requestIndex": index,
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "responseStatus": bool(response.status),
            "payloadBytes": len(response.payload),
            "error": response.error,
            "elapsedMs": elapsed_ms,
            "coordination": advisory,
            "ackCandidateSnapshot": ack_snapshots,
        }
        if is_overload_fast_fail_error(args, str(response.error), elapsed_ms):
            result["overloadFastFail"] = True
        return result
    except Exception as exc:
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        error = str(exc)
        result = {
            "status": "failed",
            "service": args.service,
            "requestIndex": index,
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": error,
            "elapsedMs": elapsed_ms,
            "coordination": {"enabled": bool(args.coordination_service), "status": "failed"},
        }
        if is_overload_fast_fail_error(args, error, elapsed_ms):
            result["overloadFastFail"] = True
        return result


def run_async_requests(user: ServiceUser,
                       args,
                       roles: list[dict],
                       key_scopes: dict[str, list[str]],
                       dependencies: list[dict],
                       scope_key_data_names: dict[str, str],
                       role_scopes: dict[str, list[str]]) -> list[dict]:
    condition = threading.Condition()
    starts: dict[int, float] = {}
    results: dict[int, dict] = {}
    state = {
        "next": 1,
        "inFlight": 0,
        "completed": 0,
    }

    def record_result(index: int, response_status: bool, payload: bytes, error: str) -> None:
        elapsed_ms = (time.perf_counter() - starts.get(index, time.perf_counter())) * 1000.0
        result = {
            "status": "executed" if response_status else "failed",
            "service": args.service,
            "requestIndex": index,
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "responseStatus": bool(response_status),
            "payloadBytes": len(payload),
            "error": error,
            "elapsedMs": elapsed_ms,
        }
        if is_overload_fast_fail_error(args, error, elapsed_ms):
            result["overloadFastFail"] = True
        print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
        with condition:
            results[index] = result
            state["inFlight"] -= 1
            state["completed"] += 1
            submit_locked()
            condition.notify_all()

    def submit_one_locked(index: int) -> None:
        starts[index] = time.perf_counter()
        state["inFlight"] += 1
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_SUBMIT "
            + json.dumps({
                "requestIndex": index,
                "requestCount": args.requests,
                "concurrency": args.concurrency,
            }, sort_keys=True),
            flush=True,
        )

        def on_response(response) -> None:
            record_result(index, bool(response.status), bytes(response.payload), str(response.error))

        def on_timeout(request_id: str) -> None:
            record_result(index, False, b"", "timeout: " + str(request_id))

        try:
            user.request_collaboration_async(
                args.service,
                encode_tensor_bundle(),
                roles=roles,
                key_scopes=key_scopes,
                dependencies=dependencies,
                scope_key_data_names=scope_key_data_names,
                role_scopes=role_scopes,
                on_response=on_response,
                on_timeout=on_timeout,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=effective_timeout_ms(args),
            )
        except Exception as exc:
            state["inFlight"] -= 1
            results[index] = {
                "status": "failed",
                "service": args.service,
                "requestIndex": index,
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "responseStatus": False,
                "payloadBytes": 0,
                "error": str(exc),
                "elapsedMs": (time.perf_counter() - starts[index]) * 1000.0,
            }
            if is_overload_fast_fail_error(
                args,
                str(exc),
                float(results[index]["elapsedMs"]),
            ):
                results[index]["overloadFastFail"] = True
            state["completed"] += 1

    def submit_locked() -> None:
        while (state["inFlight"] < args.concurrency and
               state["next"] <= args.requests):
            index = state["next"]
            state["next"] += 1
            submit_one_locked(index)

    deadline = time.perf_counter() + (
        ((effective_timeout_ms(args) + 3000) / 1000.0) *
        max(1, math.ceil(args.requests / max(1, args.concurrency))) + 10.0)
    print(
        "NDNSF_DI_NATIVE_TRACER_USER_ASYNC_WAIT "
        + json.dumps({
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "deadlineSeconds": round(deadline - time.perf_counter(), 3),
        }, sort_keys=True),
        flush=True,
    )
    with condition:
        submit_locked()
        while state["completed"] < args.requests and time.perf_counter() < deadline:
            condition.wait(timeout=0.1)

    for index in range(1, args.requests + 1):
        if index not in results:
            result = {
                "status": "failed",
                "service": args.service,
                "requestIndex": index,
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "responseStatus": False,
                "payloadBytes": 0,
                "error": "local workload deadline",
                "elapsedMs": (time.perf_counter() - starts.get(index, time.perf_counter())) * 1000.0,
            }
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
            results[index] = result
    user.stop()
    return [results[index] for index in sorted(results)]


def run_open_loop_requests(user: ServiceUser,
                           args,
                           roles: list[dict],
                           key_scopes: dict[str, list[str]],
                           dependencies: list[dict],
                           scope_key_data_names: dict[str, str],
                           role_scopes: dict[str, list[str]]) -> tuple[list[dict], dict]:
    planned = open_loop_planned_requests(args)
    condition = threading.Condition()
    starts: dict[int, float] = {}
    results: dict[int, dict] = {}
    state = {
        "inFlight": 0,
        "submitted": 0,
        "dropped": 0,
        "backpressureWaits": 0,
        "maxScheduleSlipMs": 0.0,
        "completed": 0,
    }
    schedule_start = time.perf_counter()

    def make_result(index: int,
                    response_status: bool,
                    payload: bytes,
                    error: str,
                    elapsed_ms: float,
                    mode: str = "open-loop") -> dict:
        result = {
            "status": "executed" if response_status else "failed",
            "service": args.service,
            "requestIndex": index,
            "requestCount": planned,
            "concurrency": args.concurrency,
            "mode": mode,
            "targetRps": args.target_rps,
            "openLoopDurationS": args.open_loop_duration_s,
            "responseStatus": bool(response_status),
            "payloadBytes": len(payload),
            "error": error,
            "elapsedMs": elapsed_ms,
        }
        if is_overload_fast_fail_error(args, error, elapsed_ms):
            result["overloadFastFail"] = True
        return result

    def record_result(index: int, response_status: bool, payload: bytes, error: str) -> None:
        elapsed_ms = (time.perf_counter() - starts.get(index, time.perf_counter())) * 1000.0
        result = make_result(index, response_status, payload, error, elapsed_ms)
        print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
        with condition:
            results[index] = result
            state["inFlight"] -= 1
            state["completed"] += 1
            condition.notify_all()

    def submit_one_locked(index: int) -> None:
        starts[index] = time.perf_counter()
        scheduled_at = schedule_start + ((index - 1) / args.target_rps)
        schedule_slip_ms = max(0.0, (starts[index] - scheduled_at) * 1000.0)
        state["maxScheduleSlipMs"] = max(state["maxScheduleSlipMs"], schedule_slip_ms)
        state["inFlight"] += 1
        state["submitted"] += 1
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_SUBMIT "
            + json.dumps({
                "mode": "open-loop",
                "requestIndex": index,
                "requestCount": planned,
                "concurrency": args.concurrency,
                "targetRps": args.target_rps,
                "openLoopDurationS": args.open_loop_duration_s,
                "scheduledOffsetMs": round((starts[index] - schedule_start) * 1000.0, 3),
                "scheduleSlipMs": round(schedule_slip_ms, 3),
            }, sort_keys=True),
            flush=True,
        )

        def on_response(response) -> None:
            record_result(index, bool(response.status), bytes(response.payload), str(response.error))

        def on_timeout(request_id: str) -> None:
            record_result(index, False, b"", "timeout: " + str(request_id))

        try:
            user.request_collaboration_async(
                args.service,
                encode_tensor_bundle(),
                roles=roles,
                key_scopes=key_scopes,
                dependencies=dependencies,
                scope_key_data_names=scope_key_data_names,
                role_scopes=role_scopes,
                on_response=on_response,
                on_timeout=on_timeout,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=effective_timeout_ms(args),
            )
        except Exception as exc:
            state["inFlight"] -= 1
            state["completed"] += 1
            results[index] = make_result(
                index,
                False,
                b"",
                str(exc),
                (time.perf_counter() - starts[index]) * 1000.0)
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " +
                  json.dumps(results[index], sort_keys=True), flush=True)
            condition.notify_all()

    print(
        "NDNSF_DI_NATIVE_TRACER_USER_OPEN_LOOP "
        + json.dumps({
            "requestCount": planned,
            "requestCap": args.requests,
            "concurrency": args.concurrency,
            "targetRps": args.target_rps,
            "openLoopDurationS": args.open_loop_duration_s,
        }, sort_keys=True),
        flush=True,
    )
    for index in range(1, planned + 1):
        target_time = schedule_start + ((index - 1) / args.target_rps)
        delay = target_time - time.perf_counter()
        if delay > 0:
            time.sleep(delay)
        with condition:
            while state["inFlight"] >= args.concurrency:
                state["backpressureWaits"] += 1
                condition.wait(timeout=0.1)
            submit_one_locked(index)

    deadline = max(
        schedule_start + args.open_loop_duration_s + (effective_timeout_ms(args) / 1000.0) + 20.0,
        time.perf_counter() + (effective_timeout_ms(args) / 1000.0) + 5.0,
    )
    with condition:
        while state["completed"] < planned and time.perf_counter() < deadline:
            condition.wait(timeout=0.1)

    for index in range(1, planned + 1):
        if index not in results:
            result = make_result(
                index,
                False,
                b"",
                "local workload deadline",
                (time.perf_counter() - starts.get(index, time.perf_counter())) * 1000.0,
            )
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
            results[index] = result

    metadata = {
        "mode": "open-loop",
        "targetRps": args.target_rps,
        "openLoopDurationS": args.open_loop_duration_s,
        "scheduledRequestCount": planned,
        "submittedCount": int(state["submitted"]),
        "localBackpressureCount": int(state["dropped"]),
        "localBackpressureWaitCount": int(state["backpressureWaits"]),
        "maxScheduleSlipMs": round(float(state["maxScheduleSlipMs"]), 3),
        "offeredRps": planned / args.open_loop_duration_s if args.open_loop_duration_s > 0 else 0.0,
    }
    return [results[index] for index in sorted(results)], metadata


def run_threaded_requests(users: list[ServiceUser],
                          args,
                          service_plan: dict,
                          roles: list[dict],
                          key_scopes: dict[str, list[str]],
                          dependencies: list[dict],
                          scope_key_data_names: dict[str, str],
                          role_scopes: dict[str, list[str]]) -> list[dict]:
    next_index = 1
    index_lock = threading.Lock()

    def next_request_index() -> Optional[int]:
        nonlocal next_index
        with index_lock:
            if next_index > args.requests:
                return None
            index = next_index
            next_index += 1
            return index

    def worker_loop(worker_index: int, worker_user: ServiceUser) -> list[dict]:
        worker_results: list[dict] = []
        worker_ack_state: dict[str, dict[str, Any]] = {}
        while True:
            index = next_request_index()
            if index is None:
                return worker_results
            print(
                "NDNSF_DI_NATIVE_TRACER_USER_SUBMIT "
                + json.dumps({
                    "requestIndex": index,
                    "requestCount": args.requests,
                    "concurrency": args.concurrency,
                    "workerIndex": worker_index,
                    "mode": "threaded-service-user",
                }, sort_keys=True),
                flush=True,
            )
            result = run_one_request(
                worker_user,
                args,
                service_plan,
                roles,
                key_scopes,
                dependencies,
                scope_key_data_names,
                role_scopes,
                index,
                observed_ack_runtime=worker_ack_state if worker_ack_state else None)
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
            worker_results.append(result)
            ack_snapshots = result.get("ackCandidateSnapshot", [])
            if ack_snapshots:
                worker_ack_state = observed_ack_runtime_from_snapshots(ack_snapshots)

    results: list[dict] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(users)) as executor:
        futures = [
            executor.submit(worker_loop, worker_index, worker_user)
            for worker_index, worker_user in enumerate(users)
        ]
        for future in concurrent.futures.as_completed(futures):
            results.extend(future.result())
    return sorted(results, key=lambda item: int(item.get("requestIndex", 0)))


def run_threaded_open_loop_requests(users: list[ServiceUser],
                                    args,
                                    service_plan: dict,
                                    roles: list[dict],
                                    key_scopes: dict[str, list[str]],
                                    dependencies: list[dict],
                                    scope_key_data_names: dict[str, str],
                                    role_scopes: dict[str, list[str]]) -> tuple[list[dict], dict]:
    planned = open_loop_planned_requests(args)
    schedule_start = time.perf_counter()
    results: list[dict] = []
    available_workers = list(range(len(users)))
    active: dict[concurrent.futures.Future, int] = {}
    local_backpressure_waits = 0
    max_schedule_slip_ms = 0.0

    def request_on_worker(worker_index: int, request_index: int, schedule_slip_ms: float) -> dict:
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_SUBMIT "
            + json.dumps({
                "mode": "open-loop-threaded-service-user",
                "workerIndex": worker_index + 1,
                "requestIndex": request_index,
                "requestCount": planned,
                "concurrency": args.concurrency,
                "targetRps": args.target_rps,
                "openLoopDurationS": args.open_loop_duration_s,
                "scheduledOffsetMs": round((time.perf_counter() - schedule_start) * 1000.0, 3),
                "scheduleSlipMs": round(schedule_slip_ms, 3),
            }, sort_keys=True),
            flush=True,
        )
        result = run_one_request(
            users[worker_index],
            args,
            service_plan,
            roles,
            key_scopes,
            dependencies,
            scope_key_data_names,
            role_scopes,
            request_index)
        result["requestCount"] = planned
        result["mode"] = "open-loop-threaded-service-user"
        result["workerIndex"] = worker_index + 1
        result["targetRps"] = args.target_rps
        result["openLoopDurationS"] = args.open_loop_duration_s
        return result

    def collect_completed(timeout: float = 0.0) -> None:
        if not active:
            return
        done, _pending = concurrent.futures.wait(
            list(active.keys()),
            timeout=timeout,
            return_when=concurrent.futures.FIRST_COMPLETED)
        for future in done:
            worker_index = active.pop(future)
            available_workers.append(worker_index)
            result = future.result()
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " +
                  json.dumps(result, sort_keys=True), flush=True)
            results.append(result)

    print(
        "NDNSF_DI_NATIVE_TRACER_USER_OPEN_LOOP "
        + json.dumps({
            "mode": "threaded-service-user",
            "requestCount": planned,
            "requestCap": args.requests,
            "concurrency": args.concurrency,
            "targetRps": args.target_rps,
            "openLoopDurationS": args.open_loop_duration_s,
        }, sort_keys=True),
        flush=True,
    )
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(users)) as executor:
        for index in range(1, planned + 1):
            target_time = schedule_start + ((index - 1) / args.target_rps)
            while True:
                collect_completed(timeout=0.0)
                delay = target_time - time.perf_counter()
                if delay <= 0:
                    break
                time.sleep(min(delay, 0.02))
            collect_completed(timeout=0.0)
            while not available_workers:
                local_backpressure_waits += 1
                collect_completed(timeout=0.1)
            schedule_slip_ms = max(0.0, (time.perf_counter() - target_time) * 1000.0)
            max_schedule_slip_ms = max(max_schedule_slip_ms, schedule_slip_ms)
            worker_index = available_workers.pop(0)
            active[executor.submit(
                request_on_worker,
                worker_index,
                index,
                schedule_slip_ms)] = worker_index
        while active:
            collect_completed(timeout=0.1)

    dropped = [
        item for item in results
        if item.get("error") == "local-open-loop-backpressure"
    ]
    metadata = {
        "mode": "open-loop-threaded-service-user",
        "targetRps": args.target_rps,
        "openLoopDurationS": args.open_loop_duration_s,
        "scheduledRequestCount": planned,
        "submittedCount": len(results) - len(dropped),
        "localBackpressureCount": len(dropped),
        "localBackpressureWaitCount": local_backpressure_waits,
        "maxScheduleSlipMs": round(max_schedule_slip_ms, 3),
        "offeredRps": planned / args.open_loop_duration_s if args.open_loop_duration_s > 0 else 0.0,
    }
    return sorted(results, key=lambda item: int(item.get("requestIndex", 0))), metadata


def parse_request_indices(raw: str) -> list[int]:
    indices = [
        int(item.strip())
        for item in raw.split(",")
        if item.strip()
    ]
    return sorted(index for index in indices if index > 0)


def run_worker_request_batch(user: ServiceUser,
                             args,
                             service_plan: dict,
                             roles: list[dict],
                             key_scopes: dict[str, list[str]],
                             dependencies: list[dict],
                             scope_key_data_names: dict[str, str],
                             role_scopes: dict[str, list[str]]) -> list[dict]:
    indices = parse_request_indices(args.worker_request_indices)
    if not indices:
        indices = [args.request_index]
    results: list[dict] = []
    observed_ack: dict[str, dict[str, Any]] = {}
    for index in indices:
        if args.schedule_start_epoch > 0.0 and args.target_rps > 0.0:
            target_time = args.schedule_start_epoch + ((index - 1) / args.target_rps)
            delay = target_time - time.time()
            if delay > 0:
                time.sleep(delay)
        result = run_one_request(
            user,
            args,
            service_plan,
            roles,
            key_scopes,
            dependencies,
            scope_key_data_names,
            role_scopes,
            index,
            observed_ack_runtime=observed_ack if observed_ack else None)
        result["requestCount"] = open_loop_planned_requests(args) if args.open_loop_duration_s > 0 else args.requests
        result["mode"] = (
            "open-loop-process-pool-worker"
            if args.open_loop_duration_s > 0 else
            "child-process-service-user")
        result["targetRps"] = args.target_rps if args.open_loop_duration_s > 0 else 0.0
        result["openLoopDurationS"] = args.open_loop_duration_s
        print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " +
              json.dumps(result, sort_keys=True), flush=True)
        results.append(result)
        ack_snapshots = result.get("ackCandidateSnapshot", [])
        if ack_snapshots:
            observed_ack = observed_ack_runtime_from_snapshots(ack_snapshots)
    print("NDNSF_DI_NATIVE_TRACER_USER_WORKER_BATCH " + json.dumps({
        "mode": "open-loop-process-pool-worker",
        "requestIndices": indices,
        "successCount": sum(1 for item in results if item.get("status") == "executed"),
        "failureCount": sum(1 for item in results if item.get("status") != "executed"),
    }, sort_keys=True), flush=True)
    return results


def run_child_process_requests(args,
                               scope_key_data_names: dict[str, str]) -> list[dict]:
    script = Path(__file__).resolve()
    scope_json = json.dumps(scope_key_data_names, sort_keys=True)
    child_log_dir = (Path(args.plan).resolve().parents[1] / "logs") if args.plan else None
    if child_log_dir is not None:
        child_log_dir.mkdir(parents=True, exist_ok=True)
    admission_providers = [
        item.strip()
        for item in args.burst_admission_providers.split(",")
        if item.strip()
    ]

    def admission_bias_for_index(index: int) -> str:
        if not admission_providers:
            return ""
        counts = {provider: 0 for provider in admission_providers}
        for offset in range(max(0, index - 1)):
            provider = admission_providers[offset % len(admission_providers)]
            counts[provider] += 1
        return ";".join(
            f"{provider}={count}"
            for provider, count in counts.items()
            if count > 0
        )

    def role_provider_preference_for_index(index: int) -> str:
        if not admission_providers:
            return ""
        provider = admission_providers[(index - 1) % len(admission_providers)]
        return f"/Backbone=>{provider};Backbone=>{provider}"

    open_loop = args.open_loop_duration_s > 0.0
    planned = open_loop_planned_requests(args) if open_loop else args.requests
    schedule_start = time.perf_counter()

    def run_child(index: int) -> dict:
        if not open_loop and args.submission_spacing_ms > 0:
            time.sleep(((index - 1) * args.submission_spacing_ms) / 1000.0)
        child_home = Path(tempfile.mkdtemp(prefix=f"ndnsf-di-user-{index}-"))
        parent_ndn_dir = Path(os.environ.get("HOME", "")).expanduser() / ".ndn"
        child_ndn_dir = child_home / ".ndn"
        if parent_ndn_dir.exists():
            shutil.copytree(parent_ndn_dir, child_ndn_dir)
        child_env = os.environ.copy()
        child_env["HOME"] = str(child_home)
        if (child_ndn_dir / "client.conf").exists():
            child_env["NDN_CLIENT_CONF"] = str(child_ndn_dir / "client.conf")
        admission_bias = admission_bias_for_index(index)
        if admission_bias:
            child_env["NDNSF_COLLAB_ADMISSION_BIAS"] = admission_bias
        role_provider_preference = role_provider_preference_for_index(index)
        if role_provider_preference:
            child_env["NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE"] = role_provider_preference

        def cleanup_child_home() -> None:
            try:
                shutil.rmtree(child_home)
            except Exception:
                pass

        command = [
            sys.executable,
            str(script),
            "--plan", args.plan,
            "--service", args.service,
            "--group", args.group,
            "--controller", args.controller,
            "--user", f"{args.user}/worker/{index}",
            "--trust-schema", args.trust_schema,
            "--ack-timeout-ms", str(args.ack_timeout_ms),
            "--timeout-ms", str(args.timeout_ms),
            "--overload-fast-fail-timeout-ms", str(args.overload_fast_fail_timeout_ms),
            "--permission-wait-ms", str(args.permission_wait_ms),
            "--requests", str(args.requests),
            "--concurrency", str(args.concurrency),
            "--worker-child",
            "--request-index", str(index),
            "--scope-key-data-names-json", scope_json,
            "--runtime-aware-max-replans", str(args.runtime_aware_max_replans),
            "--runtime-aware-replan-reasons", args.runtime_aware_replan_reasons,
        ]
        if args.assignment_csv:
            command.extend(["--assignment-csv", args.assignment_csv])
        if args.runtime_hints_json:
            command.extend(["--runtime-hints-json", args.runtime_hints_json])
        if args.fragment_inventory_json:
            command.extend(["--fragment-inventory-json", args.fragment_inventory_json])
        if args.coordination_service:
            command.extend([
                "--coordination-service", args.coordination_service,
                "--coordination-ack-timeout-ms", str(args.coordination_ack_timeout_ms),
                "--coordination-timeout-ms", str(args.coordination_timeout_ms),
            ])
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_SUBMIT "
            + json.dumps({
                "admissionBias": admission_bias,
                "roleProviderPreference": role_provider_preference,
                "requestIndex": index,
                "requestCount": planned,
                "concurrency": args.concurrency,
                "mode": "open-loop-child-process-service-user" if open_loop else "child-process-service-user",
                "targetRps": args.target_rps if open_loop else 0.0,
                "openLoopDurationS": args.open_loop_duration_s if open_loop else 0.0,
            }, sort_keys=True),
            flush=True,
        )
        started = time.perf_counter()
        child_log = child_log_dir / f"user-worker-{index}.log" if child_log_dir is not None else None

        def write_child_log(output: str) -> None:
            if child_log is None:
                return
            child_log.write_text(output, encoding="utf-8", errors="replace")

        try:
            completed = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                env=child_env,
                timeout=(effective_timeout_ms(args) / 1000.0) + 25.0,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            child_output = exc.stdout or ""
            if isinstance(child_output, bytes):
                child_output = child_output.decode("utf-8", errors="replace")
            write_child_log(child_output)
            cleanup_child_home()
            return {
                "status": "failed",
                "service": args.service,
                "requestIndex": index,
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "responseStatus": False,
                "payloadBytes": 0,
                "error": "child process local deadline",
                "elapsedMs": (time.perf_counter() - started) * 1000.0,
                "childOutput": child_output[-4000:],
            }

        child_output = completed.stdout or ""
        write_child_log(child_output)
        for line in child_output.splitlines():
            if line.startswith("NDNSF_DI_NATIVE_TRACER_USER_REQUEST "):
                result = json.loads(line.split(" ", 1)[1])
                result["requestCount"] = planned
                if open_loop:
                    result["mode"] = "open-loop-child-process-service-user"
                    result["targetRps"] = args.target_rps
                    result["openLoopDurationS"] = args.open_loop_duration_s
                result["childReturncode"] = completed.returncode
                if result.get("status") != "executed":
                    result["childOutput"] = child_output[-4000:]
                cleanup_child_home()
                return result
        cleanup_child_home()
        return {
            "status": "failed",
            "service": args.service,
            "requestIndex": index,
            "requestCount": args.requests,
            "concurrency": args.concurrency,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": "child did not emit request result",
            "elapsedMs": (time.perf_counter() - started) * 1000.0,
            "childReturncode": completed.returncode,
            "childOutput": child_output[-2000:],
        }

    results: list[dict] = []
    if open_loop:
        local_backpressure_waits = 0
        max_schedule_slip_ms = 0.0
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_OPEN_LOOP "
            + json.dumps({
                "mode": "child-process-service-user",
                "requestCount": planned,
                "requestCap": args.requests,
                "concurrency": args.concurrency,
                "targetRps": args.target_rps,
                "openLoopDurationS": args.open_loop_duration_s,
            }, sort_keys=True),
            flush=True,
        )
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
            active: dict[concurrent.futures.Future, int] = {}

            def collect_completed(timeout: float = 0.0) -> None:
                if not active:
                    return
                done, _pending = concurrent.futures.wait(
                    list(active.keys()),
                    timeout=timeout,
                    return_when=concurrent.futures.FIRST_COMPLETED)
                for future in done:
                    active.pop(future, None)
                    result = future.result()
                    print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " +
                          json.dumps(result, sort_keys=True), flush=True)
                    results.append(result)

            for index in range(1, planned + 1):
                target_time = schedule_start + ((index - 1) / args.target_rps)
                while True:
                    collect_completed(timeout=0.0)
                    delay = target_time - time.perf_counter()
                    if delay <= 0:
                        break
                    time.sleep(min(delay, 0.05))
                collect_completed(timeout=0.0)
                while len(active) >= args.concurrency:
                    local_backpressure_waits += 1
                    collect_completed(timeout=0.1)
                max_schedule_slip_ms = max(
                    max_schedule_slip_ms,
                    max(0.0, (time.perf_counter() - target_time) * 1000.0))
                active[executor.submit(run_child, index)] = index

            while active:
                collect_completed(timeout=0.1)
        for result in results:
            result.setdefault("localBackpressureWaitCount", local_backpressure_waits)
            result.setdefault("maxScheduleSlipMs", round(max_schedule_slip_ms, 3))
        return sorted(results, key=lambda item: int(item.get("requestIndex", 0)))

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = [
            executor.submit(run_child, index)
            for index in range(1, args.requests + 1)
        ]
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
            results.append(result)
    return sorted(results, key=lambda item: int(item.get("requestIndex", 0)))


def run_process_pool_open_loop_requests(args,
                                        scope_key_data_names: dict[str, str]) -> tuple[list[dict], dict]:
    script = Path(__file__).resolve()
    planned = open_loop_planned_requests(args)
    worker_count = min(args.concurrency, planned)
    schedule_start_epoch = time.time() + 5.0
    scope_json = json.dumps(scope_key_data_names, sort_keys=True)
    child_log_dir = (Path(args.plan).resolve().parents[1] / "logs") if args.plan else None
    if child_log_dir is not None:
        child_log_dir.mkdir(parents=True, exist_ok=True)

    assignments: list[list[int]] = [[] for _ in range(worker_count)]
    for index in range(1, planned + 1):
        assignments[(index - 1) % worker_count].append(index)

    parent_ndn_dir = Path(os.environ.get("HOME", "")).expanduser() / ".ndn"
    children: list[tuple[subprocess.Popen, Path, Path, list[int]]] = []
    print(
        "NDNSF_DI_NATIVE_TRACER_USER_OPEN_LOOP "
        + json.dumps({
            "mode": "process-pool-service-user",
            "requestCount": planned,
            "requestCap": args.requests,
            "concurrency": args.concurrency,
            "workerCount": worker_count,
            "targetRps": args.target_rps,
            "openLoopDurationS": args.open_loop_duration_s,
            "scheduleStartEpoch": schedule_start_epoch,
        }, sort_keys=True),
        flush=True,
    )
    for worker_offset, indices in enumerate(assignments, start=1):
        child_home = Path(tempfile.mkdtemp(prefix=f"ndnsf-di-user-pool-{worker_offset}-"))
        child_ndn_dir = child_home / ".ndn"
        if parent_ndn_dir.exists():
            shutil.copytree(parent_ndn_dir, child_ndn_dir)
        child_env = os.environ.copy()
        child_env["HOME"] = str(child_home)
        if (child_ndn_dir / "client.conf").exists():
            child_env["NDN_CLIENT_CONF"] = str(child_ndn_dir / "client.conf")
        command = [
            sys.executable,
            str(script),
            "--plan", args.plan,
            "--service", args.service,
            "--group", args.group,
            "--controller", args.controller,
            "--user", f"{args.user}/worker/{worker_offset}",
            "--trust-schema", args.trust_schema,
            "--ack-timeout-ms", str(args.ack_timeout_ms),
            "--timeout-ms", str(args.timeout_ms),
            "--overload-fast-fail-timeout-ms", str(args.overload_fast_fail_timeout_ms),
            "--permission-wait-ms", str(args.permission_wait_ms),
            "--requests", str(planned),
            "--concurrency", str(args.concurrency),
            "--target-rps", str(args.target_rps),
            "--open-loop-duration-s", str(args.open_loop_duration_s),
            "--open-loop-driver-mode", "child",
            "--runtime-aware-max-replans", str(args.runtime_aware_max_replans),
            "--runtime-aware-replan-reasons", args.runtime_aware_replan_reasons,
            "--worker-child",
            "--request-index", str(indices[0]),
            "--worker-request-indices", ",".join(str(index) for index in indices),
            "--schedule-start-epoch", str(schedule_start_epoch),
            "--scope-key-data-names-json", scope_json,
        ]
        if args.assignment_csv:
            command.extend(["--assignment-csv", args.assignment_csv])
        if args.runtime_hints_json:
            command.extend(["--runtime-hints-json", args.runtime_hints_json])
        if args.fragment_inventory_json:
            command.extend(["--fragment-inventory-json", args.fragment_inventory_json])
        if args.coordination_service:
            command.extend([
                "--coordination-service", args.coordination_service,
                "--coordination-ack-timeout-ms", str(args.coordination_ack_timeout_ms),
                "--coordination-timeout-ms", str(args.coordination_timeout_ms),
            ])
        child_log = (
            child_log_dir / f"user-worker-pool-{worker_offset}.log"
            if child_log_dir is not None else
            Path(tempfile.mktemp(prefix=f"ndnsf-di-user-pool-{worker_offset}-", suffix=".log"))
        )
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_SUBMIT "
            + json.dumps({
                "mode": "open-loop-process-pool-service-user",
                "workerIndex": worker_offset,
                "requestIndices": indices,
                "requestCount": planned,
                "concurrency": args.concurrency,
                "targetRps": args.target_rps,
                "openLoopDurationS": args.open_loop_duration_s,
            }, sort_keys=True),
            flush=True,
        )
        output = child_log.open("w", encoding="utf-8", errors="replace")
        output.write("RUN " + " ".join(command) + "\n")
        output.flush()
        proc = subprocess.Popen(
            command,
            text=True,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=child_env,
        )
        output.close()
        children.append((proc, child_log, child_home, indices))

    results_by_index: dict[int, dict] = {}
    deadline = time.time() + args.open_loop_duration_s + (effective_timeout_ms(args) / 1000.0) + 35.0
    for proc, child_log, child_home, indices in children:
        timeout = max(1.0, deadline - time.time())
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        child_output = child_log.read_text(encoding="utf-8", errors="replace")
        for line in child_output.splitlines():
            if line.startswith("NDNSF_DI_NATIVE_TRACER_USER_REQUEST "):
                result = json.loads(line.split(" ", 1)[1])
                result["mode"] = "open-loop-process-pool-service-user"
                result["workerReturncode"] = proc.returncode
                results_by_index[int(result.get("requestIndex", 0))] = result
        for index in indices:
            if index not in results_by_index:
                results_by_index[index] = {
                    "status": "failed",
                    "service": args.service,
                    "requestIndex": index,
                    "requestCount": planned,
                    "concurrency": args.concurrency,
                    "mode": "open-loop-process-pool-service-user",
                    "targetRps": args.target_rps,
                    "openLoopDurationS": args.open_loop_duration_s,
                    "responseStatus": False,
                    "payloadBytes": 0,
                    "error": "process-pool worker did not emit request result",
                    "elapsedMs": 0.0,
                    "workerReturncode": proc.returncode,
                    "workerLog": str(child_log),
                }
        try:
            shutil.rmtree(child_home)
        except Exception:
            pass

    results = [results_by_index[index] for index in sorted(results_by_index)]
    dropped = [
        item for item in results
        if item.get("error") == "local-open-loop-backpressure"
    ]
    metadata = {
        "mode": "open-loop-process-pool-service-user",
        "targetRps": args.target_rps,
        "openLoopDurationS": args.open_loop_duration_s,
        "scheduledRequestCount": planned,
        "submittedCount": len(results) - len(dropped),
        "localBackpressureCount": len(dropped),
        "offeredRps": planned / args.open_loop_duration_s if args.open_loop_duration_s > 0 else 0.0,
    }
    return results, metadata


def main() -> int:
    args = build_parser().parse_args()
    if args.requests <= 0:
        raise SystemExit("--requests must be positive")
    if args.concurrency <= 0:
        raise SystemExit("--concurrency must be positive")
    if args.concurrency > args.requests:
        args.concurrency = args.requests
    if args.target_rps < 0.0:
        raise SystemExit("--target-rps must be non-negative")
    if args.open_loop_duration_s < 0.0:
        raise SystemExit("--open-loop-duration-s must be non-negative")
    if args.overload_fast_fail_timeout_ms < 0:
        raise SystemExit("--overload-fast-fail-timeout-ms must be non-negative")
    open_loop = args.target_rps > 0.0 or args.open_loop_duration_s > 0.0
    if open_loop and (args.target_rps <= 0.0 or args.open_loop_duration_s <= 0.0):
        raise SystemExit("--target-rps and --open-loop-duration-s must be set together")
    if args.plan:
        service_plan = load_service_plan(Path(args.plan), args.service)
    elif args.dry_run:
        service_plan = sample_service_plan(args.service)
    else:
        raise SystemExit("--plan is required unless --dry-run is used")
    roles = collaboration_roles(service_plan, args.service)
    dependencies = collaboration_dependencies(service_plan)
    key_scopes, role_scopes = key_scopes_and_role_scopes(service_plan)
    if args.dry_run:
        payload = {
            "service": args.service,
            "roles": roles,
            "dependencies": dependencies,
            "keyScopes": key_scopes,
            "roleScopes": role_scopes,
        }
        payload.update(runtime_replan_metadata(args))
        payload.update(coordination_metadata(args))
        payload.update(overload_fast_fail_metadata(args))
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0

    user = ServiceUser(
        group=args.group,
        controller=args.controller,
        user=args.user,
        trust_schema=args.trust_schema,
        permission_wait_ms=args.permission_wait_ms,
        serve_certificates=True,
    )
    allowed = [entry.service for entry in user.get_allowed_services()]
    print("NDNSF_DI_NATIVE_TRACER_USER_ALLOWED " + json.dumps(allowed), flush=True)
    if args.service not in allowed:
        result = {
            "status": "failed",
            "service": args.service,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": f"missing user permission for {args.service}; allowed={allowed}",
            "elapsedMs": 0.0,
        }
        print("NDNSF_DI_NATIVE_TRACER_USER_EXECUTION " + json.dumps(result, sort_keys=True), flush=True)
        return 1
    if args.coordination_service and args.coordination_service not in allowed:
        result = {
            "status": "failed",
            "service": args.service,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": (
                f"missing user permission for {args.coordination_service}; "
                f"allowed={allowed}"
            ),
            "elapsedMs": 0.0,
        }
        print("NDNSF_DI_NATIVE_TRACER_USER_EXECUTION " + json.dumps(result, sort_keys=True), flush=True)
        return 1
    if args.worker_child:
        if not args.scope_key_data_names_json:
            raise SystemExit("--scope-key-data-names-json is required for worker children")
        scope_key_data_names = json.loads(args.scope_key_data_names_json)
        if args.worker_request_indices:
            results = run_worker_request_batch(
                user,
                args,
                service_plan,
                roles,
                key_scopes,
                dependencies,
                scope_key_data_names,
                role_scopes)
            return 0 if all(item["status"] == "executed" for item in results) else 1
        result = run_one_request(
            user,
            args,
            service_plan,
            roles,
            key_scopes,
            dependencies,
            scope_key_data_names,
            role_scopes,
            args.request_index)
        print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
        return 0 if result["status"] == "executed" else 1

    scope_key_data_names = publish_scope_keys(user, args.service, key_scopes)
    print(
        "NDNSF_DI_NATIVE_TRACER_SCOPE_KEYS "
        + json.dumps(scope_key_data_names, sort_keys=True),
        flush=True,
    )
    if args.wait_for_deployment:
        dep = user.wait_deployment(args.wait_for_deployment, timeout_ms=30000)
        if dep and dep.get("status") == "ACTIVE":
            print("NDNSF_DI_NATIVE_TRACER_WAIT_DEPLOYMENT " + json.dumps({
                "deploymentId": args.wait_for_deployment,
                "status": dep.get("status"),
                "fragmentMap": dep.get("fragmentMap", {}),
            }, sort_keys=True), flush=True)
        else:
            print("NDNSF_DI_NATIVE_TRACER_WAIT_DEPLOYMENT_TIMEOUT " + json.dumps({
                "deploymentId": args.wait_for_deployment,
            }, sort_keys=True), flush=True)
    workload_start = time.perf_counter()
    results = []
    base_workload_metadata = {
        **runtime_replan_metadata(args),
        **coordination_metadata(args),
        **overload_fast_fail_metadata(args),
    }
    workload_metadata = dict(base_workload_metadata)
    if open_loop:
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_CONCURRENCY "
            + json.dumps({
                "mode": (
                    "open-loop-threaded-service-user"
                    if args.open_loop_driver_mode == "threaded" else
                    "open-loop-process-pool-service-user"
                    if args.open_loop_driver_mode == "process-pool" else
                    "open-loop-child-process-service-user"),
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "targetRps": args.target_rps,
                "openLoopDurationS": args.open_loop_duration_s,
            }, sort_keys=True),
            flush=True,
        )
        if args.open_loop_driver_mode == "threaded":
            worker_users = [
                ServiceUser(
                    group=args.group,
                    controller=args.controller,
                    user=f"{args.user}/worker/{index}",
                    trust_schema=args.trust_schema,
                    permission_wait_ms=args.permission_wait_ms,
                    serve_certificates=True,
                )
                for index in range(1, args.concurrency + 1)
            ]
            for worker_user in worker_users:
                allowed_worker = [entry.service for entry in worker_user.get_allowed_services()]
                if args.service not in allowed_worker:
                    raise RuntimeError(
                        f"missing worker permission for {args.service}; "
                        f"user={worker_user.user}; allowed={allowed_worker}")
            results, workload_metadata = run_threaded_open_loop_requests(
                worker_users,
                args,
                service_plan,
                roles,
                key_scopes,
                dependencies,
                scope_key_data_names,
                role_scopes)
        elif args.open_loop_driver_mode == "process-pool":
            user.start()
            try:
                results, workload_metadata = run_process_pool_open_loop_requests(
                    args, scope_key_data_names)
            finally:
                user.stop()
        else:
            user.start()
            try:
                results = run_child_process_requests(args, scope_key_data_names)
                dropped = [
                    item for item in results
                    if item.get("error") == "local-open-loop-backpressure"
                ]
                workload_metadata = {
                    "mode": "open-loop-child-process-service-user",
                    "targetRps": args.target_rps,
                    "openLoopDurationS": args.open_loop_duration_s,
                    "scheduledRequestCount": len(results),
                    "submittedCount": len(results) - len(dropped),
                    "localBackpressureCount": len(dropped),
                    "localBackpressureWaitCount": max(
                        int(item.get("localBackpressureWaitCount", 0) or 0)
                        for item in results
                    ) if results else 0,
                    "maxScheduleSlipMs": max(
                        float(item.get("maxScheduleSlipMs", 0.0) or 0.0)
                        for item in results
                    ) if results else 0.0,
                    "offeredRps": (
                        len(results) / args.open_loop_duration_s
                        if args.open_loop_duration_s > 0 else 0.0),
                }
            finally:
                user.stop()
    elif args.concurrency == 1:
        rate_limiter = TokenBucket(args.max_rps, burst=max(1, int(args.max_rps * 2))) if args.max_rps > 0 else None
        retry_policy = RetryPolicy(max_attempts=args.retry_max_attempts) if args.retry_max_attempts > 0 else None
        metrics = NdnMetrics()
        trace_collector = TraceCollector("ndnsf-di-user")
        observed_ack: dict[str, dict[str, Any]] = {}
        for index in range(1, args.requests + 1):
            if rate_limiter is not None and not rate_limiter.consume():
                metrics.rate_limited_total.labels(service=args.service).inc()
                result = {"status": "failed", "service": args.service,
                          "requestIndex": index, "error": "rate-limited",
                          "elapsedMs": 0.0, "requestCount": args.requests,
                          "concurrency": args.concurrency,
                          "responseStatus": False, "payloadBytes": 0}
                results.append(result)
                print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
                continue
            if retry_policy is not None:
                retry_policy.reset()
                def _do_request() -> dict:
                    return run_one_request(
                        user, args, service_plan, roles, key_scopes, dependencies,
                        scope_key_data_names, role_scopes, index,
                        observed_ack_runtime=observed_ack if observed_ack else None)
                result = retry_call(_do_request, retry_policy)
                if result.get("retryAttempts", 0) > 0:
                    metrics.retry_total.labels(service=args.service).inc(result.get("retryAttempts", 0))
            else:
                result = run_one_request(
                user,
                args,
                service_plan,
                roles,
                key_scopes,
                dependencies,
                scope_key_data_names,
                role_scopes,
                index,
                observed_ack_runtime=observed_ack if observed_ack else None)
            results.append(result)
            print("NDNSF_DI_NATIVE_TRACER_USER_REQUEST " + json.dumps(result, sort_keys=True), flush=True)
            ack_snapshots = result.get("ackCandidateSnapshot", [])
            if ack_snapshots:
                observed_ack = observed_ack_runtime_from_snapshots(ack_snapshots)
            if result["status"] != "executed":
                break
    else:
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_CONCURRENCY "
            + json.dumps({
                "mode": "child-process-service-user",
                "requestCount": args.requests,
                "concurrency": args.concurrency,
                "workers": args.concurrency,
            }, sort_keys=True),
            flush=True,
        )
        user.start()
        try:
            results = run_child_process_requests(args, scope_key_data_names)
        finally:
            user.stop()

    makespan_ms = (time.perf_counter() - workload_start) * 1000.0
    workload_metadata = {
        **base_workload_metadata,
        **workload_metadata,
    }
    workload = summarize_workload(results, makespan_ms, args.service, args.concurrency, workload_metadata)
    print("NDNSF_DI_NATIVE_TRACER_USER_WORKLOAD " + json.dumps(workload, sort_keys=True), flush=True)
    print("NDNSF_DI_NATIVE_TRACER_USER_EXECUTION " + json.dumps(workload, sort_keys=True), flush=True)
    return 0 if workload["status"] == "executed" else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Submit a real NDNSF collaboration request for /Inference/NativeTracer."""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import csv
import hashlib
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
from types import MappingProxyType
from typing import Any, Callable, Mapping, Optional

from ndnsf import (
    AckCompatibilityCounters,
    NdnMetrics,
    ProviderNetworkMatrix,
    ServiceUser,
    TokenBucket,
    TraceCollector,
    build_network_matrix_from_ndnsd,
    decode_provider_capability_ack,
)
from ndnsf_distributed_inference.retry import RetryPolicy, RetryReason, retry_call
from ndnsf_distributed_inference.runtime_v1 import (
    PLACEMENT_STRATEGY_PRESETS,
    filter_feasible_providers,
    pick_optimal_placement,
)
from ndnsf_distributed_inference.deployment import (
    LEASE_SERVICE_NAME,
    DistributedLeaseTransaction,
    NdnsfLeaseTransport,
    ProviderLeaseAssignment,
    wait_deployment,
)
from ndnsf_distributed_inference.core import (
    AssignmentContext,
    ExecutionActivateMessage,
    ProviderAssignment,
    ReadySetMember,
    canonical_digest,
)
from ndnsf_distributed_inference.core.group_capability import (
    GroupMemberV1,
    GroupOperationV1,
    seal_group_capability_v1,
)
from ndnsf_distributed_inference.sdk.placement import (
    DeviceBinding,
    DeviceBindingMode,
    ExecutionRole,
    ProviderSelectionProjectionV3,
    RoleAssemblySpec,
    RoleDataflowContract,
)


SERVICE = "/Inference/NativeTracer"
GROUP = "/NDNSF-DI/Tracer/group"
CONTROLLER = "/NDNSF-DI/Tracer/controller"
USER = "/NDNSF-DI/Tracer/user"
ACK_COMPATIBILITY_COUNTERS = AckCompatibilityCounters()
_DATA_V1_MAX_BYTES = 64 << 20
_DATA_V1_MAX_SEGMENTS = 1 << 20
_USER_STAGE_DEBUG_PREFIX = "NDNSF_DI_NATIVE_TRACER_USER_STAGE"


def debug_user_stage(stage: str, **fields: Any) -> None:
    """Emit bounded request-stage diagnostics only when explicitly enabled."""

    if os.environ.get("NDNSF_DI_TRACE_USER_STAGES", "") != "1":
        return
    payload = {"stage": str(stage), **fields}
    print(
        _USER_STAGE_DEBUG_PREFIX + " " + json.dumps(payload, sort_keys=True),
        flush=True,
    )


def wire_request_id(raw: str, *, default: str) -> str:
    """Return a request ID occupying exactly one NDN name component."""

    value = str(raw or default).strip()
    if value.startswith("/"):
        value = value[1:]
    if not value or "/" in value:
        raise ValueError(
            "request ID must be exactly one NDN name component; "
            f"got {raw!r}"
        )
    return "/" + value


def json_compatible(value: Any) -> Any:
    """Return a deterministic JSON-safe copy of runtime metadata."""

    if isinstance(value, Mapping):
        return {
            str(key): json_compatible(item)
            for key, item in value.items()
        }
    if isinstance(value, (list, tuple)):
        return [json_compatible(item) for item in value]
    if isinstance(value, (set, frozenset)):
        return [json_compatible(item) for item in sorted(value, key=repr)]
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, bytes):
        return base64.b64encode(value).decode("ascii")
    return value


class StaticV3SelectionCommit:
    """ACK-bound commit inputs for one static native execution plan."""

    __slots__ = (
        "plan_digest",
        "provider_by_role",
        "assignment_payloads_by_role",
        "ack_snapshots",
    )

    def __init__(
        self,
        *,
        plan_digest: str,
        provider_by_role: Mapping[str, str],
        assignment_payloads_by_role: Mapping[str, bytes],
        ack_snapshots: tuple[dict[str, Any], ...],
    ) -> None:
        self.plan_digest = plan_digest
        self.provider_by_role = provider_by_role
        self.assignment_payloads_by_role = assignment_payloads_by_role
        self.ack_snapshots = ack_snapshots


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


def summarize_tensor_bundle(payload: bytes, *, max_values: int = 1024) -> dict[str, Any]:
    """Decode bounded NDNSF-DI tensor metadata for correctness evidence."""

    wire = bytes(payload)
    if not wire.startswith(b"NDITB001"):
        raise ValueError("response payload is not an NDNSF-DI tensor bundle")
    if max_values < 0:
        raise ValueError("max_values must be non-negative")
    offset = 8

    def take(size: int) -> bytes:
        nonlocal offset
        if size < 0 or offset + size > len(wire):
            raise ValueError("truncated NDNSF-DI tensor bundle")
        value = wire[offset:offset + size]
        offset += size
        return value

    def scalar(fmt: str) -> Any:
        return struct.unpack(fmt, take(struct.calcsize(fmt)))[0]

    tensor_count = int(scalar("<I"))
    if tensor_count > 1024:
        raise ValueError("tensor bundle count exceeds evidence limit")
    tensors: list[dict[str, Any]] = []
    for _ in range(tensor_count):
        name_size = int(scalar("<I"))
        if name_size <= 0 or name_size > 4096:
            raise ValueError("tensor name size is invalid")
        name = take(name_size).decode("utf-8")
        element_type = int(scalar("<I"))
        rank = int(scalar("<I"))
        if rank > 16:
            raise ValueError("tensor rank exceeds evidence limit")
        shape = [int(scalar("<q")) for _ in range(rank)]
        if any(dim < 0 for dim in shape):
            raise ValueError("tensor shape contains a negative dimension")
        data_size = int(scalar("<Q"))
        if data_size > (64 << 20):
            raise ValueError("tensor payload exceeds evidence limit")
        data = take(data_size)
        tensor: dict[str, Any] = {
            "name": name,
            "elementType": element_type,
            "shape": shape,
            "payloadBytes": data_size,
            "payloadSha256": "sha256:" + hashlib.sha256(data).hexdigest(),
        }
        if element_type == 1:
            element_count = math.prod(shape) if shape else 1
            if data_size != element_count * 4:
                raise ValueError("Float32 tensor payload size does not match shape")
            visible_count = min(element_count, max_values)
            tensor["values"] = list(struct.unpack(
                "<" + "f" * visible_count, data[:visible_count * 4]))
            tensor["valuesTruncated"] = visible_count != element_count
        tensors.append(tensor)
    if offset != len(wire):
        raise ValueError("tensor bundle has trailing bytes")
    return {
        "encoded": True,
        "payloadBytes": len(wire),
        "payloadSha256": "sha256:" + hashlib.sha256(wire).hexdigest(),
        "tensorCount": tensor_count,
        "tensors": tensors,
    }


def load_service_plan(path: Path, service: str) -> dict:
    plan = json.loads(path.read_text(encoding="utf-8"))
    return next(item for item in plan["services"] if item["service"] == service)


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


def assignment_context_from_preference(preference: str, *, request_id: str,
                                       deadline_ms: int) -> AssignmentContext | None:
    pairs = []
    for item in preference.split(";"):
        if "=>" in item:
            role, provider = item.split("=>", 1)
            if role and provider:
                pairs.append((role, provider))
    if not pairs:
        return None
    return AssignmentContext(
        request_id, 1, canonical_digest({"roleProviders": pairs}),
        "native-tracer-exact", tuple(pairs), deadline_ms)


def static_provider_map_from_assignment_csv(
        path: str, roles: list[dict]) -> dict[str, str]:
    """Load one unambiguous Provider assignment for every static-plan role."""
    role_names = [str(role.get("role", "")).strip() for role in roles]
    candidates_by_role = load_role_assignment_candidates(path)
    providers: dict[str, str] = {}
    ambiguous: list[str] = []
    for role in role_names:
        candidates = candidates_by_role.get(role, ())
        candidates = tuple(dict.fromkeys(
            str(item.get("provider", "")).strip()
            for item in candidates
            if str(item.get("provider", "")).strip()
        ))
        if len(candidates) == 1:
            providers[role] = candidates[0]
        elif len(candidates) > 1:
            ambiguous.append(role)
    missing = [role for role in role_names if role and role not in providers]
    if ambiguous:
        raise RuntimeError(
            "NDNSF_DATA_V1 static V3 selection has ambiguous Providers for "
            "roles: " + ",".join(ambiguous))
    if missing:
        raise RuntimeError(
            "NDNSF_DATA_V1 static V3 selection requires an explicit Provider "
            "for every role; missing: " + ",".join(missing))
    return {role: providers[role] for role in role_names if role}


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
    role_names = list(service_plan["roles"])
    execution_policy = str(
        service_plan.get("executionPolicy", "DATA_DRIVEN_V2")).strip()
    if execution_policy not in {"DATA_DRIVEN_V2", "LEGACY_READY_SET_V1"}:
        raise ValueError(
            f"unsupported execution policy in native plan: {execution_policy}")
    binding = canonical_digest({
        "service": service,
        "roles": role_names,
        "dependencies": service_plan.get("dependencies", []),
    })
    common = (
        f"executionPolicy={execution_policy};"
        f"readinessRoleCount={len(role_names)};"
        f"readinessRoles={','.join(role_names)};"
        f"readinessBindingDigest={binding};"
    ).encode()
    return [
        {
            "role": role,
            "service": service,
            "min_providers": 1,
            "max_providers": 1,
            "app_requirement": common,
        }
        for role in role_names
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
    readiness_scope = "ndnsf-di-readiness-v1"
    all_roles = list(service_plan["roles"])
    key_scopes: dict[str, list[str]] = {readiness_scope: all_roles}
    role_scopes: dict[str, list[str]] = {
        role: [readiness_scope] for role in all_roles
    }
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


def run_with_started_user(user: ServiceUser, workload):
    user.start()
    try:
        return workload()
    finally:
        user.stop()


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
    measurement_elapsed_ms = makespan_ms
    if metadata is not None:
        candidate = float(metadata.get("measurementElapsedMs", 0.0) or 0.0)
        if candidate > 0.0:
            measurement_elapsed_ms = candidate
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
        "throughputRps": (
            len(successes) / (measurement_elapsed_ms / 1000.0)
            if measurement_elapsed_ms > 0 else 0.0),
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
    parser.add_argument("--bootstrap-token", default="",
                        help="Controller bootstrap token for this user identity")
    parser.add_argument("--ack-timeout-ms", type=int, default=1200)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--requests", type=int, default=1,
                        help="Number of closed-loop collaboration requests to submit")
    parser.add_argument(
        "--fixed-request-id",
        default="",
        help=("Use one explicit request name for every request; intended for "
              "replay/duplicate negative tests"),
    )
    parser.add_argument("--concurrency", type=int, default=1,
                        help="Maximum outstanding collaboration requests")
    parser.add_argument("--submission-spacing-ms", type=int, default=0,
                        help="Delay between child request submissions in concurrent mode")
    parser.add_argument("--target-rps", type=float, default=0.0,
                        help="Open-loop offered request rate; requires --open-loop-duration-s")
    parser.add_argument("--open-loop-duration-s", type=float, default=0.0,
                        help="Submit requests on a fixed schedule for this many seconds")
    parser.add_argument("--open-loop-driver-mode",
                        choices=["child", "threaded"],
                        default="threaded",
                        help="Open-loop user driver implementation")
    parser.add_argument("--burst-admission-providers", default="",
                        help=("Comma-separated provider names used to seed "
                              "per-child burst admission bias"))
    parser.add_argument("--runtime-aware-max-replans", type=int, default=0,
                        help="Bounded runtime-aware planner replan attempt budget")
    parser.add_argument("--runtime-aware-replan-reasons", default="",
                        help="Comma-separated diagnostic reasons to record in replan metrics")
    parser.add_argument("--execution-leases", action="store_true",
                        help="Acquire fail-closed provider execution leases before collaboration")
    parser.add_argument("--execution-cancellation-gate", action="store_true",
                        help="Run one focused post-certificate cancellation fault gate")
    parser.add_argument(
        "--data-v1-no-progress-ms", type=int, default=2000,
        help=("NDNSF_DATA_V1 group no-progress watchdog; keep the default for "
              "normal runs and increase explicitly when an injected stage "
              "delay is part of the fault scenario"),
    )
    parser.add_argument("--cancellation-delay-ms", type=int, default=0,
                        help="Delay after dispatch before CANCEL (0 = ACK window plus 1s)")
    parser.add_argument("--assignment-csv", default="",
                        help="Role/provider candidate CSV used for local lease acquisition")
    parser.add_argument("--role-provider-preference", default="",
                        help=argparse.SUPPRESS)
    parser.add_argument("--max-rps", type=float, default=0.0,
                        help="Per-user token-bucket rate limit (0 = unlimited)")
    parser.add_argument("--retry-max-attempts", type=int, default=0,
                        help="Max retry attempts per request (0 = no retry)")
    parser.add_argument("--wait-for-deployment", default="",
                        help="Wait for this deployment_id to become ACTIVE before starting requests")
    parser.add_argument("--lease-timeout-ms", type=int, default=5000)
    parser.add_argument("--overload-fast-fail-timeout-ms", type=int, default=0,
                        help=("Use this shorter collaboration timeout for overload "
                              "experiments and record fast-fail diagnostics"))
    parser.add_argument("--worker-child", action="store_true",
                        help=argparse.SUPPRESS)
    parser.add_argument("--request-index", type=int, default=1,
                        help=argparse.SUPPRESS)
    parser.add_argument("--scope-key-data-names-json", default="",
                        help=argparse.SUPPRESS)
    parser.add_argument("--dry-run", action="store_true")
    return parser


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


def execution_lease_plan_digest(plan_path: str) -> str:
    """Use the exact native-plan file identity advertised by Providers."""
    return "sha256:" + hashlib.sha256(Path(plan_path).read_bytes()).hexdigest().upper()


def execution_activation_fields(
    *,
    requester: str,
    transaction_id: str,
    plan_digest: str,
    lease_set,
) -> tuple[dict[tuple[str, str], bytes], ExecutionActivateMessage]:
    """Build the exact requester activation binding for all selected roles."""
    attempt_epoch = 1
    ready_members: list[ReadySetMember] = []
    member_text: dict[tuple[str, str], str] = {}
    for lease in lease_set.leases:
        proof_digest = "sha256:" + hashlib.sha256(
            lease.assignment.resource_binding_proof).hexdigest()
        if (not lease.commit_data_name or not lease.commit_signer_certificate
                or not lease.commit_wire_digest.startswith("sha256:")):
            raise RuntimeError(
                "commit receipt lacks authenticated Data evidence for "
                + lease.assignment.provider)
        for role in lease.assignment.roles:
            assignment = ProviderAssignment(
                role=role,
                provider=lease.assignment.provider,
                provider_boot_epoch=lease.provider_epoch,
                lease_id=lease.lease_id,
                resource_binding_digest=proof_digest,
            )
            ready_members.append(ReadySetMember(
                lease.assignment.provider, role, lease.provider_epoch,
                lease.commit_wire_digest))
            # Assignment payload shares the 8,800-byte NDN packet ceiling with
            # the native plan. The activation digest covers the exact READY
            # membership tuple; its envelope uses a 128-bit deterministic
            # member identifier to keep Selection wire size bounded.
            member_text[(lease.assignment.provider, role)] = canonical_digest(
                assignment.membership_key())[7:39]
    activation = ExecutionActivateMessage(
        requester, transaction_id, attempt_epoch,
        canonical_digest({"selection": transaction_id}), plan_digest,
        tuple(ready_members),
        min(lease.expires_at_ms for lease in lease_set.leases),
        1, "requester-authorized-native-tracer")
    members = ",".join(sorted(member_text.values()))
    common = (
        "executionPolicy=LEGACY_READY_SET_V1;"
        "executionActivationRequired=true;"
        f"executionActivationSchema={ExecutionActivateMessage.SCHEMA};"
        f"executionActivationDigest={activation.digest()};"
        f"executionActivationMembers={members};"
    )
    return ({
        key: (common + f"executionActivationLocalMember={value};").encode()
        for key, value in member_text.items()
    }, activation)


def execution_lease_provider_map(args, roles: list[dict]) -> dict[str, str]:
    role_names = [str(role.get("role", "")).strip() for role in roles]
    providers: dict[str, str] = {}
    for role, candidates in load_role_assignment_candidates(args.assignment_csv).items():
        if candidates:
            provider = str(candidates[0].get("provider", "")).strip()
            if provider:
                providers[role] = provider
    missing = [role for role in role_names if role and role not in providers]
    if missing:
        raise RuntimeError(
            "execution lease requires an explicit provider for every role: "
            + ",".join(missing)
        )
    return {role: providers[role] for role in role_names if role}


def acquire_execution_leases(user: ServiceUser, args, service_plan: dict,
                             roles: list[dict], index: int,
                             request_id: str | None = None):
    provider_by_role = execution_lease_provider_map(args, roles)
    transaction_id = request_id or wire_request_id(
        "", default=f"native-tracer-{index}")
    del service_plan
    plan_digest = execution_lease_plan_digest(args.plan)
    roles_by_provider: dict[str, list[str]] = {}
    for role, provider in provider_by_role.items():
        roles_by_provider.setdefault(provider, []).append(role)
    assignments = []
    proofs: dict[str, bytes] = {}
    for provider, provider_roles in sorted(roles_by_provider.items()):
        proof_payload = json.dumps(
            {
                "schema": "ndnsf-di-binding-v1",
                "transactionId": transaction_id,
                "planDigest": plan_digest,
                "provider": provider,
                "roles": sorted(provider_roles),
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
        proof = hashlib.sha256(proof_payload).hexdigest().encode()
        proofs[provider] = proof
        assignments.append(
            ProviderLeaseAssignment(
                provider=provider,
                roles=tuple(sorted(provider_roles)),
                resource_binding_proof=proof,
            )
        )
    transaction = DistributedLeaseTransaction(
        NdnsfLeaseTransport(user, timeout_ms=args.lease_timeout_ms)
    )
    reservation_window_ms = (
        max(int(args.ack_timeout_ms), int(args.lease_timeout_ms)) + 2000
    )
    capacity_wait_ms = reservation_window_ms
    if args.overload_fast_fail_timeout_ms > 0:
        capacity_wait_ms = min(
            capacity_wait_ms, int(args.overload_fast_fail_timeout_ms)
        )
    lease_set = transaction.acquire(
        request_id=transaction_id,
        plan_digest=plan_digest,
        service_name=args.service,
        assignments=assignments,
        # This deadline covers prepare/commit through provider activation only.
        # NativeProviderHandler installs the longer execution hard deadline
        # atomically when model work starts.
        expires_at_ms=int(time.time() * 1000) + reservation_window_ms,
        capacity_wait_ms=capacity_wait_ms,
        reservation_ttl_ms=reservation_window_ms,
    )
    leases_by_provider = {
        lease.assignment.provider: lease for lease in lease_set.leases
    }
    activation_fields, activation = execution_activation_fields(
        requester=args.user,
        transaction_id=transaction_id,
        plan_digest=plan_digest,
        lease_set=lease_set,
    )
    leased_roles = []
    for role in roles:
        role_copy = dict(role)
        provider = provider_by_role[str(role_copy["role"])]
        lease = leases_by_provider[provider]
        fields = (
            f"executionRequestId={transaction_id};"
            "executionAttemptEpoch=1;"
            f"executionProviderBootId={lease.provider_epoch};"
            f"executionLeaseId={lease.lease_id};"
            f"executionLeaseEpoch={lease.provider_epoch};"
            f"executionLeasePlanDigest={plan_digest};"
            f"executionLeaseBindingProof={proofs[provider].decode()};"
            f"executionLeaseProviderRoleCount={len(lease.assignment.roles)};"
        ).encode() + activation_fields[(provider, str(role_copy["role"]))]
        existing = bytes(role_copy.get("app_requirement", b""))
        if existing and not existing.endswith(b";"):
            existing += b";"
        role_copy["app_requirement"] = existing + fields
        leased_roles.append(role_copy)
    preference = ";".join(
        f"{role}=>{provider}" for role, provider in sorted(provider_by_role.items())
    ) + ";"
    return transaction, lease_set, leased_roles, preference, activation


def execution_bindings_from_roles(
    roles: list[dict],
) -> dict[str, dict[str, str]]:
    """Extract lease/activation fields for the V3 per-role projection.

    The legacy request path appended these fields to ``app_requirement``.
    DATA_V1 selections must carry them in the canonical Selection payload so
    the Provider receives one self-contained, request-scoped assignment.
    """

    source_to_wire = {
        "executionProviderBootId": "provider_boot_id",
        "executionLeaseId": "lease_id",
        "executionLeaseEpoch": "lease_epoch",
        "executionLeasePlanDigest": "lease_plan_digest",
        "executionLeaseBindingProof": "lease_binding_proof",
        "executionLeaseProviderRoleCount": "lease_provider_role_count",
        "executionActivationDigest": "activation_digest",
        "executionActivationMembers": "activation_members",
        "executionActivationLocalMember": "activation_local_member",
    }
    bindings: dict[str, dict[str, str]] = {}
    for role in roles:
        role_name = str(role.get("role", "")).strip()
        if not role_name:
            raise ValueError("execution lease role is missing a name")
        fields = parse_semicolon_fields(role.get("app_requirement", b""))
        binding = {
            wire_key: fields[source_key]
            for source_key, wire_key in source_to_wire.items()
            if fields.get(source_key, "")
        }
        missing = [
            wire_key for wire_key in source_to_wire.values()
            if wire_key not in binding
        ]
        if missing:
            raise RuntimeError(
                f"execution lease binding is incomplete for {role_name}: "
                + ",".join(missing)
            )
        bindings[role_name] = binding
    return bindings


def int_field(fields: dict[str, Any], key: str, default: int = 0) -> int:
    try:
        return int(float(fields.get(key, default)))
    except (TypeError, ValueError):
        return default


def ack_candidates_snapshot(candidates) -> list[dict[str, Any]]:
    snapshot = []
    for candidate in candidates:
        payload = bytes(candidate.payload)
        decoded = decode_provider_capability_ack(
            payload,
            counters=ACK_COMPATIBILITY_COUNTERS,
            provider_name=str(candidate.provider_name),
            service_name=str(candidate.service_name),
        )
        hint = decoded.hint
        fields = dict(hint.service_payload)
        runtime_hint = hint.runtime_hint
        if runtime_hint is not None:
            fields["queue"] = runtime_hint.queue_length
            fields["activeWorkers"] = runtime_hint.active_work_count
        fields.setdefault("runtimeStatus", "ready" if hint.ready else "unavailable")
        fields.setdefault("negativeAckReason", hint.reason_code)
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
            "executionEvidence": fields.get("executionEvidence", {}),
            "ackEnvelopeSource": decoded.source,
            "ackCompatibilityCounters": decoded.counters,
            "telemetry": candidate.telemetry,
        })
    return snapshot


def _native_group_epoch_key_wrapper(
    recipient_public_key: bytes,
    epoch_key: bytes,
) -> bytes:
    """Wrap one DATA_V1 epoch key with the NDNSF Core RSA-OAEP primitive."""

    try:
        from ndnsf import _ndnsf
        wrapper = _ndnsf.wrap_selection_gated_input_key
    except (ImportError, AttributeError) as exc:
        raise RuntimeError(
            "NDNSF native binding lacks wrap_selection_gated_input_key") from exc
    return bytes(wrapper(bytes(epoch_key), bytes(recipient_public_key)))


def _v3_dependency_contracts(service_plan: Mapping[str, Any]) -> tuple[dict, ...]:
    """Project the native-plan spelling into the canonical V3 JSON spelling."""

    contracts: list[dict[str, Any]] = []
    for index, dependency in enumerate(service_plan.get("dependencies", ())):
        producers = tuple(str(item) for item in dependency.get("producers", ()))
        consumers = tuple(str(item) for item in dependency.get("consumers", ()))
        key_scope = str(dependency.get("keyScope", ""))
        topic_prefix = str(dependency.get("topicPrefix", "/activation"))
        if not producers or not consumers or not key_scope or not topic_prefix:
            raise ValueError("static V3 dependency is incomplete")
        value = {
            "producers": list(producers),
            "consumers": list(consumers),
            "key_scope": key_scope,
            "topic_prefix": topic_prefix,
            "object_name_template": str(dependency.get(
                "objectNameTemplate",
                "{producerProvider}/NDNSF/DI/DATA/{sessionId}/"
                "{keyScope}/{producerRole}",
            )),
            "expected_segments": int(dependency.get("expectedSegments", 0)),
            "expected_bytes": int(dependency.get("expectedBytes", 0)),
            "required": bool(dependency.get("required", True)),
            "tensors": [str(item) for item in dependency.get("tensors", ())],
            "transportProfile": str(dependency.get(
                "transportProfile", "COLLAB_LARGE_V1")),
            "collectiveOperationIndex": int(dependency.get(
                "collectiveOperationIndex", index)),
            "collectiveProducerRank": str(dependency.get(
                "collectiveProducerRank", "")),
            "collectiveSourceLayoutDigest": str(dependency.get(
                "collectiveSourceLayoutDigest", "")),
            "collectiveTargetLayoutDigest": str(dependency.get(
                "collectiveTargetLayoutDigest", "")),
            "collectiveTensorDigest": str(dependency.get(
                "collectiveTensorDigest", "")),
            "redistributions": list(dependency.get("redistributions", ())),
        }
        contracts.append(value)
    return tuple(contracts)


def _validated_data_v1_key_offer(candidate, provider: str) -> tuple[bytes, str]:
    fields = dict(getattr(candidate, "selection_input_key_offer", {}) or {})
    required = {
        "schemaVersion", "recipient", "recipientCertName",
        "recipientPublicKey", "recipientCertDigest", "providerBootEpoch",
        "ndnsfDataV1EndpointPrefix",
    }
    if not required.issubset(fields):
        raise ValueError(f"Provider {provider} omitted NDNSF_DATA_V1 key offer")
    if (fields["schemaVersion"] != "1" or fields["recipient"] != provider
            or not fields["recipientCertName"]
            or not fields["providerBootEpoch"]
            or not fields["ndnsfDataV1EndpointPrefix"]):
        raise ValueError(f"Provider {provider} returned a mismatched key offer")
    public_key_hex = str(fields["recipientPublicKey"])
    if (not public_key_hex or len(public_key_hex) % 2 != 0
            or public_key_hex != public_key_hex.lower()):
        raise ValueError(f"Provider {provider} returned an invalid key offer")
    try:
        public_key = bytes.fromhex(public_key_hex)
    except ValueError as exc:
        raise ValueError(
            f"Provider {provider} returned an invalid key offer") from exc
    expected_digest = "sha256:" + hashlib.sha256(public_key).hexdigest()
    if str(fields["recipientCertDigest"]).lower() != expected_digest:
        raise ValueError(f"Provider {provider} key offer digest mismatch")
    return public_key, str(fields["ndnsfDataV1EndpointPrefix"])


def _runtime_role_spec(
    *,
    role: str,
    provider: str,
    service_plan: Mapping[str, Any],
    evidence: Mapping[str, Any],
    plan_digest: str,
) -> RoleAssemblySpec:
    if str(evidence.get("providerName", "")) != provider:
        raise ValueError(f"Provider {provider} execution evidence identity mismatch")
    if str(evidence.get("planDigest", "")) != plan_digest:
        raise ValueError(f"Provider {provider} execution evidence plan mismatch")
    evidence_roles = tuple(str(item) for item in evidence.get("roles", ()))
    artifact_digests = dict(evidence.get("artifactDigests", {}) or {})
    artifact_digest = str(artifact_digests.get(role, "")).lower()
    if role not in evidence_roles or not artifact_digest.startswith("sha256:"):
        raise ValueError(
            f"Provider {provider} did not prove runtime readiness for {role}")

    runner_kind = str(evidence.get("runnerKind", ""))
    if runner_kind == "onnxruntime-cpu":
        backend = "onnxruntime-cpu"
    elif runner_kind == "onnxruntime-cuda":
        backend = "onnxruntime-cuda"
    else:
        raise ValueError(f"Provider {provider} has unsupported runner {runner_kind}")
    device = dict(evidence.get("device", {}) or {})
    device_kind = str(device.get("kind", ""))
    device_id = str(device.get("id", ""))
    if device_kind not in {"cpu", "cuda"} or not device_id:
        raise ValueError(f"Provider {provider} omitted runtime device evidence")
    if (backend.endswith("-cpu") != (device_kind == "cpu")):
        raise ValueError(f"Provider {provider} backend/device evidence mismatch")
    device_handle = "cpu" if device_kind == "cpu" else f"cuda:{device_id}"

    metadata = dict(service_plan.get("roleMetadata", {}).get(role, {}) or {})
    role_order = tuple(str(item) for item in service_plan.get("roles", ()))
    default_stage = role_order.index(role)
    stage = int(metadata.get("stage", default_stage))
    rank = int(metadata.get("rank", 0))
    recipe_digest = canonical_digest({
        "planDigest": plan_digest,
        "role": role,
        "rank": rank,
        "artifactDigest": artifact_digest,
        "backend": backend,
        "device": device_handle,
    })
    return RoleAssemblySpec(
        role=role,
        rank=rank,
        layer_begin=stage,
        layer_end=stage + 1,
        recipe_digest=recipe_digest,
        artifact_digest=artifact_digest,
        backend=backend,
        device_set=(() if device_kind == "cpu" else (device_handle,)),
        role_kind="HYBRID_RANK",
    )


def build_static_v3_selection_commit(
    *,
    closed,
    service_plan: Mapping[str, Any],
    role_provider_assignments: Mapping[str, str],
    plan_path: Path | str,
    deadline_ms: int,
    group_epoch_key_wrapper: Optional[
        Callable[[bytes, bytes], bytes]
    ] = None,
    execution_bindings_by_role: Optional[
        Mapping[str, Mapping[str, str]]
    ] = None,
    no_progress_ms: Optional[int] = None,
) -> StaticV3SelectionCommit:
    """Seal a static native plan after immutable ACK_CLOSED.

    The static MiniNDN/Tiger harness does not run the model-first automatic
    planner, but it must commit the same provider-scoped V3 Selection and
    request-scoped GroupCapability as that production path.
    """

    plan_path = Path(plan_path)
    plan_digest = execution_lease_plan_digest(str(plan_path))
    role_names = tuple(str(item) for item in service_plan.get("roles", ()))
    provider_by_role = {
        str(role): str(provider)
        for role, provider in role_provider_assignments.items()
    }
    if set(provider_by_role) != set(role_names) or any(
            not value for value in provider_by_role.values()):
        raise ValueError("static V3 commit requires one Provider for every role")
    if int(deadline_ms) <= int(time.time() * 1000):
        raise TimeoutError("static V3 commit deadline expired")

    candidates: dict[str, Any] = {}
    for candidate in tuple(closed.candidates):
        provider = str(candidate.provider_name)
        if (not bool(candidate.status) or provider not in set(provider_by_role.values())):
            continue
        if (str(candidate.request_id) != str(closed.request_id)
                or provider in candidates):
            raise ValueError("ACK_CLOSED Provider identity is ambiguous")
        candidates[provider] = candidate
    required_providers = set(provider_by_role.values())
    if set(candidates) != required_providers:
        raise ValueError("ACK_CLOSED does not cover every selected Provider")

    snapshots = tuple(ack_candidates_snapshot(list(candidates.values())))
    snapshots_by_provider = {
        str(item["provider"]): item for item in snapshots
    }
    if set(snapshots_by_provider) != required_providers:
        raise ValueError("Provider capability snapshots are incomplete")

    role_specs: dict[str, RoleAssemblySpec] = {}
    for role in role_names:
        provider = provider_by_role[role]
        evidence = snapshots_by_provider[provider].get("executionEvidence", {})
        if not isinstance(evidence, Mapping):
            raise ValueError(f"Provider {provider} omitted execution evidence")
        role_specs[role] = _runtime_role_spec(
            role=role,
            provider=provider,
            service_plan=service_plan,
            evidence=evidence,
            plan_digest=plan_digest,
        )

    dependencies = _v3_dependency_contracts(service_plan)
    data_v1_dependencies = tuple(
        (index, dependency)
        for index, dependency in enumerate(dependencies)
        if dependency["transportProfile"] == "NDNSF_DATA_V1"
    )
    if not data_v1_dependencies:
        raise ValueError("static V3 GroupCapability requires NDNSF_DATA_V1")

    parent: dict[str, str] = {}

    def find(item: str) -> str:
        parent.setdefault(item, item)
        while parent[item] != item:
            parent[item] = parent[parent[item]]
            item = parent[item]
        return item

    def union(left: str, right: str) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parent[max(left_root, right_root)] = min(left_root, right_root)

    dependency_providers: dict[int, tuple[tuple[str, ...], tuple[str, ...]]] = {}
    for index, dependency in data_v1_dependencies:
        producers = tuple(sorted({
            provider_by_role[str(role)] for role in dependency["producers"]
        }))
        consumers = tuple(sorted({
            provider_by_role[str(role)] for role in dependency["consumers"]
        }))
        members = tuple(sorted(set(producers) | set(consumers)))
        if not producers or not consumers or not members:
            raise ValueError("NDNSF_DATA_V1 dependency is not fully assigned")
        find(members[0])
        for member in members[1:]:
            union(members[0], member)
        dependency_providers[index] = (producers, consumers)

    components: dict[str, set[str]] = {}
    for provider in parent:
        components.setdefault(find(provider), set()).add(provider)
    public_keys: dict[str, bytes] = {}
    endpoints: dict[str, str] = {}
    offer_digests: dict[str, str] = {}
    for provider, candidate in candidates.items():
        public_keys[provider], endpoints[provider] = (
            _validated_data_v1_key_offer(candidate, provider))
        offer_digests[provider] = canonical_digest({
            "provider": provider,
            "requestId": str(candidate.request_id),
            "payloadDigest": (
                "sha256:" + hashlib.sha256(bytes(candidate.payload)).hexdigest()),
            "selectionInputKeyOffer": dict(
                candidate.selection_input_key_offer),
        })

    wrapper = group_epoch_key_wrapper or _native_group_epoch_key_wrapper
    configured_no_progress_ms = (
        2000 if no_progress_ms is None else int(no_progress_ms))
    if configured_no_progress_ms <= 0:
        raise ValueError("NDNSF_DATA_V1 no-progress bound must be positive")
    capability_by_provider: dict[str, str] = {}
    remaining_ms = max(1, int(deadline_ms) - int(time.time() * 1000))
    for component_index, member_set in enumerate(sorted(
            components.values(), key=lambda value: tuple(sorted(value)))):
        member_names = tuple(sorted(member_set))
        member_rank = {
            provider: rank for rank, provider in enumerate(member_names)
        }
        members = tuple(GroupMemberV1(
            provider=provider,
            rank=member_rank[provider],
            offer_digest=offer_digests[provider],
            endpoint_prefix=endpoints[provider],
        ) for provider in member_names)
        operations: list[GroupOperationV1] = []
        for index, dependency in data_v1_dependencies:
            producers, consumers = dependency_providers[index]
            if not (set(producers) | set(consumers)).issubset(member_set):
                continue
            redistributions = tuple(dependency.get("redistributions", ()))
            operation_kind = (
                str(redistributions[0].get("operation", ""))
                if redistributions else "PIPELINE_TRANSFER"
            )
            source_layout = str(dependency.get(
                "collectiveSourceLayoutDigest", ""))
            target_layout = str(dependency.get(
                "collectiveTargetLayoutDigest", ""))
            if not source_layout:
                source_layout = canonical_digest({
                    "scope": dependency["key_scope"], "side": "source"})
            if not target_layout:
                target_layout = canonical_digest({
                    "scope": dependency["key_scope"], "side": "target"})
            operations.append(GroupOperationV1(
                operation_index=int(dependency["collectiveOperationIndex"]),
                kind=operation_kind,
                producer_ranks=tuple(
                    str(member_rank[provider]) for provider in producers),
                consumer_ranks=tuple(
                    str(member_rank[provider]) for provider in consumers),
                tensor_layout_digest=canonical_digest({
                    "source": source_layout,
                    "target": target_layout,
                }),
                max_bytes=_DATA_V1_MAX_BYTES,
                max_segments=_DATA_V1_MAX_SEGMENTS,
            ))
        if not operations:
            raise ValueError("NDNSF_DATA_V1 Provider group has no operation")
        group_id = "group-" + canonical_digest({
            "plan": plan_digest,
            "component": component_index,
            "members": member_names,
        })[7:39]
        capability = seal_group_capability_v1(
            request_id=str(closed.request_id),
            attempt_id="attempt-1",
            plan_digest=plan_digest,
            group_id=group_id,
            epoch=1,
            ordered_members=members,
            permitted_operations=tuple(operations),
            max_inflight_bytes=_DATA_V1_MAX_BYTES,
            no_progress_ms=min(configured_no_progress_ms, remaining_ms),
            hard_deadline_ms=remaining_ms,
            wrap_epoch_key=lambda provider, key: wrapper(
                public_keys[provider], key),
        )
        for provider in member_names:
            capability_by_provider[provider] = (
                capability.project_for_provider(provider).to_bytes().hex())

    core_digest = canonical_digest({
        "requestId": str(closed.request_id),
        "ackClosedDigest": str(closed.digest),
        "planDigest": plan_digest,
        "providerByRole": provider_by_role,
        "roles": tuple(role_specs.values()),
        "dependencies": dependencies,
    })
    payloads: dict[str, bytes] = {}
    for provider in sorted(required_providers):
        provider_roles = tuple(
            role_specs[role] for role in role_names
            if provider_by_role[role] == provider
        )
        execution_bindings = {
            role: dict(execution_bindings_by_role[role])
            for role in role_names
            if (provider_by_role[role] == provider
                and execution_bindings_by_role is not None
                and role in execution_bindings_by_role)
        }
        if len(provider_roles) != 1:
            raise ValueError(
                "static V3 projection requires exactly one role per Provider")
        assembly = provider_roles[0]
        evidence = dict(
            snapshots_by_provider[provider].get("executionEvidence", {}) or {})
        topology_digest = canonical_digest({
            "provider": provider,
            "backend": assembly.backend,
            "deviceSet": tuple(assembly.device_set),
        })
        resource_digest = canonical_digest(evidence)
        offer_digest = offer_digests[provider]
        security_digest = canonical_digest({
            "profile": "STATIC_NATIVE_V3",
            "groupCapabilityV1": capability_by_provider.get(provider, ""),
        })
        execution_role = ExecutionRole(
            role_id=assembly.role, stage_id=assembly.role,
            rank=assembly.rank, layer_begin=assembly.layer_begin,
            layer_end=assembly.layer_end, backend=assembly.backend,
            adapter_id=assembly.adapter_id,
            adapter_version=assembly.adapter_version)
        dataflow = RoleDataflowContract(
            request_id=str(closed.request_id), attempt=1,
            plan_digest=plan_digest, role=assembly.role)
        cpu = assembly.backend.endswith("-cpu")
        device_binding = DeviceBinding(
            mode=(DeviceBindingMode.CPU if cpu
                  else DeviceBindingMode.SINGLE_DEVICE),
            provider=provider, role=assembly.role,
            offer_digest=offer_digest,
            topology_profile_digest=topology_digest,
            resource_snapshot_digest=resource_digest,
            resource_sequence=1,
            offer_scoped_device_handle=("" if cpu else assembly.device_set[0]))
        projection = ProviderSelectionProjectionV3(
            provider=provider,
            request_id=str(closed.request_id),
            attempt=1,
            plan_core_digest=core_digest,
            plan_digest=plan_digest,
            ack_closed_digest=str(closed.digest),
            offer_digest=offer_digest,
            security_policy_snapshot_digest=security_digest,
            roles=provider_roles,
            dependencies=dependencies,
            deadline_ms=int(deadline_ms),
            execution_role=execution_role,
            assembly=assembly,
            dataflow=dataflow,
            device_binding=device_binding,
            group_capability_v1=capability_by_provider.get(provider, ""),
            execution_bindings=execution_bindings,
        )
        payload = projection.to_bytes()
        for role in role_names:
            if provider_by_role[role] == provider:
                payloads[role] = payload

    return StaticV3SelectionCommit(
        plan_digest=plan_digest,
        provider_by_role=MappingProxyType(dict(provider_by_role)),
        assignment_payloads_by_role=MappingProxyType(payloads),
        ack_snapshots=snapshots,
    )


def observed_ack_runtime_from_snapshots(snapshots: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Convert ack candidate snapshots into per-provider observed runtime state.

    For each provider, keep the latest (highest-valued) runtime counters
    across all snapshots for local runtime-aware replanning diagnostics.
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
    fixed_request_id = str(getattr(args, "fixed_request_id", "") or "").strip()
    request_id = wire_request_id(
        fixed_request_id,
        default=f"native-tracer-{index}")
    debug_user_stage("request-enter", requestId=request_id, index=index)
    try:
        preference = str(getattr(args, "role_provider_preference", ""))
        lease_transaction = None
        lease_set = None
        execution_activation = None
        request_roles = roles
        if args.execution_leases:
            lease_transaction, lease_set, request_roles, lease_preference, execution_activation = (
                acquire_execution_leases(
                    user, args, service_plan, roles, index, request_id
                )
            )
            preference = lease_preference
        execution_bindings_by_role = (
            execution_bindings_from_roles(request_roles)
            if lease_transaction is not None else None
        )
        ack_snapshots: list[dict[str, Any]] = []

        uses_data_v1 = any(
            str(item.get("transportProfile", "")) == "NDNSF_DATA_V1"
            for item in service_plan.get("dependencies", ())
        )
        debug_user_stage(
            "request-contract-ready",
            requestId=request_id,
            usesDataV1=uses_data_v1,
            roleCount=len(request_roles),
        )
        if uses_data_v1 and not preference:
            provider_by_role = static_provider_map_from_assignment_csv(
                str(getattr(args, "assignment_csv", "")), request_roles)
            preference = ";".join(
                f"{role}=>{provider_by_role[role]}"
                for role in provider_by_role) + ";"

        def observe_ack_candidates(candidates) -> None:
            ack_snapshots.extend(ack_candidates_snapshot(candidates))

        try:
            assignment_context = assignment_context_from_preference(
                preference, request_id=request_id,
                deadline_ms=int(time.time() * 1000) + effective_timeout_ms(args))
            if uses_data_v1:
                if assignment_context is None:
                    raise RuntimeError(
                        "NDNSF_DATA_V1 static V3 selection requires an explicit "
                        "Provider for every role")
                timeout_ms = effective_timeout_ms(args)
                deadline_ms = int(time.time() * 1000) + timeout_ms
                debug_user_stage(
                    "begin-collaboration-before",
                    requestId=request_id,
                    capabilities={"NDNSF_DATA_V1": "required"},
                )
                invocation = user.begin_collaboration(
                    args.service,
                    encode_tensor_bundle(),
                    mode="DEFERRED",
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=timeout_ms,
                    request_id=request_id,
                    fail_fast_terminal_selection=True,
                    request_capabilities={"NDNSF_DATA_V1": "required"},
                )
                debug_user_stage(
                    "begin-collaboration-after",
                    requestId=request_id,
                    nativeRequestId=invocation.request_id,
                )
                debug_user_stage("acks-closed-before", requestId=request_id)
                closed = invocation.acks_closed()
                debug_user_stage(
                    "acks-closed-after",
                    requestId=request_id,
                    candidateCount=len(closed.candidates),
                )
                commit = build_static_v3_selection_commit(
                    closed=closed,
                    service_plan=service_plan,
                    role_provider_assignments=(
                        assignment_context.providers_by_role()),
                    plan_path=args.plan,
                    deadline_ms=deadline_ms,
                    no_progress_ms=int(getattr(
                        args, "data_v1_no_progress_ms", 2000)),
                    execution_bindings_by_role=execution_bindings_by_role,
                )
                ack_snapshots.extend(commit.ack_snapshots)
                debug_user_stage(
                    "commit-plan-before",
                    requestId=request_id,
                    providerCount=len(commit.provider_by_role),
                )
                if not invocation.commit_plan(
                    ack_closed_digest=closed.digest,
                    roles=request_roles,
                    key_scopes=key_scopes,
                    dependencies=dependencies,
                    scope_key_data_names=scope_key_data_names,
                    role_scopes=role_scopes,
                    role_provider_assignments=dict(commit.provider_by_role),
                    assignment_payloads_by_role=dict(
                        commit.assignment_payloads_by_role),
                ):
                    raise RuntimeError(
                        "NDNSF_DATA_V1 static V3 Selection commit was rejected")
                debug_user_stage("commit-plan-after", requestId=request_id)
                debug_user_stage("response-before", requestId=request_id)
                response = invocation.result()
                debug_user_stage(
                    "response-after",
                    requestId=request_id,
                    status=bool(response.status),
                )
            else:
                response = user.request_collaboration(
                    args.service,
                    encode_tensor_bundle(),
                    roles=request_roles,
                    key_scopes=key_scopes,
                    dependencies=dependencies,
                    scope_key_data_names=scope_key_data_names,
                    role_scopes=role_scopes,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=effective_timeout_ms(args),
                    request_id=request_id,
                    ack_observer=observe_ack_candidates,
                    assignment_context=assignment_context,
                )
        except Exception:
            if lease_transaction is not None and lease_set is not None:
                lease_transaction.release(lease_set)
            raise
        if (
            not response.status
            and lease_transaction is not None
            and lease_set is not None
        ):
            lease_transaction.release(lease_set)
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
            "ackCandidateSnapshot": ack_snapshots,
        }
        response_payload = bytes(response.payload)
        result["payloadSha256"] = (
            "sha256:" + hashlib.sha256(response_payload).hexdigest())
        if response.status:
            try:
                result["tensorBundle"] = summarize_tensor_bundle(response_payload)
            except Exception as exc:
                result["tensorBundleError"] = str(exc)
        status_request_id = next(
            (str(item.get("requestId", "")) for item in ack_snapshots
             if item.get("requestId")),
            request_id,
        )
        try:
            snapshots = user.collaboration_status(
                status_request_id, timeout_ms=500)
            result["collaborationStatus"] = [
                {
                    "provider": snapshot.provider_name,
                    "selectionDigest": snapshot.selection_digest,
                    "state": snapshot.state,
                    "members": [
                        {
                            "role": member.role,
                            "operationId": member.operation_id,
                            "state": member.state.value,
                            "attempt": member.attempt,
                            "epoch": member.epoch,
                            "sequence": member.sequence,
                            "progressKnown": member.progress_known,
                            "progress": member.progress,
                            "detailsSchema": member.details_schema,
                        }
                        for member in snapshot.member_statuses
                    ],
                }
                for snapshot in snapshots
            ]
        except Exception as exc:
            result["collaborationStatusError"] = str(exc)
        if execution_activation is not None:
            result["activationDigest"] = execution_activation.digest()
            result["activatedProviders"] = sorted({
                member.provider
                for member in execution_activation.members
            })
        if is_overload_fast_fail_error(args, str(response.error), elapsed_ms):
            result["overloadFastFail"] = True
        return json_compatible(result)
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
        }
        if is_overload_fast_fail_error(args, error, elapsed_ms):
            result["overloadFastFail"] = True
        return json_compatible(result)


def _execution_control(
    user: ServiceUser,
    *,
    provider: str,
    role: str,
    service: str,
    request_id: str,
    attempt_epoch: int,
    operation: str,
    timeout_ms: int,
    requester_identity: str,
    activation_digest: str,
    superseded_by_attempt_epoch: int = 0,
) -> dict[str, Any]:
    cancellation_id = "sha256:" + hashlib.sha256(
        (f"{operation}:{request_id}:{attempt_epoch}:{provider}:"
         f"{activation_digest}:{superseded_by_attempt_epoch}").encode()
    ).hexdigest()
    fields = {
        "schema": "ndnsf-di-execution-control-v2",
        "operation": operation,
        "requestId": request_id,
        "attemptEpoch": str(attempt_epoch),
        "providerName": provider,
        "providerRole": role,
        "requesterIdentity": requester_identity,
        "activationDigest": activation_digest,
        "cancellationId": cancellation_id,
    }
    if superseded_by_attempt_epoch:
        fields["supersededByAttemptEpoch"] = str(superseded_by_attempt_epoch)
    payload = "".join(f"{key}={value};" for key, value in fields.items()).encode()
    assignment_context = assignment_context_from_preference(
        f"{role}=>{provider};",
        request_id=f"control:{operation}:{request_id}:{attempt_epoch}:{role}",
        deadline_ms=int(time.time() * 1000) + timeout_ms,
    )
    response = user.request_collaboration(
        service,
        payload,
        roles=[{
            "role": role,
            "service": service,
            "min_providers": 1,
            "max_providers": 1,
        }],
        key_scopes={},
        dependencies=[],
        role_scopes={role: []},
        ack_timeout_ms=min(1000, max(100, timeout_ms // 3)),
        timeout_ms=timeout_ms,
        assignment_context=assignment_context,
    )
    response_fields = parse_semicolon_fields(response.payload)
    return {
        "provider": provider,
        "operation": operation,
        "requestId": request_id,
        "attemptEpoch": attempt_epoch,
        "accepted": bool(response.status) and response_fields.get("status") == "1",
        "reason": response_fields.get("reason", response.error),
        "dataName": response.data_name,
        "signerCertificate": response.signer_certificate,
        "wireDigest": response.wire_digest,
    }


def _validate_control_delivery(
    evidence: list[dict[str, Any]], expected_providers: list[str]
) -> None:
    if {item.get("provider") for item in evidence} != set(expected_providers):
        raise RuntimeError(f"execution control missed certified members: {evidence}")
    for item in evidence:
        provider = str(item["provider"]).rstrip("/")
        data_name = str(item.get("dataName", ""))
        signer = str(item.get("signerCertificate", ""))
        wire_digest = str(item.get("wireDigest", ""))
        if (
            not data_name.startswith(provider + "/")
            or not signer.startswith(provider + "/KEY/")
            or not wire_digest.startswith("sha256:")
        ):
            raise RuntimeError(
                "execution control lacks exact authenticated Provider evidence: "
                f"{item}"
            )


def run_execution_cancellation_gate(
    user: ServiceUser,
    args,
    service_plan: dict,
    roles: list[dict],
    key_scopes: dict[str, list[str]],
    dependencies: list[dict],
    scope_key_data_names: dict[str, str],
    role_scopes: dict[str, list[str]],
) -> dict[str, Any]:
    if not args.execution_leases:
        raise RuntimeError("execution cancellation gate requires execution leases")
    transaction, lease_set, request_roles, preference, activation = (
        acquire_execution_leases(
            user, args, service_plan, roles, 1,
            # DATA_V1 Selection carries the request ID through canonical NDN
            # URI encoding.  Keep the lease transaction ID URI-safe as well;
            # a raw ':' would be stored by the lease table while the
            # Selection projection is observed by the Provider as '%3A',
            # producing LEASE_REQUEST_MISMATCH before execution starts.
            wire_request_id("", default="native-tracer-cancellation")))
    request_id = activation.request_id
    providers = sorted({item.provider for item in activation.members})
    role_by_provider = {}
    for member in activation.members:
        role_by_provider.setdefault(member.provider, member.role)
    execution_bindings = execution_bindings_from_roles(request_roles)

    def control_all(operation: str, attempt_epoch: int, *, next_epoch: int = 0):
        def send(provider: str):
            return _execution_control(
                user,
                provider=provider,
                role=role_by_provider[provider],
                service=args.service,
                request_id=request_id,
                attempt_epoch=attempt_epoch,
                operation=operation,
                superseded_by_attempt_epoch=next_epoch,
                timeout_ms=args.lease_timeout_ms,
                requester_identity=args.user,
                activation_digest=activation.digest(),
            )
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=max(1, len(providers))) as executor:
            return list(executor.map(send, providers))
    try:
        # DATA_V1 requires the immutable ACK_CLOSED -> V3 Selection commit.
        # The previous cancellation gate used request_collaboration_async,
        # which serialized only the legacy semicolon assignment and therefore
        # never delivered groupCapabilityV1 to the Provider.
        invocation = user.begin_collaboration(
            args.service,
            encode_tensor_bundle(),
            mode="DEFERRED",
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=effective_timeout_ms(args),
            request_id=request_id,
            fail_fast_terminal_selection=True,
            request_capabilities={"NDNSF_DATA_V1": "required"},
        )
        closed = invocation.acks_closed()
        assignment_context = assignment_context_from_preference(
            preference,
            request_id=request_id,
            deadline_ms=int(time.time() * 1000) + effective_timeout_ms(args),
        )
        if assignment_context is None:
            raise RuntimeError(
                "execution cancellation gate has no complete Provider assignment")
        commit = build_static_v3_selection_commit(
            closed=closed,
            service_plan=service_plan,
            role_provider_assignments=assignment_context.providers_by_role(),
            plan_path=args.plan,
            deadline_ms=int(time.time() * 1000) + effective_timeout_ms(args),
            no_progress_ms=int(getattr(
                args, "data_v1_no_progress_ms", 2000)),
            execution_bindings_by_role=execution_bindings,
        )
        if not invocation.commit_plan(
                ack_closed_digest=closed.digest,
                roles=roles,
                key_scopes=key_scopes,
                dependencies=dependencies,
                scope_key_data_names=scope_key_data_names,
                role_scopes=role_scopes,
                role_provider_assignments=dict(commit.provider_by_role),
                assignment_payloads_by_role=dict(
                    commit.assignment_payloads_by_role),
        ):
            raise RuntimeError(
                "NDNSF_DATA_V1 cancellation Selection commit was rejected")

        response_state: dict[str, Any] = {}
        response_ready = threading.Event()

        def await_response() -> None:
            try:
                response_state["response"] = invocation.result(
                    timeout_ms=effective_timeout_ms(args) + 2000)
            except Exception as exc:
                response_state["error"] = str(exc)
            finally:
                response_ready.set()

        threading.Thread(target=await_response, daemon=True).start()
        cancellation_delay_ms = (
            args.cancellation_delay_ms
            if args.cancellation_delay_ms > 0
            else args.ack_timeout_ms + 1000
        )
        time.sleep(cancellation_delay_ms / 1000.0)
        cancel = control_all("CANCEL", 1)
        _validate_control_delivery(cancel, providers)
        if not any(item["accepted"] for item in cancel):
            raise RuntimeError(f"no active certified Provider applied CANCEL: {cancel}")
        if any(item["reason"] not in {"CANCELLED", "CANCEL_REJECTED"}
               for item in cancel):
            raise RuntimeError(f"unexpected certified Provider CANCEL result: {cancel}")

        # Fence the cancelled attempt while its workers may still be running.
        # Waiting for invocation.result() here defeats the gate: the cancelled
        # DATA_V1 group can remain in dependency cleanup until its workers
        # finish, so SUPERSEDE would arrive after the attempt is already
        # terminal instead of testing in-flight replacement.
        supersede = control_all("SUPERSEDE", 1, next_epoch=2)
        _validate_control_delivery(supersede, providers)
        if not all(item["accepted"] for item in supersede):
            raise RuntimeError(f"replacement attempt was not admitted: {supersede}")
        stale = control_all("CANCEL", 1)
        _validate_control_delivery(stale, providers)
        if any(item["accepted"] for item in stale):
            raise RuntimeError(f"stale cancellation affected replacement: {stale}")
        replacement_cleanup = control_all("CANCEL", 2)
        _validate_control_delivery(replacement_cleanup, providers)
        if not all(item["accepted"] for item in replacement_cleanup):
            raise RuntimeError(
                f"replacement attempt cleanup was incomplete: {replacement_cleanup}")
        response_ready.wait(timeout=2.0)
        response = response_state.get("response")
        if response is not None and response.status:
            raise RuntimeError(
                "cancelled or superseded attempt published an accepted terminal result")
    finally:
        try:
            transaction.release(lease_set)
        except Exception:
            pass

    # The terminal-preservation request is a fresh ordinary DATA_V1 request,
    # but it still uses the same authenticated per-role lease binding path as
    # every Provider started with --require-execution-lease.
    completed = run_one_request(
        user,
        args,
        service_plan,
        roles,
        key_scopes,
        dependencies,
        scope_key_data_names,
        role_scopes,
        2,
    )
    if completed.get("status") != "executed":
        raise RuntimeError(f"terminal preservation request failed: {completed}")
    completed_request_id = wire_request_id(
        "", default="native-tracer-2")
    late_cancel = [
        _execution_control(
            user,
            provider=provider,
            role=role_by_provider[provider],
            service=args.service,
            request_id=completed_request_id,
            attempt_epoch=1,
            operation="CANCEL",
            timeout_ms=args.lease_timeout_ms,
            requester_identity=args.user,
            activation_digest=str(completed.get("activationDigest", "")),
        )
        for provider in providers
    ]
    _validate_control_delivery(late_cancel, providers)
    if any(item["accepted"] for item in late_cancel):
        raise RuntimeError(f"late cancel revoked an accepted terminal attempt: {late_cancel}")
    return {
        "schema": "ndnsf-di-spec111-cancellation-fault-gate-v1",
        "status": "PASS",
        "requestId": request_id,
        "attemptEpoch": 1,
        "activationDigest": activation.digest(),
        "activatedProviders": providers,
        "cancelEvidence": cancel,
        "staleCancelEvidence": stale,
        "replacementCleanupEvidence": replacement_cleanup,
        "acceptedTerminalRequestId": completed_request_id,
        "acceptedTerminalPreserved": True,
        "dataV1NoProgressMs": int(getattr(
            args, "data_v1_no_progress_ms", 2000)),
        "lateCancelEvidence": late_cancel,
        "survivors": [],
    }


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
    measurement_elapsed_ms = (time.perf_counter() - schedule_start) * 1000.0
    metadata = {
        "mode": "open-loop-threaded-service-user",
        "targetRps": args.target_rps,
        "openLoopDurationS": args.open_loop_duration_s,
        "scheduledRequestCount": planned,
        "submittedCount": len(results) - len(dropped),
        "localBackpressureCount": len(dropped),
        "localBackpressureWaitCount": local_backpressure_waits,
        "maxScheduleSlipMs": round(max_schedule_slip_ms, 3),
        "measurementElapsedMs": measurement_elapsed_ms,
        "offeredRps": planned / args.open_loop_duration_s if args.open_loop_duration_s > 0 else 0.0,
    }
    return sorted(results, key=lambda item: int(item.get("requestIndex", 0))), metadata


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
            "--data-v1-no-progress-ms", str(args.data_v1_no_progress_ms),
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
        if role_provider_preference:
            command.extend(["--role-provider-preference", role_provider_preference])
        command.extend(["--lease-timeout-ms", str(args.lease_timeout_ms)])
        if args.execution_leases:
            command.append("--execution-leases")
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
    if args.cancellation_delay_ms < 0:
        raise SystemExit("--cancellation-delay-ms must be non-negative")
    if args.data_v1_no_progress_ms <= 0:
        raise SystemExit("--data-v1-no-progress-ms must be positive")
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
            "dataV1NoProgressMs": args.data_v1_no_progress_ms,
        }
        payload.update(runtime_replan_metadata(args))
        payload.update(overload_fast_fail_metadata(args))
        print(json.dumps(json_compatible(payload), indent=2, sort_keys=True))
        return 0

    try:
        user_ack_threads = max(
            1, int(os.environ.get("NDNSF_DI_USER_ACK_THREADS", "2")))
    except ValueError as exc:
        raise SystemExit("NDNSF_DI_USER_ACK_THREADS must be an integer") from exc
    user = ServiceUser(
        group=args.group,
        controller=args.controller,
        user=args.user,
        trust_schema=args.trust_schema,
        permission_wait_ms=args.permission_wait_ms,
        ack_threads=user_ack_threads,
        serve_certificates=True,
        bootstrap_token=args.bootstrap_token,
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
    if args.execution_leases and LEASE_SERVICE_NAME not in allowed:
        result = {
            "status": "failed",
            "service": args.service,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": (
                f"missing user permission for {LEASE_SERVICE_NAME}; "
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
    if args.execution_cancellation_gate:
        gate = run_execution_cancellation_gate(
            user, args, service_plan, roles, key_scopes, dependencies,
            scope_key_data_names, role_scopes)
        print(
            "NDNSF_DI_SPEC111_CANCELLATION_GATE " +
            json.dumps(gate, sort_keys=True),
            flush=True,
        )
        execution = {
            "status": "executed",
            "service": args.service,
            "requestCount": 2,
            "concurrency": 1,
            "successCount": 1,
            "failureCount": 1,
            "payloadBytes": 0,
            "elapsedMs": 0.0,
            "dataV1NoProgressMs": args.data_v1_no_progress_ms,
            "cancellationGate": gate,
            "requests": [],
        }
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_EXECUTION " +
            json.dumps(execution, sort_keys=True),
            flush=True,
        )
        return 0
    if args.wait_for_deployment:
        dep = wait_deployment(
            user, args.wait_for_deployment, timeout_ms=30000
        )
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
        **overload_fast_fail_metadata(args),
        "dataV1NoProgressMs": args.data_v1_no_progress_ms,
    }
    workload_metadata = dict(base_workload_metadata)
    if open_loop:
        print(
            "NDNSF_DI_NATIVE_TRACER_USER_CONCURRENCY "
            + json.dumps({
                "mode": (
                    "open-loop-threaded-service-user"
                    if args.open_loop_driver_mode == "threaded" else
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
            results, workload_metadata = run_with_started_user(
                user,
                lambda: run_threaded_open_loop_requests(
                    worker_users,
                    args,
                    service_plan,
                    roles,
                    key_scopes,
                    dependencies,
                    scope_key_data_names,
                    role_scopes),
            )
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
                result = retry_call(
                    _do_request,
                    retry_policy,
                    idempotent=True,
                    reason_getter=lambda item: (
                        RetryReason.TIMEOUT
                        if float(item.get("elapsedMs", 0.0)) >= effective_timeout_ms(args) * 0.9
                        else RetryReason.NON_RETRYABLE
                    ),
                )
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

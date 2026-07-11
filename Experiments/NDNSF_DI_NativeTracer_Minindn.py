#!/usr/bin/env python3
"""Real MiniNDN evidence launcher for the NDNSF-DI native tracer."""

from __future__ import annotations

import argparse
from collections import Counter
import csv
import json
import math
import os
import re
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))
sys.path.insert(0, str(REPO / "NDNSF-DistributedRepo" / "pythonWrapper"))
sys.path.insert(0, str(REPO / "Experiments"))
sys.path.insert(0, str(REPO / "pythonWrapper"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from ndnsf import (  # noqa: E402
    AckCompatibilityCounters,
    PeerNetworkMetric,
    ProviderCapabilityHint,
    ProviderNetworkMatrix,
    ServiceOperationState,
    ServiceOperationStatus,
    decode_provider_capability_ack,
    parse_ack_metadata,
    to_plain,
)
from ndnsf_distributed_inference.runtime_v1_evidence import write_minindn_runtime_v1_evidence  # noqa: E402
from mininet.log import info, setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import getPopen  # noqa: E402

TOPO = REPO / "Experiments/Topology/AI_Lab.conf"
TRACER_DIR = REPO / "examples/python/NDNSF-DistributedInference/native_di_tracer"
PLAN_TRACER = TRACER_DIR / "plan_tracer.py"
LLM_BUNDLE_GENERATOR = TRACER_DIR / "generate_llm_proportional_native_bundle.py"
RUNTIME_V1_MODEL_SPEC = TRACER_DIR / "llm_model_spec_qwen_tiny_proportional.json"
RUNTIME_V1_PROVIDER_PROFILES = TRACER_DIR / "llm_provider_profiles_2_4_8.json"
RUNTIME_AWARE_FIXTURES = TRACER_DIR / "runtime_aware_fixtures"
RUNTIME_AWARE_MULTI_USER_WORKLOAD = RUNTIME_AWARE_FIXTURES / "multi_user_requests.json"
USER_DRIVER = TRACER_DIR / "user_driver.py"
PROVIDER_EXE = REPO / "build/examples/di-native-provider"
PLAN_SCHEMA_EXE = REPO / "build/examples/di-native-plan-schema-smoke"
PLAN_MANIFEST_EXE = REPO / "build/examples/di-native-plan-manifest-smoke"
PROVIDER_SESSION_EXE = REPO / "build/examples/di-native-provider-session-smoke"
DEFAULT_OUT = REPO / "results/native_di_real_minindn/latest"
SERVICE = "/Inference/NativeTracer"
EXECUTION_LEASE_SERVICE = "/Inference/Control/Lease"
GROUP = "/NDNSF-DI/Tracer/group"
CONTROLLER = "/NDNSF-DI/Tracer/controller"
USER = "/NDNSF-DI/Tracer/user"
IDENTITY_SAFEBAG_PASSPHRASE = "ndnsf-di-native-tracer"
REQUIRED_ROLES = ["/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"]
NEGATIVE_ACK_RECORDED_RE = re.compile(r"event=NEGATIVE_ACK_RECORDED\b.*?\breason=([^\s,]+)")
NATIVE_ACK_DECISION_RE = re.compile(
    r"NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION\b.*?\bstatus=0\b.*?\bmessage=\"([^\"]*)\"")
NEGATIVE_ACK_PAYLOAD_RE = re.compile(r"\bnegativeAckReason=([^;\s]+)")


def _execution_operation_state(status: str, reason: str = "") -> ServiceOperationState:
    status_text = str(status or "").lower()
    reason_text = str(reason or "").lower()
    combined = f"{status_text} {reason_text}"
    if "timeout" in combined:
        return ServiceOperationState.EXPIRED
    if status_text in {"executed", "success", "successful", "local-baseline-executed"}:
        return ServiceOperationState.DONE
    if status_text in {"failed", "failure", "error"}:
        return ServiceOperationState.FAILED
    if status_text in {"gated", "not-started", "skipped"}:
        return ServiceOperationState.QUEUED
    if status_text in {"running", "started", "in-progress"}:
        return ServiceOperationState.RUNNING
    return ServiceOperationState.QUEUED


def _execution_operation_progress(payload: dict[str, Any],
                                  state: ServiceOperationState) -> float:
    request_count = int(payload.get("requestCount", 0) or 0)
    if request_count > 0:
        success_count = int(payload.get("successCount", 0) or 0)
        return max(0.0, min(1.0, success_count / request_count))
    if state in {
        ServiceOperationState.DONE,
        ServiceOperationState.FAILED,
        ServiceOperationState.CANCELED,
        ServiceOperationState.EXPIRED,
    }:
        return 1.0
    if state == ServiceOperationState.RUNNING:
        return 0.5
    return 0.0


def _execution_operation_metadata(payload: dict[str, Any]) -> dict[str, Any]:
    metadata: dict[str, Any] = {}
    for key in (
        "status",
        "reason",
        "requestCount",
        "successCount",
        "failureCount",
        "concurrency",
        "targetRps",
        "throughputRps",
        "payloadBytes",
        "roles",
        "log",
    ):
        if key in payload:
            metadata[key] = payload[key]
    if "requests" in payload:
        requests = payload.get("requests")
        metadata["requestSampleCount"] = len(requests) if isinstance(requests, list) else 0
    return metadata


def with_execution_operation_status(payload: dict[str, Any],
                                    *,
                                    operation: str,
                                    operation_id: str,
                                    service_name: str = SERVICE) -> dict[str, Any]:
    result = dict(payload or {})
    existing = result.get("operationStatus", result.get("operation_status"))
    if isinstance(existing, dict):
        try:
            result["operationStatus"] = to_plain(ServiceOperationStatus.from_dict(existing))
            return result
        except Exception:
            pass
    status_text = str(result.get("status", ""))
    reason = str(result.get("reason", result.get("error", "")))
    state = _execution_operation_state(status_text, reason)
    operation_status = ServiceOperationStatus(
        operation_id=operation_id,
        operation=operation,
        service_name=service_name,
        state=state,
        reason_code=status_text.upper().replace("-", "_"),
        message=reason or status_text,
        progress=_execution_operation_progress(result, state),
        updated_at_ms=int(time.time() * 1000),
        metadata=_execution_operation_metadata(result),
    )
    result["operationStatus"] = to_plain(operation_status)
    return result


def attach_execution_operation_statuses(summary: dict[str, Any]) -> dict[str, Any]:
    if isinstance(summary.get("userExecution"), dict):
        summary["userExecution"] = with_execution_operation_status(
            summary["userExecution"],
            operation="DI_USER_EXECUTION",
            operation_id="native-tracer-user-execution",
        )
    if isinstance(summary.get("dependencyExecution"), dict):
        summary["dependencyExecution"] = with_execution_operation_status(
            summary["dependencyExecution"],
            operation="DI_DEPENDENCY_EXECUTION",
            operation_id="native-tracer-dependency-execution",
        )
    return summary
NATIVE_TRACER_PROFILE_FIELDS = {
    "out": "out",
    "assignment": "assignment",
    "policy_bundle": "policy_bundle",
    "llm_planner_mode": "llm_planner_mode",
    "runtime_aware_user_planner": "runtime_aware_user_planner",
    "provider_network_matrix_json": "provider_network_matrix_json",
    "skip_provider_pair_telemetry_probe": "skip_provider_pair_telemetry_probe",
    "tracer_deterministic_runner": "tracer_deterministic_runner",
    "provider_check_timeout": "provider_check_timeout",
    "local_execution_only": "local_execution_only",
    "full_network": "full_network",
    "core_trace": "core_trace",
    "activation_pad_bytes": "activation_pad_bytes",
    "role_execution_delay_ms": "role_execution_delay_ms",
    "llm_stage_execution_delay_scale": "llm_stage_execution_delay_scale",
    "requests": "requests",
    "concurrency": "concurrency",
    "target_rps": "target_rps",
    "open_loop_duration_s": "open_loop_duration_s",
    "open_loop_driver_mode": "open_loop_driver_mode",
    "submission_spacing_ms": "submission_spacing_ms",
    "runtime_v1_context_tokens": "runtime_v1_context_tokens",
    "runtime_v1_generated_tokens": "runtime_v1_generated_tokens",
    "runtime_v1_prefix_id": "runtime_v1_prefix_id",
    "provider_admission_max_queue": "provider_admission_max_queue",
    "provider_admission_max_active_workers": "provider_admission_max_active_workers",
    "provider_admission_min_free_memory_mb": "provider_admission_min_free_memory_mb",
    "enable_native_admission_lease": "enable_native_admission_lease",
    "overload_fast_fail_timeout_ms": "overload_fast_fail_timeout_ms",
    "multi_user_workload": "multi_user_workload",
    "runtime_aware_max_replans": "runtime_aware_max_replans",
    "runtime_aware_replan_reasons": "runtime_aware_replan_reasons",
}


def load_json_file(path: str) -> dict:
    if not path:
        return {}
    with Path(path).expanduser().open(encoding="utf-8") as fh:
        return json.load(fh)


def load_multi_user_workload(path: str | Path) -> dict[str, Any]:
    workload_path = Path(path)
    if not str(path):
        return {"enabled": False, "path": "", "requests": []}
    payload = json.loads(workload_path.read_text(encoding="utf-8"))
    requests = payload.get("requests", [])
    if not isinstance(requests, list):
        raise RuntimeError(f"multi-user workload requests must be a list: {workload_path}")
    return {
        "enabled": True,
        "path": str(workload_path),
        "schema": payload.get("schema", ""),
        "serviceName": payload.get("serviceName", SERVICE),
        "requestCount": len(requests),
        "users": sorted({
            str(item.get("user", ""))
            for item in requests
            if isinstance(item, dict) and item.get("user")
        }),
        "requests": requests,
    }


def native_tracer_section(payload: dict) -> dict:
    profile = payload.get("profile") if isinstance(payload.get("profile"), dict) else payload
    distributed = profile.get("distributed_inference", {})
    native = distributed.get("native_tracer", {})
    if not isinstance(native, dict):
        return {}
    return native if native.get("enabled", False) else {}


def runtime_profile_defaults(runtime_profile: str, runtime_resolved: str) -> dict[str, object]:
    defaults: dict[str, object] = {}
    sources = [runtime_profile, runtime_resolved]
    for source in sources:
        section = native_tracer_section(load_json_file(source))
        for key, dest in NATIVE_TRACER_PROFILE_FIELDS.items():
            if key in section:
                defaults[dest] = section[key]
    return defaults


def default_value(defaults: dict[str, object], key: str, fallback):
    return defaults.get(key, fallback)


def default_value_any(defaults: dict[str, object], keys: list[str], fallback):
    for key in keys:
        if key in defaults:
            return defaults[key]
    return fallback


def user_worker_identities(requests: int) -> list[str]:
    return [f"{USER}/worker/{index}" for index in range(1, max(1, requests) + 1)]

DEFAULT_ASSIGNMENT = {
    "/Backbone": ("ucla", "/NDNSF-DI/Tracer/provider/backbone"),
    "/Head/Shard/0": ("arizona", "/NDNSF-DI/Tracer/provider/head0"),
    "/Head/Shard/1": ("wustl", "/NDNSF-DI/Tracer/provider/head1"),
    "/Merge": ("neu", "/NDNSF-DI/Tracer/provider/merge"),
}

ALTERNATE_ASSIGNMENT = {
    "/Backbone": ("neu", "/NDNSF-DI/Tracer/alt-provider/backbone"),
    "/Head/Shard/0": ("ucla", "/NDNSF-DI/Tracer/alt-provider/head0"),
    "/Head/Shard/1": ("arizona", "/NDNSF-DI/Tracer/alt-provider/head1"),
    "/Merge": ("wustl", "/NDNSF-DI/Tracer/alt-provider/merge"),
}

SINGLE_PROVIDER_ASSIGNMENT = {
    "/Backbone": ("ucla", "/NDNSF-DI/Tracer/provider/single"),
    "/Head/Shard/0": ("ucla", "/NDNSF-DI/Tracer/provider/single"),
    "/Head/Shard/1": ("ucla", "/NDNSF-DI/Tracer/provider/single"),
    "/Merge": ("ucla", "/NDNSF-DI/Tracer/provider/single"),
}

CAPACITY_POOL_ALTERNATE_ASSIGNMENT = "capacity-pool-alt"
CAPACITY_POOL_EXTRA_PROVIDERS = [
    {
        "assignment": CAPACITY_POOL_ALTERNATE_ASSIGNMENT,
        "role": role,
        "roles": role,
        "provider": provider,
        "node": node,
        "service": SERVICE,
    }
    for role, (node, provider) in ALTERNATE_ASSIGNMENT.items()
]

LLM_PROVIDER_RESOURCE_PROFILES = {
    "/NDNSF-DI/Tracer/provider/llm-2gb": {
        "gpuMemoryMb": "2048",
        "ramMemoryMb": "8192",
        "flopsTflops": "4.0",
        "llmStageCapacityMb": "2048",
        "llmMaxStageLayers": "4",
        "modelFamilies": "llm,onnxruntime",
    },
    "/NDNSF-DI/Tracer/provider/llm-4gb": {
        "gpuMemoryMb": "4096",
        "ramMemoryMb": "16384",
        "flopsTflops": "8.0",
        "llmStageCapacityMb": "4096",
        "llmMaxStageLayers": "8",
        "modelFamilies": "llm,onnxruntime",
    },
    "/NDNSF-DI/Tracer/provider/llm-8gb": {
        "gpuMemoryMb": "8192",
        "ramMemoryMb": "32768",
        "flopsTflops": "16.0",
        "llmStageCapacityMb": "8192",
        "llmMaxStageLayers": "16",
        "modelFamilies": "llm,onnxruntime",
    },
    "/NDNSF-DI/Tracer/provider/backbone": {
        "gpuMemoryMb": "12288",
        "ramMemoryMb": "32768",
        "flopsTflops": "18.0",
        "llmStageCapacityMb": "9216",
        "llmMaxStageLayers": "18",
        "modelFamilies": "llm,onnxruntime",
    },
    "/NDNSF-DI/Tracer/provider/head0": {
        "gpuMemoryMb": "8192",
        "ramMemoryMb": "24576",
        "flopsTflops": "12.0",
        "llmStageCapacityMb": "6144",
        "llmMaxStageLayers": "12",
        "modelFamilies": "llm,onnxruntime",
    },
    "/NDNSF-DI/Tracer/provider/head1": {
        "gpuMemoryMb": "8192",
        "ramMemoryMb": "24576",
        "flopsTflops": "10.0",
        "llmStageCapacityMb": "6144",
        "llmMaxStageLayers": "12",
        "modelFamilies": "llm,onnxruntime",
    },
    "/NDNSF-DI/Tracer/provider/merge": {
        "gpuMemoryMb": "6144",
        "ramMemoryMb": "16384",
        "flopsTflops": "7.5",
        "llmStageCapacityMb": "4096",
        "llmMaxStageLayers": "8",
        "modelFamilies": "llm,onnxruntime",
    },
    "/NDNSF-DI/Tracer/provider/single": {
        "gpuMemoryMb": "24576",
        "ramMemoryMb": "65536",
        "flopsTflops": "24.0",
        "llmStageCapacityMb": "18432",
        "llmMaxStageLayers": "24",
        "modelFamilies": "llm,onnxruntime",
    },
}


def log(message: str) -> None:
    info(message + "\n")


def mini_status() -> str:
    try:
        import minindn  # noqa: F401
    except Exception as exc:  # pragma: no cover - diagnostic path
        return f"unavailable:{exc}"
    return "available-root" if os.geteuid() == 0 else "available-non-root"


def topology_nodes(path: Path) -> set[str]:
    nodes: set[str] = set()
    in_nodes = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line == "[nodes]":
            in_nodes = True
            continue
        if line.startswith("[") and line != "[nodes]":
            in_nodes = False
        if in_nodes and line and not line.startswith("#"):
            nodes.add(line.rstrip(":"))
    return nodes


def assignment_for(name: str) -> dict[str, tuple[str, str]]:
    if name == "default":
        return DEFAULT_ASSIGNMENT
    if name == "alternate":
        return ALTERNATE_ASSIGNMENT
    if name == "single-provider":
        return SINGLE_PROVIDER_ASSIGNMENT
    if name == "capacity-pool":
        return DEFAULT_ASSIGNMENT
    raise ValueError(f"unknown assignment: {name}")


def assignment_from_rows(rows: list[dict[str, str]]) -> dict[str, tuple[str, str]]:
    assignment: dict[str, tuple[str, str]] = {}
    for row in rows:
        role = row["role"]
        node = row["node"]
        provider = row["provider"]
        if not role or not node or not provider:
            raise ValueError(f"incomplete assignment row: {row}")
        assignment[role] = (node, provider)
    if not assignment:
        raise ValueError("assignment rows are empty")
    return assignment


def runtime_candidate_for_assignment(name: str) -> str:
    if name == "single-provider":
        return "single-provider-serial"
    return "shared-backbone-current"


def assignment_for_runtime_candidate(candidate: str) -> str:
    if candidate == "single-provider-serial":
        return "single-provider"
    if candidate == "shared-backbone-current":
        return "default"
    raise ValueError(f"unsupported runtime candidate for auto assignment: {candidate}")


def launch_rows_for_assignment(assignment_name: str,
                               assignment_rows: list[dict[str, str]]) -> list[dict[str, str]]:
    rows = [dict(row) for row in assignment_rows]
    if assignment_name == "capacity-pool" and not any(
        row.get("assignment") == CAPACITY_POOL_ALTERNATE_ASSIGNMENT
        for row in rows
    ):
        rows.extend(dict(row) for row in CAPACITY_POOL_EXTRA_PROVIDERS)
    return rows


def capacity_pool_candidate_rows(primary_rows: list[dict[str, str]]) -> list[dict[str, str]]:
    rows = [dict(row) for row in primary_rows]
    existing = {
        (row.get("role", ""), row.get("provider", ""))
        for row in rows
    }
    for row in CAPACITY_POOL_EXTRA_PROVIDERS:
        key = (row["role"], row["provider"])
        if key not in existing:
            rows.append(dict(row))
            existing.add(key)
    return rows


def llm_provider_resource_profile(provider: str) -> dict[str, str]:
    return dict(LLM_PROVIDER_RESOURCE_PROFILES.get(provider, {}))


def llm_provider_resource_env(provider: str) -> dict[str, str]:
    profile = llm_provider_resource_profile(provider)
    if not profile:
        return {}
    return {
        "NDNSF_DI_PROVIDER_GPU_MEMORY_MB": profile["gpuMemoryMb"],
        "NDNSF_DI_PROVIDER_RAM_MEMORY_MB": profile["ramMemoryMb"],
        "NDNSF_DI_PROVIDER_FLOPS_TFLOPS": profile["flopsTflops"],
        "NDNSF_DI_PROVIDER_LLM_STAGE_CAPACITY_MB": profile["llmStageCapacityMb"],
        "NDNSF_DI_PROVIDER_LLM_MAX_STAGE_LAYERS": profile["llmMaxStageLayers"],
        "NDNSF_DI_PROVIDER_MODEL_FAMILIES": profile["modelFamilies"],
    }


def provider_admission_env(args) -> dict[str, str]:
    env: dict[str, str] = {}
    if args.provider_admission_max_queue >= 0:
        env["NDNSF_DI_PROVIDER_ADMISSION_MAX_QUEUE"] = str(args.provider_admission_max_queue)
    if args.provider_admission_max_active_workers >= 0:
        env["NDNSF_DI_PROVIDER_ADMISSION_MAX_ACTIVE_WORKERS"] = str(
            args.provider_admission_max_active_workers)
    if args.provider_admission_min_free_memory_mb > 0:
        env["NDNSF_DI_PROVIDER_ADMISSION_MIN_FREE_MEMORY_MB"] = str(
            args.provider_admission_min_free_memory_mb)
    return env


def shell_join(items: list[str]) -> str:
    return " ".join(perf.shell_quote(str(item)) for item in items)


def safe_log_component(value: str) -> str:
    return value.strip("/").replace("/", "-").replace("+", "-").replace(",", "-")


def run_logged(name: str, command: list[str], logs_dir: Path, env: dict[str, str]) -> None:
    log_path = logs_dir / f"{name}.log"
    with log_path.open("wb") as output:
        output.write(("RUN " + shell_join(command) + "\n").encode("utf-8"))
        output.flush()
        subprocess.run(command, cwd=str(REPO), env=env, stdout=output,
                       stderr=subprocess.STDOUT, check=True)


def write_assignment_csv(path: Path,
                         assignment_name: str,
                         roles: list[str],
                         assignment: dict[str, tuple[str, str]]) -> list[dict[str, str]]:
    rows = []
    for role in roles:
        if role not in roles:
            raise RuntimeError(f"generated plan is missing required role: {role}")
        if role not in assignment:
            raise RuntimeError(f"assignment {assignment_name} is missing role: {role}")
        node, provider = assignment[role]
        rows.append({
            "assignment": assignment_name,
            "role": role,
            "provider": provider,
            "node": node,
            "service": SERVICE,
        })
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=[
            "assignment", "role", "provider", "node", "service",
        ])
        writer.writeheader()
        writer.writerows(rows)
    return rows


def write_assignment_csv_rows(path: Path,
                              rows: list[dict[str, str]]) -> list[dict[str, str]]:
    required = ["assignment", "role", "provider", "node", "service"]
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=required)
        writer.writeheader()
        writer.writerows([{key: row[key] for key in required} for row in rows])
    return rows


def write_provider_profiles_json(path: Path,
                                 assignment_name: str,
                                 assignment: dict[str, tuple[str, str]],
                                 role_execution_delay_ms: float) -> None:
    role_compute_ms = {
        "/Backbone": 4.0,
        "/Head/Shard/0": 2.5,
        "/Head/Shard/1": 2.5,
        "/Merge": 1.5,
    }
    payload = {
        "assignment": assignment_name,
        "roles": {
            role: {
                "provider": provider,
                "node": node,
                "computeScore": 1.0,
                "queueDepth": 0,
                "roleComputeMs": role_compute_ms[role] + role_execution_delay_ms,
                "baseRoleComputeMs": role_compute_ms[role],
                "roleExecutionDelayMs": role_execution_delay_ms,
            }
            for role, (node, provider) in assignment.items()
        },
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def write_runtime_hints_json(path: Path,
                             assignment_rows: list[dict[str, str]],
                             *,
                             role_execution_delay_ms: float,
                             provider_admission_max_active_workers: int,
                             provider_admission_max_queue: int) -> None:
    role_compute_ms = {
        "/Backbone": 4.0,
        "/Head/Shard/0": 2.5,
        "/Head/Shard/1": 2.5,
        "/Merge": 1.5,
    }
    provider_roles: dict[str, dict[str, object]] = {}
    providers: dict[str, dict[str, object]] = {}
    generated_at_ms = int(time.time() * 1000)
    active_workers = max(1, int(provider_admission_max_active_workers or 1))
    max_queue = max(0, int(provider_admission_max_queue or 0))
    for row in assignment_rows:
        role = row["role"]
        provider = row["provider"]
        assignment = row.get("assignment", "")
        is_alternate = assignment == CAPACITY_POOL_ALTERNATE_ASSIGNMENT
        duration_ms = role_compute_ms.get(role, 5.0) + role_execution_delay_ms
        ready_cost_ms = 20.0 if is_alternate else 5.0
        queue_wait_ms = 10.0 if is_alternate else 0.0
        residency = "DISK_RESIDENT" if is_alternate else "CPU_RESIDENT"
        hint = {
            "provider": provider,
            "role": role,
            "assignment": assignment,
            "estimatedDurationMs": round(max(1.0, duration_ms), 3),
            "readyCostMs": ready_cost_ms,
            "residency": residency,
            "fragmentDigest": f"sha256:native-tracer:{role.strip('/').replace('/', '-')}",
            "runtimeHint": {
                "estimatedQueueWaitMs": queue_wait_ms,
                "activeWorkerCount": 0,
                "maxActiveWorkers": active_workers,
                "readyQueueDepth": 0,
                "maxQueue": max_queue,
                "fragmentResidency": residency,
                "sampleAgeMs": 0,
            },
            "leaseOffers": [{
                "status": "GRANTED",
                "estimatedStartMs": 0,
                "ttlMs": 60000,
                "source": "harness-runtime-hint-snapshot",
            }],
        }
        provider_roles[f"{provider}|{role}"] = hint
        providers.setdefault(provider, {
            "provider": provider,
            "runtimeHint": {
                "estimatedQueueWaitMs": queue_wait_ms,
                "maxActiveWorkers": active_workers,
                "maxQueue": max_queue,
                "sampleAgeMs": 0,
            },
            "roles": {},
        })
        providers[provider]["roles"][role] = hint
    path.write_text(json.dumps({
        "schema": "ndnsf-di-runtime-hints-v1",
        "generatedAtMs": generated_at_ms,
        "source": "MiniNDN harness provider profile snapshot",
        "providerRoles": provider_roles,
        "providers": providers,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run_plan_tracer(policy_dir: Path,
                    out_dir: Path,
                    logs_dir: Path,
                    env: dict[str, str],
                    assignment_name: str,
                    runtime_candidate: str,
                    role_execution_delay_ms: float,
                    activation_pad_bytes: int,
                    workload_concurrency: int,
                    target_rps: float,
                    runtime_aware_user_planner: bool = False,
                    provider_network_matrix_json: str = "",
                    log_name: str = "plan-tracer") -> dict[str, object]:
    policy_dir = policy_dir.resolve()
    out_dir = out_dir.resolve()
    logs_dir = logs_dir.resolve()
    active_assignment = assignment_for(assignment_name)
    provider_profiles_json = (
        out_dir / "provider-profiles.json"
        if log_name == "plan-tracer" else
        out_dir / f"{log_name}-provider-profiles.json"
    )
    write_provider_profiles_json(provider_profiles_json,
                                 assignment_name,
                                 active_assignment,
                                 role_execution_delay_ms)
    summary_path = (
        out_dir / "policy-summary.json"
        if log_name == "plan-tracer" else
        out_dir / f"{log_name}-policy-summary.json"
    )
    command = [
        "python3", str(PLAN_TRACER),
        "--out", str(policy_dir),
        "--summary-json", str(summary_path),
        "--runtime-candidate", runtime_candidate,
        "--provider-profiles-json", str(provider_profiles_json),
        "--activation-pad-bytes", str(activation_pad_bytes),
        "--role-execution-delay-ms", str(role_execution_delay_ms),
        "--workload-concurrency", str(workload_concurrency),
        "--target-rps", str(target_rps),
    ]
    if runtime_aware_user_planner:
        command.append("--runtime-aware-user-planner")
    if provider_network_matrix_json:
        command.extend(["--provider-network-matrix-json",
                        str(Path(provider_network_matrix_json).resolve())])
    run_logged(log_name, command, logs_dir, env)
    return json.loads(summary_path.read_text(encoding="utf-8"))


def run_llm_proportional_bundle(policy_dir: Path,
                                out_dir: Path,
                                logs_dir: Path,
                                env: dict[str, str],
                                planner_mode: str,
                                assignment_label: str,
                                role_execution_delay_ms: float,
                                stage_execution_delay_scale: float,
                                target_rps: float,
                                provider_workers: int) -> dict[str, object]:
    policy_dir = policy_dir.resolve()
    out_dir = out_dir.resolve()
    logs_dir = logs_dir.resolve()
    summary_path = out_dir / "policy-summary.json"
    run_logged("generate-llm-proportional-bundle", [
        "python3", str(LLM_BUNDLE_GENERATOR),
        "--out", str(policy_dir),
        "--summary-json", str(summary_path),
        "--planner-mode", planner_mode,
        "--assignment-label", assignment_label,
        "--stage-execution-delay-ms", str(role_execution_delay_ms),
        "--stage-execution-delay-scale", str(stage_execution_delay_scale),
        "--target-rps", str(target_rps),
        "--provider-workers", str(provider_workers),
    ], logs_dir, env)
    return json.loads(summary_path.read_text(encoding="utf-8"))


def grouped_provider_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    grouped: dict[tuple[str, str], dict[str, str]] = {}
    for row in rows:
        key = (row["node"], row["provider"])
        if key not in grouped:
            grouped[key] = {
                "assignment": row["assignment"],
                "role": row["role"],
                "roles": row.get("roles", row["role"]),
                "provider": row["provider"],
                "node": row["node"],
                "service": row["service"],
            }
        else:
            grouped[key]["roles"] += "," + row.get("roles", row["role"])
            grouped[key]["role"] += "+" + row["role"]
    return list(grouped.values())


def provider_home_dir(ndn, row: dict[str, str]) -> Path:
    base_home = Path(ndn.net[row["node"]].params["params"]["homeDir"])
    return base_home.parent / (
        base_home.name + "-provider-" + safe_log_component(row["provider"]))


def assign_provider_homes(ndn, provider_rows: list[dict[str, str]]) -> None:
    for row in provider_rows:
        row["homeDir"] = str(provider_home_dir(ndn, row))


def load_plan_roles(plan_path: Path) -> list[str]:
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    service = next(item for item in plan["services"] if item["service"] == SERVICE)
    return list(service["roles"])


def validate_prerequisites(topology_file: Path = TOPO) -> None:
    missing = [
        str(path) for path in (
            topology_file,
            PLAN_TRACER,
            LLM_BUNDLE_GENERATOR,
            USER_DRIVER,
            PROVIDER_EXE,
            PLAN_SCHEMA_EXE,
            PLAN_MANIFEST_EXE,
            PROVIDER_SESSION_EXE,
        )
        if not path.exists()
    ]
    nodes = topology_nodes(topology_file) if topology_file.exists() else set()
    required_nodes = {"memphis", "ucla", "arizona", "wustl", "neu"}
    missing_nodes = sorted(required_nodes - nodes)
    if missing or missing_nodes:
        raise RuntimeError(
            f"native tracer MiniNDN prerequisites failed missingFiles={missing} "
            f"missingNodes={missing_nodes}")


def read_log_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def wait_for_log_patterns(paths: list[Path],
                          patterns: list[str],
                          timeout_s: int,
                          label: str) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if all(
            any(pattern in read_log_text(path) for path in paths)
            for pattern in patterns
        ):
            return
        time.sleep(0.2)
    observed = {str(path): read_log_text(path)[-2000:] for path in paths}
    raise RuntimeError(f"timed out waiting for {label}: {observed}")


def validate_local_timing_csv(path: Path,
                              assignment_rows: list[dict[str, str]]) -> dict[str, object]:
    expected = {row["role"]: row["provider"] for row in assignment_rows}
    with path.open(newline="", encoding="utf-8") as input_file:
        reader = csv.DictReader(input_file)
        required_columns = [
            "sessionId", "provider", "role", "inputBytes", "outputBytes",
            "prefetchMs", "executeMs", "publishMs", "endToEndMs", "status",
        ]
        if reader.fieldnames != required_columns:
            raise RuntimeError(
                f"unexpected local execution timing columns: {reader.fieldnames}")
        rows = list(reader)
    observed_roles = {row["role"] for row in rows}
    if observed_roles != set(expected):
        raise RuntimeError(
            f"local execution roles {sorted(observed_roles)} do not match "
            f"assignment roles {sorted(expected)}")
    for row in rows:
        if row["provider"] != expected[row["role"]]:
            raise RuntimeError(
                f"local execution provider mismatch for {row['role']}: "
                f"{row['provider']} != {expected[row['role']]}")
        if row["status"] != "ok":
            raise RuntimeError(f"local execution row did not finish ok: {row}")
        int(row["inputBytes"])
        int(row["outputBytes"])
        for column in ("prefetchMs", "executeMs", "publishMs", "endToEndMs"):
            float(row[column])
    return {
        "rows": len(rows),
        "roles": sorted(observed_roles),
    }


def run_local_execution_baseline(policy_dir: Path,
                                 out_dir: Path,
                                 logs_dir: Path,
                                 env: dict[str, str],
                                 assignment_name: str,
                                 assignment_rows: list[dict[str, str]],
                                 *,
                                 model_family: str = "yolo-onnx",
                                 model_format: str = "onnx",
                                 planner_kind: str = "yolo-detect-auto",
                                 assignment_csv: Optional[Path] = None) -> dict[str, object]:
    timing_csv = out_dir / "local-execution-timing.csv"
    run_logged("local-plan-schema-smoke", [
        str(PLAN_SCHEMA_EXE),
        str(policy_dir / "native-execution-plan.json"),
        SERVICE,
        model_family,
        model_format,
        planner_kind,
    ], logs_dir, env)
    manifest_args = [
        str(PLAN_MANIFEST_EXE),
        str(policy_dir / "native-execution-plan.json"),
        str(policy_dir / "service-manifest.json"),
        SERVICE,
        "--timing-csv",
        str(timing_csv),
        "--assignment",
        assignment_name,
    ]
    if assignment_csv is not None:
        manifest_args.extend(["--assignment-csv", str(assignment_csv)])
    run_logged("local-plan-manifest-smoke", manifest_args, logs_dir, env)
    timing = validate_local_timing_csv(timing_csv, assignment_rows)
    run_logged("local-provider-session-smoke", [
        str(PROVIDER_SESSION_EXE),
    ], logs_dir, env)
    return {
        "status": "executed",
        "mode": "in-memory-dependency-io",
        "timingCsv": str(timing_csv),
        "timingRows": timing["rows"],
        "roles": timing["roles"],
        "reason": (
            "C++ native plan/manifest execution baseline completed before "
            "full NDNSF network user request execution"
        ),
    }


def provider_check_command(row: dict[str, str],
                           policy_dir: Path,
                           deterministic_runner: bool = False) -> str:
    policy_dir = policy_dir.resolve()
    args = [
        str(PROVIDER_EXE),
        "--plan", str(policy_dir / "native-execution-plan.json"),
        "--manifest", str(policy_dir / "service-manifest.json"),
        "--service", SERVICE,
        "--provider", row["provider"],
        "--group", GROUP,
        "--controller", CONTROLLER,
        "--trust-schema", "examples/trust-schema.conf",
        "--roles", row.get("roles", row["role"]),
        "--workers", "1",
        "--check-only",
        "--wiring-check-only",
        "--no-serve-certificates",
    ]
    if deterministic_runner:
        args.append("--tracer-deterministic-runner")
    return f"cd {perf.shell_quote(str(TRACER_DIR))} && exec {shell_join(args)}"


def provider_serve_command(row: dict[str, str],
                           policy_dir: Path,
                           deterministic_runner: bool = False,
                           enable_admission_lease: bool = False,
                           require_execution_lease: bool = False) -> str:
    policy_dir = policy_dir.resolve()
    args = [
        str(PROVIDER_EXE),
        "--plan", str(policy_dir / "native-execution-plan.json"),
        "--manifest", str(policy_dir / "service-manifest.json"),
        "--service", SERVICE,
        "--provider", row["provider"],
        "--group", GROUP,
        "--controller", CONTROLLER,
        "--trust-schema", str(policy_dir / "trust-schema.conf"),
        "--roles", row.get("roles", row["role"]),
        "--workers", "1",
        "--handler-threads", "2",
        "--ack-threads", "2",
        "--serve",
    ]
    if deterministic_runner:
        args.append("--tracer-deterministic-runner")
    if enable_admission_lease:
        args.extend(["--enable-admission-lease", "--admission-lease-ttl-ms", "60000"])
    if require_execution_lease:
        args.append("--require-execution-lease")
    return f"cd {perf.shell_quote(str(TRACER_DIR))} && exec {shell_join(args)}"


def controller_command(policy_dir: Path, assignment_rows: list[dict[str, str]]) -> str:
    policy_dir = policy_dir.resolve()
    code = (
        "from ndnsf import ServiceController; "
        "ServiceController("
        f"controller_prefix={CONTROLLER!r}, "
        f"policy_file={str(policy_dir / 'controller.policies')!r}, "
        f"trust_schema={str(policy_dir / 'trust-schema.conf')!r}, "
        "bootstrap_identities=[], "
        "serve_certificates=True"
        ").run()"
    )
    return f"cd {perf.shell_quote(str(REPO))} && exec python3 -c {perf.shell_quote(code)}"


def user_driver_command(policy_dir: Path,
                        requests: int,
                        concurrency: int,
                        submission_spacing_ms: int,
                        assignment_csv: Optional[Path] = None,
                        timeout_ms: int = 60000,
                        overload_fast_fail_timeout_ms: int = 0,
                        target_rps: float = 0.0,
                        open_loop_duration_s: float = 0.0,
                        open_loop_driver_mode: str = "threaded",
                        burst_admission_providers: Optional[list[str]] = None,
                        runtime_aware_max_replans: int = 0,
                        runtime_aware_replan_reasons: str = "",
                        execution_leases: bool = False) -> str:
    policy_dir = policy_dir.resolve()
    if assignment_csv is not None:
        assignment_csv = assignment_csv.resolve()
    args = [
        "python3", str(USER_DRIVER),
        "--plan", str(policy_dir / "native-execution-plan.json"),
        "--service", SERVICE,
        "--group", GROUP,
        "--controller", CONTROLLER,
        "--user", USER,
        "--trust-schema", str(policy_dir / "trust-schema.conf"),
        "--ack-timeout-ms", "8000",
        "--timeout-ms", str(timeout_ms),
        "--permission-wait-ms", "8000",
        "--requests", str(requests),
        "--concurrency", str(concurrency),
        "--submission-spacing-ms", str(submission_spacing_ms),
    ]
    if overload_fast_fail_timeout_ms > 0:
        args.extend([
            "--overload-fast-fail-timeout-ms",
            str(overload_fast_fail_timeout_ms),
        ])
    if assignment_csv is not None:
        args.extend(["--assignment-csv", str(assignment_csv)])
    if open_loop_duration_s > 0.0:
        args.extend([
            "--target-rps", str(target_rps),
            "--open-loop-duration-s", str(open_loop_duration_s),
            "--open-loop-driver-mode", open_loop_driver_mode,
        ])
    if burst_admission_providers:
        args.extend([
            "--burst-admission-providers",
            ",".join(burst_admission_providers),
        ])
    if runtime_aware_max_replans > 0:
        args.extend(["--runtime-aware-max-replans", str(runtime_aware_max_replans)])
    if runtime_aware_replan_reasons:
        args.extend(["--runtime-aware-replan-reasons", runtime_aware_replan_reasons])
    if execution_leases:
        args.append("--execution-leases")
    return f"cd {perf.shell_quote(str(REPO))} && exec {shell_join(args)}"


def user_driver_wait_timeout_s(requests: int,
                               concurrency: int,
                               request_timeout_ms: int = 60000,
                               open_loop_duration_s: float = 0.0) -> int:
    waves = max(1, math.ceil(max(1, requests) / max(1, concurrency)))
    if open_loop_duration_s > 0.0:
        return max(90, int(math.ceil(open_loop_duration_s + (request_timeout_ms / 1000.0) + 45.0)))
    return max(90, int(math.ceil(((request_timeout_ms + 3000) / 1000.0) * waves + 35.0)))


def add_worker_user_policies(policy_path: Path, requests: int) -> None:
    if requests <= 1:
        return
    text = policy_path.read_text(encoding="utf-8")
    blocks = []
    for identity in user_worker_identities(requests):
        if f"for {identity}\n" in text:
            continue
        blocks.append(
            "    user-policy\n"
            "    {\n"
            f"        for {identity}\n"
            "        allow\n"
            "        {\n"
            f"            {SERVICE}\n"
            "        }\n"
            "    }\n"
        )
    if not blocks:
        return
    insert_at = text.rfind("\n}")
    if insert_at < 0:
        raise RuntimeError(f"could not locate user-policies closing brace in {policy_path}")
    policy_path.write_text(text[:insert_at] + "\n" + "\n".join(blocks) + text[insert_at:],
                           encoding="utf-8")


def add_execution_lease_policies(policy_path: Path) -> None:
    """Allow every NativeTracer user/provider to use the DI lease service."""

    text = policy_path.read_text(encoding="utf-8")
    marker = f"            {SERVICE}\n"
    addition = marker + f"            {EXECUTION_LEASE_SERVICE}\n"
    if EXECUTION_LEASE_SERVICE not in text:
        text = text.replace(marker, addition)
    policy_path.write_text(text, encoding="utf-8")


def parse_user_execution(log_path: Path) -> dict[str, object]:
    prefix = "NDNSF_DI_NATIVE_TRACER_USER_EXECUTION "
    for line in read_log_text(log_path).splitlines():
        if line.startswith(prefix):
            return json.loads(line[len(prefix):])
    raise RuntimeError(f"user driver log did not contain execution JSON: {log_path}")


def user_execution_measurement_fields(user_result: dict[str, object]) -> dict[str, object]:
    return {
        "measurementStartEpoch": user_result.get("measurementStartEpoch", 0.0),
        "measurementElapsedMs": user_result.get("measurementElapsedMs", 0.0),
        "maxScheduleSlipMs": user_result.get("maxScheduleSlipMs", 0.0),
    }


def observed_role_timings(log_paths: list[Path]) -> set[str]:
    roles: set[str] = set()
    for path in log_paths:
        for line in read_log_text(path).splitlines():
            if "NDNSF_DI_PROVIDER_HANDLER_TIMING" not in line or " event=end " not in line:
                continue
            for part in line.split():
                if part.startswith("role="):
                    roles.add(part[len("role="):])
    return roles


def parse_trace_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for part in line.split():
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        fields[key] = value.strip()
    return fields


def collect_provider_fragment_inventory(logs_dir: Path) -> dict[str, object]:
    event_counters: Counter[str] = Counter()
    residency_counters: Counter[str] = Counter()
    latest_by_provider_role: dict[str, dict[str, str]] = {}
    latest_by_fragment: dict[str, dict[str, str]] = {}
    scanned_logs = 0
    event_count = 0
    for path in sorted(logs_dir.glob("*.log")):
        scanned_logs += 1
        for line in read_log_text(path).splitlines():
            if "NDNSF_DI_FRAGMENT_INVENTORY" not in line:
                continue
            fields = parse_trace_fields(line)
            event = fields.get("event", "")
            role = fields.get("role", "")
            provider = fields.get("provider", "")
            digest = fields.get("fragmentDigest", "")
            residency = fields.get("residency", "")
            if not event or not role or not digest:
                continue
            event_count += 1
            event_counters[event] += 1
            if residency:
                residency_counters[residency] += 1
            record = {
                "event": event,
                "provider": provider,
                "role": role,
                "fragmentDigest": digest,
                "backend": fields.get("backend", ""),
                "path": fields.get("path", ""),
                "residency": residency,
                "epochMs": fields.get("epoch_ms", ""),
                "log": str(path),
            }
            latest_by_fragment[digest] = record
            if provider and provider != "unknown":
                latest_by_provider_role[f"{provider}|{role}"] = record
    return {
        "scannedLogs": scanned_logs,
        "eventCount": event_count,
        "eventCounters": dict(sorted(event_counters.items())),
        "residencyCounters": dict(sorted(residency_counters.items())),
        "latestByProviderRole": dict(sorted(latest_by_provider_role.items())),
        "latestByFragment": dict(sorted(latest_by_fragment.items())),
    }


def collect_dependency_object_counters(logs_dir: Path) -> dict[str, object]:
    scanned_logs = 0
    observed_line_count = 0
    event_count = 0
    parse_error_count = 0
    direction_counters: Counter[str] = Counter()
    status_counters: Counter[str] = Counter()
    dependency_fetch_payload_values: list[float] = []
    dependency_publish_payload_values: list[float] = []
    examples: list[dict[str, object]] = []
    parse_errors: list[dict[str, str]] = []

    for path in sorted(logs_dir.glob("*.log")):
        scanned_logs += 1
        for line_no, line in enumerate(read_log_text(path).splitlines(), start=1):
            if "NDNSF_DI_DEPENDENCY_OBJECT" not in line:
                continue
            observed_line_count += 1
            fields = parse_trace_fields(line)
            direction = fields.get("direction", "")
            status = fields.get("status", "")
            try:
                required = (
                    "session", "scope", "producer", "consumer",
                    "direction", "payload_bytes", "planned_name", "status",
                )
                if any(not fields.get(key, "") for key in required):
                    raise ValueError("dependency trace is missing required fields")
                if direction not in {"fetch", "publish"}:
                    raise ValueError(f"invalid dependency direction: {direction}")
                if status != "ok":
                    raise ValueError(f"invalid dependency status: {status}")
                payload_bytes = int(fields["payload_bytes"])
                if payload_bytes < 0:
                    raise ValueError("dependency payload_bytes must be non-negative")
            except (TypeError, ValueError) as exc:
                parse_error_count += 1
                if len(parse_errors) < 5:
                    parse_errors.append({
                        "log": str(path),
                        "line": str(line_no),
                        "error": str(exc),
                    })
                continue
            event_count += 1
            if direction:
                direction_counters[direction] += 1
                if direction == "fetch":
                    dependency_fetch_payload_values.append(float(payload_bytes))
                elif direction == "publish":
                    dependency_publish_payload_values.append(float(payload_bytes))
            if status:
                status_counters[status] += 1
            if len(examples) < 10:
                examples.append({
                    "session": fields.get("session", ""),
                    "scope": fields.get("scope", ""),
                    "direction": direction,
                    "payloadBytes": payload_bytes,
                    "status": status,
                    "plannedName": fields.get("planned_name", ""),
                    "log": str(path),
                })

    def by_mode(counter: Counter[str]) -> dict[str, int]:
        return {key: int(value) for key, value in sorted(counter.items())}

    return {
        "scannedLogs": scanned_logs,
        "observedLineCount": observed_line_count,
        "eventCount": event_count,
        "parseErrorCount": parse_error_count,
        "parseErrors": parse_errors,
        "directionCounters": by_mode(direction_counters),
        "statusCounters": by_mode(status_counters),
        "dependencyFetchPayloadBytes": metric_stats(dependency_fetch_payload_values),
        "dependencyPublishPayloadBytes": metric_stats(dependency_publish_payload_values),
        "examples": examples,
    }


def float_field(fields: dict[str, str], key: str, fallback: float = 0.0) -> float:
    try:
        return float(fields.get(key, fallback))
    except (TypeError, ValueError):
        return fallback


def int_field(fields: dict[str, str], key: str, fallback: int = 0) -> int:
    try:
        return int(float(fields.get(key, fallback)))
    except (TypeError, ValueError):
        return fallback


def metric_stats(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {
            "count": 0,
            "mean": 0.0,
            "stddev": 0.0,
            "min": 0.0,
            "max": 0.0,
        }
    return {
        "count": len(values),
        "mean": round(statistics.mean(values), 3),
        "stddev": round(statistics.stdev(values), 3) if len(values) > 1 else 0.0,
        "min": round(min(values), 3),
        "max": round(max(values), 3),
    }


def parse_ndnping_rtts(text: str) -> list[float]:
    return [
        float(match.group(1))
        for match in re.finditer(r"time=([0-9]+(?:\.[0-9]+)?)\s*ms", text)
    ]


def provider_metadata_from_rows(rows: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    metadata: dict[str, dict[str, str]] = {}
    for row in rows:
        roles = str(row.get("roles", row.get("role", ""))).split(",")
        for role in roles:
            role = role.strip()
            if not role:
                continue
            metadata[role] = {
                "providerLog": safe_log_component(row.get("provider", "")),
                "providerNode": row.get("node", ""),
                "providerPrefix": row.get("provider", ""),
            }
    return metadata


def load_native_dependency_edges(plan_path: Path) -> list[dict[str, object]]:
    if not plan_path.exists():
        return []
    payload = json.loads(plan_path.read_text(encoding="utf-8"))
    edges: list[dict[str, object]] = []
    for service in payload.get("services", []):
        if str(service.get("service", service.get("name", ""))) not in {"", SERVICE}:
            continue
        for dependency in service.get("dependencies", []):
            producers = [str(item) for item in dependency.get("producers", [])]
            consumers = [str(item) for item in dependency.get("consumers", [])]
            for producer in producers:
                for consumer in consumers:
                    edges.append({
                        "producerRole": producer,
                        "consumerRole": consumer,
                        "scope": str(dependency.get("keyScope", "")),
                        "expectedSegments": int(dependency.get("expectedSegments", 0) or 0),
                        "expectedBytes": int(dependency.get("expectedBytes", 0) or 0),
                    })
    return edges


def node_ndn_env(node, env: dict[str, str]) -> dict[str, str]:
    node_env = dict(env)
    home = Path(
        node_env.get("NDNSF_NATIVE_TRACER_SHARED_HOME") or
        node.params["params"]["homeDir"])
    ndn_dir = home / ".ndn"
    ndn_dir.mkdir(parents=True, exist_ok=True)
    client_conf = ndn_dir / "client.conf"
    client_conf.write_text(
        f"transport=unix:///run/nfd/{node.name}.sock\n",
        encoding="utf-8")
    node_env["HOME"] = str(home)
    node_env["NDN_CLIENT_CONF"] = str(client_conf)
    node_env["NDN_CLIENT_TRANSPORT"] = f"unix:///run/nfd/{node.name}.sock"
    return node_env


def write_dependency_edge_rtt_summary(ndn,
                                      out_dir: Path,
                                      logs_dir: Path,
                                      provider_rows: list[dict[str, str]],
                                      env: dict[str, str],
                                      procs,
                                      plan_path: Path) -> dict[str, object]:
    role_meta = provider_metadata_from_rows(provider_rows)
    rows: list[dict[str, object]] = []
    for index, edge in enumerate(load_native_dependency_edges(plan_path)):
        producer = role_meta.get(str(edge["producerRole"]))
        consumer = role_meta.get(str(edge["consumerRole"]))
        if not producer or not consumer:
            rows.append({
                **edge,
                "status": "skipped",
                "reason": "missing provider metadata for dependency edge",
            })
            continue
        ping_prefix = f"{producer['providerPrefix']}/ndnping"
        producer_log = safe_log_component(producer["providerLog"])
        consumer_log = safe_log_component(consumer["providerLog"])
        server_log = logs_dir / f"ndnpingserver-edge-{index}-{producer_log}.log"
        client_log = logs_dir / f"ndnping-edge-{index}-{consumer_log}-to-{producer_log}.log"
        server_file = server_log.open("wb")
        server_cmd = f"exec ndnpingserver {perf.shell_quote(ping_prefix)}"
        server = getPopen(ndn.net[producer["providerNode"]],
                          server_cmd,
                          envDict=node_ndn_env(ndn.net[producer["providerNode"]], env),
                          shell=True,
                          stdout=server_file,
                          stderr=subprocess.STDOUT)
        procs.append((server, server_file, server_log))
        time.sleep(0.3)
        client_file = client_log.open("wb")
        client_cmd = f"exec timeout 15s ndnping {perf.shell_quote(ping_prefix)} -c 5"
        client = getPopen(ndn.net[consumer["providerNode"]],
                          client_cmd,
                          envDict=node_ndn_env(ndn.net[consumer["providerNode"]], env),
                          shell=True,
                          stdout=client_file,
                          stderr=subprocess.STDOUT)
        try:
            client.wait(timeout=20)
        except subprocess.TimeoutExpired:
            client.terminate()
            client.wait(timeout=5)
        client_file.close()
        text = client_log.read_text(errors="replace") if client_log.exists() else ""
        rtts = parse_ndnping_rtts(text)
        rows.append({
            **edge,
            "status": "measured",
            "producerNode": producer["providerNode"],
            "producerLog": producer["providerLog"],
            "producerPrefix": producer["providerPrefix"],
            "consumerNode": consumer["providerNode"],
            "consumerLog": consumer["providerLog"],
            "consumerPrefix": consumer["providerPrefix"],
            "pingPrefix": ping_prefix,
            "returncode": client.returncode,
            "count": len(rtts),
            "rttsMs": rtts,
            "summaryMs": metric_stats(rtts),
            "clientLog": str(client_log),
            "serverLog": str(server_log),
        })
    summary = {
        "plan": str(plan_path),
        "count": len(rows),
        "rows": rows,
    }
    path = out_dir / "dependency-edge-ndnping-rtt-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
    return {
        "status": "written",
        "path": str(path),
        "rowCount": len(rows),
        "measuredCount": sum(1 for row in rows if row.get("status") == "measured"),
    }


def _first_non_empty(payload: dict[str, object], keys: list[str]) -> str:
    for key in keys:
        value = payload.get(key)
        if value is None:
            continue
        text = str(value).strip()
        if text:
            return text
    return ""


def _numeric_summary_value(payload: object, keys: list[str]) -> float:
    if not isinstance(payload, dict):
        return 0.0
    for key in keys:
        value = payload.get(key)
        try:
            number = float(value)
        except (TypeError, ValueError):
            continue
        if number > 0.0:
            return number
    return 0.0


def _median_rtt_from_edge(row: dict[str, object]) -> float:
    summary_ms = row.get("summaryMs", row.get("summary_ms", {}))
    rtt = _numeric_summary_value(summary_ms, ["p50", "median", "mean", "min"])
    if rtt > 0.0:
        return round(rtt, 3)
    samples = row.get("rttsMs", row.get("rtts_ms", []))
    if not isinstance(samples, list):
        return 0.0
    values: list[float] = []
    for item in samples:
        try:
            value = float(item)
        except (TypeError, ValueError):
            continue
        if value > 0.0:
            values.append(value)
    return round(statistics.median(values), 3) if values else 0.0


def _provider_pair_metric_from_dependency_edge(
    row: dict[str, object],
    *,
    fallback_bandwidth_mbps: float = 100.0,
) -> PeerNetworkMetric | None:
    """Convert dependency-edge ndnping evidence into a core peer metric.

    The ndnping client runs at the consumer and pings the producer prefix, but
    the dependency dataflow moves producer -> consumer. Store the metric in the
    dataflow direction so later DI transfer-cost scoring can consume it directly.
    """

    src_peer = _first_non_empty(row, [
        "producerPrefix", "producerProvider", "srcPeer", "src", "producer",
    ])
    dst_peer = _first_non_empty(row, [
        "consumerPrefix", "consumerProvider", "dstPeer", "dst", "consumer",
    ])
    rtt_ms = _median_rtt_from_edge(row)
    if not src_peer or not dst_peer or rtt_ms <= 0.0:
        return None
    try:
        samples = row.get("rttsMs", [])
        fallback_count = len(samples) if isinstance(samples, list) else 0
        count = int(row.get("count", fallback_count) or 0)
    except (TypeError, ValueError):
        count = 0
    summary_ms = row.get("summaryMs", {})
    jitter_ms = _numeric_summary_value(summary_ms, ["stddev", "stdev", "std"])
    expected_bytes = 0
    try:
        expected_bytes = int(row.get("expectedBytes", row.get("bytesSampled", 0)) or 0)
    except (TypeError, ValueError):
        expected_bytes = 0
    confidence = min(0.9, max(0.2, count / 5.0)) if count else 0.2
    return PeerNetworkMetric(
        src_peer=src_peer,
        dst_peer=dst_peer,
        rtt_ms=rtt_ms,
        bandwidth_mbps=max(0.001, float(fallback_bandwidth_mbps)),
        loss_rate=0.0,
        jitter_ms=round(jitter_ms, 3),
        bytes_sampled=max(0, expected_bytes),
        confidence=round(confidence, 3),
    )


def collect_provider_pair_telemetry(out_dir: Path) -> dict[str, object]:
    """Collect core provider-pair telemetry from available MiniNDN evidence."""

    source_path = out_dir / "dependency-edge-ndnping-rtt-stats.json"
    if not source_path.exists():
        return {
            "status": "not-available",
            "reason": "dependency edge ndnping summary was not generated",
            "source": str(source_path),
            "metricCount": 0,
            "metrics": [],
        }
    try:
        payload = json.loads(source_path.read_text(encoding="utf-8"))
    except Exception as exc:
        return {
            "status": "not-available",
            "reason": f"failed to read dependency edge ndnping summary: {exc}",
            "source": str(source_path),
            "metricCount": 0,
            "metrics": [],
        }
    rows = payload.get("rows", [])
    if not isinstance(rows, list):
        rows = []
    metrics: list[PeerNetworkMetric] = []
    skipped = 0
    for row in rows:
        if not isinstance(row, dict):
            skipped += 1
            continue
        metric = _provider_pair_metric_from_dependency_edge(row)
        if metric is None:
            skipped += 1
            continue
        metrics.append(metric)
    matrix = ProviderNetworkMatrix(metrics)
    return {
        "status": "collected" if metrics else "not-available",
        "reason": "" if metrics else "no valid provider-pair RTT samples",
        "source": str(source_path),
        "sourceKind": "dependency-edge-ndnping-rtt",
        "metricCount": len(metrics),
        "skippedRowCount": skipped,
        "metrics": [to_plain(metric) for metric in metrics],
        "matrix": {
            "metrics": [to_plain(metric) for metric in matrix.metrics.values()],
            "defaultRttMs": matrix.default_rtt_ms,
            "defaultBandwidthMbps": matrix.default_bandwidth_mbps,
            "staleAfterMs": matrix.stale_after_ms,
            "stalePenaltyMs": matrix.stale_penalty_ms,
            "unknownPenaltyMs": matrix.unknown_penalty_ms,
        },
    }


def _counter_from_mapping(payload: object, keys: list[str]) -> dict[str, int]:
    result = {key: 0 for key in keys}
    if isinstance(payload, dict):
        for key, value in payload.items():
            text = str(key)
            if text not in result:
                continue
            try:
                amount = int(value)
            except (TypeError, ValueError):
                amount = 0
            result[text] += amount
    return result


def _runtime_assignment_residencies(runtime_assignment: dict[str, object]) -> Counter[str]:
    counters: Counter[str] = Counter()
    selected_residencies = runtime_assignment.get("selectedResidencies", {})
    if isinstance(selected_residencies, dict):
        for value in selected_residencies.values():
            counters[str(value)] += 1
        if counters:
            return counters
    role_assignments = runtime_assignment.get("roleAssignments", {})
    if isinstance(role_assignments, dict):
        for item in role_assignments.values():
            if isinstance(item, dict) and item.get("residency"):
                counters[str(item["residency"])] += 1
        if counters:
            return counters
    assignment = runtime_assignment.get("assignment", {})
    if isinstance(assignment, dict):
        nested = assignment.get("role_assignments", assignment.get("roleAssignments", {}))
        if isinstance(nested, dict):
            for item in nested.values():
                if isinstance(item, dict) and item.get("residency"):
                    counters[str(item["residency"])] += 1
    return counters


def _max_stable_rps(summary: dict[str, object]) -> dict[str, object]:
    entries = summary.get("rpsSweep", summary.get("rateSweep", []))
    if not isinstance(entries, list):
        entries = []
    if not entries:
        user_execution = summary.get("userExecution", {})
        if isinstance(user_execution, dict):
            request_count = int(user_execution.get("requestCount", summary.get("requestCount", 0)) or 0)
            success_count = int(user_execution.get("successCount", 0) or 0)
            success_rate = round(success_count / request_count, 6) if request_count > 0 else 0.0
            candidate_rps = float(
                user_execution.get("targetRps", 0.0) or
                user_execution.get("offeredRps", 0.0) or
                user_execution.get("throughputRps", 0.0) or
                0.0
            )
            if summary.get("status") == "SUCCESS" and success_rate >= 0.99 and candidate_rps > 0:
                point = {
                    "targetRps": candidate_rps,
                    "successRate": success_rate,
                    "failureRate": 1.0 - success_rate,
                    "p95Ms": float(user_execution.get("p95Ms", 0.0) or 0.0),
                    "source": "currentRun",
                }
                return {
                    "maxStableRps": candidate_rps,
                    "stablePoint": point,
                    "evaluatedPoints": 1,
                }
    stable_entries: list[dict[str, object]] = []
    for item in entries:
        if not isinstance(item, dict):
            continue
        success_rate = float(item.get("successRate", 0.0) or 0.0)
        failure_rate = float(item.get("failureRate", 1.0 - success_rate) or 0.0)
        p95_ms = float(item.get("p95Ms", item.get("latencyP95Ms", 0.0)) or 0.0)
        stable = bool(item.get("stable", False)) or (
            str(item.get("status", "")).upper() == "SUCCESS" and
            success_rate >= 0.99 and
            failure_rate <= 0.01
        )
        if stable:
            stable_entries.append({
                "targetRps": float(item.get("targetRps", item.get("rps", 0.0)) or 0.0),
                "successRate": success_rate,
                "failureRate": failure_rate,
                "p95Ms": p95_ms,
            })
    best = max(stable_entries, key=lambda item: float(item["targetRps"])) if stable_entries else None
    return {
        "maxStableRps": 0.0 if best is None else best["targetRps"],
        "stablePoint": best or {},
        "evaluatedPoints": len(entries),
    }


def build_campaign_metrics(summary: dict[str, object]) -> dict[str, object]:
    user_execution = summary.get("userExecution", {})
    if not isinstance(user_execution, dict):
        user_execution = {}
    provider_utilization = summary.get("providerUtilization", {})
    if not isinstance(provider_utilization, dict):
        provider_utilization = {}
    runtime_assignment = summary.get("runtimeAwarePlanner", {})
    if not isinstance(runtime_assignment, dict):
        runtime_assignment = {}
    request_count = int(user_execution.get("requestCount", summary.get("requestCount", 0)) or 0)
    success_count = int(user_execution.get("successCount", 0) or 0)
    failure_count = int(user_execution.get("failureCount", max(0, request_count - success_count)) or 0)
    lease_counters = _counter_from_mapping(summary.get("leaseCounters", {}), [
        "granted", "rejected", "expired", "consumed",
    ])
    lease_counters.update({
        "negativeAckEvents": int(
            summary.get("failureBreakdown", {}).get("negativeAckEventCount", 0)
        ) if isinstance(summary.get("failureBreakdown", {}), dict) else 0,
        "rejections": failure_count,
    })
    residency_counters = Counter(_counter_from_mapping(summary.get("residencyCounters", {}), [
        "GPU_LOADED", "CPU_RESIDENT", "DISK_RESIDENT", "REPO_AVAILABLE", "MISSING",
    ]))
    residency_counters.update(_runtime_assignment_residencies(runtime_assignment))
    inventory = summary.get("providerFragmentInventory", {})
    if isinstance(inventory, dict):
        selected_hits = inventory.get("selectedResidencyCounters", {})
        if isinstance(selected_hits, dict):
            residency_counters.update({
                str(key): int(value)
                for key, value in selected_hits.items()
            })
    utilization_values = [
        float(item.get("estimatedUtilization", 0.0))
        for item in provider_utilization.values()
        if isinstance(item, dict)
    ]
    rps_summary = _max_stable_rps(summary)
    return {
        "status": summary.get("status", ""),
        "successRate": (
            round(success_count / request_count, 6)
            if request_count > 0 else
            (1.0 if summary.get("status") == "SUCCESS" else 0.0)
        ),
        "requestCount": request_count,
        "successCount": success_count,
        "failureCount": failure_count,
        "latencyMs": {
            "p50": float(user_execution.get("p50Ms", 0.0) or 0.0),
            "p95": float(user_execution.get("p95Ms", 0.0) or 0.0),
            "mean": float(user_execution.get("meanMs", 0.0) or 0.0),
            "makespan": float(user_execution.get("makespanMs", 0.0) or 0.0),
        },
        "utilization": {
            "providers": provider_utilization,
            "meanEstimatedUtilization": (
                round(statistics.mean(utilization_values), 6)
                if utilization_values else 0.0
            ),
        },
        "leaseCounters": lease_counters,
        "residencyCounters": dict(sorted(residency_counters.items())),
        "observedResidencyCounters": (
            dict(sorted(inventory.get("residencyCounters", {}).items()))
            if isinstance(inventory, dict) and isinstance(inventory.get("residencyCounters", {}), dict)
            else {}
        ),
        "providerFragmentInventory": inventory if isinstance(inventory, dict) else {},
        "edgeCostSummary": runtime_assignment.get("edgeCostSummary", {}),
        "nodeCostSummary": runtime_assignment.get("nodeCostSummary", {}),
        "replanCount": int(user_execution.get("replanCount", 0) or 0),
        "rpsSweep": rps_summary,
        "maxStableRps": rps_summary["maxStableRps"],
    }


def write_planner_metrics(out_dir: Path, summary: dict[str, object]) -> dict[str, str]:
    metrics = build_campaign_metrics(summary)
    json_path = out_dir / "planner-metrics.json"
    csv_path = out_dir / "planner-metrics.csv"
    json_path.write_text(json.dumps(metrics, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    with csv_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=[
            "status", "requestCount", "successCount", "failureCount",
            "successRate", "p50Ms", "p95Ms", "meanMs", "makespanMs",
            "meanEstimatedUtilization", "replanCount", "maxStableRps",
        ])
        writer.writeheader()
        writer.writerow({
            "status": metrics["status"],
            "requestCount": metrics["requestCount"],
            "successCount": metrics["successCount"],
            "failureCount": metrics["failureCount"],
            "successRate": metrics["successRate"],
            "p50Ms": metrics["latencyMs"]["p50"],
            "p95Ms": metrics["latencyMs"]["p95"],
            "meanMs": metrics["latencyMs"]["mean"],
            "makespanMs": metrics["latencyMs"]["makespan"],
            "meanEstimatedUtilization": metrics["utilization"]["meanEstimatedUtilization"],
            "replanCount": metrics["replanCount"],
            "maxStableRps": metrics["maxStableRps"],
        })
    return {"json": str(json_path), "csv": str(csv_path)}


def summarize_provider_metrics(provider_log_rows: list[dict[str, object]]) -> dict[str, object]:
    providers: dict[str, dict[str, object]] = {}
    for row in provider_log_rows:
        provider = str(row["provider"])
        path = Path(str(row["log"]))
        item = providers.setdefault(provider, {
            "provider": provider,
            "node": row.get("node", ""),
            "configuredRoles": set(),
            "log": str(path),
            "sessions": set(),
            "roles": set(),
            "roleEventCount": 0,
            "queueWaitMsValues": [],
            "inputFetchWaitMsValues": [],
            "runnerPublishMsValues": [],
            "handlerMsValues": [],
            "totalMsValues": [],
            "firstWorkerStartEpochMs": None,
            "lastEndEpochMs": None,
            "workerCount": 1,
            "maxActiveWorkers": 0,
            "maxReadyQueue": 0,
            "maxWaitingInputs": 0,
            "maxPendingWork": 0,
            "inputFetchEvents": 0,
            "outputPublishEvents": 0,
            "inputBytes": 0,
            "outputBytes": 0,
        })
        item["configuredRoles"].update(str(row.get("role", "")).split(","))
        for line in read_log_text(path).splitlines():
            if "NDNSF_DI_PROVIDER_HANDLER_TIMING" in line and " event=end " in line:
                fields = parse_trace_fields(line)
                item["roleEventCount"] += 1
                if fields.get("session"):
                    item["sessions"].add(fields["session"])
                if fields.get("role"):
                    item["roles"].add(fields["role"])
                item["queueWaitMsValues"].append(float_field(fields, "queue_wait_ms"))
                item["inputFetchWaitMsValues"].append(float_field(fields, "input_fetch_wait_ms"))
                item["runnerPublishMsValues"].append(float_field(fields, "runner_publish_ms"))
                item["handlerMsValues"].append(float_field(fields, "handler_ms"))
                item["totalMsValues"].append(float_field(fields, "total_ms"))
                worker_start = int_field(fields, "worker_start_epoch_ms", 0)
                end_epoch = int_field(fields, "end_epoch_ms", 0)
                if worker_start > 0:
                    current = item["firstWorkerStartEpochMs"]
                    item["firstWorkerStartEpochMs"] = worker_start if current is None else min(current, worker_start)
                if end_epoch > 0:
                    current = item["lastEndEpochMs"]
                    item["lastEndEpochMs"] = end_epoch if current is None else max(current, end_epoch)
            elif "NDNSF_DI_PROVIDER_CAPACITY" in line:
                fields = parse_trace_fields(line)
                item["workerCount"] = max(int(item["workerCount"]), int_field(fields, "workers", 1))
                item["maxActiveWorkers"] = max(int(item["maxActiveWorkers"]), int_field(fields, "active_workers"))
                item["maxReadyQueue"] = max(int(item["maxReadyQueue"]), int_field(fields, "ready_queue"))
                item["maxWaitingInputs"] = max(int(item["maxWaitingInputs"]), int_field(fields, "waiting_inputs"))
                item["maxPendingWork"] = max(int(item["maxPendingWork"]), int_field(fields, "pending_work"))
            elif "NDNSF_DI_DEPENDENCY_INPUT_TIMING" in line:
                fields = parse_trace_fields(line)
                item["inputFetchEvents"] += 1
                item["inputBytes"] += int_field(fields, "bytes")
            elif "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING" in line:
                fields = parse_trace_fields(line)
                item["outputPublishEvents"] += 1
                item["outputBytes"] += int_field(fields, "bytes")

    result: dict[str, object] = {}
    for provider, item in sorted(providers.items()):
        handler_values = list(item["handlerMsValues"])
        busy_ms = sum(handler_values)
        first_start = item["firstWorkerStartEpochMs"]
        last_end = item["lastEndEpochMs"]
        window_ms = max(0.0, float(last_end - first_start)) if first_start and last_end else 0.0
        worker_count = max(1, int(item["workerCount"]))
        utilization = busy_ms / (window_ms * worker_count) if window_ms > 0 else 0.0
        result[provider] = {
            "provider": provider,
            "node": item["node"],
            "configuredRoles": sorted(role for role in item["configuredRoles"] if role),
            "observedRoles": sorted(item["roles"]),
            "uniqueSessionCount": len(item["sessions"]),
            "roleEventCount": item["roleEventCount"],
            "workerCount": worker_count,
            "observedWindowMs": round(window_ms, 3),
            "busyHandlerMs": round(busy_ms, 3),
            "estimatedUtilization": round(utilization, 6),
            "queueWaitMs": metric_stats(item["queueWaitMsValues"]),
            "inputFetchWaitMs": metric_stats(item["inputFetchWaitMsValues"]),
            "runnerPublishMs": metric_stats(item["runnerPublishMsValues"]),
            "handlerMs": metric_stats(item["handlerMsValues"]),
            "totalMs": metric_stats(item["totalMsValues"]),
            "capacityMax": {
                "activeWorkers": item["maxActiveWorkers"],
                "readyQueue": item["maxReadyQueue"],
                "waitingInputs": item["maxWaitingInputs"],
                "pendingWork": item["maxPendingWork"],
            },
            "dependencyIo": {
                "inputFetchEvents": item["inputFetchEvents"],
                "outputPublishEvents": item["outputPublishEvents"],
                "inputBytes": item["inputBytes"],
                "outputBytes": item["outputBytes"],
            },
            "log": item["log"],
        }
    return result


def node_client_conf(home: Path, node_name: str) -> Path:
    ndn_dir = home / ".ndn"
    ndn_dir.mkdir(parents=True, exist_ok=True)
    client_conf = ndn_dir / "client.conf"
    client_conf.write_text(
        f"transport=unix:///run/nfd/{node_name}.sock\n",
        encoding="utf-8")
    return client_conf


def run_identity_command(command: str, log_path: Path) -> None:
    with log_path.open("ab") as output:
        output.write(("RUN " + command + "\n").encode("utf-8"))
        output.flush()
        subprocess.run(command,
                       shell=True,
                       cwd=str(REPO),
                       stdout=output,
                       stderr=subprocess.STDOUT,
                       check=True)


def setup_full_network_identities(ndn,
                                  assignment_rows: list[dict[str, str]],
                                  out_dir: Path,
                                  logs_dir: Path,
                                  extra_user_identities: Optional[list[str]] = None) -> None:
    home_specs: list[dict[str, object]] = [
        {
            "key": "node:memphis",
            "node": "memphis",
            "home": Path(ndn.net["memphis"].params["params"]["homeDir"]),
            "identities": [CONTROLLER, USER, *(extra_user_identities or [])],
            "authority": True,
        },
    ]
    for row in assignment_rows:
        home = Path(row.get("homeDir", ndn.net[row["node"]].params["params"]["homeDir"]))
        home_specs.append({
            "key": f"provider:{row['node']}:{row['provider']}",
            "node": row["node"],
            "home": home,
            "identities": [row["provider"]],
            "authority": False,
        })

    certs: list[Path] = []
    safebags: dict[str, Path] = {}
    log_path = logs_dir / "identity-bootstrap.log"
    homes: dict[str, tuple[Path, Path]] = {}
    for spec in home_specs:
        key = str(spec["key"])
        node_name = str(spec["node"])
        home = Path(str(spec["home"]))
        if bool(spec.get("authority")):
            subprocess.run(["rm", "-rf", str(home / ".ndn")], check=False)
        else:
            subprocess.run(["rm", "-rf", str(home)], check=False)
        client_conf = node_client_conf(home, node_name)
        homes[key] = (home, client_conf)

    authority_home, authority_client_conf = homes["node:memphis"]
    all_identities = list(dict.fromkeys(
        [CONTROLLER, USER, *(extra_user_identities or [])] +
        [row["provider"] for row in assignment_rows]
    ))
    for identity in all_identities:
        label = identity.strip("/").replace("/", "_")
        cert_path = out_dir / f"identity-authority-{label}.cert"
        safebag_path = out_dir / f"identity-authority-{label}.safebag"
        command = (
            f"HOME={perf.shell_quote(str(authority_home))} "
            f"NDN_CLIENT_CONF={perf.shell_quote(str(authority_client_conf))} "
            f"ndnsec key-gen -t r {perf.shell_quote(identity)} > "
            f"{perf.shell_quote(str(cert_path))}"
        )
        run_identity_command(command, log_path)
        command = (
            f"HOME={perf.shell_quote(str(authority_home))} "
            f"NDN_CLIENT_CONF={perf.shell_quote(str(authority_client_conf))} "
            f"ndnsec export -i -P {perf.shell_quote(IDENTITY_SAFEBAG_PASSPHRASE)} "
            f"-o {perf.shell_quote(str(safebag_path))} "
            f"{perf.shell_quote(identity)}"
        )
        run_identity_command(command, log_path)
        certs.append(cert_path)
        safebags[identity] = safebag_path

    for spec in home_specs:
        if bool(spec.get("authority")):
            continue
        home, client_conf = homes[str(spec["key"])]
        for identity in spec["identities"]:
            command = (
                f"HOME={perf.shell_quote(str(home))} "
                f"NDN_CLIENT_CONF={perf.shell_quote(str(client_conf))} "
                f"ndnsec import -P {perf.shell_quote(IDENTITY_SAFEBAG_PASSPHRASE)} "
                f"{perf.shell_quote(str(safebags[identity]))}"
            )
            run_identity_command(command, log_path)

    for _key, (home, client_conf) in homes.items():
        for cert_path in certs:
            command = (
                f"HOME={perf.shell_quote(str(home))} "
                f"NDN_CLIENT_CONF={perf.shell_quote(str(client_conf))} "
                f"ndnsec cert-install -f {perf.shell_quote(str(cert_path))} "
                ">/dev/null 2>&1 || true"
            )
            run_identity_command(command, log_path)


def start_node_command(node,
                       name: str,
                       command: str,
                       logs_dir: Path,
                       env: dict[str, str],
                       procs,
                       home_override: Optional[Path] = None,
                       env_overrides: Optional[dict[str, str]] = None):
    path = logs_dir / f"{name}.log"
    output = path.open("wb")
    output.write(("RUN " + command + "\n").encode("utf-8"))
    output.flush()
    node_env = dict(env)
    home = Path(home_override) if home_override is not None else Path(
        node_env.get("NDNSF_NATIVE_TRACER_SHARED_HOME") or
        node.params["params"]["homeDir"])
    ndn_dir = home / ".ndn"
    ndn_dir.mkdir(parents=True, exist_ok=True)
    client_conf = ndn_dir / "client.conf"
    client_conf.write_text(
        f"transport=unix:///run/nfd/{node.name}.sock\n",
        encoding="utf-8")
    node_env["HOME"] = str(home)
    node_env["NDN_CLIENT_CONF"] = str(client_conf)
    node_env["NDN_CLIENT_TRANSPORT"] = f"unix:///run/nfd/{node.name}.sock"
    if env_overrides:
        node_env.update(env_overrides)
    proc = getPopen(node, command, envDict=node_env, shell=True,
                    stdout=output, stderr=subprocess.STDOUT)
    procs.append((proc, output, path))
    return proc, path


def stop_processes(procs) -> None:
    for proc, output, _ in reversed(procs):
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
        try:
            output.close()
        except Exception:
            pass


def wait_provider_checks(procs, timeout_s: int = 45) -> list[dict[str, object]]:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if all(proc.poll() is not None for proc, _, _ in procs):
            break
        time.sleep(0.2)
    results = []
    for proc, _, path in procs:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=3)
        results.append({
            "log": str(path),
            "returncode": proc.returncode,
            "status": "passed" if proc.returncode == 0 else "failed",
        })
    return results


def collect_negative_ack_reason_counters(logs_dir: Path) -> dict[str, object]:
    """Collect stable negative-ACK reason counters from NativeTracer logs."""

    user_reasons: Counter[str] = Counter()
    provider_reasons: Counter[str] = Counter()
    payload_reasons: Counter[str] = Counter()
    scanned_logs = 0
    for path in sorted(logs_dir.glob("*.log")):
        scanned_logs += 1
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        for match in NEGATIVE_ACK_RECORDED_RE.finditer(text):
            reason = match.group(1).strip()
            if reason:
                user_reasons[reason] += 1
        for match in NATIVE_ACK_DECISION_RE.finditer(text):
            reason = match.group(1).strip()
            if reason:
                provider_reasons[reason] += 1
        for match in NEGATIVE_ACK_PAYLOAD_RE.finditer(text):
            reason = match.group(1).strip()
            if reason:
                payload_reasons[reason] += 1
    return {
        "userRecorded": dict(sorted(user_reasons.items())),
        "providerDecisions": dict(sorted(provider_reasons.items())),
        "payloadReasons": dict(sorted(payload_reasons.items())),
        "scannedLogs": scanned_logs,
    }


def collect_admission_lease_counters(logs_dir: Path) -> dict[str, object]:
    counters: Counter[str] = Counter({
        "granted": 0,
        "rejected": 0,
        "expired": 0,
        "consumed": 0,
    })
    reasons: Counter[str] = Counter()
    scanned_logs = 0
    for path in sorted(logs_dir.glob("*.log")):
        scanned_logs += 1
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        for line in text.splitlines():
            if "NDNSF_DI_NATIVE_PROVIDER_ADMISSION_LEASE_GRANTED" in line:
                counters["granted"] += 1
            elif "NDNSF_ADMISSION_LEASE_ACCEPTED" in line:
                counters["consumed"] += 1
            elif "NDNSF_ADMISSION_LEASE_REJECTED" in line:
                counters["rejected"] += 1
                fields = parse_trace_fields(line)
                reason = fields.get("reason", "")
                if reason:
                    reasons[reason] += 1
                    if reason == "LEASE_EXPIRED":
                        counters["expired"] += 1
                    elif reason == "LEASE_ALREADY_CONSUMED":
                        counters["alreadyConsumed"] += 1
                    elif reason == "LEASE_NOT_FOUND":
                        counters["notFound"] += 1
    return {
        **dict(sorted(counters.items())),
        "reasons": dict(sorted(reasons.items())),
        "scannedLogs": scanned_logs,
    }


def collect_provider_ack_runtime_hints(logs_dir: Path) -> dict[str, object]:
    providers: dict[str, dict[str, object]] = {}
    compatibility_counters = AckCompatibilityCounters()
    scanned_logs = 0
    event_count = 0
    parse_error_count = 0
    parse_errors: list[dict[str, str]] = []
    for path in sorted(logs_dir.glob("*.log")):
        scanned_logs += 1
        text = read_log_text(path)
        for line_no, line in enumerate(text.splitlines(), start=1):
            if "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION" not in line:
                continue
            fields = parse_trace_fields(line)
            payload_text = fields.get("payload", "").strip('"')
            try:
                decoded = decode_provider_capability_ack(
                    payload_text,
                    counters=compatibility_counters,
                    provider_name=fields.get("provider", ""),
                    service_name=SERVICE,
                )
            except Exception as exc:
                parse_error_count += 1
                if len(parse_errors) < 5:
                    parse_errors.append({
                        "log": str(path),
                        "line": str(line_no),
                        "error": str(exc),
                    })
                continue
            hint = decoded.hint
            payload_fields = dict(hint.service_payload)
            if hint.runtime_hint is not None:
                payload_fields["queue"] = hint.runtime_hint.queue_length
                payload_fields["activeWorkers"] = hint.runtime_hint.active_work_count
            payload_fields.setdefault("negativeAckReason", hint.reason_code)
            provider = fields.get("provider", "") or hint.provider_name
            if not provider:
                continue
            event_count += 1
            item = providers.setdefault(provider, {
                "provider": provider,
                "ackEvents": 0,
                "successfulAckEvents": 0,
                "negativeAckEvents": 0,
                "maxQueue": 0,
                "maxReadyQueue": 0,
                "maxWaitingInputs": 0,
                "maxActiveWorkers": 0,
                "maxWorkers": 0,
                "maxIdleWorkers": 0,
                "latest": {},
            })
            status = fields.get("status", "")
            item["ackEvents"] = int(item["ackEvents"]) + 1
            if status == "1":
                item["successfulAckEvents"] = int(item["successfulAckEvents"]) + 1
            else:
                item["negativeAckEvents"] = int(item["negativeAckEvents"]) + 1
            latest = {
                "status": status,
                "roles": payload_fields.get("roles", ""),
                "queue": int_field(payload_fields, "queue", 0),
                "readyQueue": int_field(payload_fields, "readyQueue", 0),
                "waitingInputs": int_field(payload_fields, "waitingInputs", 0),
                "activeWorkers": int_field(payload_fields, "activeWorkers", 0),
                "workers": int_field(payload_fields, "workers", 0),
                "idleWorkers": int_field(payload_fields, "idleWorkers", 0),
                "runtimeStatus": payload_fields.get("runtimeStatus", ""),
                "negativeAckReason": payload_fields.get("negativeAckReason", ""),
                "leaseId": payload_fields.get("leaseId", ""),
                "leaseExpiresAtMs": payload_fields.get("leaseExpiresAtMs", ""),
                "log": str(path),
            }
            for payload_key, output_key in {
                "queue": "maxQueue",
                "readyQueue": "maxReadyQueue",
                "waitingInputs": "maxWaitingInputs",
                "activeWorkers": "maxActiveWorkers",
                "workers": "maxWorkers",
                "idleWorkers": "maxIdleWorkers",
            }.items():
                item[output_key] = max(
                    int(item[output_key]),
                    int_field(payload_fields, payload_key, 0),
                )
            item["latest"] = latest
    return {
        "scannedLogs": scanned_logs,
        "eventCount": event_count,
        "parseErrorCount": parse_error_count,
        "parseErrors": parse_errors,
        "ackCompatibilityCounters": compatibility_counters.snapshot(),
        "providers": dict(sorted(providers.items())),
    }


def collect_core_envelope_summary(logs_dir: Path) -> dict[str, object]:
    """Summarize reusable NDNSF core envelopes visible in provider ACK logs."""
    envelope_counts: Counter[str] = Counter()
    provider_readiness: Counter[str] = Counter()
    reason_codes: Counter[str] = Counter()
    service_payload_schemas: Counter[str] = Counter()
    operation_states: Counter[str] = Counter()
    operation_names: Counter[str] = Counter()
    latest_providers: dict[str, dict[str, object]] = {}
    scanned_logs = 0
    event_count = 0
    parse_errors: list[dict[str, str]] = []
    compatibility_counters = AckCompatibilityCounters()
    for path in sorted(logs_dir.glob("*.log")):
        scanned_logs += 1
        for line_no, line in enumerate(read_log_text(path).splitlines(), start=1):
            if "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION" not in line:
                continue
            event_count += 1
            fields = parse_trace_fields(line)
            payload_text = fields.get("payload", "").strip('"')
            payload_fields = parse_ack_metadata(payload_text)
            if "providerCapabilityHint" in payload_fields:
                try:
                    hint = decode_provider_capability_ack(
                        payload_text,
                        counters=compatibility_counters,
                        provider_name=fields.get("provider", ""),
                        service_name=SERVICE,
                    ).hint
                    envelope_counts["providerCapabilityHint"] += 1
                    provider_readiness["ready" if hint.ready else "notReady"] += 1
                    if hint.reason_code:
                        reason_codes[hint.reason_code] += 1
                    if hint.service_payload_schema:
                        service_payload_schemas[hint.service_payload_schema] += 1
                    runtime_hint = hint.runtime_hint
                    provider_key = fields.get("provider", "") or hint.provider_name
                    latest_providers[provider_key] = {
                        "provider": hint.provider_name,
                        "service": hint.service_name,
                        "ready": hint.ready,
                        "reasonCode": hint.reason_code,
                        "message": hint.message,
                        "servicePayloadSchema": hint.service_payload_schema,
                        "queueLength": runtime_hint.queue_length if runtime_hint else 0,
                        "activeWorkCount": runtime_hint.active_work_count if runtime_hint else 0,
                        "leaseOfferCount": len(hint.lease_offers),
                        "log": str(path),
                    }
                    if hint.operation_status is not None:
                        operation = hint.operation_status
                        envelope_counts["serviceOperationStatus"] += 1
                        operation_states[operation.state.value] += 1
                        operation_names[operation.operation] += 1
                except Exception as exc:
                    envelope_counts["providerCapabilityHintParseErrors"] += 1
                    if len(parse_errors) < 5:
                        parse_errors.append({
                            "log": str(path),
                            "line": str(line_no),
                            "error": str(exc),
                        })
            if "operationStatus" in payload_fields:
                try:
                    operation = ServiceOperationStatus.from_dict(
                        dict(payload_fields["operationStatus"]))
                    envelope_counts["serviceOperationStatus"] += 1
                    operation_states[operation.state.value] += 1
                    operation_names[operation.operation] += 1
                except Exception as exc:
                    envelope_counts["serviceOperationStatusParseErrors"] += 1
                    if len(parse_errors) < 5:
                        parse_errors.append({
                            "log": str(path),
                            "line": str(line_no),
                            "error": str(exc),
                        })
    return {
        "scannedLogs": scanned_logs,
        "eventCount": event_count,
        "envelopeCounts": dict(sorted(envelope_counts.items())),
        "providerReadiness": dict(sorted(provider_readiness.items())),
        "reasonCodes": dict(sorted(reason_codes.items())),
        "servicePayloadSchemas": dict(sorted(service_payload_schemas.items())),
        "operationStates": dict(sorted(operation_states.items())),
        "operations": dict(sorted(operation_names.items())),
        "ackCompatibilityCounters": compatibility_counters.snapshot(),
        "latestProviders": dict(sorted(latest_providers.items())),
        "parseErrors": parse_errors,
    }


def build_failure_breakdown(user_result: dict[str, object],
                            negative_ack: dict[str, object]) -> dict[str, object]:
    requests = user_result.get("requests", [])
    if not isinstance(requests, list):
        requests = []
    failed_requests = [
        item for item in requests
        if isinstance(item, dict) and item.get("status") != "executed"
    ]
    timeout_count = sum(
        1 for item in failed_requests
        if str(item.get("error", "")).startswith("timeout:")
    )
    user_recorded = negative_ack.get("userRecorded", {})
    provider_decisions = negative_ack.get("providerDecisions", {})
    payload_reasons = negative_ack.get("payloadReasons", {})
    negative_ack_count = (
        sum(int(value) for value in user_recorded.values())
        if isinstance(user_recorded, dict) else 0
    )
    return {
        "requestCount": int(user_result.get("requestCount", len(requests)) or 0),
        "successCount": int(user_result.get("successCount", 0) or 0),
        "failureCount": int(user_result.get("failureCount", len(failed_requests)) or 0),
        "timeoutCount": timeout_count,
        "otherFailureCount": max(0, len(failed_requests) - timeout_count),
        "negativeAckEventCount": negative_ack_count,
        "negativeAckReasons": user_recorded if isinstance(user_recorded, dict) else {},
        "providerNegativeAckReasons": (
            provider_decisions if isinstance(provider_decisions, dict) else {}
        ),
        "payloadNegativeAckReasons": payload_reasons if isinstance(payload_reasons, dict) else {},
    }


def write_summary(out_dir: Path, summary: dict[str, object]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    lines = [
        "NDNSF Native DI Real MiniNDN Evidence",
        f"status={summary['status']}",
        f"gitCommit={summary['gitCommit']}",
        f"command={summary['command']}",
        f"resultDir={summary['resultDir']}",
        f"policyBundle={summary['policyBundle']}",
        f"assignmentCsv={summary['assignmentCsv']}",
        f"logs={summary['logs']}",
        f"assignmentRequested={summary.get('assignmentRequested', summary.get('assignment', ''))}",
        f"assignmentResolved={summary.get('assignmentResolved', summary.get('assignment', ''))}",
        f"miniNDNStatus={summary['miniNDNStatus']}",
        f"miniNDNRun={summary['miniNDNRun']}",
        f"runnerMode={summary['runnerMode']}",
        f"activationPadBytes={summary.get('activationPadBytes', 0)}",
        f"roleExecutionDelayMs={summary.get('roleExecutionDelayMs', 0.0)}",
        f"requestCount={summary.get('requestCount', 1)}",
        f"concurrency={summary.get('concurrency', 1)}",
        f"providerAdmissionPolicy={summary.get('providerAdmissionPolicy', {})}",
        f"localExecution={summary['localExecution']['status']}",
        f"securityBootstrap={summary['securityBootstrap']['status']}",
        f"userExecution={summary['userExecution']['status']}",
        f"dependencyExecution={summary['dependencyExecution']['status']}",
    ]
    optimization = summary.get("optimizationEvidence", {})
    if isinstance(optimization, dict) and optimization.get("status") == "available":
        lines.extend([
            f"optimizationEvidence={optimization['path']}",
            f"optimizationContractVersion={optimization['contractVersion']}",
            f"selectedCandidate={optimization['selectedCandidate']}",
            f"runtimeCandidate={optimization.get('runtimeCandidate', '')}",
            f"candidateCount={optimization['candidateCount']}",
        ])
    runtime_v1 = summary.get("runtimeV1", {})
    if isinstance(runtime_v1, dict) and runtime_v1.get("status") == "available":
        lines.extend([
            f"runtimeV1PlanId={runtime_v1.get('planId', '')}",
            f"runtimeV1Summary={runtime_v1.get('summaryPath', '')}",
            f"runtimeV1Report={runtime_v1.get('reportPath', '')}",
            f"runtimeV1CacheProvider={runtime_v1.get('cacheProvider', '')}",
        ])
    if summary.get("failureReason"):
        lines.append(f"failureReason={summary['failureReason']}")
    failure_breakdown = summary.get("failureBreakdown", {})
    if isinstance(failure_breakdown, dict) and failure_breakdown:
        lines.append(
            "failureBreakdown="
            f"timeouts:{failure_breakdown.get('timeoutCount', 0)};"
            f"other:{failure_breakdown.get('otherFailureCount', 0)};"
            f"negativeAckEvents:{failure_breakdown.get('negativeAckEventCount', 0)};"
            f"negativeAckReasons:{failure_breakdown.get('negativeAckReasons', {})}"
        )
    negative_ack = summary.get("negativeAckReasonCounters", {})
    if isinstance(negative_ack, dict):
        lines.append(
            "negativeAckReasonCounters="
            f"userRecorded:{negative_ack.get('userRecorded', {})};"
            f"providerDecisions:{negative_ack.get('providerDecisions', {})};"
            f"payloadReasons:{negative_ack.get('payloadReasons', {})}"
        )
    dependency_counters = summary.get("dependencyObjectCounters", {})
    if isinstance(dependency_counters, dict):
        lines.append(
            "dependencyObjectCounters="
            f"events:{dependency_counters.get('eventCount', 0)};"
            f"directions:{dependency_counters.get('directionCounters', {})};"
            f"statuses:{dependency_counters.get('statusCounters', {})}"
        )
    core_envelopes = summary.get("coreEnvelopeSummary", {})
    if isinstance(core_envelopes, dict):
        lines.append(
            "coreEnvelopeSummary="
            f"events:{core_envelopes.get('eventCount', 0)};"
            f"envelopes:{core_envelopes.get('envelopeCounts', {})};"
            f"readiness:{core_envelopes.get('providerReadiness', {})};"
            f"schemas:{core_envelopes.get('servicePayloadSchemas', {})}"
        )
    (out_dir / "summary.txt").write_text("\n".join(lines) + "\n",
                                         encoding="utf-8")
    marker = "SUCCESS" if summary["status"] == "SUCCESS" else "FAILURE"
    (out_dir / marker).touch()


def build_base_summary(args, out_dir: Path, policy_dir: Path, logs_dir: Path) -> dict[str, object]:
    git_commit = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=str(REPO),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    ).stdout.strip() or "unknown"
    return {
        "status": "FAILURE",
        "gitCommit": git_commit,
        "command": " ".join(sys.argv),
        "resultDir": str(out_dir),
        "policyBundle": str(policy_dir),
        "runtimeProfile": {
            "profile": args.runtime_profile,
            "resolved": args.runtime_resolved,
            "service": SERVICE,
            "policyBundle": args.policy_bundle,
            "llmPlannerMode": args.llm_planner_mode,
            "runtimeAwareUserPlanner": args.runtime_aware_user_planner,
            "providerNetworkMatrixJson": args.provider_network_matrix_json,
            "targetRps": args.target_rps,
            "requests": args.requests,
            "concurrency": args.concurrency,
            "localExecutionOnly": args.local_execution_only,
            "fullNetwork": args.full_network,
            "multiUserWorkload": args.multi_user_workload,
            "runtimeAwareMaxReplans": args.runtime_aware_max_replans,
            "runtimeAwareReplanReasons": args.runtime_aware_replan_reasons,
            "enableNativeAdmissionLease": args.enable_native_admission_lease,
            "overloadFastFailTimeoutMs": args.overload_fast_fail_timeout_ms,
            "topologyFile": str(Path(args.topology_file).resolve()),
        },
        "topologyFile": str(Path(args.topology_file).resolve()),
        "nativePlan": str(policy_dir / "native-execution-plan.json"),
        "serviceManifest": str(policy_dir / "service-manifest.json"),
        "optimizationEvidence": {
            "status": "not-started",
            "reason": "policy bundle has not generated planner optimization evidence yet",
        },
        "runtimeV1": {
            "status": "not-started",
            "reason": "Runtime v1 MiniNDN evidence has not been generated yet",
        },
        "assignmentCsv": str(out_dir / "assignment.csv"),
        "logs": str(logs_dir),
        "miniNDNStatus": mini_status(),
        "miniNDNRun": "not-started",
        "securityBootstrap": {
            "status": "not-required-for-check-only",
            "reason": "provider checks validate MiniNDN placement and native provider wiring only",
        },
        "localExecution": {
            "status": "not-started",
            "reason": "local execution baseline has not run yet",
        },
        "providerChecks": [],
        "runnerMode": "none",
        "userExecution": {
            "status": "gated",
            "reason": "native tracer user request driver is not yet available",
        },
        "dependencyExecution": {
            "status": "gated",
            "reason": (
                "network dependency exchange through NdnsfCollaborationDependencyIo "
                "is gated until the native tracer user request driver is available"
            ),
        },
        "negativeAckReasonCounters": {
            "userRecorded": {},
            "providerDecisions": {},
            "payloadReasons": {},
            "scannedLogs": 0,
        },
        "failureBreakdown": {},
        "providerAdmissionPolicy": {
            "maxQueue": args.provider_admission_max_queue,
            "maxActiveWorkers": args.provider_admission_max_active_workers,
            "minFreeMemoryMb": args.provider_admission_min_free_memory_mb,
            "enabled": bool(provider_admission_env(args)),
        },
        "runtimeAwarePlanner": {
            "enabled": bool(args.runtime_aware_user_planner),
            "status": "not-started",
        },
        "multiUserWorkload": {
            "enabled": False,
        },
        "plannerMetrics": {
            "status": "not-written",
        },
        "assignment": args.assignment,
        "assignmentRequested": args.assignment,
        "assignmentResolved": args.assignment,
        "activationPadBytes": args.activation_pad_bytes,
        "roleExecutionDelayMs": args.role_execution_delay_ms,
        "requestCount": args.requests,
        "concurrency": args.concurrency,
        "failureReason": "",
        "dependencyObjectCounters": {
            "eventCount": 0,
            "directionCounters": {},
            "statusCounters": {},
        },
        "providerPairTelemetry": {
            "status": "not-started",
            "metricCount": 0,
            "metrics": [],
        },
        "providerPairTelemetryProbe": {
            "status": "not-started",
            "reason": "dependency-edge ndnping probe has not run",
            "enabled": not args.skip_provider_pair_telemetry_probe,
        },
    }


class MiniNdnArgvGuard:
    def __enter__(self):
        self._argv = sys.argv[:]
        sys.argv = [sys.argv[0]]
        return self

    def __exit__(self, exc_type, exc, tb):
        sys.argv = self._argv
        return False


def main() -> int:
    pre_parser = argparse.ArgumentParser(add_help=False)
    pre_parser.add_argument("--runtime-profile", default="",
                            help="Load defaults from an NDNSF runtime profile JSON")
    pre_parser.add_argument("--runtime-resolved", default="",
                            help="Load defaults from a runtime doctor resolved JSON")
    pre_args, _ = pre_parser.parse_known_args()
    profile_defaults = runtime_profile_defaults(
        pre_args.runtime_profile,
        pre_args.runtime_resolved)

    parser = argparse.ArgumentParser(parents=[pre_parser])
    parser.add_argument("--quick-smoke", action="store_true")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print resolved NativeTracer MiniNDN campaign arguments without running")
    parser.add_argument("--out", default=default_value(profile_defaults, "out", str(DEFAULT_OUT)))
    parser.add_argument("--topology-file",
                        default=default_value(profile_defaults, "topology_file", str(TOPO)),
                        help="MiniNDN topology file for the NativeTracer full-network harness")
    parser.add_argument("--assignment",
                        choices=[
                            "default", "alternate", "single-provider",
                            "capacity-pool", "auto", "llm-proportional",
                        ],
                        default=default_value(profile_defaults, "assignment", "default"))
    parser.add_argument("--policy-bundle",
                        choices=["native-tracer", "llm-proportional"],
                        default=default_value(profile_defaults, "policy_bundle", "native-tracer"),
                        help="Select the policy bundle generator")
    parser.add_argument("--llm-planner-mode",
                        choices=["greedy", "proportional"],
                        default=default_value(profile_defaults, "llm_planner_mode", "proportional"),
                        help="Planner mode for --policy-bundle llm-proportional")
    parser.add_argument("--runtime-aware-user-planner", action="store_true",
                        default=bool(default_value(profile_defaults, "runtime_aware_user_planner", False)),
                        help="Enable runtime-aware user-side planner metadata and campaign metrics")
    parser.add_argument("--provider-network-matrix-json",
                        default=default_value(profile_defaults, "provider_network_matrix_json", ""),
                        help=("Optional core ProviderNetworkMatrix JSON, or a previous "
                              "NativeTracer summary with providerPairTelemetry.matrix, "
                              "for runtime-aware dependency edge scoring"))
    parser.add_argument("--skip-provider-pair-telemetry-probe", action="store_true",
                        default=bool(default_value(profile_defaults, "skip_provider_pair_telemetry_probe", False)),
                        help=("Skip full-network dependency-edge ndnping probing. "
                              "By default the harness writes providerPairTelemetry "
                              "evidence before the user workload starts."))
    parser.add_argument("--multi-user-workload",
                        default=default_value(profile_defaults, "multi_user_workload", ""),
                        help="Optional runtime-aware multi-user workload fixture")
    parser.add_argument("--runtime-aware-max-replans", type=int,
                        default=default_value(profile_defaults, "runtime_aware_max_replans", 0),
                        help="Maximum bounded runtime-aware replans passed to the user driver")
    parser.add_argument("--runtime-aware-replan-reasons",
                        default=default_value(profile_defaults, "runtime_aware_replan_reasons", ""),
                        help="Comma-separated synthetic replan reasons for evidence dry-runs")
    parser.add_argument("--tracer-deterministic-runner", action="store_true",
                        default=bool(default_value(profile_defaults, "tracer_deterministic_runner", False)),
                        help="Run provider ONNX roles through the deterministic runner")
    parser.add_argument("--provider-check-timeout", type=int,
                        default=default_value(profile_defaults, "provider_check_timeout", 45))
    parser.add_argument("--local-execution-only", action="store_true",
                        default=bool(default_value(profile_defaults, "local_execution_only", False)),
                        help="Generate policy and run local native execution evidence without MiniNDN")
    parser.add_argument("--no-local-execution-only", dest="local_execution_only",
                        action="store_false",
                        help="Override a profile default and run the MiniNDN provider-check path")
    parser.add_argument("--full-network", action="store_true",
                        default=bool(default_value(profile_defaults, "full_network", False)),
                        help="Run controller, providers in --serve mode, and the NativeTracer user driver")
    parser.add_argument("--core-trace", action="store_true",
                        default=bool(default_value(profile_defaults, "core_trace", False)),
                        help="Enable NDNSF ServiceUser/ServiceProvider trace logs only for launched app processes")
    parser.add_argument("--activation-pad-bytes", type=int,
                        default=default_value(profile_defaults, "activation_pad_bytes", 0),
                        help="Add ignored padding bytes to Backbone encoded activation bundles")
    parser.add_argument("--role-execution-delay-ms", type=float,
                        default=default_value(profile_defaults, "role_execution_delay_ms", 0.0),
                        help="Add controlled per-role execution delay to NativeTracer artifacts")
    parser.add_argument("--llm-stage-execution-delay-scale", type=float,
                        default=default_value(profile_defaults, "llm_stage_execution_delay_scale", 1.0),
                        help="Scale LLM estimated per-stage compute delay when no fixed role delay is set")
    parser.add_argument("--requests", type=int,
                        default=default_value(profile_defaults, "requests", 1),
                        help="Number of closed-loop NativeTracer requests")
    parser.add_argument("--concurrency", type=int,
                        default=default_value(profile_defaults, "concurrency", 1),
                        help="Maximum outstanding NativeTracer requests")
    parser.add_argument("--target-rps", type=float,
                        default=default_value(profile_defaults, "target_rps", 0.0),
                        help="Optional target request rate for planner cost evidence and open-loop workloads")
    parser.add_argument("--open-loop-duration-s", type=float,
                        default=default_value(profile_defaults, "open_loop_duration_s", 0.0),
                        help="Submit NativeTracer requests at --target-rps for this many seconds")
    parser.add_argument("--open-loop-driver-mode",
                        choices=["child", "threaded"],
                        default=default_value(profile_defaults, "open_loop_driver_mode", "threaded"),
                        help="User-driver implementation for open-loop workloads")
    parser.add_argument("--submission-spacing-ms", type=int,
                        default=default_value(profile_defaults, "submission_spacing_ms", 250),
                        help="Delay between concurrent NativeTracer user submissions")
    parser.add_argument("--runtime-v1-context-tokens", type=int,
                        default=default_value(profile_defaults, "runtime_v1_context_tokens", 1024),
                        help="Context length recorded in Runtime v1 MiniNDN evidence")
    parser.add_argument("--runtime-v1-generated-tokens", type=int,
                        default=default_value(profile_defaults, "runtime_v1_generated_tokens", 32),
                        help="Generated-token count recorded in Runtime v1 MiniNDN evidence")
    parser.add_argument("--runtime-v1-prefix-id",
                        default=default_value(profile_defaults, "runtime_v1_prefix_id", ""),
                        help="Optional reusable prefix id recorded in Runtime v1 cache evidence")
    parser.add_argument("--provider-admission-max-queue", type=int,
                        default=default_value(profile_defaults, "provider_admission_max_queue", -1),
                        help="Opt-in native provider negative ACK when pending work reaches this queue size")
    parser.add_argument("--provider-admission-max-active-workers", type=int,
                        default=default_value(profile_defaults, "provider_admission_max_active_workers", -1),
                        help="Opt-in native provider negative ACK when active workers reach this count")
    parser.add_argument("--provider-admission-min-free-memory-mb", type=float,
                        default=default_value(profile_defaults, "provider_admission_min_free_memory_mb", 0.0),
                        help="Opt-in native provider negative ACK when advertised free memory is below this value")
    parser.add_argument("--enable-native-admission-lease", action="store_true",
                        default=bool(default_value(profile_defaults, "enable_native_admission_lease", False)),
                        help="Require NativeTracer generic admission leases before selected role execution")
    parser.add_argument("--enable-execution-leases", action="store_true",
                        help="Acquire fail-closed provider execution leases before collaboration")
    parser.add_argument("--overload-fast-fail-timeout-ms", type=int,
                        default=default_value(profile_defaults, "overload_fast_fail_timeout_ms", 0),
                        help=("Use a shorter user collaboration timeout for overload "
                              "fast-fail experiments"))
    args = parser.parse_args()
    if args.activation_pad_bytes < 0:
        raise SystemExit("--activation-pad-bytes must be non-negative")
    if args.role_execution_delay_ms < 0:
        raise SystemExit("--role-execution-delay-ms must be non-negative")
    if args.llm_stage_execution_delay_scale < 0:
        raise SystemExit("--llm-stage-execution-delay-scale must be non-negative")
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
    if args.open_loop_duration_s > 0.0 and args.target_rps <= 0.0:
        raise SystemExit("--open-loop-duration-s requires --target-rps")
    if args.submission_spacing_ms < 0:
        raise SystemExit("--submission-spacing-ms must be non-negative")
    if args.runtime_v1_context_tokens <= 0:
        raise SystemExit("--runtime-v1-context-tokens must be positive")
    if args.runtime_v1_generated_tokens < 0:
        raise SystemExit("--runtime-v1-generated-tokens must be non-negative")
    if args.runtime_aware_max_replans < 0:
        raise SystemExit("--runtime-aware-max-replans must be non-negative")
    if args.provider_admission_max_queue < -1:
        raise SystemExit("--provider-admission-max-queue must be -1 or non-negative")
    if args.provider_admission_max_active_workers < -1:
        raise SystemExit("--provider-admission-max-active-workers must be -1 or non-negative")
    if args.provider_admission_min_free_memory_mb < 0:
        raise SystemExit("--provider-admission-min-free-memory-mb must be non-negative")
    if args.overload_fast_fail_timeout_ms < 0:
        raise SystemExit("--overload-fast-fail-timeout-ms must be non-negative")
    topology_file = Path(args.topology_file).resolve()
    args.topology_file = str(topology_file)

    multi_user_workload = (
        load_multi_user_workload(args.multi_user_workload)
        if args.multi_user_workload else
        {"enabled": False, "path": "", "requests": []}
    )
    if multi_user_workload.get("enabled") and not args.open_loop_duration_s:
        args.requests = max(args.requests, int(multi_user_workload.get("requestCount", 0) or 0))
        args.concurrency = max(1, min(args.concurrency, args.requests))

    if args.dry_run:
        print(json.dumps({
            "event": "NDNSF_DI_NATIVE_TRACER_MININDN_DRY_RUN",
            "assignment": args.assignment,
            "policyBundle": args.policy_bundle,
            "llmPlannerMode": args.llm_planner_mode,
            "runtimeAwareUserPlanner": args.runtime_aware_user_planner,
            "providerNetworkMatrixJson": args.provider_network_matrix_json,
            "providerPairTelemetryProbeEnabled": (
                not args.skip_provider_pair_telemetry_probe),
            "multiUserWorkload": multi_user_workload,
            "requests": args.requests,
            "concurrency": args.concurrency,
            "targetRps": args.target_rps,
            "runtimeAwareMaxReplans": args.runtime_aware_max_replans,
            "runtimeAwareReplanReasons": args.runtime_aware_replan_reasons,
            "topologyFile": str(topology_file),
            "plannerMetrics": {
                "json": str(Path(args.out) / "planner-metrics.json"),
                "csv": str(Path(args.out) / "planner-metrics.csv"),
            },
            "userDriverCommand": user_driver_command(
                Path(args.out) / "policy-bundle",
                args.requests,
                args.concurrency,
                args.submission_spacing_ms if args.concurrency > 1 else 0,
                assignment_csv=Path(args.out) / "assignment.csv",
                overload_fast_fail_timeout_ms=args.overload_fast_fail_timeout_ms,
                target_rps=args.target_rps,
                open_loop_duration_s=args.open_loop_duration_s,
                open_loop_driver_mode=args.open_loop_driver_mode,
                runtime_aware_max_replans=args.runtime_aware_max_replans,
                runtime_aware_replan_reasons=args.runtime_aware_replan_reasons,
                execution_leases=args.enable_execution_leases),
        }, indent=2, sort_keys=True))
        return 0

    if args.quick_smoke:
        validate_prerequisites(topology_file)
        print(
            "NDNSF_DI_NATIVE_TRACER_MININDN_QUICK_SMOKE_OK "
            f"topology={topology_file} provider={PROVIDER_EXE}")
        return 0

    setLogLevel("info")
    out_dir = Path(args.out)
    policy_dir = out_dir / "policy-bundle"
    logs_dir = out_dir / "logs"
    if out_dir.exists():
        subprocess.run(["rm", "-rf", str(out_dir)], check=True)
    policy_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["PYTHONPATH"] = ":".join([
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "NDNSF-DistributedRepo" / "pythonWrapper"),
        str(REPO / "pythonWrapper"),
        str(REPO / "Experiments"),
        str(REPO),
        env.get("PYTHONPATH", ""),
    ])
    env["LD_LIBRARY_PATH"] = ":".join([
        str(REPO / "build"),
        env.get("LD_LIBRARY_PATH", ""),
    ])
    env["NDNSF_DI_DEPENDENCY_OBJECT_TRACE"] = "1"

    summary = build_base_summary(args, out_dir, policy_dir, logs_dir)
    summary["multiUserWorkload"] = {
        key: value for key, value in multi_user_workload.items()
        if key != "requests"
    }
    ndn = None
    procs = []
    try:
        validate_prerequisites(topology_file)
        requested_assignment = args.assignment
        resolved_assignment = args.assignment
        auto_recommended_candidate = ""
        auto_recommended_estimated_ms = None
        policy_summary = None
        if args.policy_bundle == "llm-proportional":
            if requested_assignment == "auto":
                raise RuntimeError("--assignment auto is only supported for --policy-bundle native-tracer")
            if requested_assignment == "default":
                requested_assignment = "llm-proportional"
                resolved_assignment = "llm-proportional"
            if requested_assignment != "llm-proportional":
                raise RuntimeError(
                    "--policy-bundle llm-proportional requires --assignment llm-proportional")
            policy_summary = run_llm_proportional_bundle(
                policy_dir,
                out_dir,
                logs_dir,
                env,
                args.llm_planner_mode,
                resolved_assignment,
                args.role_execution_delay_ms,
                args.llm_stage_execution_delay_scale,
                args.target_rps,
                1)
        elif requested_assignment == "auto":
            probe_dir = out_dir / "policy-bundle-auto-probe"
            probe_dir.mkdir(parents=True, exist_ok=True)
            probe_summary = run_plan_tracer(
                probe_dir,
                out_dir,
                logs_dir,
                env,
                "default",
                "shared-backbone-current",
                args.role_execution_delay_ms,
                args.activation_pad_bytes,
                args.concurrency,
                args.target_rps,
                args.runtime_aware_user_planner,
                args.provider_network_matrix_json,
                "plan-tracer-auto-probe")
            recommended = str(probe_summary.get(
                "plannerRecommendedCandidate",
                probe_summary.get("selectedCandidate", "shared-backbone-current")))
            auto_recommended_candidate = recommended
            auto_recommended_estimated_ms = probe_summary.get(
                "plannerRecommendedCandidateEstimatedMs",
                probe_summary.get("selectedCandidateEstimatedMs"))
            resolved_assignment = assignment_for_runtime_candidate(recommended)
            summary["autoAssignmentProbe"] = {
                "status": "executed",
                "policyBundle": str(probe_dir),
                "plannerRecommendedCandidate": recommended,
                "resolvedAssignment": resolved_assignment,
            }

        if args.policy_bundle == "llm-proportional":
            active_assignment = assignment_from_rows(
                list(policy_summary.get("assignmentRows", [])))
        else:
            active_assignment = assignment_for(resolved_assignment)
            policy_summary = run_plan_tracer(
                policy_dir,
                out_dir,
                logs_dir,
                env,
                resolved_assignment,
                runtime_candidate_for_assignment(resolved_assignment),
                args.role_execution_delay_ms,
                args.activation_pad_bytes,
                args.concurrency,
                args.target_rps,
                args.runtime_aware_user_planner,
                args.provider_network_matrix_json,
                "plan-tracer")
        add_worker_user_policies(policy_dir / "controller.policies", args.requests)
        add_execution_lease_policies(policy_dir / "controller.policies")
        summary["assignmentRequested"] = requested_assignment
        summary["assignmentResolved"] = resolved_assignment
        summary["assignment"] = resolved_assignment
        if args.policy_bundle == "llm-proportional":
            summary["optimizationEvidence"] = {
                "status": "llm-proportional-bundle",
                "planId": policy_summary.get("planId", ""),
                "plannerMode": policy_summary.get("plannerMode", "proportional"),
                "stageCount": len(policy_summary.get("stages", [])),
                "layerAllocation": policy_summary.get("summary", {}).get("layerAllocation", {}),
                "targetRps": policy_summary.get("targetRps", args.target_rps),
                "prediction": policy_summary.get("prediction", {}),
                "predictedBottleneckProvider": (
                    policy_summary.get("summary", {}).get("predictedBottleneckProvider", "")),
                "maxPredictedUtilization": (
                    policy_summary.get("summary", {}).get("maxPredictedUtilization", 0.0)),
                "predictionLimitKind": (
                    policy_summary.get("summary", {}).get("predictionLimitKind", "")),
            }
        else:
            final_policy_recommended = policy_summary.get(
                "plannerRecommendedCandidate", policy_summary["selectedCandidate"])
            effective_recommended = auto_recommended_candidate or final_policy_recommended
            summary["optimizationEvidence"] = {
                "status": "available",
                "path": policy_summary["optimizationEvidence"],
                "csv": policy_summary["optimizationEvidenceCsv"],
                "sha256": policy_summary["optimizationEvidenceSha256"],
                "contractVersion": policy_summary["optimizationContractVersion"],
                "candidateCount": policy_summary["candidateCount"],
                "selectedCandidate": policy_summary["selectedCandidate"],
                "runtimeCandidate": runtime_candidate_for_assignment(resolved_assignment),
                "selectedCandidateEstimatedMs": policy_summary["selectedCandidateEstimatedMs"],
                "plannerRecommendedCandidate": effective_recommended,
                "plannerRecommendedCandidateEstimatedMs": (
                    auto_recommended_estimated_ms
                    if auto_recommended_estimated_ms is not None else
                    policy_summary.get(
                        "plannerRecommendedCandidateEstimatedMs",
                        policy_summary["selectedCandidateEstimatedMs"])),
                "finalPolicyRecommendedCandidate": final_policy_recommended,
                "bestEstimatedCandidate": policy_summary["bestEstimatedCandidate"],
                "activationPadBytes": policy_summary.get("activationPadBytes", args.activation_pad_bytes),
                "targetRps": policy_summary.get("targetRps", args.target_rps),
                "roleExecutionDelayMs": policy_summary.get(
                    "roleExecutionDelayMs", args.role_execution_delay_ms),
                "workloadConcurrency": policy_summary.get("workloadConcurrency", args.concurrency),
            }
        runtime_assignment = policy_summary.get("runtimeAssignment", {})
        runtime_assignment_summary = policy_summary.get("runtimeAssignmentSummary", {})
        if args.runtime_aware_user_planner or runtime_assignment or runtime_assignment_summary:
            summary["runtimeAwarePlanner"] = {
                "enabled": bool(args.runtime_aware_user_planner),
                "status": "available" if runtime_assignment or runtime_assignment_summary else "enabled-no-assignment",
                "assignmentPath": policy_summary.get("runtimeAssignmentPath", ""),
                "selectedProviders": (
                    runtime_assignment_summary.get("selectedProviders", {})
                    if isinstance(runtime_assignment_summary, dict) else {}
                ),
                "selectedResidencies": (
                    runtime_assignment_summary.get("selectedResidencies", {})
                    if isinstance(runtime_assignment_summary, dict) else {}
                ),
                "roleAssignments": (
                    runtime_assignment_summary.get("roleAssignments", {})
                    if isinstance(runtime_assignment_summary, dict) else {}
                ),
                "nodeCostSummary": (
                    runtime_assignment_summary.get("nodeCostSummary", {})
                    if isinstance(runtime_assignment_summary, dict) else {}
                ),
                "edgeCostSummary": (
                    runtime_assignment_summary.get("edgeCostSummary", {})
                    if isinstance(runtime_assignment_summary, dict) else {}
                ),
                "rejectedCandidateCount": (
                    runtime_assignment_summary.get("rejectedCandidateCount", 0)
                    if isinstance(runtime_assignment_summary, dict) else 0
                ),
            }

        roles = load_plan_roles(policy_dir / "native-execution-plan.json")
        assignment_csv = out_dir / "assignment.csv"
        if args.policy_bundle == "llm-proportional":
            primary_assignment_rows = list(policy_summary.get("assignmentRows", []))
            assignment_rows = write_assignment_csv_rows(
                assignment_csv,
                primary_assignment_rows)
        else:
            primary_assignment_rows = write_assignment_csv(
                assignment_csv,
                resolved_assignment,
                roles,
                active_assignment)
            assignment_rows = (
                capacity_pool_candidate_rows(primary_assignment_rows)
                if resolved_assignment == "capacity-pool" else
                primary_assignment_rows
            )
            if assignment_rows != primary_assignment_rows:
                write_assignment_csv_rows(assignment_csv, assignment_rows)
        launch_rows = launch_rows_for_assignment(resolved_assignment, assignment_rows)
        provider_rows = grouped_provider_rows(launch_rows)
        runtime_hints_json = out_dir / "runtime-hints.json"
        write_runtime_hints_json(
            runtime_hints_json,
            assignment_rows,
            role_execution_delay_ms=args.role_execution_delay_ms,
            provider_admission_max_active_workers=args.provider_admission_max_active_workers,
            provider_admission_max_queue=args.provider_admission_max_queue,
        )
        summary["providerLaunchRows"] = launch_rows
        summary["primaryAssignmentRows"] = primary_assignment_rows
        summary["candidateAssignmentRows"] = assignment_rows
        summary["runtimeHintsJson"] = str(runtime_hints_json)
        summary["providerLaunchMode"] = (
            "capacity-pool" if resolved_assignment == "capacity-pool" else
            "static-assignment")
        summary["llmProviderResourceProfiles"] = {
            row["provider"]: llm_provider_resource_profile(row["provider"])
            for row in provider_rows
            if llm_provider_resource_profile(row["provider"])
        }
        if args.policy_bundle == "llm-proportional":
            summary["runtimeV1"] = write_minindn_runtime_v1_evidence(
                out_dir=out_dir / "runtime-v1",
                model_path=RUNTIME_V1_MODEL_SPEC,
                provider_profiles_path=RUNTIME_V1_PROVIDER_PROFILES,
                target_rps=args.target_rps,
                context_tokens=args.runtime_v1_context_tokens,
                generated_tokens=args.runtime_v1_generated_tokens,
                prefix_id=args.runtime_v1_prefix_id,
                policy_summary=policy_summary,
            )
        local_execution_assignment = (
            "default" if resolved_assignment == "capacity-pool" else
            resolved_assignment)
        model_family = str(policy_summary.get("modelFamily", "yolo-onnx"))
        model_format = str(policy_summary.get("modelFormat", "onnx"))
        planner_kind = str(policy_summary.get("plannerKind", "yolo-detect-auto"))
        summary["localExecution"] = run_local_execution_baseline(
            policy_dir,
            out_dir,
            logs_dir,
            env,
            local_execution_assignment,
            primary_assignment_rows,
            model_family=model_family,
            model_format=model_format,
            planner_kind=planner_kind,
            assignment_csv=assignment_csv if args.policy_bundle == "llm-proportional" else None)
        summary["dependencyExecution"] = {
            "status": "local-baseline-executed",
            "reason": (
                "in-memory native DependencyIo baseline executed; full network "
                "dependency exchange remains gated"
            ),
        }

        if args.local_execution_only:
            summary["miniNDNRun"] = "skipped-local-execution-only"
            summary["status"] = "SUCCESS"
            return_code = 0
            return return_code

        if mini_status() != "available-root":
            raise RuntimeError(f"MiniNDN normal mode requires root; status is {mini_status()}")

        with MiniNdnArgvGuard():
            Minindn.cleanUp()
            Minindn.verifyDependencies()
            ndn = Minindn(topoFile=str(topology_file))
            ndn.start()
            assign_provider_homes(ndn, provider_rows)
            summary["providerLaunchRows"] = launch_rows
            summary["providerProcessRows"] = provider_rows
        summary["miniNDNRun"] = "started"
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="ERROR")
        perf.wait_for_nfd_sockets(ndn, out_dir)
        if args.full_network:
            env["NDNSF_DI_RUNTIME_TIMING"] = "1"
        if args.core_trace:
            env["NDN_LOG"] = (
                "ndn_service_framework.ServiceUser=TRACE:"
                "ndn_service_framework.ServiceProvider=TRACE:"
                "ndn_service_framework.ServiceController=TRACE"
            )
        if args.full_network:
            setup_full_network_identities(ndn,
                                          provider_rows,
                                          out_dir,
                                          logs_dir,
                                          user_worker_identities(args.requests))

        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        routing.addOrigin([ndn.net["memphis"]],
                          [CONTROLLER, GROUP, USER, *user_worker_identities(args.requests)])
        for row in launch_rows:
            routing.addOrigin([ndn.net[row["node"]]], [row["provider"], GROUP])
        routing.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/NDNSF-DI/Tracer", Nfdc.STRATEGY_MULTICAST)

        if args.full_network:
            controller_proc, controller_log = start_node_command(
                ndn.net["memphis"],
                "controller",
                controller_command(policy_dir, assignment_rows),
                logs_dir,
                env,
                procs)
            time.sleep(2.0)
            if controller_proc.poll() is not None:
                raise RuntimeError(
                    f"controller exited before user execution; see {controller_log}")

            provider_logs = []
            provider_log_rows = []
            for row in provider_rows:
                node = ndn.net[row["node"]]
                command = provider_serve_command(
                    row,
                    policy_dir,
                    args.tracer_deterministic_runner or
                    args.policy_bundle == "llm-proportional",
                    args.enable_native_admission_lease,
                    args.enable_execution_leases)
                provider_env = llm_provider_resource_env(row["provider"])
                provider_env.update(provider_admission_env(args))
                proc, path = start_node_command(
                    node,
                    "provider-serve-" + safe_log_component(row["role"]) +
                    "--" + safe_log_component(row["provider"]),
                    command,
                    logs_dir,
                    env,
                    procs,
                    Path(row["homeDir"]),
                    provider_env)
                provider_logs.append(path)
                provider_log_rows.append({
                    "log": str(path),
                    "provider": row["provider"],
                    "node": row["node"],
                    "role": row["role"],
                })
            wait_for_log_patterns(provider_logs,
                                  ["NDNSF_DI_NATIVE_PROVIDER_PROVISION_READY"],
                                  args.provider_check_timeout,
                                  "native provider provisioning")
            if args.skip_provider_pair_telemetry_probe:
                summary["providerPairTelemetryProbe"] = {
                    "status": "skipped",
                    "reason": "disabled by --skip-provider-pair-telemetry-probe",
                    "enabled": False,
                }
            else:
                try:
                    summary["providerPairTelemetryProbe"] = {
                        "enabled": True,
                        **write_dependency_edge_rtt_summary(
                            ndn,
                            out_dir,
                            logs_dir,
                            provider_rows,
                            env,
                            procs,
                            policy_dir / "native-execution-plan.json",
                        ),
                    }
                except Exception as exc:
                    summary["providerPairTelemetryProbe"] = {
                        "status": "failed",
                        "enabled": True,
                        "reason": str(exc),
                    }
            time.sleep(8.0)
            user_command = user_driver_command(policy_dir,
                                    args.requests,
                                    args.concurrency,
                                    args.submission_spacing_ms if args.concurrency > 1 else 0,
                                    assignment_csv=assignment_csv,
                                    overload_fast_fail_timeout_ms=args.overload_fast_fail_timeout_ms,
                                    target_rps=args.target_rps,
                                    open_loop_duration_s=args.open_loop_duration_s,
                                    open_loop_driver_mode=args.open_loop_driver_mode,
                                    burst_admission_providers=[
                                        "/NDNSF-DI/Tracer/provider/backbone",
                                        "/NDNSF-DI/Tracer/provider/single",
                                    ] if resolved_assignment == "capacity-pool" else None,
                                    runtime_aware_max_replans=args.runtime_aware_max_replans,
                                    runtime_aware_replan_reasons=args.runtime_aware_replan_reasons,
                                    execution_leases=args.enable_execution_leases)
            user_proc, user_log = start_node_command(
                ndn.net["memphis"], "user-driver", user_command,
                logs_dir, env, procs)
            try:
                user_proc.wait(timeout=user_driver_wait_timeout_s(
                    args.requests,
                    args.concurrency,
                    request_timeout_ms=(
                        args.overload_fast_fail_timeout_ms
                        if args.overload_fast_fail_timeout_ms > 0 else
                        60000
                    ),
                    open_loop_duration_s=args.open_loop_duration_s))
            except Exception:
                user_proc.kill()
                user_proc.wait(timeout=3)
            user_result = parse_user_execution(user_log)
            summary["userExecution"] = {
                "status": "executed" if user_result.get("status") == "executed" else "failed",
                "reason": (
                    "NativeTracer user driver returned a successful NDNSF collaboration response"
                    if user_result.get("status") == "executed" else
                    "NativeTracer user driver completed with one or more failed requests"
                ),
                "log": str(user_log),
                "payloadBytes": user_result.get("payloadBytes", 0),
                "elapsedMs": user_result.get("elapsedMs", 0.0),
                "requestCount": user_result.get("requestCount", args.requests),
                "concurrency": user_result.get("concurrency", args.concurrency),
                "successCount": user_result.get("successCount", 1),
                "failureCount": user_result.get("failureCount", 0),
                "makespanMs": user_result.get("makespanMs", user_result.get("elapsedMs", 0.0)),
                "meanMs": user_result.get("meanMs", user_result.get("elapsedMs", 0.0)),
                "p50Ms": user_result.get("p50Ms", user_result.get("elapsedMs", 0.0)),
                "p95Ms": user_result.get("p95Ms", user_result.get("elapsedMs", 0.0)),
                "throughputRps": user_result.get("throughputRps", 0.0),
                "overloadFastFailCount": user_result.get("overloadFastFailCount", 0),
                "overloadFastFail": user_result.get("overloadFastFail", {}),
                "targetRps": user_result.get("targetRps", args.target_rps),
                "openLoopDurationS": user_result.get("openLoopDurationS", args.open_loop_duration_s),
                "openLoopDriverMode": user_result.get("mode", args.open_loop_driver_mode),
                "scheduledRequestCount": user_result.get("scheduledRequestCount", args.requests),
                "submittedCount": user_result.get("submittedCount", user_result.get("successCount", 1)),
                "localBackpressureCount": user_result.get("localBackpressureCount", 0),
                "localBackpressureWaitCount": user_result.get("localBackpressureWaitCount", 0),
                **user_execution_measurement_fields(user_result),
                "offeredRps": user_result.get("offeredRps", 0.0),
                "requests": user_result.get("requests", []),
            }
            negative_ack_counters = collect_negative_ack_reason_counters(logs_dir)
            summary["negativeAckReasonCounters"] = negative_ack_counters
            summary["failureBreakdown"] = build_failure_breakdown(
                user_result,
                negative_ack_counters,
            )
            if user_proc.returncode != 0 or user_result.get("status") != "executed":
                raise RuntimeError(
                    f"NativeTracer user execution failed rc={user_proc.returncode} "
                    f"result={user_result}; see {user_log}")
            roles = observed_role_timings(provider_logs)
            expected_roles = set(load_plan_roles(policy_dir / "native-execution-plan.json"))
            missing_roles = sorted(expected_roles - roles)
            if missing_roles:
                raise RuntimeError(
                    f"missing provider role timing logs for {missing_roles}; logs={provider_logs}")
            summary["runnerMode"] = "qwen-onnx-native"
            summary["providerChecks"] = [
                {
                    "log": str(path),
                    "status": "served",
                }
                for path in provider_logs
            ]
            summary["providerUtilization"] = summarize_provider_metrics(provider_log_rows)
            summary["securityBootstrap"] = {
                "status": "executed",
                "reason": "ServiceController and user/provider permission fetch path ran during full-network execution",
            }
            summary["dependencyExecution"] = {
                "status": "executed",
                "reason": "all NativeTracer roles emitted provider handler timing in serve mode",
                "roles": sorted(roles),
            }
            summary["status"] = "SUCCESS"
            return 0

        for row in provider_rows:
            node = ndn.net[row["node"]]
            command = provider_check_command(
                row,
                policy_dir,
                args.tracer_deterministic_runner or
                args.policy_bundle == "llm-proportional")
            provider_env = llm_provider_resource_env(row["provider"])
            provider_env.update(provider_admission_env(args))
            start_node_command(node,
                               "provider-check-" + safe_log_component(row["role"]) +
                               "--" + safe_log_component(row["provider"]),
                               command,
                               logs_dir,
                               env,
                               procs,
                               Path(row["homeDir"]),
                               provider_env)
        provider_results = wait_provider_checks(procs, args.provider_check_timeout)
        summary["providerChecks"] = provider_results
        failed = [item for item in provider_results if item["returncode"] != 0]
        if failed:
            raise RuntimeError(f"provider checks failed: {failed}")

        summary["status"] = "SUCCESS"
    except Exception as exc:
        summary["status"] = "FAILURE"
        summary["failureReason"] = str(exc)
    finally:
        stop_processes(procs)
        if ndn is not None:
            try:
                ndn.stop()
            except Exception:
                pass
        try:
            with MiniNdnArgvGuard():
                Minindn.cleanUp()
        except Exception:
            pass
        summary["negativeAckReasonCounters"] = collect_negative_ack_reason_counters(logs_dir)
        summary["leaseCounters"] = collect_admission_lease_counters(logs_dir)
        summary["providerAckRuntimeHints"] = collect_provider_ack_runtime_hints(logs_dir)
        summary["coreEnvelopeSummary"] = collect_core_envelope_summary(logs_dir)
        summary["providerFragmentInventory"] = collect_provider_fragment_inventory(logs_dir)
        summary["dependencyObjectCounters"] = collect_dependency_object_counters(logs_dir)
        summary["providerPairTelemetry"] = collect_provider_pair_telemetry(out_dir)
        (out_dir / "dependency_object_counters.json").write_text(
            json.dumps(summary["dependencyObjectCounters"],
                       indent=2,
                       sort_keys=True) + "\n",
            encoding="utf-8")
        attach_execution_operation_statuses(summary)
        if not summary.get("failureBreakdown") and isinstance(summary.get("userExecution"), dict):
            user_execution = summary["userExecution"]
            summary["failureBreakdown"] = build_failure_breakdown(
                user_execution,
                summary["negativeAckReasonCounters"],
            )
        try:
            summary["plannerMetrics"] = {
                "status": "written",
                **write_planner_metrics(out_dir, summary),
            }
        except Exception as exc:
            summary["plannerMetrics"] = {
                "status": "failed",
                "reason": str(exc),
            }
        write_summary(out_dir, summary)
        print((out_dir / "summary.txt").read_text(encoding="utf-8"))
    return 0 if summary["status"] == "SUCCESS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
